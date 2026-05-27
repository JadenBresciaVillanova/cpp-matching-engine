// test_order_book.cpp
// Stage-1 smoke tests. The substantive correctness suite (price-time priority,
// partial fills, IOC/FOK semantics, self-trade prevention, and a reference-model
// cross-check on randomized order flow) lands in stage 2 alongside the matching
// implementation.

#include "lob/order_book.hpp"
#include "lob/pool.hpp"
#include <gtest/gtest.h>

using namespace lob;

TEST(Pool, AllocateDeallocateRoundTrips) {
    Pool<Order> pool(4);
    EXPECT_EQ(pool.in_use(), 0u);
    auto a = pool.allocate();
    auto b = pool.allocate();
    EXPECT_NE(a, Pool<Order>::kInvalid);
    EXPECT_NE(b, Pool<Order>::kInvalid);
    EXPECT_EQ(pool.in_use(), 2u);
    pool.deallocate(a);
    EXPECT_EQ(pool.in_use(), 1u);
}

TEST(Pool, ExhaustionReturnsInvalid) {
    Pool<Order> pool(2);
    EXPECT_NE(pool.allocate(), Pool<Order>::kInvalid);
    EXPECT_NE(pool.allocate(), Pool<Order>::kInvalid);
    EXPECT_EQ(pool.allocate(), Pool<Order>::kInvalid); // exhausted
}

TEST(OrderBook, ConstructsEmpty) {
    OrderBook book(/*tick_range=*/1024, /*max_orders=*/1000);
    EXPECT_EQ(book.best_bid(), -1);
    EXPECT_EQ(book.best_ask(), -1);
    EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBook, RestingBuySetsBestBid) {
    OrderBook book(1024, 1000);
    auto r = book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 5u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), -1);
    EXPECT_EQ(book.open_orders(), 1u);
}

TEST(OrderBook, BestBidTracksImprovementOnly) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 99, 5);   // worse, ignored
    EXPECT_EQ(book.best_bid(), 100);
    book.submit(3, Side::Buy, OrderType::Limit, 101, 5);  // better
    EXPECT_EQ(book.best_bid(), 101);
}

TEST(OrderBook, BestAskTracksLowerPrice) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 200, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 198, 5);
    EXPECT_EQ(book.best_ask(), 198);
}

TEST(OrderBook, ExactFillEmptiesBook) {
    OrderBook book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 0u);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[0].incoming_id, 2u);
}

TEST(OrderBook, PartialFillOfIncomingRestsRemainder) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_FALSE(r.fully_filled);
    EXPECT_EQ(r.resting, 2u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.bid_size_at(100), 2u);
}

TEST(OrderBook, PartialFillOfRestingKeepsItOnBook) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 4);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(book.ask_size_at(100), 6u);
}

TEST(OrderBook, SweepHonorsTimePriorityWithinLevel) {
    OrderBook book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);   // oldest
    book.submit(2, Side::Sell, OrderType::Limit, 100, 3);   // newer
    book.submit(3, Side::Buy, OrderType::Limit, 100, 5);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting_id, 1u);                    // oldest first
    EXPECT_EQ(trades[0].quantity, 3u);
    EXPECT_EQ(trades[1].resting_id, 2u);
    EXPECT_EQ(trades[1].quantity, 2u);
}

TEST(OrderBook, SweepHonorsPricePriorityAcrossLevels) {
    OrderBook book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 2);   // best
    book.submit(2, Side::Sell, OrderType::Limit, 101, 2);
    book.submit(3, Side::Buy, OrderType::Limit, 101, 4);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);                        // best price first
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(book.open_orders(), 0u);
}

TEST(OrderBook, LimitPriceCapsTheSweep) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
    book.submit(2, Side::Sell, OrderType::Limit, 102, 2);   // above buyer limit
    auto r = book.submit(3, Side::Buy, OrderType::Limit, 101, 5);
    EXPECT_EQ(r.filled, 2u);                                // not through 101
    EXPECT_EQ(r.resting, 3u);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 102);
}

TEST(OrderBook, TradeExecutesAtRestingPrice) {
    OrderBook book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 98, 5);    // resting cheaper
    book.submit(2, Side::Buy, OrderType::Limit, 100, 5);    // willing to pay 100
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 98);                         // buyer gets improvement
}

TEST(OrderBook, NonCrossingOrdersBothRest) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 99, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 101, 5);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBook, FifoAggregatesLevelTotal) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 100, 7);
    EXPECT_EQ(book.bid_size_at(100), 12u);
    EXPECT_EQ(book.open_orders(), 2u);
}

TEST(OrderBook, CancelFreesSlotAndClearsBest) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1));            // double cancel fails
    EXPECT_EQ(book.open_orders(), 0u);
    EXPECT_EQ(book.best_bid(), -1);
}

TEST(OrderBook, CancelBestWalksToNextLevel) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 98, 5);
    book.cancel(1);
    EXPECT_EQ(book.best_bid(), 98);
    book.cancel(2);
    EXPECT_EQ(book.best_bid(), -1);
}

TEST(OrderBook, CancelMiddleOfFifoKeepsListIntact) {
    OrderBook book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 100, 6);
    book.submit(3, Side::Buy, OrderType::Limit, 100, 7);
    book.cancel(2);                          // unlink interior node
    EXPECT_EQ(book.bid_size_at(100), 12u);   // 5 + 7
    EXPECT_EQ(book.best_bid(), 100);
    book.cancel(1);
    book.cancel(3);
    EXPECT_EQ(book.bid_size_at(100), 0u);
    EXPECT_EQ(book.best_bid(), -1);
}
