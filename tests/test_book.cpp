// Order book, call auction, flat map, differential and L2 consistency tests.
#include <random>
#include <unordered_map>

#include "../fuzz/fuzz_common.h"
#include "me/flat_map.h"
#include "me/flow.h"
#include "me/l2.h"
#include "me/order_book.h"
#include "me/ref_book.h"
#include "test_util.h"

using namespace me;

namespace {

struct Rec {
    std::vector<Trade> trades;
    std::vector<std::pair<OrderId, Qty>> rests, cancels, expires;
    std::vector<Price> rest_px;
    std::vector<ExpireReason> expire_why;
    std::vector<RejectReason> rejects;
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_rest(OrderId id, Side, Price p, Qty q, uint32_t) { rests.push_back({id, q}); rest_px.push_back(p); }
    void on_cancel(OrderId id, Side, Price, Qty q, uint32_t) { cancels.push_back({id, q}); }
    void on_expire(OrderId id, Side, Qty q, ExpireReason why, uint32_t) {
        expires.push_back({id, q});
        expire_why.push_back(why);
    }
    void on_reject(OrderId, RejectReason r) { rejects.push_back(r); }
    void on_phase(Phase, uint32_t) {}
};

SymbolInfo sym10() { return make_symbol(0, 1000); }  // prev close 10.00, band 9.00-11.00

OrderMsg lim(OrderId id, Side s, Price p, Qty q, AccountId acct = 0) {
    OrderMsg m; m.id = id; m.side = s; m.price = p; m.qty = q; m.account = acct; return m;
}
OrderMsg mkt(OrderId id, Side s, Qty q, OrdType t = OrdType::MarketBest5IOC, AccountId acct = 0) {
    OrderMsg m; m.id = id; m.side = s; m.qty = q; m.ord_type = t; m.account = acct; return m;
}
OrderMsg cxl(OrderId id) { OrderMsg m; m.type = MsgType::Cancel; m.id = id; return m; }
OrderMsg phase(Phase p) { OrderMsg m; m.type = MsgType::Phase; m.phase = p; return m; }

}  // namespace

TEST(price_time_priority) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Sell, 1000, 100), r);
    b.process(lim(2, Side::Sell, 1000, 100), r);
    b.process(lim(3, Side::Sell, 999, 100), r);   // better price, arrives later
    b.process(lim(4, Side::Buy, 1000, 250), r);
    CHECK_EQ(r.trades.size(), 3u);
    CHECK_EQ(r.trades[0].sell_id, 3u);  // best price first
    CHECK_EQ(r.trades[0].price, 999);
    CHECK_EQ(r.trades[1].sell_id, 1u);  // then FIFO at 10.00
    CHECK_EQ(r.trades[2].sell_id, 2u);
    CHECK_EQ(r.trades[2].qty, 50);
    CHECK_EQ(b.level_qty(Side::Sell, 1000), 50);
    CHECK(r.trades[0].bs_flag == 'B');
}

TEST(partial_fill_rests_residual) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Sell, 1001, 300), r);
    b.process(lim(2, Side::Buy, 1002, 500), r);
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 1001);  // trades at the resting price
    CHECK_EQ(r.rests.back().first, 2u);
    CHECK_EQ(r.rests.back().second, 200);
    CHECK(b.has_bid());
    CHECK_EQ(b.best_bid(), 1002);
    CHECK(!b.has_ask());
}

TEST(cancel_and_unknown_cancel) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Buy, 995, 100), r);
    b.process(lim(2, Side::Buy, 998, 100), r);
    CHECK_EQ(b.best_bid(), 998);
    b.process(cxl(2), r);
    CHECK_EQ(b.best_bid(), 995);  // bitmap finds the next level
    b.process(cxl(2), r);
    CHECK_EQ(r.rejects.size(), 1u);
    CHECK(r.rejects[0] == RejectReason::UnknownOrder);
    b.process(cxl(1), r);
    CHECK(!b.has_bid());
    CHECK_EQ(b.live_orders(), 0u);
}

