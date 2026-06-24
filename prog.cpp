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
#include <unordered_set>
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
    int H, W, num_players, current_player, empty_count, number_takes;
    bool done;
    int winner;

    void init(const int board[][MAXN], int h, int w, int np, int cur, int nt = 3) {
        H = h; W = w; num_players = np; current_player = cur; number_takes = nt;
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

struct Move { int cells[10]; int n; };  // 最大 LANDS_SHARK_MAX(10) マス

static const int DR[4] = {-1,  1,  0, 0};
static const int DC[4] = { 0,  0, -1, 1};

// thread_local RNG: スレッドごとに異なるシードで探索パスを分散させる
static thread_local std::mt19937 rng{
    (uint32_t)(std::chrono::steady_clock::now().time_since_epoch().count()
               ^ (std::hash<std::thread::id>{}(std::this_thread::get_id()) * 2654435761u))
};

// ----------------------------------------------------------------
//  合法手生成（number_takes まで対応）
//  DFS で連結サブセットを列挙。anchor（最小インデックス）が正準形。
// ----------------------------------------------------------------
// 1アンカーあたりの生成上限。
//   序盤（空きマスが多い）× 大きい number_takes での組合せ爆発を防ぐ。
//   空きマスが減れば 1 アンカーの部分木は自然に小さくなり、実質全列挙になる。
//   k≤5（見えているステージ最大）は通常この上限に達せず完全列挙される。
static const int GEN_PER_ANCHOR_CAP = 256;

static void gen_moves_dfs(const Board& b, Move& cur, int anchor,
                          std::vector<Move>& out, int max_n, int& budget) {
    if (budget <= 0) return;
    out.push_back(cur);
    --budget;                       // size-1 手は必ず生成される（budget≥1）
    if (cur.n >= max_n) return;
    for (int si = 0; si < cur.n; ++si) {
        if (budget <= 0) return;
        int r0 = cur.cells[si] / b.W, c0 = cur.cells[si] % b.W;
        for (int d = 0; d < 4; ++d) {
            int r2 = r0 + DR[d], c2 = c0 + DC[d];
            if (r2<0||r2>=b.H||c2<0||c2>=b.W) continue;
            if (b.g[r2][c2] != EMPTY) continue;
            int nb = r2*b.W+c2;
            if (nb <= anchor) continue;  // anchor が最小になる正準形
            bool dup = false;
            for (int i=0; i<cur.n; ++i) if (cur.cells[i]==nb) {dup=true; break;}
            if (dup) continue;
            cur.cells[cur.n++] = nb;
            gen_moves_dfs(b, cur, anchor, out, max_n, budget);
            cur.n--;
            if (budget <= 0) return;
        }
    }
}

static void gen_moves(const Board& b, std::vector<Move>& out) {
    out.clear();
    int k = (b.number_takes > 0) ? std::min(b.number_takes, 10) : 3;
    for (int r = 0; r < b.H; ++r)
        for (int c = 0; c < b.W; ++c) {
            if (b.g[r][c] != EMPTY) continue;
            Move m{}; m.n=1; m.cells[0]=r*b.W+c;
            int budget = GEN_PER_ANCHOR_CAP;
            gen_moves_dfs(b, m, m.cells[0], out, k, budget);
        }
    // 各手のセルをソートして正準化
    for (Move& m : out)
        std::sort(m.cells, m.cells+m.n);
    // 重複除去
    std::sort(out.begin(), out.end(), [](const Move& a, const Move& b){
        if (a.n!=b.n) return a.n<b.n;
        for (int i=0;i<a.n;i++) if (a.cells[i]!=b.cells[i]) return a.cells[i]<b.cells[i];
        return false;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const Move& a, const Move& b){
        if (a.n!=b.n) return false;
        for (int i=0;i<a.n;i++) if (a.cells[i]!=b.cells[i]) return false;
        return true;
    }), out.end());
}

