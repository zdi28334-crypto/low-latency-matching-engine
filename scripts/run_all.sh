#!/usr/bin/env bash
# Build everything and run every demo / verification end to end.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
mkdir -p data

echo "==> build"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build build -j"$(nproc)"

echo; echo "==> unit tests"
./build/unit_tests

echo; echo "==> [1] matching core throughput (single thread)"
./build/bench_book 100 2000000

echo; echo "==> [2] multi-threaded engine: logging A/B at max load"
for log in sync async; do ./build/bench_engine --shards 1 --log $log --orders 2000000 --pin 1 | grep -vE "^  shard"; done

echo; echo "==> [3] latency at a fixed offered load of 100K msg/s"
./build/bench_engine --shards 1 --log async --orders 1000000 --rate 100000 --pin 1 | grep -vE "^  shard"

echo; echo "==> platform jitter (idle pinned CPU, 5s)"
./build/hiccup 1

echo; echo "==> [4] L2 feed -> rebuild -> snapshot check (SSE and SZSE styles)"
./build/gen_flow data/day.flow 100 2000000 42
./build/exchange_sim data/day.flow data/ticks.csv data/engine_snapshot.csv --venue sse
./build/l2_rebuild data/ticks.csv --venue sse --expect data/engine_snapshot.csv --watch 0 --l5 data/l5_sym0.csv
./build/exchange_sim data/day.flow data/ticks_szse.csv data/engine_snapshot_szse.csv --venue szse --stp oldest
./build/l2_rebuild data/ticks_szse.csv --venue szse --expect data/engine_snapshot_szse.csv

echo; echo "==> [5] replay + differential + golden regression"
./build/replay_diff data/day.flow --record data/golden.trades
./build/replay_diff data/day.flow --golden data/golden.trades

echo; echo "==> [5b] crash recovery: SIGKILL at random moments, recover, compare, resume"
./build/crash_test 10 50 2000000 100000

echo; echo "==> [6] Python -> C++ factor plugins consistency"
python3 python/gen_bars.py data/bars.csv 30 1500 1
./build/factor_runner data/bars.csv data/factors_cpp build/libfactor_{momentum,volatility,volume,reversion}.so
(cd python && python3 compare_factors.py "$ROOT/data/bars.csv" "$ROOT/data/factors_cpp")

echo; echo "==> [7] index-futures basis monitor"
(cd python && python3 gen_basis_data.py "$ROOT/data/basis_day.csv" && \
 python3 basis_monitor.py "$ROOT/data/basis_day.csv" --out "$ROOT/data/basis_out.csv")
