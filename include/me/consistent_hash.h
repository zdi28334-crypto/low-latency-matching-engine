// Consistent-hash ring with virtual nodes: security code -> shard.
// All orders of one security land on the same shard (single-threaded book =>
// per-security time order preserved), while different securities are
// matched in parallel. Adding/removing a shard only remaps ~1/N securities,
// which keeps re-sharding (e.g. scaling out) cheap.
// The ring is consulted once per security at startup; the hot path uses a
// precomputed SymbolId -> shard array.
#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace me {

inline uint64_t mix64(uint64_t x) {  // splitmix64 finaliser
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t hash_str(std::string_view s) {  // FNV-1a + mix
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return mix64(h);
}

class ConsistentHashRing {
public:
    ConsistentHashRing(int shards, int vnodes = 160) {
        for (int s = 0; s < shards; ++s)
            for (int v = 0; v < vnodes; ++v)
                ring_.push_back({mix64((uint64_t(s) << 32) ^ uint64_t(v) ^ 0x5bd1e995ULL), s});
        std::sort(ring_.begin(), ring_.end());
    }

    int shard_of(std::string_view key) const {
        uint64_t h = hash_str(key);
        auto it = std::lower_bound(ring_.begin(), ring_.end(), std::pair<uint64_t, int>{h, -1});
        if (it == ring_.end()) it = ring_.begin();  // wrap around
        return it->second;
    }

private:
    std::vector<std::pair<uint64_t, int>> ring_;
};

}  // namespace me
