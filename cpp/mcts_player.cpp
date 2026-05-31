/**
 * mcts_player.cpp  —  自己完結型の純粋MCTSプレイヤー（ベアメタル / 外部ライブラリ不要）
 *
 *  Python版 game/ + model/mcts.py と同じルール・同じ negamax MCTS を C++ で実装。
 *  ニューラルネット不要。ランダムロールアウトで葉の価値を推定する。
 *
 *  ルール:
 *    盤面 grid[r][c]:  -1=壁, 0=空き, 1以上=各プレイヤーが掘った
 *    手番: 1ターンに 1〜3 マスの「直交連結した空きマス」を掘る
 *    勝敗: 最後の空きマスを掘ったプレイヤーの勝ち（normal play）
 *
 *  提出時は choose_move() をコンテストのインターフェースに合わせて呼ぶ。
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

constexpr int MAXN = 15;
constexpr int WALL = -1;
constexpr int EMPTY = 0;

// ------------------------------------------------------------------ //
//  Board
// ------------------------------------------------------------------ //
struct Board {
    int8_t g[MAXN][MAXN];
    int H, W, num_players;
    int current_player;     // 1-indexed
    int empty_count;
    bool done;
    int winner;             // 0 = none

    void init(const int board[][MAXN], int h, int w, int np, int cur) {
        H = h; W = w; num_players = np; current_player = cur;
        empty_count = 0; done = false; winner = 0;
        for (int r = 0; r < H; ++r)
            for (int c = 0; c < W; ++c) {
                g[r][c] = (int8_t)board[r][c];
                if (g[r][c] == EMPTY) ++empty_count;
            }
    }

    // move = up to 3 (r,c) pairs, encoded as flat indices
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

// A move: 1..3 flat cell indices
struct Move {
    int cells[3];
    int n;
};

static const int DR[4] = {-1, 1, 0, 0};
static const int DC[4] = {0, 0, -1, 1};

// ------------------------------------------------------------------ //
//  Legal move generation (1-3 orthogonally connected empty cells)
// ------------------------------------------------------------------ //
static void gen_moves(const Board& b, std::vector<Move>& out) {
    out.clear();
    int H = b.H, W = b.W;
    // dedup of size-2 / size-3 via sorted-tuple set is costly; instead we
    // generate canonically: enforce ordering to avoid duplicates.
    for (int r = 0; r < H; ++r) {
        for (int c = 0; c < W; ++c) {
            if (b.g[r][c] != EMPTY) continue;
            int a = r * W + c;
            // size 1
            out.push_back({{a, 0, 0}, 1});
            for (int d = 0; d < 4; ++d) {
                int r2 = r + DR[d], c2 = c + DC[d];
                if (r2 < 0 || r2 >= H || c2 < 0 || c2 >= W) continue;
                if (b.g[r2][c2] != EMPTY) continue;
                int bb = r2 * W + c2;
                if (bb < a) continue;            // canonical: a is the min → no dup pairs
                out.push_back({{a, bb, 0}, 2});
                // size 3: any empty neighbor of {a,bb}, kept > a to dedup
                for (int src = 0; src < 2; ++src) {
                    int rr = (src == 0 ? r : r2), cc = (src == 0 ? c : c2);
                    for (int e = 0; e < 4; ++e) {
                        int r3 = rr + DR[e], c3 = cc + DC[e];
                        if (r3 < 0 || r3 >= H || c3 < 0 || c3 >= W) continue;
                        if (b.g[r3][c3] != EMPTY) continue;
                        int cc3 = r3 * W + c3;
                        if (cc3 == a || cc3 == bb) continue;
                        if (cc3 < a) continue;    // keep a as the minimum
                        // avoid duplicate triples: require cc3 > bb OR (cc3 between)
                        // simplest robust dedup: require the third > the second's "other"
                        // We accept (a < bb) already; require cc3 != and ordering a<bb,
                        // and to dedup the two generation paths require cc3 > a only,
                        // then canonical-sort + skip if same set seen.
                        // Use a cheap local sort+compare against existing size-3 of same a.
                        int s0 = a, s1 = bb, s2 = cc3;
                        if (s1 > s2) std::swap(s1, s2);
                        if (s0 > s1) std::swap(s0, s1);
                        if (s1 > s2) std::swap(s1, s2);
                        // emit; duplicates filtered below
                        out.push_back({{s0, s1, s2}, 3});
                    }
                }
            }
        }
    }
    // dedup size-3 (and any incidental dups) by sorting
    // moves are small; do an O(n log n) unique pass on a key
    std::sort(out.begin(), out.end(), [](const Move& x, const Move& y) {
        if (x.n != y.n) return x.n < y.n;
        if (x.cells[0] != y.cells[0]) return x.cells[0] < y.cells[0];
        if (x.cells[1] != y.cells[1]) return x.cells[1] < y.cells[1];
        return x.cells[2] < y.cells[2];
    });
    out.erase(std::unique(out.begin(), out.end(), [](const Move& x, const Move& y) {
        return x.n == y.n && x.cells[0] == y.cells[0] &&
               x.cells[1] == y.cells[1] && x.cells[2] == y.cells[2];
    }), out.end());
}

// ------------------------------------------------------------------ //
//  MCTS (negamax, random rollout)
// ------------------------------------------------------------------ //
static std::mt19937 rng(
    (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count());

struct Node {
    Board board;
    Move move;              // move that led here
    int parent;             // index, -1 for root
    std::vector<int> children;
    int visit = 0;
    double value_sum = 0.0; // from this node's player-to-move perspective
    bool expanded = false;
};

class Tree {
public:
    std::vector<Node> nodes;

    int alloc(const Board& b, const Move& m, int parent) {
        nodes.push_back(Node());
        Node& n = nodes.back();
        n.board = b; n.move = m; n.parent = parent;
        return (int)nodes.size() - 1;
    }

    // Random playout → return the WINNER's player id (0 if none).
    // N-player correct: we distribute the win to each node's mover in backprop.
    int rollout(Board b) {
        std::vector<Move> mv;
        while (!b.done) {
            gen_moves(b, mv);
            if (mv.empty()) return 0;
            const Move& m = mv[rng() % mv.size()];
            b.apply(m.cells, m.n);
        }
        return b.winner;
    }

    int expand(int idx) {
        if (nodes[idx].board.done) { nodes[idx].expanded = true; return idx; }
        Board parent_board = nodes[idx].board;   // copy out: alloc() may realloc nodes[]
        std::vector<Move> mv;
        gen_moves(parent_board, mv);
        for (const Move& m : mv) {
            Board nb = parent_board;
            nb.apply(m.cells, m.n);
            int ci = alloc(nb, m, idx);          // may invalidate references into nodes[]
            nodes[idx].children.push_back(ci);   // re-index by idx each time → safe
        }
        nodes[idx].expanded = true;
        return idx;
    }

    int select(int idx) {
        while (nodes[idx].expanded && !nodes[idx].children.empty()
               && !nodes[idx].board.done) {
            const Node& n = nodes[idx];
            double best = -1e18; int best_c = n.children[0];
            double logN = std::log((double)n.visit + 1.0);
            for (int ci : n.children) {
                const Node& c = nodes[ci];
                // c.value_sum is already from the perspective of the player who
                // moved INTO c (= n's player-to-move). No sign flip (N-player correct).
                double q = (c.visit > 0) ? (c.value_sum / c.visit) : 0.0;
                double u = 1.41421356 * std::sqrt(logN / (c.visit + 1.0));
                double s = q + u;
                if (s > best) { best = s; best_c = ci; }
            }
            idx = best_c;
        }
        return idx;
    }

    // Give +1 to each node whose mover (= parent's player-to-move) == winner.
    // Works for any number of players.
    void backprop(int idx, int winner) {
        while (idx != -1) {
            nodes[idx].visit += 1;
            int parent = nodes[idx].parent;
            if (parent != -1) {
                int mover = nodes[parent].board.current_player;
                nodes[idx].value_sum += (winner == mover) ? 1.0 : 0.0;
            }
            idx = parent;
        }
    }

    Move search(const Board& root_board, int num_sims) {
        nodes.clear();
        nodes.reserve(num_sims * 4 + 64);
        int root = alloc(root_board, Move{{0,0,0},0}, -1);
        expand(root);

        for (int s = 0; s < num_sims; ++s) {
            int leaf = select(root);
            int winner;
            if (nodes[leaf].board.done) {
                winner = nodes[leaf].board.winner;
            } else if (!nodes[leaf].expanded) {
                expand(leaf);
                winner = rollout(nodes[leaf].board);
            } else {
                // fully-expanded non-terminal leaf: do a rollout from here
                winner = rollout(nodes[leaf].board);
            }
            backprop(leaf, winner);
        }

        // most-visited child
        int best = nodes[root].children[0];
        int best_v = -1;
        for (int ci : nodes[root].children) {
            if (nodes[ci].visit > best_v) { best_v = nodes[ci].visit; best = ci; }
        }
        return nodes[best].move;
    }
};

} // namespace mcts

// ====================================================================== //
//  提出用インターフェース（コンテスト仕様に合わせて要調整）
// ====================================================================== //
struct Pos { int row, col; };

/**
 * board       : board[r][c]  (-1=壁, 0=空き, 1以上=各プレイヤー)
 * height,width: 盤面サイズ (<=15)
 * my_id       : 自分のプレイヤーID (1-indexed)
 * num_players : 総プレイヤー数
 * num_sims    : MCTSシミュレーション回数（多いほど強い・遅い）
 * 戻り値      : 掘るマスのリスト(1-3個)
 */
