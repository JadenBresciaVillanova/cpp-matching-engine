// fuzz_cross_check.cpp
// Seeded randomized cross-check: feeds the same random operation stream into
// the fast OrderBook and the reference RefOrderBook, asserting they agree on
// every observable after every operation.
//
// Default: 10 seeds x 100K ops (~1-2s in CI).
// LOB_FUZZ_LONG=1: 100 seeds x 1M ops (deep run, ~1-2 min).

#include "lob/order_book.hpp"
#include "lob/ref_order_book.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace lob;

namespace {

constexpr size_t kTickRange  = 256;
constexpr size_t kMaxOrders  = 100'000;
constexpr size_t kNumOwners  = 5;

class Fuzzer {
public:
    Fuzzer(uint64_t seed)
        : rng_(seed), seed_(seed),
          fast_(kTickRange, kMaxOrders), ref_(kTickRange, kMaxOrders),
          mid_(static_cast<Price>(kTickRange / 2))
    {
        fast_.set_trade_sink([this](const Trade& t) { ft_.push_back(t); });
        ref_.set_trade_sink([this](const Trade& t) { rt_.push_back(t); });
    }

    // Returns empty string on success, or a diagnostic on first divergence.
    std::string run(size_t num_ops) {
        for (size_t i = 0; i < num_ops; ++i) {
            err_.clear();
            ft_.clear();
            rt_.clear();
            generate_and_dispatch();
            auto msg = check(i);
            if (!msg.empty()) {
                size_t minimal = shrink(i + 1);
                return msg + " | op=" + std::to_string(i)
                     + " seed=" + std::to_string(seed_)
                     + " minimal_prefix=" + std::to_string(minimal);
            }
        }
        return {};
    }

private:
    // Run silently (no shrinking) for the shrinker's binary search.
    bool run_silent(size_t num_ops) {
        for (size_t i = 0; i < num_ops; ++i) {
            err_.clear();
            ft_.clear();
            rt_.clear();
            generate_and_dispatch();
            if (!check(i).empty()) return false;
        }
        return true;
    }

    size_t shrink(size_t failing_prefix) {
        size_t lo = 1, hi = failing_prefix;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            Fuzzer trial(seed_);
            if (trial.run_silent(mid)) lo = mid + 1;
            else                       hi = mid;
        }
        return lo;
    }

    // -----------------------------------------------------------------------
    // Operation generation
    // -----------------------------------------------------------------------

    void generate_and_dispatch() {
        double roll = udist_(rng_);

        if (roll < 0.60 || live_.empty()) {
            do_submit();
        } else if (roll < 0.85) {
            do_cancel();
        } else {
            do_modify();
        }
    }

    void do_submit() {
        OrderId id = next_id_++;
        Side side = (udist_(rng_) < 0.5) ? Side::Buy : Side::Sell;

        double tr = udist_(rng_);
        OrderType type;
        if      (tr < 0.70) type = OrderType::Limit;
        else if (tr < 0.80) type = OrderType::Market;
        else if (tr < 0.90) type = OrderType::Ioc;
        else                type = OrderType::Fok;

        Price price = clamp_price(mid_ + static_cast<Price>(ndist_(rng_) * 10));
        Quantity qty = 1 + (rng_() % 20);
        OwnerId owner = 1 + (rng_() % kNumOwners);

        auto fr = fast_.submit(id, side, type, price, qty, owner);
        auto rr = ref_.submit(id, side, type, price, qty, owner);
        compare_exec("submit", fr, rr);

        if (fr.resting > 0) live_.push_back(id);
        update_mid();
    }

    void do_cancel() {
        OrderId id = pick_live();
        bool fb = fast_.cancel(id);
        bool rb = ref_.cancel(id);
        if (fb != rb) err_ = "cancel disagreement";
        remove_live(id);
    }

    void do_modify() {
        OrderId id = pick_live();
        Price np = clamp_price(mid_ + static_cast<Price>(ndist_(rng_) * 5));
        Quantity nq = 1 + (rng_() % 20);

        auto fr = fast_.modify(id, np, nq);
        auto rr = ref_.modify(id, np, nq);
        compare_exec("modify", fr, rr);

        if (!fr.accepted || fr.resting == 0) remove_live(id);
        else if (fr.resting > 0) live_.push_back(id);
    }

    // -----------------------------------------------------------------------
    // Comparison
    // -----------------------------------------------------------------------

