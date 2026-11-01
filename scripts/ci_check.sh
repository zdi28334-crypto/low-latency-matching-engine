#!/usr/bin/env bash
# Fast correctness pipeline used by CI (no benchmarks).
#   scripts/ci_check.sh [build_dir]
set -euo pipefail
cd "$(dirname "$0")/.."
B=${1:-build}
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

echo "==> unit tests"
"$B"/unit_tests

echo "==> L2 feed -> rebuild -> snapshot check"
"$B"/gen_flow "$D"/day.flow 30 300000 7
"$B"/exchange_sim "$D"/day.flow "$D"/ticks.csv "$D"/snap.csv
"$B"/l2_rebuild "$D"/ticks.csv --expect "$D"/snap.csv

echo "==> differential replay + golden round-trip"
"$B"/replay_diff "$D"/day.flow --record "$D"/golden.trades
"$B"/replay_diff "$D"/day.flow --golden "$D"/golden.trades

echo "==> Python -> C++ factor consistency"
python3 python/gen_bars.py "$D"/bars.csv 10 800 3
"$B"/factor_runner "$D"/bars.csv "$D"/fac "$B"/libfactor_{momentum,volatility,volume,reversion}.so
(cd python && python3 compare_factors.py "$D"/bars.csv "$D"/fac)

echo "==> basis monitor"
(cd python && python3 gen_basis_data.py "$D"/basis.csv > /dev/null && \
 python3 basis_monitor.py "$D"/basis.csv --out "$D"/basis_out.csv | tail -1)
echo "ALL CHECKS PASSED"
