#pragma once

// parsers.hpp
// Market data parsers for replaying real exchange data through the engine.
//
// Two formats supported:
//   1. LOBSTER (Limit Order Book System — The Efficient Reconstructor)
//      Academic-grade L3 message data from NASDAQ. CSV format, one line per
//      event. Free sample data available from lobsterdata.com.
//
//   2. NASDAQ ITCH 5.0
//      The raw binary protocol that NASDAQ's TotalView feed uses. Fixed-size
//      messages, big-endian fields, dozens of message types (we parse the
//      order-lifecycle subset: Add, Execute, Cancel, Replace).
//
// Both parsers produce a common Event struct that the replay driver consumes.

#include "lob/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace lob {

// Common event representation shared by all parsers and the replay driver.
enum class EventAction : uint8_t {
    Add    = 1,
    Cancel = 2,
    Modify = 3,
    Execute = 4,  // passive fill (ITCH Execute messages)
};

struct MarketEvent {
    EventAction action;
    Side        side;
    OrderId     order_id;
    Price       price;      // in ticks (parser normalizes)
    Quantity    quantity;
    uint64_t    timestamp_ns;  // nanoseconds from midnight (ITCH) or seconds (LOBSTER)

    // For modify/replace: the new order id (ITCH Replace assigns a new ref)
    OrderId     new_order_id = 0;
};

// ============================================================================
// LOBSTER parser
// ============================================================================
// LOBSTER message file format (CSV, no header):
//   timestamp, event_type, order_id, size, price, direction
//
// Event types:
//   1 = Submission of a new limit order
//   2 = Cancellation (partial)
//   3 = Deletion (full cancel)
//   4 = Execution of a visible limit order
//   5 = Execution of a hidden limit order (we treat same as 4)
//   7 = Trading halt (ignored)
//
// Price is in dollar-cents * 10000 (i.e., price 250100 = $25.01).
// Direction: -1 = sell, 1 = buy.

struct LobsterParser {
    // tick_divisor: divide raw LOBSTER prices by this to get tick indices.
    // For LOBSTER's native format (price * 10000), using 100 gives cent ticks.
    int64_t tick_divisor = 100;

    std::vector<MarketEvent> parse_file(const std::string& path) const {
        std::vector<MarketEvent> events;
        events.reserve(500'000);

        std::ifstream fin(path);
        if (!fin.is_open()) {
            std::fprintf(stderr, "  LOBSTER: cannot open '%s'\n", path.c_str());
            return events;
        }

        char line[512];
        while (fin.getline(line, sizeof(line))) {
            auto ev = parse_line(line);
            if (ev) events.push_back(*ev);
        }
        return events;
    }

    std::optional<MarketEvent> parse_line(const char* line) const {
        // Format: timestamp,type,order_id,size,price,direction
        double timestamp = 0;
        int type = 0;
        uint64_t order_id = 0;
        uint64_t size = 0;
        int64_t price = 0;
        int direction = 0;

        int n = std::sscanf(line, "%lf,%d,%llu,%llu,%lld,%d",
            &timestamp, &type,
            (unsigned long long*)&order_id,
            (unsigned long long*)&size,
            (long long*)&price,
            &direction);
        if (n < 6) return std::nullopt;

        MarketEvent ev{};
        ev.order_id = order_id;
        ev.quantity = size;
        ev.price = price / tick_divisor;
        ev.side = (direction == 1) ? Side::Buy : Side::Sell;
        ev.timestamp_ns = static_cast<uint64_t>(timestamp * 1e9);

        switch (type) {
        case 1:  ev.action = EventAction::Add;     break;
        case 2:  // partial cancel — reduce size
        case 3:  ev.action = EventAction::Cancel;   break;
        case 4:
        case 5:  ev.action = EventAction::Execute;  break;
        default: return std::nullopt;
        }
        return ev;
    }
};

// ============================================================================
// NASDAQ ITCH 5.0 parser
// ============================================================================
// Binary protocol, big-endian. Messages are length-prefixed in the raw feed;
// we support both raw ITCH (with 2-byte length prefix per message) and
// "bare" streams (just concatenated message bodies, type byte first).
//
// Message types we parse (the order lifecycle):
//   'A' = Add Order (no MPID)           36 bytes
//   'F' = Add Order with MPID            40 bytes
//   'E' = Order Executed                 31 bytes
//   'X' = Order Cancel                   23 bytes
//   'D' = Order Delete                   19 bytes
//   'U' = Order Replace                  35 bytes
//
// All other message types are skipped.
//
// Field encoding: big-endian unsigned integers. We use manual byte-swapping
// rather than ntohl/ntohs to avoid platform headers and be explicit.

namespace itch {

inline uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}

inline uint64_t read_u48(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 40) |
           (static_cast<uint64_t>(p[1]) << 32) |
           (static_cast<uint64_t>(p[2]) << 24) |
           (static_cast<uint64_t>(p[3]) << 16) |
           (static_cast<uint64_t>(p[4]) << 8)  |
           static_cast<uint64_t>(p[5]);
}

