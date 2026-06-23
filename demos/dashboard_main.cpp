// dashboard_main.cpp
// Live matching engine dashboard with full SPSC ring buffer architecture.
//
// Architecture (the whole point of Stage 4):
//   [Producer thread] → inbound SPSC → [Matching core] → outbound SPSC → [Publisher thread]
//                                                                              ↓
//                                                                     dashboard state
//
// The matching core runs on the main thread, never blocks, never allocates.
// Ingest and publish are isolated on their own threads, communicating only
// through lock-free SPSC queues. This is the LMAX Disruptor pattern.
//
// Controls:
//   1-3     Switch scenario (steady / sweep / stress)
//   +/-     Speed up / slow down event rate
//   space   Pause / resume
//   q       Quit

#include "lob/order_book.hpp"
#include "lob/spsc_queue.hpp"
#include "lob/publisher.hpp"
#include "lob/rdtsc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

using namespace lob;

namespace {

// -- ANSI helpers --
void move_to(int r, int c) { std::printf("\033[%d;%dH", r, c); }
void clear()                { std::printf("\033[2J\033[H"); }
void hide_cursor()          { std::printf("\033[?25l"); }
void show_cursor()          { std::printf("\033[?25h"); }
void bold()                 { std::printf("\033[1m"); }
void dim()                  { std::printf("\033[2m"); }
void green()                { std::printf("\033[32m"); }
void red()                  { std::printf("\033[31m"); }
void yellow()               { std::printf("\033[33m"); }
void cyan()                 { std::printf("\033[36m"); }
void reset()                { std::printf("\033[0m"); }

#ifdef _WIN32
void enable_ansi_dash() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD m = 0; GetConsoleMode(h, &m);
    SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}
int get_key() { return _kbhit() ? _getch() : 0; }
#else
struct RawMode {
    termios old_;
    RawMode() { tcgetattr(STDIN_FILENO, &old_); termios t = old_; t.c_lflag &= ~(ICANON|ECHO); tcsetattr(STDIN_FILENO, TCSANOW, &t); }
    ~RawMode() { tcsetattr(STDIN_FILENO, TCSANOW, &old_); }
};
void enable_ansi_dash() {}
int get_key() {
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    timeval tv{0, 0};
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
        char c; if (read(STDIN_FILENO, &c, 1) == 1) return c;
    }
    return 0;
}
#endif

// -- Inbound event (same as replay) --
enum EventType : uint8_t { ADD = 1, CANCEL = 2, MODIFY = 3 };
struct Event {
    uint8_t  type;
    uint8_t  side;
    uint32_t order_id;
    int64_t  price;
    uint64_t quantity;
};

// -- Queues --
using InboundQueue = SPSCQueue<Event, 65536>;

// -- Config --
constexpr size_t kTicks = 2'000;
constexpr size_t kPool  = 100'000;
constexpr Price  kMid   = 1'000;
constexpr int    kRows  = 16;

// -- Trade record for the tape (written by publisher thread, read by render) --
struct TradeRec {
    Price price;
    Quantity qty;
    uint8_t aggressor_side;
};

// Thread-safe trade tape: publisher writes, render reads.
struct TradeTape {
    std::mutex mtx;
    std::vector<TradeRec> trades;
    size_t total_trades = 0;
    uint64_t total_volume = 0;

    void push(const TradeRec& t) {
        std::lock_guard<std::mutex> lk(mtx);
        trades.push_back(t);
        ++total_trades;
        total_volume += t.qty;
    }

    struct Snapshot {
        std::vector<TradeRec> recent;
        size_t total_trades;
        uint64_t total_volume;
    };

    Snapshot snapshot(size_t max_recent = 16) {
        std::lock_guard<std::mutex> lk(mtx);
        Snapshot s;
        s.total_trades = total_trades;
        s.total_volume = total_volume;
        size_t start = trades.size() > max_recent ? trades.size() - max_recent : 0;
        s.recent.assign(trades.begin() + static_cast<ptrdiff_t>(start), trades.end());
        return s;
    }
};

