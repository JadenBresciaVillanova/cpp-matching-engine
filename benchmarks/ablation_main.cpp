// ablation_main.cpp
// Ablation study: isolate each optimization's latency contribution by changing
// ONE design variable at a time from the baseline. Each variant is compiled
// via if-constexpr from a single template, so the only difference between
// builds is the specific data structure / strategy swap.
//
// Usage:
//   lob_ablation          interactive menu
//   lob_ablation 1        run single ablation
//   lob_ablation all      run all, show comparison + waterfall

#include "lob/order_book.hpp"
#include "lob/rdtsc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace lob;

namespace {

// ---------------------------------------------------------------------------
// Heap allocator: same handle-based interface as Pool<Order>, but every
// allocate does a real `new` and every deallocate does a real `delete`.
// This is the thing the pool exists to avoid.
// ---------------------------------------------------------------------------
struct HeapPool {
    std::vector<Order*> ptrs;
    std::vector<PoolHandle> free_list;
    size_t in_use_ = 0;

    explicit HeapPool(size_t cap) { ptrs.reserve(cap); }
    ~HeapPool() { for (auto* p : ptrs) delete p; }

    PoolHandle allocate() {
        ++in_use_;
        if (!free_list.empty()) {
            PoolHandle h = free_list.back();
            free_list.pop_back();
            ptrs[h] = new Order();
            return h;
        }
        ptrs.push_back(new Order());
        return static_cast<PoolHandle>(ptrs.size() - 1);
    }
    void deallocate(PoolHandle h) {
        delete ptrs[h];
        ptrs[h] = nullptr;
        free_list.push_back(h);
        --in_use_;
    }
    Order& operator[](PoolHandle h) { return *ptrs[h]; }
    const Order& operator[](PoolHandle h) const { return *ptrs[h]; }
    size_t in_use() const { return in_use_; }
};

// ---------------------------------------------------------------------------
// Policy enums — each ablation changes exactly ONE of these.
// ---------------------------------------------------------------------------
enum class LevelPolicy { Array, Map };
enum class AllocPolicy { Pool, Heap };
enum class BBOPolicy   { Cached, Scan };

// ---------------------------------------------------------------------------
// AblationBook: a single template that compiles into each variant via
// if-constexpr. Supports limit submit (with matching), cancel, best bid/ask.
// ---------------------------------------------------------------------------
template <LevelPolicy LP, AllocPolicy AP, BBOPolicy BP>
class AblationBook {
public:
    AblationBook(size_t tick_range, size_t max_orders)
        : tick_range_(tick_range), pool_(max_orders) {
        if constexpr (LP == LevelPolicy::Array) array_levels_.resize(tick_range);
        id_index_.reserve(max_orders);
    }

    ExecResult submit(OrderId id, Side side, OrderType, Price price,
                      Quantity qty, OwnerId = 0) {
        ExecResult r;
        r.id = id;
        r.accepted = true;
        Quantity remaining = qty;
        r.filled = match(side, price, remaining);
        r.fully_filled = (remaining == 0);
        if (remaining > 0) {
            rest(id, side, price, remaining);
            r.resting = remaining;
        }
        return r;
    }

    bool cancel(OrderId id) {
        auto it = id_index_.find(id);
        if (it == id_index_.end()) return false;
        unlink_and_free(it->second);
        id_index_.erase(it);
        return true;
    }

    Price best_bid() const noexcept {
        if constexpr (BP == BBOPolicy::Cached) return best_bid_;
        else return scan_best_bid();
    }
    Price best_ask() const noexcept {
        if constexpr (BP == BBOPolicy::Cached) return best_ask_;
        else return scan_best_ask();
    }
    size_t open_orders() const noexcept { return pool_.in_use(); }

    void set_trade_sink(std::function<void(const Trade&)>) {}

private:
    // -- level access ---------------------------------------------------------
    PriceLevel& level_at(Price p) {
        if constexpr (LP == LevelPolicy::Array) return array_levels_[static_cast<size_t>(p)];
        else return map_levels_[p];
    }
    const PriceLevel& level_at_const(Price p) const {
        if constexpr (LP == LevelPolicy::Array) return array_levels_[static_cast<size_t>(p)];
        else {
            auto it = map_levels_.find(p);
            static const PriceLevel empty{};
            return (it != map_levels_.end()) ? it->second : empty;
        }
    }

