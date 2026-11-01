// Level-2 tick-by-tick feeds and an order-book rebuilder that consumes them.
// Two publication styles are supported, matching the two exchanges:
//
// SSE (merged 逐笔委托 + 逐笔成交 stream):
//   A  order added to the book. SSE publishes an order only once it RESTS,
//      with its *remaining* quantity. An order that trades fully on arrival
//      never produces an 'A'; one that partially trades produces its 'T'
//      records first, then an 'A' with the residual. Market orders that never
//      rest never appear as 'A'.
//   D  resting order deleted (cancel), with the remaining quantity.
//   T  trade: buy order no, sell order no, price, qty, B/S/N aggressor flag.
//   S  status change (OCALL / TRADE / CCALL / CLOSE).
//   => a 'T' can reference an order the rebuilder has never seen (the
//      aggressor); only the side that actually rests may be decremented.
//
// SZSE (逐笔委托 + 逐笔成交, with cancellations inside 逐笔成交):
//   O  every accepted order is published ON RECEIPT with its full quantity,
//      before any trade it causes. Market orders carry price 0.
//   T  trade (both order numbers always refer to published orders).
//   C  cancellation (SZSE ExecType=4): removes the remaining quantity of an
//      order — a cancel request, an IOC remainder or a self-trade cancel.
//   S  status change.
//   => the rebuilder inserts every limit order on receipt (the book may be
//      transiently crossed until the trades that follow are applied), keeps
//      price-0 market orders off-book, and infers the resting price of a
//      best-5-to-limit remainder from its last fill.
//   Simplification: 本方最优 / 对手方最优 orders are published with the price
//   they converted to (ord 'U' / 'C').
//
// CSV columns: seq,sym,type,time,buy_no,sell_no,price,qty,bs,ord
//   ord (SZSE 'O' rows): '2' limit, '1' best-5 IOC, 'T' best-5 to limit,
//   'U' own best, 'C' counter best; '-' elsewhere.
#pragma once
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "me/order_book.h"
#include "me/types.h"

