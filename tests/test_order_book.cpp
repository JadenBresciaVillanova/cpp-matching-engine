// test_order_book.cpp
// Stage-1 smoke tests. The substantive correctness suite (price-time priority,
// partial fills, IOC/FOK semantics, self-trade prevention, and a reference-model
// cross-check on randomized order flow) lands in stage 2 alongside the matching
// implementation.

#include "lob/order_book.hpp"
#include "lob/ref_order_book.hpp"
#include "lob/photon_book.hpp"
#include "lob/pool.hpp"
#include <gtest/gtest.h>

using namespace lob;

template <typename T>
class BookTest : public testing::Test {};
using BookTypes = testing::Types<OrderBook, RefOrderBook, PhotonBook<CallbackSink>>;
TYPED_TEST_SUITE(BookTest, BookTypes);

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

TYPED_TEST(BookTest,ConstructsEmpty) {
    TypeParam book(/*tick_range=*/1024, /*max_orders=*/1000);
    EXPECT_EQ(book.best_bid(), -1);
    EXPECT_EQ(book.best_ask(), -1);
    EXPECT_EQ(book.open_orders(), 0u);
}

TYPED_TEST(BookTest,RestingBuySetsBestBid) {
    TypeParam book(1024, 1000);
    auto r = book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 5u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), -1);
    EXPECT_EQ(book.open_orders(), 1u);
}

TYPED_TEST(BookTest,BestBidTracksImprovementOnly) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 99, 5);   // worse, ignored
    EXPECT_EQ(book.best_bid(), 100);
    book.submit(3, Side::Buy, OrderType::Limit, 101, 5);  // better
    EXPECT_EQ(book.best_bid(), 101);
}

TYPED_TEST(BookTest,BestAskTracksLowerPrice) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 200, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 198, 5);
    EXPECT_EQ(book.best_ask(), 198);
}

TYPED_TEST(BookTest,ExactFillEmptiesBook) {
    TypeParam book(1024, 1000);
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

TYPED_TEST(BookTest,PartialFillOfIncomingRestsRemainder) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_FALSE(r.fully_filled);
    EXPECT_EQ(r.resting, 2u);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.bid_size_at(100), 2u);
}

TYPED_TEST(BookTest,PartialFillOfRestingKeepsItOnBook) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 4);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(book.ask_size_at(100), 6u);
}

TYPED_TEST(BookTest,SweepHonorsTimePriorityWithinLevel) {
    TypeParam book(1024, 1000);
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

TYPED_TEST(BookTest,SweepHonorsPricePriorityAcrossLevels) {
    TypeParam book(1024, 1000);
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

TYPED_TEST(BookTest,LimitPriceCapsTheSweep) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 2);
    book.submit(2, Side::Sell, OrderType::Limit, 102, 2);   // above buyer limit
    auto r = book.submit(3, Side::Buy, OrderType::Limit, 101, 5);
    EXPECT_EQ(r.filled, 2u);                                // not through 101
    EXPECT_EQ(r.resting, 3u);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 102);
}

TYPED_TEST(BookTest,TradeExecutesAtRestingPrice) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 98, 5);    // resting cheaper
    book.submit(2, Side::Buy, OrderType::Limit, 100, 5);    // willing to pay 100
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 98);                         // buyer gets improvement
}

TYPED_TEST(BookTest,NonCrossingOrdersBothRest) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 99, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 101, 5);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.open_orders(), 2u);
}

TYPED_TEST(BookTest,BidSizeAtReturnsZeroForAskLevel) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    EXPECT_EQ(book.bid_size_at(100), 0u);
    EXPECT_EQ(book.ask_size_at(100), 5u);
}

TYPED_TEST(BookTest,AskSizeAtReturnsZeroForBidLevel) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_EQ(book.ask_size_at(100), 0u);
    EXPECT_EQ(book.bid_size_at(100), 5u);
}