    // -- order access ---------------------------------------------------------
    Order& order_at(PoolHandle h) { return pool_[h]; }
    const Order& order_at(PoolHandle h) const { return pool_[h]; }

    // -- BBO scan (only compiled for Scan policy) -----------------------------
    Price scan_best_bid() const noexcept {
        if constexpr (LP == LevelPolicy::Array) {
            for (Price p = static_cast<Price>(tick_range_) - 1; p >= 0; --p)
                if (!array_levels_[static_cast<size_t>(p)].empty()) return p;
            return -1;
        } else {
            if (map_levels_.empty()) return -1;
            return map_levels_.rbegin()->first;
        }
    }
    Price scan_best_ask() const noexcept {
        if constexpr (LP == LevelPolicy::Array) {
            for (Price p = 0; p < static_cast<Price>(tick_range_); ++p)
                if (!array_levels_[static_cast<size_t>(p)].empty()) return p;
            return -1;
        } else {
            if (map_levels_.empty()) return -1;
            return map_levels_.begin()->first;
        }
    }

    // -- reprice (only for Cached BBO) ----------------------------------------
    void reprice_bid() noexcept {
        if constexpr (BP == BBOPolicy::Scan) return;
        if constexpr (LP == LevelPolicy::Array) {
            Price p = best_bid_;
            while (p >= 0 && array_levels_[static_cast<size_t>(p)].empty()) --p;
            best_bid_ = p;
        } else {
            auto it = map_levels_.lower_bound(best_bid_);
            if (it == map_levels_.begin()) { best_bid_ = -1; return; }
            --it;
            best_bid_ = it->first;
        }
    }
    void reprice_ask() noexcept {
        if constexpr (BP == BBOPolicy::Scan) return;
        if constexpr (LP == LevelPolicy::Array) {
            Price p = best_ask_;
            const auto max_p = static_cast<Price>(tick_range_);
            while (p >= 0 && p < max_p && array_levels_[static_cast<size_t>(p)].empty()) ++p;
            best_ask_ = (p >= 0 && p < max_p && !array_levels_[static_cast<size_t>(p)].empty()) ? p : -1;
        } else {
            auto it = map_levels_.upper_bound(best_ask_);
            best_ask_ = (it != map_levels_.end()) ? it->first : Price{-1};
        }
    }

    // -- matching sweep -------------------------------------------------------
    Quantity match(Side side, Price limit, Quantity& remaining) {
        Quantity filled = 0;
        while (remaining > 0) {
            Price bp = (side == Side::Buy) ? best_ask() : best_bid();
            if (bp < 0) break;
            if (side == Side::Buy  && bp > limit) break;
            if (side == Side::Sell && bp < limit) break;

            PriceLevel& lvl = level_at(bp);
            PoolHandle rh = lvl.head;
            Order& rst = order_at(rh);

            Quantity traded = std::min(remaining, rst.remaining);
            remaining -= traded;
            rst.remaining -= traded;
            lvl.total -= traded;
            filled += traded;

            if (rst.remaining == 0) {
                OrderId rid = rst.id;
                unlink_and_free(rh);
                id_index_.erase(rid);
            }
        }
        return filled;
    }

    // -- rest order -----------------------------------------------------------
    void rest(OrderId id, Side side, Price price, Quantity qty) {
        PoolHandle h = pool_.allocate();
        if (h == kNullHandle) return;
        Order& o = order_at(h);
        o.remaining = qty;
        o.prev = o.next = kNullHandle;
        o.id = id;
        o.price = price;
        o.side = side;
        o.owner = 0;

        PriceLevel& lvl = level_at(price);
        if (lvl.empty()) { lvl.head = lvl.tail = h; }
        else { order_at(lvl.tail).next = h; o.prev = lvl.tail; lvl.tail = h; }
        lvl.total += qty;
        id_index_.emplace(id, h);

        if constexpr (BP == BBOPolicy::Cached) {
            if (side == Side::Buy)  { if (best_bid_ < 0 || price > best_bid_) best_bid_ = price; }
            else                    { if (best_ask_ < 0 || price < best_ask_) best_ask_ = price; }
        }
    }

