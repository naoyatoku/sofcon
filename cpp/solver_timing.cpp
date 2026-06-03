// 終盤ソルバーの最悪時間を測る（空きマス数別）
// g++ -O2 -std=c++17 solver_timing.cpp -o solver_timing
#include "mcts_player.cpp"
#include <cstdio>
#include <chrono>
using namespace mcts;

int main() {
    std::mt19937 r(1);
    printf("=== Endgame solver worst-case timing (node cap=%ld) ===\n", ENDGAME_NODE_CAP);
    // 空きマス数を変えて、最悪付近の時間を測る（全空きの正方領域＝分岐最大）
    for (int empties : {10, 12, 14, 16}) {
        // empties マスがちょうど空く正方/長方領域を作る
        int side = 1; while (side * side < empties) ++side;
        int H = side, W = side;
        double worst = 0; long worst_nodes = 0;
        for (int trial = 0; trial < 8; ++trial) {
            int board[MAXN][MAXN] = {};
            // side*side - empties マスを壁で潰す
            int to_wall = side * side - empties;
            int placed = 0;
            while (placed < to_wall) {
                int rr = r() % H, cc = r() % W;
                if (board[rr][cc] == 0) { board[rr][cc] = -1; ++placed; }
            }
            for (int NP : {2, 5}) {
                Board b; b.init(board, H, W, NP, 1);
                if (b.empty_count != empties) continue;
                Endgame eg;
                auto t0 = std::chrono::steady_clock::now();
                EndgameResult er = eg.solve(b, 1, ENDGAME_NODE_CAP);
                double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (ms > worst) worst = ms;
                (void)er;
            }
        }
        printf("  empties=%2d (%dx%d): worst %.1f ms\n", empties, H, W, worst);
    }
    return 0;
}
