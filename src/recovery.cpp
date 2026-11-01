#include "me/recovery.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "me/latency.h"

namespace me {

uint32_t crc32(const void* data, size_t n, uint32_t crc) {
    static const std::array<uint32_t, 256> table = [] {  // IEEE 802.3 polynomial, built once (thread-safe)
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    const auto* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

namespace {

struct NullListener {
    void on_trade(const Trade&) {}
    void on_rest(OrderId, Side, Price, Qty, uint32_t) {}
    void on_cancel(OrderId, Side, Price, Qty, uint32_t) {}
    void on_expire(OrderId, Side, Qty, ExpireReason, uint32_t) {}
    void on_reject(OrderId, RejectReason) {}
    void on_phase(Phase, uint32_t) {}
};

// Fixed 24-byte on-disk order record (explicit padding, always zeroed).
struct SnapOrder {
    uint64_t id;
    uint32_t account;
    int32_t price;
    int32_t qty;
    uint8_t side;
    uint8_t pad[3];
};
static_assert(sizeof(SnapOrder) == 24);

constexpr char kSnapMagic[8] = {'M', 'E', 'S', 'N', 'A', 'P', '0', '1'};

// Accumulates bytes and their CRC while writing a FILE*.
struct CrcWriter {
    FILE* fp;
    uint32_t crc = 0;
    void put(const void* p, size_t n) {
        if (std::fwrite(p, 1, n, fp) != n) throw std::runtime_error("snapshot write failed");
        crc = crc32(p, n, crc);
    }
};

struct CrcReader {
    const std::vector<char>& buf;
    size_t pos = 0;
    template <class T>
    T get() {
        if (pos + sizeof(T) > buf.size()) throw std::runtime_error("snapshot truncated");
        T v;
        std::memcpy(&v, buf.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
};

std::vector<char> read_file(const std::string& path) {
    std::vector<char> buf;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return buf;
    char tmp[1 << 16];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof tmp, fp)) > 0) buf.insert(buf.end(), tmp, tmp + n);
    std::fclose(fp);
    return buf;
}

void fsync_dir(const std::string& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) { ::fsync(fd); ::close(fd); }
}

}  // namespace

// ------------------------------------------------------------------ setup
DurableMatcher::DurableMatcher(Tag, const std::vector<SymbolInfo>& symbols, BookConfig cfg, DurableOptions opts)
    : symbols_(symbols), cfg_(cfg), opts_(std::move(opts)) {
    if (opts_.snapshot_every == 0) throw std::invalid_argument("snapshot_every > 0");
    books_.reserve(symbols_.size());
    for (const SymbolInfo& s : symbols_) {
        if (s.id != books_.size()) throw std::invalid_argument("symbol ids must be dense 0..N-1");
        books_.emplace_back(s, 1 << 12, cfg_);
    }
}

DurableMatcher::DurableMatcher(const std::vector<SymbolInfo>& symbols, BookConfig cfg, DurableOptions opts)
    : DurableMatcher(Tag{}, symbols, cfg, std::move(opts)) {
    checkpoint();  // snapshot at seq 0 + wal-0
}

std::string DurableMatcher::wal_path(uint64_t seq) const { return opts_.dir + "/wal-" + std::to_string(seq) + ".log"; }

// ------------------------------------------------------------- checkpoint
void DurableMatcher::write_snapshot() const {
    const std::string tmp = opts_.dir + "/snapshot.tmp", fin = opts_.dir + "/snapshot.bin";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) throw std::runtime_error("cannot create " + tmp);
    CrcWriter w{fp};
    w.put(kSnapMagic, 8);
    uint64_t seq = seq_;
    uint32_t nb = static_cast<uint32_t>(books_.size());
    w.put(&seq, 8);
    w.put(&nb, 4);
    std::vector<SnapOrder> orders;
    for (uint32_t i = 0; i < nb; ++i) {
        const OrderBook& b = books_[i];
        orders.clear();
        b.for_each_order([&](const RestingOrder& o) {
            SnapOrder so{};
            so.id = o.id; so.account = o.account; so.price = o.price; so.qty = o.qty;
            so.side = static_cast<uint8_t>(o.side);
            orders.push_back(so);
        });
        uint8_t ph = static_cast<uint8_t>(b.phase());
        int32_t last = b.last_price();
        uint64_t cnt = orders.size();
        w.put(&i, 4);
        w.put(&ph, 1);
        w.put(&last, 4);
        w.put(&cnt, 8);
        if (cnt) w.put(orders.data(), cnt * sizeof(SnapOrder));
    }
    uint32_t crc = w.crc;
    if (std::fwrite(&crc, 4, 1, fp) != 1) throw std::runtime_error("snapshot write failed");
    std::fflush(fp);
    ::fsync(::fileno(fp));
    std::fclose(fp);
    if (std::rename(tmp.c_str(), fin.c_str()) != 0) throw std::runtime_error("rename snapshot failed");
    fsync_dir(opts_.dir);
}

