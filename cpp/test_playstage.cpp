// PlayStage インターフェーステスト
#define STAGE_Y_MAX 15
#define STAGE_X_MAX 15
#define LANDS_SHARK_MAX 10
#include "mcts_player.cpp"
#include <cstdio>

int main() {
    // 9x9 の空き盤面、残りは壁
    char floor[STAGE_Y_MAX][STAGE_X_MAX];
    for (int r = 0; r < STAGE_Y_MAX; ++r)
        for (int c = 0; c < STAGE_X_MAX; ++c)
            floor[r][c] = (r < 9 && c < 9) ? 0 : -1;

    RULES_TAG rules;
    rules.your_number    = 1;
    rules.number_takes   = 3;
    rules.number_players = 3;

    char count = 0;
    TAKE_TAG lands[LANDS_SHARK_MAX] = {};

    PlayStage(floor, &count, lands, rules);

    printf("count=%d\n", (int)count);
    for (int i = 0; i < (int)count; ++i)
        printf("  lands[%d] = (x=%d, y=%d)  floor[y][x]=%d\n",
               i, lands[i].x, lands[i].y, (int)floor[lands[i].y][lands[i].x]);

    // 返却されたマスが全て空き(0)かチェック
    int ok = 1;
    for (int i = 0; i < (int)count; ++i)
        if (floor[lands[i].y][lands[i].x] != 0) ok = 0;
    printf("all returned cells were empty(0): %s\n", ok ? "YES [OK]" : "NO [BUG]");

    return 0;
}
