// Single-symbol limit order book with price-time priority.
//
// Design (latency-oriented):
//  * SSE A-shares have a daily price band (涨跌停), so every possible price is
//    known up front. Each side is a flat array of price levels indexed by
//    (price - lower_limit): O(1) level lookup, no tree rebalancing.
//  * A per-side bitmap of non-empty levels finds the next best price with a
//    couple of clz/ctz instructions instead of scanning empty levels.
//  * Orders live in a pre-allocated pool with an intrusive doubly linked FIFO
//    per level; cancel is O(1) via FlatMap<OrderId, pool index>.
//  * The book never allocates on the hot path once warmed up and never does
//    I/O: every outcome is reported to a Listener (template, so calls inline).
//
// Listener interface (all methods required):
//   void on_trade(const Trade&);
//   void on_rest(OrderId, Side, Price, Qty residual, uint32_t time);   // -> L2 'A'
//   void on_cancel(OrderId, Side, Price, Qty remaining, uint32_t time); // -> L2 'D'
//   void on_reject(OrderId, RejectReason);
//   void on_phase(Phase, uint32_t time);
#pragma once
#include <cassert>
#include <cstdint>
#include <vector>

#include "me/flat_map.h"
#include "me/types.h"

namespace me {

struct AuctionResult {
    bool crossed = false;
    Price price = 0;
    int64_t volume = 0;
    int64_t imbalance = 0;
};

class OrderBook {
public:
    static constexpr uint32_t NIL = 0xFFFFFFFFu;

    explicit OrderBook(const SymbolInfo& info, size_t expected_orders = 4096)
        : info_(info),
          lo_(info.lower_limit),
          nlevels_(info.upper_limit - info.lower_limit + 1),
          bids_(nlevels_),
          asks_(nlevels_),
          bid_bits_((nlevels_ + 63) / 64, 0),
          ask_bits_((nlevels_ + 63) / 64, 0),
          index_(expected_orders) {
        pool_.reserve(expected_orders);
    }

    const SymbolInfo& info() const { return info_; }
    Phase phase() const { return phase_; }
    size_t live_orders() const { return index_.size(); }

    bool has_bid() const { return best_bid_ >= 0; }
    bool has_ask() const { return best_ask_ >= 0; }
    Price best_bid() const { return lo_ + best_bid_; }
    Price best_ask() const { return lo_ + best_ask_; }

    // Aggregated depth at a price (for snapshots / tests).
    int64_t level_qty(Side s, Price p) const {
        int i = p - lo_;
        if (i < 0 || i >= nlevels_) return 0;
        return (s == Side::Buy ? bids_ : asks_)[i].total;
    }

    // Visit all non-empty levels: f(Side, Price, total_qty, order_count).
    template <class F>
    void for_each_level(F&& f) const {
        for (int i = nlevels_ - 1; i >= 0; --i)
            if (bids_[i].count) f(Side::Buy, lo_ + i, bids_[i].total, bids_[i].count);
        for (int i = 0; i < nlevels_; ++i)
            if (asks_[i].count) f(Side::Sell, lo_ + i, asks_[i].total, asks_[i].count);
    }

    // ---------------------------------------------------------------- entry
    template <class L>
    void process(const OrderMsg& m, L& lis) {
        switch (m.type) {
            case MsgType::New: on_new(m, lis); break;
            case MsgType::Cancel: on_cancel_req(m, lis); break;
            case MsgType::Phase: on_phase_change(m.phase, m.time, lis); break;
        }
    }

