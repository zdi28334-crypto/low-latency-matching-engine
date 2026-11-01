// Shared fuzz driver: decodes an arbitrary byte string into an order stream,
// runs it through the optimised OrderBook and the naive RefBook, and checks
//   * identical trade streams (ids, price, qty, aggressor flag)
//   * identical depth at every price level and identical live-order count
//   * invariants of the fast book: never crossed outside an auction,
//     level totals non-negative.
// Used by the libFuzzer target (fuzz_book.cpp) and by a randomised unit test.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "me/flow.h"
#include "me/order_book.h"
#include "me/ref_book.h"

namespace fuzz {

using namespace me;

struct Recorder {
    std::vector<Trade> trades;
    void on_trade(const Trade& t) { trades.push_back(t); }
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

// Returns true if both books agree; otherwise fills *err.
inline bool run_case(const uint8_t* data, size_t n, std::string* err) {
    // Byte 0 picks the security: a 10.00 stock (2% > 10 ticks) or a 3.00 stock
    // (10-tick floor dominates) so both cage branches get exercised.
    if (n < 1) return true;
    SymbolInfo si = make_symbol(0, (data[0] & 1) ? 1000 : 300);
    OrderBook fast(si, 64);
    RefBook ref(si);
    Recorder rec;
    std::vector<OrderId> ids;
    OrderId next_id = 1;
    const Price center = si.prev_close;
    size_t i = 1;
    auto byte = [&]() -> uint8_t { return i < n ? data[i++] : 0; };

    while (i < n) {
        uint8_t op = byte();
        OrderMsg m;
        switch (op % 8) {
            case 0: case 1: case 2: case 3: {  // limit order, mostly near the centre
                uint8_t b = byte();
                m.id = next_id++;
                m.side = (b & 1) ? Side::Buy : Side::Sell;
                int off = (b & 0x80) ? int(int8_t(byte())) * 2        // wide: may hit band/cage
                                     : int((b >> 1) & 0x0F) - 8;     // tight: lots of crossing
                m.price = center + off;
                m.qty = 100 * (1 + (byte() % 8));
                ids.push_back(m.id);
                break;
            }
            case 4: {  // market best-5 IOC
                uint8_t b = byte();
                m.id = next_id++;
                m.side = (b & 1) ? Side::Buy : Side::Sell;
                m.ord_type = OrdType::MarketBest5IOC;
                m.qty = 100 * (1 + (b >> 1) % 30);
                break;
            }
            case 5: case 6: {  // cancel a known (possibly already dead) id, or a bogus one
                uint8_t b = byte();
                m.type = MsgType::Cancel;
                m.id = ids.empty() || b == 0xFF ? 999999 : ids[b % ids.size()];
                break;
            }
            case 7: {  // phase change, including unusual transitions
                m.type = MsgType::Phase;
                m.phase = static_cast<Phase>(byte() % 4);
                break;
            }
        }
        fast.process(m, rec);
        ref.process(m);

        if (fast.phase() == Phase::Continuous && fast.has_bid() && fast.has_ask() &&
            fast.best_bid() >= fast.best_ask()) {
            *err = "fast book crossed in continuous trading";
            return false;
        }
    }

    if (rec.trades.size() != ref.trades.size()) {
        *err = "trade count " + std::to_string(rec.trades.size()) + " vs " + std::to_string(ref.trades.size());
        return false;
    }
    for (size_t k = 0; k < rec.trades.size(); ++k) {
        if (!(rec.trades[k] == ref.trades[k])) {
            *err = "trade #" + std::to_string(k) + " differs";
            return false;
        }
    }
    if (fast.live_orders() != ref.live_orders()) {
        *err = "live orders " + std::to_string(fast.live_orders()) + " vs " + std::to_string(ref.live_orders());
        return false;
    }
    for (Side s : {Side::Buy, Side::Sell}) {
        auto d = ref.depth(s);
        size_t fast_levels = 0;
        bool bad = false;
        fast.for_each_level([&](Side fs, Price p, int64_t q, uint32_t) {
            if (fs != s) return;
            ++fast_levels;
            auto it = d.find(p);
            if (q <= 0 || it == d.end() || it->second != q) bad = true;
        });
        if (bad || fast_levels != d.size()) {
            *err = std::string("depth differs on ") + (s == Side::Buy ? "bid" : "ask") + " side";
            return false;
        }
    }
    return true;
}

}  // namespace fuzz
