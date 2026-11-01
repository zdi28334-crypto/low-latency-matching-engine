// Concurrency, sharding and multi-threaded engine correctness tests.
#include <cstdio>
#include <thread>

#include "me/consistent_hash.h"
#include "me/engine.h"
#include "me/flow.h"
#include "me/spsc_queue.h"
#include "test_util.h"

using namespace me;

TEST(spsc_order_preserved_across_threads) {
    SpscQueue<uint64_t> q(1024);
    const uint64_t N = 2'000'000;
    std::thread prod([&] { for (uint64_t i = 1; i <= N; ++i) q.push(i); });
    uint64_t expect = 1, v;
    bool ok = true;
    while (expect <= N) {
        if (q.try_pop(v)) { if (v != expect) ok = false; ++expect; }
    }
    prod.join();
    CHECK(ok);
}

TEST(consistent_hash_balance_and_remap) {
    const int nsym = 2000;
    ConsistentHashRing r4(4), r5(5);
    int cnt[4] = {0}, moved = 0;
    char code[8];
    for (int i = 0; i < nsym; ++i) {
        std::snprintf(code, sizeof code, "%06d", 600000 + i);
        int a = r4.shard_of(code), b = r5.shard_of(code);
        ++cnt[a];
        if (a != b) ++moved;
        CHECK_EQ(a, r4.shard_of(code));  // deterministic
    }
    for (int c : cnt) CHECK(c > nsym / 4 * 0.75 && c < nsym / 4 * 1.25);  // within ±25% of fair share
    double frac = double(moved) / nsym;
    std::printf("    4->5 shards remapped %.1f%% of securities (ideal 20%%)\n", frac * 100);
    CHECK(frac < 0.30);
}

// The sharded engine must give every security exactly the same book as a
// single-threaded run: sharding must not change matching results.
TEST(engine_sharded_equals_single_thread) {
    Flow f = generate_flow({40, 200000, 2024});
    std::vector<OrderBook> ref;
    struct Null {
        int64_t trades = 0;
        void on_trade(const Trade&) { ++trades; }
        void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
        void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
        void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}
        void on_reject(OrderId, RejectReason) {}
        void on_phase(Phase, uint32_t) {}
    } nl;
    for (auto& s : f.symbols) ref.emplace_back(s);
    for (auto& m : f.msgs) ref[m.sym].process(m, nl);

    EngineOptions opt;
    opt.shards = 3;
    opt.log_mode = LogMode::Async;
    opt.log_path = "/tmp/me_test_engine.log";
    opt.queue_capacity = 1024;  // small queue: exercises back-pressure
    Engine eng(f.symbols, opt);
    eng.start();
    for (auto& m : f.msgs) eng.submit(m);
    eng.finish();

    int64_t trades = 0, logged_expected = 0;
    uint64_t msgs = 0;
    for (int k = 0; k < eng.shards(); ++k) {
        const ShardStats& s = eng.stats(k);
        trades += s.trades; msgs += s.msgs;
        logged_expected += s.trades + s.rests + s.cancels + s.expires + s.rejects;
    }
    CHECK_EQ(msgs, f.msgs.size());
    CHECK_EQ(trades, nl.trades);
    CHECK_EQ(eng.log_written() + eng.log_dropped(), uint64_t(logged_expected));
    for (auto& s : f.symbols) {
        const OrderBook& b = eng.book(s.id);
        int mismatches = 0;
        ref[s.id].for_each_level([&](Side sd, Price p, int64_t q, uint32_t) {
            if (b.level_qty(sd, p) != q) ++mismatches;
        });
        CHECK_EQ(mismatches, 0);
        CHECK_EQ(b.live_orders(), ref[s.id].live_orders());
    }
    std::remove("/tmp/me_test_engine.log");
}
