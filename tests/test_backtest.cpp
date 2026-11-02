// Backtest fill-simulation tests. Each scenario is a tiny hand-written order
// flow plus scripted own orders; the queue model running on the recorded feed
// must produce exactly the fills of the ground truth (own orders inserted into
// the real matching engine), for both SSE- and SZSE-style feeds.
#include "me/backtest.h"
#include "test_util.h"

using namespace me;
using namespace me::bt;

namespace {

struct Scripted {
    struct Ord { int64_t tick; Side side; Price px; Qty qty; bool take; };
    std::vector<Ord> orders;
    template <class Emit>
    void on_timer(int64_t tick, SymbolId, const View&, Qty, Emit&& emit) const {
        for (size_t i = 0; i < orders.size(); ++i)
            if (orders[i].tick == tick) emit(kOwnBase + i, orders[i].side, orders[i].px, orders[i].qty, orders[i].take);
    }
};

// 09:30:SS.mmm
uint32_t at(int sec, int ms = 0) { return 93000000 + uint32_t(sec) * 1000 + uint32_t(ms); }

struct FlowBuilder {
    Flow f;
    OrderId next = 1;
    FlowBuilder() {
        f.symbols.push_back(make_symbol(0, 1000));  // 10.00, band 9.00 - 11.00
        OrderMsg m; m.type = MsgType::Phase; m.phase = Phase::Continuous; m.time = at(0);
        f.msgs.push_back(m);
    }
    OrderId limit(uint32_t t, Side s, Price px, Qty q) {
        OrderMsg m; m.id = next++; m.side = s; m.price = px; m.qty = q; m.time = t;
        f.msgs.push_back(m);
        return m.id;
    }
    void cancel(uint32_t t, OrderId id) {
        OrderMsg m; m.type = MsgType::Cancel; m.id = id; m.time = t;
        f.msgs.push_back(m);
    }
    void close(uint32_t t) {
        OrderMsg m; m.type = MsgType::Phase; m.phase = Phase::Closed; m.time = t;
        f.msgs.push_back(m);
    }
};

Params test_params() {
    Params p;
    p.start_ms = to_ms(at(0));
    p.interval_ms = 1000;  // tick k fires at 09:30:(k+1).000
    p.latency_ms = 0;
    p.ttl_ms = 3600000;
    p.end_ms = to_ms(at(50));
    return p;
}

struct Outcome {
    std::vector<Qty> filled;
    std::vector<int64_t> first_fill_ms;
};

Outcome outcome(const Result& r, size_t n) {
    Outcome o{std::vector<Qty>(n, 0), std::vector<int64_t>(n, -1)};
    for (size_t i = 0; i < n; ++i) {
        auto it = r.orders.find(kOwnBase + i);
        if (it != r.orders.end()) o.filled[i] = it->second.filled;
    }
    for (const FillRec& f : r.fills) {
        size_t i = f.id - kOwnBase;
        if (o.first_fill_ms[i] < 0) o.first_fill_ms[i] = f.t_ms;
    }
    return o;
}

// Runs truth + all three models on both feed styles; returns per-model outcomes.
struct AllOutcomes { Outcome truth, queue, touch, through; };

AllOutcomes run_all(const Flow& f, const Scripted& s, Venue v) {
    BookConfig cfg;
    cfg.venue = v;
    Params p = test_params();
    const size_t n = s.orders.size();
    AllOutcomes a;
    a.truth = outcome(run_truth(f, cfg, s, p), n);
    auto feed = make_feed(f, cfg);
    a.queue = outcome(run_sim(feed, f.symbols.size(), v, FillModel::Queue, s, p), n);
    a.touch = outcome(run_sim(feed, f.symbols.size(), v, FillModel::Touch, s, p), n);
    a.through = outcome(run_sim(feed, f.symbols.size(), v, FillModel::Through, s, p), n);
    return a;
}

}  // namespace

// Queue position: we join behind 500 shares. The first sell only eats orders
// ahead of us; the second sell trades an order that arrived after us, which
// in reality would have hit us first.
TEST(backtest_queue_position) {
    for (Venue v : {Venue::SSE, Venue::SZSE}) {
        FlowBuilder b;
        b.limit(at(0, 100), Side::Buy, 1000, 300);
        b.limit(at(0, 200), Side::Buy, 1000, 200);
        // own bid 100 @10.00 at 09:30:01
        b.limit(at(2), Side::Sell, 1000, 400);  // eats 300 + 100 ahead of us
        b.limit(at(3), Side::Buy, 1000, 100);   // joins behind us
        b.limit(at(4), Side::Sell, 1000, 100);  // eats the last 100 ahead of us
        b.limit(at(5), Side::Sell, 1000, 150);  // our turn
        b.close(at(9));
        Scripted s{{{0, Side::Buy, 1000, 100, false}}};
        AllOutcomes a = run_all(b.f, s, v);
        CHECK_EQ(a.truth.filled[0], 100);
        CHECK_EQ(a.truth.first_fill_ms[0], to_ms(at(5)));
        CHECK_EQ(a.queue.filled[0], 100);
        CHECK_EQ(a.queue.first_fill_ms[0], to_ms(at(5)));   // same time as reality
        CHECK_EQ(a.touch.first_fill_ms[0], to_ms(at(2)));   // touch fills far too early
        // No trade below 10.00, so "through" only gets the 50 shares of the last
        // sell that came to rest at our price: it under-fills.
        CHECK_EQ(a.through.filled[0], 50);
    }
}