TEST(market_best5_ioc) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    for (int i = 0; i < 6; ++i) b.process(lim(10 + i, Side::Sell, 1000 + i, 100), r);
    b.process(mkt(99, Side::Buy, 10000), r);
    CHECK_EQ(r.trades.size(), 5u);               // stops after 5 levels
    CHECK_EQ(r.trades.back().price, 1004);
    CHECK_EQ(b.best_ask(), 1005);
    CHECK(!b.has_bid());                          // remainder cancelled, never rests
}

TEST(price_band_and_phase_rejects) {
    OrderBook b(sym10()); Rec r;
    b.process(lim(1, Side::Buy, 1000, 100), r);  // Closed
    b.process(phase(Phase::OpeningAuction), r);
    b.process(lim(2, Side::Buy, 1101, 100), r);  // above 涨停
    b.process(mkt(3, Side::Buy, 100), r);        // market not allowed in auction
    b.process(lim(4, Side::Buy, 1000, 0), r);
    CHECK_EQ(r.rejects.size(), 4u);
    CHECK(r.rejects[0] == RejectReason::Closed);
    CHECK(r.rejects[1] == RejectReason::OutOfBand);
    CHECK(r.rejects[2] == RejectReason::MarketInAuction);
    CHECK(r.rejects[3] == RejectReason::BadQty);
}

// Worked example — candidate 10.01 has max volume but violates rule (2)
// (1500 shares bid above it would stay unfilled), so 10.03 wins.
TEST(call_auction_rules) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::OpeningAuction), r);
    b.process(lim(1, Side::Buy, 1005, 1000), r);
    b.process(lim(2, Side::Buy, 1003, 500), r);
    b.process(lim(3, Side::Buy, 1000, 800), r);
    b.process(lim(4, Side::Sell, 998, 600), r);
    b.process(lim(5, Side::Sell, 1001, 700), r);
    b.process(lim(6, Side::Sell, 1004, 900), r);
    CHECK(r.trades.empty());  // nothing trades during the auction
    AuctionResult ar = b.auction_price();
    CHECK(ar.crossed);
    CHECK_EQ(ar.price, 1003);
    CHECK_EQ(ar.volume, 1300);
    b.process(phase(Phase::Continuous), r);
    int64_t vol = 0;
    for (auto& t : r.trades) { vol += t.qty; CHECK_EQ(t.price, 1003); CHECK(t.bs_flag == 'N'); }
    CHECK_EQ(vol, 1300);
    CHECK_EQ(r.trades[0].buy_id, 1u);   // highest bid first
    CHECK_EQ(r.trades[0].sell_id, 4u);  // lowest ask first
    CHECK(b.best_bid() < b.best_ask()); // book uncrossed afterwards
}

TEST(call_auction_midpoint) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::OpeningAuction), r);
    b.process(lim(1, Side::Buy, 1002, 500), r);
    b.process(lim(2, Side::Sell, 1000, 500), r);
    AuctionResult ar = b.auction_price();
    CHECK_EQ(ar.price, 1001);  // 10.00 and 10.02 tie on volume and imbalance
    CHECK_EQ(ar.imbalance, 0);
    b.process(phase(Phase::Continuous), r);
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 1001);
}

TEST(call_auction_no_cross) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::OpeningAuction), r);
    b.process(lim(1, Side::Buy, 999, 500), r);
    b.process(lim(2, Side::Sell, 1000, 500), r);
    CHECK(!b.auction_price().crossed);
    b.process(phase(Phase::Continuous), r);
    CHECK(r.trades.empty());
}

TEST(price_cage_percent_rule) {
    OrderBook b(sym10()); Rec r;  // prev close 10.00
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Sell, 1000, 100), r);   // buy base = best ask 10.00
    b.process(lim(2, Side::Buy, 1021, 100), r);    // > 10.20 -> rejected
    CHECK_EQ(r.rejects.size(), 1u);
    CHECK(r.rejects[0] == RejectReason::PriceCage);
    b.process(lim(3, Side::Buy, 1020, 100), r);    // == 102% -> accepted, trades at 10.00
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 1000);
    b.process(lim(4, Side::Buy, 990, 100), r);     // sell base = best bid 9.90
    b.process(lim(5, Side::Sell, 970, 100), r);    // < 9.90*98% = 9.702 -> rejected
    b.process(lim(6, Side::Sell, 971, 100), r);    // accepted
    CHECK_EQ(r.rejects.size(), 2u);
    CHECK(r.rejects[1] == RejectReason::PriceCage);
    CHECK_EQ(r.trades.size(), 2u);
}

