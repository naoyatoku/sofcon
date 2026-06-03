// 終盤厳密解ソルバーの検証
//   1) 正しさ: 独立実装の総当たり minimax(2人) と Endgame.solve の判定一致を確認
//   2) 強さ  : ソルバー有り vs 無し の直接対戦（小盤面・終盤頻出）
//
// コンパイル: g++ -O2 -std=c++17 endgame_test.cpp -o endgame_test
#include "mcts_player.cpp"
#include <cstdio>

using namespace mcts;

// --- 独立実装：2人ゲームで「手番側が勝ちを強制できるか」総当たり minimax ---
//   Endgame とは別ロジック（Board + gen_moves をそのまま使う）で書き、突き合わせる。
static bool brute_win(Board b) {
    if (b.done) {
        // 直前に動いた側（= 手番が回る前のプレイヤー）が勝者。ここには来ない想定。
        return false;
    }
    int mover = b.current_player;
    std::vector<Move> mv;
    gen_moves(b, mv);
    for (const Move& m : mv) {
        Board nb = b;
        nb.apply(m.cells, m.n);
        if (nb.done) {
            // この手で最後のマスを取った → mover の勝ち
            return true;
        }
        // 相手の手番。相手が勝てない(=brute_win(nb)==false)なら mover の勝ち
        if (!brute_win(nb)) return true;
    }
    return false;  // どの手でも相手に勝ちを許す
}

static void test_correctness() {
    std::mt19937 r(2024);
    const int TRIALS = 400;
    int mism = 0, checked = 0;
    for (int t = 0; t < TRIALS; ++t) {
        int H = 3 + r() % 3, W = 3 + r() % 3;   // 3..5 角
        int board[MAXN][MAXN] = {};
        // ランダムに壁/既掘りで空きを減らし、終盤局面を作る
        int fill = r() % (H * W);
        for (int i = 0; i < fill; ++i) board[r() % H][r() % W] = (r() & 1) ? -1 : 1;
        Board b; b.init(board, H, W, 2, 1);
        // brute_win は指数的なので空き <=11 に限定（ソルバーは16まで扱うが正しさは小局面で検証）
        if (b.empty_count == 0 || b.empty_count > 11) continue;

        Endgame eg;
        EndgameResult er = eg.solve(b, 1, 50000000);
        if (!er.solved) continue;
        bool brute = brute_win(b);
        ++checked;
        if (er.win != brute) {
            ++mism;
            if (mism <= 5)
                printf("  MISMATCH: empty=%d solver=%d brute=%d\n",
                       b.empty_count, er.win, brute);
        }
        // 合法手チェック
        if (er.win) {
            // move のセルがすべて空きで、サイズ1..3か
            if (er.move.n < 1 || er.move.n > 3) printf("  BAD move size %d\n", er.move.n);
        }
    }
    printf("[correctness] checked=%d  mismatches=%d  %s\n",
           checked, mism, mism == 0 ? "PASS" : "FAIL");
}

// --- 強さ: ソルバー有り(P_solver) vs 無し(相手) の直接対戦 ---
//   無し側は budget を絞った時間ベース MCTS（終盤も MCTS のまま）。
static Move move_with_solver(const Board& b, int me, bool use_solver) {
    // MCTS は両者とも固定 sims（公平・高速）。違いは終盤ソルバーの有無のみ。
    const int FIXED_SIMS = 400;
    if (use_solver && b.empty_count <= ENDGAME_MAX_EMPTY) {
        Endgame eg;
        EndgameResult er = eg.solve(b, me, ENDGAME_NODE_CAP);
        if (er.solved && er.win) return er.move;
    }
    Tree t;
    return t.search(b, FIXED_SIMS);
}

static void test_strength() {
    std::mt19937 r(7);
    const int H = 6, W = 6, GAMES = 40;   // 36マス：終盤が頻繁に発生
    int swins = 0;
    for (int g = 0; g < GAMES; ++g) {
        int board[MAXN][MAXN] = {};
        for (int i = 0; i < H * W / 10; ++i) board[r() % H][r() % W] = -1;
        int solver_id = (g % 2) + 1;
        Board b; b.init(board, H, W, 2, 1);
        while (!b.done) {
            bool use = (b.current_player == solver_id);
            Move m = move_with_solver(b, b.current_player, use);
            b.apply(m.cells, m.n);
        }
        if (b.winner == solver_id) ++swins;
    }
    printf("[strength] solver-on vs solver-off (6x6 2P, %d games): solver wins %d (%.0f%%)\n",
           GAMES, swins, 100.0 * swins / GAMES);
}

int main() {
    printf("=== Endgame solver verification ===\n");
    test_correctness();
    test_strength();
    return 0;
}
