// Backtest fill simulation on Level-2 tick-by-tick data.
//
// A strategy's orders are simulated against a recorded market: the book is
// rebuilt from the tick feed and each own order is given a position in the
// price-level queue. Because SSE/SZSE tick data identify every order, the
// queue can be tracked order by order instead of being estimated:
//
//   queue model (FillModel::Queue)
//     * on arrival, the orders resting at our price are "ahead" of us.
//       Exchange order numbers increase with arrival time, so this is stored
//       as a single cutoff: the last order number in the level's queue;
//     * a trade at our price against an order numbered above the cutoff (it
//       arrived after us) would have hit us first -> we fill; one numbered
//       at or below the cutoff only moves the queue in front of us;
//     * a trade at a worse price than ours (the aggressor swept past our
//       level) -> we fill;
//     * an opposite order that ends up resting at a price crossing ours would
//       have traded with us -> we fill at our price;
//     * an own order that is marketable on arrival takes visible liquidity
//       from the rebuilt book, level by level; what we took is remembered so
//       the same liquidity is never used twice.
//   touch model:   fill completely as soon as a trade prints at our price
//   through model: fill completely only when a trade prints beyond our price
//
// The simulation cannot see its own market impact (the recorded aggressor
// still trades with the order behind us). To measure how much that matters,
// TruthWorld runs the same strategy with its orders really inserted into the
// matching engine together with the original order flow, i.e. the ground
// truth that a recorded feed can only approximate.
//
// Both worlds are driven by the same Driver and strategy code: decisions are
// taken on a timer (state = all events strictly before the timer), orders
// arrive after a latency, and are cancelled after a time-to-live.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "me/consistent_hash.h"
#include "me/flow.h"
#include "me/l2.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/types.h"

namespace me::bt {

// HHMMSSmmm -> milliseconds since midnight
inline int64_t to_ms(uint32_t t) {
    return int64_t(t / 10000000) * 3600000 + int64_t(t / 100000 % 100) * 60000 + int64_t(t / 1000 % 100) * 1000 +
           int64_t(t % 1000);
}
inline uint32_t from_ms(int64_t ms) {
    int64_t s = ms / 1000;
    return uint32_t((s / 3600) * 10000000 + (s / 60 % 60) * 100000 + (s % 60) * 1000 + ms % 1000);
}

inline constexpr OrderId kOwnBase = 1ULL << 62;
inline constexpr AccountId kOwnAccount = 0x4F574E;  // "OWN"
inline bool is_own(OrderId id) { return id >= kOwnBase; }
inline Side other(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }

struct View {
    bool has_bid = false, has_ask = false;
    Price bid = 0, ask = 0;
};

struct Params {
    int64_t interval_ms = 5000;  // decision timer
    int64_t latency_ms = 20;     // decision -> order reaches the exchange
    int64_t ttl_ms = 30000;      // unfilled orders are cancelled after this
    double take_ratio = 0.1;     // share of decisions that cross the spread
    int64_t start_ms = to_ms(93000000), end_ms = to_ms(145600000);
    int64_t lunch_start = to_ms(113000000), lunch_end = to_ms(130000000);
    Qty base_position = 1'000'000'000;  // 底仓 per security: T+1, only this may be sold today
    double commission_bps = 0, stamp_tax_bps = 0;  // stamp tax: sells only
};

struct OwnOrder {
    SymbolId sym = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;
    bool take = false;
    int64_t decided_ms = 0;
    Qty filled = 0;
    bool placed = false, rejected = false, live = false;
};

struct FillRec {
    OrderId id;
    SymbolId sym;
    Side side;
    Price price;
    Qty qty;
    int64_t t_ms;
};

struct Result {
    std::unordered_map<OrderId, OwnOrder> orders;
    std::vector<FillRec> fills;
    double pnl = 0, fees = 0, turnover = 0;
    int64_t bought = 0, sold = 0;
    uint64_t events = 0;
    double seconds = 0;
};

class FillSink {
public:
    virtual ~FillSink() = default;
    virtual void on_fill(OrderId id, Price px, Qty q, int64_t t) = 0;
    virtual void on_done(OrderId id) = 0;  // order no longer live (filled / cancelled / rejected)
};

// ------------------------------------------------------------------ strategy
// Example strategy used for validation: every timer tick, for every security,
// quote one lot-sized bid at the best bid and one ask at the best ask (join
// the queue); with probability take_ratio instead send one order that crosses
// the spread. Decisions depend only on (security, tick, market view), never
// on own fills, so both worlds take identical decisions whenever they see the
// same market.
struct JoinBestStrategy {
    double take_ratio = 0.1;
    template <class Emit>
    void on_timer(int64_t tick, SymbolId sym, const View& v, Qty sellable, Emit&& emit) const {
        const uint64_t r = mix64((uint64_t(sym) << 32) ^ uint64_t(tick) ^ 0xB5A9C3ULL);
        const Qty q = static_cast<Qty>(100 * (1 + (r >> 20) % 3));
        const bool take = double(r % 10000) < take_ratio * 10000;
        const OrderId base = kOwnBase + (uint64_t(sym) << 36) + uint64_t(tick) * 4;
        if (take) {
            if ((r >> 12) & 1) { if (v.has_ask) emit(base, Side::Buy, v.ask, q, true); }
            else if (v.has_bid && sellable >= q) emit(base, Side::Sell, v.bid, q, true);
        } else {
            if (v.has_bid) emit(base + 1, Side::Buy, v.bid, q, false);
            if (v.has_ask && sellable >= q) emit(base + 2, Side::Sell, v.ask, q, false);
        }
    }
};

// -------------------------------------------------------------------- driver
template <class World, class Strategy>
class Driver final : public FillSink {
public:
    Driver(World& w, const Strategy& s, const Params& p, size_t nsym)
        : w_(w), s_(s), p_(p), pos_(nsym, 0), cash_(nsym, 0.0), sold_(nsym, 0), pend_sell_(nsym, 0) {
        next_timer_ = p_.start_ms + p_.interval_ms;
        normalize_timer();
        w_.attach(this);
    }