TEST(price_cage_ten_tick_floor_and_fallbacks) {
    OrderBook b(make_symbol(1, 200)); Rec r;  // 2.00 stock: 2% = 4 ticks < 10 ticks
    b.process(phase(Phase::OpeningAuction), r);
    b.process(lim(1, Side::Buy, 220, 100), r);     // no cage during the auction
    CHECK(r.rejects.empty());
    b.process(cxl(1), r);
    b.process(phase(Phase::Continuous), r);
    // empty book -> base = last price (= prev close 2.00): limit is 2.10
    b.process(lim(2, Side::Buy, 211, 100), r);
    b.process(lim(3, Side::Buy, 210, 100), r);
    CHECK_EQ(r.rejects.size(), 1u);
    CHECK_EQ(r.rests.back().first, 3u);
    // buy base falls back to best bid (2.10) when there is no ask
    b.process(lim(4, Side::Buy, 220, 100), r);     // 2.10 + 10 ticks = 2.20 -> ok
    CHECK_EQ(r.rejects.size(), 1u);
    // market orders are not caged
    b.process(mkt(5, Side::Sell, 50), r);
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(b.last_price(), 220);
}

// ---------------------------------------------------------- self-trade prevention
// Book: sell #1 (acct 8) @10.00, sell #2 (acct 7) @10.00, sell #3 (acct 8) @10.01.
// Incoming: buy 300 @10.01 from acct 7, which meets its own order #2.
static void stp_setup(OrderBook& b, Rec& r) {
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Sell, 1000, 100, 8), r);
    b.process(lim(2, Side::Sell, 1000, 100, 7), r);
    b.process(lim(3, Side::Sell, 1001, 100, 8), r);
    b.process(lim(9, Side::Buy, 1001, 300, 7), r);
}

TEST(stp_cancel_newest) {
    BookConfig cfg; cfg.stp = StpPolicy::CancelNewest;
    OrderBook b(sym10(), 64, cfg); Rec r;
    stp_setup(b, r);
    CHECK_EQ(r.trades.size(), 1u);                 // only #1
    CHECK_EQ(r.expires.size(), 1u);
    CHECK_EQ(r.expires[0].second, 200);            // remainder of #9 dropped
    CHECK(r.expire_why[0] == ExpireReason::SelfTrade);
    CHECK_EQ(b.level_qty(Side::Sell, 1000), 100);  // #2 untouched
    CHECK_EQ(b.level_qty(Side::Sell, 1001), 100);  // #3 untouched
    CHECK(!b.has_bid());
}

TEST(stp_cancel_oldest) {
    BookConfig cfg; cfg.stp = StpPolicy::CancelOldest;
    OrderBook b(sym10(), 64, cfg); Rec r;
    stp_setup(b, r);
    CHECK_EQ(r.trades.size(), 2u);                 // #1, then #3 after skipping #2
    CHECK_EQ(r.trades[1].sell_id, 3u);
    CHECK_EQ(r.cancels.size(), 1u);
    CHECK_EQ(r.cancels[0].first, 2u);              // resting own order cancelled
    CHECK_EQ(b.best_bid(), 1001);                  // remainder 100 rests
    CHECK_EQ(b.level_qty(Side::Buy, 1001), 100);
    CHECK(!b.has_ask());
}