// ----------------------------------------------------------------
//  選んだ手を number_takes まで貪欲に拡張する
// ----------------------------------------------------------------
static Move extend_move(const Board& b, Move m, int max_n) {
    max_n = std::min(max_n, 10);
    while (m.n < max_n) {
        bool extended = false;
        for (int si = 0; si < m.n && !extended; ++si) {
            int r0 = m.cells[si] / b.W, c0 = m.cells[si] % b.W;
            for (int d = 0; d < 4 && !extended; ++d) {
                int r2 = r0 + DR[d], c2 = c0 + DC[d];
                if (r2 < 0 || r2 >= b.H || c2 < 0 || c2 >= b.W) continue;
                if (b.g[r2][c2] != EMPTY) continue;
                int nb = r2 * b.W + c2;
                bool dup = false;
                for (int i = 0; i < m.n; ++i) if (m.cells[i] == nb) { dup = true; break; }
                if (!dup) { m.cells[m.n++] = nb; extended = true; }
            }
        }
        if (!extended) break;
    }
    return m;
}

// ----------------------------------------------------------------
//  ちょうど size マスの連結手を直接構築する（パリティ直接手用）
//    gen_moves の全列挙に頼らないので、大きい number_takes でも高速・安全。
//    各空きマスを起点に貪欲拡張し、最初に size に到達したものを返す。
//    どこからも size に届かなければ n=0 を返す。
// ----------------------------------------------------------------
static Move build_exact_move(const Board& b, int size) {
    size = std::min(size, 10);
    if (size <= 0) return Move{{0}, 0};
    for (int r = 0; r < b.H; ++r)
        for (int c = 0; c < b.W; ++c) {
            if (b.g[r][c] != EMPTY) continue;
            Move m{}; m.n = 1; m.cells[0] = r * b.W + c;
            m = extend_move(b, m, size);
            if (m.n == size) return m;
        }
    return Move{{0}, 0};
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

    int target = (b.number_takes > 0) ? std::min(b.number_takes, 10) : 3;
    for (int ext = 0; ext < target - 1; ++ext) {
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
//  決定論的対戦相手シミュレーション
//  Player 2: 左上から縦方向に 1 マス
//  Player 3: 右下から横方向に 1 マス
//  Player 4: 横方向で最大連結グループ（左上優先）
//  Player 5: 縦方向で最大連結グループ（左上優先）
// ----------------------------------------------------------------
static void bot_p2(Board& b) {
    for (int c = 0; c < b.W; ++c)
        for (int r = 0; r < b.H; ++r)
            if (b.g[r][c] == EMPTY) {
                int cell = r * b.W + c;
                b.apply(&cell, 1); return;
            }
}

// P3: 右下から左上へ走査（P2の鏡＝列優先・右の列から、列内は下から）
static void bot_p3(Board& b) {
    for (int c = b.W - 1; c >= 0; --c)
        for (int r = b.H - 1; r >= 0; --r)
            if (b.g[r][c] == EMPTY) {
                int cell = r * b.W + c;
                b.apply(&cell, 1); return;
            }
}

// P4: 横方向の最大ラン。優先順: 取得数大 → 開始列が左 → 行が上
static void bot_p4(Board& b) {
    int k = b.number_takes;
    int best = 0, bestCol = 0, bestRow = 0, br = -1, bc = -1;
    for (int r = 0; r < b.H; ++r) {
        int c = 0;
        while (c < b.W) {
            if (b.g[r][c] != EMPTY) { ++c; continue; }
            int s = c;
            while (c < b.W && b.g[r][c] == EMPTY) ++c;
            int take = std::min(c - s, k);   // 行rのラン [s, c)
            bool better = (br < 0) || take > best
                || (take == best && s < bestCol)
                || (take == best && s == bestCol && r < bestRow);
            if (better) { best = take; bestCol = s; bestRow = r; br = r; bc = s; }
        }
    }
    if (br >= 0 && best > 0) {
        int cells[10];
        for (int i = 0; i < best; ++i) cells[i] = br * b.W + (bc + i);
        b.apply(cells, best);
    }
}

// P5: 縦方向の最大ラン。優先順: 取得数大 → 行が上 → 列が左
static void bot_p5(Board& b) {
    int k = b.number_takes;
    int best = 0, bestRow = 0, bestCol = 0, br = -1, bc = -1;
    for (int c = 0; c < b.W; ++c) {
        int r = 0;
        while (r < b.H) {
            if (b.g[r][c] != EMPTY) { ++r; continue; }
            int s = r;
            while (r < b.H && b.g[r][c] == EMPTY) ++r;
            int take = std::min(r - s, k);   // 列cのラン [s, r)
            bool better = (br < 0) || take > best
                || (take == best && s < bestRow)
                || (take == best && s == bestRow && c < bestCol);
            if (better) { best = take; bestRow = s; bestCol = c; br = s; bc = c; }
        }
    }
    if (br >= 0 && best > 0) {
        int cells[10];
        for (int i = 0; i < best; ++i) cells[i] = (br + i) * b.W + bc;
        b.apply(cells, best);
    }
}

static void apply_bot(Board& b) {
    switch (b.current_player) {
        case 2: bot_p2(b); break;
        case 3: bot_p3(b); break;
        case 4: bot_p4(b); break;
        case 5: bot_p5(b); break;
        default: { Move m = fast_random_move(b); b.apply(m.cells, m.n); }
    }
}

// 自分の次の番まで bot を確定的にシミュレーション
static void sim_bots(Board& b) {
    while (!b.done && b.current_player != 1) apply_bot(b);
}

// 1ラウンド確定的先読み
// 即勝ち or 勝利パリティになる手を返す。なければ n=0
static Move det_lookahead(const Board& b) {
    std::vector<Move> moves;
    gen_moves(b, moves);
    if (moves.empty()) return Move{{0,0,0}, 0};

    Move best{}; int best_sc = INT_MIN;
    int round_size = b.number_takes * b.num_players;

    for (const Move& mv : moves) {
        Board sim = b;
        sim.apply(mv.cells, mv.n);
        sim_bots(sim);

        int sc;
        if (sim.done) {
            sc = (sim.winner == 1) ? 100 : -100;
        } else {
            int rem = (round_size > 0) ? sim.empty_count % round_size : 0;
            sc = (rem >= 1 && rem <= sim.number_takes) ? 10 : -10;
        }
        if (sc > best_sc) { best_sc = sc; best = mv; }
        if (best_sc == 100) break;
    }
    // 勝利パリティ未満ならMCTSに任せる
    if (best_sc < 10) return Move{{0,0,0}, 0};
    return best;
}

// ----------------------------------------------------------------
//  AI戦専用 終盤完全読みソルバー（敵=決定論bot）
//    敵(P2〜P5)が確定的に動くので、枝分かれは自分(P1)の手だけ。
//    勝てる手順が存在すれば、その最初の手を out_first に返す。
//    Expectimax(敵=ランダム仮定)と違い、実際のbot相手に厳密。
//    b は「自分(P1)の手番」の盤面である前提。
// ----------------------------------------------------------------
static bool det_solve(Board b, Move& out_first,
                      std::chrono::steady_clock::time_point deadline) {
    std::vector<Move> moves;
    gen_moves(b, moves);
    if (moves.empty()) return false;

    int round_size = b.number_takes * b.num_players;

    // 移動順: 即勝ち→勝利パリティ→その他 の順で試すと早く解に到達する
    std::vector<std::pair<int,int>> ord;  // (score, move index)
    ord.reserve(moves.size());
    for (int i = 0; i < (int)moves.size(); ++i) {
        Board sim = b;
        sim.apply(moves[i].cells, moves[i].n);
        int sc;
        if (sim.done) {
            sc = (sim.winner == 1) ? 100 : -100;
        } else {
            sim_bots(sim);  // 敵を全員進めて自分の手番へ
            if (sim.done) sc = (sim.winner == 1) ? 100 : -100;
            else {
                int rem = (round_size > 0) ? sim.empty_count % round_size : 0;
                sc = (rem >= 1 && rem <= sim.number_takes) ? 10 : -10;
            }
        }
        ord.push_back({sc, i});
    }
    std::sort(ord.begin(), ord.end(),
              [](const std::pair<int,int>& a, const std::pair<int,int>& b_) {
                  return a.first > b_.first;
              });

    for (const auto& pr : ord) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        const Move& mv = moves[pr.second];
        Board sim = b;
        sim.apply(mv.cells, mv.n);
        if (sim.done) {
            if (sim.winner == 1) { out_first = mv; return true; }
            continue;
        }
        sim_bots(sim);  // 敵を全員進めて自分の手番へ
        if (sim.done) {
            if (sim.winner == 1) { out_first = mv; return true; }
            continue;
        }
        Move dummy{};
        if (det_solve(sim, dummy, deadline)) { out_first = mv; return true; }
    }
    return false;
}

// 確定的ロールアウト: 自分はランダム、相手は bot 確定
static int det_rollout(Board b) {
    while (!b.done) {
        if (b.current_player == 1) {
            Move m = fast_random_move(b); b.apply(m.cells, m.n);
        } else {
            apply_bot(b);
        }
    }
    return b.winner;
}

// ----------------------------------------------------------------
//  パリティ評価
//    全員が number_takes マスずつ取ると仮定したとき、
//    現在のプレイヤーが最後のマスを取れるかを判定する。
//    勝利条件: empty % (number_takes * num_players) == number_takes
// ----------------------------------------------------------------
static int parity_winner(const Board& b) {
    if (b.number_takes <= 0) return 0;
    int round_size = b.number_takes * b.num_players;
    int rem = b.empty_count % round_size;
    if (rem >= 1 && rem <= b.number_takes) return b.current_player;
    return 0;  // 0 = 不明（勝ち保証なし）
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
                // パリティで評価できればロールアウト不要、
                // それ以外は確定的ロールアウト（bot の行動を再現）
                winner = parity_winner(nodes[leaf].board);
                if (winner == 0) winner = det_rollout(nodes[leaf].board);
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
        buildMoves(k, std::min(b.number_takes > 0 ? b.number_takes : 3, 10));

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
        // 1手も評価できなかった場合のみ諦める
        if (best_prob < 0.f) return {};
        res.solved   = true;
        res.win_prob = best_prob;
        // 部分評価でも MCTSより良い情報なので常に採用
        // （タイムアウトした場合は不完全だが、それでも MCTS よりまし）
        res.win = true;
        return res;
    }

private:
    int n_, me_, W_; long node_cap_, nodes_;
    std::chrono::steady_clock::time_point start_; double max_ms_;
    std::vector<int> cells_; std::vector<std::vector<int>> adj_;
    std::vector<uint32_t> moveMasks_; std::unordered_map<uint64_t, float> memo_;

    // DFS で連結サブセットのビットマスクを列挙
    //   防御的に上限（BUILD_MOVES_CAP）を設け、超えたら打ち切る。
    //   solve() 自体は empty≤20 で呼ばれるが、k が大きいと部分集合が増えるため。
    static const int BUILD_MOVES_CAP = 300000;
    void buildMoves_dfs(uint32_t mask, int n_empty, int n_takes,
                        std::unordered_set<uint32_t>& seen) {
        if ((int)moveMasks_.size() >= BUILD_MOVES_CAP) return;
        if (!seen.insert(mask).second) return;  // 重複スキップ
        moveMasks_.push_back(mask);
        if (__builtin_popcount(mask) >= n_takes) return;
        for (int i = 0; i < n_empty; ++i) {
            if (!(mask & (1u << i))) continue;
            for (int j : adj_[i]) {
                if (!(mask & (1u << j)))
                    buildMoves_dfs(mask | (1u << j), n_empty, n_takes, seen);
            }
        }
    }

    void buildMoves(int k, int n_takes) {
        moveMasks_.clear();
        std::unordered_set<uint32_t> seen;
        for (int i = 0; i < k; ++i)
            buildMoves_dfs(1u << i, k, n_takes, seen);
        std::sort(moveMasks_.begin(), moveMasks_.end());
    }

    Move decode(uint32_t mv) {
        Move m; m.n = 0;
        for (int i = 0; i < (int)cells_.size() && m.n < 10; ++i)
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

}; // namespace mcts

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

}; // namespace pool