    // -- unlink and free ------------------------------------------------------
    void unlink_and_free(PoolHandle h) noexcept {
        Order& o = order_at(h);
        PriceLevel& lvl = level_at(o.price);

        if (o.prev != kNullHandle) order_at(o.prev).next = o.next;
        else                       lvl.head = o.next;
        if (o.next != kNullHandle) order_at(o.next).prev = o.prev;
        else                       lvl.tail = o.prev;

        lvl.total -= o.remaining;
        Side side = o.side;
        Price price = o.price;
        pool_.deallocate(h);

        if (lvl.empty()) {
            if constexpr (LP == LevelPolicy::Map) map_levels_.erase(price);
            if constexpr (BP == BBOPolicy::Cached) {
                if (side == Side::Buy  && price == best_bid_) reprice_bid();
                if (side == Side::Sell && price == best_ask_) reprice_ask();
            }
        }
    }

    // -- storage --------------------------------------------------------------
    std::vector<PriceLevel> array_levels_;     // used when LP == Array
    std::map<Price, PriceLevel> map_levels_;   // used when LP == Map
    std::conditional_t<AP == AllocPolicy::Pool, Pool<Order>, HeapPool> pool_;
    std::unordered_map<OrderId, PoolHandle> id_index_;
    Price best_bid_ = -1, best_ask_ = -1;     // used when BP == Cached
    size_t tick_range_;
};

// Instantiations — each changes exactly ONE policy from the baseline.
using Baseline   = AblationBook<LevelPolicy::Array, AllocPolicy::Pool, BBOPolicy::Cached>;
using MapVar     = AblationBook<LevelPolicy::Map,   AllocPolicy::Pool, BBOPolicy::Cached>;
using HeapVar    = AblationBook<LevelPolicy::Array, AllocPolicy::Heap, BBOPolicy::Cached>;
using ScanVar    = AblationBook<LevelPolicy::Array, AllocPolicy::Pool, BBOPolicy::Scan>;

// ---------------------------------------------------------------------------
// Benchmark runner (same mixed workload for every variant)
// ---------------------------------------------------------------------------
struct Stats {
    std::vector<uint64_t> samples;
    explicit Stats(size_t cap) { samples.reserve(cap); }
    void record(uint64_t c) { samples.push_back(c); }
    void finalize() { std::sort(samples.begin(), samples.end()); }
    size_t count() const { return samples.size(); }
    uint64_t pct(double p) const {
        if (samples.empty()) return 0;
        return samples[static_cast<size_t>(p / 100.0 * static_cast<double>(samples.size() - 1))];
    }
    uint64_t max_val() const { return samples.empty() ? 0 : samples.back(); }
};

constexpr size_t kTicks  = 2'000;
constexpr size_t kPool   = 100'000;
constexpr Price  kMid    = 1'000;
constexpr size_t kSeed   = 5'000;
constexpr size_t kWarmup = 20'000;
constexpr size_t kOps    = 200'000;

template <typename Book>
Stats run_workload(uint64_t seed) {
    Book book(kTicks, kPool);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> udist(0.0, 1.0);

    std::vector<OrderId> live;
    live.reserve(kPool);
    OrderId next_id = 1;

    // Seed book
    for (size_t i = 0; i < kSeed; ++i) {
        Price bp = kMid - 1 - static_cast<Price>(rng() % 80);
        book.submit(next_id, Side::Buy, OrderType::Limit, bp, 1 + rng() % 20);
        live.push_back(next_id++);
        Price ap = kMid + 1 + static_cast<Price>(rng() % 80);
        book.submit(next_id, Side::Sell, OrderType::Limit, ap, 1 + rng() % 20);
        live.push_back(next_id++);
    }

    Stats stats(kOps);

    auto run_op = [&]() {
        double roll = udist(rng);

        if (roll < 0.30 || live.size() < 100) {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy)
                ? kMid - 1 - static_cast<Price>(rng() % 80)
                : kMid + 1 + static_cast<Price>(rng() % 80);
            OrderId id = next_id++;
            unsigned c0, c1;
            uint64_t t0 = read_tsc(c0);
            auto r = book.submit(id, side, OrderType::Limit, price, 1 + rng() % 20);
            uint64_t t1 = read_tsc(c1);
            if (r.resting > 0) live.push_back(id);
            return (c0 == c1) ? t1 - t0 : uint64_t(0);
        }
        if (roll < 0.55) {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy)
                ? kMid + 100 : kMid - 100;
            OrderId id = next_id++;
            unsigned c0, c1;
            uint64_t t0 = read_tsc(c0);
            book.submit(id, side, OrderType::Limit, price, 1 + rng() % 3);
            uint64_t t1 = read_tsc(c1);
            return (c0 == c1) ? t1 - t0 : uint64_t(0);
        }
        if (roll < 0.80 && !live.empty()) {
            size_t idx = rng() % live.size();
            OrderId cid = live[idx];
            unsigned c0, c1;
            uint64_t t0 = read_tsc(c0);
            book.cancel(cid);
            uint64_t t1 = read_tsc(c1);
            live[idx] = live.back(); live.pop_back();
            return (c0 == c1) ? t1 - t0 : uint64_t(0);
        }
        if (!live.empty()) {
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy)
                ? kMid - 1 - static_cast<Price>(rng() % 80)
                : kMid + 1 + static_cast<Price>(rng() % 80);
            OrderId id = next_id++;
            unsigned c0, c1;
            uint64_t t0 = read_tsc(c0);
            auto r = book.submit(id, side, OrderType::Limit, price, 1 + rng() % 20);
            uint64_t t1 = read_tsc(c1);
            if (r.resting > 0) live.push_back(id);
            return (c0 == c1) ? t1 - t0 : uint64_t(0);
        }
        return uint64_t(0);
    };

    for (size_t i = 0; i < kWarmup; ++i) run_op();
    for (size_t i = 0; i < kOps; ++i) {
        uint64_t c = run_op();
        if (c > 0) stats.record(c);
    }
    return stats;
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
struct Result {
    const char* name;
    Stats stats;
    uint64_t p50_ns, p99_ns, p999_ns, max_ns;
};

