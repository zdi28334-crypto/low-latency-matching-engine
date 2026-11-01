"""Second-level annualised basis monitor for CSI 300 (IF) / CSI 500 (IC) index
futures, with WeCom (企业微信) robot alerts.

annualised basis = (F - S) / S * 365 / days_to_expiry
  F: futures price, S: index spot, expiry: CFFEX rule = third Friday of the
  contract month (rolls to the next business day on a holiday — supply a
  holiday calendar for production use).

Alert logic (per contract), evaluated every second:
  * level rule : basis outside [lower, upper]
  * jump rule  : |z-score| of basis vs the trailing window > z_max
  * debounce   : a breach must persist `confirm` seconds before alerting
  * cooldown   : the same contract/rule alerts at most once per `cooldown` s
  * recovery   : one "recovered" message after `recover` clean seconds
Alerts go to the WeCom robot webhook in env WECOM_WEBHOOK; without it the
payload is printed (dry run).

usage: python basis_monitor.py data.csv [--date 2026-11-02] [--out basis.csv]
"""
import argparse
import calendar
import collections
import datetime as dt
import json
import math
import os
import urllib.request

PAIRS = [  # contract, spot column, futures column, expiry year, month, lower, upper
    ("IF2611", "hs300", "IF2611", 2026, 11, -0.12, 0.04),
    ("IC2611", "csi500", "IC2611", 2026, 11, -0.20, 0.03),
]


def third_friday(year, month):
    c = calendar.Calendar()
    fridays = [d for d in c.itermonthdates(year, month) if d.month == month and d.weekday() == 4]
    return fridays[2]


def days_to_expiry(today, expiry):
    return max((expiry - today).days, 1)


class RollingZ:
    def __init__(self, window):
        self.w = window
        self.q = collections.deque()
        self.s = self.ss = 0.0

    def update(self, x):
        """z-score of x against the trailing window (before adding x)."""
        z = None
        if len(self.q) >= max(30, self.w // 4):
            m = self.s / len(self.q)
            var = max(self.ss / len(self.q) - m * m, 0.0)
            sd = math.sqrt(var)
            z = (x - m) / sd if sd > 1e-12 else None
        self.q.append(x)
        self.s += x
        self.ss += x * x
        if len(self.q) > self.w:
            y = self.q.popleft()
            self.s -= y
            self.ss -= y * y
        return z


class WeComNotifier:
    def __init__(self, webhook=None):
        self.webhook = webhook
        self.sent = []

    def send(self, text):
        payload = {"msgtype": "text", "text": {"content": text}}
        self.sent.append(text)
        if not self.webhook:
            print("[dry-run WeCom]", json.dumps(payload, ensure_ascii=False))
            return
        req = urllib.request.Request(self.webhook, data=json.dumps(payload).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=3) as r:
                r.read()
        except Exception as e:  # never let alerting crash the monitor
            print("WeCom send failed:", e)


class ContractMonitor:
    def __init__(self, name, lower, upper, notifier, z_window=300, z_max=6.0,
                 confirm=3, cooldown=60, recover=30):
        self.name, self.lower, self.upper = name, lower, upper
        self.z = RollingZ(z_window)
        self.z_max, self.confirm, self.cooldown, self.recover = z_max, confirm, cooldown, recover
        self.notifier = notifier
        self.breach_run = collections.Counter()
        self.last_alert = {}
        self.active = set()
        self.clean_run = 0

    def on_tick(self, t_sec, t_str, basis, f, s):
        z = self.z.update(basis)
        rules = set()
        if basis < self.lower or basis > self.upper:
            rules.add("level")
        if z is not None and abs(z) > self.z_max:
            rules.add("jump")
        for r in ("level", "jump"):
            self.breach_run[r] = self.breach_run[r] + 1 if r in rules else 0
        fired = []
        for r in rules:
            if self.breach_run[r] >= self.confirm and t_sec - self.last_alert.get(r, -1e9) >= self.cooldown:
                self.last_alert[r] = t_sec
                self.active.add(r)
                detail = (f"区间[{self.lower:.0%}, {self.upper:.0%}]" if r == "level"
                          else f"z={z:+.1f} (阈值 {self.z_max})")
                self.notifier.send(f"[基差告警] {self.name} {t_str} 年化基差 {basis:+.2%} "
                                   f"({'越界' if r == 'level' else '跳变'}, {detail}) F={f:.1f} S={s:.2f}")
                fired.append(r)
        if rules:
            self.clean_run = 0
        elif self.active:
            self.clean_run += 1
            if self.clean_run >= self.recover:
                self.notifier.send(f"[基差恢复] {self.name} {t_str} 年化基差 {basis:+.2%} 已连续 {self.recover}s 回到正常")
                self.active.clear()
                self.clean_run = 0
        return z, fired


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--date", default="2026-11-02")
    ap.add_argument("--out", default="basis.csv")
    args = ap.parse_args()
    today = dt.date.fromisoformat(args.date)
    notifier = WeComNotifier(os.environ.get("WECOM_WEBHOOK"))
    mons, dte = {}, {}
    for name, _, _, y, m, lo, hi in PAIRS:
        mons[name] = ContractMonitor(name, lo, hi, notifier)
        dte[name] = days_to_expiry(today, third_friday(y, m))
        print(f"{name}: expiry {third_friday(y, m)}, {dte[name]} days, alert band [{lo:.0%}, {hi:.0%}]")

    n = alerts = 0
    with open(args.data) as fin, open(args.out, "w") as fout:
        header = fin.readline().strip().split(",")
        col = {c: i for i, c in enumerate(header)}
        fout.write("time,contract,spot,futures,basis_pts,annualized_basis,zscore,alert\n")
        for line in fin:
            p = line.strip().split(",")
            t = p[0]
            t_sec = int(t[:2]) * 3600 + int(t[2:4]) * 60 + int(t[4:6])
            t_str = f"{t[:2]}:{t[2:4]}:{t[4:6]}"
            for name, sc, fc, *_ in PAIRS:
                s, f = float(p[col[sc]]), float(p[col[fc]])
                basis = (f - s) / s * 365 / dte[name]
                z, fired = mons[name].on_tick(t_sec, t_str, basis, f, s)
                alerts += len(fired)
                fout.write(f"{t},{name},{s:.2f},{f:.1f},{f - s:.2f},{basis:.6f},"
                           f"{'' if z is None else f'{z:.2f}'},{'|'.join(fired)}\n")
            n += 1
    print(f"processed {n} seconds x {len(PAIRS)} contracts, {alerts} alerts, "
          f"{len(notifier.sent)} WeCom messages -> {args.out}")


if __name__ == "__main__":
    main()
