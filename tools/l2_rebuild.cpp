// Level-2 order-book reconstruction from a tick-by-tick feed.
//   l2_rebuild <ticks.csv> [--expect engine_snapshot.csv] [--watch SYM --l5 out.csv]
// --expect : compare the rebuilt books level by level with a reference snapshot
// --watch  : write a 5-level depth snapshot of one security after every trade
//            (the market state a strategy / backtest would see).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

#include "me/l2.h"
#include "me/latency.h"

using namespace me;

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: l2_rebuild ticks.csv [--expect snap.csv] [--watch SYM --l5 out.csv]\n"); return 1; }
    std::string expect, l5path;
    long watch = -1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--expect")) expect = argv[i + 1];
        else if (!std::strcmp(argv[i], "--watch")) watch = std::atol(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--l5")) l5path = argv[i + 1];
    }
    FILE* in = std::fopen(argv[1], "r");
    if (!in) { std::perror(argv[1]); return 1; }
    FILE* l5 = (!l5path.empty() && watch >= 0) ? std::fopen(l5path.c_str(), "w") : nullptr;
    if (l5) {
        std::fputs("seq,time,last", l5);
        for (int k = 1; k <= 5; ++k) std::fprintf(l5, ",bid%d,bidv%d", k, k);
        for (int k = 1; k <= 5; ++k) std::fprintf(l5, ",ask%d,askv%d", k, k);
        std::fputc('\n', l5);
    }

    L2Rebuilder rb;
    char line[256];
    TickRecord r;
    uint64_t n = 0, bad = 0;
    Price last_px = 0;
    int64_t t0 = now_ns();
    if (!std::fgets(line, sizeof line, in)) return 1;  // header
    while (std::fgets(line, sizeof line, in)) {
        if (!read_tick_line(line, r)) { ++bad; continue; }
        rb.apply(r);
        ++n;
        if (r.type == 'T' && long(r.sym) == watch) last_px = r.price;
        // Snapshot after continuous trades and after each status change; skip
        // the individual auction ('N') prints — mid-uncross the book is
        // transiently crossed, the state after the uncross is what matters.
        bool snap = (r.type == 'T' && r.bs != 'N') || r.type == 'S';
        if (l5 && snap && long(r.sym) == watch) {
            std::fprintf(l5, "%lu,%09u,%s", (unsigned long)r.seq, r.time, last_px ? ticks_to_str(last_px).c_str() : "");
            auto bids = rb.depth(r.sym, Side::Buy, 5), asks = rb.depth(r.sym, Side::Sell, 5);
            for (size_t k = 0; k < 5; ++k)
                k < bids.size() ? std::fprintf(l5, ",%s,%ld", ticks_to_str(bids[k].first).c_str(), (long)bids[k].second)
                                : std::fprintf(l5, ",,");
            for (size_t k = 0; k < 5; ++k)
                k < asks.size() ? std::fprintf(l5, ",%s,%ld", ticks_to_str(asks[k].first).c_str(), (long)asks[k].second)
                                : std::fprintf(l5, ",,");
            std::fputc('\n', l5);
        }
    }
    double secs = (now_ns() - t0) / 1e9;
    std::fclose(in);
    if (l5) std::fclose(l5);
    const auto& st = rb.stats();
    std::printf("rebuilt from %lu records in %.2fs (%.2f M rec/s), parse errors=%lu\n", (unsigned long)n, secs,
                n / secs / 1e6, (unsigned long)bad);
    std::printf("  A=%lu D=%lu T=%lu S=%lu | trades whose aggressor never rested=%lu | anomalies=%lu\n",
                (unsigned long)st.adds, (unsigned long)st.dels, (unsigned long)st.trades, (unsigned long)st.status,
                (unsigned long)st.trades_aggressor_unseen, (unsigned long)st.anomalies);

    if (expect.empty()) return st.anomalies ? 2 : 0;
    // --- level-by-level comparison against the engine snapshot
    FILE* ef = std::fopen(expect.c_str(), "r");
    if (!ef) { std::perror(expect.c_str()); return 1; }
    std::map<std::tuple<unsigned, char, Price>, int64_t> want;
    if (!std::fgets(line, sizeof line, ef)) return 1;
    unsigned sym, cnt; char side; int pi, pf; long q;
    while (std::fgets(line, sizeof line, ef))
        if (std::sscanf(line, "%u,%c,%d.%d,%ld,%u", &sym, &side, &pi, &pf, &q, &cnt) == 6) want[{sym, side, pi * 100 + pf}] = q;
    std::fclose(ef);
    size_t matched = 0, mismatched = 0, extra = 0;
    std::map<unsigned, bool> syms;
    for (auto& [k, v] : want) syms[std::get<0>(k)] = true;
    for (auto& [s, _] : syms) {
        for (Side sd : {Side::Buy, Side::Sell}) {
            const auto* lv = rb.levels(s, sd);
            if (!lv) continue;
            for (auto& [p, qq] : *lv) {
                auto it = want.find({s, sd == Side::Buy ? 'B' : 'S', p});
                if (it == want.end()) ++extra;
                else if (it->second == qq) { ++matched; want.erase(it); }
                else { ++mismatched; want.erase(it); }
            }
        }
    }
    size_t missing = want.size();
    std::printf("snapshot check: %zu levels match, %zu qty mismatch, %zu missing, %zu extra -> %s\n", matched,
                mismatched, missing, extra, (mismatched + missing + extra) ? "FAIL" : "PASS");
    return (mismatched + missing + extra) ? 3 : 0;
}
