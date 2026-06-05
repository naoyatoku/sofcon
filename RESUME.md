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

## 🟢 現在の到達点

| 成果物 | 状態 |
|--------|------|
| **`prog.cpp`** | ✅ **提出本命。スレッドプール+終盤ソルバー完備** |
| **スレッドプール並列化** | ✅ StartStage()で起動、50ステージ通して稼働。per-callスレッド生成コストゼロ |
| **終盤厳密解ソルバー** | ✅ 空き≤20マスで完全探索（パラノイド・2〜5人対応）|
| 時間管理（StartStage連動） | ✅ StartStage()でステージカウント→残り時間÷残りステージで予算配分 |
| **DLLビルド環境** | ✅ MSYS2 MinGW-w64 GCC 15.2.0 インストール済み（C:\msys64） |
| **build.bat** | ✅ ダブルクリックで prog.dll 生成 |
| **実機テスト** | ✅ 22/30ステージ勝利（シングルスレッド版）|

### 📊 セルフテスト対ランダム勝率（9×9, 各20戦）
| 人数 | 勝率 | ベースライン | 判定 |
|---|---|---|---|
| 2人 | **100%** | 50% | ✅ 圧勝 |
| 3人 | **85%** | 33% | ✅ 圧勝 |
| 4人 | **60%** | 25% | ✅ 優勢 |
| 5人 | **30%** | 20% | ✅ OK |

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

## 📋 次回やることリスト（優先順）

### ★最優先：実機テスト
1. **スレッドプール版を実機でテスト**
   - `build.bat` でビルド → `prog.dll` + `libwinpthread-1.dll` を提出
   - 前回 22/30勝 → スレッドプールで改善期待
   - もし弱くなった場合は `prog.cpp` の `pool::init_once()` を無効化して単スレッドに戻す

2. **コンテスト側ヘッダ（entry.h）がある場合**
   - `entry.h` をプロジェクトフォルダに置いてから `build.bat` を実行
   - `entry.h` の中で STAGE_Y_MAX 等が定義されていれば prog.cpp の `#ifndef` が無視される

### 必要なら
3. **スレッドプールが不安定な場合** → `pool::init_once()` をコメントアウトしてシングルスレッドに
4. **`libwinpthread-1.dll` が配布できない場合** → winlibs win32スレッド版に移行（GitHub: brechtsanders/winlibs_mingw で "win32" 版を手動ダウンロード）

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
