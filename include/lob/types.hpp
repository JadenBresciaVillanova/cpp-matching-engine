#pragma once

// types.hpp
// -----------------------------------------------------------------------------
// Fundamental domain types for the matching engine.
//
// Design notes:
//  - We use fixed-width integer types everywhere. Prices and quantities are
//    NEVER floating point. Floating point cannot exactly represent decimal
//    monetary values and introduces rounding/comparison bugs that are
//    catastrophic in a matching engine (e.g. two orders that should match at
//    an equal price failing an == comparison). Real exchanges represent price
//    as an integer number of "ticks". We do the same.
//
//  - Price is stored as an integer tick index, not a dollar value. The mapping
//    from ticks to a human-readable price lives at the presentation edge, never
//    in the hot path.
//
//  - We give every distinct concept its own strong-ish typedef so that argument
//    order mistakes (passing a Quantity where a Price is expected) are at least
//    visible at the call site. A fuller design would use opaque strong types;
//    we keep plain typedefs here for readability and revisit if needed.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <limits>

namespace lob {

// Monotonic identifier assigned to every order accepted by the book.
using OrderId = std::uint64_t;

// Price expressed as an integer number of ticks above some reference.
// Signed so that arithmetic on price differences (used in imbalance / spread
// calculations) cannot silently wrap.
using Price = std::int64_t;

// Order / trade size. Unsigned: a negative quantity is never meaningful, and
// using unsigned makes "remaining quantity hit zero" the natural terminal state.
using Quantity = std::uint64_t;

// Logical sequence number for events flowing through the engine. Used to make
// replays deterministic and to give every event a total order independent of
// wall-clock time.
using Sequence = std::uint64_t;

// Identifies the participant (firm / account) that submitted the order,
// used for self-trade prevention. Zero means anonymous / no STP.
using OwnerId = std::uint64_t;

// Sentinel meaning "no order".
inline constexpr OrderId kNullOrderId = 0;

enum class Side : std::uint8_t {
    Buy  = 0,
    Sell = 1,
};

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Order types we support. Each has distinct matching semantics:
//  - Limit : rests in the book if it does not fully match on arrival.
//  - Market: matches against the best available prices; never rests. Any
//            unfilled remainder is cancelled.
//  - Ioc   : "Immediate Or Cancel" - matches what it can right now, cancels
//            the rest. Like Market but with a price limit.
//  - Fok   : "Fill Or Kill" - either the entire quantity can be filled
//            immediately or nothing is done at all.
enum class OrderType : std::uint8_t {
    Limit  = 0,
    Market = 1,
    Ioc    = 2,
    Fok    = 3,
};

// Time-in-force: orthogonal to OrderType. Limit orders can be GTC, DAY, or GTD;
// Market orders are implicitly IOC. IOC/FOK live in OrderType because they
// change the matching algorithm (sweep semantics), not just when the order
// expires. A production exchange separates these cleanly; many toy
// implementations conflate them.
enum class TimeInForce : std::uint8_t {
    GTC = 0,   // Good-Til-Cancel: rests until explicitly cancelled (default)
    DAY = 1,   // Good-for-day: cancelled at session close
    GTD = 2,   // Good-Til-Date: cancelled at a user-specified timestamp
};

// Reported when two orders match. Pushed to a sink so the engine core never
// formats or transmits anything itself (that is edge work).
struct Trade {
    OrderId  resting_id;
    OrderId  incoming_id;
    Price    price;       // trade executes at the RESTING order's price
    Quantity quantity;
    Sequence seq;
};

// Result of submitting an order, returned to the caller / publisher.
struct ExecResult {
    OrderId  id = kNullOrderId;
    Quantity filled = 0;     // total quantity that traded on submission
    Quantity resting = 0;    // quantity left resting in the book afterwards
    bool     accepted = false;
    bool     fully_filled = false;
};

} // namespace lob
