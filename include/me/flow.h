// Synthetic order-flow generator + binary flow file I/O.
//
// Real SSE Level-2 data is a paid product, so the project ships a generator
// that produces a full trading day per symbol:
//   opening call auction -> continuous trading -> closing call auction -> close
// Order mix in continuous trading (roughly what a real book sees):
//   ~55% passive limit, ~25% cancel, ~15% marketable limit, ~5% best-5 IOC.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "me/types.h"

namespace me {

struct Flow {
    std::vector<SymbolInfo> symbols;
    std::vector<OrderMsg> msgs;
};

struct FlowParams {
    int symbols = 100;
    int64_t orders = 2'000'000;  // approx. total New+Cancel messages
    uint64_t seed = 42;
};

inline SymbolInfo make_symbol(SymbolId id, Price prev_close) {
    SymbolInfo s{};
    s.id = id;
    std::snprintf(s.code, sizeof(s.code), "%06u", (600000 + id) % 1000000);
    s.prev_close = prev_close;
    s.lower_limit = static_cast<Price>(std::lround(prev_close * 0.9));
    s.upper_limit = static_cast<Price>(std::lround(prev_close * 1.1));
    return s;
}

// HHMMSSmmm for a fraction [0,1) of a session [start_sec, end_sec) of the day.
inline uint32_t clock_at(double frac, int start_sec, int end_sec) {
    double t = start_sec + frac * (end_sec - start_sec);
    int s = static_cast<int>(t);
    int ms = static_cast<int>((t - s) * 1000);
    return static_cast<uint32_t>(((s / 3600) * 10000 + (s / 60 % 60) * 100 + s % 60) * 1000 + ms);
}

inline Flow generate_flow(const FlowParams& p) {
    std::mt19937_64 rng(p.seed);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    Flow f;
    struct State { double mid; std::vector<OrderId> recent; size_t rpos = 0; };
    std::vector<State> st(p.symbols);
    for (int i = 0; i < p.symbols; ++i) {
        Price pc = static_cast<Price>(500 + U(rng) * 9500);  // 5 ~ 100 CNY
        f.symbols.push_back(make_symbol(i, pc));
        st[i].mid = pc;
        st[i].recent.assign(256, 0);
    }
    OrderId next_id = 1;
    auto qty = [&] { return static_cast<Qty>(100 * (1 + static_cast<int>(-std::log(1 - U(rng)) * 8))); };
    auto clamp = [&](const SymbolInfo& s, double px) {
        Price q = static_cast<Price>(std::lround(px));
        return std::max(s.lower_limit, std::min(s.upper_limit, q));
    };
    auto remember = [&](State& s, OrderId id) { s.recent[s.rpos++ & 255] = id; };
    auto phase_msg = [&](Phase ph, uint32_t t) {
        for (int i = 0; i < p.symbols; ++i) {
            OrderMsg m; m.type = MsgType::Phase; m.sym = i; m.phase = ph; m.time = t;
            f.msgs.push_back(m);
        }
    };

    const int64_t n_open = p.orders * 3 / 100, n_close = p.orders * 3 / 100;
    const int64_t n_cont = p.orders - n_open - n_close;
    f.msgs.reserve(p.orders + 4 * p.symbols);

    // Auction orders cluster around the reference price and overlap, so the
    // uncross actually trades.
    auto auction_orders = [&](int64_t n, int start, int end, bool closing) {
        for (int64_t k = 0; k < n; ++k) {
            int s = static_cast<int>(rng() % p.symbols);
            const SymbolInfo& si = f.symbols[s];
            OrderMsg m;
            m.sym = s; m.id = next_id++; m.qty = qty();
            m.side = U(rng) < 0.5 ? Side::Buy : Side::Sell;
            double ref = closing ? st[s].mid : si.prev_close;
            double off = (U(rng) - 0.5) * 0.012 * ref;  // ±0.6%
            m.price = clamp(si, ref + off);
            m.time = clock_at(double(k) / n, start, end);
            f.msgs.push_back(m);
            remember(st[s], m.id);
        }
    };

    phase_msg(Phase::OpeningAuction, 91500000);
    auction_orders(n_open, 9 * 3600 + 15 * 60, 9 * 3600 + 25 * 60, false);
    phase_msg(Phase::Continuous, 93000000);

    for (int64_t k = 0; k < n_cont; ++k) {
        int s = static_cast<int>(rng() % p.symbols);
        const SymbolInfo& si = f.symbols[s];
        State& ss = st[s];
        ss.mid += (U(rng) - 0.5) * 0.6;  // random walk (ticks)
        ss.mid = std::max<double>(si.lower_limit + 5, std::min<double>(si.upper_limit - 5, ss.mid));
        // morning 09:30-11:30, afternoon 13:00-14:57
        double frac = double(k) / n_cont;
        uint32_t t = frac < 0.5 ? clock_at(frac * 2, 9 * 3600 + 1800, 11 * 3600 + 1800)
                                : clock_at(frac * 2 - 1, 13 * 3600, 14 * 3600 + 57 * 60);
        OrderMsg m;
        m.sym = s; m.time = t;
        double r = U(rng);
        if (r < 0.25) {  // cancel a recent order (may already be filled -> reject)
            OrderId victim = ss.recent[rng() & 255];
            if (victim == 0) continue;
            m.type = MsgType::Cancel; m.id = victim;
            f.msgs.push_back(m);
            continue;
        }
        m.id = next_id++; m.qty = qty();
        m.side = U(rng) < 0.5 ? Side::Buy : Side::Sell;
        double dir = m.side == Side::Buy ? 1.0 : -1.0;
        if (r < 0.80) {         // passive: 1..~20 ticks behind mid
            double off = 1 + std::floor(-std::log(1 - U(rng)) * 4);
            m.price = clamp(si, ss.mid - dir * off);
        } else if (r < 0.95) {  // marketable limit: cross mid by 0..3 ticks
            m.price = clamp(si, ss.mid + dir * std::floor(U(rng) * 4));
        } else {
            m.ord_type = OrdType::MarketBest5IOC;
            m.price = 0;
        }
        f.msgs.push_back(m);
        remember(ss, m.id);
    }

    phase_msg(Phase::ClosingAuction, 145700000);
    auction_orders(n_close, 14 * 3600 + 57 * 60, 15 * 3600, true);
    phase_msg(Phase::Closed, 150000000);
    return f;
}

// ---- binary file: "MEFLOW01" | u32 nsym | SymbolInfo[] | u64 nmsg | OrderMsg[]
inline void save_flow(const Flow& f, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) throw std::runtime_error("cannot open " + path);
    std::fwrite("MEFLOW01", 1, 8, fp);
    uint32_t ns = f.symbols.size();
    uint64_t nm = f.msgs.size();
    std::fwrite(&ns, sizeof ns, 1, fp);
    std::fwrite(f.symbols.data(), sizeof(SymbolInfo), ns, fp);
    std::fwrite(&nm, sizeof nm, 1, fp);
    std::fwrite(f.msgs.data(), sizeof(OrderMsg), nm, fp);
    std::fclose(fp);
}

inline Flow load_flow(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) throw std::runtime_error("cannot open " + path);
    char magic[8];
    Flow f;
    uint32_t ns = 0;
    uint64_t nm = 0;
    if (std::fread(magic, 1, 8, fp) != 8 || std::memcmp(magic, "MEFLOW01", 8) != 0)
        throw std::runtime_error("bad flow file " + path);
    if (std::fread(&ns, sizeof ns, 1, fp) != 1) throw std::runtime_error("truncated");
    f.symbols.resize(ns);
    if (std::fread(f.symbols.data(), sizeof(SymbolInfo), ns, fp) != ns) throw std::runtime_error("truncated");
    if (std::fread(&nm, sizeof nm, 1, fp) != 1) throw std::runtime_error("truncated");
    f.msgs.resize(nm);
    if (std::fread(f.msgs.data(), sizeof(OrderMsg), nm, fp) != nm) throw std::runtime_error("truncated");
    std::fclose(fp);
    return f;
}

}  // namespace me
