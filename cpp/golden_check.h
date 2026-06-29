// ===================================================================== //
//  golden_check.h — 自作 forward の各段出力を golden/*.bin と照合する。
//
//  使い方（自作 forward のテストコードで）:
//    #include "golden_check.h"
//    ...stem を計算して float* my_stem (= 16*15*15 個) を得る...
//    golden::check("stem", my_stem, 16*15*15);   // 一致判定を表示
//
//  golden は先に生成しておく:
//    python tools/dump_golden.py --checkpoint model/checkpoint_16ch_15x15.pt --seed 42
//
//  .bin フォーマット（tools/dump_golden.py と一致）:
//    int32 ndim, int32 shape[ndim], float32 data[prod(shape)]  // C順(row-major)
//
//  段の名前: input / stem / block0 / block1 / block2 /
//            policy_probs / value_vec / value0_sigmoid
// ===================================================================== //
#pragma once
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

namespace golden {

// golden/<name>.bin を読み込む。成功で true、data/shape に格納。
inline bool load(const std::string& name, std::vector<float>& data,
                 std::vector<int>& shape, const std::string& dir = "golden") {
    std::string path = dir + "/" + name + ".bin";
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::printf("[golden] 開けません: %s\n", path.c_str()); return false; }
    int32_t ndim = 0;
    if (std::fread(&ndim, sizeof(int32_t), 1, f) != 1) { std::fclose(f); return false; }
    shape.resize(ndim);
    long n = 1;
    for (int i = 0; i < ndim; ++i) {
        int32_t s = 0;
        if (std::fread(&s, sizeof(int32_t), 1, f) != 1) { std::fclose(f); return false; }
        shape[i] = s; n *= s;
    }
    data.resize(n);
    size_t got = std::fread(data.data(), sizeof(float), n, f);
    std::fclose(f);
    return got == (size_t)n;
}

// 自作出力 mine[0..count) を golden/<name>.bin と比較。
// L2(golden), L2(diff), 最大絶対差, 先頭8値を表示。max_abs_diff < tol で合格。
inline bool check(const std::string& name, const float* mine, long count,
                  float tol = 1e-4f, const std::string& dir = "golden") {
    std::vector<float> g; std::vector<int> shape;
    if (!load(name, g, shape, dir)) return false;

    if ((long)g.size() != count) {
        std::printf("[%s] ★要素数不一致: golden=%zu  mine=%ld\n",
                    name.c_str(), g.size(), count);
        return false;
    }
    double l2g = 0, l2d = 0, maxd = 0; long argmax = -1;
    for (long i = 0; i < count; ++i) {
        double d = (double)mine[i] - g[i];
        l2g += (double)g[i] * g[i];
        l2d += d * d;
        if (std::fabs(d) > maxd) { maxd = std::fabs(d); argmax = i; }
    }
    l2g = std::sqrt(l2g); l2d = std::sqrt(l2d);
    bool ok = (maxd < tol);

    std::printf("[%s] %s  L2(golden)=%.6f  L2(diff)=%.3e  maxdiff=%.3e @%ld\n",
                name.c_str(), ok ? "一致 ✓" : "★不一致", l2g, l2d, maxd, argmax);
    std::printf("    golden first8:");
    for (int i = 0; i < 8 && i < count; ++i) std::printf(" %+.6f", g[i]);
    std::printf("\n    mine   first8:");
    for (int i = 0; i < 8 && i < count; ++i) std::printf(" %+.6f", mine[i]);
    std::printf("\n");
    if (!ok && argmax >= 0)
        std::printf("    最大ズレ位置 @%ld: golden=%+.6f  mine=%+.6f\n",
                    argmax, g[argmax], mine[argmax]);
    return ok;
}

} // namespace golden
