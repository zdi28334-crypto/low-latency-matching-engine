// Core value types shared by every component of the matching engine.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace me {

using Price = int32_t;     // price in ticks; 1 tick = 0.01 CNY (SSE A-share)
using Qty = int32_t;       // shares
using OrderId = uint64_t;  // must be non-zero (0 is the "empty" key of FlatMap)
using SymbolId = uint32_t;

enum class Side : uint8_t { Buy = 0, Sell = 1 };

// Limit: normal limit order.
// MarketBest5IOC: SSE "最优五档即时成交剩余撤销" — sweeps at most 5 price
// levels of the opposite side, then cancels the remainder.
enum class OrdType : uint8_t { Limit = 0, MarketBest5IOC = 1 };

enum class MsgType : uint8_t { New = 0, Cancel = 1, Phase = 2 };

// SSE trading phases. Leaving an auction phase triggers the uncross.
//   09:15-09:25 OpeningAuction, 09:30-11:30 / 13:00-14:57 Continuous,
//   14:57-15:00 ClosingAuction, then Closed.
enum class Phase : uint8_t { Closed = 0, OpeningAuction = 1, Continuous = 2, ClosingAuction = 3 };

inline bool is_auction(Phase p) { return p == Phase::OpeningAuction || p == Phase::ClosingAuction; }

// Inbound message. Fixed-size POD so it can travel through lock-free rings
// and be memory-mapped straight from a flow file.
struct OrderMsg {
    int64_t ts = 0;      // gateway ingress timestamp (TscClock ticks)
    OrderId id = 0;      // New: new order id; Cancel: id to cancel
    SymbolId sym = 0;
    Price price = 0;     // ticks; ignored for MarketBest5IOC
    Qty qty = 0;
    MsgType type = MsgType::New;
    Side side = Side::Buy;
    OrdType ord_type = OrdType::Limit;
    Phase phase = Phase::Closed;  // only for MsgType::Phase
    uint32_t time = 0;   // exchange time HHMMSSmmm
};

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

enum class RejectReason : uint8_t { BadQty, OutOfBand, UnknownOrder, MarketInAuction, Closed, DuplicateId, PriceCage };

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