std::vector<Pos> choose_move(
    const int board[][mcts::MAXN], int height, int width,
    int my_id, int num_players, int num_sims = 800)
{
    mcts::Board b;
    b.init(board, height, width, num_players, my_id);

    mcts::Tree tree;
    mcts::Move m = tree.search(b, num_sims);

    std::vector<Pos> result;
    for (int i = 0; i < m.n; ++i)
        result.push_back({m.cells[i] / width, m.cells[i] % width});
    return result;
}

// ---- セルフテスト用 main (提出時は削除可) ----
#ifdef MCTS_SELFTEST
#include <cstdio>
// 1人のMCTSプレイヤー vs 残り全員ランダム を人数別に検証。
// N人なら均等時の基準勝率は 1/N。これを明確に上回れば正しく強い。
static void run_suite(int NP, int H, int W, int GAMES, int SIMS, std::mt19937& r) {
    int mcts_wins = 0;
    for (int game = 0; game < GAMES; ++game) {
        int board[mcts::MAXN][mcts::MAXN] = {};
        for (int i = 0; i < H * W / 10; ++i)       // 壁 約10%
            board[r() % H][r() % W] = -1;

        int mcts_id = (game % NP) + 1;             // 席を回して公平に
        mcts::Board b;
        b.init(board, H, W, NP, 1);
        std::vector<mcts::Move> mv;
        while (!b.done) {
            mcts::Move chosen;
            if (b.current_player == mcts_id) {
                mcts::Tree t;
                chosen = t.search(b, SIMS);
            } else {
                mcts::gen_moves(b, mv);
                chosen = mv[r() % mv.size()];
            }
            b.apply(chosen.cells, chosen.n);
        }
        if (b.winner == mcts_id) ++mcts_wins;
    }
    double wr = 100.0 * mcts_wins / GAMES, base = 100.0 / NP;
    printf("  %dP: MCTS winrate %5.1f%%  (baseline %.1f%%)  %s\n",
           NP, wr, base, wr > base + 10 ? "[OK]" : (wr > base ? "[weak]" : "[BAD]"));
}

int main() {
    std::mt19937 r(12345);
    const int H = 9, W = 9, GAMES = 30, SIMS = 400;
    printf("MCTS vs random (1 MCTS player, rest random), %dx%d, %d games, sims=%d\n",
           H, W, GAMES, SIMS);
    for (int NP : {2, 3, 4, 6})
        run_suite(NP, H, W, GAMES, SIMS, r);
    return 0;
}
#endif
