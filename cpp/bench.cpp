// Timing benchmark for choose_move
// コンパイル: g++ -O2 -std=c++17 bench.cpp -o bench
//
// 測定内容:
//   1. sims固定モードで各sims数の所要時間（盤面サイズ別）
//   2. 時間ベースモードで50ゲーム分の総消費時間シミュレーション
#include "mcts_player.cpp"
#include <cstdio>
#include <chrono>

int main() {
    // --- 1. sims固定ベンチ ---
    printf("=== sims-fixed benchmark (2P, all-empty board) ===\n");
    for (int HW : {9, 12, 15}) {
        int board[mcts::MAXN][mcts::MAXN] = {};
        printf(" %dx%d:\n", HW, HW);
        for (int sims : {50, 100, 200, 400, 800}) {
            auto t0 = std::chrono::steady_clock::now();
            auto mv = choose_move(board, HW, HW, 1, 2, sims);
            double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            printf("   sims=%4d : %6.1f ms\n", sims, ms);
        }
    }

    // --- 2. 時間ベースの予算推定チェック ---
    printf("\n=== budget_per_move estimate (9x9, 5P, 50 stages) ===\n");
    printf("  Simulating first 10 calls to budget_per_move:\n");
    for (int i = 0; i < 10; ++i) {
        double b = mcts::budget_per_move(9 * 9, 5);
        printf("   call %2d : %.1f ms\n", i + 1, b);
    }

    return 0;
}
