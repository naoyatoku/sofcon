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

## 📌 別PCでの続き（2026-06-29 引き継ぎ）— まずここを読む

### A. NN 学習（このPCで継続中。別PCへは git pull で引き継ぐ）
- checkpoint は **iter 3090 付近**まで進行（強い教師＋6ワーカー並列、低電力モードで安定稼働）。
- 起動コマンド（AC直挿し必須・スリープ対策済み）:
  ```
  python train/self_play.py --board_h 15 --board_w 15 --channels 16 --blocks 3 \
      --sims 50 --iters 6000 --games 6 --workers 6 --players_min 2 --players_max 3 \
      --save model/checkpoint_16ch_15x15.pt
  ```
- **早期評価（head2head: NN-MCTS vs 純粋MCTS, 2P）**: iter2425=50.0%、iter2970も≈50%。
  → まだ「強い教師が明確に効いた」とは言えず**互角**。実機テスト（明日の環境）が本判断。
  - 注意: head2head は純粋MCTS相手であって**実機botとは別物**。
  - `tools/head2head.py` は arch(channels/blocks) を checkpoint から読むよう修正済み。

### B. conv2d 用テンソルヘルパー（別PCで続ける本題）→ `cpp/tensor.h`
- 目的: NN推論の conv2d を、先頭ポインタ+offsetで窓をずらす軽量アクセサで実装。
  範囲外0返しで padding を自然表現、dot() の x_ofs/y_ofs でカーネルをスライド。
- **🔴 まず直す致命バグ**: `tensor_2d::v()` の添字 `y*__W*x` → 正しくは `y*__W + x`
  （ファイル内 FIXME 済み）。これを直さないと conv 出力が全部壊れる。
- その他の指摘（tensor.h 冒頭コメントに全記載）:
  - `__name`（先頭2アンダースコア）は処理系予約 → 改名推奨。
  - `tensor_3d::__win_cache` は共有可変でスレッド非安全。推論はスレッドプール並列
    なので、重み共有なら dot() 内でローカル tensor_2d を作る方式へ。
  - 形状ASSERT・conv内部の分岐除去（高速化）は後回しでOK。
- **次の一手**: バグ修正 → 既存 `cpp/inference_impl.h` の `conv2d` と同入力で
  出力一致する単体テスト（`cpp/test_tensor.cpp` 想定）を書いて突き合わせる。
  ASSERT は SOFCON_DEBUG 時のみ有効な自前マクロを用意（本番で abort させない）。

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

### 🧠🔥 強い教師（exact endgame solver）導入（2026-06-25）
**問題**: sims=50 の MCTS が作る教師が弱く、loss を下げても「弱い先生の真似」で頭打ち。
**対策**: 終盤（empty ≤ `SOLVE_THRESHOLD=12`）を**厳密ソルバーで解いて教師ラベルに**。
- `game/solver.py` … メモ化 maxn 厳密ソルバー。`solve_value_vec()` / `solve_best_move()`。
  - 実測コスト(2p,k=3,最悪密ブロブ): empty12≈0.56s, 14≈2.1s, 16≈124s(爆発) → 閾値12が安全。
- `model/mcts.py` … 葉が empty≤12 なら NN value の代わりに**厳密値**を backup（探索自体が強化）。
- `train/self_play.py` … 保存局面が empty≤12 なら **value教師=厳密勝者 / policy教師=最適手 one-hot** で上書き。
- コスト: 1ゲーム 87s→100s（+13s, 終盤求解分）。≈6.7分/iter。
- **学習再開済み**: iter 1935 から継続中。ログ `train/strong_teacher.log`（PID は起動時）。
  起動コマンド例:
  ```
  python train/self_play.py --board_h 15 --board_w 15 --channels 16 --blocks 3 \
      --sims 50 --iters 6000 --games 4 --players_min 2 --players_max 3 \
      --save model/checkpoint_16ch_15x15.pt
  ```