namespace me {

struct TickRecord {
    uint64_t seq;
    SymbolId sym;
    char type;  // SSE: A D T S   SZSE: O T C S
    uint32_t time;
    OrderId buy_no, sell_no;
    Price price;
    Qty qty;
    char bs;    // B S N (for S records: first letter of the phase code)
    char ord = '-';
};

inline const char* phase_code(Phase p) {
    switch (p) {
        case Phase::OpeningAuction: return "OCALL";
        case Phase::Continuous: return "TRADE";
        case Phase::ClosingAuction: return "CCALL";
        default: return "CLOSE";
    }
}

inline char ord_code(OrdType t) {
    switch (t) {
        case OrdType::Limit: return '2';
        case OrdType::MarketBest5IOC: return '1';
        case OrdType::MarketBest5ToLimit: return 'T';
        case OrdType::OwnBest: return 'U';
        case OrdType::CounterBest: return 'C';
    }
    return '?';
}

// ---------------------------------------------------------------- CSV I/O
class TickWriter {
public:
    explicit TickWriter(const std::string& path) : fp_(std::fopen(path.c_str(), "w")) {
        if (!fp_) throw std::runtime_error("cannot open " + path);
        std::setvbuf(fp_, nullptr, _IOFBF, 1 << 20);
        std::fputs("seq,sym,type,time,buy_no,sell_no,price,qty,bs,ord\n", fp_);
    }
    ~TickWriter() { if (fp_) std::fclose(fp_); }
    void write(const TickRecord& r) {
        std::fprintf(fp_, "%lu,%u,%c,%09u,%lu,%lu,%d.%02d,%d,%c,%c\n", (unsigned long)r.seq, r.sym, r.type, r.time,
                     (unsigned long)r.buy_no, (unsigned long)r.sell_no, r.price / 100, r.price % 100, r.qty, r.bs,
                     r.ord);
    }
    void write_status(uint64_t seq, SymbolId sym, uint32_t time, Phase p) {
        std::fprintf(fp_, "%lu,%u,S,%09u,0,0,0.00,0,%s,-\n", (unsigned long)seq, sym, time, phase_code(p));
    }
private:
    FILE* fp_;
};

// Parses the CSV above. Prices are parsed as decimals into ticks.
inline bool read_tick_line(const char* line, TickRecord& r) {
    unsigned long seq, b, s;
    unsigned sym, time;
    char type, bs[8], ord = '-';
    int pi, pf, qty;
    int n = std::sscanf(line, "%lu,%u,%c,%u,%lu,%lu,%d.%d,%d,%7[^,\n],%c", &seq, &sym, &type, &time, &b, &s, &pi, &pf,
                        &qty, bs, &ord);
    if (n < 10) return false;
    r = TickRecord{seq, sym, type, time, b, s, pi * 100 + pf, qty, bs[0], ord};
    return true;
}

// ------------------------------------------------------------ publishers
// SSE style. Plain OrderBook listener.
template <class Sink>
struct L2Publisher {
    Sink& sink;
    SymbolId sym = 0;
    uint64_t seq = 0;
    void on_trade(const Trade& t) {
        sink.write(TickRecord{++seq, sym, 'T', t.time, t.buy_id, t.sell_id, t.price, t.qty, t.bs_flag});
    }
    void on_rest(OrderId id, Side s, Price p, Qty q, uint32_t time) {
        sink.write(TickRecord{++seq, sym, 'A', time, s == Side::Buy ? id : 0, s == Side::Sell ? id : 0, p, q,
                              s == Side::Buy ? 'B' : 'S'});
    }
    void on_cancel(OrderId id, Side s, Price p, Qty q, uint32_t time) {
        sink.write(TickRecord{++seq, sym, 'D', time, s == Side::Buy ? id : 0, s == Side::Sell ? id : 0, p, q,
                              s == Side::Buy ? 'B' : 'S'});
    }
    void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}  // never rested: nothing to publish
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase p, uint32_t time) { sink.write_status(++seq, sym, time, p); }

    // Uniform driver interface shared with SzsePublisher.
    void process(OrderBook& book, const OrderMsg& m) { sym = m.sym; book.process(m, *this); }
};

// SZSE style. The order record must precede its trades but rejected orders
// are never published, so events are buffered for the duration of one message.
template <class Sink>
struct SzsePublisher {
    explicit SzsePublisher(Sink& s) : sink(s) {}
    Sink& sink;
    SymbolId sym = 0;
    uint64_t seq = 0;

    void process(OrderBook& book, const OrderMsg& m) {
        sym = m.sym;
        cur_ = &m;
        rejected_ = false;
        buf_.clear();
        if (m.type == MsgType::New) {  // effective price of converted market orders
            const bool buy = m.side == Side::Buy;
            eff_px_ = m.price;
            if (m.ord_type == OrdType::OwnBest) eff_px_ = buy ? (book.has_bid() ? book.best_bid() : 0)
                                                               : (book.has_ask() ? book.best_ask() : 0);
            if (m.ord_type == OrdType::CounterBest) eff_px_ = buy ? (book.has_ask() ? book.best_ask() : 0)
                                                                   : (book.has_bid() ? book.best_bid() : 0);
            if (m.ord_type == OrdType::MarketBest5IOC || m.ord_type == OrdType::MarketBest5ToLimit) eff_px_ = 0;
        }
        book.process(m, *this);
        if (m.type == MsgType::New && !rejected_) {
            const bool buy = m.side == Side::Buy;
            sink.write(TickRecord{++seq, sym, 'O', m.time, buy ? m.id : 0, buy ? 0 : m.id, eff_px_, m.qty,
                                  buy ? 'B' : 'S', ord_code(m.ord_type)});
        }
        for (TickRecord& r : buf_) { r.seq = ++seq; sink.write(r); }
    }