TEST(stp_cancel_both_and_disabled) {
    BookConfig cfg; cfg.stp = StpPolicy::CancelBoth;
    OrderBook b(sym10(), 64, cfg); Rec r;
    stp_setup(b, r);
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.cancels.size(), 1u);
    CHECK_EQ(r.expires.size(), 1u);
    CHECK_EQ(b.level_qty(Side::Sell, 1000), 0);
    CHECK_EQ(b.level_qty(Side::Sell, 1001), 100);

    OrderBook b2(sym10()); Rec r2;                 // default: no STP
    stp_setup(b2, r2);
    CHECK_EQ(r2.trades.size(), 3u);
    CHECK_EQ(r2.trades[1].sell_id, 2u);            // self-trade allowed

    BookConfig c3; c3.stp = StpPolicy::CancelNewest;
    OrderBook b3(sym10(), 64, c3); Rec r3;         // account 0 is never checked
    b3.process(phase(Phase::Continuous), r3);
    b3.process(lim(1, Side::Sell, 1000, 100, 0), r3);
    b3.process(lim(2, Side::Buy, 1000, 100, 0), r3);
    CHECK_EQ(r3.trades.size(), 1u);
}

// ---------------------------------------------------------- market order types
TEST(market_best5_to_limit) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(mkt(1, Side::Buy, 100, OrdType::MarketBest5ToLimit), r);  // empty book
    CHECK(r.rejects.back() == RejectReason::NoCounterparty);
    b.process(lim(2, Side::Sell, 1000, 100), r);
    b.process(lim(3, Side::Sell, 1001, 100), r);
    b.process(mkt(4, Side::Buy, 300, OrdType::MarketBest5ToLimit), r);
    CHECK_EQ(r.trades.size(), 2u);
    CHECK_EQ(r.rests.back().first, 4u);
    CHECK_EQ(r.rests.back().second, 100);
    CHECK_EQ(r.rest_px.back(), 1001);              // remainder rests at the last fill price
    CHECK_EQ(b.best_bid(), 1001);
}

TEST(own_best_and_counter_best) {
    OrderBook b(sym10()); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(mkt(1, Side::Buy, 100, OrdType::OwnBest), r);      // no own bid
    b.process(mkt(2, Side::Buy, 100, OrdType::CounterBest), r);  // no ask
    CHECK_EQ(r.rejects.size(), 2u);
    CHECK(r.rejects[0] == RejectReason::NoCounterparty);
    b.process(lim(3, Side::Buy, 995, 100), r);
    b.process(mkt(4, Side::Buy, 200, OrdType::OwnBest), r);      // joins the best bid
    CHECK_EQ(r.rest_px.back(), 995);
    CHECK_EQ(b.level_qty(Side::Buy, 995), 300);
    b.process(lim(5, Side::Sell, 1000, 100), r);
    b.process(lim(6, Side::Sell, 1002, 100), r);
    b.process(mkt(7, Side::Buy, 300, OrdType::CounterBest), r);  // limit at best ask 10.00
    CHECK_EQ(r.trades.size(), 1u);
    CHECK_EQ(r.trades[0].price, 1000);
    CHECK_EQ(r.rest_px.back(), 1000);                            // remainder rests at 10.00
    CHECK_EQ(b.level_qty(Side::Buy, 1000), 200);
    CHECK_EQ(b.level_qty(Side::Sell, 1002), 100);                // 10.02 not touched
    b.process(mkt(8, Side::Sell, 600, OrdType::MarketBest5IOC), r);  // bids hold 500
    CHECK_EQ(r.expires.size(), 1u);
    CHECK(!r.expire_why.empty() && r.expire_why.back() == ExpireReason::IOC);
    CHECK(!r.expires.empty() && r.expires.back().second == 100);
}

