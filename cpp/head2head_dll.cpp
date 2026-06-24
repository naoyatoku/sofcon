// head2head_dll.cpp — 提出DLL同士を対戦させる検証ハーネス
//
// 2つの prog*.dll を LoadLibrary で読み込み、ゲームエンジン上で対戦させる。
// 各ゲームごとに DLL を再読込して時間予算(g_start)をリセット → 公平比較。
//
// ビルド:
//   g++ -O2 -std=c++14 head2head_dll.cpp -o head2head_dll.exe
// 実行:
//   head2head_dll.exe <dllA> <dllB> [games] [board] [players] [takes]
//   例: head2head_dll.exe ../prog_nn.dll ../prog_nonn.dll 40 11 2 3

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <random>
#include <string>

static const int Y_MAX = 15, X_MAX = 15, LANDS_MAX = 10;

typedef struct { int x, y; } TAKE_TAG;
typedef struct { char your_number, number_takes, number_players; } RULES_TAG;

typedef void (*StartFn)();
typedef void (*EndFn)();
typedef void (*PlayFn)(char[Y_MAX][X_MAX], char*, TAKE_TAG[LANDS_MAX], RULES_TAG);

struct Engine {
    HMODULE h = nullptr;
    StartFn start = nullptr;
    EndFn   end   = nullptr;
    PlayFn  play  = nullptr;
    std::string path;

    bool load(const std::string& p) {
        path = p;
        h = LoadLibraryA(p.c_str());
        if (!h) { printf("LoadLibrary失敗: %s (err=%lu)\n", p.c_str(), GetLastError()); return false; }
        start = (StartFn)GetProcAddress(h, "StartStage");
        end   = (EndFn)  GetProcAddress(h, "EndStage");
        play  = (PlayFn) GetProcAddress(h, "PlayStage");
        if (!start || !end || !play) { printf("export解決失敗: %s\n", p.c_str()); return false; }
        return true;
    }
    void unload() { if (h) { FreeLibrary(h); h = nullptr; } }
};

// ----- ゲーム盤面 -----
struct Game {
    int H, W, n, takes;
    std::vector<std::vector<int>> g;  // -1壁 0空き 1..n
    int empty_count = 0;

    void init(int h, int w, int np, int nt, double wall_ratio, std::mt19937& rng) {
        H = h; W = w; n = np; takes = nt;
        g.assign(H, std::vector<int>(W, 0));
        empty_count = H * W;
        int walls = (int)(H * W * wall_ratio);
        std::uniform_int_distribution<int> dr(0, H-1), dc(0, W-1);
        for (int i = 0; i < walls; ++i) {
            int r = dr(rng), c = dc(rng);
            if (g[r][c] == 0) { g[r][c] = -1; --empty_count; }
        }
    }
};

// 手の妥当性チェック（4連結・空き・範囲・枚数）
static bool valid_move(const Game& gm, const std::vector<int>& cells) {
    int cnt = (int)cells.size();
    if (cnt < 1 || cnt > gm.takes) return false;
    for (int idx : cells) {
        int r = idx / gm.W, c = idx % gm.W;
        if (r < 0 || r >= gm.H || c < 0 || c >= gm.W) return false;
        if (gm.g[r][c] != 0) return false;
    }
    // 連結性（cells集合内でBFS）
    std::vector<int> stk = { cells[0] };
    std::vector<int> seen = { cells[0] };
    auto inset = [&](int v){ for (int x : cells) if (x==v) return true; return false; };
    auto inseen = [&](int v){ for (int x : seen) if (x==v) return true; return false; };
    const int DR[4]={-1,1,0,0}, DC[4]={0,0,-1,1};
    while (!stk.empty()) {
        int v = stk.back(); stk.pop_back();
        int r = v / gm.W, c = v % gm.W;
        for (int d=0; d<4; ++d) {
            int nr=r+DR[d], nc=c+DC[d];
            if (nr<0||nr>=gm.H||nc<0||nc>=gm.W) continue;
            int nv = nr*gm.W+nc;
            if (inset(nv) && !inseen(nv)) { seen.push_back(nv); stk.push_back(nv); }
        }
    }
    return (int)seen.size() == cnt;
}

