// Mean-reversion family: rolling z-score and (simple-average) RSI.
#include "plugin_common.h"

using namespace fac;

static Names names("zscore", "rsi");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const int64_t n = in->n;
    if (n == 0) return 0;
    const double* c = in->close;
    if (idx < kNumWindows) {  // zscore_w
        std::vector<double> mean(n);
        rolling_mean_std(c, n, w, mean.data(), out);
        for (int64_t t = w - 1; t < n; ++t) out[t] = out[t] > 0 ? (c[t] - mean[t]) / out[t] : NaN;
    } else {                  // rsi_w over price changes d[t-w+1..t], d[0] undefined
        std::vector<double> g(n, 0.0), l(n, 0.0), sg(n), sl(n);
        for (int64_t t = 1; t < n; ++t) {
            double d = c[t] - c[t - 1];
            if (d > 0) g[t] = d; else l[t] = -d;
        }
        // windows over d[1..n-1]; sub-array index k corresponds to t = k + 1
        rolling_sum(g.data() + 1, n - 1, w, sg.data() + 1);
        rolling_sum(l.data() + 1, n - 1, w, sl.data() + 1);
        out[0] = NaN;
        for (int64_t t = 1; t < n; ++t) {
            if (t < w) { out[t] = NaN; continue; }
            double G = sg[t], L = sl[t];
            out[t] = (G + L) > 0 ? 100.0 * G / (G + L) : 50.0;
        }
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "reversion", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
