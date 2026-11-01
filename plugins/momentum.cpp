// Momentum family: rate of change and price / moving-average ratio.
#include "plugin_common.h"

using namespace fac;

static Names names("roc", "ma_ratio");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const double* c = in->close;
    for (int64_t t = 0; t < in->n; ++t) {
        if (idx < kNumWindows) out[t] = t >= w ? c[t] / c[t - w] - 1.0 : NaN;              // roc_w
        else out[t] = t >= w - 1 ? c[t] / window_mean(c, t, w) - 1.0 : NaN;                // ma_ratio_w
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "momentum", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