TYPED_TEST(BookTest,SizeAtEmptyLevelIsZero) {
    TypeParam book(1024, 1000);
    EXPECT_EQ(book.bid_size_at(100), 0u);
    EXPECT_EQ(book.ask_size_at(100), 0u);
}

TYPED_TEST(BookTest,FifoAggregatesLevelTotal) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 100, 7);
    EXPECT_EQ(book.bid_size_at(100), 12u);
    EXPECT_EQ(book.open_orders(), 2u);
}

TYPED_TEST(BookTest,CancelFreesSlotAndClearsBest) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1));            // double cancel fails
    EXPECT_EQ(book.open_orders(), 0u);
    EXPECT_EQ(book.best_bid(), -1);
}

TYPED_TEST(BookTest,CancelBestWalksToNextLevel) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 98, 5);
    book.cancel(1);
    EXPECT_EQ(book.best_bid(), 98);
    book.cancel(2);
    EXPECT_EQ(book.best_bid(), -1);
}

TYPED_TEST(BookTest,CancelMiddleOfFifoKeepsListIntact) {
    TypeParam book(1024, 1000);
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

// ---------------------------------------------------------------------------
// Market orders
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest,MarketBuySweepsAllLiquidity) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 101, 4);

    auto r = book.submit(3, Side::Buy, OrderType::Market, 0, 7);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 7u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 0u);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[1].price, 101);
}

TYPED_TEST(BookTest,MarketSellSweepsAcrossLevels) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 99, 3);

    auto r = book.submit(3, Side::Sell, OrderType::Market, 0, 7);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 7u);
    EXPECT_TRUE(r.fully_filled);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);   // best bid first
    EXPECT_EQ(trades[1].price, 99);
}

TYPED_TEST(BookTest,MarketIntoEmptyBookFillsNothing) {
    TypeParam book(1024, 1000);
    auto r = book.submit(1, Side::Buy, OrderType::Market, 0, 10);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_FALSE(r.fully_filled);
    EXPECT_EQ(r.resting, 0u);         // remainder cancelled, never rests
    EXPECT_EQ(book.open_orders(), 0u);
}

TYPED_TEST(BookTest,MarketPartialFillDoesNotRest) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    auto r = book.submit(2, Side::Buy, OrderType::Market, 0, 10);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_FALSE(r.fully_filled);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 0u);
}

// ---------------------------------------------------------------------------
// IOC (Immediate Or Cancel)
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest,IocFillsAndCancelsRemainder) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    auto r = book.submit(2, Side::Buy, OrderType::Ioc, 100, 5);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_FALSE(r.fully_filled);
    EXPECT_EQ(r.resting, 0u);         // remainder cancelled
    EXPECT_EQ(book.open_orders(), 0u);
}

TYPED_TEST(BookTest,IocExactFill) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    auto r = book.submit(2, Side::Buy, OrderType::Ioc, 100, 5);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(book.open_orders(), 0u);
}

TYPED_TEST(BookTest,IocRespectsLimitPrice) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 105, 4);

    // IOC buy at limit 102: should only fill the 100-level, not 105
    auto r = book.submit(3, Side::Buy, OrderType::Ioc, 102, 10);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 1u); // the 105 ask survives
    EXPECT_EQ(book.best_ask(), 105);
}

TYPED_TEST(BookTest,IocNoCrossingLiquidityFillsNothing) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 200, 5);
    auto r = book.submit(2, Side::Buy, OrderType::Ioc, 100, 5); // below ask
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 1u); // resting sell untouched
}

// ---------------------------------------------------------------------------
// FOK (Fill Or Kill)
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest,FokFillsWhenFullyAvailable) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 101, 4);

    auto r = book.submit(3, Side::Buy, OrderType::Fok, 101, 6);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 6u);
    EXPECT_TRUE(r.fully_filled);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, 100);
    EXPECT_EQ(trades[0].quantity, 3u);
    EXPECT_EQ(trades[1].price, 101);
    EXPECT_EQ(trades[1].quantity, 3u);
    EXPECT_EQ(book.ask_size_at(101), 1u); // 1 leftover on the 101 level
}