    // SSE call-auction price determination (上交所交易规则 3.6):
    //  (1) maximise executed volume;
    //  (2) all bids above / asks below the price are fully filled;
    //  (3) at the price, at least one side is fully filled (implied by (1));
    //  tie-break: minimise unfilled volume (imbalance); if still tied, take
    //  the midpoint of the remaining prices, rounded half-up to a tick.
    // Also used during the auction to publish the indicative price (虚拟参考价).
    AuctionResult auction_price() const {
        AuctionResult r;
        if (best_bid_ < 0 || best_ask_ < 0 || best_bid_ < best_ask_) return r;
        const int a = best_ask_, b = best_bid_, n = b - a + 1;
        cum_buy_.assign(n, 0);   // bids with price >= p
        cum_sell_.assign(n, 0);  // asks with price <= p
        int64_t acc = 0;
        for (int i = b; i >= a; --i) { acc += bids_[i].total; cum_buy_[i - a] = acc; }
        acc = 0;
        for (int i = a; i <= b; ++i) { acc += asks_[i].total; cum_sell_[i - a] = acc; }

        int64_t best_vol = 0, best_imb = 0;
        int lo_idx = -1, hi_idx = -1;
        for (int pass = 0; pass < 2 && lo_idx < 0; ++pass) {
            for (int i = a; i <= b; ++i) {
                if (!bids_[i].count && !asks_[i].count) continue;  // only declared prices
                int64_t bv = cum_buy_[i - a], sv = cum_sell_[i - a];
                int64_t vol = bv < sv ? bv : sv;
                if (vol <= 0) continue;
                if (pass == 0) {  // condition (2); pass 1 is a defensive fallback
                    int64_t buy_above = bv - bids_[i].total, sell_below = sv - asks_[i].total;
                    if (buy_above > vol || sell_below > vol) continue;
                }
                int64_t imb = bv > sv ? bv - sv : sv - bv;
                if (vol > best_vol || (vol == best_vol && imb < best_imb)) {
                    best_vol = vol; best_imb = imb; lo_idx = hi_idx = i;
                } else if (vol == best_vol && imb == best_imb) {
                    hi_idx = i;
                }
            }
        }
        if (lo_idx < 0) return r;
        r.crossed = true;
        r.price = lo_ + (lo_idx + hi_idx + 1) / 2;  // midpoint, half-up
        r.volume = best_vol;
        r.imbalance = best_imb;
        return r;
    }

private:
    struct Node {
        OrderId id;
        Qty qty;
        uint32_t prev, next;
        int32_t level;  // index into side array
        Side side;
    };
    struct Level {
        uint32_t head = NIL, tail = NIL;
        int64_t total = 0;
        uint32_t count = 0;
    };

    // ---------------------------------------------------------- new order
    template <class L>
    void on_new(const OrderMsg& m, L& lis) {
        if (m.qty <= 0) return lis.on_reject(m.id, RejectReason::BadQty);
        if (phase_ == Phase::Closed) return lis.on_reject(m.id, RejectReason::Closed);
        const bool market = m.ord_type == OrdType::MarketBest5IOC;
        if (market && is_auction(phase_)) return lis.on_reject(m.id, RejectReason::MarketInAuction);
        if (!market && (m.price < info_.lower_limit || m.price > info_.upper_limit))
            return lis.on_reject(m.id, RejectReason::OutOfBand);
        if (index_.find(m.id)) return lis.on_reject(m.id, RejectReason::DuplicateId);

        Qty left = m.qty;
        if (phase_ == Phase::Continuous) {
            left = (m.side == Side::Buy) ? sweep<Side::Buy>(m, market, lis)
                                         : sweep<Side::Sell>(m, market, lis);
        }
        if (left > 0 && !market) {
            rest(m.id, m.side, m.price - lo_, left);
            lis.on_rest(m.id, m.side, m.price, left, m.time);
        }
        // market remainder is cancelled (IOC); never rests, so no L2 'A'/'D'.
    }

    // Match an aggressive order against the opposite side; returns leftover.
    template <Side S, class L>
    Qty sweep(const OrderMsg& m, bool market, L& lis) {
        Qty left = m.qty;
        int levels_used = 0;
        const int limit_idx = m.price - lo_;
        while (left > 0) {
            int idx = (S == Side::Buy) ? best_ask_ : best_bid_;
            if (idx < 0) break;
            if (market) {
                if (levels_used == 5) break;
            } else if (S == Side::Buy ? idx > limit_idx : idx < limit_idx) {
                break;
            }
            ++levels_used;
            Level& lv = (S == Side::Buy) ? asks_[idx] : bids_[idx];
            const Price px = lo_ + idx;
            while (left > 0 && lv.head != NIL) {
                uint32_t ni = lv.head;
                Node& rn = pool_[ni];
                Qty q = rn.qty < left ? rn.qty : left;
                left -= q;
                rn.qty -= q;
                lv.total -= q;
                if (S == Side::Buy) lis.on_trade(Trade{m.id, rn.id, px, q, 'B', m.time});
                else lis.on_trade(Trade{rn.id, m.id, px, q, 'S', m.time});
                if (rn.qty == 0) unlink(ni, lv, S == Side::Buy ? Side::Sell : Side::Buy);
            }
        }
        return left;
    }

    // ------------------------------------------------------------- cancel
    template <class L>
    void on_cancel_req(const OrderMsg& m, L& lis) {
        const uint32_t* p = index_.find(m.id);
        if (!p) return lis.on_reject(m.id, RejectReason::UnknownOrder);
        uint32_t ni = *p;
        Node& n = pool_[ni];
        Side s = n.side;
        int idx = n.level;
        Qty q = n.qty;
        Level& lv = (s == Side::Buy) ? bids_[idx] : asks_[idx];
        lv.total -= q;
        unlink(ni, lv, s);
        lis.on_cancel(m.id, s, lo_ + idx, q, m.time);
    }

