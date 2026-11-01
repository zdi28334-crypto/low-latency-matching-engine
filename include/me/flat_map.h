// Open-addressing hash map OrderId -> uint32_t (pool index).
// Linear probing + backward-shift deletion (no tombstones), so lookups stay
// short even under heavy add/cancel churn. Key 0 is reserved as "empty".
#pragma once
#include <cstdint>
#include <vector>

namespace me {

class FlatMap {
public:
    explicit FlatMap(size_t capacity_hint = 1 << 16) { rehash(round_up(capacity_hint * 2)); }

    bool insert(uint64_t key, uint32_t value) {
        if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
        size_t i = home(key);
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = {key, value};
        ++size_;
        return true;
    }

    // Returns pointer to value or nullptr.
    const uint32_t* find(uint64_t key) const {
        size_t i = home(key);
        while (slots_[i].key != 0) {
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(uint64_t key) {
        size_t i = home(key);
        while (slots_[i].key != key) {
            if (slots_[i].key == 0) return false;
            i = (i + 1) & mask_;
        }
        // Backward shift: pull later entries of the same probe run into the hole.
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == 0) break;
            size_t k = home(slots_[j].key);
            // Entry at j may move to i iff i lies in the cyclic range [k, j).
            if (((j - k) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = 0;
        --size_;
        return true;
    }

    size_t size() const { return size_; }

private:
    struct Slot {
        uint64_t key = 0;
        uint32_t value = 0;
    };

    static size_t round_up(size_t n) {
        size_t c = 16;
        while (c < n) c <<= 1;
        return c;
    }
    size_t home(uint64_t key) const { return (key * 0x9E3779B97F4A7C15ULL) >> shift_; }

    void rehash(size_t cap) {
        std::vector<Slot> old;
        old.swap(slots_);
        slots_.assign(cap, Slot{});
        mask_ = cap - 1;
        shift_ = 64 - __builtin_ctzll(cap);
        size_ = 0;
        for (const Slot& s : old)
            if (s.key != 0) insert(s.key, s.value);
    }

    std::vector<Slot> slots_;
    size_t mask_ = 0;
    int shift_ = 0;
    size_t size_ = 0;
};

}  // namespace me