inline uint64_t read_u64(const uint8_t* p) {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8)  |
           static_cast<uint64_t>(p[7]);
}

// Message sizes (body only, not including the 2-byte length prefix)
inline size_t message_size(uint8_t type) {
    switch (type) {
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    // Skip types we don't need but must know the size of to advance:
    case 'S': return 12;   // System Event
    case 'R': return 39;   // Stock Directory
    case 'H': return 25;   // Stock Trading Action
    case 'Y': return 20;   // Reg SHO Restriction
    case 'L': return 26;   // Market Participant Position
    case 'V': return 35;   // MWCB Decline Level
    case 'W': return 12;   // MWCB Status
    case 'K': return 28;   // IPO Quoting Period Update
    case 'J': return 35;   // LULD Auction Collar
    case 'h': return 21;   // Operational Halt
    case 'Q': return 40;   // Cross Trade
    case 'B': return 19;   // Broken Trade
    case 'I': return 50;   // NOII
    case 'N': return 20;   // RPII
    case 'P': return 44;   // Non-Cross Trade
    case 'C': return 36;   // Order Executed with Price
    default:  return 0;    // unknown — can't advance
    }
}

} // namespace itch

struct ItchParser {
    // Filter to a specific stock symbol (8 chars, space-padded right).
    // Empty = accept all symbols.
    std::string symbol_filter;

    // Tick divisor: ITCH prices are in fixed-point with 4 implied decimals
    // (i.e., price field 250100 = $25.0100). Divide by this to get ticks.
    int64_t tick_divisor = 100;

    // Whether the file has 2-byte length prefixes before each message.
    bool length_prefixed = true;

    std::vector<MarketEvent> parse_file(const std::string& path) const {
        std::vector<MarketEvent> events;
        events.reserve(1'000'000);

        std::ifstream fin(path, std::ios::binary);
        if (!fin.is_open()) {
            std::fprintf(stderr, "  ITCH: cannot open '%s'\n", path.c_str());
            return events;
        }

        // Read entire file into memory for speed
        fin.seekg(0, std::ios::end);
        auto file_size = fin.tellg();
        fin.seekg(0, std::ios::beg);
        std::vector<uint8_t> buf(static_cast<size_t>(file_size));
        fin.read(reinterpret_cast<char*>(buf.data()), file_size);

        const uint8_t* ptr = buf.data();
        const uint8_t* end = ptr + buf.size();

        // Padded symbol for comparison
        char sym_padded[9] = "        ";
        if (!symbol_filter.empty()) {
            size_t len = std::min(symbol_filter.size(), size_t{8});
            std::memcpy(sym_padded, symbol_filter.c_str(), len);
        }

        while (ptr < end) {
            size_t msg_len;
            const uint8_t* body;

            if (length_prefixed) {
                if (ptr + 2 > end) break;
                msg_len = itch::read_u16(ptr);
                body = ptr + 2;
                if (body + msg_len > end) break;
                ptr = body + msg_len;
            } else {
                if (ptr >= end) break;
                uint8_t type = *ptr;
                msg_len = itch::message_size(type);
                if (msg_len == 0) break;
                body = ptr;
                if (body + msg_len > end) break;
                ptr = body + msg_len;
            }

            if (msg_len == 0) continue;
            auto ev = parse_message(body, msg_len, sym_padded);
            if (ev) events.push_back(*ev);
        }

        return events;
    }