// -- Rolling latency tracker --
struct LatencyTracker {
    uint64_t samples[8192];
    size_t pos = 0, count = 0;

    void record(uint64_t ns) {
        samples[pos++ & 8191] = ns;
        if (count < 8192) ++count;
    }

    uint64_t percentile(double p) const {
        if (count == 0) return 0;
        std::vector<uint64_t> sorted(samples, samples + count);
        std::sort(sorted.begin(), sorted.end());
        return sorted[static_cast<size_t>(p / 100.0 * static_cast<double>(count - 1))];
    }

    uint64_t max_val() const {
        if (count == 0) return 0;
        return *std::max_element(samples, samples + count);
    }
};

// -- Producer thread (ingest edge) --
struct Producer {
    InboundQueue& queue;
    std::atomic<bool>& running;
    std::atomic<int>& scenario;
    std::atomic<int>& speed_level;
    std::atomic<bool>& paused;

    void operator()() {
        std::mt19937_64 rng(42);
        std::uniform_real_distribution<double> ud(0.0, 1.0);
        std::vector<uint32_t> live;
        live.reserve(50'000);
        uint32_t next_id = 1;
        Price mid = kMid;

        auto rand_bid = [&]() -> Price { return std::clamp(mid - 1 - static_cast<Price>(rng() % 40), Price{1}, static_cast<Price>(kTicks - 2)); };
        auto rand_ask = [&]() -> Price { return std::clamp(mid + 1 + static_cast<Price>(rng() % 40), Price{1}, static_cast<Price>(kTicks - 2)); };

        constexpr int delays_us[] = {5000, 1000, 200, 50, 10, 0};
        uint64_t seq = 0;

        while (running.load(std::memory_order_relaxed)) {
            if (paused.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            int sc = scenario.load(std::memory_order_relaxed);
            double r = ud(rng);
            Event ev{};

            bool burst = (sc == 2) && (seq % 3000) > 2850;

            if (burst || (sc == 2 && r < 0.15)) {
                uint8_t s = (rng() & 1) ? 0 : 1;
                ev = {ADD, s, next_id++,
                      s == 0 ? std::clamp(mid + 60, Price{1}, static_cast<Price>(kTicks - 2))
                             : std::clamp(mid - 60, Price{1}, static_cast<Price>(kTicks - 2)),
                      1 + rng() % 5};
            } else if (r < 0.30 || live.size() < 80) {
                uint8_t s = (rng() & 1) ? 0 : 1;
                ev = {ADD, s, next_id++, s == 0 ? rand_bid() : rand_ask(), 1 + rng() % 20};
                live.push_back(ev.order_id);
            } else if (r < 0.50) {
                uint8_t s = (rng() & 1) ? 0 : 1;
                ev = {ADD, s, next_id++,
                      s == 0 ? std::clamp(mid + static_cast<Price>(rng() % 20), Price{1}, static_cast<Price>(kTicks - 2))
                             : std::clamp(mid - static_cast<Price>(rng() % 20), Price{1}, static_cast<Price>(kTicks - 2)),
                      1 + rng() % 5};
            } else if (r < (sc == 3 ? 0.95 : 0.80) && !live.empty()) {
                size_t idx = rng() % live.size();
                ev = {CANCEL, 0, live[idx], 0, 0};
                live[idx] = live.back(); live.pop_back();
            } else if (!live.empty()) {
                size_t idx = rng() % live.size();
                uint8_t s = (rng() & 1) ? 0 : 1;
                ev = {MODIFY, s, live[idx], s == 0 ? rand_bid() : rand_ask(), 1 + rng() % 20};
            } else {
                uint8_t s = (rng() & 1) ? 0 : 1;
                ev = {ADD, s, next_id++, s == 0 ? rand_bid() : rand_ask(), 1 + rng() % 20};
                live.push_back(ev.order_id);
            }

            while (!queue.try_push(ev) && running.load(std::memory_order_relaxed))
                std::this_thread::yield();

            int sl = speed_level.load(std::memory_order_relaxed);
            if (delays_us[sl] > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(delays_us[sl]));
            ++seq;
        }
    }
};

// -- Render --
void render(const OrderBook& book, const TradeTape::Snapshot& snap,
            const LatencyTracker& lat, const TscCalibration& cal,
            size_t ops, double throughput, size_t in_q_depth, size_t out_q_depth,
            int scenario, int speed_level, bool paused,
            uint64_t pub_msgs) {
    move_to(1, 1);

    const char* sc_names[] = {"steady", "sweep", "stress"};
    const char* speed_names[] = {"0.2x", "1x", "5x", "20x", "100x", "MAX"};

    // Header
    bold(); cyan();
    std::printf("  LOB MATCHING ENGINE DASHBOARD");
    reset(); dim();
    std::printf("  |  scenario: ");
    reset(); yellow(); std::printf("%-7s", sc_names[scenario - 1]); reset();
    dim(); std::printf("  |  speed: ");
    reset(); yellow(); std::printf("%-4s", speed_names[speed_level]); reset();
    if (paused) { red(); bold(); std::printf("  PAUSED"); reset(); }
    else { dim(); std::printf("        "); reset(); }
    std::printf("                  \n");

    dim();
    std::printf("  [1-3] scenario  [+/-] speed  [space] pause  [q] quit");
    reset();
    std::printf("                      \n");

    // Architecture diagram
    dim();
    std::printf("  ingest -> SPSC[in] -> matching core -> SPSC[out] -> publisher");
    reset();
    std::printf("          \n\n");

    // Price ladder
    Price bb = book.best_bid(), ba = book.best_ask();
    Price center = kMid;
    if (bb >= 0 && ba >= 0) center = (bb + ba) / 2;
    else if (bb >= 0) center = bb;
    else if (ba >= 0) center = ba;

    Price top = center + kRows / 2, bot = center - kRows / 2;
    if (bot < 0) bot = 0;
    if (top >= static_cast<Price>(kTicks)) top = static_cast<Price>(kTicks - 1);

    Quantity max_q = 1;
    for (Price p = bot; p <= top; ++p) {
        Quantity bq = book.bid_size_at(p), aq = book.ask_size_at(p);
        if (bq > max_q) max_q = bq;
        if (aq > max_q) max_q = aq;
    }

    bold();
    std::printf("  %-38s", "ORDER BOOK DEPTH");
    std::printf("  TRADE TAPE (via publish SPSC)        \n");
    reset();

    int tape_idx = 0;

    for (Price p = top; p >= bot; --p) {
        Quantity bq = book.bid_size_at(p), aq = book.ask_size_at(p);
        int b_len = max_q > 0 ? static_cast<int>(14.0 * static_cast<double>(bq) / static_cast<double>(max_q)) : 0;
        int a_len = max_q > 0 ? static_cast<int>(14.0 * static_cast<double>(aq) / static_cast<double>(max_q)) : 0;
        if (bq > 0 && b_len < 1) b_len = 1;
        if (aq > 0 && a_len < 1) a_len = 1;

        std::string b_bar(static_cast<size_t>(14 - b_len), ' ');
        if (bq > 0) { green(); std::printf("  %s%s", b_bar.c_str(), std::string(static_cast<size_t>(b_len), '#').c_str()); reset(); }
        else std::printf("  %14s", "");

        if (bq > 0) { green(); std::printf(" %4llu", (unsigned long long)bq); reset(); }
        else std::printf("     ");

        bool is_bb = (p == bb), is_ba = (p == ba);
        if (is_bb || is_ba) { bold(); std::printf(is_bb ? "\033[32m" : "\033[31m"); }
        else { dim(); }
        std::printf(" %4lld ", (long long)p);
        reset();

        if (aq > 0) { red(); std::printf("%-4llu %s", (unsigned long long)aq, std::string(static_cast<size_t>(a_len), '#').c_str()); reset(); }
        else std::printf("     ");

        // Pad to trade tape column
        std::printf("%*s", static_cast<int>(14 - a_len + (aq > 0 ? 0 : 5)), "");
        dim(); std::printf("|"); reset();

        // Trade tape entry (populated by publisher thread via outbound SPSC)
        if (tape_idx < static_cast<int>(snap.recent.size())) {
            const auto& t = snap.recent[static_cast<size_t>(tape_idx)];
            if (t.aggressor_side == 0) green(); else red();
            std::printf(" %s %3llu @ %4lld",
                t.aggressor_side == 0 ? "BUY " : "SELL",
                (unsigned long long)t.qty, (long long)t.price);
            reset();
        }
        std::printf("                 \n");
        ++tape_idx;
    }

    // Separator
    dim();
    std::printf("  ──────────────────────────────────────────────────────────────\n");
    reset();

    // Stats row 1: latency
    uint64_t lp50 = cal.cycles_to_ns(lat.percentile(50));
    uint64_t lp99 = cal.cycles_to_ns(lat.percentile(99));
    uint64_t lp999 = cal.cycles_to_ns(lat.percentile(99.9));
    uint64_t lmax = cal.cycles_to_ns(lat.max_val());

    int bar50  = static_cast<int>(std::min(lp50  / 50, uint64_t(20)));
    int bar99  = static_cast<int>(std::min(lp99  / 50, uint64_t(20)));
    int bar999 = static_cast<int>(std::min(lp999 / 50, uint64_t(20)));

    bold(); std::printf("  LATENCY (ns)"); reset();
    std::printf("                        ");
    bold(); std::printf("STATISTICS"); reset();
    std::printf("                     \n");

    cyan();
    std::printf("   p50   "); reset();
    green(); std::printf("%-20s", std::string(static_cast<size_t>(bar50), '#').c_str()); reset();
    std::printf(" %4llu", (unsigned long long)lp50);
    std::printf("    Throughput: ");
    yellow(); std::printf("%8.0f", throughput); reset();
    std::printf(" ops/sec    \n");

    cyan();
    std::printf("   p99   "); reset();
    yellow(); std::printf("%-20s", std::string(static_cast<size_t>(bar99), '#').c_str()); reset();
    std::printf(" %4llu", (unsigned long long)lp99);
    std::printf("    Open orders: ");
    std::printf("%6zu", book.open_orders());
    std::printf("               \n");

    cyan();
    std::printf("   p99.9 "); reset();
    red(); std::printf("%-20s", std::string(static_cast<size_t>(bar999), '#').c_str()); reset();
    std::printf(" %4llu", (unsigned long long)lp999);
    std::printf("    Spread: ");
    if (bb >= 0 && ba >= 0) { yellow(); std::printf("%lld", (long long)(ba - bb)); reset(); std::printf(" ticks"); }
    else std::printf("--    ");
    std::printf("                  \n");

    cyan();
    std::printf("   max   "); reset();
    dim(); std::printf("%-20s", ""); reset();
    std::printf(" %4llu", (unsigned long long)lmax);
    std::printf("    Trades: %zu  Vol: %llu",
        snap.total_trades, (unsigned long long)snap.total_volume);
    std::printf("           \n");

    // Queue depths — the key observability for the SPSC architecture
    dim();
    std::printf("                                        In queue:  %5zu / %zu",
        in_q_depth, InboundQueue::capacity());
    reset();
    std::printf("      \n");
    dim();
    std::printf("                                        Out queue: %5zu / %zu   pub: %llu msgs",
        out_q_depth, OutQueue::capacity(), (unsigned long long)pub_msgs);
    reset();
    std::printf("    \n");

    std::fflush(stdout);
}

} // namespace

int main() {
    enable_ansi_dash();
#ifndef _WIN32
    RawMode raw;
#endif
    clear();
    hide_cursor();

    const auto cal = TscCalibration::measure();

    // -- Inbound SPSC: producer → matching core --
    auto in_queue_ptr = std::make_unique<InboundQueue>();
    InboundQueue& in_queue = *in_queue_ptr;

    // -- Outbound SPSC: matching core → publisher --
    auto out_queue_ptr = std::make_unique<OutQueue>();
    OutQueue& out_queue = *out_queue_ptr;

    std::atomic<bool> running{true};
    std::atomic<int> scenario{1};
    std::atomic<int> speed_level{2};
    std::atomic<bool> paused{false};

    // -- Producer thread (ingest edge) --
    Producer prod{in_queue, running, scenario, speed_level, paused};
    std::thread producer_thread([&prod]() { prod(); });

    // -- Publisher thread (publish edge) --
    TradeTape tape;
    Publisher publisher(out_queue,
        [&tape](const OutMsg& msg) {
            tape.push({msg.price, msg.quantity, msg.side});
        });
    publisher.start();

    // -- Matching core (this thread — single-threaded, no locks) --
    OrderBook book(kTicks, kPool);
    LatencyTracker lat{};
    size_t ops = 0;

    uint8_t last_aggressor = 0;
    book.set_trade_sink(make_spsc_trade_sink(out_queue, last_aggressor));

    auto last_render = std::chrono::steady_clock::now();
    auto last_throughput_check = last_render;
    size_t ops_at_last_check = 0;
    double throughput = 0;

    while (running.load()) {
        Event ev;
        int batch = 0;
        while (in_queue.try_pop(ev) && batch < 500) {
            unsigned c0, c1;
            uint64_t t0, t1;

            switch (ev.type) {
            case ADD: {
                auto side = (ev.side == 0) ? Side::Buy : Side::Sell;
                last_aggressor = ev.side;
                t0 = read_tsc(c0);
                book.submit(ev.order_id, side, OrderType::Limit, ev.price, ev.quantity);
                t1 = read_tsc(c1);
                if (c0 == c1) lat.record(t1 - t0);
                break;
            }
            case CANCEL:
                t0 = read_tsc(c0);
                book.cancel(ev.order_id);
                t1 = read_tsc(c1);
                if (c0 == c1) lat.record(t1 - t0);
                break;
            case MODIFY:
                t0 = read_tsc(c0);
                book.modify(ev.order_id, ev.price, ev.quantity);
                t1 = read_tsc(c1);
                if (c0 == c1) lat.record(t1 - t0);
                break;
            }
            ++ops;
            ++batch;
        }

        // Handle keyboard input
        int key = get_key();
        if (key == 'q' || key == 'Q') { running = false; break; }
        if (key == '1') scenario = 1;
        if (key == '2') scenario = 2;
        if (key == '3') scenario = 3;
        if (key == '+' || key == '=') { int s = speed_level.load(); if (s < 5) speed_level = s + 1; }
        if (key == '-' || key == '_') { int s = speed_level.load(); if (s > 0) speed_level = s - 1; }
        if (key == ' ') paused = !paused.load();

        auto now = std::chrono::steady_clock::now();

        // Throughput calc every 500ms
        auto tp_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_throughput_check).count();
        if (tp_elapsed >= 500) {
            throughput = static_cast<double>(ops - ops_at_last_check) /
                         (static_cast<double>(tp_elapsed) / 1000.0);
            ops_at_last_check = ops;
            last_throughput_check = now;
        }

        // Render at ~20 FPS
        auto render_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render).count();
        if (render_elapsed >= 50) {
            auto snap = tape.snapshot(kRows);
            render(book, snap, lat, cal, ops, throughput,
                   in_queue.size_approx(), out_queue.size_approx(),
                   scenario.load(), speed_level.load(), paused.load(),
                   publisher.messages_processed());
            last_render = now;
        }

        if (batch == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    running = false;
    producer_thread.join();
    publisher.stop();
    show_cursor();

    auto final_snap = tape.snapshot();
    std::printf("\n\n  Done. %zu events processed, %zu trades via publish SPSC.\n\n",
        ops, final_snap.total_trades);
    return 0;
}
