"""Research-side reference implementation of the 4 factor families.

This is the "Python factor code" that gets migrated to C++ plugins. The C++
side (plugins/*.cpp) must reproduce every value here; compare_factors.py
checks it factor by factor.
"""
import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

WINDOWS = [2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 30, 40, 50, 60, 90, 120, 180, 240, 250]


def _roll(x, w):
    """Windows x[t-w+1..t] for t >= w-1 -> shape (n-w+1, w)."""
    return sliding_window_view(x, w)


def _place(n, start, values):
    out = np.full(n, np.nan)
    if len(values):
        out[start:start + len(values)] = values
    return out


def roc(c, w):
    n = len(c)
    return _place(n, w, c[w:] / c[:-w] - 1.0) if n > w else np.full(n, np.nan)


def ma_ratio(c, w):
    n = len(c)
    if n < w:
        return np.full(n, np.nan)
    return _place(n, w - 1, c[w - 1:] / _roll(c, w).mean(axis=1) - 1.0)


def ret_std(c, w):
    n = len(c)
    r = c[1:] / c[:-1] - 1.0          # r[t] for t >= 1
    if len(r) < w:
        return np.full(n, np.nan)
    return _place(n, w, _roll(r, w).std(axis=1, ddof=1))


def hl_range(h, l, c, w):
    x = (h - l) / c
    n = len(c)
    return _place(n, w - 1, _roll(x, w).mean(axis=1)) if n >= w else np.full(n, np.nan)


def vol_ratio(v, w):
    n = len(v)
    return _place(n, w - 1, v[w - 1:] / _roll(v, w).mean(axis=1)) if n >= w else np.full(n, np.nan)


def vwap_dev(c, v, w):
    n = len(c)
    if n < w:
        return np.full(n, np.nan)
    pv = _roll(c * v, w).sum(axis=1)
    vv = _roll(v, w).sum(axis=1)
    with np.errstate(divide="ignore", invalid="ignore"):
        vals = np.where(vv > 0, c[w - 1:] / (pv / vv) - 1.0, np.nan)
    return _place(n, w - 1, vals)


def zscore(c, w):
    n = len(c)
    if n < w:
        return np.full(n, np.nan)
    win = _roll(c, w)
    sd = win.std(axis=1, ddof=1)
    with np.errstate(divide="ignore", invalid="ignore"):
        vals = np.where(sd > 0, (c[w - 1:] - win.mean(axis=1)) / sd, np.nan)
    return _place(n, w - 1, vals)


def rsi(c, w):
    n = len(c)
    d = np.diff(c)                    # d[t] for t >= 1
    if len(d) < w:
        return np.full(n, np.nan)
    win = _roll(d, w)
    g = np.where(win > 0, win, 0).sum(axis=1)
    l = np.where(win < 0, -win, 0).sum(axis=1)
    with np.errstate(divide="ignore", invalid="ignore"):
        vals = np.where(g + l > 0, 100.0 * g / (g + l), 50.0)
    return _place(n, w, vals)


FAMILIES = {
    "momentum": [("roc", lambda o, h, l, c, v, w: roc(c, w)),
                 ("ma_ratio", lambda o, h, l, c, v, w: ma_ratio(c, w))],
    "volatility": [("ret_std", lambda o, h, l, c, v, w: ret_std(c, w)),
                   ("hl_range", lambda o, h, l, c, v, w: hl_range(h, l, c, w))],
    "volume": [("vol_ratio", lambda o, h, l, c, v, w: vol_ratio(v, w)),
               ("vwap_dev", lambda o, h, l, c, v, w: vwap_dev(c, v, w))],
    "reversion": [("zscore", lambda o, h, l, c, v, w: zscore(c, w)),
                  ("rsi", lambda o, h, l, c, v, w: rsi(c, w))],
}


def factor_list():
    """[(family, name, fn, window)] in the same order as the C++ plugins."""
    out = []
    for fam, ops in FAMILIES.items():
        for op, fn in ops:
            for w in WINDOWS:
                out.append((fam, f"{op}_{w}", fn, w))
    return out