// 1ゲーム実行。seat[p] = そのプレイヤーを操作するEngineのインデックス(0/1)
// 戻り値: 勝者プレイヤー番号(1..n)、不正手は -操作Engine で表現
static int play_game(Engine eng[2], int seatEngine[6], const Game& start_board) {
    Game gm = start_board;

    // 各DLLを再読込して時間予算リセット
    for (int e = 0; e < 2; ++e) {
        eng[e].unload();
        eng[e].load(eng[e].path);
        eng[e].start();  // StartStage（モード検出リセット・スレッドプール起動）
    }

    int cur = 1;
    int guard = 0;
    while (gm.empty_count > 0 && guard++ < 10000) {
        int e = seatEngine[cur];

        char floor[Y_MAX][X_MAX];
        for (int r = 0; r < Y_MAX; ++r)
            for (int c = 0; c < X_MAX; ++c)
                floor[r][c] = (r < gm.H && c < gm.W) ? (char)gm.g[r][c] : (char)-1;

        char count = 0;
        TAKE_TAG lands[LANDS_MAX];
        memset(lands, 0, sizeof(lands));
        RULES_TAG rules;
        rules.your_number    = (char)cur;
        rules.number_takes   = (char)gm.takes;
        rules.number_players = (char)gm.n;

        eng[e].play(floor, &count, lands, rules);

        std::vector<int> cells;
        for (int i = 0; i < (int)count && i < LANDS_MAX; ++i)
            cells.push_back(lands[i].y * gm.W + lands[i].x);

        if (!valid_move(gm, cells)) {
            for (int x = 0; x < 2; ++x) eng[x].end();
            return -(e + 1);  // 不正手 → そのEngineの負け
        }
        for (int idx : cells) { gm.g[idx/gm.W][idx%gm.W] = cur; --gm.empty_count; }

        if (gm.empty_count == 0) {
            for (int x = 0; x < 2; ++x) eng[x].end();
            return cur;  // 最後に取ったプレイヤーの勝ち
        }
        cur = cur % gm.n + 1;
    }
    for (int x = 0; x < 2; ++x) eng[x].end();
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("使い方: head2head_dll <dllA> <dllB> [games=40] [board=11] [players=2] [takes=3]\n");
        return 1;
    }
    Engine eng[2];
    eng[0].path = argv[1];
    eng[1].path = argv[2];

    int games   = (argc>3)? atoi(argv[3]) : 40;
    int board   = (argc>4)? atoi(argv[4]) : 11;
    int players = (argc>5)? atoi(argv[5]) : 2;
    int takes   = (argc>6)? atoi(argv[6]) : 3;

    // 初回ロード確認
    if (!eng[0].load(eng[0].path) || !eng[1].load(eng[1].path)) return 1;

    printf("=== Head-to-Head ===\n");
    printf("A = %s\nB = %s\n", argv[1], argv[2]);
    printf("games=%d board=%dx%d players=%d takes=%d\n\n", games, board, board, players, takes);

    std::mt19937 rng(12345);
    int winA = 0, winB = 0, draw = 0, invalidA = 0, invalidB = 0;

    for (int gi = 0; gi < games; ++gi) {
        Game gb;
        gb.init(board, board, players, takes, 0.10, rng);

        // 座席割当：偶数ゲームは A=P1, 奇数は B=P1（先手の有利を相殺）
        int seatEngine[6] = {0};
        for (int p = 1; p <= players; ++p) {
            // 交互に割当（P1から A,B,A,B... を gi でずらす）
            seatEngine[p] = ((p - 1 + gi) % 2 == 0) ? 0 : 1;
        }

        int w = play_game(eng, seatEngine, gb);

        if (w > 0) {
            int e = seatEngine[w];
            if (e == 0) ++winA; else ++winB;
        } else if (w < 0) {
            int e = -w - 1;
            if (e == 0) { ++invalidA; ++winB; }
            else        { ++invalidB; ++winA; }
        } else ++draw;

        printf("game %2d: winner=P%d (engine %s)\n", gi+1,
               (w>0?w:0), (w>0? (seatEngine[w]==0?"A":"B") : (w<0?"INVALID":"draw")));
    }

    eng[0].unload(); eng[1].unload();

    printf("\n=== 結果 ===\n");
    printf("A 勝利: %d\n", winA);
    printf("B 勝利: %d\n", winB);
    if (draw)     printf("引分: %d\n", draw);
    if (invalidA) printf("A 不正手: %d\n", invalidA);
    if (invalidB) printf("B 不正手: %d\n", invalidB);
    printf("A勝率: %.1f%%\n", 100.0 * winA / games);
    return 0;
}
