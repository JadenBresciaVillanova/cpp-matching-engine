// test_photon.cpp
// Tests for Photon-specific features: GTD expiry, lifecycle events, FlatMap.

#include "lob/photon_book.hpp"
#include "lob/flat_map.hpp"
#include <gtest/gtest.h>
#include <vector>

using namespace lob;

// ============================================================================
// FlatMap tests
// ============================================================================

TEST(FlatMap, InsertAndFind) {
    FlatMap<uint64_t, uint32_t> m(64);
    m.insert(42, 100);
    auto* v = m.find(42);
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 100u);
}

TEST(FlatMap, FindMissing) {
    FlatMap<uint64_t, uint32_t> m(64);
    EXPECT_EQ(m.find(99), nullptr);
}

TEST(FlatMap, EraseAndFind) {
    FlatMap<uint64_t, uint32_t> m(64);
    m.insert(10, 1);
    m.insert(20, 2);
    EXPECT_TRUE(m.erase(10));
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_NE(m.find(20), nullptr);
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatMap, EraseNonexistent) {
    FlatMap<uint64_t, uint32_t> m(64);
    EXPECT_FALSE(m.erase(999));
}

TEST(FlatMap, UpdateExisting) {
    FlatMap<uint64_t, uint32_t> m(64);
    m.insert(5, 10);
    m.insert(5, 20);
    EXPECT_EQ(*m.find(5), 20u);
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatMap, ManySequentialKeys) {
    FlatMap<uint64_t, uint32_t> m(10000);
    for (uint64_t i = 1; i <= 5000; ++i) m.insert(i, static_cast<uint32_t>(i * 10));
    for (uint64_t i = 1; i <= 5000; ++i) {
        auto* v = m.find(i);
        ASSERT_NE(v, nullptr) << "key=" << i;
        EXPECT_EQ(*v, static_cast<uint32_t>(i * 10));
    }
    EXPECT_EQ(m.size(), 5000u);
}

TEST(FlatMap, InsertAfterErase) {
    FlatMap<uint64_t, uint32_t> m(64);
    m.insert(1, 10);
    m.insert(2, 20);
    m.erase(1);
    m.insert(3, 30);
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_EQ(*m.find(2), 20u);
    EXPECT_EQ(*m.find(3), 30u);
}

// ============================================================================
// Pool FIFO policy tests
// ============================================================================

TEST(PoolFifo, AllocateInOrder) {
    Pool<PhotonOrder, FreeListPolicy::Fifo> pool(4);
    auto a = pool.allocate();
    auto b = pool.allocate();
    auto c = pool.allocate();
    EXPECT_EQ(a, 0u);
    EXPECT_EQ(b, 1u);
    EXPECT_EQ(c, 2u);
}

TEST(PoolFifo, DeallocateReusesOldestFirst) {
    Pool<PhotonOrder, FreeListPolicy::Fifo> pool(4);
    auto a = pool.allocate();  // 0
    auto b = pool.allocate();  // 1
    auto c = pool.allocate();  // 2
    auto d = pool.allocate();  // 3, pool now empty
    pool.deallocate(a);  // free list: 0
    pool.deallocate(c);  // free list: 0 → 2
    pool.deallocate(b);  // free list: 0 → 2 → 1
    // FIFO: oldest freed (a=0) is reused first
    auto e = pool.allocate();
    auto f = pool.allocate();
    auto g = pool.allocate();
    EXPECT_EQ(e, a);  // 0 (freed first)
    EXPECT_EQ(f, c);  // 2 (freed second)
    EXPECT_EQ(g, b);  // 1 (freed third)
    (void)d;
}

// ============================================================================
// PhotonBook lifecycle events
// ============================================================================

struct TestSink {
    std::vector<BookUpdate> events;
    std::function<void(const Trade&)> on_trade;

    void operator()(const BookUpdate& u) {
        events.push_back(u);
        if (u.type == UpdateType::Fill && on_trade) {
            Trade t;
            t.resting_id  = u.counter_id;
            t.incoming_id = u.order_id;
            t.price       = u.price;
            t.quantity    = u.quantity;
            t.seq         = u.seq;
            on_trade(t);
        }
    }
};

