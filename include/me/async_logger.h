// Asynchronous binary logger backed by an mmap'ed file.
//
// Hot path (matching thread): copy a 40-byte POD record into its own SPSC
// ring — no formatting, no syscalls, no locks. If the ring is full the record
// is dropped and counted (the matcher must never block on logging).
// Cold path (logger thread): drains all rings, formats text lines and memcpy's
// them into a memory-mapped file that grows with mremap; the kernel flushes
// pages in the background, so there is no write() per line.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "me/spsc_queue.h"

namespace me {

struct LogRecord {
    int64_t ts;
    uint32_t sym;
    char kind;     // T trade, A rest, D cancel, X expired remainder, R reject
    char bs;
    uint16_t pad;
    uint64_t a, b; // ids
    int32_t px, qty;
};
static_assert(sizeof(LogRecord) == 40);

// Grow-on-demand memory-mapped append-only file.
class MmapFile {
public:
    explicit MmapFile(const std::string& path, size_t chunk = 64ull << 20);
    ~MmapFile();
    void append(const char* data, size_t n);
    void sync();  // msync(MS_SYNC): survive power loss, not just a process crash
    size_t size() const { return pos_; }
private:
    void grow(size_t need);
    int fd_ = -1;
    char* base_ = nullptr;
    size_t mapped_ = 0, pos_ = 0, chunk_;
};

int format_record(const LogRecord& r, char* out);  // returns bytes written

class AsyncLogger {
public:
    AsyncLogger(const std::string& path, int producers, size_t ring_capacity = 1 << 16);
    ~AsyncLogger();

    // Called from producer thread `p` only.
    inline void log(int p, const LogRecord& r) {
        if (!rings_[p]->try_push(r)) dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    void start(int cpu = -1);
    void stop();  // drains everything, then joins
    uint64_t written() const { return written_; }
    uint64_t dropped() const { return dropped_.load(); }
    size_t bytes() const { return file_.size(); }

private:
    void run();
    size_t drain_once();

    MmapFile file_;
    std::vector<std::unique_ptr<SpscQueue<LogRecord>>> rings_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
    uint64_t written_ = 0;
};

}  // namespace me
