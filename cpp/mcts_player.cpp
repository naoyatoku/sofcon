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
#include <unordered_map>

namespace mcts {

constexpr int MAXN   = 15;
constexpr int WALL   = -1;
constexpr int EMPTY  = 0;

// 終盤厳密解ソルバーの発動条件
//   ENDGAME_MAX_EMPTY : 空きマスがこの数以下なら完全探索を試みる
//   ENDGAME_NODE_CAP  : 探索ノード上限（超えたら MCTS にフォールバック）
constexpr int    ENDGAME_MAX_EMPTY = 20;
constexpr long   ENDGAME_NODE_CAP  = 8000000;
constexpr double ENDGAME_MAX_MS    = 200.0;  // 壁時計上限。超えたらMCTSへフォールバック（失格防止）

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

// 1手あたりの時間予算をステージ検出型で計算
//   empty_count : 現在の空きマス数（Board.empty_count をそのまま渡す）
//   num_players : プレイヤー数
//
//   【アルゴリズム】
//   ・空きマスが前回より増えた = 新ステージ開始 → ステージカウントを進める
//   ・残り時間 ÷ 残りステージ数 = このステージの予算
//   ・このステージの予算 ÷ 残り自分の手数 = 1手の予算
//   ・number_takes=3 固定なので平均3マス/手で計算
//   これにより、小盤面/大盤面どちらでも残り時間を均等に使い切れる。
static double budget_per_move(int empty_count, int num_players) {
    static int s_stages_done = 0;   // 開始したステージ数（1-indexed）
    static int s_prev_empty  = -1;  // 前回呼び出し時の空きマス数

    // 空きマスが増えた → 新ステージ開始
    if (empty_count > s_prev_empty) ++s_stages_done;
    s_prev_empty = empty_count;

    double rem         = remaining_ms();
    int    stages_left = std::max(1, 50 - s_stages_done + 1);
    double stage_budget = rem / stages_left;   // このステージに割り当てる時間

    // このステージで自分があと何手打てるか（number_takes=3 固定で推算）
    double our_turns = std::max(1.0, empty_count / 3.0 / num_players);
    double budget    = stage_budget / our_turns;

    // 最低1ms、最大500ms（盤面が小さい終盤でも使いすぎない上限）
    return std::max(1.0, std::min(budget, 500.0));
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

// ------------------------------------------------------------------ //
//  終盤厳密解ソルバー（パラノイド完全探索）
//
//  将来の合法手・勝敗は「空きマスの集合」と「手番」だけで決まる
//  （誰が掘ったかは無関係）。空きマスを 0..k-1 に再ラベルしてビット
//  マスクで表し、メモ化付き完全探索で「自分が勝ちを強制できるか」を解く。
//
//  パラノイド仮定：自分以外の全員が自分の勝ちを阻止しようとする。
//   → 「勝てる」と出たら相手が誰でも本当に勝てる（健全）。
//   → 「勝てない」場合は相手のミスで勝てる可能性が残るので MCTS に委ねる。
//
//  n人すべてで正しい（2人なら通常の minimax = Grundy と一致）。
// ------------------------------------------------------------------ //

struct EndgameResult {
    bool solved = false;   // 探索を完了できたか（false=ノード上限超過→MCTSへ）
    bool win    = false;   // 自分が勝ちを強制できるか
    Move move{};           // win のときに打つべき手
};

class Endgame {
public:
    // empty_count <= max_empty のときだけ呼ぶこと
    EndgameResult solve(const Board& b, int me, long node_cap,
                        double max_ms = ENDGAME_MAX_MS) {
        n_ = b.num_players;
        me_ = me;
        node_cap_ = node_cap;
        nodes_ = 0;
        start_ = std::chrono::steady_clock::now();
        max_ms_ = max_ms;
        memo_.clear();

        // 空きマスを列挙して 0..k-1 に再ラベル
        cells_.clear();
        int idx_of[MAXN * MAXN];
        for (int i = 0; i < MAXN * MAXN; ++i) idx_of[i] = -1;
        for (int r = 0; r < b.H; ++r)
            for (int c = 0; c < b.W; ++c)
                if (b.g[r][c] == EMPTY) {
                    idx_of[r * b.W + c] = (int)cells_.size();
                    cells_.push_back(r * b.W + c);
                }
        int k = (int)cells_.size();
        W_ = b.W;
        if (k == 0 || k > 30) return {};   // 32bit マスクの安全域

        // 隣接（4連結）を構築
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

        // 連結する 1〜3 マスの手をすべてビットマスクで列挙
        buildMoves(k);

        uint32_t full = (k == 32) ? 0xffffffffu : ((1u << k) - 1u);
        int cur_off = 0;   // 根は自分の手番（off=0）

        // 根：自分の手番。勝ちを強制できる手を探す
        EndgameResult res;
        for (uint32_t mv : moveMasks_) {
            if ((mv & full) != mv) continue;               // 全セルが空きでない
            uint32_t nm = full & ~mv;
            bool win;
            if (nm == 0) win = true;                       // 最後のマスを取った→自分勝ち
            else {
                int r = rec(nm, 1 % n_);
                if (r < 0) return {};                       // ノード上限→未解決
                win = (r == 1);
            }
            if (win) { res.solved = true; res.win = true; res.move = decode(mv); return res; }
        }
        res.solved = true; res.win = false;                 // 勝ちを強制できない
        return res;
    }

private:
    int n_, me_, W_;
    long node_cap_, nodes_;
    std::chrono::steady_clock::time_point start_;
    double max_ms_;
    std::vector<int> cells_;
    std::vector<std::vector<int>> adj_;
    std::vector<uint32_t> moveMasks_;
    std::unordered_map<uint64_t, char> memo_;

    void buildMoves(int k) {
        moveMasks_.clear();
        std::vector<uint32_t> tmp;
        for (int i = 0; i < k; ++i) {
            tmp.push_back(1u << i);                          // size 1
            for (int j : adj_[i]) {
                if (j < i) continue;
                tmp.push_back((1u << i) | (1u << j));         // size 2
            }
        }
        // size 3：連結トリプル（i<j 辺に、i か j の隣接 e を足す）
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
        // 重複除去
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

    // mask の状態・手番 off で「自分(off=0基準のme)が勝ちを強制できるか」
    //   返り値: 1=勝てる, 0=勝てない, -1=ノード上限で未解決
    int rec(uint32_t mask, int off) {
        if (++nodes_ > node_cap_) return -1;
        // 65536ノードごとに壁時計をチェック（チェック自体の負荷を抑制）
        if ((nodes_ & 0xffff) == 0) {
            double el = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start_).count();
            if (el > max_ms_) return -1;
        }
        uint64_t key = ((uint64_t)mask << 6) | (uint32_t)off;
        auto it = memo_.find(key);
        if (it != memo_.end()) return it->second;

        bool my_turn = (off == 0);
        // my_turn: 1手でも勝てれば勝ち / 相手手番: 全手で勝てないと勝ちにならない
        int result = my_turn ? 0 : 1;
        bool decided = false;

        for (uint32_t mv : moveMasks_) {
            if ((mv & mask) != mv) continue;                 // 適用不可
            uint32_t nm = mask & ~mv;
            int child;
            if (nm == 0) {
                // この手番のプレイヤーが最後のマスを取った → そのプレイヤーの勝ち
                child = my_turn ? 1 : 0;
            } else {
                int r = rec(nm, (off + 1) % n_);
                if (r < 0) return -1;                        // 上限伝播
                child = r;
            }
            if (my_turn) {
                if (child == 1) { result = 1; decided = true; break; }  // 勝ち手発見
            } else {
                if (child == 0) { result = 0; decided = true; break; }  // 相手が阻止可能
            }
        }
        (void)decided;
        memo_[key] = (char)result;
        return result;
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

    mcts::Move m;
    // --- 終盤厳密解：空きが少なければ完全探索で勝ちを強制 ---
    if (b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {
        mcts::Endgame eg;
        mcts::EndgameResult er = eg.solve(b, my_id, mcts::ENDGAME_NODE_CAP);
        if (er.solved && er.win) {
            std::vector<Pos> result;
            for (int i = 0; i < er.move.n; ++i)
                result.push_back({er.move.cells[i] / width, er.move.cells[i] % width});
            return result;
        }
    }

    mcts::Tree tree;
    if (num_sims > 0) {
        m = tree.search(b, num_sims);
    } else {
        double budget = mcts::budget_per_move(b.empty_count, num_players);
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

    mcts::Move m;
    bool decided = false;
    // --- 終盤厳密解：空きが少なければ完全探索で勝ちを強制 ---
    if (b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {
        mcts::Endgame eg;
        mcts::EndgameResult er = eg.solve(b, my_id, mcts::ENDGAME_NODE_CAP);
        if (er.solved && er.win) { m = er.move; decided = true; }
    }
    if (!decided) {
        double budget = mcts::budget_per_move(b.empty_count, num_players);
        mcts::Tree tree;
        m = tree.search_timed(b, budget);
    }

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
                double budget = mcts::budget_per_move(b.empty_count, NP);
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
