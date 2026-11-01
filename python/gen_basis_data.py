"""Synthetic second-level spot/futures data for one trading day.

Columns: time,hs300,csi500,IF2611,IC2611
Two anomalies are injected so the monitor has something to catch:
  10:45:00 - 10:47:00  IC discount suddenly deepens to about -25% annualised
  14:10:00 - 14:11:30  IF flips to about +6% annualised premium

usage: python gen_basis_data.py out.csv [trade_date=2026-11-02] [seed=3]
"""
import datetime as dt
import sys

import numpy as np

from basis_monitor import days_to_expiry, third_friday


def trading_seconds():
    t = []
    for start, end in [((9, 30), (11, 30)), ((13, 0), (15, 0))]:
        s = start[0] * 3600 + start[1] * 60
        e = end[0] * 3600 + end[1] * 60
        t.extend(range(s, e))
    return np.array(t)


def main():
    out = sys.argv[1]
    date = dt.date.fromisoformat(sys.argv[2]) if len(sys.argv) > 2 else dt.date(2026, 11, 2)
    rng = np.random.default_rng(int(sys.argv[3]) if len(sys.argv) > 3 else 3)
    secs = trading_seconds()
    n = len(secs)
    d_if = days_to_expiry(date, third_friday(2026, 11))
    hs = 3900 * np.exp(np.cumsum(rng.normal(0, 0.00012, n)))
    zz = 6100 * np.exp(np.cumsum(rng.normal(0, 0.00016, n)))
    # annualised basis paths (slowly mean-reverting) + quote noise
    b_if = -0.03 + np.cumsum(rng.normal(0, 0.0004, n)) * 0.2
    b_ic = -0.10 + np.cumsum(rng.normal(0, 0.0006, n)) * 0.2
    hhmmss = lambda s: (s // 3600) * 10000 + (s // 60 % 60) * 100 + s % 60
    tags = np.array([hhmmss(s) for s in secs])
    b_ic[(tags >= 104500) & (tags < 104700)] = -0.25
    b_if[(tags >= 141000) & (tags < 141130)] = 0.06
    f_if = hs * (1 + b_if * d_if / 365) + rng.normal(0, 0.2, n)
    f_ic = zz * (1 + b_ic * d_if / 365) + rng.normal(0, 0.4, n)
    with open(out, "w") as f:
        f.write("time,hs300,csi500,IF2611,IC2611\n")
        for i in range(n):
            f.write(f"{tags[i]:06d},{hs[i]:.2f},{zz[i]:.2f},{f_if[i]:.1f},{f_ic[i]:.1f}\n")
    print(f"wrote {out}: {n} seconds, trade date {date}, IF/IC2611 expire {third_friday(2026, 11)} ({d_if} days)")


if __name__ == "__main__":
    main()