// ---------------------------------------------------------- SZSE auction tie-break
TEST(szse_auction_closest_to_reference) {
    BookConfig sz; sz.venue = Venue::SZSE;
    // Opening auction: 9.97 and 10.04 tie on volume and imbalance.
    // SSE takes the midpoint (10.01); SZSE takes the one closest to the
    // previous close 10.00, i.e. 9.97.
    for (Venue v : {Venue::SSE, Venue::SZSE}) {
        BookConfig c; c.venue = v;
        OrderBook b(sym10(), 64, c); Rec r;
        b.process(phase(Phase::OpeningAuction), r);
        b.process(lim(1, Side::Buy, 1004, 500), r);
        b.process(lim(2, Side::Sell, 997, 500), r);
        CHECK_EQ(b.auction_price().price, v == Venue::SSE ? 1001 : 997);
    }
    // Closing auction: reference is the last trade price (10.03 here), so
    // between 10.00 and 10.04 SZSE picks 10.04.
    OrderBook b(sym10(), 64, sz); Rec r;
    b.process(phase(Phase::Continuous), r);
    b.process(lim(1, Side::Sell, 1003, 100), r);
    b.process(lim(2, Side::Buy, 1003, 100), r);
    CHECK_EQ(b.last_price(), 1003);
    b.process(phase(Phase::ClosingAuction), r);
    b.process(lim(3, Side::Buy, 1004, 500), r);
    b.process(lim(4, Side::Sell, 1000, 500), r);
    AuctionResult ar = b.auction_price();
    CHECK_EQ(ar.price, 1004);
    b.process(phase(Phase::Closed), r);
    CHECK_EQ(r.trades.back().price, 1004);
    CHECK_EQ(b.last_price(), 1004);
}

// Snapshot / restore reproduces the exact book including FIFO order.
TEST(snapshot_restore_roundtrip) {
    Flow f = generate_flow({4, 40000, 5});
    std::vector<OrderBook> a;
    std::vector<Rec> ra(4), rb(4);
    for (auto& s : f.symbols) a.emplace_back(s);
    const size_t cut = f.msgs.size() / 2;
    for (size_t i = 0; i < cut; ++i) a[f.msgs[i].sym].process(f.msgs[i], ra[f.msgs[i].sym]);
    std::vector<OrderBook> b;
    for (auto& s : f.symbols) {
        b.emplace_back(s);
        OrderBook& src = a[s.id];
        b.back().restore_state(src.phase(), src.last_price());
        src.for_each_order([&](const RestingOrder& o) { b.back().restore_order(o); });
    }
    std::vector<Rec> ra2(4);
    for (size_t i = cut; i < f.msgs.size(); ++i) {
        a[f.msgs[i].sym].process(f.msgs[i], ra2[f.msgs[i].sym]);
        b[f.msgs[i].sym].process(f.msgs[i], rb[f.msgs[i].sym]);
    }
    for (int s = 0; s < 4; ++s) {
        bool same = ra2[s].trades == rb[s].trades;
        CHECK(same);
        std::vector<OrderId> oa, ob;
        a[s].for_each_order([&](const RestingOrder& o) { oa.push_back(o.id); });
        b[s].for_each_order([&](const RestingOrder& o) { ob.push_back(o.id); });
        CHECK(oa == ob);
    }
}

TEST(flat_map_vs_std) {
    FlatMap fm(16);
    std::unordered_map<uint64_t, uint32_t> ref;
    std::mt19937_64 rng(7);
    for (int i = 0; i < 400000; ++i) {
        uint64_t k = 1 + rng() % 5000;
        int op = rng() % 3;
        if (op == 0) { bool a = fm.insert(k, i); bool b = ref.emplace(k, i).second; CHECK_EQ(a, b); }
        else if (op == 1) { CHECK_EQ(fm.erase(k), ref.erase(k) == 1); }
        else {
            const uint32_t* p = fm.find(k);
            auto it = ref.find(k);
            CHECK_EQ(p != nullptr, it != ref.end());
            if (p && it != ref.end()) CHECK_EQ(*p, it->second);
        }
    }
    CHECK_EQ(fm.size(), ref.size());
}

// All venue x self-trade-prevention combinations the book supports.
static std::vector<BookConfig> all_configs() {
    std::vector<BookConfig> v;
    for (Venue venue : {Venue::SSE, Venue::SZSE})
        for (StpPolicy stp : {StpPolicy::None, StpPolicy::CancelNewest, StpPolicy::CancelOldest, StpPolicy::CancelBoth}) {
            BookConfig c; c.venue = venue; c.stp = stp;
            v.push_back(c);
        }
    return v;
}

