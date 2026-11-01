// Crash-recovery tests: snapshot + WAL replay must reproduce the exact books.
#include <stdlib.h>

#include <filesystem>
#include <fstream>

#include "me/flow.h"
#include "me/recovery.h"
#include "test_util.h"

using namespace me;
namespace fs = std::filesystem;

namespace {

struct Null {
    void on_trade(const Trade&) {}
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "/tmp/me_recovery_XXXXXX";
        path = mkdtemp(tmpl);
    }
    ~TempDir() { fs::remove_all(path); }
};

// Fingerprint of plain books after processing msgs[0, n).
uint64_t reference_fp(const Flow& f, uint64_t n, const BookConfig& cfg) {
    std::vector<OrderBook> books;
    for (auto& s : f.symbols) books.emplace_back(s, 1 << 12, cfg);
    Null nl;
    for (uint64_t i = 0; i < n; ++i) books[f.msgs[i].sym].process(f.msgs[i], nl);
    return books_fingerprint([&](size_t i) -> const OrderBook& { return books[i]; }, books.size());
}

uint64_t matcher_fp(const DurableMatcher& dm) {
    return books_fingerprint([&](size_t i) -> const OrderBook& { return dm.book(i); }, dm.books());
}

BookConfig test_cfg() {
    BookConfig c;
    c.stp = StpPolicy::CancelOldest;
    return c;
}

const Flow& test_flow() {
    static Flow f = [] {
        FlowParams p{10, 60000, 77};
        p.accounts = 40;
        return generate_flow(p);
    }();
    return f;
}

}  // namespace

TEST(recovery_after_stop_and_resume) {
    const Flow& f = test_flow();
    const BookConfig cfg = test_cfg();
    TempDir dir;
    const uint64_t M = 23456;
    {
        DurableMatcher dm(f.symbols, cfg, {dir.path, 5000});
        Null nl;
        for (uint64_t i = 0; i < M; ++i) dm.process(f.msgs[i], nl);
    }
    RecoveryInfo ri;
    auto dm = DurableMatcher::recover(f.symbols, cfg, {dir.path, 5000}, &ri);
    CHECK_EQ(ri.snapshot_seq, 20000u);
    CHECK_EQ(ri.replayed, 3456u);
    CHECK_EQ(ri.last_seq, M);
    CHECK(!ri.torn_tail);
    CHECK_EQ(matcher_fp(*dm), reference_fp(f, M, cfg));
    // Resume: the gateway resends everything after last_seq.
    Null nl;
    for (uint64_t i = ri.last_seq; i < f.msgs.size(); ++i) dm->process(f.msgs[i], nl);
    CHECK_EQ(matcher_fp(*dm), reference_fp(f, f.msgs.size(), cfg));
}

TEST(recovery_torn_tail_and_corruption) {
    const Flow& f = test_flow();
    const BookConfig cfg = test_cfg();
    const uint64_t M = 13000;  // snapshot at 10000, 3000 records in wal-10000.log
    for (int scenario = 0; scenario < 3; ++scenario) {
        TempDir dir;
        {
            DurableMatcher dm(f.symbols, cfg, {dir.path, 5000});
            Null nl;
            for (uint64_t i = 0; i < M; ++i) dm.process(f.msgs[i], nl);
        }
        const std::string wal = dir.path + "/wal-10000.log";
        const auto size = fs::file_size(wal);
        CHECK_EQ(size, 3000u * sizeof(WalRecord));
        uint64_t expect_last = M;
        if (scenario == 0) {
            // Crash in the middle of an append: the last record is cut short.
            fs::resize_file(wal, size - 30);
            expect_last = M - 1;
        } else if (scenario == 1) {
            // Bit rot in record #1000 of the segment: replay stops right before it.
            std::fstream fp(wal, std::ios::in | std::ios::out | std::ios::binary);
            fp.seekp(1000 * sizeof(WalRecord) + 30);
            char c;
            fp.read(&c, 1);
            c ^= 0x5A;
            fp.seekp(1000 * sizeof(WalRecord) + 30);
            fp.write(&c, 1);
            expect_last = 10000 + 1000;
        } else {
            // Crash while writing the next snapshot: a half-written snapshot.tmp is ignored.
            std::ofstream(dir.path + "/snapshot.tmp") << "garbage from an interrupted checkpoint";
        }
        RecoveryInfo ri;
        auto dm = DurableMatcher::recover(f.symbols, cfg, {dir.path, 5000}, &ri);
        CHECK_EQ(ri.last_seq, expect_last);
        CHECK_EQ(ri.torn_tail, scenario != 2);
        CHECK_EQ(matcher_fp(*dm), reference_fp(f, expect_last, cfg));
    }
}
