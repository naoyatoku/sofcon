// number_takes 別の合法手数 & MCTS試行回数 ベンチ
// g++ -O3 -std=c++14 -DBENCH_LEGAL bench_legal.cpp -o bench_legal
//
// prog.cpp の mcts 名前空間を直接使うため、PlayStage等のDLL部分を
// 切り離してincludeできないので、必要な定義をプリプロセッサで無効化する。

// Build from repo root:
//   g++ -O3 -std=c++14 -Icpp/bench_stub cpp/bench_legal.cpp -o bench_legal
// __declspec(dllexport) などはスタブで無効化される。
#define BENCH_LEGAL 1
#include <cstdio>
#include <chrono>
#include <vector>

#include "../prog.cpp"

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    using namespace mcts;

    printf("=== 合法手数（空き盤面、anchor正準形） ===\n");
    printf(" %-6s", "HxW");
    for (int k : {3,4,5,6,7,8,9}) printf(" k=%-8d", k);
    printf("\n");

    for (int HW : {9, 12, 15}) {
        printf(" %-6d", HW);
        for (int k : {3,4,5,6,7,8,9}) {
            int board[MAXN][MAXN] = {};
            Board b; b.init(board, HW, HW, 5, 1, k);
            std::vector<Move> moves;
            gen_moves(b, moves);
            printf(" %-10zu", moves.size());
        }
        printf("\n");
    }

    printf("\n=== gen_moves 所要時間（1回, ms） ===\n");
    printf(" %-6s", "HxW");
    for (int k : {3,4,5,6,7,8,9}) printf(" k=%-8d", k);
    printf("\n");
    for (int HW : {9, 12, 15}) {
        printf(" %-6d", HW);
        for (int k : {3,4,5,6,7,8,9}) {
            int board[MAXN][MAXN] = {};
            Board b; b.init(board, HW, HW, 5, 1, k);
            std::vector<Move> moves;
            double t0 = now_ms();
            int N = 20;
            for (int i=0;i<N;i++) gen_moves(b, moves);
            double ms = (now_ms()-t0)/N;
            printf(" %-10.2f", ms);
        }
        printf("\n");
    }

    printf("\n=== MCTS 100ms あたりのシミュレーション回数 ===\n");
    printf(" %-6s", "HxW");
    for (int k : {3,4,5,6,7,8,9}) printf(" k=%-8d", k);
    printf("\n");
    for (int HW : {9, 12, 15}) {
        printf(" %-6d", HW);
        for (int k : {3,4,5,6,7,8,9}) {
            int board[MAXN][MAXN] = {};
            Board b; b.init(board, HW, HW, 5, 1, k);
            Tree t;
            // search_timed は sims をカウントしないので簡易計測：
            // 100ms ぶん回して内部 sims を測るため、ここでは
            // search_timed をそのまま呼び budget=100ms とする。
            double t0 = now_ms();
            t.search_timed(b, 100.0);
            double ms = now_ms()-t0;
            // nodes.size() がツリーノード総数 ≒ 試行回数の目安
            printf(" %-10zu", t.nodes.size());
        }
        printf("\n");
    }

    return 0;
}
