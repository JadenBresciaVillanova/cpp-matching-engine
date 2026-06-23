#pragma once

// ref_order_book.hpp
// Deliberately slow, obviously-correct reference implementation of the order
// book. Exists to be cross-checked against the fast OrderBook on millions of
// randomized operations. Optimized for readability, not speed.
//
// Data structures:
//   std::map<Price, std::deque<RefOrder>>  per side.
//   - map gives ordered iteration, so best bid = rbegin, best ask = begin.
//   - deque gives FIFO within a level: push_back on arrival, pop_front on fill.
//   - No caching. Re-derive best prices from the maps on every call.

#include "lob/types.hpp"
#include "lob/order_book.hpp" // Trade, ExecResult (shared data types)

#include <algorithm>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <unordered_map>

namespace lob {

struct RefOrder {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity remaining;
    OwnerId  owner;
};

class RefOrderBook {
public:
    using TradeSink = std::function<void(const Trade&)>;

    RefOrderBook(std::size_t = 0, std::size_t = 0) {}

    void set_trade_sink(TradeSink sink) { trade_sink_ = std::move(sink); }

    Price best_bid() const noexcept {
        return bids_.empty() ? Price{-1} : bids_.rbegin()->first;
    }
    Price best_ask() const noexcept {
        return asks_.empty() ? Price{-1} : asks_.begin()->first;
    }
    Quantity bid_size_at(Price p) const noexcept { return level_total(bids_, p); }
    Quantity ask_size_at(Price p) const noexcept { return level_total(asks_, p); }
    std::size_t open_orders() const noexcept { return id_index_.size(); }

    ExecResult submit(OrderId id, Side side, OrderType type, Price price,
                      Quantity qty, OwnerId owner = 0) {
        ExecResult r;
        r.id = id;

        if (type == OrderType::Fok && !can_fill(side, price, qty, owner))
            return r;

        Price limit = price;
        if (type == OrderType::Market)
            limit = (side == Side::Buy) ? std::numeric_limits<Price>::max()
                                        : std::numeric_limits<Price>::min();

        Quantity remaining = qty;
        r.filled = do_match(id, side, limit, remaining, owner);
        r.accepted = true;
        r.fully_filled = (remaining == 0);

        if (remaining > 0 && type == OrderType::Limit) {
            bool would_cross =
                (side == Side::Buy  && best_ask() >= 0 && price >= best_ask()) ||
                (side == Side::Sell && best_bid() >= 0 && price <= best_bid());
            if (!would_cross) {
                rest(id, side, price, remaining, owner);
                r.resting = remaining;
            }
        }
        return r;
    }

    bool cancel(OrderId id) {
        auto it = id_index_.find(id);
        if (it == id_index_.end()) return false;
        auto& [side, price, owner] = it->second;
        auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto lvl = book.find(price);
        auto& dq = lvl->second;
        dq.erase(std::find_if(dq.begin(), dq.end(),
                 [id](const RefOrder& o) { return o.id == id; }));
        if (dq.empty()) book.erase(lvl);
        id_index_.erase(it);
        return true;
    }

    ExecResult modify(OrderId id, Price new_price, Quantity new_qty) {
        auto it = id_index_.find(id);
        if (it == id_index_.end()) return {};

        auto [side, old_price, owner] = it->second;

        if (new_qty == 0) {
            cancel(id);
            ExecResult r;
            r.id = id;
            r.accepted = true;
            return r;
        }

        auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto& dq = book.at(old_price);
        auto oi = std::find_if(dq.begin(), dq.end(),
                  [id](const RefOrder& o) { return o.id == id; });
        Quantity old_rem = oi->remaining;

        if (new_price == old_price && new_qty <= old_rem) {
            oi->remaining = new_qty;
            ExecResult r;
            r.id = id;
            r.accepted = true;
            r.resting = new_qty;
            return r;
        }

        cancel(id);
        return submit(id, side, OrderType::Limit, new_price, new_qty, owner);
    }

private:
    struct Info { Side side; Price price; OwnerId owner; };
    using Book = std::map<Price, std::deque<RefOrder>>;

    static Quantity level_total(const Book& b, Price p) noexcept {
        auto it = b.find(p);
        if (it == b.end()) return 0;
        Quantity t = 0;
        for (const auto& o : it->second) t += o.remaining;
        return t;
    }

    void rest(OrderId id, Side side, Price price, Quantity qty, OwnerId owner) {
        auto& book = (side == Side::Buy) ? bids_ : asks_;
        book[price].push_back({id, side, price, qty, owner});
        id_index_[id] = {side, price, owner};
    }

    Quantity do_match(OrderId in_id, Side in_side, Price limit,
                      Quantity& remaining, OwnerId in_owner) {
        Quantity filled = 0;
        auto& opp = (in_side == Side::Buy) ? asks_ : bids_;

        while (remaining > 0 && !opp.empty()) {
            auto best = (in_side == Side::Buy) ? opp.begin()
                                               : std::prev(opp.end());
            if (in_side == Side::Buy  && best->first > limit) break;
            if (in_side == Side::Sell && best->first < limit) break;

            auto& dq = best->second;
            auto& rst = dq.front();

            if (in_owner != 0 && in_owner == rst.owner) break;

            Quantity traded = std::min(remaining, rst.remaining);
            if (trade_sink_) {
                trade_sink_({rst.id, in_id, rst.price, traded, seq_++});
            } else {
                ++seq_;
            }

            remaining -= traded;
            rst.remaining -= traded;
            filled += traded;

            if (rst.remaining == 0) {
                OrderId rid = rst.id;
                dq.pop_front();
                id_index_.erase(rid);
                if (dq.empty()) opp.erase(best);
            }
        }
        return filled;
    }

    bool can_fill(Side in_side, Price limit, Quantity qty,
                  OwnerId in_owner) const noexcept {
        Quantity avail = 0;
        const auto& opp = (in_side == Side::Buy) ? asks_ : bids_;

        if (in_side == Side::Buy) {
            for (auto it = opp.begin(); it != opp.end() && it->first <= limit; ++it) {
                if (!scan_level(it->second, in_owner, avail)) return avail >= qty;
                if (avail >= qty) return true;
            }
        } else {
            for (auto it = opp.rbegin(); it != opp.rend() && it->first >= limit; ++it) {
                if (!scan_level(it->second, in_owner, avail)) return avail >= qty;
                if (avail >= qty) return true;
            }
        }
        return avail >= qty;
    }

    // Walk a level's FIFO, accumulating available qty. Returns false if a
    // same-owner order is encountered (cancel-incoming STP would stop here).
    static bool scan_level(const std::deque<RefOrder>& dq, OwnerId in_owner,
                           Quantity& avail) noexcept {
        for (const auto& o : dq) {
            if (in_owner != 0 && o.owner == in_owner) return false;
            avail += o.remaining;
        }
        return true;
    }

    Book bids_, asks_;
    std::unordered_map<OrderId, Info> id_index_;
    Sequence seq_ = 0;
    TradeSink trade_sink_;
};

} // namespace lob
