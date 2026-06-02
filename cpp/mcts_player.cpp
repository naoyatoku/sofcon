/**
 * mcts_player.cpp  —  自己完結型の純粋MCTSプレイヤー（ベアメタル / 外部ライブラリ不要）
 *
 *  【時間管理】
 *    全50ステージ通算30秒以内の制約に対応。
 *    プロセス起動からの経過時間をグローバルに管理し、
 *    残り時間÷残り推定手数 で 1手あたりの予算を自動計算する。
 *
 *  ルール:
 *    board[r][c]:  -1=壁, 0=空き, 1以上=各プレイヤーが掘った
 *    1ターン 1〜3 マスの「直交連結した空きマス」を掘る
 *    勝敗: 最後の空きマスを掘ったプレイヤーの勝ち（normal play）
 *
 *  コンパイル:  g++ -O2 -std=c++17 mcts_player.cpp -o mcts_player
 */

#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>

namespace mcts {

constexpr int MAXN   = 15;
constexpr int WALL   = -1;
constexpr int EMPTY  = 0;

// ------------------------------------------------------------------ //
//  グローバル時間管理
//    TOTAL_BUDGET_MS : 全ステージで使える上限（余裕を持って28秒）
//    g_start         : プロセス起動時刻
// ------------------------------------------------------------------ //
static constexpr double TOTAL_BUDGET_MS = 28000.0;  // 30秒 - 2秒の安全マージン
static const auto g_start = std::chrono::steady_clock::now();

static double elapsed_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_start).count();
}
static double remaining_ms() {
    return std::max(0.0, TOTAL_BUDGET_MS - elapsed_ms());
}

// 1手あたりの時間予算を推定（静的カウンタで総手数を管理）
// board_cells: 盤面の総セル数（H*W）, num_players: プレイヤー数
static double budget_per_move(int board_cells, int num_players) {
    // 初回呼び出し時にステージ全体の総手数（自分の番）を推定して固定
    static int s_total_our_turns = -1;
    static int s_turns_done      = 0;

    if (s_total_our_turns < 0) {
        // 空きマス ≒ 盤面の90%、平均2マス/手、自分の番は1/num_players
        int our_turns_per_stage = std::max(1,
            (int)(board_cells * 0.9 / 2.0 / num_players));
        s_total_our_turns = 50 * our_turns_per_stage;  // 50ステージ分
    }

    double rem   = remaining_ms();
    int rem_turns = std::max(1, s_total_our_turns - s_turns_done);
    double budget = rem / rem_turns;
    ++s_turns_done;

    // 最低1ms、最大200ms
    return std::max(1.0, std::min(budget, 200.0));
}

// ------------------------------------------------------------------ //
//  Board
// ------------------------------------------------------------------ //
struct Board {
    int8_t g[MAXN][MAXN];
    int H, W, num_players;
    int current_player;   // 1-indexed
    int empty_count;
    bool done;
    int winner;

    void init(const int board[][MAXN], int h, int w, int np, int cur) {
        H = h; W = w; num_players = np; current_player = cur;
        empty_count = 0; done = false; winner = 0;
        for (int r = 0; r < H; ++r)
            for (int c = 0; c < W; ++c) {
                g[r][c] = (int8_t)board[r][c];
                if (g[r][c] == EMPTY) ++empty_count;
            }
    }

    void apply(const int* cells, int n) {
        int pid = current_player;
        for (int i = 0; i < n; ++i) {
            int r = cells[i] / W, c = cells[i] % W;
            g[r][c] = (int8_t)pid;
        }
        empty_count -= n;
        if (empty_count == 0) { done = true; winner = pid; }
        else current_player = current_player % num_players + 1;
    }
};

struct Move {
    int cells[3];
    int n;
};

static const int DR[4] = {-1,  1,  0, 0};
static const int DC[4] = { 0,  0, -1, 1};

