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
    std::vector<std::pair<OrderId, Qty>> rests, cancels;
    std::vector<RejectReason> rejects;
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_rest(OrderId id, Side, Price, Qty q, uint32_t) { rests.push_back({id, q}); }
    void on_cancel(OrderId id, Side, Price, Qty q, uint32_t) { cancels.push_back({id, q}); }
    void on_reject(OrderId, RejectReason r) { rejects.push_back(r); }
    void on_phase(Phase, uint32_t) {}
};

SymbolInfo sym10() { return make_symbol(0, 1000); }  // prev close 10.00, band 9.00-11.00

OrderMsg lim(OrderId id, Side s, Price p, Qty q) {
    OrderMsg m; m.id = id; m.side = s; m.price = p; m.qty = q; return m;
}
OrderMsg mkt(OrderId id, Side s, Qty q) {
    OrderMsg m; m.id = id; m.side = s; m.qty = q; m.ord_type = OrdType::MarketBest5IOC; return m;
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

// Differential test: fast book vs naive reference on random full-day flows.
TEST(differential_vs_reference) {
    for (uint64_t seed = 1; seed <= 5; ++seed) {
        Flow f = generate_flow({8, 60000, seed});
        std::vector<OrderBook> fast;
        std::vector<RefBook> ref;
        std::vector<Rec> rec(f.symbols.size());
        for (auto& s : f.symbols) { fast.emplace_back(s); ref.emplace_back(s); }
        for (auto& m : f.msgs) { fast[m.sym].process(m, rec[m.sym]); ref[m.sym].process(m); }
        size_t total = 0;
        for (size_t i = 0; i < f.symbols.size(); ++i) {
            CHECK_EQ(rec[i].trades.size(), ref[i].trades.size());
            bool same = rec[i].trades == ref[i].trades;
            CHECK(same);
            total += rec[i].trades.size();
            for (Side s : {Side::Buy, Side::Sell}) {
                auto d = ref[i].depth(s);
                for (auto& [p, q] : d) CHECK_EQ(fast[i].level_qty(s, p), q);
            }
        }
        CHECK(total > 1000);
    }
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

// L2 consistency: feed produced by the engine -> rebuilt book == engine book.
TEST(l2_rebuild_matches_engine) {
    Flow f = generate_flow({6, 80000, 99});
    struct VecSink {
        std::vector<TickRecord> v;
        void write(const TickRecord& r) { v.push_back(r); }
        void write_status(uint64_t seq, SymbolId sym, uint32_t t, Phase) { v.push_back({seq, sym, 'S', t, 0, 0, 0, 0, 'S'}); }
    } sink;
    L2Publisher<VecSink> pub{sink};
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s);
    for (auto& m : f.msgs) { pub.sym = m.sym; books[m.sym].process(m, pub); }

    L2Rebuilder rb;
    for (auto& r : sink.v) rb.apply(r);
    CHECK_EQ(rb.stats().anomalies, 0u);
    CHECK(rb.stats().trades_aggressor_unseen > 0);  // continuous trades: aggressor never rested
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
