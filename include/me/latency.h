// Timestamps and latency statistics for stage-level observation.
//
// TscClock: reads the CPU time-stamp counter (rdtscp, ~10 ns) instead of
// clock_gettime (~20-30 ns), calibrated once against steady_clock. Requires
// an invariant TSC (constant_tsc + nonstop_tsc) so ticks are comparable
// across cores; otherwise falls back to steady_clock transparently.
//
// LatencyHistogram: HdrHistogram-style log-linear buckets. O(1) record, fixed
// memory (~1k counters), mergeable across threads; 32 exact buckets below
// 32 ticks, then 16 sub-buckets per power of two (<= 6.25% relative error).
// Replaces pushing every sample into a vector on the hot path.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace me {

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

class TscClock {
public:
    static uint64_t now() {
#if defined(__x86_64__)
        if (state().use_tsc) {
            unsigned aux;
            return __rdtscp(&aux);
        }
#endif
        return static_cast<uint64_t>(now_ns());
    }
    static double ns_per_tick() { return state().ns_per_tick; }
    static double to_ns(double ticks) { return ticks * state().ns_per_tick; }
    static bool using_tsc() { return state().use_tsc; }

private:
    struct State {
        bool use_tsc = false;
        double ns_per_tick = 1.0;
        State() {
#if defined(__x86_64__)
            std::ifstream f("/proc/cpuinfo");
            std::string line;
            bool c = false, n = false;
            while (std::getline(f, line) && !(c && n)) {
                if (line.rfind("flags", 0) == 0) {
                    c = line.find(" constant_tsc") != std::string::npos;
                    n = line.find(" nonstop_tsc") != std::string::npos;
                }
            }
            if (c && n) {
                unsigned aux;
                int64_t t0 = now_ns();
                uint64_t c0 = __rdtscp(&aux);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                int64_t t1 = now_ns();
                uint64_t c1 = __rdtscp(&aux);
                ns_per_tick = double(t1 - t0) / double(c1 - c0);
                use_tsc = true;
            }
#endif
        }
    };
    static State& state() { static State s; return s; }
};

struct Percentiles {
    uint64_t n = 0;
    double mean = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;  // ns
};

class LatencyHistogram {
public:
    static constexpr int kLinear = 32;  // exact below 32
    static constexpr int kSub = 16;     // sub-buckets per octave above
    static constexpr int kBuckets = kLinear + (64 - 5) * kSub;

    void record(uint64_t v) {
        ++counts_[index(v)];
        ++n_;
        sum_ += v;
        if (v > max_) max_ = v;
    }
    void merge(const LatencyHistogram& o) {
        for (int i = 0; i < kBuckets; ++i) counts_[i] += o.counts_[i];
        n_ += o.n_; sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
    }
    uint64_t count() const { return n_; }

    // Value (in recorded units) at quantile q, reported as bucket midpoint.
    double value_at(double q) const {
        if (!n_) return 0;
        uint64_t target = std::max<uint64_t>(1, static_cast<uint64_t>(q * n_ + 0.5)), acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += counts_[i];
            if (acc >= target) return std::min(midpoint(i), double(max_));
        }
        return double(max_);
    }

    // Summary converted to ns with `ns_per_unit` (TscClock::ns_per_tick()).
    Percentiles summary(double ns_per_unit) const {
        Percentiles p;
        p.n = n_;
        if (!n_) return p;
        p.mean = double(sum_) / n_ * ns_per_unit;
        p.p50 = value_at(0.50) * ns_per_unit;
        p.p90 = value_at(0.90) * ns_per_unit;
        p.p99 = value_at(0.99) * ns_per_unit;
        p.p999 = value_at(0.999) * ns_per_unit;
        p.max = double(max_) * ns_per_unit;
        return p;
    }

    static int index(uint64_t v) {
        if (v < kLinear) return static_cast<int>(v);
        int m = 63 - __builtin_clzll(v);  // >= 5
        int shift = m - 4;
        return kLinear + (m - 5) * kSub + static_cast<int>((v >> shift) & (kSub - 1));
    }
    static double midpoint(int i) {
        if (i < kLinear) return i;
        int k = i - kLinear, m = k / kSub + 5, frac = k % kSub, shift = m - 4;
        double lower = double(uint64_t(kSub + frac) << shift);
        return lower + double(uint64_t(1) << shift) / 2;
    }

private:
    std::array<uint64_t, kBuckets> counts_{};
    uint64_t n_ = 0, sum_ = 0, max_ = 0;
};

inline std::string fmt_ns(double ns) {
    char b[32];
    if (ns < 1e3) std::snprintf(b, sizeof b, "%.0f ns", ns);
    else if (ns < 1e6) std::snprintf(b, sizeof b, "%.2f us", ns / 1e3);
    else std::snprintf(b, sizeof b, "%.2f ms", ns / 1e6);
    return b;
}

inline void print_percentiles(const char* name, const Percentiles& p) {
    std::printf("  %-12s n=%-9lu mean=%-10s p50=%-10s p90=%-10s p99=%-10s p99.9=%-10s max=%s\n", name,
                (unsigned long)p.n, fmt_ns(p.mean).c_str(), fmt_ns(p.p50).c_str(), fmt_ns(p.p90).c_str(),
                fmt_ns(p.p99).c_str(), fmt_ns(p.p999).c_str(), fmt_ns(p.max).c_str());
}

}  // namespace me