static std::mt19937 rng(
    (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());

// ------------------------------------------------------------------ //
//  合法手生成（展開フェーズ用）
// ------------------------------------------------------------------ //
static void gen_moves(const Board& b, std::vector<Move>& out) {
    out.clear();
    int H = b.H, W = b.W;
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            if (b.g[r][c] != EMPTY) continue;
            int a = r * W + c;
            out.push_back({{a, 0, 0}, 1});
            for (int d = 0; d < 4; ++d) {
                int r2 = r + DR[d], c2 = c + DC[d];
                if (r2 < 0 || r2 >= H || c2 < 0 || c2 >= W) continue;
                if (b.g[r2][c2] != EMPTY) continue;
                int bb = r2 * W + c2;
                if (bb < a) continue;              // 正準: a を最小にして重複排除
                out.push_back({{a, bb, 0}, 2});
                for (int src = 0; src < 2; ++src) {
                    int rr = (src == 0 ? r : r2), cc = (src == 0 ? c : c2);
                    for (int e = 0; e < 4; ++e) {
                        int r3 = rr + DR[e], c3 = cc + DC[e];
                        if (r3 < 0 || r3 >= H || c3 < 0 || c3 >= W) continue;
                        if (b.g[r3][c3] != EMPTY) continue;
                        int cc3 = r3 * W + c3;
                        if (cc3 == a || cc3 == bb || cc3 < a) continue;
                        int s0 = a, s1 = bb, s2 = cc3;
                        if (s1 > s2) std::swap(s1, s2);
                        if (s0 > s1) std::swap(s0, s1);
                        if (s1 > s2) std::swap(s1, s2);
                        out.push_back({{s0, s1, s2}, 3});
                    }
                }
            }
        }
    }
    // サイズ3のトリプル重複を除去
    std::sort(out.begin(), out.end(), [](const Move& x, const Move& y) {
        if (x.n != y.n) return x.n < y.n;
        if (x.cells[0] != y.cells[0]) return x.cells[0] < y.cells[0];
        if (x.cells[1] != y.cells[1]) return x.cells[1] < y.cells[1];
        return x.cells[2] < y.cells[2];
    });
    out.erase(std::unique(out.begin(), out.end(), [](const Move& x, const Move& y) {
        return x.n == y.n &&
               x.cells[0] == y.cells[0] &&
               x.cells[1] == y.cells[1] &&
               x.cells[2] == y.cells[2];
    }), out.end());
}

// ------------------------------------------------------------------ //
//  高速ランダム手（ロールアウト専用）
//    gen_moves の sort+unique を省略し、1手だけ素早くサンプリングする。
//    分布は一様ではないが、ロールアウトの精度には影響しない。
// ------------------------------------------------------------------ //
static Move fast_random_move(const Board& b) {
    // 空きマス一覧（最大 MAXN*MAXN = 225）
    int empties[MAXN * MAXN], ne = 0;
    for (int r = 0; r < b.H; ++r)
        for (int c = 0; c < b.W; ++c)
            if (b.g[r][c] == EMPTY) empties[ne++] = r * b.W + c;

    // ランダムな起点を選ぶ
    Move m; m.cells[0] = empties[rng() % ne]; m.n = 1;

    // 50% の確率で最大2回まで隣接マスへ延伸する
    for (int ext = 0; ext < 2; ++ext) {
        if (rng() & 1) break;
        // move 内のランダムなセルから4方向をシャッフルして試みる
        int src  = m.cells[rng() % m.n];
        int r0   = src / b.W, c0 = src % b.W;
        int dirs[4] = {0, 1, 2, 3};
        for (int i = 3; i > 0; --i) std::swap(dirs[i], dirs[rng() % (i + 1)]);
        bool extended = false;
        for (int di = 0; di < 4 && !extended; ++di) {
            int d  = dirs[di];
            int r2 = r0 + DR[d], c2 = c0 + DC[d];
            if (r2 < 0 || r2 >= b.H || c2 < 0 || c2 >= b.W) continue;
            if (b.g[r2][c2] != EMPTY) continue;
            int nb = r2 * b.W + c2;
            bool dup = false;
            for (int i = 0; i < m.n; ++i) if (m.cells[i] == nb) { dup = true; break; }
            if (!dup) { m.cells[m.n++] = nb; extended = true; }
        }
        if (!extended) break;
    }
    return m;
}