    // Call before the world processes a market event stamped t (ms).
    void before_event(int64_t t) {
        if (t > last_t_) {
            w_.prepare();
            last_t_ = t;
        }
        for (;;) {
            const int64_t ta = q_.empty() ? kNever : q_.top().t;
            const int64_t nx = std::min(ta, next_timer_);
            if (nx > t) return;
            now_ = nx;
            if (ta <= next_timer_) {  // arrivals before a timer at the same instant
                Item it = q_.top();
                q_.pop();
                apply(it);
            } else {
                fire(next_timer_);
                next_timer_ += p_.interval_ms;
                normalize_timer();
            }
        }
    }

    // Call before the world processes a phase change of `sym`.
    void phase_event(SymbolId sym, bool to_continuous, int64_t t) {
        if (w_.continuous(sym) && !to_continuous) w_.cancel_all(sym, t);
    }

    Result finish(const std::vector<Price>& last_px) {
        for (size_t s = 0; s < pos_.size(); ++s) res_.pnl += cash_[s] + double(pos_[s]) * last_px[s] / 100.0;
        return std::move(res_);
    }

    void on_fill(OrderId id, Price px, Qty q, int64_t t) override {
        OwnOrder& o = res_.orders.at(id);
        o.filled += q;
        res_.fills.push_back(FillRec{id, o.sym, o.side, px, q, t});
        const double notional = double(px) * q / 100.0;
        double fee = notional * p_.commission_bps / 1e4;
        if (o.side == Side::Buy) {
            pos_[o.sym] += q;
            cash_[o.sym] -= notional;
            res_.bought += q;
        } else {
            fee += notional * p_.stamp_tax_bps / 1e4;
            pos_[o.sym] -= q;
            cash_[o.sym] += notional;
            sold_[o.sym] += q;
            pend_sell_[o.sym] -= q;
            res_.sold += q;
        }
        cash_[o.sym] -= fee;
        res_.fees += fee;
        res_.turnover += notional;
    }

    void on_done(OrderId id) override {
        OwnOrder& o = res_.orders.at(id);
        if (!o.live) return;
        o.live = false;
        if (o.side == Side::Sell) pend_sell_[o.sym] -= o.qty - o.filled;
    }

private:
    static constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
    struct Item {
        int64_t t;
        uint64_t seq;
        OrderId id;
        bool place;
        bool operator>(const Item& o) const { return t != o.t ? t > o.t : seq > o.seq; }
    };

