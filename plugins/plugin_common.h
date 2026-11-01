// Helpers shared by the factor plugins. Each family = 2 base operators x a
// grid of look-back windows. Formulas mirror python/factors_ref.py exactly
// (same warm-up length, same ddof=1 two-pass std), which is what makes a
// 1e-9 cross-language tolerance achievable.
#pragma once
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "factor/factor_api.h"

#define FACTOR_EXPORT extern "C" __attribute__((visibility("default")))

namespace fac {

inline constexpr int kWindows[] = {2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 90, 120, 180, 240, 250};
inline constexpr int kNumWindows = sizeof(kWindows) / sizeof(kWindows[0]);
inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

inline double window_mean(const double* x, int64_t end, int w) {  // x[end-w+1..end]
    double s = 0;
    for (int64_t i = end - w + 1; i <= end; ++i) s += x[i];
    return s / w;
}

inline double window_std(const double* x, int64_t end, int w) {  // ddof = 1, two-pass
    double m = window_mean(x, end, w), ss = 0;
    for (int64_t i = end - w + 1; i <= end; ++i) ss += (x[i] - m) * (x[i] - m);
    return std::sqrt(ss / (w - 1));
}

// Name table built once: "<op>_<w>".
struct Names {
    std::vector<std::string> v;
    Names(const char* op_a, const char* op_b) {
        for (const char* op : {op_a, op_b})
            for (int w : kWindows) v.push_back(std::string(op) + "_" + std::to_string(w));
    }
};

}  // namespace fac
