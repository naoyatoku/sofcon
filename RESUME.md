# sofcon 開発メモ（次回の続きはここから）

社内ソフトコンテスト用AI。最大15×15のマス目で空き地(0)を掘り、最後のマスを
取ったプレイヤーが勝ち。1ターン1〜3マス（直交連結）。プレイヤー2〜6人。
提出は C++ 関数（盤面の2次元配列を受け取り、掘るマス座標の配列を返す）。
**外部ライブラリのリンク禁止 → ベアメタルで実装。**

---

## 🟢 現在の到達点

| 成果物 | 状態 |
|--------|------|
| ゲームエンジン（Python / C++） | ✅ 完成・検証済み |
| **C++純粋MCTSプレイヤー** `cpp/mcts_player.cpp` | ✅ **完成。2〜6人すべてでランダムに明確勝利・提出可能・ライブラリ不要** |
| MCTS N人対応 | ✅ negamax→勝者ベースに修正（C++ / Python 両方） |
| **NN 人数非依存化** | ✅ 入力9ch固定・視点相対・value=6人勝率ベクトル。何人でも1つのネットで対応 |
| NN強化学習（2〜6人混合） | ✅ iter300完走。汎用ベースライン完成 |

### 📊 完走版NN（generalist iter300）の対ランダム勝率（各20戦）
| 人数 | NN勝率 | 基準 | | 人数 | NN勝率 | 基準 |
|---|---|---|---|---|---|---|
| 2人 | 70% | 50% | | 4人 | 30% | 25% |
| 3人 | 40% | 33% | | 6人 | 20% | 17% |

→ 全人数でランダム超え（2人が最強）。ただし純粋MCTS(2人100%/4人70%/6人33%)には未到達。
→ NNは「明日の人数特化ファインチューニングの土台」。提出本命は C++純粋MCTS のまま。

退避ファイル: `checkpoint_generalist_iter300.pt.bak`（汎用完走版）, `checkpoint_2p_iter60.pt.bak`, `checkpoint_OLD_buggy.pt.bak`

**結論: 提出物の本命（C++純粋MCTS）は全人数対応で確保済み。**
NNは「人数判明後に専用ファインチューニングで特化」する土台として2〜6人混合で学習中。

### ⚠ 重要：人数が不明でも問題なし
- C++ MCTSは実行時に `num_players` を渡すだけ。コード変更不要で何人でも動く。
- NNも人数非依存アーキテクチャ。明日人数判明 → `evaluate.py --players N` で確認、
  必要なら `--players_min N --players_max N` で短時間ファインチューニングして特化。

---

## ▶ すぐ動かすコマンド（C:\private\claude\sofcon で実行）

### C++ MCTSプレイヤーの動作確認（ランダムとの対戦）
```bash
cd cpp
g++ -O2 -std=c++17 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test
./mcts_test.exe
```

### 15×15の1手あたり時間を測る
```bash
cd cpp
g++ -O2 -std=c++17 bench.cpp -o bench
./bench.exe
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

## 📋 次回（明日）やることリスト（優先順）

### ★最優先：会社で2つ確認する
1. **最大プレイヤー数**（2〜6のどれか）
2. **1ターンの制限時間**（これでsims数=強さが決まる）
   - 計測済み(15×15初手・最悪): sims200=0.6秒/400=1.2秒/800=2.4秒/1600=4.7秒
   - 制限1秒→sims200〜300か高速化 / 制限3〜5秒→sims800〜1600で強力

### 判明後の作業
3. **NNを判明人数で集中ファインチューニング**（汎用版を土台に分散をやめる）
   ```bash
   # 例：4人と判明したら
   python train/self_play.py --iters 150 --sims 50 --games 4 --players_min 4 --players_max 4
   ```
   → その後 `python tools/evaluate.py --games 30 --sims 50 --players 4` で強さ確認
4. **NN vs 純粋MCTS** で勝てるか判定 → 勝てたらNN版を提出、ダメなら純粋MCTS提出
   ```bash
   python tools/head2head.py --games 20 --nn_sims 80 --roll_sims 200 --players 4
   ```
5. （制限が厳しければ）`cpp/mcts_player.cpp` の `gen_moves` 高速化（vector確保削減で2〜4倍）
6. **提出**: `cpp/mcts_player.cpp` の `choose_move()` をコンテストのI/F（Pos構造体・関数名）に合わせる
   - 純粋MCTSなら weights 不要・g++のみでコンパイル可・全人数対応済み

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
- Python実行は必ず `C:\private\claude\sofcon` 直下から（`game`等のimportのため）
- Windowsのprint絵文字はcp932で落ちる → ASCIIで書く
- C++ MCTSで `std::vector<Node>` の参照を `push_back` をまたいで保持すると
  use-after-free でセグフォる → 必ずインデックス参照する（修正済み）
- 学習は5iterごとに `model/checkpoint.pt` 保存。PCが落ちても再開可
