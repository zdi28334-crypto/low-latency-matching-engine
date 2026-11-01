// Shared fuzz driver: decodes an arbitrary byte string into an order stream,
// runs it through the optimised OrderBook and the naive RefBook, and checks
//   * identical trade streams (ids, price, qty, aggressor flag)
//   * identical resting orders in identical FIFO order
//   * invariants of the fast book: never crossed outside an auction
//   * the Level-2 feed published by the fast book (SSE or SZSE style)
//     rebuilds to exactly the same depth
//   * snapshot/restore: a mid-stream checkpoint (if requested by the input)
//     is restored into a fresh book that carries on processing
// Used by the libFuzzer target (fuzz_book.cpp) and by a randomised unit test.
#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "me/flow.h"
#include "me/l2.h"
#include "me/order_book.h"
#include "me/ref_book.h"

namespace fuzz {

using namespace me;

struct VecSink {
    std::vector<TickRecord> v;
    void write(const TickRecord& r) { v.push_back(r); }
    void write_status(uint64_t seq, SymbolId sym, uint32_t t, Phase) { v.push_back({seq, sym, 'S', t, 0, 0, 0, 0, 'S'}); }
};

// Wraps either publisher and records the trades it sees.
template <class Pub>
struct Driver {
    VecSink sink;
    Pub pub{sink};
    void process(OrderBook& b, const OrderMsg& m) { pub.process(b, m); }
};

inline bool same_book(const OrderBook& fast, const RefBook& ref, std::string* err) {
    std::vector<std::pair<OrderId, Qty>> fo;
    fast.for_each_order([&](const RestingOrder& o) { fo.push_back({o.id, o.qty}); });
    if (fo != ref.orders()) { *err = "resting orders / FIFO order differ"; return false; }
    return true;
}

inline bool check_l2(const std::vector<TickRecord>& feed, Venue v, const OrderBook& fast, std::string* err) {
    L2Rebuilder rb(v);
    for (const TickRecord& r : feed) rb.apply(r);
    rb.finish();
    if (rb.stats().anomalies) { *err = "L2 rebuild anomalies"; return false; }
    for (Side s : {Side::Buy, Side::Sell}) {
        const auto* lv = rb.levels(0, s);
        size_t n = 0;
        bool bad = false;
        fast.for_each_level([&](Side fs, Price p, int64_t q, uint32_t) {
            if (fs != s) return;
            ++n;
            if (!lv) { bad = true; return; }
            auto it = lv->find(p);
            if (it == lv->end() || it->second != q) bad = true;
        });
        if (bad || n != (lv ? lv->size() : 0)) { *err = "L2 rebuilt depth differs"; return false; }
    }
    return true;
}

// Returns true if every check passes; otherwise fills *err.
template <class Pub>
inline bool run_case_impl(const uint8_t* data, size_t n, const BookConfig& cfg, const SymbolInfo& si,
                          std::string* err) {
    auto fast = std::make_unique<OrderBook>(si, 64, cfg);
    RefBook ref(si, cfg);
    Driver<Pub> drv;
    std::vector<Trade> trades;
    std::vector<OrderId> ids;
    OrderId next_id = 1;
    const Price center = si.prev_close;
    size_t i = 1;
    auto byte = [&]() -> uint8_t { return i < n ? data[i++] : 0; };

    // Trades are taken from the published feed (T records) so the feed itself
    // is cross-checked against the reference book as well.
    size_t feed_pos = 0;
    auto collect_trades = [&] {
        for (; feed_pos < drv.sink.v.size(); ++feed_pos) {
            const TickRecord& r = drv.sink.v[feed_pos];
            if (r.type == 'T') trades.push_back(Trade{r.buy_no, r.sell_no, r.price, r.qty, r.bs, r.time});
        }
    };

    while (i < n) {
        uint8_t op = byte();
        OrderMsg m;
        switch (op % 10) {
            case 0: case 1: case 2: case 3: {  // limit order, mostly near the centre
                uint8_t b = byte();
                m.id = next_id++;
                m.side = (b & 1) ? Side::Buy : Side::Sell;
                int off = (b & 0x80) ? int(int8_t(byte())) * 2      // wide: may hit band/cage
                                     : int((b >> 1) & 0x0F) - 8;   // tight: lots of crossing
                m.price = center + off;
                m.qty = 100 * (1 + (byte() % 8));
                m.account = byte() % 4;            // 0 = anonymous, 1..3 collide often
                ids.push_back(m.id);
                break;
            }
            case 4: case 5: {  // market order types
                uint8_t b = byte();
                m.id = next_id++;
                m.side = (b & 1) ? Side::Buy : Side::Sell;
                m.ord_type = static_cast<OrdType>(1 + (b >> 1) % 4);
                m.qty = 100 * (1 + (b >> 3) % 30);
                m.account = byte() % 4;
                ids.push_back(m.id);
                break;
            }
            case 6: case 7: {  // cancel a known (possibly already dead) id, or a bogus one
                uint8_t b = byte();
                m.type = MsgType::Cancel;
                m.id = ids.empty() || b == 0xFF ? 999999 : ids[b % ids.size()];
                break;
            }
            case 8: {  // phase change, including unusual transitions
                m.type = MsgType::Phase;
                m.phase = static_cast<Phase>(byte() % 4);
                break;
            }
            case 9: {  // checkpoint: snapshot the fast book into a fresh one
                auto fresh = std::make_unique<OrderBook>(si, 64, cfg);
                fresh->restore_state(fast->phase(), fast->last_price());
                fast->for_each_order([&](const RestingOrder& o) { fresh->restore_order(o); });
                fast = std::move(fresh);
                continue;
            }
        }
        drv.process(*fast, m);
        ref.process(m);

        if (fast->phase() == Phase::Continuous && fast->has_bid() && fast->has_ask() &&
            fast->best_bid() >= fast->best_ask()) {
            *err = "fast book crossed in continuous trading";
            return false;
        }
    }
    collect_trades();

    if (trades.size() != ref.trades.size()) {
        *err = "trade count " + std::to_string(trades.size()) + " vs " + std::to_string(ref.trades.size());
        return false;
    }
    for (size_t k = 0; k < trades.size(); ++k) {
        if (!(trades[k] == ref.trades[k])) {
            *err = "trade #" + std::to_string(k) + " differs";
            return false;
        }
    }
    if (!same_book(*fast, ref, err)) return false;
    return check_l2(drv.sink.v, cfg.venue, *fast, err);
}

inline bool run_case(const uint8_t* data, size_t n, std::string* err) {
    if (n < 1) return true;
    // Byte 0: bit0 picks a 10.00 stock (2% > 10 ticks) or a 3.00 stock (the
    // 10-tick floor dominates); bits 1-2 the STP policy; bit 3 the venue.
    BookConfig cfg;
    cfg.stp = static_cast<StpPolicy>((data[0] >> 1) & 3);
    cfg.venue = (data[0] & 8) ? Venue::SZSE : Venue::SSE;
    SymbolInfo si = make_symbol(0, (data[0] & 1) ? 1000 : 300);
    return cfg.venue == Venue::SSE ? run_case_impl<L2Publisher<VecSink>>(data, n, cfg, si, err)
                                   : run_case_impl<SzsePublisher<VecSink>>(data, n, cfg, si, err);
}

}  // namespace fuzz