// ============================================================
//  対戦モード検出
//    P2・P3 は AI 戦では必ず 1 マスしか取らない。
//    前ターンとの差分を見て 2 マス以上取っていれば対人戦と判定。
// ============================================================
namespace mode {
    static bool have_prev     = false;
    static bool is_human      = false;  // true = 対人戦
    static char prev[STAGE_Y_MAX][STAGE_X_MAX];

    static void reset() {
        have_prev = false;
        is_human  = false;
    }

    static void update(const char floor[][STAGE_X_MAX], int H, int W) {
        if (!is_human && have_prev) {
            int p2 = 0, p3 = 0;
            for (int r = 0; r < H; ++r)
                for (int c = 0; c < W; ++c) {
                    int cur = (signed char)floor[r][c];
                    int prv = (signed char)prev[r][c];
                    if (cur == 2 && prv != 2) ++p2;
                    if (cur == 3 && prv != 3) ++p3;
                }
            // AI戦の P2・P3 は 1 マス固定 → 2 マス以上なら対人戦
            if (p2 > 1 || p3 > 1) is_human = true;
        }
        for (int r = 0; r < H; ++r)
            for (int c = 0; c < W; ++c)
                prev[r][c] = floor[r][c];
        have_prev = true;
    }
}; // namespace mode