TEST(Photon, AckOnRest) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    auto& ev = book.sink().events;
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, UpdateType::Ack);
    EXPECT_EQ(ev[0].order_id, 1u);
    EXPECT_EQ(ev[0].quantity, 10u);
}

TEST(Photon, FillOnMatch) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Sell, OrderType::Limit, 50, 10);
    book.sink().events.clear();

    book.submit(2, Side::Buy, OrderType::Limit, 50, 10);
    auto& ev = book.sink().events;
    ASSERT_GE(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, UpdateType::Fill);
    EXPECT_EQ(ev[0].order_id, 2u);
    EXPECT_EQ(ev[0].counter_id, 1u);
    EXPECT_EQ(ev[0].quantity, 10u);
}

TEST(Photon, CancelEvent) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    book.sink().events.clear();

    book.cancel(1);
    auto& ev = book.sink().events;
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, UpdateType::Canceled);
    EXPECT_EQ(ev[0].order_id, 1u);
}

TEST(Photon, ReplaceEvent) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    book.sink().events.clear();

    book.modify(1, 50, 5);
    auto& ev = book.sink().events;
    ASSERT_EQ(ev.size(), 1u);
    EXPECT_EQ(ev[0].type, UpdateType::Replaced);
    EXPECT_EQ(ev[0].quantity, 5u);
}

TEST(Photon, MonotonicSequence) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    book.submit(2, Side::Sell, OrderType::Limit, 60, 10);
    book.cancel(1);

    auto& ev = book.sink().events;
    for (size_t i = 1; i < ev.size(); ++i) {
        EXPECT_GT(ev[i].seq, ev[i - 1].seq);
    }
}

// ============================================================================
// GTD expiry
// ============================================================================

TEST(Photon, GtdExpiryRemovesOrder) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10, 0,
                TimeInForce::GTD, 1000);
    EXPECT_EQ(book.open_orders(), 1u);

    book.check_expiry(999);
    EXPECT_EQ(book.open_orders(), 1u);

    // Expiry is exclusive: order valid while time < expiry_ns
    book.check_expiry(1000);
    EXPECT_EQ(book.open_orders(), 0u);

    bool found_expired = false;
    for (auto& e : book.sink().events) {
        if (e.type == UpdateType::Expired && e.order_id == 1) {
            found_expired = true;
        }
    }
    EXPECT_TRUE(found_expired);
}

TEST(Photon, GtdExpiryUpdatesBook) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10, 0,
                TimeInForce::GTD, 500);
    EXPECT_EQ(book.best_bid(), 50);

    book.check_expiry(501);
    EXPECT_EQ(book.best_bid(), -1);
}

TEST(Photon, GtcDoesNotExpire) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    book.check_expiry(999999999);
    EXPECT_EQ(book.open_orders(), 1u);
}

TEST(Photon, MultipleGtdExpireInOrder) {
    PhotonBook<TestSink> book(100, 100, TestSink{});
    book.submit(1, Side::Buy, OrderType::Limit, 48, 10, 0, TimeInForce::GTD, 100);
    book.submit(2, Side::Buy, OrderType::Limit, 49, 10, 0, TimeInForce::GTD, 200);
    book.submit(3, Side::Buy, OrderType::Limit, 50, 10, 0, TimeInForce::GTD, 300);

    book.check_expiry(150);
    EXPECT_EQ(book.open_orders(), 2u);

    book.check_expiry(250);
    EXPECT_EQ(book.open_orders(), 1u);

    book.check_expiry(350);
    EXPECT_EQ(book.open_orders(), 0u);
}

// ============================================================================
// NullSink (zero-overhead) compiles and works
// ============================================================================

TEST(Photon, NullSinkCompiles) {
    PhotonBook<NullSink> book(100, 100);
    book.submit(1, Side::Buy, OrderType::Limit, 50, 10);
    book.submit(2, Side::Sell, OrderType::Limit, 50, 10);
    book.cancel(2);
    EXPECT_EQ(book.open_orders(), 0u);
}
