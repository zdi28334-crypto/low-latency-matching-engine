// Replay & result-comparison harness for matching logic / performance work.
//   replay_diff <in.flow> [--record golden.trades] [--golden golden.trades]
//               [--venue sse|szse] [--stp none|newest|oldest|both]
// 1. replays the flow through the optimised OrderBook and the naive RefBook
//    and requires identical trade streams (differential test);
// 2. --record writes the trade stream as a golden file; --golden compares
//    the current build against a golden file from an earlier build, so any
//    performance change that alters matching results is caught.
#include <cstdio>
#include <cstring>
#include <string>

#include "me/flow.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/ref_book.h"

using namespace me;

struct Collect {
    std::vector<Trade>* out;
    void on_trade(const Trade& t) { out->push_back(t); }
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

static uint64_t digest(const std::vector<Trade>& v) {
    uint64_t h = 1469598103934665603ULL;
    for (const Trade& t : v) {
        uint64_t f[5] = {t.buy_id, t.sell_id, uint64_t(uint32_t(t.price)), uint64_t(uint32_t(t.qty)), uint64_t(t.bs_flag)};
        for (uint64_t x : f) { h ^= x; h *= 1099511628211ULL; }
    }
    return h;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: replay_diff in.flow [--record f] [--golden f]\n"); return 1; }
    std::string record, golden;
    BookConfig cfg;
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string k = argv[i], v = argv[i + 1];
        if (k == "--record") record = v;
        else if (k == "--golden") golden = v;
        else if (k == "--venue") cfg.venue = v == "szse" ? Venue::SZSE : Venue::SSE;
        else if (k == "--stp") cfg.stp = v == "newest" ? StpPolicy::CancelNewest
                                       : v == "oldest" ? StpPolicy::CancelOldest
                                       : v == "both"   ? StpPolicy::CancelBoth : StpPolicy::None;
    }
    Flow f = load_flow(argv[1]);
    const size_t ns = f.symbols.size();

    // Optimised book. Trades are kept per security then concatenated in
    // security order, so both implementations are compared on the same key.
    std::vector<std::vector<Trade>> fast_tr(ns), ref_tr(ns);
    std::vector<OrderBook> fast;
    std::vector<Collect> col(ns);
    for (size_t i = 0; i < ns; ++i) { fast.emplace_back(f.symbols[i], 1 << 14, cfg); col[i].out = &fast_tr[i]; }
    int64_t t0 = now_ns();
    for (const OrderMsg& m : f.msgs) fast[m.sym].process(m, col[m.sym]);
    double t_fast = (now_ns() - t0) / 1e9;

    std::vector<RefBook> ref;
    for (auto& s : f.symbols) ref.emplace_back(s, cfg);
    t0 = now_ns();
    for (const OrderMsg& m : f.msgs) ref[m.sym].process(m);
    double t_ref = (now_ns() - t0) / 1e9;
    for (size_t i = 0; i < ns; ++i) ref_tr[i] = std::move(ref[i].trades);

    std::vector<Trade> all_fast, all_ref;
    for (size_t i = 0; i < ns; ++i) {
        all_fast.insert(all_fast.end(), fast_tr[i].begin(), fast_tr[i].end());
        all_ref.insert(all_ref.end(), ref_tr[i].begin(), ref_tr[i].end());
    }
    size_t auction = 0;
    for (auto& t : all_fast) auction += t.bs_flag == 'N';
    std::printf("replayed %zu msgs: OrderBook %.3fs, RefBook %.3fs; trades %zu (auction %zu)\n", f.msgs.size(), t_fast,
                t_ref, all_fast.size(), auction);

    bool ok = all_fast.size() == all_ref.size();
    size_t first_bad = 0;
    for (; ok && first_bad < all_fast.size(); ++first_bad)
        if (!(all_fast[first_bad] == all_ref[first_bad])) ok = false;
    if (ok) std::printf("differential: PASS (digest %016lx)\n", (unsigned long)digest(all_fast));
    else std::printf("differential: FAIL at trade #%zu\n", first_bad);

    if (!record.empty()) {
        FILE* fp = std::fopen(record.c_str(), "wb");
        std::fwrite(all_fast.data(), sizeof(Trade), all_fast.size(), fp);
        std::fclose(fp);
        std::printf("recorded golden trades -> %s\n", record.c_str());
    }
    if (!golden.empty()) {
        FILE* fp = std::fopen(golden.c_str(), "rb");
        if (!fp) { std::perror(golden.c_str()); return 1; }
        std::vector<Trade> g;
        Trade t;
        while (std::fread(&t, sizeof t, 1, fp) == 1) g.push_back(t);
        std::fclose(fp);
        bool same = g == all_fast;
        std::printf("golden compare: %s (%zu vs %zu trades)\n", same ? "PASS" : "FAIL", g.size(), all_fast.size());
        ok = ok && same;
    }
    return ok ? 0 : 1;
}
