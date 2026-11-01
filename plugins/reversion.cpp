// Mean-reversion family: rolling z-score and (simple-average) RSI.
#include "plugin_common.h"

using namespace fac;

static Names names("zscore", "rsi");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const double* c = in->close;
    for (int64_t t = 0; t < in->n; ++t) {
        if (idx < kNumWindows) {  // zscore_w
            if (t < w - 1) { out[t] = NaN; continue; }
            double sd = window_std(c, t, w);
            out[t] = sd > 0 ? (c[t] - window_mean(c, t, w)) / sd : NaN;
        } else {                  // rsi_w over price changes d[t-w+1..t]
            if (t < w) { out[t] = NaN; continue; }
            double g = 0, l = 0;
            for (int64_t i = t - w + 1; i <= t; ++i) {
                double d = c[i] - c[i - 1];
                if (d > 0) g += d; else l -= d;
            }
            out[t] = (g + l) > 0 ? 100.0 * g / (g + l) : 50.0;
        }
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "reversion", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
