#pragma once

// order_book.hpp
// -----------------------------------------------------------------------------
// The limit order book (LOB) data structure.
//
// This is the centerpiece. Everything else exists to feed it, measure it, or
// prove it correct. The design choices below are the ones an HFT engineer will
// look at first, so each is justified.
//
// PRICE-TIME PRIORITY:
//   Resting orders are matched best-price-first, and within a price level,
//   first-come-first-served (FIFO). This is the standard "price-time priority"
//   used by most equity and crypto venues. It dictates the data structure:
//   we need fast "best price" lookup AND fast "oldest order at this price".
//
// STRUCTURE: direct-indexed array of price levels.
//   levels_[tick] is the PriceLevel for that integer tick. This is an array,
//   not a std::map (red-black tree) and not a hash map. Why:
//     - A tree gives O(log n) best-price and pointer-chases across cache lines
//       on every traversal. Trees are what naive implementations reach for.
//     - A direct-indexed array gives O(1) access to any price level and lays
//       levels out contiguously in memory, so walking adjacent price levels
//       (which matching does constantly) is a sequential, cache-friendly scan.
//   The cost is memory proportional to the price *range*, not the number of
//   live orders. For a single instrument with a bounded tick range this is a
//   small, fixed, cache-resident array - an excellent trade. (Real venues with
//   enormous ranges use a hybrid; we document that as a known extension.)
//
// BEST BID / BEST ASK in O(1) amortized:
//   We cache the current best bid tick and best ask tick. On a match or cancel
//   that empties the top level, we walk inward to the next non-empty level.
//   That walk is the only non-O(1) operation, it touches contiguous memory,
//   and in steady state it moves by one level at a time. We measure it.
//
// ORDERS WITHIN A LEVEL: intrusive doubly-linked FIFO.
//   Each PriceLevel holds the pool-handle of its head (oldest) and tail
//   (newest) order. Each order stores prev/next handles. This gives:
//     - O(1) append on arrival (push to tail).
//     - O(1) pop on fill (take from head -> preserves time priority).
//     - O(1) cancel of an arbitrary order GIVEN its handle (unlink in place),
//       because we keep an id -> handle index so a cancel does not search.
//   "Intrusive" means the links live inside the Order record itself (in the
//   pool), so there is no separate list-node allocation - again, zero malloc.
//
// SINGLE-THREADED CORE:
//   This book is NOT thread-safe and intentionally so. The fastest matching
//   engines run the core on one pinned thread with no locks; concurrency is
//   pushed to the edges (ingest / publish) via SPSC ring buffers. A mutex in
//   here would be the single largest latency source. If your instinct is "add
//   threads to go faster," the lesson of this project is why that is wrong for
//   the matching core specifically.
// -----------------------------------------------------------------------------

#include "lob/types.hpp"
#include "lob/pool.hpp"

#include <vector>
#include <unordered_map>
#include <functional>

namespace lob {

// A resting order as stored in the pool. POD by design (see Pool static_assert).
// Layout note: hot fields (the ones touched on every match - remaining qty and
// the FIFO links) are grouped first so a match touches as few cache lines as
// possible.
struct Order {
    Quantity remaining = 0;   // unfilled size; order leaves the book at 0
    PoolHandle prev = kNullHandle; // older order at this level
    PoolHandle next = kNullHandle; // newer order at this level
    OrderId  id    = kNullOrderId;
    Price    price = 0;
    Side     side  = Side::Buy;
    OwnerId  owner = 0;           // for self-trade prevention; 0 = anonymous
};

// A single price level: a FIFO of orders plus an aggregate quantity so we can
// answer "how much size is resting at this price" without walking the list.
struct PriceLevel {
    PoolHandle head = kNullHandle; // oldest
    PoolHandle tail = kNullHandle; // newest
    Quantity total = 0;                                // sum of remaining at level
    bool empty() const noexcept { return head == kNullHandle; }
};

class OrderBook {
public:
    using TradeSink = std::function<void(const Trade&)>;