void DurableMatcher::checkpoint() {
    if (wal_ && opts_.fsync) wal_->sync();
    write_snapshot();                          // 1. state up to seq_ is durable
    const uint64_t old_start = wal_start_;
    const bool had_wal = wal_ != nullptr;
    wal_ = std::make_unique<WalWriter>(wal_path(seq_));  // 2. new segment for seq_+1..
    wal_start_ = seq_;
    if (had_wal && old_start != seq_) std::remove(wal_path(old_start).c_str());  // 3. old segment obsolete
}

// --------------------------------------------------------------- recovery
std::unique_ptr<DurableMatcher> DurableMatcher::recover(const std::vector<SymbolInfo>& symbols, BookConfig cfg,
                                                        DurableOptions opts, RecoveryInfo* info) {
    RecoveryInfo ri;
    std::unique_ptr<DurableMatcher> dm(new DurableMatcher(Tag{}, symbols, cfg, std::move(opts)));

    // ---- 1. snapshot
    int64_t t0 = now_ns();
    std::vector<char> buf = read_file(dm->opts_.dir + "/snapshot.bin");
    if (buf.size() < 8 + 8 + 4 + 4 || std::memcmp(buf.data(), kSnapMagic, 8) != 0)
        throw std::runtime_error("no valid snapshot in " + dm->opts_.dir);
    uint32_t stored_crc;
    std::memcpy(&stored_crc, buf.data() + buf.size() - 4, 4);
    if (crc32(buf.data(), buf.size() - 4) != stored_crc) throw std::runtime_error("snapshot CRC mismatch");
    CrcReader rd{buf};
    rd.pos = 8;
    const uint64_t snap_seq = rd.get<uint64_t>();
    const uint32_t nb = rd.get<uint32_t>();
    if (nb != dm->books_.size()) throw std::runtime_error("snapshot has a different symbol set");
    for (uint32_t k = 0; k < nb; ++k) {
        uint32_t i = rd.get<uint32_t>();
        Phase ph = static_cast<Phase>(rd.get<uint8_t>());
        Price last = rd.get<int32_t>();
        uint64_t cnt = rd.get<uint64_t>();
        OrderBook& b = dm->books_.at(i);
        b.restore_state(ph, last);
        for (uint64_t j = 0; j < cnt; ++j) {
            SnapOrder so = rd.get<SnapOrder>();
            b.restore_order(RestingOrder{so.id, so.account, so.price, so.qty, static_cast<Side>(so.side)});
        }
    }
    ri.snapshot_seq = snap_seq;
    ri.snapshot_ms = (now_ns() - t0) / 1e6;

    // ---- 2. WAL replay
    t0 = now_ns();
    std::vector<char> wal = read_file(dm->wal_path(snap_seq));
    NullListener nl;
    uint64_t expect = snap_seq + 1;
    size_t pos = 0;
    for (; pos + sizeof(WalRecord) <= wal.size(); pos += sizeof(WalRecord)) {
        WalRecord r;
        std::memcpy(&r, wal.data() + pos, sizeof r);
        if (r.magic != kWalMagic || r.seq != expect) break;
        if (crc32(&r.seq, sizeof(r.seq) + sizeof(r.msg)) != r.crc) break;
        if (r.msg.sym >= dm->books_.size()) break;
        dm->books_[r.msg.sym].process(r.msg, nl);
        ++expect;
        ++ri.replayed;
    }
    for (size_t k = pos; k < wal.size(); ++k)
        if (wal[k] != 0) { ri.torn_tail = true; break; }
    ri.replay_ms = (now_ns() - t0) / 1e6;
    ri.last_seq = snap_seq + ri.replayed;

    // ---- 3. resume: checkpoint the recovered state, start a fresh WAL
    dm->seq_ = ri.last_seq;
    dm->wal_start_ = snap_seq;
    dm->checkpoint();
    if (snap_seq != dm->seq_) std::remove(dm->wal_path(snap_seq).c_str());
    if (info) *info = ri;
    return dm;
}

}  // namespace me
