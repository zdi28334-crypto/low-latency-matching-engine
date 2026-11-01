// Bounded lock-free single-producer / single-consumer ring buffer.
//  * head/tail on separate cache lines (no false sharing);
//  * each side caches the other side's index, so the shared atomic is only
//    re-read when the ring looks full/empty — this removes most cross-core
//    cache-line traffic;
//  * acquire/release ordering only, no locks, no CAS.
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define ME_CPU_RELAX() _mm_pause()
#else
#define ME_CPU_RELAX() std::this_thread::yield()
#endif

namespace me {

inline constexpr size_t kCacheLine = 64;

template <class T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>, "SpscQueue holds POD messages");

public:
    explicit SpscQueue(size_t capacity_pow2) : cap_(capacity_pow2), mask_(capacity_pow2 - 1),
                                               buf_(std::make_unique<T[]>(capacity_pow2)) {
        if (capacity_pow2 == 0 || (capacity_pow2 & mask_)) throw std::invalid_argument("capacity must be 2^k");
    }

    bool try_push(const T& v) {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == cap_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == cap_) return false;
        }
        buf_[t & mask_] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return false;
        }
        out = buf_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Spin briefly, then yield (machines with fewer cores than threads).
    void push(const T& v) {
        for (unsigned spins = 0; !try_push(v); ++spins) {
            if (spins < 256) ME_CPU_RELAX(); else std::this_thread::yield();
        }
    }

    size_t size_approx() const {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

private:
    const size_t cap_, mask_;
    std::unique_ptr<T[]> buf_;
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    size_t tail_cache_ = 0;  // consumer-local
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    size_t head_cache_ = 0;  // producer-local
    char pad_[kCacheLine - sizeof(size_t)];
};

}  // namespace me
