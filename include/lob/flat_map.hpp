#pragma once

// flat_map.hpp
// Open-addressed hash map with linear probing.
//
// WHY NOT std::unordered_map?
//   unordered_map is a node-based (chained) hash table: every lookup hashes,
//   follows a pointer to the bucket, then chases pointers down a linked list
//   of heap-allocated nodes. Each pointer chase is a potential cache miss.
//   Profiling the matching engine shows id_index_ lookup (order cancel, modify,
//   fill cleanup) in the top-3 hottest operations. Replacing the chained table
//   with a flat, cache-friendly alternative is the single largest win available
//   outside the matching loop itself.
//
// THIS IMPLEMENTATION:
//   - Open addressing: keys and values live in a single contiguous array.
//     The hash + first probe lands in a cache line that's likely already hot;
//     linear probing walks contiguous memory — no pointer chasing.
//   - Fibonacci hashing (Knuth multiplicative): maps sequential OrderIds
//     (common: monotonically increasing) uniformly across the table, avoiding
//     the pathological clustering that identity-hash + linear-probe creates.
//   - Power-of-2 capacity: modular indexing is a single bitwise AND.
//   - Tombstone deletion: required for linear probing correctness (a gap in
//     the probe chain would falsely terminate a search).
//   - Pre-sized at construction, never grows. Same zero-realloc philosophy as
//     the Pool. Capacity is 2x expected count for ~50% load factor.
//
// SENTINEL REQUIREMENTS:
//   Key{0} is reserved as the "empty" marker. This is safe because OrderId 0
//   (kNullOrderId) is never a real order. Key(-1) is the tombstone.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

template <typename Key, typename Value>
class FlatMap {
public:
    static constexpr Key kEmpty     = Key{0};
    static constexpr Key kTombstone = static_cast<Key>(-1);

    explicit FlatMap(size_t expected_count) {
        size_t cap = 16;
        while (cap < expected_count * 2) cap <<= 1;
        slots_.resize(cap);
        mask_ = cap - 1;
        // Fibonacci hash shift: extract top log2(cap) bits of the product.
        shift_ = 64;
        for (size_t v = cap; v > 1; v >>= 1) --shift_;
    }

    Value* find(Key k) noexcept {
        size_t i = index_of(k);
        while (true) {
            auto& s = slots_[i];
            if (s.key == k)      return &s.value;
            if (s.key == kEmpty) return nullptr;
            i = (i + 1) & mask_;
        }
    }

    const Value* find(Key k) const noexcept {
        return const_cast<FlatMap*>(this)->find(k);
    }

    void insert(Key k, Value v) noexcept {
        size_t i = index_of(k);
        while (true) {
            auto& s = slots_[i];
            if (s.key == kEmpty || s.key == kTombstone) {
                s.key = k;
                s.value = v;
                ++count_;
                return;
            }
            if (s.key == k) {
                s.value = v;
                return;
            }
            i = (i + 1) & mask_;
        }
    }

    bool erase(Key k) noexcept {
        size_t i = index_of(k);
        while (true) {
            auto& s = slots_[i];
            if (s.key == k) {
                s.key = kTombstone;
                --count_;
                return true;
            }
            if (s.key == kEmpty) return false;
            i = (i + 1) & mask_;
        }
    }

    void reserve(size_t) noexcept {}
    size_t size() const noexcept { return count_; }
    size_t capacity() const noexcept { return slots_.size(); }

private:
    // Knuth multiplicative hash (Fibonacci hashing).
    // 2^64 / phi = 11400714819323198485. Multiplying by this constant and
    // extracting the top bits scatters sequential keys uniformly.
    static constexpr uint64_t kFibonacci = 11400714819323198485ULL;

    size_t index_of(Key k) const noexcept {
        return static_cast<size_t>(
            (static_cast<uint64_t>(k) * kFibonacci) >> shift_);
    }

    struct Slot {
        Key key = kEmpty;
        Value value{};
    };

    std::vector<Slot> slots_;
    size_t mask_  = 0;
    size_t shift_ = 64;
    size_t count_ = 0;
};

} // namespace lob