// Differential test: fast book vs naive reference on random full-day flows,
// for every venue / STP combination. Few accounts -> frequent self-trades.
TEST(differential_vs_reference) {
    size_t total = 0, expired = 0;
    for (const BookConfig& cfg : all_configs()) {
        for (uint64_t seed = 1; seed <= 2; ++seed) {
            FlowParams fp{8, 40000, seed};
            fp.accounts = 20;
            Flow f = generate_flow(fp);
            std::vector<OrderBook> fast;
            std::vector<RefBook> ref;
            std::vector<Rec> rec(f.symbols.size());
            for (auto& s : f.symbols) { fast.emplace_back(s, 64, cfg); ref.emplace_back(s, cfg); }
            for (auto& m : f.msgs) { fast[m.sym].process(m, rec[m.sym]); ref[m.sym].process(m); }
            for (size_t i = 0; i < f.symbols.size(); ++i) {
                CHECK_EQ(rec[i].trades.size(), ref[i].trades.size());
                bool same = rec[i].trades == ref[i].trades;
                CHECK(same);
                total += rec[i].trades.size();
                expired += rec[i].expires.size();
                std::vector<std::pair<OrderId, Qty>> fo;
                fast[i].for_each_order([&](const RestingOrder& o) { fo.push_back({o.id, o.qty}); });
                bool same_orders = fo == ref[i].orders();  // same orders, same FIFO order
                CHECK(same_orders);
            }
        }
    }
    std::printf("    %zu trades, %zu expired remainders across %zu configs\n", total, expired, all_configs().size());
    CHECK(total > 10000);
}

// Randomised version of the libFuzzer target: many short adversarial streams
// (tight prices, crossing, cancels of dead ids, odd phase transitions, cage).
TEST(random_fuzz_vs_reference) {
    std::mt19937_64 rng(12345);
    std::vector<uint8_t> buf;
    int cases = 0;
    for (; cases < 30000; ++cases) {
        buf.resize(1 + rng() % 800);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        std::string err;
        if (!fuzz::run_case(buf.data(), buf.size(), &err)) {
            std::printf("    case %d: %s\n", cases, err.c_str());
            CHECK(false);
            break;
        }
    }
    std::printf("    %d random streams, all identical\n", cases);
}

// L2 consistency: feed produced by the engine -> rebuilt book == engine book,
// for both publication styles and with self-trade cancels in the stream.
struct VecSink {
    std::vector<TickRecord> v;
    void write(const TickRecord& r) { v.push_back(r); }
    void write_status(uint64_t seq, SymbolId sym, uint32_t t, Phase) { v.push_back({seq, sym, 'S', t, 0, 0, 0, 0, 'S'}); }
};

template <class Pub>
static void check_l2_roundtrip(const BookConfig& cfg) {
    FlowParams fp{6, 80000, 99};
    fp.accounts = 30;
    Flow f = generate_flow(fp);
    VecSink sink;
    Pub pub(sink);
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s, 64, cfg);
    for (auto& m : f.msgs) pub.process(books[m.sym], m);

    L2Rebuilder rb(cfg.venue);
    for (auto& r : sink.v) rb.apply(r);
    rb.finish();
    CHECK_EQ(rb.stats().anomalies, 0u);
    if (cfg.venue == Venue::SSE) CHECK(rb.stats().trades_aggressor_unseen > 0);  // aggressor never rested
    else CHECK_EQ(rb.stats().trades_aggressor_unseen, 0u);                       // SZSE publishes every order
    for (size_t i = 0; i < books.size(); ++i) {
        size_t n_levels = 0;
        books[i].for_each_level([&](Side s, Price p, int64_t q, uint32_t) {
            ++n_levels;
            auto* lv = rb.levels(i, s);
            CHECK(lv != nullptr);
            if (lv) { auto it = lv->find(p); CHECK(it != lv->end()); if (it != lv->end()) CHECK_EQ(it->second, q); }
        });
        size_t rb_levels = rb.levels(i, Side::Buy)->size() + rb.levels(i, Side::Sell)->size();
        CHECK_EQ(rb_levels, n_levels);
    }
}

TEST(l2_rebuild_matches_engine) {
    for (const BookConfig& cfg : all_configs()) {
        if (cfg.venue == Venue::SSE) check_l2_roundtrip<L2Publisher<VecSink>>(cfg);
        else check_l2_roundtrip<SzsePublisher<VecSink>>(cfg);
    }
}
