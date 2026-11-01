/* Stable C ABI for factor plugins (.so).
 * A plugin exports one symbol, `factor_plugin_entry`, returning a static
 * descriptor. The production side loads plugins with dlopen and never links
 * against them, so research can ship a new factor family without rebuilding
 * the trading system. Plain C types only -> no C++ ABI coupling. */
#ifndef FACTOR_API_H
#define FACTOR_API_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACTOR_ABI_VERSION 1

typedef struct {
    const double* open;
    const double* high;
    const double* low;
    const double* close;
    const double* volume;
    int64_t n; /* bars, oldest first */
} FactorInput;

typedef struct {
    int abi_version;
    const char* family;
    int num_factors;
    const char* (*factor_name)(int idx);
    /* Writes n values to out; warm-up positions are NaN. Returns 0 on success. */
    int (*compute)(int idx, const FactorInput* in, double* out);
} FactorPlugin;

typedef const FactorPlugin* (*factor_plugin_entry_fn)(void);

#ifdef __cplusplus
}
#endif
#endif