// ------------------------------------------------------------------ //
//  MCTS（negamax、N人対応）
// ------------------------------------------------------------------ //
struct Node {
    Board board;
    Move  move;
    int   parent;
    std::vector<int> children;
    int    visit     = 0;
    double value_sum = 0.0;  // このノードを指した側（parent の current_player）視点
    bool   expanded  = false;
};

class Tree {
public:
    std::vector<Node> nodes;

    int alloc(const Board& b, const Move& m, int parent) {
        nodes.push_back(Node{});
        Node& n  = nodes.back();
        n.board  = b; n.move = m; n.parent = parent;
        return (int)nodes.size() - 1;
    }

    // 高速ロールアウト：fast_random_move を使用
    int rollout(Board b) {
        while (!b.done) {
            Move m = fast_random_move(b);
            b.apply(m.cells, m.n);
        }
        return b.winner;
    }

    void expand(int idx) {
        if (nodes[idx].board.done) { nodes[idx].expanded = true; return; }
        Board pb = nodes[idx].board;  // push_back で nodes[] が再確保される前にコピー
        std::vector<Move> mv;
        gen_moves(pb, mv);
        for (const Move& m : mv) {
            Board nb = pb;
            nb.apply(m.cells, m.n);
            int ci = alloc(nb, m, idx);
            nodes[idx].children.push_back(ci);
        }
        nodes[idx].expanded = true;
    }

    int select(int idx) {
        while (nodes[idx].expanded &&
               !nodes[idx].children.empty() &&
               !nodes[idx].board.done)
        {
            const Node& n   = nodes[idx];
            double logN     = std::log((double)n.visit + 1.0);
            double best     = -1e18;
            int    best_c   = n.children[0];
            for (int ci : n.children) {
                const Node& c = nodes[ci];
                double q = (c.visit > 0) ? (c.value_sum / c.visit) : 0.0;
                double u = 1.41421356 * std::sqrt(logN / (c.visit + 1.0));
                if (q + u > best) { best = q + u; best_c = ci; }
            }
            idx = best_c;
        }
        return idx;
    }

    // N人対応バックプロパゲーション：勝者プレイヤーの手番ノードに+1
    void backprop(int idx, int winner) {
        while (idx != -1) {
            nodes[idx].visit++;
            int par = nodes[idx].parent;
            if (par != -1) {
                if (nodes[par].board.current_player == winner)
                    nodes[idx].value_sum += 1.0;
            }
            idx = par;
        }
    }

    Move best_child(int root) {
        int best = nodes[root].children[0], best_v = -1;
        for (int ci : nodes[root].children)
            if (nodes[ci].visit > best_v) { best_v = nodes[ci].visit; best = ci; }
        return nodes[best].move;
    }

    // ---- 時間ベース探索（本番用） ----
    Move search_timed(const Board& root_board, double budget_ms) {
        auto t0 = std::chrono::steady_clock::now();
        nodes.clear();
        nodes.reserve(4096);
        int root = alloc(root_board, Move{{0,0,0},0}, -1);
        expand(root);
        if (nodes[root].children.empty()) return Move{{0,0,0},0};

        int sims = 0;
        while (true) {
            // 8シムごとに時間チェック（チェック自体のオーバーヘッドを抑制）
            if ((sims & 7) == 0) {
                double used = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (used >= budget_ms) break;
            }
            int leaf = select(root);
            int winner;
            if (nodes[leaf].board.done) {
                winner = nodes[leaf].board.winner;
            } else {
                if (!nodes[leaf].expanded) expand(leaf);
                winner = rollout(nodes[leaf].board);
            }
            backprop(leaf, winner);
            ++sims;
        }
        return best_child(root);
    }