// An order ahead of us is cancelled; later an incoming sell comes to rest at
// our price in the recorded market (nobody else bids there) -> it would have
// traded with us.
TEST(backtest_cancel_ahead_and_resting_cross) {
    for (Venue v : {Venue::SSE, Venue::SZSE}) {
        FlowBuilder b;
        OrderId a1 = b.limit(at(0, 100), Side::Buy, 1000, 300);
        b.limit(at(0, 500), Side::Buy, 999, 500);
        b.cancel(at(2), a1);
        b.limit(at(3), Side::Sell, 1000, 60);  // rests at 10.00 without us
        b.limit(at(4), Side::Buy, 1000, 60);   // in the recorded market this takes it
        b.close(at(9));
        Scripted s{{{0, Side::Buy, 1000, 100, false}}};
        AllOutcomes a = run_all(b.f, s, v);
        CHECK_EQ(a.truth.filled[0], 60);
        CHECK_EQ(a.queue.filled[0], 60);
        CHECK_EQ(a.queue.first_fill_ms[0], to_ms(at(3)));
        CHECK_EQ(a.through.filled[0], 60);  // resting-cross is model independent
    }
}

// An aggressive sell sweeps through our level down to 9.98.
TEST(backtest_sweep_through) {
    for (Venue v : {Venue::SSE, Venue::SZSE}) {
        FlowBuilder b;
        b.limit(at(0, 100), Side::Buy, 1000, 300);
        b.limit(at(0, 200), Side::Buy, 999, 500);
        b.limit(at(2), Side::Sell, 998, 700);
        b.close(at(9));
        Scripted s{{{0, Side::Buy, 1000, 100, false}}};
        AllOutcomes a = run_all(b.f, s, v);
        CHECK_EQ(a.truth.filled[0], 100);
        CHECK_EQ(a.queue.filled[0], 100);
        CHECK_EQ(a.through.filled[0], 100);
    }
}

// Own marketable buy takes visible liquidity over two levels; on the way it
// reaches our own resting ask, which self-trade prevention cancels.
TEST(backtest_take_liquidity_with_stp) {
    for (Venue v : {Venue::SSE, Venue::SZSE}) {
        FlowBuilder b;
        b.limit(at(0, 100), Side::Sell, 1001, 200);
        // own ask 100 @10.01 at 09:30:01 (behind the 200)
        b.limit(at(1, 500), Side::Sell, 1002, 500);
        // own buy 300 @10.02 at 09:30:02
        b.close(at(9));
        Scripted s{{{0, Side::Sell, 1001, 100, false}, {1, Side::Buy, 1002, 300, true}}};
        AllOutcomes a = run_all(b.f, s, v);
        CHECK_EQ(a.truth.filled[0], 0);    // cancelled by STP
        CHECK_EQ(a.truth.filled[1], 300);  // 200 @10.01 + 100 @10.02
        CHECK_EQ(a.queue.filled[0], 0);
        CHECK_EQ(a.queue.filled[1], 300);
    }
}

// Taking twice must not reuse the same visible liquidity.
TEST(backtest_no_double_take) {
    FlowBuilder b;
    b.limit(at(0, 100), Side::Sell, 1001, 150);
    b.close(at(9));
    Scripted s{{{0, Side::Buy, 1001, 100, true}, {1, Side::Buy, 1001, 100, true}}};
    AllOutcomes a = run_all(b.f, s, Venue::SSE);
    CHECK_EQ(a.truth.filled[0] + a.truth.filled[1], 150);
    CHECK_EQ(a.queue.filled[0], 100);
    CHECK_EQ(a.queue.filled[1], 50);  // the second only finds 50 left
}

// A-share T+1: with a base position of 200 shares the strategy can sell at
// most 200 shares today, whatever it buys.
TEST(backtest_t_plus_1_base_position) {
    FlowParams fp{2, 40000, 8};
    Flow f = generate_flow(fp);
    anonymize(f);
    Params p;
    p.base_position = 200;
    p.interval_ms = 2000;
    JoinBestStrategy st{0.2};
    BookConfig cfg;
    Result r = run_sim(make_feed(f, cfg), f.symbols.size(), Venue::SSE, FillModel::Queue, st, p);
    std::vector<int64_t> sold(f.symbols.size(), 0);
    for (const FillRec& x : r.fills) if (x.side == Side::Sell) sold[x.sym] += x.qty;
    for (int64_t s : sold) CHECK(s <= 200);
    CHECK(r.bought > 0);
}
