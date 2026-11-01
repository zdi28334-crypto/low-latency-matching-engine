// Crash recovery: write-ahead log (WAL) + periodic snapshots.
//
// DurableMatcher wraps a set of order books with the classic recipe:
//   1. every inbound message gets a sequence number and is appended to the
//      WAL *before* it is matched (memcpy into an mmap'ed file: survives a
//      process crash immediately; call sync() / set fsync for power loss);
//   2. every `snapshot_every` messages the full state of all books (every
//      resting order in FIFO order, phase, last price) is written to
//      snapshot.tmp, fsync'ed and atomically renamed to snapshot.bin; then a
//      new WAL segment wal-<seq>.log is started and the previous one deleted;
//   3. recovery = load snapshot (seq S) + replay wal-S.log from S+1 while
//      records are valid. Each WAL record carries a magic and a CRC32, so a
//      record torn by the crash (or any corruption) ends the replay cleanly.
// Matching is deterministic, so replaying the same messages reproduces the
// same books bit for bit. After recovery the matcher checkpoints immediately
// and the upstream gateway resends everything after last_seq.
//
// Crash windows and why each is safe:
//   * during a WAL append           -> torn record fails its CRC, dropped
//   * while writing snapshot.tmp    -> old snapshot.bin + its WAL still intact
//   * after rename, before new WAL  -> snapshot covers every accepted message
//   * before old WAL is deleted     -> recovery only reads wal-<snapshot seq>
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "me/async_logger.h"
#include "me/order_book.h"
#include "me/types.h"

namespace me {

uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);

struct WalRecord {
    uint32_t magic;  // kWalMagic
    uint32_t crc;    // CRC32 of seq + msg
    uint64_t seq;
    OrderMsg msg;
};
static_assert(sizeof(WalRecord) == 56);
inline constexpr uint32_t kWalMagic = 0x314C4157;  // "WAL1"

class WalWriter {
public:
    explicit WalWriter(const std::string& path) : file_(path, 16ull << 20) {}
    void append(uint64_t seq, const OrderMsg& m) {
        WalRecord r{kWalMagic, 0, seq, m};
        r.crc = crc32(&r.seq, sizeof(r.seq) + sizeof(r.msg));
        file_.append(reinterpret_cast<const char*>(&r), sizeof r);
    }
    void sync() { file_.sync(); }
private:
    MmapFile file_;
};

struct DurableOptions {
    std::string dir;                  // must exist
    uint64_t snapshot_every = 100000; // messages between checkpoints
    bool fsync = false;               // msync the WAL at every checkpoint
};

struct RecoveryInfo {
    uint64_t snapshot_seq = 0;   // state loaded from the snapshot
    uint64_t replayed = 0;       // WAL records re-applied
    uint64_t last_seq = 0;       // = snapshot_seq + replayed; resume after this
    bool torn_tail = false;      // garbage after the last valid record
    double snapshot_ms = 0, replay_ms = 0;
};

class DurableMatcher {
public:
    // Fresh start: empty books, seq 0, snapshot + WAL created in opts.dir.
    DurableMatcher(const std::vector<SymbolInfo>& symbols, BookConfig cfg, DurableOptions opts);

    // Rebuild state from opts.dir after a crash.
    static std::unique_ptr<DurableMatcher> recover(const std::vector<SymbolInfo>& symbols, BookConfig cfg,
                                                   DurableOptions opts, RecoveryInfo* info);

    template <class L>
    void process(const OrderMsg& m, L& lis) {
        ++seq_;
        wal_->append(seq_, m);  // log first, then match
        books_[m.sym].process(m, lis);
        if (seq_ % opts_.snapshot_every == 0) checkpoint();
    }

    void checkpoint();
    uint64_t seq() const { return seq_; }
    const OrderBook& book(SymbolId s) const { return books_[s]; }
    size_t books() const { return books_.size(); }

private:
    struct Tag {};
    DurableMatcher(Tag, const std::vector<SymbolInfo>& symbols, BookConfig cfg, DurableOptions opts);
    std::string wal_path(uint64_t seq) const;
    void write_snapshot() const;

    std::vector<SymbolInfo> symbols_;
    BookConfig cfg_;
    DurableOptions opts_;
    std::vector<OrderBook> books_;
    std::unique_ptr<WalWriter> wal_;
    uint64_t seq_ = 0;
    uint64_t wal_start_ = 0;
};

// Order-sensitive fingerprint of a set of books (phase, last price, every
// resting order in priority order) for equality checks in tests and tools.
template <class Books>
uint64_t books_fingerprint(const Books& books, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ULL; };
    for (size_t i = 0; i < n; ++i) {
        const OrderBook& b = books(i);
        mix(i); mix(uint64_t(b.phase())); mix(uint64_t(uint32_t(b.last_price())));
        b.for_each_order([&](const RestingOrder& o) {
            mix(o.id); mix(o.account); mix(uint64_t(uint32_t(o.price))); mix(uint64_t(uint32_t(o.qty)));
            mix(uint64_t(o.side));
        });
    }
    return h;
}

}  // namespace me