    void normalize_timer() {
        if (next_timer_ >= p_.lunch_start && next_timer_ < p_.lunch_end) next_timer_ = p_.lunch_end;
        if (next_timer_ > p_.end_ms) next_timer_ = kNever;
    }

    void fire(int64_t T) {
        const size_t n = pos_.size();
        for (SymbolId sym = 0; sym < n; ++sym) {
            if (!w_.continuous(sym)) continue;
            const View v = w_.view(sym);
            const Qty sellable = static_cast<Qty>(
                std::max<int64_t>(0, int64_t(p_.base_position) - sold_[sym] - pend_sell_[sym]));
            s_.on_timer(tick_, sym, v, sellable, [&](OrderId id, Side side, Price px, Qty q, bool take) {
                OwnOrder o;
                o.sym = sym; o.side = side; o.price = px; o.qty = q; o.take = take; o.decided_ms = T;
                res_.orders[id] = o;
                if (side == Side::Sell) pend_sell_[sym] += q;
                q_.push(Item{T + p_.latency_ms, ++seq_, id, true});
                q_.push(Item{T + p_.ttl_ms + p_.latency_ms, ++seq_, id, false});
            });
        }
        ++tick_;
    }

    void apply(const Item& it) {
        OwnOrder& o = res_.orders.at(it.id);
        if (it.place) {
            if (!w_.continuous(o.sym)) {  // only continuous trading
                o.rejected = true;
                if (o.side == Side::Sell) pend_sell_[o.sym] -= o.qty;
                return;
            }
            o.placed = o.live = true;
            w_.place(it.id, o.sym, o.side, o.price, o.qty, now_);
        } else if (o.live) {
            w_.cancel(it.id, now_);
        }
    }

    World& w_;
    const Strategy& s_;
    Params p_;
    Result res_;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q_;
    std::vector<int64_t> pos_;
    std::vector<double> cash_;
    std::vector<int64_t> sold_, pend_sell_;
    int64_t next_timer_ = 0, last_t_ = -1, now_ = 0, tick_ = 0;
    uint64_t seq_ = 0;
};

// --------------------------------------------------------------- truth world
// Own orders are real orders inside the matching engine, competing with the
// original order flow. Market accounts must be anonymised (account 0) so that
// self-trade prevention (CancelOldest) only ever applies to own orders.
class TruthWorld {
public:
    TruthWorld(const std::vector<SymbolInfo>& syms, BookConfig cfg) : lis_{this}, live_(syms.size()) {
        cfg.stp = StpPolicy::CancelOldest;
        books_.reserve(syms.size());
        for (const SymbolInfo& s : syms) books_.emplace_back(s, 1 << 14, cfg);
    }
    void attach(FillSink* s) { sink_ = s; }
    void prepare() {}

