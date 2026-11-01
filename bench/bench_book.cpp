// Single-thread matching core throughput: optimised OrderBook vs naive RefBook.
//   bench_book [symbols=100] [orders=2000000]
#include <cstdlib>

#include "me/flow.h"
#include "me/latency.h"
#include "me/order_book.h"
#include "me/ref_book.h"

using namespace me;

struct Count {
    uint64_t trades = 0;
    void on_trade(const Trade&) { ++trades; }
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

int main(int argc, char** argv) {
    int nsym = argc > 1 ? std::atoi(argv[1]) : 100;
    int64_t norders = argc > 2 ? std::atoll(argv[2]) : 2'000'000;
    Flow f = generate_flow({nsym, norders, 7});
    std::printf("flow: %zu messages, %d securities\n", f.msgs.size(), nsym);

    // --- optimised book, best of 3
    double best = 1e18;
    uint64_t trades = 0;
    for (int rep = 0; rep < 3; ++rep) {
        std::vector<OrderBook> books;
        for (auto& s : f.symbols) books.emplace_back(s, 1 << 14);
        Count c;
        int64_t t0 = now_ns();
        for (const OrderMsg& m : f.msgs) books[m.sym].process(m, c);
        double el = double(now_ns() - t0);
        if (el < best) best = el;
        trades = c.trades;
    }
    std::printf("OrderBook : %8.2f M msg/s  (%.1f ns/msg, %lu trades)\n", f.msgs.size() / best * 1e3,
                best / f.msgs.size(), (unsigned long)trades);

    // --- per-message latency of the core (sampled every msg)
    {
        std::vector<OrderBook> books;
        for (auto& s : f.symbols) books.emplace_back(s, 1 << 14);
        Count c;
        std::vector<int64_t> lat;
        lat.reserve(f.msgs.size());
        for (const OrderMsg& m : f.msgs) {
            int64_t t0 = now_ns();
            books[m.sym].process(m, c);
            lat.push_back(now_ns() - t0);
        }
        print_percentiles("core/msg", summarize(std::move(lat)));
    }

    // --- naive reference book
    {
        std::vector<RefBook> books;
        for (auto& s : f.symbols) books.emplace_back(s);
        int64_t t0 = now_ns();
        for (const OrderMsg& m : f.msgs) books[m.sym].process(m);
        double el = double(now_ns() - t0);
        std::printf("RefBook   : %8.2f M msg/s  (%.1f ns/msg)  -> OrderBook speed-up x%.1f\n",
                    f.msgs.size() / el * 1e3, el / f.msgs.size(), el / best);
    }
    return 0;
}
