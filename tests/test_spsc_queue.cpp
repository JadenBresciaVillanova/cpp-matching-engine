// test_spsc_queue.cpp
// Unit tests for the lock-free SPSC ring buffer.

#include "lob/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace lob;

TEST(SPSCQueue, PushPopSingle) {
    SPSCQueue<int, 4> q;
    int val = 0;
    EXPECT_FALSE(q.try_pop(val));
    EXPECT_TRUE(q.try_push(42));
    EXPECT_TRUE(q.try_pop(val));
    EXPECT_EQ(val, 42);
    EXPECT_FALSE(q.try_pop(val));
}

TEST(SPSCQueue, FillToCapacity) {
    SPSCQueue<int, 4> q;
    // Usable slots = Capacity - 1 = 3 (one slot is always empty to distinguish
    // full from empty — standard SPSC ring buffer invariant).
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_FALSE(q.try_push(4));
    EXPECT_EQ(q.size_approx(), 3u);
}

TEST(SPSCQueue, FifoOrder) {
    SPSCQueue<int, 8> q;
    for (int i = 0; i < 7; ++i) EXPECT_TRUE(q.try_push(i));
    for (int i = 0; i < 7; ++i) {
        int val = -1;
        EXPECT_TRUE(q.try_pop(val));
        EXPECT_EQ(val, i);
    }
}

TEST(SPSCQueue, Wraparound) {
    SPSCQueue<int, 4> q;
    // Push 3, pop 3, push 3 again — forces head/tail to wrap around the buffer.
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 3; ++i) EXPECT_TRUE(q.try_push(round * 3 + i));
        for (int i = 0; i < 3; ++i) {
            int val = -1;
            EXPECT_TRUE(q.try_pop(val));
            EXPECT_EQ(val, round * 3 + i);
        }
    }
}

TEST(SPSCQueue, SizeApprox) {
    SPSCQueue<int, 16> q;
    EXPECT_EQ(q.size_approx(), 0u);
    q.try_push(1);
    q.try_push(2);
    EXPECT_EQ(q.size_approx(), 2u);
    int v;
    q.try_pop(v);
    EXPECT_EQ(q.size_approx(), 1u);
}

TEST(SPSCQueue, Capacity) {
    SPSCQueue<int, 64> q;
    EXPECT_EQ(q.capacity(), 64u);
}

TEST(SPSCQueue, EmptyPopReturnsFalse) {
    SPSCQueue<int, 4> q;
    int val = 99;
    EXPECT_FALSE(q.try_pop(val));
    EXPECT_EQ(val, 99);
}

TEST(SPSCQueue, StructPayload) {
    struct Msg { uint64_t id; double value; };
    SPSCQueue<Msg, 8> q;
    EXPECT_TRUE(q.try_push({42, 3.14}));
    Msg m{};
    EXPECT_TRUE(q.try_pop(m));
    EXPECT_EQ(m.id, 42u);
    EXPECT_DOUBLE_EQ(m.value, 3.14);
}

// Concurrent correctness: one producer thread, one consumer thread, both
// running flat out. The consumer verifies that every value arrives exactly
// once and in order.
TEST(SPSCQueue, ConcurrentProducerConsumer) {
    constexpr size_t kCount = 1'000'000;
    SPSCQueue<uint64_t, 1024> q;
    std::atomic<bool> done{false};

    std::thread producer([&]() {
        for (uint64_t i = 0; i < kCount; ++i) {
            while (!q.try_push(i)) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });

    uint64_t expected = 0;
    bool order_ok = true;
    while (expected < kCount) {
        uint64_t val;
        if (q.try_pop(val)) {
            if (val != expected) { order_ok = false; break; }
            ++expected;
        } else if (done.load(std::memory_order_acquire) && q.size_approx() == 0) {
            break;
        }
    }

    producer.join();
    EXPECT_TRUE(order_ok);
    EXPECT_EQ(expected, kCount);
}

// Burst: producer writes a batch, consumer reads a batch, repeat. Tests
// that the queue handles interleaved bursts without corruption.
TEST(SPSCQueue, BurstPattern) {
    constexpr size_t kBatch = 31;
    constexpr size_t kRounds = 200;
    SPSCQueue<uint32_t, 64> q;

    uint32_t next_write = 0;
    uint32_t next_read = 0;
    for (size_t r = 0; r < kRounds; ++r) {
        for (size_t i = 0; i < kBatch; ++i) {
            EXPECT_TRUE(q.try_push(next_write++));
        }
        for (size_t i = 0; i < kBatch; ++i) {
            uint32_t val;
            EXPECT_TRUE(q.try_pop(val));
            EXPECT_EQ(val, next_read++);
        }
    }
}
