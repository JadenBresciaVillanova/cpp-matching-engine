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

template <class T>
class Pool {
public:
    using Handle = PoolHandle;
    static constexpr Handle kInvalid = kNullHandle;

    explicit Pool(std::size_t capacity) : nodes_(capacity) {
        // Thread every slot onto the free list: 0 -> 1 -> 2 -> ... -> end.
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            nodes_[i].next_free = static_cast<Handle>(i + 1);
        }
        if (capacity > 0) {
            nodes_[capacity - 1].next_free = kInvalid;
            free_head_ = 0;
        }
    }

    // O(1). Returns kInvalid when exhausted (caller must handle, e.g. reject the
    // incoming order rather than crash).
    Handle allocate() noexcept {
        // Asserted here rather than at class scope so that T need only be a
        // complete type at the point of first use, not at instantiation. This
        // lets a type hold a Pool of itself indirectly without a circular
        // completeness requirement.
        static_assert(std::is_trivially_copyable<T>::value,
                      "Pool<T> assumes trivially copyable T; revisit lifetime "
                      "management (placement new / destroy_at) if this changes.");
        if (free_head_ == kInvalid) return kInvalid;
        const Handle h = free_head_;
        free_head_ = nodes_[h].next_free;
        ++in_use_;
        return h;
    }

    // O(1). Returns the slot to the free list. We do not run T's destructor here
    // because T is a trivial POD order record; if T ever gains non-trivial
    // members this must call std::destroy_at and allocate() must placement-new.
    void deallocate(Handle h) noexcept {
        nodes_[h].next_free = free_head_;
        free_head_ = h;
        --in_use_;
    }

    T&       operator[](Handle h) noexcept       { return nodes_[h].value; }
    const T& operator[](Handle h) const noexcept { return nodes_[h].value; }

    std::size_t capacity() const noexcept { return nodes_.size(); }
    std::size_t in_use() const noexcept   { return in_use_; }

private:
    // A slot is either a live T or a free-list link. We overlap them in a union
    // so the free list costs no extra memory. T must be trivially handled for
    // this to be safe without explicit lifetime management; static_assert guards
    // that assumption so a future change to T fails loudly instead of silently.
    union Node {
        T value;
        Handle next_free;
        Node() noexcept {}   // union with non-trivial members needs a ctor
        ~Node() noexcept {}
    };

    std::vector<Node> nodes_;
    Handle free_head_ = kInvalid;
    std::size_t in_use_ = 0;
};

} // namespace lob
