// Reference order book: deliberately naive (std::map + std::deque) and written
// independently of OrderBook. Used only for differential testing — the fast
// book must produce exactly the same trades and the same resting orders.
#pragma once
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "me/types.h"

namespace me {

class RefBook {
public:
    explicit RefBook(const SymbolInfo& info, BookConfig cfg = {}) : info_(info), cfg_(cfg), last_(info.prev_close) {}

    std::vector<Trade> trades;

    void process(const OrderMsg& m) {
        if (m.type == MsgType::Phase) {
            if (is_auction(phase_) && !is_auction(m.phase)) uncross(m.time);
            phase_ = m.phase;
            return;
        }
        if (m.type == MsgType::Cancel) {
            auto it = loc_.find(m.id);
            if (it != loc_.end()) remove_order(it->second.first, it->second.second, m.id);
            return;
        }
        on_new(m);
    }

    size_t live_orders() const { return loc_.size(); }

    // price -> total qty
    std::map<Price, int64_t> depth(Side s) const {
        std::map<Price, int64_t> d;
        if (s == Side::Buy) { for (auto& [p, q] : bids_) for (auto& o : q) d[p] += o.qty; }
        else { for (auto& [p, q] : asks_) for (auto& o : q) d[p] += o.qty; }
        return d;
    }

    // Resting orders in priority order (same convention as OrderBook::for_each_order).
    std::vector<std::pair<OrderId, Qty>> orders() const {
        std::vector<std::pair<OrderId, Qty>> v;
        for (auto& [p, q] : bids_) for (auto& o : q) v.push_back({o.id, o.qty});
        for (auto& [p, q] : asks_) for (auto& o : q) v.push_back({o.id, o.qty});
        return v;
    }

private:
    struct O { OrderId id; Qty qty; AccountId acct; };

    void on_new(const OrderMsg& m) {
        if (m.qty <= 0 || phase_ == Phase::Closed || loc_.count(m.id)) return;
        const bool market = m.ord_type != OrdType::Limit;
        if (market && phase_ != Phase::Continuous) return;
        const bool buy = m.side == Side::Buy;
        const bool cont = phase_ == Phase::Continuous;

        Price px = m.price;
        bool five = false;
        switch (m.ord_type) {
            case OrdType::Limit:
                if (px < info_.lower_limit || px > info_.upper_limit) return;
                if (cont && !cage_ok(m.side, px)) return;
                break;
            case OrdType::OwnBest:
                if (buy ? bids_.empty() : asks_.empty()) return;
                px = buy ? bids_.begin()->first : asks_.begin()->first;
                break;
            case OrdType::CounterBest:
                if (buy ? asks_.empty() : bids_.empty()) return;
                px = buy ? asks_.begin()->first : bids_.begin()->first;
                break;
            case OrdType::MarketBest5ToLimit:
                if (buy ? asks_.empty() : bids_.empty()) return;
                five = true;
                break;
            case OrdType::MarketBest5IOC:
                five = true;
                break;
        }

        Qty left = m.qty;
        bool killed = false;
        Price last_fill = 0;
        if (cont) {
            if (buy) match_side(asks_, m, five, px, left, killed, last_fill, [&](Price p) { return p <= px; });
            else match_side(bids_, m, five, px, left, killed, last_fill, [&](Price p) { return p >= px; });
        }
        if (left == 0 || killed) return;
        if (m.ord_type == OrdType::MarketBest5IOC) return;
        Price rp = px;
        if (m.ord_type == OrdType::MarketBest5ToLimit) {
            if (last_fill == 0) return;  // nothing filled -> remainder dropped
            rp = last_fill;
        }
        (buy ? bids_[rp] : asks_[rp]).push_back({m.id, left, m.account});
        loc_[m.id] = {m.side, rp};
    }

    template <class Book, class Ok>
    void match_side(Book& book, const OrderMsg& m, bool five, Price, Qty& left, bool& killed, Price& last_fill,
                    Ok price_ok) {
        const bool buyer = m.side == Side::Buy;
        const bool stp_on = m.account != 0 && cfg_.stp != StpPolicy::None;
        const bool cancel_resting = cfg_.stp == StpPolicy::CancelOldest || cfg_.stp == StpPolicy::CancelBoth;
        const bool kill_incoming = cfg_.stp == StpPolicy::CancelNewest || cfg_.stp == StpPolicy::CancelBoth;
        int levels = 0;
        while (left > 0 && !book.empty()) {
            auto it = book.begin();
            const Price p = it->first;
            if (five ? levels == 5 : !price_ok(p)) break;
            ++levels;
            auto& q = it->second;
            while (left > 0 && !q.empty()) {
                O& o = q.front();
                if (stp_on && o.acct == m.account) {
                    if (cancel_resting) { loc_.erase(o.id); q.pop_front(); }
                    if (kill_incoming) { killed = true; break; }
                    continue;
                }
                Qty x = std::min(o.qty, left);
                o.qty -= x; left -= x;
                last_ = last_fill = p;
                if (buyer) trades.push_back({m.id, o.id, p, x, 'B', m.time});
                else trades.push_back({o.id, m.id, p, x, 'S', m.time});
                if (o.qty == 0) { loc_.erase(o.id); q.pop_front(); }
            }
            if (q.empty()) book.erase(it);
            if (killed) return;
        }
    }

