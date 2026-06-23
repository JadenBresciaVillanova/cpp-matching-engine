// replay_main.cpp
// Deterministic replay of market event streams through the engine.
//
// Supports two modes:
//   1. Synthetic — generated from a seeded RNG (steady/sweep/stress scenarios)
//   2. File     — real market data (LOBSTER CSV or NASDAQ ITCH 5.0 binary)
//
// Usage:
//   lob_replay                               default (steady, 500K synthetic)
//   lob_replay steady|sweep|stress [count]   choose scenario + event count
//   lob_replay file <path> [--symbol SYM]    replay a data file
//   lob_replay file <path> [--ticks N] [--pool N] [--tick-div N] [--symbol SYM]
//
// The binary event format for synthetic scenarios is inspired by ITCH 5.0.
// For real data, the file parser auto-detects LOBSTER vs ITCH format.

#include "lob/order_book.hpp"
#include "lob/parsers.hpp"
#include "lob/rdtsc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
void enable_ansi_replay() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0; GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
#else
void enable_ansi_replay() {}
#endif

using namespace lob;

namespace {

// ---------------------------------------------------------------------------
// Synthetic event generation (original code, unchanged)
// ---------------------------------------------------------------------------
enum SynEventType : uint8_t { SYN_ADD = 1, SYN_CANCEL = 2, SYN_MODIFY = 3 };

#pragma pack(push, 1)
struct SynEvent {
    uint8_t  type;
    uint8_t  side;       // 0=Buy, 1=Sell
    uint16_t _pad;
    uint32_t order_id;
    int64_t  price;
    uint64_t quantity;
};
#pragma pack(pop)
static_assert(sizeof(SynEvent) == 24, "SynEvent must be 24 bytes");

constexpr size_t kSynTicks = 10'000;
constexpr Price  kSynMid   = 5'000;

struct Generator {
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> udist{0.0, 1.0};
    std::vector<uint32_t> live;
    uint32_t next_id = 1;
    Price mid = kSynMid;

