// Helpers shared by the factor plugins. Each family = 2 base operators x a
// grid of look-back windows. Formulas mirror python/factors_ref.py exactly
// (same warm-up length, ddof=1 std), so a 1e-9 cross-language tolerance holds.
//
// Rolling-window computation is O(n) instead of O(n*w):
//  * sums are updated incrementally (add the new value, drop the old one);
//  * floating-point drift from repeated add/subtract is bounded by an exact
//    recomputation every max(w, 16) steps (amortised O(1) per step);
//  * variance uses moments of (x - K), with the anchor K reset to the window
//    mean at every recomputation, which avoids catastrophic cancellation in
//    sum(x^2) - sum(x)^2 / w for price-level data;
//  * very short windows (w <= kDirectMax) are computed directly: the cost is
//    trivial and their variance can be tiny, where drift would be relatively
//    largest.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "factor/factor_api.h"

#define FACTOR_EXPORT extern "C" __attribute__((visibility("default")))

namespace fac {

inline constexpr int kWindows[] = {2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 90, 120, 180, 240, 250};
inline constexpr int kNumWindows = sizeof(kWindows) / sizeof(kWindows[0]);
inline constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
inline constexpr int kDirectMax = 16;

inline double direct_sum(const double* x, int64_t end, int w) {
    double s = 0;
    for (int64_t i = end - w + 1; i <= end; ++i) s += x[i];
    return s;
}

// out[t] = sum(x[t-w+1..t]) for t >= w-1, NaN before.
inline void rolling_sum(const double* x, int64_t n, int w, double* out) {
    const int64_t refresh = w > kDirectMax ? w : kDirectMax;
    double s = 0;
    for (int64_t t = 0; t < n; ++t) {
        s += x[t];
        if (t >= w) s -= x[t - w];
        if (t < w - 1) { out[t] = NaN; continue; }
        if ((t - (w - 1)) % refresh == 0) s = direct_sum(x, t, w);  // bound drift
        out[t] = s;
    }
}

// Rolling mean and sample std (ddof = 1) for t >= w-1; NaN before.
inline void rolling_mean_std(const double* x, int64_t n, int w, double* mean, double* sd) {
    for (int64_t t = 0; t < n && t < w - 1; ++t) mean[t] = sd[t] = NaN;
    if (w <= kDirectMax) {
        for (int64_t t = w - 1; t < n; ++t) {
            double m = direct_sum(x, t, w) / w, ss = 0;
            for (int64_t i = t - w + 1; i <= t; ++i) ss += (x[i] - m) * (x[i] - m);
            mean[t] = m;
            sd[t] = std::sqrt(ss / (w - 1));
        }
        return;
    }
    double K = 0, s = 0, ss = 0;  // moments of (x - K) over the window
    for (int64_t t = w - 1; t < n; ++t) {
        if ((t - (w - 1)) % w == 0) {  // exact recompute + re-anchor
            K = direct_sum(x, t, w) / w;
            s = ss = 0;
            for (int64_t i = t - w + 1; i <= t; ++i) { double d = x[i] - K; s += d; ss += d * d; }
        } else {
            double a = x[t] - K, b = x[t - w] - K;
            s += a - b;
            ss += a * a - b * b;
        }
        double m = s / w;
        double var = (ss - s * m) / (w - 1);
        mean[t] = K + m;
        sd[t] = std::sqrt(var > 0 ? var : 0);
    }
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