TYPED_TEST(BookTest,FokRejectsInsufficientLiquidity) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);

    auto r = book.submit(2, Side::Buy, OrderType::Fok, 100, 5);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 1u);     // book unchanged
    EXPECT_EQ(book.ask_size_at(100), 3u);
}

TYPED_TEST(BookTest,FokRejectsWhenLiquidityExistsBeyondLimit) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 105, 10);

    // 13 total, but only 3 within the 102 limit
    auto r = book.submit(3, Side::Buy, OrderType::Fok, 102, 5);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(book.open_orders(), 2u);
}

TYPED_TEST(BookTest,FokRejectsOnEmptyBook) {
    TypeParam book(1024, 1000);
    auto r = book.submit(1, Side::Buy, OrderType::Fok, 100, 5);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(book.open_orders(), 0u);
}

TYPED_TEST(BookTest,FokSellFillsFromBids) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Buy, OrderType::Limit, 99, 5);

    auto r = book.submit(3, Side::Sell, OrderType::Fok, 99, 8);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 8u);
    EXPECT_TRUE(r.fully_filled);
    EXPECT_EQ(book.bid_size_at(100), 0u);
    EXPECT_EQ(book.bid_size_at(99), 2u);  // 5 + 5 - 8 = 2 remaining on 99
}

// ---------------------------------------------------------------------------
// modify()
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest,ModifySizeDecreaseKeepsPriority) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 5);

    auto r = book.modify(1, 100, 3);   // shrink 10 -> 3, keep FIFO position
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 3u);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(book.ask_size_at(100), 8u);   // 3 + 5

    // Match: order 1 should still be first (time priority preserved)
    book.submit(3, Side::Buy, OrderType::Limit, 100, 4);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(trades[0].quantity, 3u);
    EXPECT_EQ(trades[1].resting_id, 2u);
    EXPECT_EQ(trades[1].quantity, 1u);
}

TYPED_TEST(BookTest,ModifySizeIncreaseLosesPriority) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 5);

    auto r = book.modify(1, 100, 10);   // increase: cancel + resubmit to back
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 10u);

    // Match: order 2 is now first because order 1 lost priority
    book.submit(3, Side::Buy, OrderType::Limit, 100, 6);
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting_id, 2u);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_EQ(trades[1].resting_id, 1u);
    EXPECT_EQ(trades[1].quantity, 1u);
}

TYPED_TEST(BookTest,ModifyPriceChangeMovesLevel) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    EXPECT_EQ(book.best_ask(), 100);

    auto r = book.modify(1, 101, 5);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 5u);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.ask_size_at(100), 0u);
    EXPECT_EQ(book.ask_size_at(101), 5u);
}

TYPED_TEST(BookTest,ModifyPriceChangeCrossesBook) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 105, 3);

    // Move the sell from 105 down to 99 — crosses the bid at 100
    auto r = book.modify(2, 99, 3);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_TRUE(r.fully_filled);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100);     // executes at resting bid price
    EXPECT_EQ(trades[0].quantity, 3u);
    EXPECT_EQ(book.bid_size_at(100), 2u);
}

TYPED_TEST(BookTest,ModifyCrossingPartiallyRestsRemainder) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 3);
    book.submit(2, Side::Sell, OrderType::Limit, 105, 7);

    // Move the sell to 99 — fills 3 against the bid, rests 4 at 99
    auto r = book.modify(2, 99, 7);
    EXPECT_EQ(r.filled, 3u);
    EXPECT_EQ(r.resting, 4u);
    EXPECT_EQ(book.best_ask(), 99);
    EXPECT_EQ(book.ask_size_at(99), 4u);
}

