#include "order_book.h"

#include <algorithm>

OrderBook::OrderBook(Price min_px, Price max_px, size_t reserve_orders)
    : min_px_(min_px), max_px_(max_px),
      bids_(static_cast<size_t>(max_px - min_px)),
      asks_(static_cast<size_t>(max_px - min_px)) {
    pool_.reserve(reserve_orders);
    index_.reserve(reserve_orders);
}

uint32_t OrderBook::alloc() {
    if (free_head_ != NIL) {
        const uint32_t n = free_head_;
        free_head_ = pool_[n].next;
        return n;
    }
    pool_.push_back(Node{});
    return static_cast<uint32_t>(pool_.size() - 1);
}

void OrderBook::release(uint32_t n) {
    pool_[n].next = free_head_;
    free_head_ = n;
}

void OrderBook::unlink(uint32_t n) {
    Node& node = pool_[n];
    Level& lvl = level(node.side, node.px);
    if (node.prev != NIL) pool_[node.prev].next = node.next; else lvl.head = node.next;
    if (node.next != NIL) pool_[node.next].prev = node.prev; else lvl.tail = node.prev;
    lvl.qty -= node.remaining;
    --lvl.count;
    index_.erase(node.id);
    const Side side = node.side;
    const Price px = node.px;
    release(n);
    if (lvl.count == 0) {
        if (side == Side::Buy && px == best_bid_) advance_best(Side::Buy);
        else if (side == Side::Sell && px == best_ask_) advance_best(Side::Sell);
    }
}

void OrderBook::advance_best(Side s) {
    if (s == Side::Buy) {
        for (Price p = best_bid_ - 1; p >= min_px_; --p) {
            if (bids_[p - min_px_].count) { best_bid_ = p; return; }
        }
        best_bid_ = -1;
    } else {
        for (Price p = best_ask_ + 1; p < max_px_; ++p) {
            if (asks_[p - min_px_].count) { best_ask_ = p; return; }
        }
        best_ask_ = -1;
    }
}

void OrderBook::rest(const Command& c, Qty remaining) {
    const uint32_t n = alloc();
    Level& lvl = level(c.side, c.px);
    pool_[n] = Node{c.id, c.agent, remaining, c.px, c.side, NIL, lvl.tail};
    if (lvl.tail != NIL) pool_[lvl.tail].next = n; else lvl.head = n;
    lvl.tail = n;
    ++lvl.count;
    lvl.qty += remaining;
    index_.emplace(c.id, n);
    if (c.side == Side::Buy) {
        if (best_bid_ < 0 || c.px > best_bid_) best_bid_ = c.px;
    } else {
        if (best_ask_ < 0 || c.px < best_ask_) best_ask_ = c.px;
    }
}

AddResult OrderBook::add(const Command& c, std::vector<Fill>& fills, std::vector<CancelInfo>& stp) {
    AddResult r;
    if (c.qty <= 0) { r.reason = RejectReason::BadQty; return r; }
    if (c.px < min_px_ || c.px >= max_px_) { r.reason = RejectReason::OutOfBand; return r; }
    if (index_.count(c.id)) { r.reason = RejectReason::DuplicateId; return r; }

    Qty remaining = c.qty;
    const Side opp = opposite(c.side);

    auto crosses = [&]() {
        if (c.side == Side::Buy) return best_ask_ >= 0 && c.px >= best_ask_;
        return best_bid_ >= 0 && c.px <= best_bid_;
    };

    while (remaining > 0 && crosses()) {
        const Price lpx = (c.side == Side::Buy) ? best_ask_ : best_bid_;
        Level& lvl = level(opp, lpx);
        uint32_t n = lvl.head;
        while (n != NIL && remaining > 0) {
            Node& node = pool_[n];
            const uint32_t next = node.next;
            if (node.agent == c.agent) {
                stp.push_back(CancelInfo{node.id, node.agent, node.remaining});
                unlink(n); // may advance best if the level empties
                n = next;
                continue;
            }
            const Qty tq = std::min(remaining, node.remaining);
            node.remaining -= tq;
            remaining -= tq;
            lvl.qty -= tq;
            fills.push_back(Fill{node.id, node.agent, c.id, c.agent, c.side, lpx, tq, node.remaining});
            if (node.remaining == 0) unlink(n);
            n = next;
        }
    }

    r.accepted = true;
    r.filled = c.qty - remaining;
    if (remaining > 0 && c.tif == TimeInForce::GTC) {
        rest(c, remaining);
        r.resting = remaining;
    }
    return r;
}

bool OrderBook::cancel(OrderId id, AgentId agent, CancelInfo& out, RejectReason& why) {
    auto it = index_.find(id);
    if (it == index_.end()) { why = RejectReason::UnknownOrder; return false; }
    const Node& node = pool_[it->second];
    if (node.agent != agent) { why = RejectReason::NotOwner; return false; }
    out = CancelInfo{id, agent, node.remaining};
    unlink(it->second);
    why = RejectReason::None;
    return true;
}

Qty OrderBook::level_qty(Side s, Price px) const {
    if (px < min_px_ || px >= max_px_) return 0;
    return level(s, px).qty;
}

uint64_t OrderBook::checksum() const {
    uint64_t h = 0;
    for (const auto& kv : index_) {
        const Node& n = pool_[kv.second];
        uint64_t x = n.id * 0x9E3779B97F4A7C15ULL;
        x ^= static_cast<uint64_t>(n.px) * 0xC2B2AE3D27D4EB4FULL;
        x ^= static_cast<uint64_t>(n.remaining) * 0x165667B19E3779F9ULL;
        x ^= (n.side == Side::Buy ? 0x1ULL : 0x2ULL) << 60;
        h += x; // addition is order independent
    }
    return h;
}
