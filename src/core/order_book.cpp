// order_book.cpp
// Implementation of the OrderBook hot path.
//
// STAGE 1 SCAFFOLD: constructor + the trivial accessors are real. The matching
// logic (submit/cancel/modify and the private helpers) is stubbed and will be
// filled in together in stage 2, where each branch (limit rest, market sweep,
// IOC/FOK, partial fill, best-price reprice) is implemented and unit-tested as
// it lands. The stubs let the whole tree build and link now.

#include "lob/order_book.hpp"

namespace lob {

OrderBook::OrderBook(std::size_t tick_range, std::size_t max_orders)
    : levels_(tick_range), pool_(max_orders) {
    id_index_.reserve(max_orders);
}

ExecResult OrderBook::submit(OrderId id, Side side, OrderType type,
                             Price price, Quantity qty) {
    // STAGE 2, STEP 2: matching limit orders.
    //   1. Match against the opposite side honoring price-time priority.
    //   2. Rest any unmatched remainder (Limit only; Market/IOC/FOK handled in
    //      the next step - for now non-Limit types still no-op out).
    ExecResult r;
    r.id = id;

    if (type != OrderType::Limit) {
        // Market/IOC/FOK arrive in step 3. Until then, do not pretend to fill.
        return r; // accepted = false
    }

    Quantity remaining = qty;
    const Quantity filled = match(id, side, price, remaining);

    r.accepted = true;
    r.filled = filled;
    r.fully_filled = (remaining == 0);

    if (remaining > 0) {
        rest_order(id, side, price, remaining);
        r.resting = remaining;
    }
    return r;
}

// Core matching sweep. Walks the best opposite level's FIFO from the head
// (oldest first = time priority), trading min(incoming, resting) at the RESTING
// order's price each step, until the incoming order is exhausted or the book no
// longer crosses. Relies on unlink_and_free to drop fully-consumed resting
// orders and to reprice the best when a level empties.
Quantity OrderBook::match(OrderId incoming_id, Side incoming_side,
                          Price limit_price, Quantity& remaining) {
    Quantity total_filled = 0;

    while (remaining > 0) {
        // Is there a crossing level on the opposite side?
        PoolHandle resting_h = kNullHandle;
        if (incoming_side == Side::Buy) {
            // Buyer crosses the best ask if best_ask_ exists and <= limit.
            if (best_ask_ < 0 || best_ask_ > limit_price) break;
            resting_h = levels_[best_ask_].head;
        } else {
            // Seller crosses the best bid if best_bid_ exists and >= limit.
            if (best_bid_ < 0 || best_bid_ < limit_price) break;
            resting_h = levels_[best_bid_].head;
        }

        Order& resting = pool_[resting_h];
        const Quantity traded =
            (remaining < resting.remaining) ? remaining : resting.remaining;
        const Price trade_price = resting.price; // resting order has price priority

        // Emit the trade.
        if (trade_sink_) {
            Trade t;
            t.resting_id  = resting.id;
            t.incoming_id = incoming_id;
            t.price       = trade_price;
            t.quantity    = traded;
            t.seq         = seq_++;
            trade_sink_(t);
        } else {
            ++seq_;
        }

        // Apply the fill to both sides.
        remaining          -= traded;
        resting.remaining  -= traded;
        levels_[resting.price].total -= traded;
        total_filled       += traded;

        if (resting.remaining == 0) {
            // Resting order fully consumed. unlink_and_free decrements the level
            // total by remaining (now 0, so harmless) and repices best on empty.
            const OrderId rid = resting.id;
            unlink_and_free(resting_h);
            id_index_.erase(rid);
        }
        // If the incoming order is exhausted, the loop condition ends us.
    }
    return total_filled;
}

bool OrderBook::cancel(OrderId id) {
    auto it = id_index_.find(id);
    if (it == id_index_.end()) return false;
    unlink_and_free(it->second);
    id_index_.erase(it);
    return true;
}

ExecResult OrderBook::modify(OrderId /*id*/, Price /*new_price*/,
                             Quantity /*new_qty*/) {
    // TODO(stage2): in-place size decrease keeps priority; price change or size
    // increase => cancel + resubmit (loses priority).
    return ExecResult{};
}

// Append a non-crossing order onto the tail of its price level's FIFO. All O(1).
void OrderBook::rest_order(OrderId id, Side side, Price price, Quantity qty) {
    const PoolHandle h = pool_.allocate();
    if (h == kNullHandle) {
        // Pool exhausted. A real engine rejects the order here; for now we
        // simply do not rest it. (submit() will surface acceptance state once
        // it consults the pool result - tracked for the matching rewrite.)
        return;
    }

    Order& o = pool_[h];
    o.id        = id;
    o.side      = side;
    o.price     = price;
    o.remaining = qty;
    o.prev      = kNullHandle;
    o.next      = kNullHandle;

    PriceLevel& lvl = levels_[price];
    if (lvl.empty()) {
        lvl.head = h;
        lvl.tail = h;
    } else {
        // Link after the current tail to preserve FIFO / time priority.
        Order& old_tail = pool_[lvl.tail];
        old_tail.next = h;
        o.prev        = lvl.tail;
        lvl.tail      = h;
    }
    lvl.total += qty;

    id_index_.emplace(id, h);

    // Update cached best price if this order improves it.
    if (side == Side::Buy) {
        if (best_bid_ < 0 || price > best_bid_) best_bid_ = price;
    } else {
        if (best_ask_ < 0 || price < best_ask_) best_ask_ = price;
    }
}

// After the best level may have emptied, walk inward to the next non-empty
// level. This is the only non-O(1) step; it scans contiguous memory and in
// steady state moves one tick at a time. Sets the sentinel -1 if no level
// remains on that side.
void OrderBook::reprice_best_bid() noexcept {
    // Bids: best is the HIGHEST priced non-empty level, so scan downward.
    Price p = best_bid_;
    while (p >= 0 && levels_[p].empty()) --p;
    best_bid_ = (p >= 0) ? p : -1;
}

void OrderBook::reprice_best_ask() noexcept {
    // Asks: best is the LOWEST priced non-empty level, so scan upward.
    Price p = best_ask_;
    const Price max_tick = static_cast<Price>(levels_.size());
    while (p >= 0 && p < max_tick && levels_[p].empty()) ++p;
    best_ask_ = (p >= 0 && p < max_tick && !levels_[p].empty()) ? p : -1;
}

// Unlink a handle from its price level's FIFO, decrement aggregates, refresh the
// best price if the top level emptied, and return the slot to the pool. O(1)
// except for the reprice walk when the best level empties. Caller is responsible
// for erasing the id_index_ entry.
void OrderBook::unlink_and_free(PoolHandle h) noexcept {
    Order& o = pool_[h];
    PriceLevel& lvl = levels_[o.price];

    // Splice out of the doubly-linked list.
    if (o.prev != kNullHandle) pool_[o.prev].next = o.next;
    else                       lvl.head = o.next;   // was head
    if (o.next != kNullHandle) pool_[o.next].prev = o.prev;
    else                       lvl.tail = o.prev;   // was tail

    lvl.total -= o.remaining;

    const Side side = o.side;
    const Price price = o.price;
    pool_.deallocate(h);

    // If we just emptied the level that held the best price, move the best.
    if (lvl.empty()) {
        if (side == Side::Buy && price == best_bid_)  reprice_best_bid();
        else if (side == Side::Sell && price == best_ask_) reprice_best_ask();
    }
}

} // namespace lob