    // ---- sims固定探索（テスト用） ----
    Move search(const Board& root_board, int num_sims) {
        nodes.clear();
        nodes.reserve(num_sims * 4 + 64);
        int root = alloc(root_board, Move{{0,0,0},0}, -1);
        expand(root);
        if (nodes[root].children.empty()) return Move{{0,0,0},0};
        for (int s = 0; s < num_sims; ++s) {
            int leaf = select(root);
            int winner;
            if (nodes[leaf].board.done) {
                winner = nodes[leaf].board.winner;
            } else {
                if (!nodes[leaf].expanded) expand(leaf);
                winner = rollout(nodes[leaf].board);
            }
            backprop(leaf, winner);
        }
        return best_child(root);
    }
};

} // namespace mcts

// ====================================================================== //
//  提出用インターフェース
// ====================================================================== //
struct Pos { int row, col; };

/**
 * board        : board[r][c]  (-1=壁, 0=空き, 1以上=各プレイヤー)
 * height,width : 盤面サイズ (<=15)
 * my_id        : 自分のプレイヤーID (1-indexed)
 * num_players  : 総プレイヤー数 (2〜5)
 * num_sims     : >0 なら sims 固定（テスト用）、0 なら時間ベース（本番用）
 */
std::vector<Pos> choose_move(
    const int board[][mcts::MAXN], int height, int width,
    int my_id, int num_players, int num_sims = 0)
{
    mcts::Board b;
    b.init(board, height, width, num_players, my_id);

    mcts::Tree tree;
    mcts::Move m;
    if (num_sims > 0) {
        m = tree.search(b, num_sims);
    } else {
        double budget = mcts::budget_per_move(height * width, num_players);
        m = tree.search_timed(b, budget);
    }

    std::vector<Pos> result;
    for (int i = 0; i < m.n; ++i)
        result.push_back({m.cells[i] / width, m.cells[i] % width});
    return result;
}

// ====================================================================== //
//  コンテスト提出用 DLL インターフェース
//
//  PlayStage() が1ターンごとに呼ばれる。
//  floor[y][x] の y=行・x=列。lands[i] の x=列・y=行。
//
//  コンパイル（DLL）:
//    g++ -O2 -std=c++17 -shared -fPIC mcts_player.cpp -o mcts_player.dll
//    ※ __declspec(dllexport) は MSVC/MinGW 向け。Linux なら不要。
// ====================================================================== //

// コンテスト側ヘッダで定義されていない場合のデフォルト
#ifndef STAGE_Y_MAX
#  define STAGE_Y_MAX 15
#endif
#ifndef STAGE_X_MAX
#  define STAGE_X_MAX 15
#endif
#ifndef LANDS_SHARK_MAX
#  define LANDS_SHARK_MAX 10
#endif

typedef struct {
    int x;   // 取る陣地位置(列)
    int y;   // 取る陣地位置(行)
} TAKE_TAG;

typedef struct {
    char your_number;     // あなたのプレイヤナンバー (当日戦は1固定)
    char number_takes;    // 1ターンに取得可能な陣地数 (3〜10, 当日戦は3固定)
    char number_players;  // ゲームに参加する人数 (2〜5)
} RULES_TAG;

// Windows DLL エクスポート属性（Linux/WSL ではスキップ）
#ifdef _WIN32
#  define DLL_EXPORT extern "C" __declspec(dllexport)
#else
#  define DLL_EXPORT extern "C"
#endif