    bool cage_ok(Side s, Price p) const {
        if (!cfg_.cage.enabled) return true;
        // Written as plain floating-point bounds on purpose (independent of the
        // integer arithmetic in OrderBook::within_cage).
        Price bb = bids_.empty() ? -1 : bids_.begin()->first;
        Price ba = asks_.empty() ? -1 : asks_.begin()->first;
        const double pct = cfg_.cage.pct / 100.0;
        if (s == Side::Buy) {
            Price base = ba >= 0 ? ba : bb >= 0 ? bb : last_;
            return p <= std::max(base * (1 + pct), double(base + cfg_.cage.min_ticks)) + 1e-9;
        }
        Price base = bb >= 0 ? bb : ba >= 0 ? ba : last_;
        return p >= std::min(base * (1 - pct), double(base - cfg_.cage.min_ticks)) - 1e-9;
    }

    void remove_order(Side side, Price px, OrderId id) {
        if (side == Side::Buy) erase_from(bids_, px, id); else erase_from(asks_, px, id);
        loc_.erase(id);
    }
    template <class Book>
    static void erase_from(Book& book, Price px, OrderId id) {
        auto& q = book[px];
        for (auto o = q.begin(); o != q.end(); ++o)
            if (o->id == id) { q.erase(o); break; }
        if (q.empty()) book.erase(px);
    }

    void uncross(uint32_t time) {
        // Gather every declared price and evaluate the exchange rules by brute force.
        std::map<Price, int64_t> bid_at, ask_at;
        for (auto& [p, q] : bids_) for (auto& o : q) bid_at[p] += o.qty;
        for (auto& [p, q] : asks_) for (auto& o : q) ask_at[p] += o.qty;
        std::vector<Price> prices;
        for (auto& [p, _] : bid_at) prices.push_back(p);
        for (auto& [p, _] : ask_at) prices.push_back(p);
        std::sort(prices.begin(), prices.end());
        prices.erase(std::unique(prices.begin(), prices.end()), prices.end());

        auto eval = [&](Price p, int64_t& bv, int64_t& sv, int64_t& above, int64_t& below) {
            bv = sv = above = below = 0;
            for (auto& [bp, q] : bid_at) { if (bp >= p) bv += q; if (bp > p) above += q; }
            for (auto& [ap, q] : ask_at) { if (ap <= p) sv += q; if (ap < p) below += q; }
        };
        std::vector<Price> best;
        int64_t best_vol = 0, best_imb = 0;
        for (int pass = 0; pass < 2 && best.empty(); ++pass) {
            for (Price p : prices) {
                int64_t bv, sv, above, below;
                eval(p, bv, sv, above, below);
                int64_t vol = std::min(bv, sv);
                if (vol <= 0) continue;
                if (pass == 0 && (above > vol || below > vol)) continue;
                int64_t imb = std::llabs(bv - sv);
                if (vol > best_vol || (vol == best_vol && imb < best_imb)) {
                    best_vol = vol; best_imb = imb; best.assign(1, p);
                } else if (vol == best_vol && imb == best_imb) {
                    best.push_back(p);
                }
            }
        }
        if (best.empty()) return;
        Price P;
        if (cfg_.venue == Venue::SZSE) {
            const Price ref = phase_ == Phase::OpeningAuction ? info_.prev_close : last_;
            P = best.front();
            for (Price c : best)  // strictly closer wins; ascending order keeps the lower on ties
                if (std::abs(c - ref) < std::abs(P - ref)) P = c;
        } else {
            P = (best.front() + best.back() + 1) / 2;
        }
        last_ = P;
        while (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= P && asks_.begin()->first <= P) {
            auto& bq = bids_.begin()->second;
            auto& aq = asks_.begin()->second;
            O& b = bq.front();
            O& a = aq.front();
            Qty x = std::min(b.qty, a.qty);
            b.qty -= x; a.qty -= x;
            trades.push_back({b.id, a.id, P, x, 'N', time});
            if (b.qty == 0) { loc_.erase(b.id); bq.pop_front(); if (bq.empty()) bids_.erase(bids_.begin()); }
            if (a.qty == 0) { loc_.erase(a.id); aq.pop_front(); if (aq.empty()) asks_.erase(asks_.begin()); }
        }
    }

    SymbolInfo info_;
    BookConfig cfg_;
    Price last_;
    Phase phase_ = Phase::Closed;
    std::map<Price, std::deque<O>, std::greater<Price>> bids_;
    std::map<Price, std::deque<O>> asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>> loc_;
};

}  // namespace me
