/**
 * player_example.cpp  —  コンテスト提出ファイルのひな形
 *
 * 自分でコンテストのインターフェース(Pos構造体・関数シグネチャ)に
 * 合わせて書き換えてください。
 * weights.h を同じディレクトリに置いてコンパイルするだけで動きます。
 *   g++ -O2 -std=c++17 player_example.cpp -o player
 */

#include "weights.h"        // 生成された重み (tools/export_weights.py)
#include "inference.h"      // conv2d, batch_norm, etc.
#include "inference_impl.h" // forward()

#include <vector>
#include <algorithm>
#include <cstring>

// ---- コンテストのインターフェース (要書き換え) ----
struct Pos { int row, col; };

// board: HEIGHT x WIDTH  (-1=壁, 0=空き, 1以上=各プレイヤー占有)
// my_id: 自分のプレイヤーID (1-indexed)
// num_players: 総プレイヤー数
// 戻り値: 今ターンに掘るマスのリスト (1-3マス、直交連結)
std::vector<Pos> choose_move(
    const int board[][15], int height, int width,
    int my_id, int num_players)
{
    // ---- 入力テンソル構築 ----
    // チャンネル: [壁, 空き, player1, player2, ..., current_player_norm]
    int C_in = 2 + num_players + 1;
    std::vector<float> inp(C_in * height * width, 0.f);
    auto ch = [&](int c, int r, int col) -> float& {
        return inp[(c * height + r) * width + col];
    };

    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            int v = board[r][c];
            if (v == -1)         ch(0, r, c) = 1.f;
            else if (v == 0)     ch(1, r, c) = 1.f;
            else                 ch(1 + v, r, c) = 1.f;
        }
    }
    float norm = (num_players > 1) ? (float)(my_id - 1) / (num_players - 1) : 0.f;
    for (int r = 0; r < height; ++r)
        for (int c = 0; c < width; ++c)
            ch(C_in - 1, r, c) = norm;

    // ---- NN 推論 ----
    std::vector<float> policy(height * width);
    float value;
    sofcon_nn::NetWeights wt;
    sofcon_nn::forward(inp.data(), C_in, height, width, wt, policy.data(), value);

    // ---- 合法手列挙 (1-3マス直交連結) から最良手を選択 ----
    // ここでは簡易版: policy値が最高の空きマスを anchor にして
    // そこから貪欲に最大3マス連結拡張する
    //
    // より正確には MCTS を C++ で実装して policy/value を使うべきですが、
    // 時間制限がある場合はこの greedy で十分機能します。

    // 最高スコアの空きマスを anchor に選択
    int best_r = -1, best_c = -1;
    float best_score = -1e9f;
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            if (board[r][c] != 0) continue;
            float s = policy[r * width + c];
            if (s > best_score) { best_score = s; best_r = r; best_c = c; }
        }
    }

    if (best_r < 0) return {};  // no empty cells (shouldn't happen)

    // 連結拡張 (最大3マス)
    std::vector<Pos> move = {{best_r, best_c}};
    bool used[15][15] = {};
    used[best_r][best_c] = true;

    const int DR[] = {-1,1,0,0};
    const int DC[] = {0,0,-1,1};

    for (int step = 0; step < 2; ++step) {
        // find best scoring neighbor of current move set
        int nr = -1, nc = -1;
        float nb_score = -1e9f;
        for (auto& p : move) {
            for (int d = 0; d < 4; ++d) {
                int rr = p.row + DR[d], cc = p.col + DC[d];
                if (rr < 0 || rr >= height || cc < 0 || cc >= width) continue;
                if (board[rr][cc] != 0 || used[rr][cc]) continue;
                float s = policy[rr * width + cc];
                if (s > nb_score) { nb_score = s; nr = rr; nc = cc; }
            }
        }
        if (nr < 0) break;
        // only extend if it improves expected outcome (value > 0)
        // simple heuristic: extend if remaining empty count is divisible
        move.push_back({nr, nc});
        used[nr][nc] = true;
    }

    return move;
}