    void market(const OrderMsg& m, int64_t t) {
        now_ = t;
        books_[m.sym].process(m, lis_);
    }
    bool continuous(SymbolId s) const { return books_[s].phase() == Phase::Continuous; }
    View view(SymbolId s) const {
        View v;
        auto keep = [](OrderId id, AccountId) { return !is_own(id); };
        v.has_bid = books_[s].best_where(Side::Buy, keep, v.bid);
        v.has_ask = books_[s].best_where(Side::Sell, keep, v.ask);
        return v;
    }
    void place(OrderId id, SymbolId sym, Side side, Price px, Qty q, int64_t t) {
        now_ = t;
        OrderMsg m;
        m.id = id; m.sym = sym; m.side = side; m.price = px; m.qty = q; m.account = kOwnAccount;
        m.time = from_ms(t);
        rem_[id] = {sym, q};
        live_[sym].insert(id);
        books_[sym].process(m, lis_);
    }
    void cancel(OrderId id, int64_t t) {
        auto it = rem_.find(id);
        if (it == rem_.end()) return;
        now_ = t;
        OrderMsg m;
        m.type = MsgType::Cancel; m.id = id; m.sym = it->second.sym; m.time = from_ms(t);
        books_[m.sym].process(m, lis_);
    }
    void cancel_all(SymbolId sym, int64_t t) {
        std::vector<OrderId> ids(live_[sym].begin(), live_[sym].end());
        std::sort(ids.begin(), ids.end());
        for (OrderId id : ids) cancel(id, t);
    }
    Price last_price(SymbolId s) const { return books_[s].last_price(); }
    size_t size() const { return books_.size(); }

private:
    struct Rem { SymbolId sym; Qty qty; };
    void fill(OrderId id, Price px, Qty q) {
        auto it = rem_.find(id);
        if (it == rem_.end()) return;
        it->second.qty -= q;
        sink_->on_fill(id, px, q, now_);
        if (it->second.qty == 0) done(id);
    }
    void done(OrderId id) {
        auto it = rem_.find(id);
        if (it == rem_.end()) return;
        live_[it->second.sym].erase(id);
        rem_.erase(it);
        sink_->on_done(id);
    }
    struct Listener {
        TruthWorld* w;
        void on_trade(const Trade& t) {
            if (is_own(t.buy_id)) w->fill(t.buy_id, t.price, t.qty);
            if (is_own(t.sell_id)) w->fill(t.sell_id, t.price, t.qty);
        }
        void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
        void on_cancel(OrderId id, Side, Price, Qty, uint32_t) { if (is_own(id)) w->done(id); }
        void on_expire(OrderId id, Side, Qty, ExpireReason, uint32_t) { if (is_own(id)) w->done(id); }
        void on_reject(OrderId id, RejectReason) { if (is_own(id)) w->done(id); }
        void on_phase(Phase, uint32_t) {}
    };

    Listener lis_;
    std::vector<OrderBook> books_;
    std::vector<std::unordered_set<OrderId>> live_;
    std::unordered_map<OrderId, Rem> rem_;
    FillSink* sink_ = nullptr;
    int64_t now_ = 0;
};

// ----------------------------------------------------------------- sim world
enum class FillModel : uint8_t { Queue, Touch, Through };

inline const char* model_name(FillModel m) {
    return m == FillModel::Queue ? "queue" : m == FillModel::Touch ? "touch" : "through";
}

class SimWorld {
public:
    SimWorld(size_t nsym, Venue v, FillModel m)
        : rb_(v), model_(m), own_(nsym), consumed_(nsym), cont_(nsym, 0), dirty_(nsym, 0), last_px_(nsym, 0) {}
    void attach(FillSink* s) { sink_ = s; }

    // ---- market side
    void record(const TickRecord& r, int64_t t) {
        now_ = t;
        const SymbolId sym = r.sym;
        switch (r.type) {
            case 'S':
                cont_[sym] = r.bs == 'T';
                break;
            case 'T':
                last_px_[sym] = r.price;
                if (r.bs != 'N' && !own_[sym].empty()) on_trade(sym, r);
                break;
            default: break;
        }
        rb_.apply(r);
        mark_dirty(sym);
    }

    // Called by the driver whenever the exchange timestamp advances: complete
    // pending SZSE market orders, then let own orders trade with any opposite
    // order that came to rest at a crossing price.
    void prepare() {
        rb_.flush_pending();
        for (SymbolId s : dirty_list_) {
            dirty_[s] = 0;
            settle(s);
        }
        dirty_list_.clear();
    }

    bool continuous(SymbolId s) const { return cont_[s] != 0; }

    View view(SymbolId s) const {
        View v;
        v.has_bid = best_avail(s, Side::Buy, v.bid);
        v.has_ask = best_avail(s, Side::Sell, v.ask);
        return v;
    }

    void place(OrderId id, SymbolId sym, Side side, Price px, Qty q, int64_t t) {
        now_ = t;
        Own o{id, side, px, q, ++seq_, 0};
        take(sym, o, /*aggressive=*/true);
        if (o.rem == 0) { sink_->on_done(id); return; }
        if (const auto* qv = rb_.queue(sym, side, px)) if (!qv->empty()) o.cutoff = qv->back();
        own_[sym].push_back(o);
        index_[id] = sym;
    }

    void cancel(OrderId id, int64_t t) {
        auto it = index_.find(id);
        if (it == index_.end()) return;
        now_ = t;
        remove(it->second, id);
    }

    void cancel_all(SymbolId sym, int64_t t) {
        now_ = t;
        std::vector<OrderId> ids;
        for (const Own& o : own_[sym]) ids.push_back(o.id);
        for (OrderId id : ids) remove(sym, id);
    }