    void on_trade(const Trade& t) {
        buf_.push_back(TickRecord{0, sym, 'T', t.time, t.buy_id, t.sell_id, t.price, t.qty, t.bs_flag});
    }
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}  // already published on receipt
    void on_cancel(OrderId id, Side s, Price, Qty q, uint32_t time) { cancel(id, s, q, time); }
    void on_expire(OrderId id, Side s, Qty q, ExpireReason, uint32_t time) { cancel(id, s, q, time); }
    void on_reject(OrderId, RejectReason) { rejected_ = true; }
    void on_phase(Phase p, uint32_t time) { sink.write_status(++seq, sym, time, p); }

private:
    void cancel(OrderId id, Side s, Qty q, uint32_t time) {
        buf_.push_back(TickRecord{0, sym, 'C', time, s == Side::Buy ? id : 0, s == Side::Sell ? id : 0, 0, q,
                                  s == Side::Buy ? 'B' : 'S'});
    }
    const OrderMsg* cur_ = nullptr;
    bool rejected_ = false;
    Price eff_px_ = 0;
    std::vector<TickRecord> buf_;
};

// ------------------------------------------------------------ rebuilder
// Rebuilds per-symbol books purely from a tick feed of either style.
class L2Rebuilder {
public:
    struct Stats {
        uint64_t adds = 0, dels = 0, trades = 0, status = 0;
        uint64_t trades_aggressor_unseen = 0;  // SSE: expected, the aggressor never rested
        uint64_t anomalies = 0;                // records that do not fit the book
    };

    explicit L2Rebuilder(Venue v = Venue::SSE) : venue_(v) {}

    void apply(const TickRecord& r) {
        if (venue_ == Venue::SSE) apply_sse(r); else apply_szse(r);
    }
    // Call once after the last record (finalises a pending SZSE market order).
    void finish() { for (auto& [sym, bk] : books_) finalize(bk); }

    // Top-N depth for one side: vector of (price, qty), best first.
    std::vector<std::pair<Price, int64_t>> depth(SymbolId sym, Side s, size_t n) const {
        std::vector<std::pair<Price, int64_t>> out;
        auto it = books_.find(sym);
        if (it == books_.end()) return out;
        if (s == Side::Buy) {
            for (auto p = it->second.bids.rbegin(); p != it->second.bids.rend() && out.size() < n; ++p)
                out.push_back(*p);
        } else {
            for (auto p = it->second.asks.begin(); p != it->second.asks.end() && out.size() < n; ++p)
                out.push_back(*p);
        }
        return out;
    }

    const std::map<Price, int64_t>* levels(SymbolId sym, Side s) const {
        auto it = books_.find(sym);
        if (it == books_.end()) return nullptr;
        return s == Side::Buy ? &it->second.bids : &it->second.asks;
    }
    const Stats& stats() const { return st_; }

private:
    struct Ord {
        Side side;
        Price price;   // 0 = off-book (SZSE market order still being processed)
        Qty qty;
        char ord = '2';
        Price last_fill = 0;
    };
    struct Book {
        std::unordered_map<OrderId, Ord> orders;
        std::map<Price, int64_t> bids, asks;
        OrderId pending = 0;  // SZSE: market order whose events are still arriving
        std::map<Price, int64_t>& level(Side s) { return s == Side::Buy ? bids : asks; }
    };

    void add(Book& bk, OrderId id, const Ord& o) {
        bk.orders[id] = o;
        if (o.price) bk.level(o.side)[o.price] += o.qty;
    }