### ⚡ マルチプロセス並列化（2026-06-27 実装済み）
- 逐次自己対戦は実質1コアのみ使用 → 約31分/iter と判明（当初見積6.7分は誤り）。
- `train/self_play.py` に `--workers N` を追加。`mp.Pool` で各ゲームを独立プロセスで並列実行。
  - 各 worker は `_worker_init` で自前の net を1つ持ち、`torch.set_num_threads(1)` でピン留め。
  - 毎iter 親が `state_dict` を全 worker に配り、`imap_unordered` でゲーム結果を回収。
- **本番起動**: `--workers 12 --games 12 --sims 50`（物理14コア中12使用、OS用に余裕）。
  ```
  python train/self_play.py --board_h 15 --board_w 15 --channels 16 --blocks 3 \
      --sims 50 --iters 6000 --games 12 --workers 12 --players_min 2 --players_max 3 \
      --save model/checkpoint_16ch_15x15.pt
  ```
  ログ: `train/parallel.log` / `.err`。

### 🔌 電源・スリープ対策（2026-06-27）— 学習が夜間に止まる問題
- 原因①: **USBハブ給電の接触不良で瞬断** → AC直挿しに変更で解消。
- 原因②: **蓋を閉じると Modern Standby（スリープ）でプロセス凍結**（電源断と誤認）。
  - `powercfg /setacvalueindex SCHEME_CURRENT SUB_BUTTONS LIDACTION 0`（蓋AC=何もしない）
  - `powercfg /change standby-timeout-ac 0`（アイドルスリープなし）
  - `self_play.py` の `_keep_system_awake()`（`SetThreadExecutionState`）でOSレベルにも禁止。
- **AC直挿し必須**（DC=バッテリー時はスリープ設定据え置き）。

### 🧠 NN 学習状況（2026-06-24 時点）
- `model/checkpoint_16ch_15x15.pt`（15×15, **16ch×3blocks**, players 2-3）
- **iter 1691 / loss 約0.82**。sims=50 に上げて教師を強化中（途中で停止しコミット）
- `cpp/weights.h` は iter1691 の checkpoint から生成済み（コミット済み）
- 実機 27勝（過去最高）は NN込みビルド。NN無し版と head-to-head では NN版が弱い
  → 現NN(self-play sims少)は policy が弱く、本領は今後の高sims学習しだい

### 💻 別PC（GPU機）で学習を再開する手順
```bash
git clone <repo> && cd sofcon          # checkpoint/weights.h はコミット済み
# 学習再開（--no-resume を付けない＝checkpointから継続）
python train/self_play.py --board_h 15 --board_w 15 \
    --channels 16 --blocks 3 --sims 50 --iters 6000 \
    --games 4 --players_min 2 --players_max 3 \
    --save model/checkpoint_16ch_15x15.pt
# 学習後に重みを反映：
python tools/export_weights.py --checkpoint model/checkpoint_16ch_15x15.pt --output cpp/weights.h
```
⚠️ アーキテクチャ（channels=16, blocks=3）は inference.h と一致必須。変えると C++ 推論が壊れる。

### ⚡ 学習を速くするには（重要）
- **GPUはあまり効かない**：NNが極小＋MCTSは batch=1 逐次評価のため。真の律速は Python の MCTS/move-gen（CPU単スレッド）。
- **効くのは CPU マルチプロセス並列**：自己対戦の各ゲームは独立 → コア数分の worker で並列実行すれば ほぼコア数倍。
  - 現状 sims=50 で約5.8分/iter（4ゲーム逐次）。8並列なら約0.7分/iter ≈ 8倍速の見込み。
  - **未実装**。`train/self_play.py` の `run_training` 内 `for _ in range(games_per_iter)` ループを
    `multiprocessing.Pool` で並列化すればよい（play_game は独立なので素直に並列化できる）。
- さらに上：並列ゲームのNN評価をバッチ化すれば GPU も活きる（大掛かり）。

### 🌿 ブランチ
- `main` … 提出版（実機27勝）。クリーンに保つ
- `bot-diag` … botモデル自己診断モード（`-DBOT_DIAG` ビルドで prog_diag.dll）。
  `<det_solved>`なのに負ける残バグ（botモデルの実機ズレ）を特定する用。
  実機で走らせ、ズレたら `bot_diag.log` に記録して abort する。

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