    Price last_price(SymbolId s) const { return last_px_[s]; }
    const L2Rebuilder& rebuilder() const { return rb_; }

private:
    struct Own {
        OrderId id;
        Side side;
        Price price;
        Qty rem;
        uint64_t seq;
        OrderId cutoff;  // market orders numbered <= cutoff at our price were ahead of us
    };

    static bool better(Side s, Price a, Price b) { return s == Side::Buy ? a > b : a < b; }
    static bool crosses(Side s, Price own_px, Price opp_px) { return s == Side::Buy ? opp_px <= own_px : opp_px >= own_px; }

    void mark_dirty(SymbolId s) {
        if (!dirty_[s] && !own_[s].empty()) { dirty_[s] = 1; dirty_list_.push_back(s); }
    }

    Qty consumed_qty(SymbolId sym, OrderId id) const {
        auto it = consumed_[sym].find(id);
        return it == consumed_[sym].end() ? 0 : it->second;
    }

    // Best price of a side counting only liquidity we have not taken ourselves.
    bool best_avail(SymbolId sym, Side s, Price& out) const {
        const auto* qs = rb_.queues(sym, s);
        if (!qs) return false;
        auto ok = [&](const std::vector<OrderId>& q) {
            for (OrderId id : q) if (rb_.order_qty(sym, id) > consumed_qty(sym, id)) return true;
            return false;
        };
        if (s == Side::Buy) {
            for (auto it = qs->rbegin(); it != qs->rend(); ++it) if (ok(it->second)) { out = it->first; return true; }
        } else {
            for (auto it = qs->begin(); it != qs->end(); ++it) if (ok(it->second)) { out = it->first; return true; }
        }
        return false;
    }

    void fill(SymbolId, Own& o, Qty q, Price px) {
        o.rem -= q;
        sink_->on_fill(o.id, px, q, now_);
    }

    void remove(SymbolId sym, OrderId id) {
        auto& v = own_[sym];
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == id) {
                v.erase(v.begin() + static_cast<long>(i));
                index_.erase(id);
                sink_->on_done(id);
                return;
            }
    }

    void drop_filled(SymbolId sym) {
        auto& v = own_[sym];
        for (size_t i = 0; i < v.size();) {
            if (v[i].rem == 0) {
                const OrderId id = v[i].id;
                v.erase(v.begin() + static_cast<long>(i));
                index_.erase(id);
                sink_->on_done(id);
            } else {
                ++i;
            }
        }
    }

    // Own orders of side s in exchange priority: better price first, then older.
    std::vector<Own*>& by_priority(SymbolId sym, Side s) {
        std::vector<Own*>& v = prio_buf_;
        v.clear();
        for (Own& o : own_[sym]) if (o.side == s && o.rem > 0) v.push_back(&o);
        std::sort(v.begin(), v.end(), [s](const Own* a, const Own* b) {
            return a->price != b->price ? better(s, a->price, b->price) : a->seq < b->seq;
        });
        return v;
    }

    // A market trade: the resting order `rid` (side rs) traded qty x at price q.
    void on_trade(SymbolId sym, const TickRecord& r) {
        const Side rs = r.bs == 'S' ? Side::Buy : Side::Sell;  // resting side = aggressor's opposite
        const OrderId rid = rs == Side::Buy ? r.buy_no : r.sell_no;
        const Price q = r.price;
        Qty x = r.qty;
        if (model_ != FillModel::Queue) {
            for (Own* o : by_priority(sym, rs)) {
                const bool hit = model_ == FillModel::Touch ? (o->price == q || better(rs, o->price, q))
                                                            : better(rs, o->price, q);
                if (hit) fill(sym, *o, o->rem, o->price);
            }
            drop_filled(sym);
            return;
        }
        // by_priority order = exchange priority; an order whose queue is still in
        // front of the traded order is skipped (the trade only moved its queue).
        for (Own* o : by_priority(sym, rs)) {
            if (x == 0) break;
            const bool eligible = better(rs, o->price, q)               // aggressor swept past our level
                                  || (o->price == q && rid > o->cutoff);  // traded order arrived after us
            if (!eligible) continue;
            const Qty f = std::min(o->rem, x);
            fill(sym, *o, f, o->price);
            x -= f;
        }
        drop_filled(sym);
    }