DLL_EXPORT
void PlayStage(char floor[STAGE_Y_MAX][STAGE_X_MAX],
               char*    count,
               TAKE_TAG lands[LANDS_SHARK_MAX],
               RULES_TAG rules)
{
    // ---- 1. 有効な盤面サイズを検出 ----
    //   非壁セル（!= -1）が存在する最大行・最大列から実際のH×Wを求める。
    //   これにより、STAGE_Y_MAX×STAGE_X_MAX より小さい盤面にも対応する。
    int H = 0, W = 0;
    for (int r = 0; r < STAGE_Y_MAX && r < mcts::MAXN; ++r)
        for (int c = 0; c < STAGE_X_MAX && c < mcts::MAXN; ++c)
            if ((signed char)floor[r][c] != mcts::WALL) {
                if (r + 1 > H) H = r + 1;
                if (c + 1 > W) W = c + 1;
            }
    // 全セルが壁（あり得ないが念のため）の場合のフォールバック
    if (H == 0) H = (STAGE_Y_MAX < mcts::MAXN) ? STAGE_Y_MAX : mcts::MAXN;
    if (W == 0) W = (STAGE_X_MAX < mcts::MAXN) ? STAGE_X_MAX : mcts::MAXN;

    // ---- 2. char floor[y][x] → int board[row][col] 変換 ----
    //   char は signed/unsigned 両方あり得るため (signed char) で明示的にキャスト。
    //   値: -1=壁, 0=空き, 1〜5=各プレイヤー陣地
    int board[mcts::MAXN][mcts::MAXN] = {};
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            board[r][c] = (int)(signed char)floor[r][c];

    // ---- 3. MCTSで最善手を決定 ----
    int my_id       = (int)(signed char)rules.your_number;
    int num_players = (int)(signed char)rules.number_players;

    mcts::Board b;
    b.init(board, H, W, num_players, my_id);

    double budget = mcts::budget_per_move(H * W, num_players);
    mcts::Tree tree;
    mcts::Move m = tree.search_timed(b, budget);

    // ---- 4. 結果を出力引数に書き戻す ----
    //   *count  : 取るマスの個数
    //   lands[i]: {x=列, y=行} の組
    *count = (char)m.n;
    for (int i = 0; i < m.n; ++i) {
        lands[i].y = m.cells[i] / W;   // row（floor の第1添字）
        lands[i].x = m.cells[i] % W;   // col（floor の第2添字）
    }
}

// ====================================================================== //
//  セルフテスト（提出時は削除可）
//  コンパイル: g++ -O2 -std=c++17 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test
// ====================================================================== //
#ifdef MCTS_SELFTEST
#include <cstdio>

static void run_suite(int NP, int H, int W, int GAMES, std::mt19937& r) {
    int wins = 0;
    for (int game = 0; game < GAMES; ++game) {
        int board[mcts::MAXN][mcts::MAXN] = {};
        for (int i = 0; i < H * W / 10; ++i)   // 壁 約10%
            board[r() % H][r() % W] = mcts::WALL;

        int mcts_id = (game % NP) + 1;          // 席を回して公平に
        mcts::Board b;
        b.init(board, H, W, NP, 1);

        std::vector<mcts::Move> mv;
        while (!b.done) {
            mcts::Move chosen;
            if (b.current_player == mcts_id) {
                double budget = mcts::budget_per_move(H * W, NP);
                mcts::Tree t;
                chosen = t.search_timed(b, budget);
            } else {
                mcts::gen_moves(b, mv);
                chosen = mv[r() % mv.size()];
            }
            b.apply(chosen.cells, chosen.n);
        }
        if (b.winner == mcts_id) ++wins;
    }
    double wr   = 100.0 * wins / GAMES;
    double base = 100.0 / NP;
    printf("  %dP  winrate %5.1f%%  (baseline %.1f%%)  %s\n",
           NP, wr, base,
           wr > base + 10 ? "[OK]" : (wr > base ? "[weak]" : "[BAD]"));
}

int main() {
    std::mt19937 r(42);
    const int H = 9, W = 9, GAMES = 20;
    printf("=== Time-based MCTS vs random  %dx%d  %d games ===\n", H, W, GAMES);
    for (int NP : {2, 3, 4, 5})
        run_suite(NP, H, W, GAMES, r);
    printf("Total elapsed: %.0f ms  (budget %.0f ms)\n",
           mcts::elapsed_ms(), mcts::TOTAL_BUDGET_MS);
    return 0;
}
#endif
