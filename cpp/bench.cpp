// Timing benchmark for choose_move on a 15x15 board.
// Compile WITHOUT -DMCTS_SELFTEST so mcts_player.cpp's main is excluded.
//   g++ -O2 -std=c++17 bench.cpp -o bench
#include "mcts_player.cpp"
#include <cstdio>
#include <chrono>

int main() {
    int board[mcts::MAXN][mcts::MAXN] = {}; // all empty 15x15
    int H = 15, W = 15, NP = 2, my_id = 1;

    for (int sims : {200, 400, 800, 1600, 3200}) {
        auto t0 = std::chrono::steady_clock::now();
        auto mv = choose_move(board, H, W, my_id, NP, sims);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        printf("sims=%5d : %7.1f ms  (move size %zu)\n", sims, ms, mv.size());
    }
    return 0;
}
