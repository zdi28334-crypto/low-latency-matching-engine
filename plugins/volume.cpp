// Volume family: volume ratio and deviation from rolling VWAP.
#include "plugin_common.h"

using namespace fac;

static Names names("vol_ratio", "vwap_dev");

static const char* name(int idx) { return names.v[idx].c_str(); }

static int compute(int idx, const FactorInput* in, double* out) {
    const int w = kWindows[idx % kNumWindows];
    const int64_t n = in->n;
    const double *c = in->close, *v = in->volume;
    if (idx < kNumWindows) {  // vol_ratio_w
        rolling_sum(v, n, w, out);
        for (int64_t t = w - 1; t < n; ++t) out[t] = v[t] / (out[t] / w);
    } else {                  // vwap_dev_w
        std::vector<double> pv(n), spv(n);
        for (int64_t t = 0; t < n; ++t) pv[t] = c[t] * v[t];
        rolling_sum(pv.data(), n, w, spv.data());
        rolling_sum(v, n, w, out);
        for (int64_t t = w - 1; t < n; ++t) out[t] = out[t] > 0 ? c[t] / (spv[t] / out[t]) - 1.0 : NaN;
    }
    return 0;
}

static const FactorPlugin plugin{FACTOR_ABI_VERSION, "volume", 2 * kNumWindows, name, compute};
FACTOR_EXPORT const FactorPlugin* factor_plugin_entry(void) { return &plugin; }
