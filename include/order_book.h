#pragma once

#include "types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

struct Fill {
    OrderId maker_id;
    AgentId maker;
    OrderId taker_id;
    AgentId taker;
    Side taker_side;
    Price px; // maker's price
    Qty qty;
    Qty maker_remaining;
};

struct CancelInfo {
    OrderId id;
    AgentId agent;
    Qty remaining; // quantity removed from the book
};

struct AddResult {
    bool accepted = false;
    RejectReason reason = RejectReason::None;
    Qty filled = 0;
    Qty resting = 0;
};

// Price-time priority limit order book over a fixed price band.
// Single-threaded: the engine thread is the only caller. No I/O, no clocks.
class OrderBook {
public:
    OrderBook(Price min_px, Price max_px, size_t reserve_orders = 1 << 16);

    // Match then rest (GTC) or discard the remainder (IOC).
    // Resting orders from the same agent that would be hit are cancelled and
    // reported in `stp` (self-trade prevention: cancel resting).
    AddResult add(const Command& c, std::vector<Fill>& fills, std::vector<CancelInfo>& stp);

    bool cancel(OrderId id, AgentId agent, CancelInfo& out, RejectReason& why);

    bool has_bid() const { return best_bid_ >= 0; }
    bool has_ask() const { return best_ask_ >= 0; }
    Price best_bid() const { return best_bid_; }
    Price best_ask() const { return best_ask_; }
    Qty best_bid_qty() const { return has_bid() ? bids_[best_bid_ - min_px_].qty : 0; }
    Qty best_ask_qty() const { return has_ask() ? asks_[best_ask_ - min_px_].qty : 0; }
    Qty level_qty(Side s, Price px) const;
    size_t open_orders() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.count(id) != 0; }
    Price min_px() const { return min_px_; }
    Price max_px() const { return max_px_; }

    // Order-independent hash of every resting order (id, price, remaining).
    uint64_t checksum() const;

private:
    static constexpr uint32_t NIL = UINT32_MAX;

    struct Node {
        OrderId id;
        AgentId agent;
        Qty remaining;
        Price px;
        Side side;
        uint32_t next;
        uint32_t prev;
    };

    struct Level {
        uint32_t head = NIL;
        uint32_t tail = NIL;
        Qty qty = 0;
        uint32_t count = 0;
    };

    Level& level(Side s, Price px) { return (s == Side::Buy ? bids_ : asks_)[px - min_px_]; }
    const Level& level(Side s, Price px) const { return (s == Side::Buy ? bids_ : asks_)[px - min_px_]; }

    uint32_t alloc();
    void release(uint32_t n);
    void unlink(uint32_t n); // remove from level + index, release, fix best
    void advance_best(Side s);
    void rest(const Command& c, Qty remaining);

    Price min_px_;
    Price max_px_;
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    Price best_bid_ = -1;
    Price best_ask_ = -1;
    std::vector<Node> pool_;
    uint32_t free_head_ = NIL;
    std::unordered_map<OrderId, uint32_t> index_;
};