Result measure(const char* name, Stats s, const TscCalibration& cal) {
    s.finalize();
    uint64_t p50  = cal.cycles_to_ns(s.pct(50));
    uint64_t p99  = cal.cycles_to_ns(s.pct(99));
    uint64_t p999 = cal.cycles_to_ns(s.pct(99.9));
    uint64_t mx   = cal.cycles_to_ns(s.max_val());
    return {name, std::move(s), p50, p99, p999, mx};
}

void print_row(const Result& r, const Result* base = nullptr) {
    std::printf("  %-34s %5llu   %5llu   %6llu   %7llu",
        r.name,
        (unsigned long long)r.p50_ns,
        (unsigned long long)r.p99_ns,
        (unsigned long long)r.p999_ns,
        (unsigned long long)r.max_ns);
    if (base && base->p50_ns > 0) {
        double pct = 100.0 * (static_cast<double>(r.p50_ns) / static_cast<double>(base->p50_ns) - 1.0);
        if (pct > 0) std::printf("   +%.0f%%", pct);
        else         std::printf("   --");
    }
    std::printf("\n");
}

void print_waterfall(const Result& base, const Result* variants, size_t n,
                     const char** labels) {
    std::printf("\n  Impact waterfall (p50 overhead vs baseline):\n\n");
    struct Entry { const char* label; int64_t delta; };
    std::vector<Entry> entries;
    for (size_t i = 0; i < n; ++i) {
        int64_t d = static_cast<int64_t>(variants[i].p50_ns) -
                    static_cast<int64_t>(base.p50_ns);
        entries.push_back({labels[i], d});
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.delta > b.delta; });

    int64_t max_delta = entries.empty() ? 1 : std::max(entries[0].delta, int64_t(1));
    for (const auto& e : entries) {
        int bar_len = static_cast<int>(30.0 * static_cast<double>(e.delta) /
                                       static_cast<double>(max_delta));
        if (bar_len < 0) bar_len = 0;
        std::printf("    %-28s +%4lld ns  %s\n",
            e.label,
            (long long)e.delta,
            std::string(static_cast<size_t>(bar_len), '#').c_str());
    }
}

#ifdef _WIN32
#include <windows.h>
void enable_ansi() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
void enable_ansi() {}
#endif

