# sofcon 開発メモ（次回の続きはここから）

社内ソフトコンテスト用AI。空き地(0)を掘り、最後のマスを取ったプレイヤーが勝ち。
提出は **DLL（.dll）** の `PlayStage()` 関数。外部ライブラリのリンク禁止 → ベアメタル実装。

## 🔴 確定済みコンテスト仕様

| 項目 | 内容 |
|------|------|
| 最大プレイヤー数 | **5人**（自分含む、当日戦は `your_number=1` 固定） |
| 時間制限 | **全50ステージ通算30秒以内** |
| 1ステージ | 1ゲーム（対局）= 1ステージ |
| 1手あたりマス数 | **3マス固定**（当日戦は `number_takes=3` 固定） |
| 提出形式 | **DLL**。`PlayStage()` が1ターンごとに呼ばれる |

### コンテスト提出インターフェース（確定）
```cpp
extern "C" __declspec(dllexport)
void PlayStage(char floor[STAGE_Y_MAX][STAGE_X_MAX],
               char*    count,
               TAKE_TAG lands[LANDS_SHARK_MAX],
               RULES_TAG rules)
// floor[y][x]: -1=壁, 0=空き, 1〜5=各プレイヤー陣地
// *count     : 取るマス数を書き込む
// lands[i]   : {x=列, y=行} を書き込む（4連結条件）
// rules      : {your_number, number_takes, number_players}
```

---

## 🟢 現在の到達点

| 成果物 | 状態 |
|--------|------|
| **`cpp/mcts_player.cpp`** | ✅ **提出本命。PlayStage() 実装済み・DLLビルド可** |
| **終盤厳密解ソルバー** | ✅ 空き≤16マスで完全探索（パラノイド・2〜5人対応）。総当たり258局面と一致(PASS)。ソルバー有vs無で98%勝利 |
| 時間管理（ステージ検出型） | ✅ 残り時間÷残りステージで1手予算を自動配分 |
| 高速ロールアウト（fast_random_move） | ✅ sort+uniqueを排除し大幅高速化 |
| MCTS 2〜5人対応 | ✅ 勝者ベースのバックプロパゲーション |
| **実機テスト** | ✅ **36/50ステージ勝利（72%）、使用時間7.6秒** |
| NN強化学習（汎用ver） | ✅ iter300完走済み（提出はMCTSが本命） |

### 📊 時間ベースMCTS の対ランダム勝率（9×9, 各20戦）

**改善後（ステージ検出型・28秒フル活用版）**
| 人数 | 勝率 | ベースライン | 判定 |
|---|---|---|---|
| 2人 | **100%** | 50% | ✅ 圧勝 |
| 3人 | **85%** | 33% | ✅ 圧勝 |
| 4人 | **60%** | 25% | ✅ 優勢 |
| 5人 | **30%** | 20% | ✅ OK |

→ 全人数でランダム超え。50ステージ通算～28秒で完走。
→ 実機でも **36/50勝（72%）** を達成済み。

---

## ▶ すぐ動かすコマンド（`C:\Users\toku\private\sofcon` で実行）

### C++ セルフテスト（ランダムとの対戦）
```bash
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test && ./mcts_test"
```

### PlayStage() 動作確認テスト
```bash
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 test_playstage.cpp -o test_playstage && ./test_playstage"
```

### DLL ビルド（Windows / MinGW）
```bash
g++ -O2 -std=c++17 -shared -o mcts_player.dll cpp/mcts_player.cpp
```

### 速度ベンチマーク
```bash
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 bench.cpp -o bench && ./bench"
```

---

## 📋 次回やることリスト（優先順）

### ★最優先
1. **実機で再テスト**（ステージ検出型に変えたので勝率が上がっているはず）
   - 前回: 36/50勝（7.6秒使用） → 改善後: 時間をフル活用するので更に強いはず

2. **LANDS_SHARK_MAX の値を確認**
   - コンテスト側ヘッダに定義があればそれを使う（現在のデフォルト: 10）
   - `number_takes=3` 固定なら `lands[0..2]` しか使わないので問題なし

3. **DLLビルドして提出**
   ```bash
   # MinGW（Windows）でDLLビルド
   g++ -O2 -std=c++17 -shared -o mcts_player.dll cpp/mcts_player.cpp
   ```
   - コンテスト側ヘッダ（`STAGE_Y_MAX` 等の定数定義）があれば先に `#include`

### 余裕があれば
4. **5人専用NNファインチューニング**（現状MCTSが強いので優先度低）
   ```bash
   python train/self_play.py --iters 150 --sims 50 --games 4 --players_min 5 --players_max 5
   ```

---

## 📁 ファイル構成

```
sofcon/
├── RESUME.md             ← このファイル
├── cpp/
│   ├── mcts_player.cpp   ★提出本命（PlayStage() 実装済み・これ1ファイルで完結）
│   ├── test_playstage.cpp  PlayStage() 動作確認テスト
│   ├── bench.cpp         速度ベンチ
│   ├── inference.h       NN提出時用（現在不使用）
│   └── inference_impl.h  同上
├── game/                 Python版ゲームエンジン（開発用）
├── model/                ニューラルネット（開発用）
├── train/                自己対戦学習スクリプト（開発用）
└── tools/                評価・エクスポートツール（開発用）
```

## ⚠ ハマりどころメモ

- Python実行は必ず `C:\Users\toku\private\sofcon` 直下から（`game` 等のimportのため）
- C++コンパイルはWSL経由: `wsl -e bash -c "cd /mnt/c/... && g++ ..."`
- DLLの `__declspec(dllexport)` はLinux/WSLでは無効 → `_WIN32` guard で切り替え済み
- `floor` 配列の型は `char`（signed/unsigned 曖昧）→ `(signed char)` キャストで確実に -1 を認識
- `floor[y][x]` の軸順注意：y=行（第1添字）、x=列（第2添字）
- `lands[i].y=行, lands[i].x=列`（直感と逆になりやすい）
- C++ MCTSで `std::vector<Node>` の参照を `push_back` をまたいで保持するとuse-after-freeでセグフォ → インデックス参照に修正済み
