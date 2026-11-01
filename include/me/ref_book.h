// Reference order book: deliberately naive (std::map + std::deque) and written
// independently of OrderBook. Used only for differential testing — the fast
// book must produce exactly the same trade stream as this one.
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
    explicit RefBook(const SymbolInfo& info, CageConfig cage = {}) : info_(info), cage_(cage), last_(info.prev_close) {}

    std::vector<Trade> trades;

    void process(const OrderMsg& m) {
        if (m.type == MsgType::Phase) {
            if (is_auction(phase_) && !is_auction(m.phase)) uncross(m.time);
            phase_ = m.phase;
            return;
        }
        if (m.type == MsgType::Cancel) {
            auto it = loc_.find(m.id);
            if (it == loc_.end()) return;
            auto [side, px] = it->second;
            auto& q = side == Side::Buy ? bids_[px] : asks_[px];
            for (auto o = q.begin(); o != q.end(); ++o)
                if (o->id == m.id) { q.erase(o); break; }
            if (q.empty()) { if (side == Side::Buy) bids_.erase(px); else asks_.erase(px); }
            loc_.erase(it);
            return;
        }
        // New
        if (m.qty <= 0 || phase_ == Phase::Closed) return;
        bool market = m.ord_type == OrdType::MarketBest5IOC;
        if (market && is_auction(phase_)) return;
        if (!market && (m.price < info_.lower_limit || m.price > info_.upper_limit)) return;
        if (loc_.count(m.id)) return;
        if (!market && phase_ == Phase::Continuous && cage_.enabled) {
            // Written as plain floating-point bounds on purpose (independent of
            // the integer arithmetic in OrderBook::within_cage).
            auto best = [](auto& mp) { return mp.empty() ? -1 : mp.begin()->first; };
            Price bb = best(bids_), ba = best(asks_);
            if (m.side == Side::Buy) {
                Price base = ba >= 0 ? ba : bb >= 0 ? bb : last_;
                double hi = std::max(base * (1 + cage_.pct / 100.0), double(base + cage_.min_ticks));
                if (m.price > hi + 1e-9) return;
            } else {
                Price base = bb >= 0 ? bb : ba >= 0 ? ba : last_;
                double lo = std::min(base * (1 - cage_.pct / 100.0), double(base - cage_.min_ticks));
                if (m.price < lo - 1e-9) return;
            }
        }
        Qty left = m.qty;
        if (phase_ == Phase::Continuous) {
            int levels = 0;
            if (m.side == Side::Buy) {
                while (left > 0 && !asks_.empty()) {
                    auto it = asks_.begin();
                    if (market ? levels == 5 : it->first > m.price) break;
                    ++levels;
                    left = eat(it->second, left, it->first, m, true);
                    if (it->second.empty()) asks_.erase(it);
                }
            } else {
                while (left > 0 && !bids_.empty()) {
                    auto it = bids_.begin();
                    if (market ? levels == 5 : it->first < m.price) break;
                    ++levels;
                    left = eat(it->second, left, it->first, m, false);
                    if (it->second.empty()) bids_.erase(it);
                }
            }
        }
        if (left > 0 && !market) {
            (m.side == Side::Buy ? bids_[m.price] : asks_[m.price]).push_back({m.id, left});
            loc_[m.id] = {m.side, m.price};
        }
    }

    size_t live_orders() const { return loc_.size(); }

    // price -> total qty
    std::map<Price, int64_t> depth(Side s) const {
        std::map<Price, int64_t> d;
        if (s == Side::Buy) { for (auto& [p, q] : bids_) for (auto& o : q) d[p] += o.qty; }
        else { for (auto& [p, q] : asks_) for (auto& o : q) d[p] += o.qty; }
        return d;
    }

private:
    struct O { OrderId id; Qty qty; };

    Qty eat(std::deque<O>& q, Qty left, Price px, const OrderMsg& m, bool buyer_aggr) {
        while (left > 0 && !q.empty()) {
            O& o = q.front();
            Qty x = std::min(o.qty, left);
            o.qty -= x; left -= x;
            last_ = px;
            if (buyer_aggr) trades.push_back({m.id, o.id, px, x, 'B', m.time});
            else trades.push_back({o.id, m.id, px, x, 'S', m.time});
            if (o.qty == 0) { loc_.erase(o.id); q.pop_front(); }
        }
        return left;
    }

    void uncross(uint32_t time) {
        // Gather every declared price and evaluate the SSE rules by brute force.
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
        Price P = (best.front() + best.back() + 1) / 2;
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
    CageConfig cage_;
    Price last_;
    Phase phase_ = Phase::Closed;
    std::map<Price, std::deque<O>, std::greater<Price>> bids_;
    std::map<Price, std::deque<O>> asks_;
    std::unordered_map<OrderId, std::pair<Side, Price>> loc_;
};

}  // namespace me
