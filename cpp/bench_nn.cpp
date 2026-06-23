// C++ NN inference speed benchmark
// g++ -O2 -std=c++14 bench_nn.cpp -o bench_nn
#include "weights.h"
#include "inference.h"
#include "inference_impl.h"
#include <cstdio>
#include <chrono>
#include <vector>

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    sofcon_nn::NetWeights wt{};  // unused (globals from weights.h)

    for (int HW : {9, 15}) {
        int hw = HW * HW;
        int c_in = 9;
        std::vector<float> input(c_in * hw, 0.f);
        // channel 1 = all empty
        for (int i = 0; i < hw; ++i) input[hw + i] = 1.f;

        std::vector<float> policy(hw);
        float value = 0.f;

        // warmup
        for (int i = 0; i < 3; ++i)
            sofcon_nn::forward(input.data(), c_in, HW, HW, wt, policy.data(), value);

        int N = 20;
        double t0 = now_ms();
        for (int i = 0; i < N; ++i)
            sofcon_nn::forward(input.data(), c_in, HW, HW, wt, policy.data(), value);
        double ms = (now_ms() - t0) / N;

        printf("%dx%d: %.1f ms/call  (value=%.4f)\n", HW, HW, ms, value);
    }
    return 0;
}
