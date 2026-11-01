// Multi-threaded sharded matching engine.
//
//   gateway thread ──route(sym)──► SPSC[shard k] ──► shard k thread
//                                                    ├─ OrderBook per security (single owner)
//                                                    └─ events ─► logger ring ─► logger thread (mmap)
//
//  * Routing: consistent hash on the security code -> shard. One security is
//    owned by exactly one thread, and its messages flow through a single FIFO
//    queue, so per-security order sequence is preserved without locks while
//    different securities are matched in parallel.
//  * Stage-level latency, recorded per message:
//      queue_wait = dequeue      - gateway ingress
//      match_core = match done   - dequeue        (pure book work)
//      publish    = events out   - match done     (logging / output)
//      e2e        = events out   - gateway ingress
//  * Logging modes (for the "move non-core work off the hot path" A/B test):
//      Async: 40-byte record into an SPSC ring, formatted by a logger thread.
//      Sync : snprintf + fwrite under a mutex inside the matching thread.
#pragma once
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "me/async_logger.h"
#include "me/consistent_hash.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/spsc_queue.h"
#include "me/types.h"

namespace me {

enum class LogMode { None, Sync, Async };

struct EngineOptions {
    int shards = 2;
    size_t queue_capacity = 1 << 16;
    BookConfig book;  // venue, self-trade prevention, price cage
    LogMode log_mode = LogMode::Async;
    std::string log_path = "engine.log";
    bool record_latency = true;
    // CPU pinning (empty / -1 = let the OS schedule). Pinning keeps matching
    // threads from being preempted by the logger or migrated between cores,
    // which is what dominates tail latency on small machines.
    std::vector<int> shard_cpus;
    int logger_cpu = -1;
};

// Pin the calling thread to one CPU; returns false if not permitted.
bool pin_current_thread(int cpu);

// Latency histograms are in TscClock ticks; convert with TscClock::ns_per_tick().
struct ShardStats {
    uint64_t msgs = 0, trades = 0, rests = 0, cancels = 0, expires = 0, rejects = 0;
    int64_t traded_qty = 0;
    LatencyHistogram queue_wait, match_core, publish, e2e;
};

class Engine {
public:
    Engine(const std::vector<SymbolInfo>& symbols, EngineOptions opt);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    // Gateway side (single producer thread): stamps ingress time and routes.
    void submit(OrderMsg m);
    // Stop shards after they drained their queues; stop the logger.
    void finish();

    int shard_of(SymbolId s) const { return route_[s]; }
    int shards() const { return static_cast<int>(shards_.size()); }
    const ShardStats& stats(int shard) const;
    const OrderBook& book(SymbolId s) const;
    uint64_t log_dropped() const { return logger_ ? logger_->dropped() : 0; }
    uint64_t log_written() const;

private:
    struct Shard;
    void run_shard(Shard& sh);

    EngineOptions opt_;
    std::vector<SymbolInfo> symbols_;
    std::vector<int> route_;        // SymbolId -> shard
    std::vector<int> local_index_;  // SymbolId -> index within its shard
    std::vector<std::unique_ptr<Shard>> shards_;
    std::unique_ptr<AsyncLogger> logger_;
    FILE* sync_fp_ = nullptr;
    std::mutex sync_mu_;
    uint64_t sync_written_ = 0;
    bool started_ = false, finished_ = false;
};

}  // namespace me