    // tick_range: number of representable price ticks [0, tick_range).
    // max_orders: pool capacity = peak simultaneous open orders.
    OrderBook(std::size_t tick_range, std::size_t max_orders);

    // Submit an order. Matching semantics depend on `type`. Trades generated on
    // the way are emitted to the sink in execution order. This is THE hot path.
    // `owner` identifies the participant for self-trade prevention (0 = no STP).
    ExecResult submit(OrderId id, Side side, OrderType type, Price price,
                      Quantity qty, OwnerId owner = 0);

    // Cancel a resting order by id. O(1): id -> handle -> unlink. Returns true
    // if the order was live and is now removed.
    bool cancel(OrderId id);

    // Modify: cancel-and-replace semantics. A size *decrease* keeps time
    // priority (handled in place); a price change or size *increase* loses
    // priority and is implemented as cancel + new submit. We make that policy
    // explicit because getting modify-priority wrong is a classic correctness
    // bug interviewers probe for.
    ExecResult modify(OrderId id, Price new_price, Quantity new_qty);

    // Top-of-book queries, O(1).
    Price best_bid() const noexcept { return best_bid_; }
    Price best_ask() const noexcept { return best_ask_; }
    Quantity bid_size_at(Price p) const noexcept {
        const auto& lvl = levels_[p];
        if (lvl.empty()) return 0;
        return (pool_[lvl.head].side == Side::Buy) ? lvl.total : 0;
    }
    Quantity ask_size_at(Price p) const noexcept {
        const auto& lvl = levels_[p];
        if (lvl.empty()) return 0;
        return (pool_[lvl.head].side == Side::Sell) ? lvl.total : 0;
    }

    void set_trade_sink(TradeSink sink) { trade_sink_ = std::move(sink); }

    // For tests / instrumentation.
    std::size_t open_orders() const noexcept { return pool_.in_use(); }

private:
    // --- matching ---
    // Sweep the opposite side for an incoming order, emitting trades in
    // execution order, until the incoming order is exhausted or the book no
    // longer crosses `limit_price`. For Market orders pass the worst possible
    // price so the cross test always passes while liquidity remains. Returns the
    // quantity that was filled; updates *remaining in place.
    Quantity match(OrderId incoming_id, Side incoming_side, Price limit_price,
                   Quantity& remaining, OwnerId incoming_owner);

    // Read-only check for FOK: can `qty` units be filled at prices crossing
    // `limit_price` on the opposite side? Owner-aware: with cancel-incoming
    // STP, a same-owner resting order would kill the sweep, so liquidity
    // behind it is unreachable and must not be counted.
    bool can_fill(Side incoming_side, Price limit_price,
                  Quantity qty, OwnerId incoming_owner) const noexcept;

    // Append a fully-or-partially-unmatched order onto its resting level.
    void rest_order(OrderId id, Side side, Price price, Quantity qty,
                    OwnerId owner);
    // Walk inward to refresh best_bid_/best_ask_ after a level empties.
    void reprice_best_bid() noexcept;
    void reprice_best_ask() noexcept;
    // Unlink a handle from its price level's FIFO and return its slot.
    void unlink_and_free(PoolHandle h) noexcept;

    std::vector<PriceLevel> levels_;            // direct-indexed by tick
    Pool<Order>             pool_;              // all order storage

    // Teaching implementation deliberately uses std::unordered_map here.
    // PhotonBook demonstrates the production alternative (FlatMap: open-addressed,
    // cache-friendly, zero-allocation). Same applies to std::function TradeSink
    // above vs PhotonBook's compile-time-polymorphic template Sink.
    std::unordered_map<OrderId, PoolHandle> id_index_; // id -> slot

    Price best_bid_ = -1;   // -1 sentinel: no bids
    Price best_ask_ = -1;   // -1 sentinel: no asks
    Sequence seq_ = 0;
    TradeSink trade_sink_;
};

} // namespace lob