    // Removes q from an order (whole remainder if `whole`). False if unknown.
    bool reduce(Book& bk, OrderId id, Qty q, bool whole, Price trade_px = 0) {
        if (id == 0) return false;
        auto it = bk.orders.find(id);
        if (it == bk.orders.end()) return false;
        Ord& o = it->second;
        if (q > o.qty || (whole && q != o.qty)) { ++st_.anomalies; q = o.qty; }
        if (o.price) {
            auto& lv = bk.level(o.side);
            auto li = lv.find(o.price);
            li->second -= q;
            if (li->second == 0) lv.erase(li);
        }
        if (trade_px) o.last_fill = trade_px;
        o.qty -= q;
        if (o.qty == 0) {
            if (bk.pending == id) bk.pending = 0;
            bk.orders.erase(it);
        }
        return true;
    }

    void apply_sse(const TickRecord& r) {
        Book& bk = books_[r.sym];
        switch (r.type) {
            case 'A': {
                ++st_.adds;
                Side s = r.bs == 'B' ? Side::Buy : Side::Sell;
                add(bk, s == Side::Buy ? r.buy_no : r.sell_no, Ord{s, r.price, r.qty});
                break;
            }
            case 'D':
                ++st_.dels;
                if (!reduce(bk, r.bs == 'B' ? r.buy_no : r.sell_no, r.qty, true)) ++st_.anomalies;
                break;
            case 'T': {
                ++st_.trades;
                bool b = reduce(bk, r.buy_no, r.qty, false);
                bool s = reduce(bk, r.sell_no, r.qty, false);
                if (r.bs == 'N') { if (!(b && s)) ++st_.anomalies; }  // auction: both rest
                else if (!(b || s)) ++st_.anomalies;
                else if (!(b && s)) ++st_.trades_aggressor_unseen;
                break;
            }
            case 'S': ++st_.status; break;
            default: ++st_.anomalies;
        }
    }

    // Place the remainder of a best-5-to-limit order at its last fill price
    // once the records belonging to it are over.
    void finalize(Book& bk) {
        if (!bk.pending) return;
        const OrderId id = bk.pending;
        bk.pending = 0;
        auto it = bk.orders.find(id);
        if (it == bk.orders.end()) return;
        Ord o = it->second;
        bk.orders.erase(it);
        if (o.ord == 'T' && o.last_fill && o.qty > 0) { o.price = o.last_fill; add(bk, id, o); }
        else ++st_.anomalies;  // an IOC remainder must have been cancelled explicitly
    }

    void apply_szse(const TickRecord& r) {
        Book& bk = books_[r.sym];
        // A pending market order's sweep is over once something other than its
        // own trades or a cancellation shows up. (Cancellations may interleave:
        // self-trade prevention cancels resting orders mid-sweep.)
        if (bk.pending) {
            const OrderId p = bk.pending;
            const bool own_trade = r.type == 'T' && (r.buy_no == p || r.sell_no == p);
            if (!own_trade && r.type != 'C') finalize(bk);
        }
        switch (r.type) {
            case 'O': {
                ++st_.adds;
                Side s = r.bs == 'B' ? Side::Buy : Side::Sell;
                OrderId id = s == Side::Buy ? r.buy_no : r.sell_no;
                const bool offbook = r.ord == '1' || r.ord == 'T';
                add(bk, id, Ord{s, offbook ? 0 : r.price, r.qty, r.ord});
                if (offbook) bk.pending = id;
                break;
            }
            case 'T': {
                ++st_.trades;
                bool b = reduce(bk, r.buy_no, r.qty, false, r.price);
                bool s = reduce(bk, r.sell_no, r.qty, false, r.price);
                if (!(b && s)) ++st_.anomalies;  // SZSE: both orders were published
                break;
            }
            case 'C':
                ++st_.dels;
                if (!reduce(bk, r.bs == 'B' ? r.buy_no : r.sell_no, r.qty, true)) ++st_.anomalies;
                break;
            case 'S': ++st_.status; break;
            default: ++st_.anomalies;
        }
    }

    Venue venue_;
    std::unordered_map<SymbolId, Book> books_;
    Stats st_;
};

}  // namespace me