    // -------------------------------------------------------------- phase
    template <class L>
    void on_phase_change(Phase np, uint32_t time, L& lis) {
        if (is_auction(phase_) && !is_auction(np)) uncross(time, lis);
        phase_ = np;
        lis.on_phase(np, time);
    }

    template <class L>
    void uncross(uint32_t time, L& lis) {
        AuctionResult r = auction_price();
        if (!r.crossed) return;
        const int p = r.price - lo_;
        // Price priority across levels, time priority (FIFO) within a level.
        while (best_bid_ >= p && best_ask_ >= 0 && best_ask_ <= p) {
            Level& bl = bids_[best_bid_];
            Level& al = asks_[best_ask_];
            uint32_t bi = bl.head, ai = al.head;
            Node& bn = pool_[bi];
            Node& an = pool_[ai];
            Qty q = bn.qty < an.qty ? bn.qty : an.qty;
            bn.qty -= q; an.qty -= q;
            bl.total -= q; al.total -= q;
            lis.on_trade(Trade{bn.id, an.id, r.price, q, 'N', time});
            if (bn.qty == 0) unlink(bi, bl, Side::Buy);
            if (an.qty == 0) unlink(ai, al, Side::Sell);
        }
    }

    // ------------------------------------------------------- book plumbing
    void rest(OrderId id, Side s, int idx, Qty q) {
        uint32_t ni;
        if (!free_.empty()) { ni = free_.back(); free_.pop_back(); }
        else { ni = static_cast<uint32_t>(pool_.size()); pool_.emplace_back(); }
        Level& lv = (s == Side::Buy) ? bids_[idx] : asks_[idx];
        pool_[ni] = Node{id, q, lv.tail, NIL, idx, s};
        if (lv.tail != NIL) pool_[lv.tail].next = ni; else lv.head = ni;
        lv.tail = ni;
        lv.total += q;
        if (lv.count++ == 0) {
            auto& bits = (s == Side::Buy) ? bid_bits_ : ask_bits_;
            bits[idx >> 6] |= 1ULL << (idx & 63);
            if (s == Side::Buy) { if (idx > best_bid_) best_bid_ = idx; }
            else if (best_ask_ < 0 || idx < best_ask_) best_ask_ = idx;
        }
        index_.insert(id, ni);
    }

    // Remove node from its level (qty already accounted in lv.total).
    void unlink(uint32_t ni, Level& lv, Side s) {
        Node& n = pool_[ni];
        if (n.prev != NIL) pool_[n.prev].next = n.next; else lv.head = n.next;
        if (n.next != NIL) pool_[n.next].prev = n.prev; else lv.tail = n.prev;
        index_.erase(n.id);
        free_.push_back(ni);
        if (--lv.count == 0) {
            int idx = n.level;
            auto& bits = (s == Side::Buy) ? bid_bits_ : ask_bits_;
            bits[idx >> 6] &= ~(1ULL << (idx & 63));
            if (s == Side::Buy) { if (idx == best_bid_) best_bid_ = highest_set_at_or_below(bid_bits_, idx); }
            else if (idx == best_ask_) best_ask_ = lowest_set_at_or_above(ask_bits_, idx);
        }
    }

    static int highest_set_at_or_below(const std::vector<uint64_t>& bits, int idx) {
        int w = idx >> 6;
        uint64_t word = bits[w] & (((idx & 63) == 63) ? ~0ULL : ((2ULL << (idx & 63)) - 1));
        for (;;) {
            if (word) return (w << 6) + 63 - __builtin_clzll(word);
            if (--w < 0) return -1;
            word = bits[w];
        }
    }
    int lowest_set_at_or_above(const std::vector<uint64_t>& bits, int idx) const {
        int w = idx >> 6;
        const int nw = static_cast<int>(bits.size());
        uint64_t word = bits[w] & (~0ULL << (idx & 63));
        for (;;) {
            if (word) return (w << 6) + __builtin_ctzll(word);
            if (++w >= nw) return -1;
            word = bits[w];
        }
    }

    SymbolInfo info_;
    Price lo_;
    int nlevels_;
    std::vector<Level> bids_, asks_;
    std::vector<uint64_t> bid_bits_, ask_bits_;
    int best_bid_ = -1, best_ask_ = -1;
    std::vector<Node> pool_;
    std::vector<uint32_t> free_;
    FlatMap index_;
    Phase phase_ = Phase::Closed;
    mutable std::vector<int64_t> cum_buy_, cum_sell_;  // auction scratch
};

}  // namespace me