void run_single(int mode, const TscCalibration& cal) {
    constexpr uint64_t seed = 42;

    std::printf("\n  Running baseline...");
    std::fflush(stdout);
    auto base = measure("Baseline (all optimizations)",
                        run_workload<Baseline>(seed), cal);
    std::printf(" done\n");

    const char* label = "";
    Stats variant_stats(0);

    switch (mode) {
    case 1:
        label = "std::map price levels";
        std::printf("  Running %s...", label);
        std::fflush(stdout);
        variant_stats = run_workload<MapVar>(seed);
        break;
    case 2:
        label = "new/delete (no pool)";
        std::printf("  Running %s...", label);
        std::fflush(stdout);
        variant_stats = run_workload<HeapVar>(seed);
        break;
    case 3:
        label = "Linear BBO scan";
        std::printf("  Running %s...", label);
        std::fflush(stdout);
        variant_stats = run_workload<ScanVar>(seed);
        break;
    default:
        std::printf("  Unknown mode %d\n", mode);
        return;
    }
    auto var = measure(label, std::move(variant_stats), cal);
    std::printf(" done\n\n");

    std::printf("  Variant                               p50     p99    p99.9      max    vs base\n");
    std::printf("  -----------------------------------------------------------------------------\n");
    print_row(base);
    print_row(var, &base);
    std::printf("\n");
}

void run_all(const TscCalibration& cal) {
    constexpr uint64_t seed = 42;

    std::printf("\n  Running baseline...");
    std::fflush(stdout);
    auto base = measure("Baseline (all optimizations)",
                        run_workload<Baseline>(seed), cal);
    std::printf(" done\n");

    std::printf("  Running std::map price levels...");
    std::fflush(stdout);
    auto map_r = measure("std::map price levels",
                         run_workload<MapVar>(seed), cal);
    std::printf(" done\n");

    std::printf("  Running new/delete (no pool)...");
    std::fflush(stdout);
    auto heap_r = measure("new/delete (no pool)",
                          run_workload<HeapVar>(seed), cal);
    std::printf(" done\n");

    std::printf("  Running linear BBO scan...");
    std::fflush(stdout);
    auto scan_r = measure("Linear BBO scan",
                          run_workload<ScanVar>(seed), cal);
    std::printf(" done\n\n");

    std::printf("  Variant                               p50     p99    p99.9      max    vs base\n");
    std::printf("  -----------------------------------------------------------------------------\n");
    print_row(base);
    print_row(map_r, &base);
    print_row(heap_r, &base);
    print_row(scan_r, &base);
    std::printf("  -----------------------------------------------------------------------------\n");

    const char* labels[] = {
        "Direct-indexed array",
        "Pool allocator",
        "BBO caching"
    };
    Result variants[] = {map_r, heap_r, scan_r};
    print_waterfall(base, variants, 3, labels);
    std::printf("\n");
}

void print_menu() {
    std::printf("\n");
    std::printf("  \033[1mAblation Study: Isolating Each Optimization\033[0m\n");
    std::printf("  ==========================================\n\n");
    std::printf("  Each variant changes ONE thing from the baseline.\n");
    std::printf("  Same workload, same seed, same measurement.\n\n");
    std::printf("  Modes:\n");
    std::printf("    1    Array vs std::map         (price level data structure)\n");
    std::printf("    2    Pool vs new/delete        (allocation strategy)\n");
    std::printf("    3    Cached BBO vs linear scan (top-of-book caching)\n");
    std::printf("    all  Run all and show comparison waterfall\n\n");
}

} // namespace

int main(int argc, char* argv[]) {
    enable_ansi();

    const auto cal = TscCalibration::measure();

    std::printf("\n  TSC freq: %.2f ticks/ns  |  Workload: %zuk ops  |  Book: %zu orders/side\n",
        cal.ticks_per_ns, kOps / 1000, kSeed);
    std::printf("  NOTE: Numbers only meaningful on isolated physical core.\n");

    if (argc > 1) {
        if (std::strcmp(argv[1], "all") == 0) {
            run_all(cal);
        } else {
            int mode = std::atoi(argv[1]);
            if (mode >= 1 && mode <= 3) run_single(mode, cal);
            else { print_menu(); return 1; }
        }
        return 0;
    }

    print_menu();
    std::printf("  Enter mode (1/2/3/all): ");
    std::fflush(stdout);

    char buf[16] = {};
    if (!std::fgets(buf, sizeof(buf), stdin)) return 1;

    if (std::strncmp(buf, "all", 3) == 0) {
        run_all(cal);
    } else {
        int mode = std::atoi(buf);
        if (mode >= 1 && mode <= 3) run_single(mode, cal);
        else { std::printf("  Invalid mode.\n"); return 1; }
    }
    return 0;
}