TYPED_TEST(BookTest,ModifyToZeroIsCancellation) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Buy, OrderType::Limit, 100, 5);

    auto r = book.modify(1, 100, 0);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.open_orders(), 0u);
    EXPECT_EQ(book.best_bid(), -1);
}

TYPED_TEST(BookTest,ModifyNonExistentFails) {
    TypeParam book(1024, 1000);
    auto r = book.modify(42, 100, 5);
    EXPECT_FALSE(r.accepted);
}

TYPED_TEST(BookTest,ModifyNoOpSameValues) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 3);

    auto r = book.modify(1, 100, 5);    // same price, same qty
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.resting, 5u);
    EXPECT_EQ(book.ask_size_at(100), 8u); // unchanged
}

// ---------------------------------------------------------------------------
// Self-trade prevention (cancel-incoming)
// ---------------------------------------------------------------------------

TYPED_TEST(BookTest,StpCancelsIncomingOnSameOwner) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });

    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/42);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 3, /*owner=*/42);

    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 0u);          // no self-trade
    EXPECT_EQ(r.resting, 0u);         // remainder cancelled, not rested
    EXPECT_TRUE(trades.empty());
    EXPECT_EQ(book.open_orders(), 1u); // resting sell untouched
    EXPECT_EQ(book.ask_size_at(100), 5u);
}

TYPED_TEST(BookTest,StpDoesNotFireForDifferentOwners) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/42);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 5, /*owner=*/99);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.fully_filled);
}

TYPED_TEST(BookTest,StpDisabledForAnonymousOwner) {
    TypeParam book(1024, 1000);
    // owner=0 means anonymous: STP does not apply
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/0);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 100, 5, /*owner=*/0);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.fully_filled);
}

TYPED_TEST(BookTest,StpPartialFillBeforeSameOwner) {
    TypeParam book(1024, 1000);
    std::vector<Trade> trades;
    book.set_trade_sink([&](const Trade& t) { trades.push_back(t); });

    // Two resting asks: one from a different owner, one from the same
    book.submit(1, Side::Sell, OrderType::Limit, 100, 3, /*owner=*/10);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/42);

    auto r = book.submit(3, Side::Buy, OrderType::Limit, 100, 10, /*owner=*/42);
    EXPECT_EQ(r.filled, 3u);          // traded with owner 10
    EXPECT_EQ(r.resting, 0u);         // remainder cancelled (STP)
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting_id, 1u);
    EXPECT_EQ(book.ask_size_at(100), 5u); // owner 42's sell untouched
}

TYPED_TEST(BookTest,StpWithMarketOrder) {
    TypeParam book(1024, 1000);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/42);
    auto r = book.submit(2, Side::Buy, OrderType::Market, 0, 5, /*owner=*/42);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(r.resting, 0u);
}

TYPED_TEST(BookTest,FokRejectedWhenStpWouldFire) {
    TypeParam book(1024, 1000);
    // Same owner on both sides: FOK can_fill should see that the sweep
    // would be killed by STP and reject upfront.
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/42);
    auto r = book.submit(2, Side::Buy, OrderType::Fok, 100, 5, /*owner=*/42);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.filled, 0u);
    EXPECT_EQ(book.open_orders(), 1u);
}

TYPED_TEST(BookTest,FokSucceedsWhenStpOrderIsBehindEnoughLiquidity) {
    TypeParam book(1024, 1000);
    // Different-owner liquidity comes first in the FIFO; same-owner is behind
    book.submit(1, Side::Sell, OrderType::Limit, 100, 5, /*owner=*/10);
    book.submit(2, Side::Sell, OrderType::Limit, 100, 3, /*owner=*/42);

    auto r = book.submit(3, Side::Buy, OrderType::Fok, 100, 5, /*owner=*/42);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.filled, 5u);
    EXPECT_TRUE(r.fully_filled);
}
