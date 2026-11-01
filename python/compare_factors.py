"""Cross-language consistency check: Python reference vs C++ .so plugins.

usage: python compare_factors.py bars.csv cpp_prefix [rtol=1e-9]
  cpp_prefix.bin / cpp_prefix.names are written by factor_runner.
Pass criteria per factor: identical NaN (warm-up) positions and
|cpp - py| <= rtol * max(1, |py|) everywhere else.
"""
import sys
import time

import numpy as np
import pandas as pd

from factors_ref import factor_list


def main():
    bars_path, prefix = sys.argv[1], sys.argv[2]
    rtol = float(sys.argv[3]) if len(sys.argv) > 3 else 1e-9
    df = pd.read_csv(bars_path, dtype={"sym": str})
    names = [tuple(line.strip().split(",")) for line in open(prefix + ".names")]
    rows = len(df)
    cpp = np.fromfile(prefix + ".bin", dtype=np.float64).reshape(len(names), rows)

    groups = [g for _, g in df.groupby("sym", sort=False)]
    flist = factor_list()
    by_name = {(fam, name): (fn, w) for fam, name, fn, w in flist}

    t0 = time.perf_counter()
    py = np.empty_like(cpp)
    for k, key in enumerate(names):
        fn, w = by_name[key]
        pos = 0
        for g in groups:
            o, h, l, c, v = (g[col].to_numpy(np.float64) for col in ["open", "high", "low", "close", "volume"])
            py[k, pos:pos + len(g)] = fn(o, h, l, c, v, w)
            pos += len(g)
    t_py = time.perf_counter() - t0

    failures, worst = [], (0.0, None)
    fams = {}
    for k, key in enumerate(names):
        a, b = cpp[k], py[k]
        nan_ok = np.array_equal(np.isnan(a), np.isnan(b))
        m = ~np.isnan(b)
        err = np.abs(a[m] - b[m]) / np.maximum(1.0, np.abs(b[m])) if m.any() else np.array([0.0])
        e = float(err.max()) if err.size else 0.0
        if e > worst[0]:
            worst = (e, key)
        ok = nan_ok and e <= rtol
        fams.setdefault(key[0], [0, 0])[0 if ok else 1] += 1
        if not ok:
            failures.append((key, nan_ok, e))

    print(f"python reference: {len(names)} factors x {rows} rows in {t_py:.2f}s")
    for fam, (ok, bad) in fams.items():
        print(f"  {fam:<11} {ok:>3} pass  {bad} fail")
    print(f"worst relative error: {worst[0]:.3e} ({worst[1][1] if worst[1] else '-'})  tolerance {rtol:g}")
    for key, nan_ok, e in failures[:10]:
        print(f"  FAIL {key}: nan_mask_equal={nan_ok} max_err={e:.3e}")
    print("RESULT:", "PASS" if not failures else f"FAIL ({len(failures)} factors)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
