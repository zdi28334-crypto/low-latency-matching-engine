// Core value types shared by every component of the matching engine.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace me {

using Price = int32_t;     // price in ticks; 1 tick = 0.01 CNY (A-share)
using Qty = int32_t;       // shares
using OrderId = uint64_t;  // must be non-zero (0 is the "empty" key of FlatMap)
using SymbolId = uint32_t;
using AccountId = uint32_t;  // 0 = anonymous (no self-trade check)

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Order types (A-share market orders are only accepted in continuous trading):
//   Limit              normal limit order
//   MarketBest5IOC     最优五档即时成交剩余撤销: sweep at most 5 opposite price
//                      levels, cancel the remainder
//   MarketBest5ToLimit 最优五档即时成交剩余转限价: sweep at most 5 levels, the
//                      remainder rests as a limit order at the last fill price;
//                      rejected if the opposite side is empty
//   OwnBest            本方最优: limit order at the own side's best price;
//                      rejected if the own side is empty
//   CounterBest        对手方最优: limit order at the opposite side's best price;
//                      rejected if the opposite side is empty
enum class OrdType : uint8_t { Limit = 0, MarketBest5IOC = 1, MarketBest5ToLimit = 2, OwnBest = 3, CounterBest = 4 };

inline bool is_market(OrdType t) { return t != OrdType::Limit; }

enum class MsgType : uint8_t { New = 0, Cancel = 1, Phase = 2 };

// Trading phases. Leaving an auction phase triggers the uncross.
//   09:15-09:25 OpeningAuction, 09:30-11:30 / 13:00-14:57 Continuous,
//   14:57-15:00 ClosingAuction, then Closed.
enum class Phase : uint8_t { Closed = 0, OpeningAuction = 1, Continuous = 2, ClosingAuction = 3 };

inline bool is_auction(Phase p) { return p == Phase::OpeningAuction || p == Phase::ClosingAuction; }

// Inbound message. Fixed-size POD (40 bytes, no padding) so it can travel
// through lock-free rings, be memory-mapped from a flow file and be written
// verbatim to the write-ahead log.
struct OrderMsg {
    int64_t ts = 0;      // gateway ingress timestamp (TscClock ticks)
    OrderId id = 0;      // New: new order id; Cancel: id to cancel
    SymbolId sym = 0;
    Price price = 0;     // ticks; ignored for market order types
    Qty qty = 0;
    MsgType type = MsgType::New;
    Side side = Side::Buy;
    OrdType ord_type = OrdType::Limit;
    Phase phase = Phase::Closed;  // only for MsgType::Phase
    uint32_t time = 0;   // exchange time HHMMSSmmm
    AccountId account = 0;
};
static_assert(sizeof(OrderMsg) == 40, "OrderMsg layout is part of the flow/WAL format");

struct Trade {
    OrderId buy_id;
    OrderId sell_id;
    Price price;
    Qty qty;
    char bs_flag;    // 'B' buyer aggressor, 'S' seller aggressor, 'N' auction
    uint32_t time;
};

inline bool operator==(const Trade& a, const Trade& b) {
    return a.buy_id == b.buy_id && a.sell_id == b.sell_id && a.price == b.price &&
           a.qty == b.qty && a.bs_flag == b.bs_flag;
}

enum class RejectReason : uint8_t {
    BadQty, OutOfBand, UnknownOrder, MarketInAuction, Closed, DuplicateId, PriceCage, NoCounterparty
};

// Why the unfilled remainder of an incoming order was removed without resting.
enum class ExpireReason : uint8_t { IOC, SelfTrade };

// Exchange whose rules the book follows. Differences implemented:
//  * call-auction tie-break after "max volume, min imbalance":
//      SSE  -> midpoint of the remaining candidate prices
//      SZSE -> the candidate closest to the reference price (previous close
//              for the opening auction, last trade price otherwise)
//  * Level-2 tick-by-tick publication style (see l2.h)
enum class Venue : uint8_t { SSE = 0, SZSE = 1 };

// Self-trade prevention, checked in continuous trading when an incoming order
// would match a resting order of the same non-zero account:
//   None          allow the self-trade
//   CancelNewest  stop matching and cancel the incoming remainder
//   CancelOldest  cancel the resting order and keep matching
//   CancelBoth    cancel the resting order and the incoming remainder
enum class StpPolicy : uint8_t { None = 0, CancelNewest = 1, CancelOldest = 2, CancelBoth = 3 };

// 价格笼子 (main-board price cage), applied to limit orders in continuous
// trading only:
//   buy  price <= max(buy_base  * (100 + pct) / 100, buy_base  + min_ticks)
//   sell price >= min(sell_base * (100 - pct) / 100, sell_base - min_ticks)
// buy_base  = best ask, else best bid, else last trade, else previous close
// sell_base = best bid, else best ask, else last trade, else previous close
// Out-of-cage orders are rejected. (Some venues instead park them until the
// cage moves; that variant is not implemented.)
struct CageConfig {
    bool enabled = true;
    int pct = 2;         // 2%
    int min_ticks = 10;  // 0.10 CNY floor for low-priced stocks
};

struct BookConfig {
    Venue venue = Venue::SSE;
    StpPolicy stp = StpPolicy::None;
    CageConfig cage;
};

// Static per-symbol reference data.
struct SymbolInfo {
    SymbolId id;
    char code[8];        // e.g. "600000"
    Price prev_close;
    Price lower_limit;   // 跌停价
    Price upper_limit;   // 涨停价
};

inline std::string ticks_to_str(Price p) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d.%02d", p / 100, p % 100);
    return buf;
}

}  // namespace me