// ============================================================
//  DLL エクスポート関数
// ============================================================

// 各ステージの開始時に呼ばれる
extern "C" __declspec(dllexport) void StartStage() {
    pool::init_once();
    mcts::on_stage_start();
    mode::reset();  // ステージ開始時に対戦モード検出をリセット
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

    // 2. 対戦モード検出 & char floor → int board 変換
    mode::update(floor, H, W);

    int board[mcts::MAXN][mcts::MAXN] = {};
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            board[r][c] = (int)(signed char)floor[r][c];

    int my_id        = (int)(signed char)rules.your_number;
    int num_players  = (int)(signed char)rules.number_players;
    int number_takes = (int)(unsigned char)rules.number_takes;

    mcts::Board b;
    b.init(board, H, W, num_players, my_id, number_takes);

    mcts::Move m;
    bool decided = false;

    // 優先順位:
    //  0. AI戦終盤完全読み（敵=決定論bot、厳密。最優先）
    //  1. Expectimax（終盤の確率的最善手 - 敵=ランダム仮定）
    //  2. パリティ直接手（今すぐ勝てる）
    //  3. det_lookahead（AI戦: 1ラウンド先読みで勝利パリティを狙う）
    //  4. MCTS（汎用フォールバック）

    // 0. AI戦終盤完全読み：敵が決定論的botなので厳密に解ける。
    //    Expectimax(敵=ランダム仮定)より正確で「solvedなのに負け」を防ぐ。
    //    my_id==1（当日戦固定）かつAI戦モードのときのみ。
    if (!decided && !mode::is_human && my_id == 1
        && b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(150);
        mcts::Move dm{};
        if (mcts::det_solve(b, dm, deadline) && dm.n > 0) {
            m = dm; decided = true;
        }
    }

    // 1. 終盤Expectimax：空き≤ENDGAME_MAX_EMPTY なら確率的最善手
    if (!decided && b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {
        mcts::Endgame eg;
        mcts::EndgameResult er = eg.solve(b, my_id, mcts::ENDGAME_NODE_CAP);
        if (er.solved) { m = er.move; decided = true; }
    }

    // 2. パリティ直接手：empty % (k*n) == k なら即勝ち
    //    gen_moves の全列挙に頼らず目標サイズの手を直接構築（大きいkでも安全）
    if (!decided && number_takes > 0) {
        int round_size  = number_takes * num_players;
        int target_take = b.empty_count % round_size;
        if (target_take >= 1 && target_take <= number_takes) {
            mcts::Move mv = mcts::build_exact_move(b, target_take);
            if (mv.n == target_take) { m = mv; decided = true; }
        }
    }

    // 3. 確定的深さ優先探索（AI戦のみ）
    //    相手が決定論的 → 自分の手だけ枝分かれ → 勝ち手順を探索
    //    時間予算の半分を使い、見つかれば採用
    if (!decided && !mode::is_human) {
        mcts::Move dm = mcts::det_lookahead(b);
        if (dm.n > 0) { m = dm; decided = true; }
    }

    // 4. MCTS（スレッドプール並列、確定的ロールアウト使用）
    if (!decided) {
        double budget = mcts::budget_per_move(b.empty_count, num_players);
        m = pool::search(b, budget);
    }

    // 5. 結果を出力引数に書き戻す
    *count = (char)m.n;
    for (int i = 0; i < m.n; ++i) {
        lands[i].y = m.cells[i] / W;
        lands[i].x = m.cells[i] % W;
    }
}

// DllMain: 何も特別な処理は不要
//   スレッドは detach 済みのためプロセス終了時に OS が自動クリーンアップ
bool APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)hModule; (void)ul_reason_for_call; (void)lpReserved;
    return TRUE;
}
