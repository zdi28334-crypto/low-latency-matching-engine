"""Generate synthetic OHLCV bars (GBM) for factor-consistency testing.

usage: python gen_bars.py out.csv [symbols=30] [bars=1500] [seed=1]
"""
import sys

import numpy as np


def main():
    out = sys.argv[1]
    nsym = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    nbar = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    rng = np.random.default_rng(int(sys.argv[4]) if len(sys.argv) > 4 else 1)
    with open(out, "w") as f:
        f.write("sym,t,open,high,low,close,volume\n")
        for s in range(nsym):
            code = f"{600000 + s:06d}"
            p0 = rng.uniform(5, 100)
            ret = rng.normal(0.0002, rng.uniform(0.01, 0.03), nbar)
            close = p0 * np.exp(np.cumsum(ret))
            open_ = np.r_[p0, close[:-1]] * np.exp(rng.normal(0, 0.003, nbar))
            high = np.maximum(open_, close) * (1 + np.abs(rng.normal(0, 0.006, nbar)))
            low = np.minimum(open_, close) * (1 - np.abs(rng.normal(0, 0.006, nbar)))
            vol = np.round(rng.lognormal(13, 0.6, nbar), -2)
            for t in range(nbar):
                f.write(f"{code},{t},{open_[t]:.4f},{high[t]:.4f},{low[t]:.4f},{close[t]:.4f},{vol[t]:.0f}\n")
    print(f"wrote {out}: {nsym} securities x {nbar} bars")


if __name__ == "__main__":
    main()
