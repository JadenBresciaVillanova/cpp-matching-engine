#pragma once

// spsc_queue.hpp
// Lock-free single-producer / single-consumer ring buffer.
//
// This is the primitive that connects the ingest edge (network / file replay)
// to the matching core without a mutex. A mutex in the transfer path would
// be the single largest latency source in the system — larger than the
// matching itself — because every lock acquisition can block on the OS
// scheduler. SPSC avoids that entirely: the producer writes, the consumer
// reads, and the only synchronization is two atomic indexes with
// acquire/release ordering (not even sequential consistency, which is
// overkill for a single-reader single-writer queue).
//
// Capacity must be a power of 2 so modular indexing is a bitwise AND
// (one cycle) instead of a division (20-90 cycles on x86).

#include <atomic>
#include <cstddef>

namespace lob {

inline constexpr size_t kCacheLine = 64;

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0,
        "Capacity must be a power of 2.");
    static_assert(Capacity > 1, "Capacity must be > 1.");

public:
    bool try_push(const T& item) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & kMask;
        if (next == head_.load(std::memory_order_acquire)) return false;
        buffer_[tail] = item;
        tail_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;
        item = buffer_[head];
        head_.store((head + 1) & kMask, std::memory_order_release);
        return true;
    }

    size_t size_approx() const noexcept {
        return (tail_.load(std::memory_order_relaxed) -
                head_.load(std::memory_order_relaxed)) & kMask;
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<size_t> head_{0};
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) T buffer_[Capacity];
};

} // namespace lob
