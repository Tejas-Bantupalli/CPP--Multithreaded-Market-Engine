#pragma once

#include "histogram.h"
#include "spsc_queue.h"
#include "types.h"

#include <atomic>
#include <cstdint>
#include <random>
#include <string>

class AgentContext;

// Implement this to add a trader. One instance runs on one thread; the engine
// calls on_event for every event it delivers and on_idle when the inbound
// queue is empty. Both calls happen on the agent's own thread.
class Strategy {
public:
    virtual ~Strategy() = default;
    virtual const char* name() const = 0;
    virtual void on_start(AgentContext&) {}
    virtual void on_event(const Event& ev, AgentContext& ctx) = 0;
    virtual void on_idle(AgentContext&) {}
};

// Agent-side counters and histograms. Single writer: the agent thread.
struct AgentStats {
    uint64_t submitted = 0;      // commands pushed successfully
    uint64_t queue_full = 0;     // commands dropped because the outbound queue was full
    uint64_t cancels_sent = 0;
    uint64_t events = 0;
    uint64_t fills = 0;
    uint64_t rejected = 0;
    uint64_t cancelled = 0;
    Histogram delivery;   // engine publish -> agent pop
    Histogram react;      // agent pop -> submit
    Histogram event_age;  // engine publish -> submit
};

// The strategy's only view of the world. Owned by the engine, used on the
// agent's thread only.
class AgentContext {
public:
    AgentContext(AgentId id, SPSCQueue<Command, QCAP>& out, SPSCQueue<Event, QCAP>& in,
                 Price initial_px, uint64_t seed)
        : id_(id), out_(out), in_(in), last_px_(initial_px), rng_(seed) {}

    AgentId id() const { return id_; }
    Ts now() const { return now_ns(); }
    std::mt19937_64& rng() { return rng_; }

    // Returns the new order id, or 0 if the outbound queue was full.
    OrderId submit(Side side, Price px, Qty qty, TimeInForce tif = TimeInForce::GTC) {
        Command c;
        c.kind = CmdKind::New;
        c.side = side;
        c.tif = tif;
        c.agent = id_;
        c.id = (static_cast<OrderId>(id_) << 40) | (++local_seq_);
        c.px = px;
        c.qty = qty;
        c.t_decide = t_dispatch_;
        c.t_submit = now_ns();
        if (!out_.try_push(c)) { ++stats_.queue_full; return 0; }
        ++stats_.submitted;
        if (t_dispatch_) {
            stats_.react.record(c.t_submit - t_dispatch_);
            stats_.event_age.record(c.t_submit - t_event_);
        }
        return c.id;
    }

    bool cancel(OrderId id) {
        Command c;
        c.kind = CmdKind::Cancel;
        c.agent = id_;
        c.id = id;
        c.t_decide = t_dispatch_;
        c.t_submit = now_ns();
        if (!out_.try_push(c)) { ++stats_.queue_full; return false; }
        ++stats_.submitted;
        ++stats_.cancels_sent;
        return true;
    }

    // Position and cash as seen through this agent's own fill events.
    Qty position() const { return position_; }
    double cash() const { return cash_; }

    // Last top of book seen. qty == 0 means empty side.
    Price bid_px() const { return bid_px_; }
    Qty bid_qty() const { return bid_qty_; }
    Price ask_px() const { return ask_px_; }
    Qty ask_qty() const { return ask_qty_; }
    bool has_bid() const { return bid_qty_ > 0; }
    bool has_ask() const { return ask_qty_ > 0; }
    Price last_px() const { return last_px_; }

    // Best guess of fair value: mid if two-sided, else the one side, else last trade.
    Price fair() const {
        if (has_bid() && has_ask()) return (bid_px_ + ask_px_) / 2;
        if (has_bid()) return bid_px_;
        if (has_ask()) return ask_px_;
        return last_px_;
    }

    const AgentStats& stats() const { return stats_; }

    // ----- engine-side plumbing (called on the agent thread by the runner) -----
    bool poll(Event& ev) { return in_.try_pop(ev); }

    void dispatch(const Event& ev, Strategy& s) {
        t_dispatch_ = now_ns();
        t_event_ = ev.ts;
        ++stats_.events;
        stats_.delivery.record(t_dispatch_ - ev.ts);
        switch (ev.kind) {
            case EventKind::Fill:
                ++stats_.fills;
                if (ev.side == Side::Buy) { position_ += ev.qty; cash_ -= to_dollars(ev.px) * ev.qty; }
                else { position_ -= ev.qty; cash_ += to_dollars(ev.px) * ev.qty; }
                break;
            case EventKind::Trade:
                last_px_ = ev.px;
                update_top(ev);
                break;
            case EventKind::BookUpdate:
                update_top(ev);
                break;
            case EventKind::SessionStart:
                last_px_ = ev.px;
                break;
            case EventKind::Rejected: ++stats_.rejected; break;
            case EventKind::Cancelled: ++stats_.cancelled; break;
            case EventKind::Ack: break;
        }
        s.on_event(ev, *this);
        t_dispatch_ = 0;
    }

private:
    void update_top(const Event& ev) {
        bid_px_ = ev.bid_px; bid_qty_ = ev.bid_qty;
        ask_px_ = ev.ask_px; ask_qty_ = ev.ask_qty;
    }

    AgentId id_;
    SPSCQueue<Command, QCAP>& out_;
    SPSCQueue<Event, QCAP>& in_;
    uint64_t local_seq_ = 0;
    Ts t_dispatch_ = 0;
    Ts t_event_ = 0;
    Qty position_ = 0;
    double cash_ = INITIAL_CASH;
    Price last_px_;
    Price bid_px_ = 0;
    Qty bid_qty_ = 0;
    Price ask_px_ = 0;
    Qty ask_qty_ = 0;
    std::mt19937_64 rng_;
    AgentStats stats_;
};
