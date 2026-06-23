// DiscoSoftwareContest
// prog.cpp  -  MCTS + 終盤厳密解ソルバー + スレッドプール並列化
//              + NN guided MCTS (PUCT with policy priors)

// NN統合: weights.h が存在する場合のみ有効
#if __has_include("cpp/weights.h")
#  include "cpp/weights.h"
#  include "cpp/inference.h"
#  include "cpp/inference_impl.h"
#  define USE_NN 1
#endif

#include "entry.h"
#include <windows.h>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>

// コンテスト定数（entry.h で定義済みなら上書きされない）
#ifndef STAGE_MAX
#  define STAGE_MAX       50
#endif
#ifndef STAGE_Y_MAX
#  define STAGE_Y_MAX     15
#endif
#ifndef STAGE_X_MAX
#  define STAGE_X_MAX     15
#endif
#ifndef LANDS_SHARK_MAX
#  define LANDS_SHARK_MAX 10
#endif

enum FLOOR_TYPE {
    WATER = -1,   // 取れないマス（海面）
    LANDS =  0,   // 取れるマス（地面）
    AREA1 =  1,   // プレイヤー1の陣地（当日戦はあなた固定）
    AREA2 =  2,
    AREA3 =  3,
    AREA4 =  4,
    AREA5 =  5,
};

typedef struct {
    int x;   // 取る陣地位置(x=列) 返信時に更新してもらう
    int y;   // 取る陣地位置(y=行) 返信時に更新してもらう
} TAKE_TAG;

typedef struct {
    char your_number;    // あなたのプレイヤーナンバー（当日戦は1固定）
    char number_takes;   // 1ターンに取得可能な陣地数（3〜10、当日戦は3固定）
    char number_players; // ゲームに参加する人数（2〜5）
} RULES_TAG;

// ============================================================
//  MCTS エンジン
// ============================================================
namespace mcts {

constexpr int MAXN = 15;
constexpr int WALL = -1;
constexpr int EMPTY = 0;

// 終盤厳密解ソルバーの発動条件
constexpr int    ENDGAME_MAX_EMPTY = 20;       // 空き≤この数なら完全探索
constexpr long   ENDGAME_NODE_CAP  = 8000000;  // 探索ノード上限
constexpr double ENDGAME_MAX_MS    = 200.0;    // 壁時計上限（超えたらMCTSへ）

// 時間管理
static constexpr double TOTAL_BUDGET_MS = 28000.0; // 30秒 - 2秒の安全マージン
static constexpr int    TOTAL_STAGES    = 50;       // コンテストの総ステージ数

static const auto g_start = std::chrono::steady_clock::now();

static double elapsed_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_start).count();
}
static double remaining_ms() {
    return std::max(0.0, TOTAL_BUDGET_MS - elapsed_ms());
}

// ステージカウンタ（StartStage() から直接更新するためヒューリスティック不要）
static int s_stages_done = 0;
static void on_stage_start() { ++s_stages_done; }

// 1手あたりの時間予算
//   残り時間 ÷ 残りステージ数 = このステージの予算
//   ステージ予算 ÷ 自分の残り手数 = 1手の予算
static double budget_per_move(int empty_count, int num_players) {
    double rem = remaining_ms();
    int stages_left = std::max(1, TOTAL_STAGES - s_stages_done + 1);
    double stage_budget = rem / stages_left;
    double our_turns = std::max(1.0, empty_count / 3.0 / num_players);
    double budget = stage_budget / our_turns;
    return std::max(1.0, std::min(budget, 500.0));
}

