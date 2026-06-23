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
| ステージフック | `StartStage()` / `EndStage()` が各ステージ開始・終了時に呼ばれる |

### コンテスト提出インターフェース（確定）
```cpp
extern "C" __declspec(dllexport) void StartStage();   // 各ステージ開始時
extern "C" __declspec(dllexport) void EndStage();     // 各ステージ終了時
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

## 🟢 現在の到達点（2026-06-23 時点・実機 23/30勝 = 過去最高）

| 成果物 | 状態 |
|--------|------|
| **`prog.cpp`** | ✅ **提出本命。下記の多段戦略+スレッドプール完備** |
| **PlayStage 判断順** | ① Expectimaxソルバー(空き≤20) → ② パリティ直接手 → ③ det_lookahead(AI戦のみ) → ④ MCTS |
| **Expectimax ソルバー** | ✅ 終盤を確率的に評価（自分=最大化, 敵=平均/ランダム仮定）。5人戦でも機能 |
| **パリティ戦略** | ✅ Nim理論。`empty % (number_takes×num_players) == number_takes` で勝ち手を直接構築 |
| **AI戦 bot 完全再現** | ✅ P2(縦1) P3(横1) P4(横最大) P5(縦最大) を決定論シミュレーション |
| **対戦モード自動検出** | ✅ P2/P3が2マス以上取れば対人戦と判定し戦略切替 |
| **可変 number_takes 対応** | ✅ gen_moves/Endgame が 3〜10 マスに対応 |
| **クラッシュ防止** | ✅ gen_moves に1アンカー上限(256) → 隠しステージ(k最大10)で爆発しない |
| **NN guided MCTS (PUCT)** | ✅ `cpp/weights.h` があれば自動有効(`__has_include`)。16ch×3blocks |
| **スレッドプール並列化** | ✅ StartStage()で起動、50ステージ通して稼働 |
| **DLLビルド環境** | ✅ MSYS2 MinGW-w64（C:\msys64）。build.bat は -O3 |

### ⚠️ コンテスト仕様の追加判明事項（重要）
- **number_takes は可変**：見えている1〜30ステージは最大5、**隠し31〜50ステージは最大10**まで上がる
- 取れるマス数を毎手 `number_takes` まで使うのが基本（k>3で3マスしか取らないと大敗する）
- AI戦と対人戦の2フェーズがあり、**実行時に区別できない** → mode自動検出で対応
- gen_moves の合法手数（実測, 空き盤面）: k=8で旧138,543手→新4,470手（上限で抑制済み）

### 🧠 NN 学習状況
- `model/checkpoint_16ch_15x15.pt`（15×15, 16ch×3blocks, players 2-3, sims=5）
- iter 370+ / loss 約4.4 まで低下。**このPCで放置継続中**（別PCでは checkpoint から resume 可能）
- `cpp/weights.h` は上記 checkpoint から生成済み（コミット済み）

---

## ▶ すぐ動かすコマンド

### DLL ビルド（一番重要）
```
build.bat をダブルクリック
→ prog.dll + libwinpthread-1.dll が生成される
→ この2ファイルをコンテストに提出
```

コマンドラインから直接ビルドする場合：
```bat
set PATH=%PATH%;C:\msys64\mingw64\bin;C:\msys64\usr\bin
g++ -O2 -std=c++14 -shared -static-libgcc -static-libstdc++ ^
    -Wl,--whole-archive -lwinpthread -Wl,--no-whole-archive ^
    -o prog.dll prog.cpp
copy C:\msys64\mingw64\bin\libwinpthread-1.dll .
```

### セルフテスト（WSL経由）
```bash
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++14 -pthread -DMCTS_SELFTEST mcts_player.cpp -o mcts_test && ./mcts_test"
```

---

## 💻 別PCで続きを始める手順
```bash
git clone <repo> && cd sofcon
# checkpoint と cpp/weights.h はコミット済みなので即ビルド可能
build.bat               # prog.dll 生成（weights.h があるので NN 有効でビルドされる）
```
NN 学習を別PCで再開する場合（任意・PyTorch必要）:
```bash
python train/self_play.py --board_h 15 --board_w 15 --channels 16 --blocks 3 \
    --sims 5 --iters 2000 --players_min 2 --players_max 3 \
    --save model/checkpoint_16ch_15x15.pt
