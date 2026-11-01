// Real crash test for the WAL + snapshot recovery.
//   crash_test [trials=8] [symbols=50] [orders=2000000] [snapshot_every=100000]
// Each trial forks a child that runs a DurableMatcher over the whole flow, and
// SIGKILLs it at a random moment (no destructors, no flush — a real crash).
// The parent then recovers from the files on disk and checks that
//   1. the recovered books equal a reference run over exactly the first
//      last_seq messages (same orders, same FIFO order, same phase/last price);
//   2. after resuming from last_seq+1, the final books equal a reference run
//      over the whole flow.
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <random>

#include "me/flow.h"
#include "me/latency.h"
#include "me/recovery.h"

using namespace me;

namespace {
struct Null {
    void on_trade(const Trade&) {}
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

uint64_t reference_fp(const Flow& f, uint64_t n, const BookConfig& cfg) {
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s, 1 << 12, cfg);
    Null nl;
    for (uint64_t i = 0; i < n; ++i) books[f.msgs[i].sym].process(f.msgs[i], nl);
    return books_fingerprint([&](size_t i) -> const OrderBook& { return books[i]; }, books.size());
}
}  // namespace

int main(int argc, char** argv) {
    int trials = argc > 1 ? std::atoi(argv[1]) : 8;
    FlowParams fp;
    fp.symbols = argc > 2 ? std::atoi(argv[2]) : 50;
    fp.orders = argc > 3 ? std::atoll(argv[3]) : 2'000'000;
    uint64_t every = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 100000;
    fp.seed = 11;
    Flow f = generate_flow(fp);
    BookConfig cfg;
    cfg.stp = StpPolicy::CancelOldest;

    // How long does a full uninterrupted durable run take? Kill within that window.
    int64_t full_ns;
    {
        char tmpl[] = "/tmp/me_crash_XXXXXX";
        std::string dir = mkdtemp(tmpl);
        int64_t t0 = now_ns();
        DurableMatcher dm(f.symbols, cfg, {dir, every});
        Null nl;
        for (const OrderMsg& m : f.msgs) dm.process(m, nl);
        full_ns = now_ns() - t0;
        std::filesystem::remove_all(dir);
    }
    const uint64_t final_fp = reference_fp(f, f.msgs.size(), cfg);
    std::printf("flow: %zu msgs, %zu securities; durable run takes %.0f ms (%.2f M msg/s incl. WAL + snapshots every %lu)\n",
                f.msgs.size(), f.symbols.size(), full_ns / 1e6, f.msgs.size() / (full_ns / 1e9) / 1e6,
                (unsigned long)every);
    std::printf("%-6s %-10s %-12s %-10s %-10s %-11s %-11s %s\n", "trial", "kill@ms", "last_seq", "snapshot", "replayed",
                "recover_ms", "torn_tail", "result");

    std::mt19937_64 rng(2026);
    int passed = 0;
    for (int t = 0; t < trials; ++t) {
        char tmpl[] = "/tmp/me_crash_XXXXXX";
        std::string dir = mkdtemp(tmpl);
        const int64_t kill_after = static_cast<int64_t>((0.05 + 0.9 * (rng() % 1000) / 1000.0) * full_ns);

        pid_t pid = fork();
        if (pid == 0) {  // child: run until killed
            DurableMatcher dm(f.symbols, cfg, {dir, every});
            Null nl;
            for (const OrderMsg& m : f.msgs) dm.process(m, nl);
            pause();  // finished early: wait for the kill like a live process would
            _exit(0);
        }
        int64_t deadline = now_ns() + kill_after;
        while (now_ns() < deadline) usleep(200);
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);

        RecoveryInfo ri;
        int64_t t0 = now_ns();
        auto dm = DurableMatcher::recover(f.symbols, cfg, {dir, every}, &ri);
        double rec_ms = (now_ns() - t0) / 1e6;
        bool ok_point = books_fingerprint([&](size_t i) -> const OrderBook& { return dm->book(i); }, dm->books()) ==
                        reference_fp(f, ri.last_seq, cfg);
        Null nl;
        for (uint64_t i = ri.last_seq; i < f.msgs.size(); ++i) dm->process(f.msgs[i], nl);
        bool ok_final =
            books_fingerprint([&](size_t i) -> const OrderBook& { return dm->book(i); }, dm->books()) == final_fp;
        bool ok = ok_point && ok_final && WIFSIGNALED(status);
        passed += ok;
        std::printf("%-6d %-10.0f %-12lu %-10lu %-10lu %-11.1f %-11s %s\n", t, kill_after / 1e6,
                    (unsigned long)ri.last_seq, (unsigned long)ri.snapshot_seq, (unsigned long)ri.replayed, rec_ms,
                    ri.torn_tail ? "yes" : "no",
                    ok ? "PASS" : (!ok_point ? "FAIL (state at crash)" : !ok_final ? "FAIL (after resume)" : "FAIL"));
        dm.reset();
        std::filesystem::remove_all(dir);
    }
    std::printf("%d/%d trials recovered exactly\n", passed, trials);
    return passed == trials ? 0 : 1;
}
