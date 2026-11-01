// Volume family: volume ratio and deviation from rolling VWAP.
#include "plugin_common.h"

using namespace fac;

static Names names("vol_ratio", "vwap_dev");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const double *c = in->close, *v = in->volume;
    for (int64_t t = 0; t < in->n; ++t) {
        if (t < w - 1) { out[t] = NaN; continue; }
        if (idx < kNumWindows) {  // vol_ratio_w
            out[t] = v[t] / window_mean(v, t, w);
        } else {                  // vwap_dev_w
            double pv = 0, vv = 0;
            for (int64_t i = t - w + 1; i <= t; ++i) { pv += c[i] * v[i]; vv += v[i]; }
            out[t] = vv > 0 ? c[t] / (pv / vv) - 1.0 : NaN;
        }
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "volume", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