    explicit Generator(uint64_t seed) : rng(seed) { live.reserve(50'000); }

    Price rand_bid() { return mid - 1 - static_cast<Price>(rng() % 60); }
    Price rand_ask() { return mid + 1 + static_cast<Price>(rng() % 60); }

    Price clamp(Price p) {
        return std::clamp(p, Price{1}, static_cast<Price>(kSynTicks - 2));
    }

    uint32_t pick_live() {
        if (live.empty()) return 0;
        return live[rng() % live.size()];
    }

    void remove_live(uint32_t id) {
        auto it = std::find(live.begin(), live.end(), id);
        if (it != live.end()) { *it = live.back(); live.pop_back(); }
    }

    SynEvent make_passive() {
        uint8_t s = (rng() & 1) ? 0 : 1;
        Price p = clamp(s == 0 ? rand_bid() : rand_ask());
        uint32_t id = next_id++;
        live.push_back(id);
        return {SYN_ADD, s, 0, id, p, 1 + rng() % 20};
    }

    SynEvent make_aggressive() {
        uint8_t s = (rng() & 1) ? 0 : 1;
        Price p = clamp(s == 0 ? mid + 50 + static_cast<Price>(rng() % 30)
                                : mid - 50 - static_cast<Price>(rng() % 30));
        uint32_t id = next_id++;
        return {SYN_ADD, s, 0, id, p, 1 + rng() % 5};
    }

    SynEvent make_cancel() {
        uint32_t id = pick_live();
        if (id == 0) return make_passive();
        remove_live(id);
        return {SYN_CANCEL, 0, 0, id, 0, 0};
    }

    SynEvent make_modify() {
        uint32_t id = pick_live();
        if (id == 0) return make_passive();
        uint8_t s = (rng() & 1) ? 0 : 1;
        Price p = clamp(s == 0 ? rand_bid() : rand_ask());
        return {SYN_MODIFY, s, 0, id, p, 1 + rng() % 20};
    }
};

std::vector<SynEvent> gen_steady(size_t n, uint64_t seed) {
    Generator g(seed);
    std::vector<SynEvent> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double r = g.udist(g.rng);
        if      (r < 0.30 || g.live.size() < 100) out.push_back(g.make_passive());
        else if (r < 0.50)                         out.push_back(g.make_aggressive());
        else if (r < 0.85)                         out.push_back(g.make_cancel());
        else                                       out.push_back(g.make_modify());
    }
    return out;
}

std::vector<SynEvent> gen_sweep(size_t n, uint64_t seed) {
    Generator g(seed);
    std::vector<SynEvent> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        bool burst = (i % 5000) > 4900;
        double r = g.udist(g.rng);
        if (burst) {
            out.push_back(g.make_aggressive());
        } else if (r < 0.35 || g.live.size() < 100) {
            out.push_back(g.make_passive());
        } else if (r < 0.50) {
            out.push_back(g.make_aggressive());
        } else if (r < 0.85) {
            out.push_back(g.make_cancel());
        } else {
            out.push_back(g.make_modify());
        }
    }
    return out;
}

std::vector<SynEvent> gen_stress(size_t n, uint64_t seed) {
    Generator g(seed);
    std::vector<SynEvent> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double r = g.udist(g.rng);
        if      (r < 0.20 || g.live.size() < 50) out.push_back(g.make_passive());
        else if (r < 0.25)                        out.push_back(g.make_aggressive());
        else if (r < 0.90)                        out.push_back(g.make_cancel());
        else                                      out.push_back(g.make_modify());
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stats + reporting (shared by both modes)
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

void report(const char* label, Stats& s, const TscCalibration& cal) {
    s.finalize();
    if (s.count() == 0) { std::printf("  %-20s  (no samples)\n", label); return; }
    auto ns = [&](uint64_t c) -> unsigned long long {
        return static_cast<unsigned long long>(cal.cycles_to_ns(c));
    };
    std::printf("  %-20s  n=%-8zu  p50=%5llu  p99=%5llu  p99.9=%6llu  max=%7llu ns\n",
        label, s.count(), ns(s.pct(50)), ns(s.pct(99)), ns(s.pct(99.9)), ns(s.max_val()));
}

// ---------------------------------------------------------------------------
// Synthetic replay
// ---------------------------------------------------------------------------
void replay_synthetic(const std::vector<SynEvent>& events, const char* scenario,
                      const TscCalibration& cal) {
    constexpr size_t kPool = 200'000;
    constexpr size_t kWarmupFrac = 10;

    OrderBook book(kSynTicks, kPool);
    size_t warmup = events.size() / kWarmupFrac;

    Stats s_add(events.size()), s_cancel(events.size()),
          s_modify(events.size()), s_all(events.size());

    auto wall_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        bool measure = (i >= warmup);
        unsigned c0 = 0, c1 = 0;
        uint64_t t0 = 0, t1 = 0;

        switch (e.type) {
        case SYN_ADD: {
            auto side = (e.side == 0) ? Side::Buy : Side::Sell;
            t0 = read_tsc(c0);
            book.submit(e.order_id, side, OrderType::Limit, e.price, e.quantity);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_add.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
        case SYN_CANCEL:
            t0 = read_tsc(c0);
            book.cancel(e.order_id);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_cancel.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        case SYN_MODIFY:
            t0 = read_tsc(c0);
            book.modify(e.order_id, e.price, e.quantity);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_modify.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - wall_start).count();
    double throughput = static_cast<double>(events.size()) /
        (static_cast<double>(wall_ms) / 1000.0);

    std::printf("\n  \033[1mReplay: %s\033[0m\n", scenario);
    std::printf("  ──────────────────────────────────────────────────────────────────\n");
    std::printf("  Events:     %zu  (warmup: %zu discarded)\n", events.size(), warmup);
    std::printf("  Wall time:  %lld ms\n", static_cast<long long>(wall_ms));
    std::printf("  Throughput: %.0f events/sec\n", throughput);
    std::printf("  Book state: %zu open orders, bid=%lld ask=%lld\n\n",
        book.open_orders(),
        static_cast<long long>(book.best_bid()),
        static_cast<long long>(book.best_ask()));

    std::printf("  Event Type             Samples    p50    p99   p99.9      max\n");
    std::printf("  ──────────────────────────────────────────────────────────────\n");
    report("ADD", s_add, cal);
    report("CANCEL", s_cancel, cal);
    report("MODIFY", s_modify, cal);
    std::printf("  ──────────────────────────────────────────────────────────────\n");
    report("ALL", s_all, cal);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// File replay (LOBSTER / ITCH)
// ---------------------------------------------------------------------------
void replay_file(const std::vector<MarketEvent>& events, const char* source,
                 const TscCalibration& cal, size_t tick_range, size_t pool_size) {
    constexpr size_t kWarmupFrac = 10;

    OrderBook book(tick_range, pool_size);
    size_t warmup = events.size() / kWarmupFrac;

    Stats s_add(events.size()), s_cancel(events.size()),
          s_modify(events.size()), s_execute(events.size()),
          s_all(events.size());

    size_t skipped = 0;
    size_t trades = 0;
    book.set_trade_sink([&trades](const Trade&) { ++trades; });

    auto wall_start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        bool measure = (i >= warmup);
        unsigned c0 = 0, c1 = 0;
        uint64_t t0 = 0, t1 = 0;

        if (e.price < 0 || e.price >= static_cast<Price>(tick_range)) {
            ++skipped;
            continue;
        }

        switch (e.action) {
        case EventAction::Add: {
            t0 = read_tsc(c0);
            book.submit(e.order_id, e.side, OrderType::Limit, e.price, e.quantity);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_add.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
        case EventAction::Cancel: {
            t0 = read_tsc(c0);
            book.cancel(e.order_id);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_cancel.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
        case EventAction::Modify: {
            t0 = read_tsc(c0);
            book.modify(e.order_id, e.price, e.quantity);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_modify.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
        case EventAction::Execute: {
            // Execute = partial fill of a resting order. Our engine doesn't
            // support external fills; we model this as a cancel (the resting
            // order leaves the book).
            t0 = read_tsc(c0);
            book.cancel(e.order_id);
            t1 = read_tsc(c1);
            if (measure && c0 == c1) { s_execute.record(t1 - t0); s_all.record(t1 - t0); }
            break;
        }
        }
    }

    auto wall_end = std::chrono::steady_clock::now();
    auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        wall_end - wall_start).count();
    double throughput = wall_ms > 0
        ? static_cast<double>(events.size()) / (static_cast<double>(wall_ms) / 1000.0)
        : 0;

    // Price range analysis
    Price min_p = std::numeric_limits<Price>::max();
    Price max_p = std::numeric_limits<Price>::min();
    for (const auto& e : events) {
        if (e.price > 0 && e.price < static_cast<Price>(tick_range)) {
            if (e.price < min_p) min_p = e.price;
            if (e.price > max_p) max_p = e.price;
        }
    }

    std::printf("\n  \033[1mReplay: %s\033[0m\n", source);
    std::printf("  ──────────────────────────────────────────────────────────────────\n");
    std::printf("  Events:      %zu  (warmup: %zu discarded)\n", events.size(), warmup);
    std::printf("  Skipped:     %zu (out-of-range prices)\n", skipped);
    std::printf("  Wall time:   %lld ms\n", static_cast<long long>(wall_ms));
    std::printf("  Throughput:  %.0f events/sec\n", throughput);
    std::printf("  Price range: %lld – %lld ticks (tick_range=%zu)\n",
        (long long)min_p, (long long)max_p, tick_range);
    std::printf("  Book state:  %zu open orders, bid=%lld ask=%lld\n",
        book.open_orders(),
        static_cast<long long>(book.best_bid()),
        static_cast<long long>(book.best_ask()));
    std::printf("  Trades:      %zu\n\n", trades);

    // Event breakdown
    size_t n_add = 0, n_cancel = 0, n_modify = 0, n_exec = 0;
    for (const auto& e : events) {
        switch (e.action) {
        case EventAction::Add:     ++n_add; break;
        case EventAction::Cancel:  ++n_cancel; break;
        case EventAction::Modify:  ++n_modify; break;
        case EventAction::Execute: ++n_exec; break;
        }
    }
    std::printf("  Event breakdown: %zu add, %zu cancel, %zu modify, %zu execute\n\n",
        n_add, n_cancel, n_modify, n_exec);

    std::printf("  Event Type             Samples    p50    p99   p99.9      max\n");
    std::printf("  ──────────────────────────────────────────────────────────────\n");
    report("ADD", s_add, cal);
    report("CANCEL", s_cancel, cal);
    report("MODIFY", s_modify, cal);
    report("EXECUTE", s_execute, cal);
    std::printf("  ──────────────────────────────────────────────────────────────\n");
    report("ALL", s_all, cal);
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
void print_usage() {
    std::printf("\n  \033[1mMarket Data Replay\033[0m\n\n");
    std::printf("  Usage:\n");
    std::printf("    lob_replay                                synthetic (steady, 500K)\n");
    std::printf("    lob_replay steady|sweep|stress [count]    synthetic scenario\n");
    std::printf("    lob_replay all                            run all synthetic scenarios\n");
    std::printf("    lob_replay file <path> [options]          replay a data file\n\n");
    std::printf("  File options:\n");
    std::printf("    --symbol SYM    filter to stock symbol (ITCH only, e.g. AAPL)\n");
    std::printf("    --ticks N       tick range for the book (default: 100000)\n");
    std::printf("    --pool N        pool capacity (default: 500000)\n");
    std::printf("    --tick-div N    price divisor for ticks (default: 100)\n");
    std::printf("    --no-prefix     ITCH file has no 2-byte length prefixes\n\n");
    std::printf("  Supported formats:\n");
    std::printf("    LOBSTER  (.csv)   Academic L3 message data from lobsterdata.com\n");
    std::printf("    ITCH 5.0 (.bin)   NASDAQ TotalView binary feed\n\n");
    std::printf("  Format is auto-detected from extension and content.\n\n");
}

} // namespace

int main(int argc, char* argv[]) {
    enable_ansi_replay();

    const auto cal = TscCalibration::measure();
    std::printf("\n  TSC freq: %.2f ticks/ns\n", cal.ticks_per_ns);

    if (argc > 1 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        print_usage();
        return 0;
    }

    // -- File replay mode --
    if (argc > 2 && std::strcmp(argv[1], "file") == 0) {
        const char* path = argv[2];
        std::string symbol;
        size_t tick_range = 100'000;
        size_t pool_size = 500'000;
        int64_t tick_div = 100;
        bool no_prefix = false;

        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
                symbol = argv[++i];
            } else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
                tick_range = static_cast<size_t>(std::atoll(argv[++i]));
            } else if (std::strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
                pool_size = static_cast<size_t>(std::atoll(argv[++i]));
            } else if (std::strcmp(argv[i], "--tick-div") == 0 && i + 1 < argc) {
                tick_div = std::atoll(argv[++i]);
            } else if (std::strcmp(argv[i], "--no-prefix") == 0) {
                no_prefix = true;
            }
        }

        DataFormat fmt = detect_format(path);

        std::vector<MarketEvent> events;
        const char* fmt_name = "unknown";

        if (fmt == DataFormat::Lobster) {
            fmt_name = "LOBSTER";
            std::printf("  Detected format: LOBSTER\n");
            std::printf("  Parsing %s...", path);
            std::fflush(stdout);
            LobsterParser parser;
            parser.tick_divisor = tick_div;
            events = parser.parse_file(path);
        } else {
            fmt_name = "ITCH 5.0";
            std::printf("  Detected format: ITCH 5.0\n");
            if (!symbol.empty())
                std::printf("  Symbol filter: %s\n", symbol.c_str());
            std::printf("  Parsing %s...", path);
            std::fflush(stdout);
            ItchParser parser;
            parser.symbol_filter = symbol;
            parser.tick_divisor = tick_div;
            parser.length_prefixed = !no_prefix;
            events = parser.parse_file(path);
        }

        std::printf(" %zu events\n", events.size());

        if (events.empty()) {
            std::printf("  No events parsed. Check the file path and format.\n\n");
            return 1;
        }

        char label[256];
        std::snprintf(label, sizeof(label), "%s — %s%s%s",
            fmt_name, path,
            symbol.empty() ? "" : " [",
            symbol.empty() ? "" : (symbol + "]").c_str());

        replay_file(events, label, cal, tick_range, pool_size);
        return 0;
    }

    // -- Synthetic replay mode (original behavior) --
    const char* scenario = argc > 1 ? argv[1] : "steady";
    size_t count = 500'000;
    if (argc > 2 && std::strcmp(argv[1], "file") != 0)
        count = static_cast<size_t>(std::atoll(argv[2]));

    constexpr uint64_t seed = 12345;

    if (std::strcmp(scenario, "all") == 0) {
        std::printf("  Generating steady (%zu events)...", count);
        std::fflush(stdout);
        auto ev1 = gen_steady(count, seed);
        std::printf(" done\n");
        replay_synthetic(ev1, "steady \xe2\x80\x94 typical mixed workload", cal);

        std::printf("  Generating sweep (%zu events)...", count);
        std::fflush(stdout);
        auto ev2 = gen_sweep(count, seed);
        std::printf(" done\n");
        replay_synthetic(ev2, "sweep \xe2\x80\x94 periodic aggressive bursts", cal);

        std::printf("  Generating stress (%zu events)...", count);
        std::fflush(stdout);
        auto ev3 = gen_stress(count, seed);
        std::printf(" done\n");
        replay_synthetic(ev3, "stress \xe2\x80\x94 high cancel rate", cal);
    } else if (std::strcmp(scenario, "steady") == 0 ||
               std::strcmp(scenario, "sweep") == 0 ||
               std::strcmp(scenario, "stress") == 0) {
        std::printf("  Generating %s (%zu events)...", scenario, count);
        std::fflush(stdout);

        std::vector<SynEvent> events;
        if (std::strcmp(scenario, "steady") == 0)      events = gen_steady(count, seed);
        else if (std::strcmp(scenario, "sweep") == 0)   events = gen_sweep(count, seed);
        else                                            events = gen_stress(count, seed);

        std::printf(" done\n");
        replay_synthetic(events, scenario, cal);
    } else {
        std::printf("  Unknown command '%s'\n", scenario);
        print_usage();
        return 1;
    }
    return 0;
}
