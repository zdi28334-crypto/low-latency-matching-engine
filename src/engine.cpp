#include "me/engine.h"

#include <pthread.h>
#include <sched.h>

#include <stdexcept>

#include "me/latency.h"

namespace me {

bool pin_current_thread(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

namespace {
constexpr SymbolId kStopSym = 0xFFFFFFFFu;
}

struct Engine::Shard {
    int id;
    SpscQueue<OrderMsg> inbox;
    std::vector<OrderBook> books;
    std::thread th;
    ShardStats st;
    Shard(int i, size_t cap) : id(i), inbox(cap) {}
};

// Collects a message's events during matching; published after the match
// so the two stages can be timed separately.
struct EventBuffer {
    std::vector<LogRecord> ev;
    ShardStats* st;
    SymbolId sym = 0;
    int64_t ts = 0;
    void push(char kind, char bs, uint64_t a, uint64_t b, int32_t px, int32_t q) {
        ev.push_back(LogRecord{ts, sym, kind, bs, 0, a, b, px, q});
    }
    void on_trade(const Trade& t) {
        ++st->trades; st->traded_qty += t.qty;
        push('T', t.bs_flag, t.buy_id, t.sell_id, t.price, t.qty);
    }
    void on_rest(OrderId id, Side s, Price p, Qty q, uint32_t) {
        ++st->rests; push('A', s == Side::Buy ? 'B' : 'S', id, 0, p, q);
    }
    void on_cancel(OrderId id, Side s, Price p, Qty q, uint32_t) {
        ++st->cancels; push('D', s == Side::Buy ? 'B' : 'S', id, 0, p, q);
    }
    void on_expire(OrderId id, Side s, Qty q, ExpireReason r, uint32_t) {
        ++st->expires; push('X', s == Side::Buy ? 'B' : 'S', id, uint64_t(r), 0, q);
    }
    void on_reject(OrderId id, RejectReason r) { ++st->rejects; push('R', 0, id, uint64_t(r), 0, 0); }
    void on_phase(Phase, uint32_t) {}
};

Engine::Engine(const std::vector<SymbolInfo>& symbols, EngineOptions opt) : opt_(std::move(opt)), symbols_(symbols) {
    if (opt_.shards < 1) throw std::invalid_argument("shards >= 1");
    ConsistentHashRing ring(opt_.shards);
    for (int i = 0; i < opt_.shards; ++i) shards_.push_back(std::make_unique<Shard>(i, opt_.queue_capacity));
    route_.resize(symbols_.size());
    local_index_.resize(symbols_.size());
    for (const SymbolInfo& s : symbols_) {
        if (s.id >= symbols_.size()) throw std::invalid_argument("symbol ids must be dense 0..N-1");
        int k = ring.shard_of(s.code);
        route_[s.id] = k;
        local_index_[s.id] = static_cast<int>(shards_[k]->books.size());
        shards_[k]->books.emplace_back(s, 1 << 14, opt_.book);
    }
    (void)TscClock::now();  // calibrate before any thread starts
    if (opt_.log_mode == LogMode::Async) {
        logger_ = std::make_unique<AsyncLogger>(opt_.log_path, opt_.shards, 1 << 20);
    } else if (opt_.log_mode == LogMode::Sync) {
        sync_fp_ = std::fopen(opt_.log_path.c_str(), "w");
        if (!sync_fp_) throw std::runtime_error("open " + opt_.log_path);
    }
}

Engine::~Engine() {
    if (started_ && !finished_) finish();
    if (sync_fp_) std::fclose(sync_fp_);
}

void Engine::start() {
    if (logger_) logger_->start(opt_.logger_cpu);
    for (auto& sh : shards_) {
        Shard* p = sh.get();
        int cpu = p->id < static_cast<int>(opt_.shard_cpus.size()) ? opt_.shard_cpus[p->id] : -1;
        p->th = std::thread([this, p, cpu] {
            if (cpu >= 0 && !pin_current_thread(cpu)) std::fprintf(stderr, "warn: cannot pin shard %d\n", p->id);
            run_shard(*p);
        });
    }
    started_ = true;
}

void Engine::submit(OrderMsg m) {
    m.ts = static_cast<int64_t>(TscClock::now());
    shards_[route_[m.sym]]->inbox.push(m);
}

void Engine::finish() {
    if (finished_) return;
    OrderMsg stop;
    stop.sym = kStopSym;
    for (auto& sh : shards_) sh->inbox.push(stop);
    for (auto& sh : shards_) sh->th.join();
    if (logger_) logger_->stop();
    if (sync_fp_) std::fflush(sync_fp_);
    finished_ = true;
}

void Engine::run_shard(Shard& sh) {
    EventBuffer buf;
    buf.st = &sh.st;
    buf.ev.reserve(256);
    char line[128];
    const bool lat = opt_.record_latency;
    OrderMsg m;
    unsigned idle = 0;
    for (;;) {
        if (!sh.inbox.try_pop(m)) {
            if (++idle < 512) ME_CPU_RELAX(); else std::this_thread::yield();
            continue;
        }
        idle = 0;
        if (m.sym == kStopSym) break;
        const uint64_t t_deq = lat ? TscClock::now() : 0;

        buf.ev.clear();
        buf.sym = m.sym;
        buf.ts = m.ts;
        sh.books[local_index_[m.sym]].process(m, buf);
        const uint64_t t_match = lat ? TscClock::now() : 0;

        // ---- publish stage
        switch (opt_.log_mode) {
            case LogMode::Async:
                for (const LogRecord& r : buf.ev) logger_->log(sh.id, r);
                break;
            case LogMode::Sync: {
                std::lock_guard<std::mutex> g(sync_mu_);
                for (const LogRecord& r : buf.ev) {
                    int n = format_record(r, line);
                    std::fwrite(line, 1, n, sync_fp_);
                    ++sync_written_;
                }
                break;
            }
            case LogMode::None: break;
        }
        ++sh.st.msgs;
        if (lat) {
            const uint64_t t_pub = TscClock::now();
            const uint64_t t_in = static_cast<uint64_t>(m.ts);
            // ingress is stamped on another core: clamp tiny cross-core skew
            sh.st.queue_wait.record(t_deq > t_in ? t_deq - t_in : 0);
            sh.st.match_core.record(t_match - t_deq);
            sh.st.publish.record(t_pub - t_match);
            sh.st.e2e.record(t_pub > t_in ? t_pub - t_in : 0);
        }
    }
}

const ShardStats& Engine::stats(int shard) const { return shards_.at(shard)->st; }

const OrderBook& Engine::book(SymbolId s) const { return shards_[route_[s]]->books[local_index_[s]]; }

uint64_t Engine::log_written() const { return logger_ ? logger_->written() : sync_written_; }

}  // namespace me
