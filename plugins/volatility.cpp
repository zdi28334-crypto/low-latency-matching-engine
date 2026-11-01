// Volatility family: realised return std and mean high-low range.
#include "plugin_common.h"

using namespace fac;

static Names names("ret_std", "hl_range");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const int64_t n = in->n;
    if (n == 0) return 0;
    std::vector<double> x(n), tmp(n);
    if (idx < kNumWindows) {  // ret_std_w: std of simple returns r[t-w+1..t], r[0] undefined
        for (int64_t t = 1; t < n; ++t) x[t] = in->close[t] / in->close[t - 1] - 1.0;
        out[0] = NaN;
        // run on r[1..n-1]; result index k corresponds to t = k + 1
        rolling_mean_std(x.data() + 1, n - 1, w, tmp.data(), out + 1);
    } else {                  // hl_range_w: mean of (high-low)/close
        for (int64_t t = 0; t < n; ++t) x[t] = (in->high[t] - in->low[t]) / in->close[t];
        rolling_sum(x.data(), n, w, out);
        for (int64_t t = w - 1; t < n; ++t) out[t] /= w;
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "volatility", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