    // Own order o trades with visible opposite liquidity.
    //   aggressive = true : o is arriving (taker), fills at the resting prices;
    //                       own opposite orders reached by the sweep are
    //                       cancelled (self-trade prevention, CancelOldest).
    //   aggressive = false: an opposite order came to rest across our resting
    //                       order (it would have traded with us), fills at our price.
    void take(SymbolId sym, Own& o, bool aggressive) {
        const Side opp = other(o.side);
        auto& cons = consumed_[sym];
        bool has_opp_own = false;
        if (aggressive) for (const Own& x : own_[sym]) if (x.side == opp) { has_opp_own = true; break; }
        // Own opposite orders the sweep reaches before market order next_market
        // (0 = reaches all crossing ones) are cancelled.
        auto stp_reach = [&](Price level, OrderId next_market) {
            if (!has_opp_own) return;
            victims_.clear();
            for (const Own& x : own_[sym]) {
                if (x.side != opp || x.rem == 0) continue;
                if (better(opp, x.price, level) || (x.price == level && (!next_market || next_market > x.cutoff)))
                    victims_.push_back(x.id);
            }
            for (OrderId id : victims_) remove(sym, id);
            has_opp_own = false;
            for (const Own& x : own_[sym]) if (x.side == opp) { has_opp_own = true; break; }
        };
        auto level = [&](Price L, const std::vector<OrderId>& fifo) {  // false = stop sweeping
            if (!crosses(o.side, o.price, L)) return false;
            for (OrderId mid : fifo) {
                const Qty avail = rb_.order_qty(sym, mid) - consumed_qty(sym, mid);
                if (avail <= 0) continue;
                stp_reach(L, mid);
                const Qty f = std::min(o.rem, avail);
                fill(sym, o, f, aggressive ? L : o.price);
                cons[mid] += f;
                if (o.rem == 0) return false;
            }
            return true;
        };
        if (const auto* qs = rb_.queues(sym, opp)) {
            if (opp == Side::Sell) { for (auto it = qs->begin(); it != qs->end() && level(it->first, it->second);) ++it; }
            else { for (auto it = qs->rbegin(); it != qs->rend() && level(it->first, it->second);) ++it; }
        }
        if (o.rem > 0) stp_reach(o.price, 0);  // sweep reaches every crossing own order
    }

    void settle(SymbolId sym) {
        for (Side s : {Side::Buy, Side::Sell}) {
            Price top = 0;  // our best price on this side
            bool any = false;
            for (const Own& o : own_[sym])
                if (o.side == s && o.rem > 0 && (!any || better(s, o.price, top))) { top = o.price; any = true; }
            Price opp;
            if (!any || !best_avail(sym, other(s), opp) || !crosses(s, top, opp)) continue;
            for (Own* o : by_priority(sym, s)) {
                if (!best_avail(sym, other(s), opp) || !crosses(s, o->price, opp)) break;
                take(sym, *o, false);
            }
        }
        drop_filled(sym);
    }

    L2Rebuilder rb_;
    FillModel model_;
    std::vector<std::vector<Own>> own_;
    std::vector<std::unordered_map<OrderId, Qty>> consumed_;
    std::unordered_map<OrderId, SymbolId> index_;
    std::vector<uint8_t> cont_, dirty_;
    std::vector<SymbolId> dirty_list_;
    std::vector<Price> last_px_;
    std::vector<Own*> prio_buf_;
    std::vector<OrderId> victims_;
    FillSink* sink_ = nullptr;
    int64_t now_ = 0;
    uint64_t seq_ = 0;
};

// ------------------------------------------------------------------- runners
// Market flow for a backtest: anonymised accounts, so the matching outcome of
// the market is independent of the self-trade setting.
inline void anonymize(Flow& f) {
    for (OrderMsg& m : f.msgs) m.account = 0;
}

struct VecSink {
    std::vector<TickRecord> v;
    void write(const TickRecord& r) { v.push_back(r); }
    void write_status(uint64_t seq, SymbolId sym, uint32_t t, Phase p) {
        v.push_back(TickRecord{seq, sym, 'S', t, 0, 0, 0, 0, phase_code(p)[0]});
    }
};

