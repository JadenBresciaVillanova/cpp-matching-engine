#pragma once

// pool.hpp
// -----------------------------------------------------------------------------
// Fixed-capacity object pool.
//
// WHY: The matching hot path must not call malloc/free or new/delete. General
// allocators take locks, can trigger system calls (mmap/brk), have
// unpredictable latency, and fragment over time. In a latency-sensitive engine
// any of those is a tail-latency spike. So we pre-allocate ALL the order slots
// we will ever need up front, and hand them out by index from a free list.
//
// DESIGN:
//  - Storage is a single contiguous std::vector<T>. Contiguity is good for
//    cache behaviour: orders allocated near each other in time tend to live
//    near each other in memory.
//  - The free list is "intrusive": when a slot is free, we reuse the slot's own
//    memory to store the index of the next free slot. This means the free list
//    costs zero extra memory and allocate/free are O(1) with no pointer chasing
//    into separate bookkeeping structures.
//  - We hand out indices (Handle), not raw pointers. An index is stable even if
//    we ever needed to relocate storage, is smaller than a pointer, and makes
//    use-after-free easier to sanity-check (an index is bounds-checkable).
//
// TRADE-OFF: capacity is fixed at construction. If exhausted, allocate() fails
// (returns kInvalid) rather than growing - growing would mean a reallocation
// and a latency spike, exactly what we are avoiding. The capacity is a sizing
// decision made from expected peak open-order count.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>
#include <type_traits>
#include <new>

namespace lob {

// Standalone handle type. Defined outside Pool<T> so that types stored IN the
// pool (e.g. Order) can hold handles to their neighbours WITHOUT depending on
// Pool<Order> being a complete/instantiated type - that would be circular
// (Order -> Pool<Order> -> static_assert needs Order complete -> ...).
using PoolHandle = std::uint32_t;
inline constexpr PoolHandle kNullHandle = static_cast<PoolHandle>(-1);

// Free-list policy: LIFO reuses the most recently freed slot (hot in cache,
// good for temporal locality). FIFO reuses the longest-freed slot (more
// uniform distribution across the pool, more predictable latency after
// heavy cancel/resubmit churn).
enum class FreeListPolicy { Lifo, Fifo };

template <class T, FreeListPolicy Policy = FreeListPolicy::Lifo>
class Pool {
public:
    using Handle = PoolHandle;
    static constexpr Handle kInvalid = kNullHandle;

    explicit Pool(std::size_t capacity) : nodes_(capacity) {
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            nodes_[i].next_free = static_cast<Handle>(i + 1);
        }
        if (capacity > 0) {
            nodes_[capacity - 1].next_free = kInvalid;
            free_head_ = 0;
            if constexpr (Policy == FreeListPolicy::Fifo) {
                free_tail_ = static_cast<Handle>(capacity - 1);
            }
        }
    }

    Handle allocate() noexcept {
        static_assert(std::is_trivially_copyable<T>::value,
                      "Pool<T> assumes trivially copyable T; revisit lifetime "
                      "management (placement new / destroy_at) if this changes.");
        if (free_head_ == kInvalid) [[unlikely]] return kInvalid;
        const Handle h = free_head_;
        free_head_ = nodes_[h].next_free;
        if constexpr (Policy == FreeListPolicy::Fifo) {
            if (free_head_ == kInvalid) free_tail_ = kInvalid;
        }
        ++in_use_;
        return h;
    }

    void deallocate(Handle h) noexcept {
        if constexpr (Policy == FreeListPolicy::Fifo) {
            // Append to tail: recently freed slots go to the back of the line.
            nodes_[h].next_free = kInvalid;
            if (free_tail_ != kInvalid)
                nodes_[free_tail_].next_free = h;
            else
                free_head_ = h;
            free_tail_ = h;
        } else {
            // Push to head: most recently freed slot is reused first (hot cache).
            nodes_[h].next_free = free_head_;
            free_head_ = h;
        }
        --in_use_;
    }

    T&       operator[](Handle h) noexcept       { return nodes_[h].value; }
    const T& operator[](Handle h) const noexcept { return nodes_[h].value; }

    std::size_t capacity() const noexcept { return nodes_.size(); }
    std::size_t in_use() const noexcept   { return in_use_; }

private:
    union Node {
        T value;
        Handle next_free;
        Node() noexcept {}
        ~Node() noexcept {}
    };

    std::vector<Node> nodes_;
    Handle free_head_ = kInvalid;
    Handle free_tail_ = kInvalid;  // used only by Fifo policy
    std::size_t in_use_ = 0;
};

} // namespace lob
