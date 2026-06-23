// demo_main.cpp
// Live terminal visualization of the order book processing a synthetic order
// flow. Shows a price ladder with bid/ask depth, a scrolling trade tape, and
// live statistics. No external dependencies — uses ANSI escape codes.
//
// Run:  ./build/Release/lob_demo
// Stop: Ctrl+C (or it exits after the configured number of operations).

#include "lob/order_book.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace lob;

namespace {

// ANSI escape helpers
void clear_screen() { std::printf("\033[2J\033[H"); }
void move_to(int row, int col) { std::printf("\033[%d;%dH", row, col); }
void set_color(const char* code) { std::printf("\033[%sm", code); }
void reset_color() { std::printf("\033[0m"); }

constexpr const char* kGreen  = "32";
constexpr const char* kRed    = "31";
constexpr const char* kYellow = "33";
constexpr const char* kCyan   = "36";
constexpr const char* kBold   = "1";
constexpr const char* kDim    = "2";

// Configuration
constexpr size_t kTicks       = 1000;
constexpr size_t kPool        = 50'000;
constexpr Price  kMid         = 500;
constexpr size_t kLadderRows  = 20;     // price levels to show (10 bid + 10 ask)
constexpr size_t kMaxTrades   = 12;     // trades to show in the tape
constexpr size_t kOpsTotal    = 10'000;
constexpr int    kDelayMs     = 40;     // ms between visual updates

struct TradeRecord {
    OrderId resting_id;
    OrderId incoming_id;
    Price price;
    Quantity qty;
    Side aggressor_side;
};

void enable_ansi() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// Bar of a given character, capped at max_width
std::string bar(Quantity qty, Quantity max_qty, int max_width, char ch) {
    if (max_qty == 0 || qty == 0) return "";
    int len = static_cast<int>(
        static_cast<double>(qty) / static_cast<double>(max_qty) * max_width);
    if (len < 1 && qty > 0) len = 1;
    if (len > max_width) len = max_width;
    return std::string(static_cast<size_t>(len), ch);
}

void draw(const OrderBook& book,
          const std::vector<TradeRecord>& trades,
          size_t op_num, size_t total_trades, size_t total_fills) {

    Price bb = book.best_bid();
    Price ba = book.best_ask();

    // Find the price range to display centered on the mid
    Price center = kMid;
    if (bb >= 0 && ba >= 0) center = (bb + ba) / 2;
    else if (bb >= 0) center = bb;
    else if (ba >= 0) center = ba;

    Price top = center + static_cast<Price>(kLadderRows / 2);
    Price bot = center - static_cast<Price>(kLadderRows / 2);
    if (bot < 0) bot = 0;
    if (top >= static_cast<Price>(kTicks)) top = static_cast<Price>(kTicks - 1);

    // Find max qty for scaling bars
    Quantity max_qty = 1;
    for (Price p = bot; p <= top; ++p) {
        Quantity bq = book.bid_size_at(p);
        Quantity aq = book.ask_size_at(p);
        if (bq > max_qty) max_qty = bq;
        if (aq > max_qty) max_qty = aq;
    }

    move_to(1, 1);

    // Header
    set_color(kBold);
    std::printf("  LIMIT ORDER BOOK");
    reset_color();
    set_color(kDim);
    std::printf("  |  op %zu/%zu", op_num, kOpsTotal);
    std::printf("  |  open: %zu", book.open_orders());
    std::printf("  |  trades: %zu", total_trades);
    std::printf("  |  fills: %zu", total_fills);
    reset_color();
    std::printf("                    \n\n");

    // Spread info
    if (bb >= 0 && ba >= 0) {
        std::printf("  Spread: ");
        set_color(kYellow);
        std::printf("%lld", static_cast<long long>(ba - bb));
        reset_color();
        std::printf(" ticks  (bid=%lld  ask=%lld)",
            static_cast<long long>(bb), static_cast<long long>(ba));
    } else {
        std::printf("  Spread: --");
    }
    std::printf("                              \n\n");

    // Column headers
    std::printf("  %20s  %6s  %5s  %-6s  %-20s\n",
        "BID QTY", "BID", "PRICE", "ASK", "ASK QTY");
    set_color(kDim);
    std::printf("  %s\n", std::string(65, '-').c_str());
    reset_color();

    constexpr int kBarWidth = 18;

    // Price ladder (top = highest price)
    for (Price p = top; p >= bot; --p) {
        Quantity bq = book.bid_size_at(p);
        Quantity aq = book.ask_size_at(p);

        bool is_bb = (p == bb);
        bool is_ba = (p == ba);

        // Bid bar (right-aligned)
        std::string bid_bar = bar(bq, max_qty, kBarWidth, '#');
        std::string bid_pad(static_cast<size_t>(kBarWidth) - bid_bar.size(), ' ');

        std::printf("  ");

        // Bid side
        if (bq > 0) {
            std::printf("%s", bid_pad.c_str());
            set_color(kGreen);
            std::printf("%s", bid_bar.c_str());
            reset_color();
            std::printf("  ");
            set_color(kGreen);
            std::printf("%6llu", static_cast<unsigned long long>(bq));
            reset_color();
        } else {
            std::printf("%*s  %6s", kBarWidth, "", "");
        }

        // Price column
        if (is_bb || is_ba) {
            std::printf("  ");
            set_color(kBold);
            set_color(is_bb ? kGreen : kRed);
            std::printf("%5lld", static_cast<long long>(p));
            reset_color();
        } else {
            set_color(kDim);
            std::printf("  %5lld", static_cast<long long>(p));
            reset_color();
        }

        // Ask side
        if (aq > 0) {
            std::printf("  ");
            set_color(kRed);
            std::printf("%-6llu", static_cast<unsigned long long>(aq));
            reset_color();
            set_color(kRed);
            std::printf("%s", bar(aq, max_qty, kBarWidth, '#').c_str());
            reset_color();
        } else {
            std::printf("  %-6s", "");
        }

        std::printf("          \n");
    }

    set_color(kDim);
    std::printf("  %s\n", std::string(65, '-').c_str());
    reset_color();

    // Trade tape
    std::printf("\n  ");
    set_color(kBold);
    std::printf("TRADE TAPE");
    reset_color();
    std::printf("\n");

    size_t start = trades.size() > kMaxTrades ? trades.size() - kMaxTrades : 0;
    for (size_t i = start; i < trades.size(); ++i) {
        const auto& t = trades[i];
        std::printf("  ");
        set_color(t.aggressor_side == Side::Buy ? kGreen : kRed);
        std::printf("  %s", t.aggressor_side == Side::Buy ? "BUY " : "SELL");
        reset_color();
        std::printf("  %llu @ %lld",
            static_cast<unsigned long long>(t.qty),
            static_cast<long long>(t.price));
        std::printf("    (order %llu hit %llu)",
            static_cast<unsigned long long>(t.incoming_id),
            static_cast<unsigned long long>(t.resting_id));
        std::printf("              \n");
    }
    // Pad remaining lines to prevent stale output
    for (size_t i = trades.size() - start; i < kMaxTrades; ++i)
        std::printf("  %60s\n", "");

    std::printf("\n");
    set_color(kDim);
    std::printf("  Press Ctrl+C to stop\n");
    reset_color();

    std::fflush(stdout);
}

} // namespace