    std::optional<MarketEvent> parse_message(const uint8_t* msg, size_t len,
                                              const char* sym_padded) const {
        uint8_t type = msg[0];

        switch (type) {
        case 'A':  // Add Order (no MPID) — 36 bytes
        case 'F':  // Add Order with MPID — 40 bytes
        {
            if (len < 36) return std::nullopt;
            // Filter by stock symbol (bytes 11-18)
            if (symbol_filter.size() > 0 && std::memcmp(msg + 11, sym_padded, 8) != 0)
                return std::nullopt;

            MarketEvent ev{};
            ev.action = EventAction::Add;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11 - 2);
            // Order reference number is at bytes 11..14 (4 bytes) — actually
            // ITCH spec: offset 1=stock_locate(2), 3=tracking(2), 5=timestamp(6),
            // 11=order_ref(8), 19=side(1), 20=shares(4), 24=stock(8), 32=price(4)
            ev.order_id = itch::read_u64(msg + 11 - 2);
            // Re-reading more carefully from the spec:
            // 'A' message layout:
            //  0: type (1)
            //  1: stock locate (2)
            //  3: tracking number (2)
            //  5: timestamp (6)
            // 11: order reference number (8)
            // 19: buy/sell indicator (1) 'B' or 'S'
            // 20: shares (4)
            // 24: stock (8)
            // 32: price (4)
            ev.order_id = itch::read_u64(msg + 11);
            ev.side = (msg[19] == 'B') ? Side::Buy : Side::Sell;
            ev.quantity = itch::read_u32(msg + 20);
            ev.price = static_cast<int64_t>(itch::read_u32(msg + 32)) / tick_divisor;
            // Re-check symbol filter at correct offset
            if (!symbol_filter.empty() && std::memcmp(msg + 24, sym_padded, 8) != 0)
                return std::nullopt;
            return ev;
        }

        case 'E':  // Order Executed — 31 bytes
        {
            if (len < 31) return std::nullopt;
            // 0: type, 1: locate(2), 3: tracking(2), 5: timestamp(6),
            // 11: order_ref(8), 19: shares(4), 23: match_number(8)
            MarketEvent ev{};
            ev.action = EventAction::Execute;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11);
            ev.quantity = itch::read_u32(msg + 19);
            ev.price = 0;  // execution at resting price (not in this message)
            return ev;
        }

        case 'C':  // Order Executed with Price — 36 bytes
        {
            if (len < 36) return std::nullopt;
            MarketEvent ev{};
            ev.action = EventAction::Execute;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11);
            ev.quantity = itch::read_u32(msg + 19);
            ev.price = static_cast<int64_t>(itch::read_u32(msg + 32)) / tick_divisor;
            return ev;
        }

        case 'X':  // Order Cancel — 23 bytes
        {
            if (len < 23) return std::nullopt;
            // 11: order_ref(8), 19: cancelled_shares(4)
            MarketEvent ev{};
            ev.action = EventAction::Cancel;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11);
            ev.quantity = itch::read_u32(msg + 19);
            return ev;
        }

        case 'D':  // Order Delete — 19 bytes
        {
            if (len < 19) return std::nullopt;
            MarketEvent ev{};
            ev.action = EventAction::Cancel;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11);
            ev.quantity = 0;  // full cancel
            return ev;
        }

        case 'U':  // Order Replace — 35 bytes
        {
            if (len < 35) return std::nullopt;
            // 11: original_ref(8), 19: new_ref(8), 27: shares(4), 31: price(4)
            MarketEvent ev{};
            ev.action = EventAction::Modify;
            ev.timestamp_ns = itch::read_u48(msg + 5);
            ev.order_id = itch::read_u64(msg + 11);
            ev.new_order_id = itch::read_u64(msg + 19);
            ev.quantity = itch::read_u32(msg + 27);
            ev.price = static_cast<int64_t>(itch::read_u32(msg + 31)) / tick_divisor;
            return ev;
        }

        default:
            return std::nullopt;
        }
    }
};

// ============================================================================
// Format auto-detection
// ============================================================================

enum class DataFormat { Unknown, Lobster, Itch };

inline DataFormat detect_format(const std::string& path) {
    // Check file extension first
    auto ext_pos = path.rfind('.');
    if (ext_pos != std::string::npos) {
        std::string ext = path.substr(ext_pos);
        // Lowercase
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".csv") return DataFormat::Lobster;
        if (ext == ".bin" || ext == ".itch") return DataFormat::Itch;
    }

    // Peek at first bytes: if it starts with ASCII digits and commas, LOBSTER
    std::ifstream fin(path, std::ios::binary);
    if (!fin.is_open()) return DataFormat::Unknown;
    char buf[64]{};
    fin.read(buf, sizeof(buf));

    bool looks_csv = false;
    for (int i = 0; i < 32 && buf[i]; ++i) {
        if (buf[i] == ',') { looks_csv = true; break; }
        if (buf[i] != '.' && buf[i] != '-' && !std::isdigit(static_cast<unsigned char>(buf[i])))
            break;
    }
    if (looks_csv) return DataFormat::Lobster;

    return DataFormat::Itch;
}

} // namespace lob