    void compare_exec(const char* op, const ExecResult& a, const ExecResult& b) {
        if (a.accepted != b.accepted)
            err_ = std::string(op) + ": accepted " + std::to_string(a.accepted)
                 + " vs " + std::to_string(b.accepted);
        else if (a.filled != b.filled)
            err_ = std::string(op) + ": filled " + std::to_string(a.filled)
                 + " vs " + std::to_string(b.filled);
        else if (a.resting != b.resting)
            err_ = std::string(op) + ": resting " + std::to_string(a.resting)
                 + " vs " + std::to_string(b.resting);
        else if (a.fully_filled != b.fully_filled)
            err_ = std::string(op) + ": fully_filled " + std::to_string(a.fully_filled)
                 + " vs " + std::to_string(b.fully_filled);
    }

    std::string check(size_t i) {
        if (!err_.empty()) return err_;

        // Trade stream: same count, same content, same order.
        if (ft_.size() != rt_.size()) {
            return "trade_count " + std::to_string(ft_.size())
                 + " vs " + std::to_string(rt_.size());
        }
        for (size_t j = 0; j < ft_.size(); ++j) {
            auto& a = ft_[j]; auto& b = rt_[j];
            if (a.resting_id != b.resting_id || a.incoming_id != b.incoming_id ||
                a.price != b.price || a.quantity != b.quantity)
                return "trade_content at trade " + std::to_string(j);
        }

        if (fast_.best_bid() != ref_.best_bid())
            return "best_bid " + std::to_string(fast_.best_bid())
                 + " vs " + std::to_string(ref_.best_bid());
        if (fast_.best_ask() != ref_.best_ask())
            return "best_ask " + std::to_string(fast_.best_ask())
                 + " vs " + std::to_string(ref_.best_ask());
        if (fast_.open_orders() != ref_.open_orders())
            return "open_orders " + std::to_string(fast_.open_orders())
                 + " vs " + std::to_string(ref_.open_orders());

        if (i % 1000 == 0) {
            for (size_t p = 0; p < kTickRange; ++p) {
                Price pp = static_cast<Price>(p);
                if (fast_.bid_size_at(pp) != ref_.bid_size_at(pp))
                    return "bid_depth@" + std::to_string(p)
                         + " fast=" + std::to_string(fast_.bid_size_at(pp))
                         + " ref=" + std::to_string(ref_.bid_size_at(pp));
                if (fast_.ask_size_at(pp) != ref_.ask_size_at(pp))
                    return "ask_depth@" + std::to_string(p)
                         + " fast=" + std::to_string(fast_.ask_size_at(pp))
                         + " ref=" + std::to_string(ref_.ask_size_at(pp));
            }
        }
        return {};
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    Price clamp_price(Price p) const {
        return std::clamp(p, Price{1}, static_cast<Price>(kTickRange - 2));
    }

    void update_mid() {
        Price b = fast_.best_bid(), a = fast_.best_ask();
        if (b >= 0 && a >= 0) mid_ = (b + a) / 2;
    }

    OrderId pick_live() {
        if (live_.empty()) return next_id_++;
        // 90% chance to target a live id, 10% random (exercises "not found")
        if (udist_(rng_) < 0.10) return rng_() % next_id_ + 1;
        return live_[rng_() % live_.size()];
    }

    void remove_live(OrderId id) {
        auto it = std::find(live_.begin(), live_.end(), id);
        if (it != live_.end()) {
            *it = live_.back();
            live_.pop_back();
        }
    }

    std::mt19937_64 rng_;
    uint64_t seed_;
    std::uniform_real_distribution<double> udist_{0.0, 1.0};
    std::normal_distribution<double> ndist_{0.0, 1.0};

    OrderBook    fast_;
    RefOrderBook ref_;

    std::vector<Trade> ft_, rt_;
    std::vector<OrderId> live_;
    OrderId next_id_ = 1;
    Price mid_;
    std::string err_;
};

} // namespace

TEST(CrossCheck, SeedSweep) {
    const char* env = std::getenv("LOB_FUZZ_LONG");
    const size_t num_seeds = env ? 100 : 10;
    const size_t ops       = env ? 1'000'000 : 100'000;

    for (uint64_t s = 0; s < num_seeds; ++s) {
        Fuzzer f(s);
        auto msg = f.run(ops);
        ASSERT_TRUE(msg.empty()) << "seed=" << s << " " << msg;
    }
}
