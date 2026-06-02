# sofcon 開発メモ（次回の続きはここから）

社内ソフトコンテスト用AI。最大15×15のマス目で空き地(0)を掘り、最後のマスを
取ったプレイヤーが勝ち。1ターン1〜3マス（直交連結）。プレイヤー2〜5人。
提出は C++ 関数（盤面の2次元配列を受け取り、掘るマス座標の配列を返す）。
**外部ライブラリのリンク禁止 → ベアメタルで実装。**

## 🔴 確定済みコンテスト仕様
| 項目 | 内容 |
|------|------|
| 最大プレイヤー数 | **5人**（自分含む） |
| 時間制限 | **全50ステージ通算30秒以内** |
| 1ステージ | 1ゲーム（対局）= 1ステージ |
| 1手あたり予算 | 9×9・5人なら約80ms（自動管理） |

---

## 🟢 現在の到達点

| 成果物 | 状態 |
|--------|------|
| ゲームエンジン（Python / C++） | ✅ 完成・検証済み |
| **C++時間ベースMCTSプレイヤー** `cpp/mcts_player.cpp` | ✅ **完成。50ステージ30秒以内を自動管理。2〜5人全対応** |
| 時間管理（グローバルタイマー） | ✅ プロセス起動から28秒の予算を自動配分 |
| 高速ロールアウト（fast_random_move） | ✅ sort+uniqueを排除し大幅高速化 |
| MCTS N人対応 | ✅ 勝者ベースのバックプロパゲーション |
| **NN 人数非依存化** | ✅ 入力9ch固定・視点相対・value=5人勝率ベクトル |
| NN強化学習（2〜5人混合） | ✅ iter300完走。汎用ベースライン完成 |

### 📊 時間ベースMCTS の対ランダム勝率（9×9, 各20戦）
| 人数 | 勝率 | ベースライン | 判定 |
|---|---|---|---|
| 2人 | **100%** | 50% | ✅ 圧勝 |
| 3人 | **90%** | 33% | ✅ 圧勝 |
| 4人 | **75%** | 25% | ✅ 圧勝 |
| 5人 | **40%** | 20% | ✅ OK |

→ 全人数でベースラインの2倍以上。50ステージ通算28秒で完走。
→ 提出本命は **C++時間ベースMCTS**。

退避ファイル: `checkpoint_generalist_iter300.pt.bak`（汎用完走版）, `checkpoint_2p_iter60.pt.bak`

**結論: 提出物の本命（C++時間ベースMCTS）は全人数・時間制限対応で確保済み。**

---

## ▶ すぐ動かすコマンド（C:\Users\toku\private\sofcon で実行）

### C++ MCTSプレイヤーの動作確認（ランダムとの対戦）
```bash
# WSL経由でコンパイル＆実行
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test && ./mcts_test"
# または build_cpp.bat をダブルクリック
```

### 速度ベンチマーク
```bash
wsl -e bash -c "cd /mnt/c/Users/toku/private/sofcon/cpp && g++ -O2 -std=c++17 bench.cpp -o bench && ./bench"
```

### Python版MCTSの強さ確認（純粋MCTS=ロールアウト）
```bash
python tools/evaluate.py --rollout --games 30 --sims 100 --players 2 --board_h 9 --board_w 9
```

### NN学習（チェックポイントから自動再開。PCが落ちても続行可）
```bash
# ★PCが落ちたらこれを実行するだけで続きから再開（train.bat ダブルクリックでも可）
python train/self_play.py --iters 300 --sims 50 --games 4 --players_min 2 --players_max 6 --board_h 9 --board_w 9
# 最初からやり直す場合のみ --no-resume を付ける
```

### 学習済みNNの強さ確認（人数を指定）
```bash
python tools/evaluate.py --games 30 --sims 50 --players 2   # 2人戦での対ランダム勝率
python tools/evaluate.py --games 30 --sims 50 --players 4   # 4人戦
python tools/evaluate.py --games 30 --sims 50 --players 6   # 6人戦
```

### NN vs 純粋MCTS（提出差し替え判断用）
```bash
python tools/head2head.py --games 20 --nn_sims 80 --roll_sims 200 --players 2
```

### 明日：人数判明後のファインチューニング（例：4人専用に特化）
```bash
python train/self_play.py --iters 100 --sims 50 --games 4 --players_min 4 --players_max 4
# 既存checkpointを土台に4人特化（resumeで継続学習）
```

---

## 📋 次回やることリスト（優先順）

### ★最優先：提出の準備
1. **コンテストのI/F（関数名・引数型）を確認して `choose_move()` を合わせる**
   - 現在の提出I/F: `choose_move(board[][MAXN], height, width, my_id, num_players)`
   - `num_sims=0` がデフォルト → 時間ベースで動く
   - 関数名/構造体名/引数が違う場合は該当箇所のみ変更

2. **NNを5人特化ファインチューニング**（時間があれば・省略可）
   ```bash
   python train/self_play.py --iters 150 --sims 50 --games 4 --players_min 5 --players_max 5
   python tools/evaluate.py --games 30 --sims 50 --players 5
   ```

3. **NN vs 時間ベースMCTS** で勝てるか判定 → 勝てたらNN版を検討
   ```bash
   python tools/head2head.py --games 20 --nn_sims 80 --roll_sims 200 --players 5
   ```

---

## 📁 ファイル構成

```
sofcon/
├── RESUME.md             ← このファイル
├── train.bat            ← 学習をダブルクリックで再開
├── game/
│   ├── board.py          盤面・apply_move・勝敗判定
│   └── move_gen.py       合法手列挙(1-3マス直交連結)
├── model/
│   ├── network.py        SofconNet (CNN: Policy + Value)
│   └── mcts.py           MCTS (negamax/PUCT, rolloutモード有り) ★バグ修正済み
├── train/
│   ├── self_play.py      自己対戦学習（チェックポイント再開対応）
│   └── log.txt           学習ログ
├── tools/
│   ├── evaluate.py       強さ評価（--rollout で純粋MCTS診断）
│   ├── export_weights.py PyTorch重み → C++ヘッダ変換
│   └── verify_inference.py Python vs C++ 数値一致確認
└── cpp/
    ├── mcts_player.cpp   ★本命：自己完結MCTSプレイヤー（提出候補）
    ├── bench.cpp         15×15の速度ベンチ
    ├── inference.h       手書きConv/BN/ReLU（NN提出時に使う）
    ├── inference_impl.h  forward()実体
    └── player_example.cpp NN版の提出ひな形
```

## ⚠ ハマりどころメモ
- Python実行は必ず `C:\Users\toku\private\sofcon` 直下から（`game`等のimportのため）
- C++コンパイルはWSL経由（`wsl -e bash -c "cd /mnt/c/... && g++ ..."`）。build_cpp.batはWSL使用に更新済み。
- Windowsのprint絵文字はcp932で落ちる → ASCIIで書く
- C++ MCTSで `std::vector<Node>` の参照を `push_back` をまたいで保持すると
  use-after-free でセグフォる → 必ずインデックス参照する（修正済み）
- 学習は5iterごとに `model/checkpoint.pt` 保存。PCが落ちても再開可
