// Momentum family: rate of change and price / moving-average ratio.
#include "plugin_common.h"

using namespace fac;

static Names names("roc", "ma_ratio");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const int64_t n = in->n;
    const double* c = in->close;
    if (idx < kNumWindows) {  // roc_w
        for (int64_t t = 0; t < n; ++t) out[t] = t >= w ? c[t] / c[t - w] - 1.0 : NaN;
    } else {                  // ma_ratio_w
        rolling_sum(c, n, w, out);
        for (int64_t t = w - 1; t < n; ++t) out[t] = c[t] / (out[t] / w) - 1.0;
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "momentum", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