// ----------------------------------------------------------------
//  Board
// ----------------------------------------------------------------
struct Board {
    int8_t g[MAXN][MAXN];
    int H, W, num_players, current_player, empty_count;
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

struct Move { int cells[3]; int n; };

static const int DR[4] = {-1,  1,  0, 0};
static const int DC[4] = { 0,  0, -1, 1};

// thread_local RNG: スレッドごとに異なるシードで探索パスを分散させる
static thread_local std::mt19937 rng{
    (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count()
               ^ (std::hash<std::thread::id>{}(std::this_thread::get_id()) * 2654435761u))
};

// ----------------------------------------------------------------
//  合法手生成（展開フェーズ用）
// ----------------------------------------------------------------
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
                if (bb < a) continue;
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

// ----------------------------------------------------------------
//  高速ランダム手（ロールアウト専用）
// ----------------------------------------------------------------
static Move fast_random_move(const Board& b) {
    int empties[MAXN * MAXN], ne = 0;
    for (int r = 0; r < b.H; ++r)
        for (int c = 0; c < b.W; ++c)
            if (b.g[r][c] == EMPTY) empties[ne++] = r * b.W + c;

    Move m; m.cells[0] = empties[rng() % ne]; m.n = 1;

    for (int ext = 0; ext < 2; ++ext) {
        if (rng() & 1) break;
        int src  = m.cells[rng() % m.n];
        int r0 = src / b.W, c0 = src % b.W;
        int dirs[4] = {0, 1, 2, 3};
        for (int i = 3; i > 0; --i) std::swap(dirs[i], dirs[rng() % (i + 1)]);
        bool extended = false;
        for (int di = 0; di < 4 && !extended; ++di) {
            int d = dirs[di];
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

// ----------------------------------------------------------------
//  NN入力変換（USE_NN時のみ）
// ----------------------------------------------------------------
#ifdef USE_NN
static void board_to_nn_input(const Board& b, float* out) {
    const int hw = b.H * b.W;
    std::fill(out, out + 9 * hw, 0.f);
    for (int r = 0; r < b.H; ++r) {
        for (int c = 0; c < b.W; ++c) {
            int idx = r * b.W + c;
            int8_t cell = b.g[r][c];
            if (cell == WALL)  out[idx] = 1.f;
            if (cell == EMPTY) out[hw + idx] = 1.f;
            for (int k = 0; k < b.num_players && k < 6; ++k) {
                int pid = (b.current_player - 1 + k) % b.num_players + 1;
                if (cell == (int8_t)pid) out[(2 + k) * hw + idx] = 1.f;
            }
        }
    }
    float np_norm = (float)(b.num_players - 1) / 5.f;
    std::fill(out + 8 * hw, out + 9 * hw, np_norm);
}
#endif

// ----------------------------------------------------------------
//  MCTS（N人対応）
// ----------------------------------------------------------------
struct Node {
    Board board; Move move; int parent;
    std::vector<int> children;
    int visit = 0; double value_sum = 0.0; bool expanded = false;
    float prior = 1.f;  // NN policy prior（未設定時は均一）
};

class Tree {
public:
    std::vector<Node> nodes;

    int alloc(const Board& b, const Move& m, int parent) {
        nodes.push_back(Node{});
        Node& n = nodes.back();
        n.board = b; n.move = m; n.parent = parent;
        return (int)nodes.size() - 1;
    }

    int rollout(Board b) {
        while (!b.done) {
            Move m = fast_random_move(b);
            b.apply(m.cells, m.n);
        }
        return b.winner;
    }

    void expand(int idx) {
        if (nodes[idx].board.done) { nodes[idx].expanded = true; return; }
        Board pb = nodes[idx].board;
        std::vector<Move> mv;
        gen_moves(pb, mv);
        for (const Move& m : mv) {
            Board nb = pb; nb.apply(m.cells, m.n);
            int ci = alloc(nb, m, idx);
            nodes[idx].children.push_back(ci);
        }
        nodes[idx].expanded = true;
    }

    int select(int idx) {
        while (nodes[idx].expanded &&
               !nodes[idx].children.empty() &&
               !nodes[idx].board.done) {
            const Node& n = nodes[idx];
            double sqrtN = std::sqrt((double)n.visit + 1.0);
            double best = -1e18;
            int best_c = n.children[0];
            for (int ci : n.children) {
                const Node& c = nodes[ci];
                double q = (c.visit > 0) ? (c.value_sum / c.visit) : 0.0;
                // PUCT: prior があれば活用、なければ UCB と同等
                double u = 1.41421356 * c.prior * sqrtN / (1.0 + c.visit);
                if (q + u > best) { best = q + u; best_c = ci; }
            }
            idx = best_c;
        }
        return idx;
    }

    void backprop(int idx, int winner) {
        while (idx != -1) {
            nodes[idx].visit++;
            int par = nodes[idx].parent;
            if (par != -1 && nodes[par].board.current_player == winner)
                nodes[idx].value_sum += 1.0;
            idx = par;
        }
    }

    // NN value を使った backprop（float win-prob、多人数対応）
    void backprop_float(int idx, float cur_player_value) {
        int leaf_player = nodes[idx].board.current_player;
        int n = nodes[idx].board.num_players;
        float opp_value = (n > 1) ? (1.f - cur_player_value) / (n - 1) : 0.f;
        while (idx != -1) {
            nodes[idx].visit++;
            int par = nodes[idx].parent;
            if (par != -1) {
                int mover = nodes[par].board.current_player;
                int k = (mover - leaf_player + n) % n;
                nodes[idx].value_sum += (k == 0) ? cur_player_value : opp_value;
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

    Move search_timed(const Board& root_board, double budget_ms) {
        auto t0 = std::chrono::steady_clock::now();
        nodes.clear(); nodes.reserve(4096);
        int root = alloc(root_board, Move{{0, 0, 0}, 0}, -1);
        expand(root);
        if (nodes[root].children.empty()) return Move{{0, 0, 0}, 0};

#ifdef USE_NN
        // NN を1回呼んでrootの子ノードにpolicy priorを設定
        {
            static float nn_input[9 * MAXN * MAXN];
            static float nn_policy[MAXN * MAXN];
            float nn_value_out = 0.f;
            board_to_nn_input(root_board, nn_input);
            sofcon_nn::NetWeights wt{};
            sofcon_nn::forward(nn_input, 9, root_board.H, root_board.W,
                               wt, nn_policy, nn_value_out);
            // 各子ノードのanchorセルのpolicyをpriorに設定
            float prior_sum = 0.f;
            for (int ci : nodes[root].children) {
                int anchor = nodes[ci].move.cells[0];
                nodes[ci].prior = std::max(nn_policy[anchor], 1e-4f);
                prior_sum += nodes[ci].prior;
            }
            // 正規化
            if (prior_sum > 0.f)
                for (int ci : nodes[root].children)
                    nodes[ci].prior /= prior_sum;
        }
#endif

        int sims = 0;
        while (true) {
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
};

// ----------------------------------------------------------------
//  終盤厳密解ソルバー（Expectimax完全探索）
//    自分の番: 最も勝率が高い手を選ぶ
//    敵の番:   全合法手の平均勝率を返す（ランダムプレイ仮定）
//    → 5人戦でも「最も勝ちやすい手」を正しく評価できる
// ----------------------------------------------------------------
struct EndgameResult {
    bool solved = false, win = false;
    float win_prob = 0.f;
    Move move{};
};

class Endgame {
public:
    EndgameResult solve(const Board& b, int me, long node_cap,
                        double max_ms = ENDGAME_MAX_MS) {
        n_ = b.num_players; me_ = me;
        node_cap_ = node_cap; nodes_ = 0;
        start_ = std::chrono::steady_clock::now(); max_ms_ = max_ms;
        memo_.clear(); cells_.clear();

        int idx_of[MAXN * MAXN];
        for (int i = 0; i < MAXN * MAXN; ++i) idx_of[i] = -1;
        for (int r = 0; r < b.H; ++r)
            for (int c = 0; c < b.W; ++c)
                if (b.g[r][c] == EMPTY) {
                    idx_of[r * b.W + c] = (int)cells_.size();
                    cells_.push_back(r * b.W + c);
                }
        int k = (int)cells_.size(); W_ = b.W;
        if (k == 0 || k > 30) return {};

        adj_.assign(k, {});
        for (int i = 0; i < k; ++i) {
            int r = cells_[i] / b.W, c = cells_[i] % b.W;
            for (int d = 0; d < 4; ++d) {
                int rr = r + DR[d], cc = c + DC[d];
                if (rr < 0 || rr >= b.H || cc < 0 || cc >= b.W) continue;
                int fi = rr * b.W + cc;
                if (idx_of[fi] >= 0) adj_[i].push_back(idx_of[fi]);
            }
        }
        buildMoves(k);

        uint32_t full = (k == 32) ? 0xffffffffu : ((1u << k) - 1u);
        EndgameResult res;
        float best_prob = -1.f;
        for (uint32_t mv : moveMasks_) {
            if ((mv & full) != mv) continue;
            uint32_t nm = full & ~mv;
            float prob = (nm == 0) ? 1.f : rec(nm, 1 % n_);
            if (prob < 0.f) break;  // タイムアウト
            if (prob > best_prob) {
                best_prob = prob;
                res.move = decode(mv);
                if (best_prob == 1.f) break;  // 必勝確定
            }
        }
        if (best_prob < 0.f) return {};  // タイムアウト（手なし）
        res.solved   = true;
        res.win_prob = best_prob;
        res.win      = (best_prob > 0.f);  // 勝ち目があれば採用
        return res;
    }

private:
    int n_, me_, W_; long node_cap_, nodes_;
    std::chrono::steady_clock::time_point start_; double max_ms_;
    std::vector<int> cells_; std::vector<std::vector<int>> adj_;
    std::vector<uint32_t> moveMasks_; std::unordered_map<uint64_t, float> memo_;

    void buildMoves(int k) {
        moveMasks_.clear(); std::vector<uint32_t> tmp;
        for (int i = 0; i < k; ++i) {
            tmp.push_back(1u << i);
            for (int j : adj_[i]) {
                if (j < i) continue;
                tmp.push_back((1u << i) | (1u << j));
            }
        }
        for (int i = 0; i < k; ++i)
            for (int j : adj_[i]) {
                if (j < i) continue;
                uint32_t base = (1u << i) | (1u << j);
                for (int src : {i, j})
                    for (int e : adj_[src]) {
                        if (e == i || e == j) continue;
                        tmp.push_back(base | (1u << e));
                    }
            }
        std::sort(tmp.begin(), tmp.end());
        tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
        moveMasks_.swap(tmp);
    }

    Move decode(uint32_t mv) {
        Move m; m.n = 0;
        for (int i = 0; i < (int)cells_.size() && m.n < 3; ++i)
            if (mv & (1u << i)) m.cells[m.n++] = cells_[i];
        return m;
    }

    // Expectimax: 自分の番は最大化、敵の番は平均（ランダム仮定）
    // 戻り値: 自分の勝率 0.0〜1.0、タイムアウト時は -1.f
    float rec(uint32_t mask, int off) {
        if (++nodes_ > node_cap_) return -1.f;
        if ((nodes_ & 0xffff) == 0) {
            double el = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_).count();
            if (el > max_ms_) return -1.f;
        }
        uint64_t key = ((uint64_t)mask << 6) | (uint32_t)off;
        auto it = memo_.find(key);
        if (it != memo_.end()) return it->second;

        bool my_turn = (off == 0);
        float result = my_turn ? 0.f : 0.f;
        float sum = 0.f; int cnt = 0;

        for (uint32_t mv : moveMasks_) {
            if ((mv & mask) != mv) continue;
            uint32_t nm = mask & ~mv;
            float child = (nm == 0) ? (my_turn ? 1.f : 0.f)
                                    : rec(nm, (off + 1) % n_);
            if (child < 0.f) return -1.f;  // タイムアウト伝播

            if (my_turn) {
                if (child > result) result = child;
                if (result == 1.f) break;  // 必勝確定、枝刈り
            } else {
                sum += child; ++cnt;        // 敵は平均を取る
            }
        }
        if (!my_turn) result = (cnt > 0) ? sum / cnt : 0.f;

        memo_[key] = result;
        return result;
    }
};

} // namespace mcts

// ============================================================
//  スレッドプール
//    StartStage() の初回呼び出し時にスレッドを起動し、
//    50ステージを通して稼働し続ける（スレッド生成コストゼロ）。
//    PlayStage() 呼び出しごとに condition_variable (~0.1ms) で
//    起こし、全スレッドが並列 MCTS → 多数決で最善手を決定する。
// ============================================================
namespace pool {

static constexpr int MAX_WORKERS = 16;
static int  g_nw = 0;
static std::thread g_threads[MAX_WORKERS];

// ジョブ情報（SetupJob で書き込み、worker で読み取り）
static mcts::Board g_board;
static double      g_budget_ms = 0.0;
static mcts::Move  g_results[MAX_WORKERS];

// 同期プリミティブ
static std::mutex               g_mtx;
static std::condition_variable  g_cv_start;  // メイン → ワーカーへの開始通知
static std::condition_variable  g_cv_done;   // ワーカー → メインへの完了通知
static int  g_generation = 0;   // ジョブ世代番号（インクリメントで起動）
static int  g_done_cnt   = 0;   // 完了スレッド数
static bool g_exit       = false;

static void worker(int id) {
    int my_gen = -1;
    for (;;) {
        mcts::Board local_board;
        double      local_budget;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            // 新ジョブまたは終了シグナルを待つ
            g_cv_start.wait(lk, [&]{
                return g_generation != my_gen || g_exit;
            });
            if (g_exit) return;
            my_gen       = g_generation;
            local_board  = g_board;      // ロック保持中にコピー（スレッドセーフ）
            local_budget = g_budget_ms;
        }
        // MCTS をロックなしで実行（各スレッドが独立した Tree を所有）
        mcts::Tree t;
        g_results[id] = t.search_timed(local_board, local_budget);

        // 完了を通知
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            if (++g_done_cnt == g_nw)
                g_cv_done.notify_one();
        }
    }
}

// StartStage() の初回呼び出し時のみスレッドを起動
static void init_once() {
    static std::once_flag flag;
    std::call_once(flag, []{
        g_nw = (int)std::thread::hardware_concurrency();
        if (g_nw < 1) g_nw = 1;
        if (g_nw > MAX_WORKERS) g_nw = MAX_WORKERS;
        for (int i = 0; i < g_nw; ++i)
            g_threads[i] = std::thread(worker, i);
        // detach: プロセス終了時に OS が自動クリーンアップ
        for (int i = 0; i < g_nw; ++i)
            g_threads[i].detach();
    });
}

// 並列 MCTS 探索（PlayStage から呼び出す）
static mcts::Move search(const mcts::Board& board, double budget_ms) {
    // スレッドが 1 本ならオーバーヘッドなしでシングル動作
    if (g_nw <= 1) {
        mcts::Tree t;
        return t.search_timed(board, budget_ms);
    }

    // ジョブをセット → 全ワーカーを起こす
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_board     = board;
        g_budget_ms = budget_ms;
        g_done_cnt  = 0;
        ++g_generation;
    }
    g_cv_start.notify_all();

    // 全ワーカーの完了を待つ
    {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv_done.wait(lk, [&]{ return g_done_cnt == g_nw; });
    }

    // 多数決：最も多くのスレッドが推薦した手を採用
    std::unordered_map<uint32_t, int>        votes;
    std::unordered_map<uint32_t, mcts::Move> mv_for;
    uint32_t best_k = 0; int best_v = -1;
    for (int i = 0; i < g_nw; ++i) {
        const mcts::Move& m = g_results[i];
        uint32_t k = ((uint32_t)(uint8_t)m.n        << 24)
                   | ((uint32_t)(uint8_t)m.cells[0] << 16)
                   | ((uint32_t)(uint8_t)m.cells[1] <<  8)
                   | ((uint32_t)(uint8_t)m.cells[2]);
        if (++votes[k] == 1) mv_for[k] = m;
        if (votes[k] > best_v) { best_v = votes[k]; best_k = k; }
    }
    return mv_for[best_k];
}

} // namespace pool

// ============================================================
//  DLL エクスポート関数
// ============================================================

// 各ステージの開始時に呼ばれる
//   - 初回のみスレッドプールを起動
//   - ステージカウンタを進める（時間配分の精度向上）
extern "C" __declspec(dllexport) void StartStage() {
    pool::init_once();
    mcts::on_stage_start();
}

// 各ステージの終了時に呼ばれる
//   スレッドはステージをまたいで稼働し続けるため何もしない
extern "C" __declspec(dllexport) void EndStage() {
}

// 各ターンに呼ばれる（メイン処理）
extern "C" __declspec(dllexport)
void PlayStage(char floor[STAGE_Y_MAX][STAGE_X_MAX],
               char*    count,
               TAKE_TAG lands[LANDS_SHARK_MAX],
               RULES_TAG rules)
{
    // 1. 有効な盤面サイズを検出（非壁セルの最大行・列から求める）
    int H = 0, W = 0;
    for (int r = 0; r < STAGE_Y_MAX && r < mcts::MAXN; ++r)
        for (int c = 0; c < STAGE_X_MAX && c < mcts::MAXN; ++c)
            if ((signed char)floor[r][c] != mcts::WALL) {
                if (r + 1 > H) H = r + 1;
                if (c + 1 > W) W = c + 1;
            }
    if (H == 0) H = (STAGE_Y_MAX < mcts::MAXN) ? STAGE_Y_MAX : mcts::MAXN;
    if (W == 0) W = (STAGE_X_MAX < mcts::MAXN) ? STAGE_X_MAX : mcts::MAXN;

    // 2. char floor[y][x] → int board[row][col] 変換
    int board[mcts::MAXN][mcts::MAXN] = {};
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            board[r][c] = (int)(signed char)floor[r][c];

    int my_id       = (int)(signed char)rules.your_number;
    int num_players = (int)(signed char)rules.number_players;

    mcts::Board b;
    b.init(board, H, W, num_players, my_id);

    mcts::Move m;
    bool decided = false;

    // 3. 終盤厳密解：空き≤ENDGAME_MAX_EMPTY なら完全探索で勝ちを強制
    if (b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {
        mcts::Endgame eg;
        mcts::EndgameResult er = eg.solve(b, my_id, mcts::ENDGAME_NODE_CAP);
        if (er.solved && er.win) { m = er.move; decided = true; }
    }

    // 4. MCTS（スレッドプール並列）
    if (!decided) {
        double budget = mcts::budget_per_move(b.empty_count, num_players);
        m = pool::search(b, budget);
    }

    // 5. 結果を出力引数に書き戻す
    *count = (char)m.n;
    for (int i = 0; i < m.n; ++i) {
        lands[i].y = m.cells[i] / W;   // row（floor の第1添字）
        lands[i].x = m.cells[i] % W;   // col（floor の第2添字）
    }
}

// DllMain: 何も特別な処理は不要
//   スレッドは detach 済みのためプロセス終了時に OS が自動クリーンアップ
bool APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)hModule; (void)ul_reason_for_call; (void)lpReserved;
    return TRUE;
}
