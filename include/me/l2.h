// SSE-style Level-2 tick-by-tick feed (逐笔委托 + 逐笔成交 merged stream) and
// an order-book rebuilder that consumes it.
//
// Feed semantics follow the SSE merged tick format:
//   A  order added to the book. NOTE: SSE publishes an order only once it
//      RESTS, with its *remaining* quantity. An order that trades fully on
//      arrival never produces an 'A'; one that partially trades produces its
//      'T' records first, then an 'A' with the residual.
//   D  order deleted (cancel), with the remaining quantity.
//   T  trade: buy order no, sell order no, price, qty, B/S/N aggressor flag.
//   S  status change (OCALL / TRADE / CCALL / CLOSE).
// The consequence for reconstruction: a 'T' can reference an order the
// rebuilder has never seen (the aggressor) — it must only decrement the side
// that actually rests in the book.
#pragma once
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "me/types.h"

namespace me {

struct TickRecord {
    uint64_t seq;
    SymbolId sym;
    char type;  // A D T S
    uint32_t time;
    OrderId buy_no, sell_no;
    Price price;
    Qty qty;
    char bs;    // B S N (for S records: phase code)
};

inline const char* phase_code(Phase p) {
    switch (p) {
        case Phase::OpeningAuction: return "OCALL";
        case Phase::Continuous: return "TRADE";
        case Phase::ClosingAuction: return "CCALL";
        default: return "CLOSE";
    }
}

// ---------------------------------------------------------------- CSV I/O
class TickWriter {
public:
    explicit TickWriter(const std::string& path) : fp_(std::fopen(path.c_str(), "w")) {
        if (!fp_) throw std::runtime_error("cannot open " + path);
        std::setvbuf(fp_, nullptr, _IOFBF, 1 << 20);
        std::fputs("seq,sym,type,time,buy_no,sell_no,price,qty,bs\n", fp_);
    }
    ~TickWriter() { if (fp_) std::fclose(fp_); }
    void write(const TickRecord& r) {
        std::fprintf(fp_, "%lu,%u,%c,%09u,%lu,%lu,%d.%02d,%d,%c\n", (unsigned long)r.seq, r.sym, r.type,
                     r.time, (unsigned long)r.buy_no, (unsigned long)r.sell_no, r.price / 100, r.price % 100,
                     r.qty, r.bs);
    }
    void write_status(uint64_t seq, SymbolId sym, uint32_t time, Phase p) {
        std::fprintf(fp_, "%lu,%u,S,%09u,0,0,0.00,0,%s\n", (unsigned long)seq, sym, time, phase_code(p));
    }
private:
    FILE* fp_;
};

// Parses the CSV above. Status rows come back with type 'S' and bs = first
// letter of the phase code. Prices are parsed as decimals into ticks.
inline bool read_tick_line(const char* line, TickRecord& r) {
    unsigned long seq, b, s;
    unsigned sym, time;
    char type, bs[8];
    int pi, pf, qty;
    if (std::sscanf(line, "%lu,%u,%c,%u,%lu,%lu,%d.%d,%d,%7s", &seq, &sym, &type, &time, &b, &s, &pi, &pf, &qty,
                    bs) != 10)
        return false;
    r = TickRecord{seq, sym, type, time, b, s, pi * 100 + pf, qty, bs[0]};
    return true;
}

// Listener that turns OrderBook events into SSE-style tick records.
// Used by the exchange simulator to *produce* an L2 feed.
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
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase p, uint32_t time) { sink.write_status(++seq, sym, time, p); }
};

// ------------------------------------------------------------ rebuilder
// Rebuilds per-symbol books purely from the tick feed.
class L2Rebuilder {
public:
    struct Stats {
        uint64_t adds = 0, dels = 0, trades = 0, status = 0;
        uint64_t trades_aggressor_unseen = 0;  // expected: one side not in book
        uint64_t anomalies = 0;                // trade/delete that does not fit the book
    };

    void apply(const TickRecord& r) {
        Book& bk = books_[r.sym];
        switch (r.type) {
            case 'A': {
                ++st_.adds;
                Side s = r.bs == 'B' ? Side::Buy : Side::Sell;
                OrderId id = s == Side::Buy ? r.buy_no : r.sell_no;
                bk.orders[id] = {s, r.price, r.qty};
                bk.level(s)[r.price] += r.qty;
                break;
            }
            case 'D': {
                ++st_.dels;
                OrderId id = r.bs == 'B' ? r.buy_no : r.sell_no;
                if (!reduce(bk, id, r.qty, true)) ++st_.anomalies;
                break;
            }
            case 'T': {
                ++st_.trades;
                bool b = reduce(bk, r.buy_no, r.qty, false);
                bool s = reduce(bk, r.sell_no, r.qty, false);
                if (r.bs == 'N') { if (!(b && s)) ++st_.anomalies; }  // auction: both rest
                else if (!(b || s)) ++st_.anomalies;
                else if (!(b && s)) ++st_.trades_aggressor_unseen;
                last_px_[r.sym] = r.price;
                break;
            }
            case 'S': ++st_.status; break;
        }
    }

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
    struct Ord { Side side; Price price; Qty qty; };
    struct Book {
        std::unordered_map<OrderId, Ord> orders;
        std::map<Price, int64_t> bids, asks;
        std::map<Price, int64_t>& level(Side s) { return s == Side::Buy ? bids : asks; }
    };

    bool reduce(Book& bk, OrderId id, Qty q, bool whole) {
        if (id == 0) return false;
        auto it = bk.orders.find(id);
        if (it == bk.orders.end()) return false;
        Ord& o = it->second;
        if (q > o.qty || (whole && q != o.qty)) { ++st_.anomalies; q = o.qty; }
        auto& lv = bk.level(o.side);
        auto li = lv.find(o.price);
        li->second -= q;
        if (li->second == 0) lv.erase(li);
        o.qty -= q;
        if (o.qty == 0) bk.orders.erase(it);
        return true;
    }

    std::unordered_map<SymbolId, Book> books_;
    std::unordered_map<SymbolId, Price> last_px_;
    Stats st_;
};

}  // namespace me
