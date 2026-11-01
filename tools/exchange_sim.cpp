// Exchange simulator: replays an order flow through the matching engine and
// publishes what an exchange would disseminate — a tick-by-tick Level-2 feed
// in SSE or SZSE style — plus the engine's own final book snapshot (ground
// truth for verifying L2 reconstruction).
//   exchange_sim <in.flow> <ticks.csv> <engine_snapshot.csv> [--venue sse|szse]
//                [--stp none|newest|oldest|both]
#include <cstdio>
#include <cstring>
#include <string>

#include "me/flow.h"
#include "me/l2.h"
#include "me/latency.h"
#include "me/order_book.h"

using namespace me;

StpPolicy parse_stp(const std::string& s) {
    if (s == "newest") return StpPolicy::CancelNewest;
    if (s == "oldest") return StpPolicy::CancelOldest;
    if (s == "both") return StpPolicy::CancelBoth;
    return StpPolicy::None;
}

template <class Pub>
uint64_t run(Flow& f, std::vector<OrderBook>& books, TickWriter& tw) {
    Pub pub{tw};
    for (const OrderMsg& m : f.msgs) pub.process(books[m.sym], m);
    return pub.seq;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: exchange_sim in.flow ticks.csv snapshot.csv [--venue sse|szse] [--stp none|newest|oldest|both]\n");
        return 1;
    }
    BookConfig cfg;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--venue")) cfg.venue = std::strcmp(argv[i + 1], "szse") ? Venue::SSE : Venue::SZSE;
        else if (!std::strcmp(argv[i], "--stp")) cfg.stp = parse_stp(argv[i + 1]);
    }
    Flow f = load_flow(argv[1]);
    TickWriter tw(argv[2]);
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s, 1 << 14, cfg);

    int64_t t0 = now_ns();
    uint64_t records = cfg.venue == Venue::SSE ? run<L2Publisher<TickWriter>>(f, books, tw)
                                               : run<SzsePublisher<TickWriter>>(f, books, tw);
    double secs = (now_ns() - t0) / 1e9;

    FILE* fp = std::fopen(argv[3], "w");
    std::fputs("sym,side,price,qty,orders\n", fp);
    size_t levels = 0;
    for (auto& b : books) {
        b.for_each_level([&](Side s, Price p, int64_t q, uint32_t n) {
            std::fprintf(fp, "%u,%c,%s,%ld,%u\n", b.info().id, s == Side::Buy ? 'B' : 'S', ticks_to_str(p).c_str(),
                         (long)q, n);
            ++levels;
        });
    }
    std::fclose(fp);
    std::printf("[%s] published %lu tick records in %.2fs; engine snapshot: %zu price levels\n",
                cfg.venue == Venue::SSE ? "SSE" : "SZSE", (unsigned long)records, secs, levels);
}
