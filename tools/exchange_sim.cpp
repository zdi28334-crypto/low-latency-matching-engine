// Exchange simulator: replays an order flow through the matching engine and
// publishes what an exchange would disseminate — an SSE-style merged
// tick-by-tick Level-2 feed — plus the engine's own final book snapshot
// (ground truth for verifying L2 reconstruction).
//   exchange_sim <in.flow> <ticks.csv> <engine_snapshot.csv>
#include <cstdio>

#include "me/flow.h"
#include "me/l2.h"
#include "me/latency.h"
#include "me/order_book.h"

using namespace me;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: exchange_sim in.flow ticks.csv snapshot.csv\n"); return 1; }
    Flow f = load_flow(argv[1]);
    TickWriter tw(argv[2]);
    L2Publisher<TickWriter> pub{tw};
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s, 1 << 14);

    int64_t t0 = now_ns();
    for (const OrderMsg& m : f.msgs) {
        pub.sym = m.sym;
        books[m.sym].process(m, pub);
    }
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
    std::printf("published %lu tick records in %.2fs; engine snapshot: %zu price levels\n",
                (unsigned long)pub.seq, secs, levels);
}
