#include "me/async_logger.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace me {

MmapFile::MmapFile(const std::string& path, size_t chunk) : chunk_(chunk) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) throw std::runtime_error("open " + path);
    grow(chunk_);
}

MmapFile::~MmapFile() {
    if (base_) ::munmap(base_, mapped_);
    if (fd_ >= 0) {
        if (::ftruncate(fd_, static_cast<off_t>(pos_)) != 0) std::perror("ftruncate");
        ::close(fd_);
    }
}

void MmapFile::grow(size_t need) {
    size_t new_size = mapped_;
    while (new_size < need) new_size += chunk_;
    if (::ftruncate(fd_, static_cast<off_t>(new_size)) != 0) throw std::runtime_error("ftruncate");
    void* p = base_ ? ::mremap(base_, mapped_, new_size, MREMAP_MAYMOVE)
                    : ::mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap/mremap");
    base_ = static_cast<char*>(p);
    mapped_ = new_size;
}

void MmapFile::append(const char* data, size_t n) {
    if (pos_ + n > mapped_) grow(pos_ + n);
    std::memcpy(base_ + pos_, data, n);
    pos_ += n;
}

void MmapFile::sync() {
    if (base_ && pos_ && ::msync(base_, pos_, MS_SYNC) != 0) std::perror("msync");
}

int format_record(const LogRecord& r, char* out) {
    return std::snprintf(out, 128, "%ld %u %c %c %lu %lu %d %d\n", (long)r.ts, r.sym, r.kind, r.bs ? r.bs : '-',
                         (unsigned long)r.a, (unsigned long)r.b, r.px, r.qty);
}

AsyncLogger::AsyncLogger(const std::string& path, int producers, size_t ring_capacity) : file_(path) {
    for (int i = 0; i < producers; ++i) rings_.push_back(std::make_unique<SpscQueue<LogRecord>>(ring_capacity));
}

AsyncLogger::~AsyncLogger() { stop(); }

bool pin_current_thread(int cpu);  // engine.cpp

void AsyncLogger::start(int cpu) {
    th_ = std::thread([this, cpu] {
        if (cpu >= 0) pin_current_thread(cpu);
        run();
    });
}

void AsyncLogger::stop() {
    if (!th_.joinable()) return;
    stop_.store(true, std::memory_order_release);
    th_.join();
}

size_t AsyncLogger::drain_once() {
    char buf[64 * 1024];
    size_t used = 0, n = 0;
    LogRecord r;
    for (auto& ring : rings_) {
        for (int k = 0; k < 4096 && ring->try_pop(r); ++k) {
            if (used + 128 > sizeof buf) { file_.append(buf, used); used = 0; }
            used += format_record(r, buf + used);
            ++n;
        }
    }
    if (used) file_.append(buf, used);
    written_ += n;
    return n;
}

void AsyncLogger::run() {
    unsigned idle = 0;
    for (;;) {
        bool stopping = stop_.load(std::memory_order_acquire);
        size_t n = drain_once();
        if (n == 0) {
            if (stopping) break;  // rings empty after stop requested
            if (++idle > 64) std::this_thread::sleep_for(std::chrono::microseconds(50));
            else std::this_thread::yield();
        } else {
            idle = 0;
        }
    }
}

}  // namespace me