int main() {
    enable_ansi();
    clear_screen();

    OrderBook book(kTicks, kPool);
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> udist(0.0, 1.0);

    std::vector<TradeRecord> trades;
    std::vector<OrderId> live;
    live.reserve(kPool);
    trades.reserve(1000);

    Side last_aggressor = Side::Buy;
    book.set_trade_sink([&](const Trade& t) {
        trades.push_back({t.resting_id, t.incoming_id, t.price, t.quantity,
                          last_aggressor});
    });

    OrderId next_id = 1;
    size_t total_trades = 0;
    size_t total_fills = 0;

    Price mid = kMid;

    auto update_mid = [&]() {
        Price b = book.best_bid(), a = book.best_ask();
        if (b >= 0 && a >= 0) mid = (b + a) / 2;
    };

    for (size_t i = 0; i < kOpsTotal; ++i) {
        double roll = udist(rng);
        size_t trades_before = trades.size();

        if (roll < 0.40 || live.size() < 50) {
            // Passive limit order (rest behind the spread)
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy)
                ? mid - 1 - static_cast<Price>(rng() % 15)
                : mid + 1 + static_cast<Price>(rng() % 15);
            price = std::clamp(price, Price{1}, static_cast<Price>(kTicks - 2));
            Quantity qty = 1 + rng() % 30;
            OrderId id = next_id++;

            last_aggressor = side;
            auto r = book.submit(id, side, OrderType::Limit, price, qty);
            if (r.resting > 0) live.push_back(id);
        }
        else if (roll < 0.60) {
            // Aggressive limit order (crosses the spread)
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price price = (side == Side::Buy)
                ? mid + 5 + static_cast<Price>(rng() % 10)
                : mid - 5 - static_cast<Price>(rng() % 10);
            price = std::clamp(price, Price{1}, static_cast<Price>(kTicks - 2));
            Quantity qty = 1 + rng() % 10;
            OrderId id = next_id++;

            last_aggressor = side;
            auto r = book.submit(id, side, OrderType::Limit, price, qty);
            if (r.resting > 0) live.push_back(id);
        }
        else if (roll < 0.70) {
            // Market order
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Quantity qty = 1 + rng() % 5;
            OrderId id = next_id++;

            last_aggressor = side;
            book.submit(id, side, OrderType::Market, 0, qty);
        }
        else if (roll < 0.90 && !live.empty()) {
            // Cancel
            size_t idx = rng() % live.size();
            book.cancel(live[idx]);
            live[idx] = live.back();
            live.pop_back();
        }
        else if (!live.empty()) {
            // Modify
            size_t idx = rng() % live.size();
            Side side = (rng() & 1) ? Side::Buy : Side::Sell;
            Price np = (side == Side::Buy)
                ? mid - 1 - static_cast<Price>(rng() % 15)
                : mid + 1 + static_cast<Price>(rng() % 15);
            np = std::clamp(np, Price{1}, static_cast<Price>(kTicks - 2));
            Quantity nq = 1 + rng() % 20;

            last_aggressor = side;
            auto r = book.modify(live[idx], np, nq);
            if (!r.accepted || r.resting == 0) {
                live[idx] = live.back();
                live.pop_back();
            }
        }

        size_t new_trades = trades.size() - trades_before;
        total_trades += new_trades;
        total_fills += new_trades;
        update_mid();

        // Update the display every few operations
        if (i % 3 == 0 || new_trades > 0) {
            draw(book, trades, i + 1, total_trades, total_fills);
            std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
        }
    }

    // Final draw
    draw(book, trades, kOpsTotal, total_trades, total_fills);

    std::printf("\n  Done. %zu operations, %zu trades.\n\n", kOpsTotal, total_trades);
    return 0;
}
