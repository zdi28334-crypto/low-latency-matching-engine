// Timestamps and percentile summaries for stage-level latency observation.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace me {

inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Percentiles {
    size_t n = 0;
    double mean = 0;
    int64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
};

inline Percentiles summarize(std::vector<int64_t> v) {
    Percentiles p;
    if (v.empty()) return p;
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * v.size()))]; };
    double sum = 0;
    for (int64_t x : v) sum += x;
    p.n = v.size(); p.mean = sum / v.size();
    p.p50 = at(0.50); p.p90 = at(0.90); p.p99 = at(0.99); p.p999 = at(0.999); p.max = v.back();
    return p;
}

inline std::string fmt_ns(double ns) {
    char b[32];
    if (ns < 1e3) std::snprintf(b, sizeof b, "%.0f ns", ns);
    else if (ns < 1e6) std::snprintf(b, sizeof b, "%.2f us", ns / 1e3);
    else std::snprintf(b, sizeof b, "%.2f ms", ns / 1e6);
    return b;
}

inline void print_percentiles(const char* name, const Percentiles& p) {
    std::printf("  %-12s n=%-9zu mean=%-10s p50=%-10s p90=%-10s p99=%-10s p99.9=%-10s max=%s\n", name, p.n,
                fmt_ns(p.mean).c_str(), fmt_ns(p.p50).c_str(), fmt_ns(p.p90).c_str(), fmt_ns(p.p99).c_str(),
                fmt_ns(p.p999).c_str(), fmt_ns(p.max).c_str());
}

}  // namespace me
