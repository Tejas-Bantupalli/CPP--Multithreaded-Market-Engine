#pragma once

#include "histogram.h"
#include "multicast_ring.h"
#include "spsc_queue.h"
#include "transport.h"
#include "types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
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

    // ---- stack ownership -----------------------------------------------
    // A real desk owns its market-data path, its processing and its order path.
    // These let a strategy do the same instead of accepting the host's defaults.

    // Which plumbing carries your events and orders. Return nothing to accept
    // whatever the host was configured with.
    virtual std::optional<TransportKind> transport() const { return std::nullopt; }

    // Own your entire event loop. The default drains in batches and falls back to
    // the host idle policy, which is a reasonable general-purpose loop and is not
    // necessarily the right one for you. Override it to control batch size, how
    // you wait, when you run housekeeping, and when you submit.
    //
    // Contract: return promptly once `running` reads false, and call ctx.apply()
    // for every event you consume or your own books will drift from the engine's.
    virtual void run(AgentContext& ctx, const std::atomic<bool>& running);
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
    AgentContext(AgentId id, Channel<Command>& out, Channel<Event>& in,
                 Price initial_px, uint64_t seed, uint64_t market_seed)
        : id_(id), out_(out), in_(in), last_px_(initial_px), rng_(seed), market_seed_(market_seed) {}

    AgentId id() const { return id_; }
    Ts now() const { return now_ns(); }
    // Nanoseconds since the session started. Every agent shares the same origin.
    Ts elapsed() const { return now_ns() - t_start_; }
    // Session-wide seed. Agents that must agree on a latent process (the noise
    // traders' fundamental value) derive it from this without sharing memory.
    uint64_t market_seed() const { return market_seed_; }
    Price initial_px() const { return initial_px_; }
    void begin_session(Ts t_start) { t_start_ = t_start; } // engine, before the agent thread starts
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
        if (!out_.push(c)) { ++stats_.queue_full; return 0; }
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
        if (!out_.push(c)) { ++stats_.queue_full; return false; }
        ++stats_.submitted;
        ++stats_.cancels_sent;
        return true;
    }

    // Position and cash as seen through this agent's own fill events.
    Qty position() const { return position_; }
    double cash() const { return cash_; }
    // Venue fees this agent has paid so far, signed. Negative means net rebates.
    double fees() const { return fees_; }

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
    using EventRing = MulticastRing<Event, QCAP>;
    void attach_ring(const EventRing* ring) { ring_ = ring; } // before the agent thread starts
    uint64_t multicast_dropped() const { return reader_.dropped; }

    // Private events first, then broadcasts from the ring when one is attached.
    bool poll(Event& ev) {
        if (in_.pop(ev)) return true;
        return ring_ && ring_->try_read(reader_, ev);
    }

    // Let a blocking transport park the thread instead of the runner spinning.
    // Never used when a multicast ring is attached, since that ring is polled.
    bool transport_blocks() const { return in_.blocking() && ring_ == nullptr; }
    bool wait_poll(Event& ev, int64_t timeout_ns) { return in_.wait_pop(ev, timeout_ns); }
    const char* transport_name() const { return in_.name(); }

    // Bookkeeping only: position, cash, fees, top of book, and the delivery
    // histogram. A custom run loop must call this for every event it consumes.
    // It does not call your on_event.
    void apply(const Event& ev) {
        t_dispatch_ = now_ns();
        t_event_ = ev.ts;
        ++stats_.events;
        stats_.delivery.record(t_dispatch_ - ev.ts);
        switch (ev.kind) {
            case EventKind::Fill:
                ++stats_.fills;
                if (ev.side == Side::Buy) { position_ += ev.qty; cash_ -= to_dollars(ev.px) * ev.qty; }
                else { position_ -= ev.qty; cash_ += to_dollars(ev.px) * ev.qty; }
                cash_ -= ev.fee;      // negative fee is a rebate
                fees_ += ev.fee;
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
    }

    // apply() then hand the event to the strategy. This is what the default loop
    // uses; t_dispatch_ stays set across on_event so react latency is measured
    // from the moment the event was taken off the channel.
    void dispatch(const Event& ev, Strategy& s) {
        apply(ev);
        s.on_event(ev, *this);
        t_dispatch_ = 0;
    }

    // Wait the way the host was configured to. Only meaningful for polling
    // transports; a blocking one should use wait_poll instead.
    void idle() const {
        switch (idle_) {
            case IdlePolicy::Spin: break;
            case IdlePolicy::Yield: std::this_thread::yield(); break;
            case IdlePolicy::Sleep: std::this_thread::sleep_for(std::chrono::microseconds(50)); break;
        }
    }
    void set_idle(IdlePolicy p) { idle_ = p; }   // engine, before the thread starts

private:
    void update_top(const Event& ev) {
        bid_px_ = ev.bid_px; bid_qty_ = ev.bid_qty;
        ask_px_ = ev.ask_px; ask_qty_ = ev.ask_qty;
    }

    AgentId id_;
    Channel<Command>& out_;
    Channel<Event>& in_;
    uint64_t local_seq_ = 0;
    Ts t_dispatch_ = 0;
    Ts t_event_ = 0;
    Qty position_ = 0;
    double cash_ = INITIAL_CASH;
    double fees_ = 0;
    Price last_px_;
    Price bid_px_ = 0;
    Qty bid_qty_ = 0;
    Price ask_px_ = 0;
    Qty ask_qty_ = 0;
    std::mt19937_64 rng_;
    const EventRing* ring_ = nullptr;
    EventRing::Reader reader_;
    uint64_t market_seed_;
    Price initial_px_ = last_px_;
    Ts t_start_ = 0;
    IdlePolicy idle_ = IdlePolicy::Yield;
    AgentStats stats_;
};

// The default event loop. Batch-drain, then housekeeping, then wait. Strategies
// that care about their data path are expected to replace this.
inline void Strategy::run(AgentContext& ctx, const std::atomic<bool>& running) {
    const bool parks = ctx.transport_blocks();
    constexpr int64_t PARK_NS = 50000;  // 50 us, so on_idle still ticks
    Event ev;
    while (running.load(std::memory_order_relaxed)) {
        int n = 0;
        while (n < 256 && ctx.poll(ev)) {
            ctx.dispatch(ev, *this);
            ++n;
        }
        if (n == 0) {
            on_idle(ctx);
            if (parks) {
                if (ctx.wait_poll(ev, PARK_NS)) ctx.dispatch(ev, *this);
            } else {
                ctx.idle();
            }
        }
    }
}
