// bench_main.cpp
// Benchmark harness for the limit order book matching engine.
//
// Measures per-operation latency using rdtscp cycle counting. Each operation
// type is benchmarked independently with a seeded book at steady-state depth,
// then percentile distributions are reported.
//
// The numbers this produces are ONLY meaningful on an isolated physical core
// with the frequency governor pinned. See docs/benchmarking.md for the full
// methodology and a step-by-step bare-metal runbook.

#include "lob/order_book.hpp"
#include "lob/rdtsc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace lob;

namespace {

// Collects raw cycle-count samples during measurement, computes percentiles
// after the fact. The record path is a single push_back into a pre-reserved
// vector: O(1) amortized, no branches, no allocations.
struct Stats {
    std::vector<uint64_t> samples;

    explicit Stats(size_t cap) { samples.reserve(cap); }

    void record(uint64_t cycles) { samples.push_back(cycles); }

    void finalize() { std::sort(samples.begin(), samples.end()); }

    size_t count() const { return samples.size(); }

    uint64_t pct(double p) const {
        if (samples.empty()) return 0;
        auto i = static_cast<size_t>(
            p / 100.0 * static_cast<double>(samples.size() - 1));
        return samples[i];
    }

    uint64_t max_val() const { return samples.empty() ? 0 : samples.back(); }
};

void report(const char* label, Stats& s, const TscCalibration& cal) {
    s.finalize();
    if (s.count() == 0) {
        std::printf("  %-24s  (no valid samples)\n", label);
        return;
    }
    auto ns = [&](uint64_t c) -> unsigned long long {
        return static_cast<unsigned long long>(cal.cycles_to_ns(c));
    };
    std::printf("  %-24s  n=%-7zu  p50=%5llu  p99=%5llu  p99.9=%6llu  max=%7llu ns\n",
        label, s.count(),
        ns(s.pct(50)), ns(s.pct(99)), ns(s.pct(99.9)), ns(s.max_val()));
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
constexpr size_t kTicks   = 10'000;
constexpr size_t kPool    = 200'000;
constexpr Price  kMid     = 5'000;
constexpr size_t kSeed    = 10'000;   // orders per side
constexpr size_t kWarmup  = 50'000;
constexpr size_t kOps     = 500'000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
Price rand_bid(std::mt19937_64& rng) {
    return kMid - 1 - static_cast<Price>(rng() % 100);
}

Price rand_ask(std::mt19937_64& rng) {
    return kMid + 1 + static_cast<Price>(rng() % 100);
}

OrderId seed_book(OrderBook& book, std::mt19937_64& rng, Quantity qty) {
    OrderId id = 1;
    for (size_t i = 0; i < kSeed; ++i)
        book.submit(id++, Side::Buy, OrderType::Limit, rand_bid(rng), qty);
    for (size_t i = 0; i < kSeed; ++i)
        book.submit(id++, Side::Sell, OrderType::Limit, rand_ask(rng), qty);
    return id;
}

// ---------------------------------------------------------------------------
// submit (resting): passive limit orders behind the spread.
// Each iteration: measured submit, unmeasured cancel to stay at steady state.
// ---------------------------------------------------------------------------
void bench_add(Stats& stats) {
    OrderBook book(kTicks, kPool);
    std::mt19937_64 rng(10);
    OrderId id = seed_book(book, rng, 1 + rng() % 20);

    for (size_t i = 0; i < kWarmup + kOps; ++i) {
        Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        Price price = (side == Side::Buy)
            ? kMid - 101 - static_cast<Price>(rng() % 100)
            : kMid + 101 + static_cast<Price>(rng() % 100);

        unsigned c0, c1;
        uint64_t t0 = read_tsc(c0);
        book.submit(id, side, OrderType::Limit, price, 1 + rng() % 10);
        uint64_t t1 = read_tsc(c1);

        if (i >= kWarmup && c0 == c1) stats.record(t1 - t0);
        book.cancel(id);
        ++id;
    }
}

// ---------------------------------------------------------------------------
// submit (match): aggressive market orders that cross the spread.
// Seed with qty=1 so each match fully consumes one resting order. Replenish
// one passive order per iteration to maintain depth.
// ---------------------------------------------------------------------------
void bench_match(Stats& stats) {
    OrderBook book(kTicks, kPool);
    std::mt19937_64 rng(20);
    OrderId id = seed_book(book, rng, 1);

    for (size_t i = 0; i < kWarmup + kOps; ++i) {
        Side rs = (rng() & 1) ? Side::Buy : Side::Sell;
        book.submit(id++, rs, OrderType::Limit,
            (rs == Side::Buy) ? rand_bid(rng) : rand_ask(rng), 1);

        Side side = (rng() & 1) ? Side::Buy : Side::Sell;

        unsigned c0, c1;
        uint64_t t0 = read_tsc(c0);
        book.submit(id, side, OrderType::Market, 0, 1);
        uint64_t t1 = read_tsc(c1);

        if (i >= kWarmup && c0 == c1) stats.record(t1 - t0);
        ++id;
    }
}

// ---------------------------------------------------------------------------
// cancel: cancel a random resting order, then replenish.
// ---------------------------------------------------------------------------
void bench_cancel(Stats& stats) {
    OrderBook book(kTicks, kPool);
    std::mt19937_64 rng(30);
    std::vector<OrderId> live;
    live.reserve(kSeed * 2 + 1);
    OrderId id = 1;

    for (size_t i = 0; i < kSeed; ++i) {
        book.submit(id, Side::Buy, OrderType::Limit,
            rand_bid(rng), 1 + rng() % 20);
        live.push_back(id++);
        book.submit(id, Side::Sell, OrderType::Limit,
            rand_ask(rng), 1 + rng() % 20);
        live.push_back(id++);
    }

    for (size_t i = 0; i < kWarmup + kOps; ++i) {
        size_t idx = rng() % live.size();
        OrderId cid = live[idx];

        unsigned c0, c1;
        uint64_t t0 = read_tsc(c0);
        book.cancel(cid);
        uint64_t t1 = read_tsc(c1);

        live[idx] = live.back();
        live.pop_back();
        if (i >= kWarmup && c0 == c1) stats.record(t1 - t0);

        Side side = (rng() & 1) ? Side::Buy : Side::Sell;
        book.submit(id, side, OrderType::Limit,
            (side == Side::Buy) ? rand_bid(rng) : rand_ask(rng),
            1 + rng() % 20);
        live.push_back(id);
        ++id;
    }
}

// ---------------------------------------------------------------------------
// modify: modify a random resting order (mix of in-place and cancel+resubmit).
// Tracks (id, side) so modifies stay on the correct side of the book.
// ---------------------------------------------------------------------------
void bench_modify(Stats& stats) {
    OrderBook book(kTicks, kPool);
    std::mt19937_64 rng(40);

    struct Entry { OrderId id; Side side; };
    std::vector<Entry> live;
    live.reserve(kSeed * 2);
    OrderId id = 1;

    for (size_t i = 0; i < kSeed; ++i) {
        book.submit(id, Side::Buy, OrderType::Limit,
            rand_bid(rng), 5 + rng() % 20);
        live.push_back({id++, Side::Buy});
        book.submit(id, Side::Sell, OrderType::Limit,
            rand_ask(rng), 5 + rng() % 20);
        live.push_back({id++, Side::Sell});
    }

    auto replace = [&](size_t idx) {
        live[idx] = live.back();
        live.pop_back();
        Side s = (rng() & 1) ? Side::Buy : Side::Sell;
        book.submit(id, s, OrderType::Limit,
            (s == Side::Buy) ? rand_bid(rng) : rand_ask(rng), 5 + rng() % 20);
        live.push_back({id, s});
        ++id;
    };

    for (size_t i = 0; i < kWarmup + kOps; ++i) {
        if (live.empty()) break;
        size_t idx = rng() % live.size();
        auto [oid, side] = live[idx];

        Price np = (side == Side::Buy) ? rand_bid(rng) : rand_ask(rng);
        Quantity nq = 1 + rng() % 20;

        unsigned c0, c1;
        uint64_t t0 = read_tsc(c0);
        auto r = book.modify(oid, np, nq);
        uint64_t t1 = read_tsc(c1);

        if (i >= kWarmup && c0 == c1) stats.record(t1 - t0);

        if (!r.accepted || r.resting == 0)
            replace(idx);
    }
}

} // namespace

int main() {
    const auto cal = TscCalibration::measure();

    std::printf("\n  Limit Order Book -- Latency Benchmark\n");
    std::printf("  ======================================\n\n");
    std::printf("  Tick range:     %zu\n", kTicks);
    std::printf("  Pool capacity:  %zu\n", kPool);
    std::printf("  Book depth:     %zu orders/side (seeded)\n", kSeed);
    std::printf("  TSC freq:       %.2f ticks/ns\n", cal.ticks_per_ns);
    std::printf("  Warmup:         %zu ops (discarded)\n", kWarmup);
    std::printf("  Measured:       %zu ops per category\n\n", kOps);
    std::printf("  NOTE: Numbers only meaningful on an isolated physical core.\n");
    std::printf("  See docs/benchmarking.md for methodology.\n\n");

    Stats s_add(kOps), s_match(kOps), s_cancel(kOps), s_modify(kOps);

    auto wall_start = std::chrono::steady_clock::now();

    bench_add(s_add);
    bench_match(s_match);
    bench_cancel(s_cancel);
    bench_modify(s_modify);

    auto wall_end = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - wall_start).count();

    std::printf("  Operation                 Samples    p50    p99   p99.9      max\n");
    std::printf("  -----------------------------------------------------------------\n");
    report("submit (resting)", s_add, cal);
    report("submit (match)", s_match, cal);
    report("cancel", s_cancel, cal);
    report("modify", s_modify, cal);
    std::printf("  -----------------------------------------------------------------\n");
    std::printf("  Wall time: %lld ms\n\n", static_cast<long long>(wall_ms));

    return 0;
}
