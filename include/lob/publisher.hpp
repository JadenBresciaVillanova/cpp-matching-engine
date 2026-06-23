#pragma once

// publisher.hpp
// Outbound SPSC edge: matching core → publisher thread.
//
// The matching core is single-threaded and must never block. Instead of
// calling an std::function trade sink that might do I/O or contend on a
// mutex, the core pushes fixed-size messages into an outbound SPSC queue.
// A separate publisher thread drains the queue and does whatever slow work
// is needed (logging, network broadcast, dashboard update).
//
// This completes the edge architecture:
//   [ingest thread] → SPSC → [matching core] → SPSC → [publisher thread]
//
// The message is a superset of Trade + ExecResult so downstream consumers
// get a single ordered stream of everything that happened.

#include "lob/spsc_queue.hpp"
#include "lob/types.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace lob {

enum class OutMsgType : uint8_t {
    Trade      = 0,
    ExecReport = 1,
};

struct OutMsg {
    OutMsgType type;
    uint8_t    side;          // aggressor side for trades
    uint8_t    _pad[2]{};
    OrderId    order_id;
    OrderId    counter_id;    // resting id for trades
    Price      price;
    Quantity   quantity;
    Quantity   filled;        // total filled (exec reports)
    Quantity   resting;       // remaining resting (exec reports)
    Sequence   seq;
    uint8_t    accepted;
    uint8_t    fully_filled;
    uint8_t    _pad2[6]{};
};

using OutQueue = SPSCQueue<OutMsg, 65536>;

// Publisher: owns a thread that drains the outbound queue and calls
// user-provided callbacks. Construct with the queue and callbacks,
// call start(), and stop() or let the destructor join.
class Publisher {
public:
    using TradeCb = std::function<void(const OutMsg&)>;
    using ExecCb  = std::function<void(const OutMsg&)>;

    Publisher(OutQueue& queue, TradeCb on_trade = {}, ExecCb on_exec = {})
        : queue_(queue)
        , on_trade_(std::move(on_trade))
        , on_exec_(std::move(on_exec)) {}

    ~Publisher() { stop(); }

    void start() {
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this]() { run(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    uint64_t messages_processed() const noexcept {
        return processed_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        while (running_.load(std::memory_order_acquire)) {
            OutMsg msg;
            int batch = 0;
            while (queue_.try_pop(msg) && batch < 512) {
                switch (msg.type) {
                case OutMsgType::Trade:
                    if (on_trade_) on_trade_(msg);
                    break;
                case OutMsgType::ExecReport:
                    if (on_exec_) on_exec_(msg);
                    break;
                }
                processed_.fetch_add(1, std::memory_order_relaxed);
                ++batch;
            }
            if (batch == 0) {
                std::this_thread::yield();
            }
        }
        // Drain remaining messages after stop signal
        OutMsg msg;
        while (queue_.try_pop(msg)) {
            switch (msg.type) {
            case OutMsgType::Trade:
                if (on_trade_) on_trade_(msg);
                break;
            case OutMsgType::ExecReport:
                if (on_exec_) on_exec_(msg);
                break;
            }
            processed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    OutQueue& queue_;
    TradeCb on_trade_;
    ExecCb on_exec_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> processed_{0};
    std::thread thread_;
};

// Helper: creates a TradeSink (for OrderBook::set_trade_sink) that pushes
// Trade messages into the outbound SPSC queue instead of calling a callback.
// This is the zero-overhead bridge from the core to the publish edge.
//
// The overrun counter is incremented atomically whenever the queue is full and
// a message is dropped. Monitoring can poll this to detect back-pressure —
// a non-zero count means the publisher thread can't keep up with the matching
// core, which in production would trigger an alert.
inline auto make_spsc_trade_sink(OutQueue& queue, uint8_t& aggressor_side_ref,
                                  std::atomic<uint64_t>& overrun_count) {
    return [&queue, &aggressor_side_ref, &overrun_count](const Trade& t) {
        OutMsg msg{};
        msg.type = OutMsgType::Trade;
        msg.side = aggressor_side_ref;
        msg.order_id = t.incoming_id;
        msg.counter_id = t.resting_id;
        msg.price = t.price;
        msg.quantity = t.quantity;
        msg.seq = t.seq;
        if (!queue.try_push(msg)) [[unlikely]] {
            overrun_count.fetch_add(1, std::memory_order_relaxed);
        }
    };
}

// Backward-compatible overload without overrun counter.
inline auto make_spsc_trade_sink(OutQueue& queue, uint8_t& aggressor_side_ref) {
    static std::atomic<uint64_t> dummy_overruns{0};
    return make_spsc_trade_sink(queue, aggressor_side_ref, dummy_overruns);
}

} // namespace lob