# --no-resume を付けなければ checkpoint から継続
```
学習後に重みを反映:
```bash
python tools/export_weights.py --checkpoint model/checkpoint_16ch_15x15.pt --output cpp/weights.h
```

## 📋 次回やることリスト（優先順）
1. **NN を入れた版 vs 入れない版の実機勝率比較**
   - weights.h を一時退避してビルド = NN無し版。両者で勝率を比べ、NNが効くか確認
   - 現状の 23/30 は NN 込み（weights.h 存在下でビルド）か要確認
2. **敗北ステージが理論上勝てるか検証**
   - 負けたステージ盤面で時間無制限DFS → 勝ち手順の有無を確認
   - 無ければ初期配置由来で勝率上限。あれば探索改善の余地あり
3. **提出**
   - `python tools/bundle.py --checkpoint model/checkpoint_16ch_15x15.pt --output prog_submit.cpp`
     → ソース1ファイル化（コード提出も必要なため）
   - `build.bat` → prog.dll + libwinpthread-1.dll
   - 提出: prog_submit.cpp + prog.dll + libwinpthread-1.dll
   - 提出期限まで残り約6日（2026-06-23 時点）

### ハマりどころ
- entry.h はコンテスト側ヘッダ。無ければ build.bat がダミー生成
- `libwinpthread-1.dll` が配布不可なら winlibs win32スレッド版に移行
- スレッドプールが不安定なら `pool::init_once()` を無効化して単スレッドに

---

## 📁 ファイル構成

```
sofcon/
├── RESUME.md               ← このファイル
├── prog.cpp                ★提出本命（これ1ファイルで完結）
│                             - MCTS + 終盤ソルバー + スレッドプール
│                             - StartStage() / EndStage() / PlayStage() 実装済み
├── build.bat               ★DLLビルドスクリプト（ダブルクリックで実行）
│
├── cpp/
│   ├── mcts_player.cpp     開発・テスト用（prog.cppと同内容のスタンドアロン版）
│   ├── test_playstage.cpp  PlayStage()動作確認テスト
│   └── bench.cpp           速度ベンチ
├── game/                   Python版ゲームエンジン（開発用）
├── model/                  ニューラルネット（開発用・提出不要）
├── train/                  自己対戦学習スクリプト（開発用）
└── tools/                  評価ツール（開発用）
```

## ⚠ ハマりどころメモ

### ビルド関連
- **g++ は PATH に自動追加されない** → `C:\msys64\mingw64\bin;C:\msys64\usr\bin` が必要
  （`build.bat` は内部で自動追加しているので大丈夫）
- **entry.h** はコンテスト側が配布するヘッダ。なければ `build.bat` がダミーを自動生成
- **提出は2ファイル**: `prog.dll` + `libwinpthread-1.dll`（同じフォルダに置く）

### コード関連
- `floor[y][x]` の軸順注意：y=行（第1添字）、x=列（第2添字）
- `lands[i].y=行, lands[i].x=列`（直感と逆になりやすい）
- `(signed char)floor[r][c]` キャストで確実に -1 を認識
- スレッドプールは `StartStage()` 初回のみ起動、`detach()` 済みなので終了処理不要

### スレッドプールの動作
```
StartStage() 初回
  → hardware_concurrency() 本のスレッドを起動して待機

PlayStage() 毎ターン
  → condition_variable で全スレッドを起こす (~0.1ms)
  → 全スレッドが並列MCTS実行
  → 多数決で最善手を返す

EndStage() → 何もしない（スレッドは次ステージも稼働継続）
```
