# 低延迟撮合引擎（C++20）

[![CI](https://github.com/zdi28334-crypto/low-latency-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/zdi28334-crypto/low-latency-matching-engine/actions/workflows/ci.yml)

一个面向 A 股（上交所规则）的 C++20 低延迟撮合引擎，包含多线程分片撮合、集合竞价与连续竞价、Level-2 逐笔行情重建、延迟观测与优化、Python→C++ 因子插件化、回放对比与股指期货基差监控。所有指标都由本仓库代码在本机实测得到，`scripts/run_all.sh` 一条命令即可全部复现。

```
./scripts/run_all.sh      # 编译 → 单测 → 基准 → L2 重建 → 回放对比 → 因子校验 → 基差监控
./scripts/ci_check.sh     # CI 用的快速正确性检查（不跑基准）
```

依赖：g++ ≥ 11 / clang ≥ 14、CMake ≥ 3.16、Python 3 + numpy + pandas。无第三方 C++ 库。

---

## 1. 功能模块 ↔ 代码对照

| 模块 | 实现 | 关键文件 |
|---|---|---|
| 分片与多线程撮合 | 一致性 Hash 环（虚拟节点）把证券分到 shard；每个 shard 独占线程 + 独占 OrderBook，经 SPSC 无锁队列收单 | `consistent_hash.h` `spsc_queue.h` `engine.h/.cpp` |
| 撮合规则与 L2 行情重建 | 价格-时间优先撮合、最优五档即时成交剩余撤销、上交所集合竞价定价规则、主板价格笼子；按上交所逐笔合并格式（A/D/T/S）输出行情并从行情重建盘口 | `order_book.h` `l2.h` `tools/exchange_sim.cpp` `tools/l2_rebuild.cpp` |
| 低延迟与延迟观测 | mmap 异步日志（热路径只拷贝 40 字节记录）；rdtscp 计时 + 对数直方图记录 queue_wait / match_core / publish / e2e 四段延迟；同步日志 vs 异步日志 A/B 对比 | `async_logger.h/.cpp` `latency.h` `bench/bench_engine.cpp` |
| 因子插件化 | 4 个因子族 `.so`（C ABI + dlopen 加载），O(n) 滚动窗口算法，160 个因子与 Python 逐值比对 | `include/factor/factor_api.h` `plugins/*.cpp` `tools/factor_runner.cpp` `python/factors_ref.py` `python/compare_factors.py` |
| 回放验证与基差监控 | 优化版与朴素版撮合差分回放 + golden 文件回归 + 模糊测试对拍；IF/IC 秒级年化基差监控 + 企业微信告警 | `ref_book.h` `tools/replay_diff.cpp` `fuzz/` `python/basis_monitor.py` |

---

## 2. 架构

```
                         ┌──────────── shard 0 (pinned CPU 1) ─────────────┐
 gateway ─ route(sym) ─► │ SPSC inbox ─► OrderBook[sym…] ─► EventBuffer    │──┐
 (stamp ingress TSC)     └─────────────────────────────────────────────────┘  │  LogRecord(40B)
          │              ┌──────────── shard 1 (pinned CPU 2) ─────────────┐  ├──► SPSC ring ─► logger thread ─► mmap 文件
          └────────────► │ SPSC inbox ─► OrderBook[sym…] ─► EventBuffer    │──┘   (格式化在热路径之外)
                         └─────────────────────────────────────────────────┘
   延迟打点:  ingress ──queue_wait──► dequeue ──match_core──► matched ──publish──► published
```

**为什么这样分片。** 同一证券的所有消息只进一个 shard 的单一 FIFO 队列，由单一线程处理。因此同一标的订单时序天然一致，不需要任何锁；不同证券在不同线程上并行撮合。一致性 Hash 的好处是扩缩容时只迁移约 1/N 的证券：单测里 4→5 个 shard 只有 21.6% 的证券换了归属，理想值是 20%。路由表在启动时预计算成数组，热路径上是 O(1) 查表。

### OrderBook 的低延迟设计（`include/me/order_book.h`）
- **数组档位**：A 股有涨跌停，所有可能价格事先已知。每一侧是一个按 `price - 跌停价` 下标的档位数组，查找档位 O(1)，没有红黑树的再平衡开销。
- **位图找最优价**：每侧维护一个“非空档位”位图。档位清空后，用 `clz/ctz` 几条指令就能找到下一个最优价，不用逐档扫描。
- **对象池 + 侵入式双向链表**：订单节点预分配，同价位按 FIFO 链接。撤单通过 `FlatMap<OrderId → 节点下标>` 定位，O(1) 完成。
- **FlatMap**：开放寻址加线性探测，删除用 backward-shift（不留墓碑），大量挂撤单之后探测链也不会变长。
- **热路径零分配、零 I/O**：撮合结果全部通过模板化的 Listener 回调传出，调用可以内联。

### 上交所集合竞价定价（`OrderBook::auction_price`）
按上交所交易规则依次判断：(1) 成交量最大；(2) 高于该价的买单、低于该价的卖单全部成交；(3) 该价位上买卖至少一方全部成交。仍有多个价格时取未成交量最小者，再并列则取中间价（这里实现为四舍五入到最小价位）。

单测 `call_auction_rules` 构造了一个反例：10.01 的成交量同样最大（1300），但违反条件 (2)，因为高于它的买单有 1500 股无法全部成交，所以最终定价 10.03。竞价阶段同一个函数还用于计算虚拟参考价。

### 主板价格笼子（`OrderBook::within_cage`，`CageConfig`）
连续竞价阶段的限价单要满足以下约束，否则拒单：
- 买入：申报价 ≤ max(买入基准价 × 102%，买入基准价 + 10 个最小价位)
- 卖出：申报价 ≥ min(卖出基准价 × 98%，卖出基准价 − 10 个最小价位)

买入基准价依次取卖一、买一、最近成交价、前收盘价；卖出基准价依次取买一、卖一、最近成交价、前收盘价。“10 个最小价位”这条兜底主要影响低价股：比如 2 元的股票，2% 只有 4 分钱，兜底放宽到 1 角。集合竞价阶段和市价单不受笼子约束。

比较全部用整数完成（`p*100 <= base*102`），避免浮点误差导致边界价被误判。参考实现 RefBook 故意改用浮点写法，与之独立对拍。另一种处理方式是把超出笼子的申报暂存、等笼子移动后再参与撮合，本项目未实现。上线前应以交易所最新发布的规则原文为准。

### Level-2 逐笔语义（`include/me/l2.h`）
按上交所逐笔合并行情的口径输出 A（挂单）、D（撤单）、T（成交）、S（状态）四类记录。重建时有一个容易踩的坑：**上交所只在订单“挂上簿”时才发布 A，且数量是剩余量**。一笔到达即全部成交的主动单从来不会有 A；部分成交的则先出 T，再出剩余量的 A。所以重建器处理 T 时只扣减簿上真实存在的一方，主动方查不到是正常现象。本次运行中有 843,535 笔成交属于这种情况，异常计数为 0。

### 延迟测量本身也要低开销（`include/me/latency.h`）
- **TSC 计时**：启动时检查 CPU 是否支持不变 TSC（`constant_tsc` + `nonstop_tsc`），支持则用 `rdtscp` 读时间戳计数器，并用 steady_clock 校准换算系数；不支持则自动退回 steady_clock。实测每次取时间 15 ns，steady_clock 为 21 ns。网关线程和 shard 线程在不同核上打点，跨核差值偶尔因微小偏差为负，这里统一截断为 0。
- **对数直方图**：思路同 HdrHistogram。低于 32 个 tick 的值精确计数，之上每个 2 的幂分成 16 个桶，相对误差不超过 6.25%。记录是 O(1)，内存固定约 1000 个计数器，多线程结果可以直接合并。原来的做法是每条消息往 vector 里写 4 个数，热路径上会有缓存污染和偶发的扩容。改用直方图后，异步日志模式的满负载吞吐从约 1.8–2.1 M msg/s 提升到约 2.4–2.8 M msg/s。

### 因子的 O(n) 滚动算法（`plugins/plugin_common.h`）
- 窗口和改为增量更新：加入新值、减去滑出窗口的旧值，复杂度从 O(n·w) 降到 O(n)。
- 反复加减会累积浮点误差，所以每 max(w, 16) 步做一次精确重算，摊销后每步仍是 O(1)。
- 方差不直接用 Σx² − (Σx)²/w，那样对价格序列会发生灾难性抵消。这里改为累计 (x − K) 的矩，锚点 K 在每次重算时设为窗口均值。
- 窗口很短（w ≤ 16）时直接计算：成本可以忽略，而且这种窗口的方差可能极小，累积误差在相对意义上会被放大。
- 结果：计算耗时 0.54 s → 0.055 s，约快 10 倍，与 Python 的最大相对误差仍为 8e-14。用 20 只 × 8000 根的长序列压测，最大误差为 2e-13。

---

## 3. 实测结果

测试环境：2 vCPU 云虚拟机，g++ 13，`-O2 -march=native`。合成订单流为 100 只证券、约 200 万条消息的完整交易日（开盘集合竞价 → 连续竞价 → 收盘集合竞价）。数值在不同次运行间有约 ±20% 的波动，表中给出的是多次运行的范围。

### 3.1 正确性（全部通过）
| 检查 | 结果 |
|---|---|
| 单元测试 | 17/17 通过 |
| 差分回放：优化版 OrderBook vs 朴素 std::map 版 RefBook | 905,577 笔成交逐笔一致（含 62,042 笔集合竞价成交） |
| 随机对拍（单测内） | 30,000 条随机构造的极端订单流（价格密集交叉、撤已死单、乱序切换阶段、触发价格笼子），全部一致 |
| libFuzzer 覆盖率引导模糊测试（开启 ASan + UBSan） | 120 秒内执行 807,651 次，0 不一致、0 内存错误 |
| ThreadSanitizer / AddressSanitizer + UBSan 跑全部单测 | 0 报告 |
| golden 回归：本次构建 vs 已录制成交文件 | 一致 |
| 多线程分片引擎 vs 单线程 | 40 只证券的最终盘口逐档一致 |
| L2 重建盘口 vs 撮合引擎真实盘口 | 9,359 个价位全部一致；0 缺失 / 0 多余 / 0 数量不符 |
| C++ 因子 vs Python 参考 | 160/160 通过，最大相对误差 8.2e-14（容差 1e-9） |

模糊测试除了比对成交和盘口，还会检查一条不变量：连续竞价阶段买一价必须低于卖一价。这条可以证明集合竞价撮合结束后盘口不会处于交叉状态。

### 3.2 撮合核心（单线程）
| 实现 | 吞吐 | 每条消息 |
|---|---|---|
| OrderBook（数组档位 + 位图 + FlatMap） | 9.4–13 M msg/s | 78–106 ns |
| RefBook（std::map + deque） | 2.3–3.0 M msg/s | 330–440 ns |
| 加速比 | **约 4×** | |

单条消息处理延迟：p50 约 180–200 ns，p99 约 0.63–0.74 µs。

### 3.3 非核心逻辑移出热路径（A/B 对比，1 shard，满负载）
| 日志方式 | 吞吐 | publish 阶段 mean / p99 |
|---|---|---|
| 同步：热路径 snprintf + fwrite + mutex | 1.04–1.12 M msg/s | 438–464 ns / 2.3–2.5 µs |
| 异步：40B 记录入 SPSC，mmap 落盘 | 2.4–2.8 M msg/s | 48 ns / 0.29 µs |

异步日志带来约 **2.3–2.5× 吞吐**，publish 阶段的 p99 降低约 **8×**，245 万条日志 0 丢失。（以上是改用 TSC 加直方图之后的数字；改造前用 vector 记录样本时，两者分别是 0.89–0.93 和 1.8–2.1 M msg/s。）

### 3.4 固定负载 100K msg/s 下的阶段延迟（1 shard，异步日志，绑核）
| 阶段 | p50 | p90 | p99 |
|---|---|---|---|
| queue_wait | 0.4–0.6 µs | 1.0–1.6 µs | 0.12–0.7 ms |
| match_core | 0.3 µs | 0.6–0.7 µs | 1.1–1.3 µs |
| publish | 0.1 µs | 0.2 µs | 0.5 µs |
| **e2e** | **≈1 µs** | **≈2 µs** | **0.12–0.7 ms** |

**e2e 尾延迟的来源。** 引擎自身（match_core + publish）的 p99 不到 2 µs，e2e 的 p99 几乎全部来自 queue_wait。`hiccup` 工具在一个空闲、绑核、只空转读时钟的线程上测了 5 秒，平台就出现了约 7,000 次 >10 µs、50–115 次 >100 µs、2–16 次 >1 ms 的停顿，最长 3–4 ms。这些是虚拟机调度和 steal time 造成的，任何程序在这台机器上都躲不开。在物理机上做 `isolcpus`、关中断亲和、关超线程后，尾延迟会显著下降，这是下一步该做的部署层优化。

### 3.5 其他
- 因子：160 个因子 × 45,000 行，C++ 0.055 s，比 Python（numpy 向量化）的 1.5–1.8 s 快约 30 倍，比优化前的 C++ 朴素实现快约 10 倍。
- L2 重建：222 万条逐笔记录，约 0.9 M 条/s，单线程且含 CSV 解析。
- 基差监控：14,400 秒 × 2 个合约，两段注入的异常都被捕获，在正常区间内没有误报，异常结束后各发一条恢复消息。

---

## 4. 工程质量与 CI

每次 push 都会在 GitHub Actions 上运行 `.github/workflows/ci.yml`，包含：
- **build-test**：g++ 和 clang++ 各编译一遍，跑 `scripts/ci_check.sh`（单测、L2 重建对账、差分回放、golden 回归、因子一致性、基差监控）。
- **sanitizers**：分别在 AddressSanitizer + UBSan 和 ThreadSanitizer 下跑全部单测。TSan 专门用来检查 SPSC 队列、异步日志这些无锁路径有没有数据竞争。
- **fuzz**：libFuzzer 跑 60 秒，对拍 OrderBook 和 RefBook。

本地复现：
```
cmake -S . -B build-tsan -DME_SANITIZE=thread  && cmake --build build-tsan --target unit_tests && ./build-tsan/unit_tests
cmake -S . -B build-asan -DME_SANITIZE=address && cmake --build build-asan --target unit_tests && ./build-asan/unit_tests
CXX=clang++ cmake -S . -B build-fuzz -DME_FUZZ=ON && cmake --build build-fuzz --target fuzz_book && ./build-fuzz/fuzz_book -max_total_time=60
```

---

## 5. 目录

```
include/me/      types / flat_map / order_book / ref_book / spsc_queue / consistent_hash
                 latency / async_logger / engine / flow / l2
include/factor/  factor_api.h（插件 C ABI）
src/             engine.cpp / async_logger.cpp
plugins/         momentum / volatility / volume / reversion（各编成 .so）
tools/           gen_flow / exchange_sim / l2_rebuild / replay_diff / factor_runner / hiccup
bench/           bench_book（核心吞吐）/ bench_engine（多线程 + 阶段延迟）
tests/           17 个单测（无第三方依赖）
fuzz/            libFuzzer 目标 + 共用的对拍驱动
python/          factors_ref / compare_factors / gen_bars / basis_monitor / gen_basis_data
scripts/         run_all.sh（全量）/ ci_check.sh（CI 快检）
.github/         GitHub Actions 工作流
```

### 工具用法速查
```
build/gen_flow data/day.flow 100 2000000 42
build/exchange_sim data/day.flow data/ticks.csv data/engine_snapshot.csv
build/l2_rebuild data/ticks.csv --expect data/engine_snapshot.csv --watch 0 --l5 data/l5_sym0.csv
build/replay_diff data/day.flow --record data/golden.trades     # 录制
build/replay_diff data/day.flow --golden data/golden.trades     # 回归
build/bench_engine --shards 2 --log async|sync|none --rate 100000 --pin 1
WECOM_WEBHOOK=https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=xxx python3 python/basis_monitor.py data.csv
```

## 6. 已知简化 / 可扩展方向
- 数据是合成的。真实上交所 L2 逐笔是付费数据，`l2_rebuild` 的输入是一个明确定义的 CSV 格式，接真实数据只需要写一个字段映射。
- 价格笼子只实现了“超出即拒单”，未实现“暂存后再撮合”的方式。
- 未实现：申报数量整手校验、本方最优 / 对手方最优等其他市价单类型、自成交防范、盘中证券迁移、崩溃恢复（WAL + 快照）、深交所逐笔口径。
- L2 重建用的是 std::map 加 CSV 解析，约 0.9 M 条/s。改用数组档位和二进制格式可以明显提速。
- 基差监控的到期日按“合约月第三个周五”计算，未接节假日日历。
- 只有 2 vCPU，无法展示多 shard 的线性扩展；在更多核的机器上用 `--shards N --pin 1` 测试即可。
