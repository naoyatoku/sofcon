/**
 * mcts_player.cpp  —  自己完結型の純粋MCTSプレイヤー（ベアメタル / 外部ライブラリ不要）
 *
 *  【時間管理】
 *    全50ステージ通算30秒以内の制約に対応。（TOTAL_STAGES で変更可）
 *    プロセス起動からの経過時間をグローバルに管理し、
 *    残り時間÷残り推定手数 で 1手あたりの予算を自動計算する。
 *
 *  ルール:
 *    board[r][c]:  -1=壁, 0=空き, 1以上=各プレイヤーが掘った
 *    1ターン 1〜3 マスの「直交連結した空きマス」を掘る
 *    勝敗: 最後の空きマスを掘ったプレイヤーの勝ち（normal play）
 *
 *  コンパイル: g++ -O2 -std=c++14 mcts_player.cpp -o mcts_player
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
static constexpr int    TOTAL_STAGES    = 50;       // コンテストのステージ総数
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
    int    stages_left = std::max(1, TOTAL_STAGES - s_stages_done + 1);
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

    //=============================================================================
    //toku 盤面をプレイヤーの掘ったマークにする。
    //  ここで全部が埋まると、winnerにプレイヤー番号が入る。それまでは0、プレイヤーは1originか
    //=============================================================================
    void apply(const int* cells, int n) {
        int pid = current_player;
        for (int i = 0; i < n; ++i) {                                   //
            int r = cells[i] / W, c = cells[i] % W;
            g[r][c] = (int8_t)pid;
        }
        empty_count -= n;

        if (empty_count == 0) { done = true; winner = pid; }            //終了ならば勝者決定(done=true)
        else current_player = current_player % num_players + 1;         //そうでないならばプレイヤーを進める。
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
    Board board;                                                                //盤面OBJ
    Move  move;                                                                 //
    int   parent;
    std::vector<int> children;
    int    visit     = 0;
    double value_sum = 0.0;  // このノードを指した側（parent の current_player）視点
    bool   expanded  = false;
};

class Tree {
public:
    std::vector<Node> nodes;

    //==================================================================================================
    //  alloc() :   一つノードを追加する。この関数では、moveも登録するが、実際の盤面に反映はしない。
    //==================================================================================================
    int alloc(const Board& b, const Move& m, int parent) {
        nodes.push_back(Node{});                            //新ノード（デフォルトコンストラクタ）を追加。
        Node& n  = nodes.back();                            //nは最終ノード。
        n.board  = b; n.move = m; n.parent = parent;        //最終ノードに、盤面obj、move({0,0,0},0),parent(親ノード:idxかな)
                                                                //                        ^^^^^^:場所？(～3個まで)+n個数
        return (int)nodes.size() - 1;                       //自分のインデックスを返す。
    }

    //============================================================================================
    // 高速ロールアウト：ランダムに手を選び続けて、最後までやり、勝者を決める。
    //============================================================================================
    int rollout(Board b) {
        while (!b.done) {                           
            Move m = fast_random_move(b);           //これは、ランダムの一手です。
            b.apply(m.cells, m.n);                  //あてはめます。プレイヤー順は、Board.apply()で進むので繰り返すと勝者が最後にとる。
        }                                           //終了したら、→終わるまでやる。
        return b.winner;                            //勝者を返す。
    }

    //===========================================================================================================
    //  expand()    :   idxのノードについて、とりうる手すべてを行う子ノードを追加する。
    //                  各子ノードでは、すでにその行動をとったあとの状態の盤面objを登録する。（勝敗が決まる場合がある。）
    //===========================================================================================================
    void expand(int idx) {
        if (nodes[idx].board.done) { nodes[idx].expanded = true; return; }              //盤面がすでに終了しているならばexpanded=trueにして終わり。
        Board pb = nodes[idx].board;  // push_back で nodes[] が再確保される前にコピー    //pbに盤面をコピー。
        std::vector<Move> mv;                               //  
        gen_moves(pb, mv);                                  //とりうる手を全部列挙する。
        for (const Move& m : mv) {                          //各手について
            Board nb = pb;                                  //nb : 盤面コピー
            nb.apply(m.cells, m.n);                         //nbに手を適用する。
            int ci = alloc(nb, m, idx);                     //親ノード(idx)に対して、とりうる手を行う
            nodes[idx].children.push_back(ci);             //自分の子ノードインデックスを登録する。:childrenは、vector<int>.
        }
        nodes[idx].expanded = true;                         //このノードの展開は終わり。
    }


    //=============================================================================
    //  select()    :   子供ノードから、一番価値の高いものを選択する。
    //=============================================================================
    int select(int idx) {
        while (nodes[idx].expanded &&                   //      expand=true:子ノードの追加が終わっている。
               !nodes[idx].children.empty() &&          // &    子供ノード（この盤面でとれる手）存在する。
               !nodes[idx].board.done)                  // &    この盤面でゲーム終了していない。
        {
            const Node& n   = nodes[idx];                           //(const) n: 本idxのノードです。
            double logN     = std::log((double)n.visit + 1.0);      //logN(ln) : visit+1の対数。-> log e (visit+1) : e^n = visit+1となる n
                                                                    //(e^1=e) ln e = 1
                                                                    //  → visitが指数関数的に増えても、lnは、 e^nのn部分なのでゆっくり増える。
                                                                    //  visit = e^1(2.7) -> 1 , e^3(2.7^3=20.0855) -> 3 , e^10(2.7^10=59049) -> 10
            double best     = -1e18;                                //bestが、めちゃ小さい値。
            int    best_c   = n.children[0];                       //best_c:0番目の子供
            //========================================================================
            //  全子供について、一番価値の高い子供のidx(best_c)を見つける。
            //========================================================================
            for (int ci : n.children) {                                         //全子供
                const Node& c = nodes[ci];                                      //c:子供ノード
                double q = (c.visit > 0) ? (c.value_sum / c.visit) : 0.0;       //q:value_sim/visit もしくは 0 => ivisit当たりの価値。
                                                                                //「勝率」：value_simは、勝った回数、visitは、試行回数らしい。
                                                                                //      →   試行した回数ののうち、勝った回数の割りあい。
                                                                                //      →   「確かな実績」

                double u = 1.41421356 * std::sqrt(logN / (c.visit + 1.0));      //u:√2 * √(ln visit+1)/visit) : visitが多いほど、すごく小さくなる値。
                                                                                //「探索ボーナス」：visitが少ないと、「試す価値あり」と判定する。

                if (q + u > best) { best = q + u; best_c = ci; }                //q + u が全体の価値。
                                                                                //qが小さいうちは、まだ探索していない道を探すuが優位になり、新しい試行をしようとして、
                                                                                //qが大きくなっていくと、実績のある手を選択するようになる。・・すごい。面白い。
            }
            //========================================================================
            //  ここまで来たら、子供の中で一番価値の高いものがbest_cに入っている。
            //========================================================================
            idx = best_c;
        }
        //toku 子供がいない、展開していない、もしくはゲーム終了しているノードが選択されたら、idxを返す。
        //      つまり、これ以上選択できないノードが選択されたら、そこをleafとして返す。
        return idx;
    }

    //=============================================================================
    // N人対応バックプロパゲーション:
    //  勝者の手番を持つノードだけに、「勝ち」をつける。
    //   idx:選択したleafノードのidx、winner:ロールアウトの勝者 
    //  ---
    //  toku idxのところのノードで、rolloutした結果のwinner(つまり、このidxから見通して勝つのが誰かを予想した結果、)
    //      parのcurrect_player(このidxを選択した人)がwinnerなら(つまりこのidxを打った人が勝つと予想した)場合、このidxの価値を一つ上げる。
    //      visitも増える。
    //      これを、さらにその親にさかのぼっていく。さらにその親がwinnerならば、このparも価値が上がる。
    //      勝っても負けても、visitはインクリメントする。
    //  →この価値判定と、visit回数を足していきrootまでさかのぼらせるのをバックプロパゲーションと呼んでいる。
    //=============================================================================
    void backprop(int idx, int winner) {
        while (idx != -1) {
            nodes[idx].visit++;                         //まず選択したleaf(idx)のvisit（試行回数）を増やします。
            int par = nodes[idx].parent;                            //親ノードにさかのぼる。
            if (par != -1) {                                        //親がいれば
                if (nodes[par].board.current_player == winner)      //親の盤面の手番が、勝者と同じならば、
                    nodes[idx].value_sum += 1.0;                    //選択ノード(idx)の勝ち数を増やす。
            }
            idx = par;                                              //さらに親にさかのぼる。
        }
    }

    Move best_child(int root) {
        int best = nodes[root].children[0], best_v = -1;
        for (int ci : nodes[root].children)
            if (nodes[ci].visit > best_v) { best_v = nodes[ci].visit; best = ci; }
        return nodes[best].move;
    }

    // ---- 時間ベース探索（本番用） ----
    //==================================================================
    //  制限時間内で最小手を見つける。
    // rood_board   :   Board盤面obj
    // budget_ms    ;   制限時間

    //  ==================================================================
    //      badget_timedで
    //  ==================================================================
    Move search_timed(const Board& root_board, double budget_ms) {
        auto t0 = std::chrono::steady_clock::now();                     //toku 時間計測のため
        nodes.clear();                                                  //木のノードをクリアする。
        nodes.reserve(4096);                                            //4096個のノードを確保する。   nodesは、vectgor<class Node>メンバ。

        int root = alloc(root_board, Move{{0,0,0},0}, -1);              //root:一番最初のノードか。-1は親なし   :rootは0だと思う。
                                                                        //  alloc()は、このノードをnodeをTreeに追加。
        expand(root);                                                   //  expand()は、このノードに登録された盤面についてとりうる手全部についての子ノードを追加する。
                                                                        //              終わったら、このノードのexpanded=trueにする。
        if (nodes[root].children.empty()) return Move{{0,0,0},0};       //  取りうる手がなかった（=chidrenに何も登録されなかった）場合、どこにも動けない手を返す。


        //================================================================================
        //  予算時間内に、rootの子のうち、一番価値の高いものを見つける。
        //  このroot_boardは、この関数が呼ばれた時点での盤面です。この盤面での、一番いい手を見つける。
        //=================================================================================
        int sims = 0;                                                   //
        while (true) {
            // 8シムごとに時間チェック（チェック自体のオーバーヘッドを抑制）
            if ((sims & 7) == 0) {                                          //++sims&7  : 0-7 : 8回やるごとに入る。
                double used = std::chrono::duration<double, std::milli>(    //t0からの経過時間がbudget_msを超えたら時間切れとする。
                    std::chrono::steady_clock::now() - t0).count();
                if (used >= budget_ms) break;
            }

            int leaf = select(root);                                        //一番価値の高い子ノードを選択します。(leafとする)
            int winner;
            //もし子供がない（leafがもうない）場合は、doneになっているはずなので、勝者を保存する。
            //もしくは子供でゲーム終了になった
            if (nodes[leaf].board.done) {                                   //もしleafでゲーム終了したら
                winner = nodes[leaf].board.winner;                          //勝者をwinnerに保存
            } else {
                //ゲームがまだ終わってない場合、leafを展開します。
                if (!nodes[leaf].expanded) expand(leaf);                    //そうでない場合、expandしてないノードであればexpandし、
                //
                winner = rollout(nodes[leaf].board);                        //ロールアウトして勝者をwinnerに保存する。
                                                                            //  →ロールアウトは、ランダムに手を選び続けてゲーム終了まで進める。勝者を返す。
            }
            backprop(leaf, winner);                                         //この選択ノード（から、ロールアウトした結果の勝者）を
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
            int leaf = select(root);                            // 一番価値の高い子ノードを選択します。(leafとする)
            int winner;
            if (nodes[leaf].board.done) {
                winner = nodes[leaf].board.winner;              //leafで終わっていれば、
            } else {
                if (!nodes[leaf].expanded) expand(leaf);        //expandはするが・・、ここではそこまで。
                winner = rollout(nodes[leaf].board);            //leafの盤面を使ったロールアウトした勝者をwinnerとする。
            }
            backprop(leaf, winner);         //
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
        int idx_of[MAXN * MAXN];                                                        //15×15分
        for (int i = 0; i < MAXN * MAXN; ++i) idx_of[i] = -1;                           //0-225 -1で初期化
        for (int r = 0; r < b.H; ++r)                                                   //盤面のh
            for (int c = 0; c < b.W; ++c)                                               //盤面のw
                if (b.g[r][c] == EMPTY) {                                               //空き地ならば、
                    idx_of[r * b.W + c] = (int)cells_.size();                           //idx_ofの該当箇所に cells_.size()(=> vector<int>) : 最終の（今回追加するcells_のindex）
                    cells_.push_back(r * b.W + c);                                      //idx_of
                }
        int k = (int)cells_.size();     //kは次のインデックス
        W_ = b.W;                                   //W_は盤面の広さ
        if (k == 0 || k > 30) return {};   // 32bit マスクの安全域      //kは、盤面上の空きますの数です。（連結してるしてないに関係なし）

        // 隣接（4連結）を構築
        adj_.assign(k, {});                                     //k個分のからのvector配列

        //すべての空きますに対する操作
        for (int i = 0; i < k; ++i) {
            int r = cells_[i] / b.W, c = cells_[i] % b.W;       //cells_[i]の座標( c , r )を取得
            //iのマスについて、4方向を調べる。
            for (int d = 0; d < 4; ++d) {                       //d : 0 -  4 四方向らしい。
                int rr = r + DR[d], cc = c + DC[d];                         //(r,c)の左右・上下隣接の場所
                if (rr < 0 || rr >= b.H || cc < 0 || cc >= b.W) continue;   // ステージ外は除外
                int fi = rr * b.W + cc;                                     //fiはその場所のインデックス
                if (idx_of[fi] >= 0) adj_[i].push_back(idx_of[fi]);         //fiが空きならばadj_に足す。
            }
        }
        //=====================================================
        //  ここで adj_ は、cells_[]


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
    if (b.empty_count <= mcts::ENDGAME_MAX_EMPTY) {             //残りますが
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
//    g++ -O2 -std=c++14 -shared -fPIC mcts_player.cpp -o mcts_player.dll
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
//  コンパイル: g++ -O2 -std=c++14 -DMCTS_SELFTEST mcts_player.cpp -o mcts_test
// ====================================================================== //
#define MCTS_SELFTEST       //

#ifdef MCTS_SELFTEST
#include <cstdio>

static void run_suite(int NP, int H, int W, int GAMES, std::mt19937& r) {
    int wins = 0;
    for (int game = 0; game < GAMES; ++game) {
        //=======================================
        //  盤面作成
        //=======================================
        int board[mcts::MAXN][mcts::MAXN] = {};                    //board toku ローカル変数
        for (int i = 0; i < H * W / 10; ++i)                       // 壁 約10%をちりばめる
            board[r() % H][r() % W] = mcts::WALL;
        int mcts_id = (game % NP) + 1;                             // 席を回して公平に(toku    プレイ手番を変更する：mcts_idはプレイヤーの手番)
        mcts::Board b;
        b.init(board, H, W, NP, 1);                                 //初期盤面を作る。
        //=======================================
        //  ゲーム開始
        //=======================================
        std::vector<mcts::Move> mv;
        while (!b.done) {
            mcts::Move chosen;
            if (b.current_player == mcts_id) {                                  //プレイヤー手番のとき
                //===================================================
                //  自分の手番ならば、制限時間を決める
                //===================================================
                double budget = mcts::budget_per_move(b.empty_count, NP);       //toku これはこの手で使える時間のこと。
                mcts::Tree t;                                                   //ノードを用意するんですかね。
                //                                                              //toku このtreeというのが、いろいろ機能をもっているようだ。
                //===================================================
                //  toku ここがアルゴリズム
                //  この盤面での最善手を見つける。
                //===================================================
                chosen = t.search_timed(b, budget);                             //toku 盤面を渡し、時間内に良い手を見つける。
            } else {                                                            //他のプレイヤーの手番のとき、
                mcts::gen_moves(b, mv);                                         //      gen_movesは、取りうる手をすべて列挙する。
                chosen = mv[r() % mv.size()];                                   //      chosenは、その中でランダムに選んだもの:r()%size()
            }
            //これを盤面に反映させる。
            //aplyすると、手番が変わります。
            b.apply(chosen.cells, chosen.n);
        }
        //b.doneが!=0のとき試合終了。
        if (b.winner == mcts_id) ++wins;
    }    //ここで  1 ゲーム終了
    double wr   = 100.0 * wins / GAMES;
    double base = 100.0 / NP;
    printf("  %dP  winrate %5.1f%%  (baseline %.1f%%)  %s\n",
           NP, wr, base,
           wr > base + 10 ? "[OK]" : (wr > base ? "[weak]" : "[BAD]"));
}
//これゲームスタート
int mcts_main() {
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