// The exchange's tick feed for a (market-only) flow.
inline std::vector<TickRecord> make_feed(const Flow& f, BookConfig cfg) {
    std::vector<OrderBook> books;
    for (const SymbolInfo& s : f.symbols) books.emplace_back(s, 1 << 14, cfg);
    VecSink sink;
    if (cfg.venue == Venue::SSE) {
        L2Publisher<VecSink> pub{sink};
        for (const OrderMsg& m : f.msgs) pub.process(books[m.sym], m);
    } else {
        SzsePublisher<VecSink> pub(sink);
        for (const OrderMsg& m : f.msgs) pub.process(books[m.sym], m);
    }
    return std::move(sink.v);
}

template <class Strategy>
Result run_truth(const Flow& f, BookConfig cfg, const Strategy& s, const Params& p) {
    TruthWorld w(f.symbols, cfg);
    Driver<TruthWorld, Strategy> d(w, s, p, f.symbols.size());
    const int64_t t0 = now_ns();
    for (const OrderMsg& m : f.msgs) {
        const int64_t t = to_ms(m.time);
        d.before_event(t);
        if (m.type == MsgType::Phase) d.phase_event(m.sym, m.phase == Phase::Continuous, t);
        w.market(m, t);
    }
    d.before_event(std::numeric_limits<int64_t>::max() / 2);
    std::vector<Price> last(f.symbols.size());
    for (size_t i = 0; i < last.size(); ++i) last[i] = w.last_price(SymbolId(i));
    Result r = d.finish(last);
    r.events = f.msgs.size();
    r.seconds = (now_ns() - t0) / 1e9;
    return r;
}

template <class Strategy>
Result run_sim(const std::vector<TickRecord>& feed, size_t nsym, Venue v, FillModel model, const Strategy& s,
               const Params& p) {
    SimWorld w(nsym, v, model);
    Driver<SimWorld, Strategy> d(w, s, p, nsym);
    const int64_t t0 = now_ns();
    for (const TickRecord& r : feed) {
        const int64_t t = to_ms(r.time);
        d.before_event(t);
        if (r.type == 'S') d.phase_event(r.sym, r.bs == 'T', t);
        w.record(r, t);
    }
    d.before_event(std::numeric_limits<int64_t>::max() / 2);
    std::vector<Price> last(nsym);
    for (size_t i = 0; i < nsym; ++i) last[i] = w.last_price(SymbolId(i));
    Result r = d.finish(last);
    r.events = feed.size();
    r.seconds = (now_ns() - t0) / 1e9;
    return r;
}

// --------------------------------------------------------------- comparison
struct Comparison {
    uint64_t orders = 0;        // placed orders in the model
    int64_t placed_qty = 0, filled_qty = 0;
    uint64_t common = 0;        // orders with identical decisions in both worlds
    uint64_t exact = 0;         // ... whose filled quantity matches exactly
    uint64_t agree = 0;         // ... where both filled something or both nothing
    double mae = 0;             // mean |filled_model - filled_truth| (shares)
    int64_t over = 0, under = 0;  // total shares over / under-filled vs truth
};

inline Comparison compare(const Result& truth, const Result& model, int which /*0 all, 1 passive, 2 take*/) {
    Comparison c;
    double abs_sum = 0;
    for (const auto& [id, m] : model.orders) {
        if (!m.placed) continue;
        if ((which == 1 && m.take) || (which == 2 && !m.take)) continue;
        ++c.orders;
        c.placed_qty += m.qty;
        c.filled_qty += m.filled;
        auto it = truth.orders.find(id);
        if (it == truth.orders.end()) continue;
        const OwnOrder& t = it->second;
        if (!t.placed || t.side != m.side || t.price != m.price || t.qty != m.qty) continue;
        ++c.common;
        const int64_t d = int64_t(m.filled) - t.filled;
        c.exact += d == 0;
        c.agree += (m.filled > 0) == (t.filled > 0);
        abs_sum += double(d < 0 ? -d : d);
        if (d > 0) c.over += d; else c.under -= d;
    }
    if (c.common) c.mae = abs_sum / double(c.common);
    return c;
}

}  // namespace me::bt
