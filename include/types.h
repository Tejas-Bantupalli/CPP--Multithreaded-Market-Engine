#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

// ===================== Scalar types =====================
using Price = int64_t;    // integer ticks; see TICK
using Qty = int32_t;
using OrderId = uint64_t; // (agent id << 40) | agent-local sequence
using AgentId = uint32_t;
using Seq = uint64_t;
using Ts = int64_t;       // nanoseconds on steady_clock

constexpr double TICK = 0.01;
constexpr double INITIAL_CASH = 100000.0;
constexpr size_t QCAP = 1 << 14; // per-queue capacity (power of two)

// How a polling consumer waits when it has nothing to do.
enum class IdlePolicy { Spin, Yield, Sleep };

inline Ts now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline double to_dollars(Price p) { return static_cast<double>(p) * TICK; }
inline Price to_ticks(double d) { return static_cast<Price>(std::llround(d / TICK)); }

enum class Side : uint8_t { Buy = 0, Sell = 1 };
inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline const char* side_name(Side s) { return s == Side::Buy ? "buy" : "sell"; }

enum class TimeInForce : uint8_t { GTC = 0, IOC = 1 };
enum class CmdKind : uint8_t { New = 0, Cancel = 1 };

enum class RejectReason : uint8_t {
    None = 0,
    OutOfBand,
    BadQty,
    DuplicateId,
    UnknownOrder,
    NotOwner,
    Collar,       // outside the session's limit-up/limit-down band
};

inline const char* reject_name(RejectReason r) {
    switch (r) {
        case RejectReason::None: return "none";
        case RejectReason::OutOfBand: return "out_of_band";
        case RejectReason::BadQty: return "bad_qty";
        case RejectReason::DuplicateId: return "duplicate_id";
        case RejectReason::UnknownOrder: return "unknown_order";
        case RejectReason::NotOwner: return "not_owner";
        case RejectReason::Collar: return "collar";
    }
    return "?";
}

// ===================== Agent -> Engine =====================
struct Command {
    CmdKind kind{CmdKind::New};
    Side side{Side::Buy};
    TimeInForce tif{TimeInForce::GTC};
    AgentId agent{0};
    OrderId id{0};
    Price px{0};
    Qty qty{0};
    Ts t_decide{0}; // when the agent popped the event it is reacting to; 0 if idle-driven
    Ts t_submit{0}; // taken immediately before the queue push
};

// ===================== Engine -> Agent =====================
enum class EventKind : uint8_t {
    SessionStart = 0, // broadcast; px = reference price
    Trade,            // broadcast; px, qty, side = aggressor side, plus top of book
    BookUpdate,       // broadcast; top of book changed
    Ack,              // private; order accepted: qty = filled immediately, remaining = resting
    Fill,             // private; px, qty, remaining, side = this order's side, is_maker
    Cancelled,        // private; remaining = qty removed from the book
    Rejected,         // private; reason
};

inline const char* event_name(EventKind k) {
    switch (k) {
        case EventKind::SessionStart: return "session_start";
        case EventKind::Trade: return "trade";
        case EventKind::BookUpdate: return "book_update";
        case EventKind::Ack: return "ack";
        case EventKind::Fill: return "fill";
        case EventKind::Cancelled: return "cancelled";
        case EventKind::Rejected: return "rejected";
    }
    return "?";
}

struct Event {
    EventKind kind{EventKind::SessionStart};
    Side side{Side::Buy};
    RejectReason reason{RejectReason::None};
    uint8_t is_maker{0};
    Seq seq{0};        // engine-wide event sequence
    Ts ts{0};          // engine publish time
    OrderId order_id{0};
    Price px{0};
    Qty qty{0};
    Qty remaining{0};
    double fee{0};     // Fill only: venue fee for this fill, signed (negative = rebate)
    // Top of book at publish time. qty == 0 means that side is empty.
    Price bid_px{0};
    Qty bid_qty{0};
    Price ask_px{0};
    Qty ask_qty{0};
};

// ===================== Engine -> Logger =====================
struct TradeRecord {
    Ts ts{0};
    Seq seq{0};
    Price px{0};
    Qty qty{0};
    AgentId buyer{0};
    AgentId seller{0};
    Side aggressor{Side::Buy};
};
