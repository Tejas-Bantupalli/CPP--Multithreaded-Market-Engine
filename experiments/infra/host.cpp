#include "plugin.h"
#include "api.h"
#include <algorithm>
#include <memory>

// Frozen experiment host: candidates cannot receive AgentContext through the API.
class InfraTrader final : public Strategy {
    std::unique_ptr<InfraPolicy> infra_{make_infra()};
    std::size_t samples_ = 0;
    OrderId pending_ = 0;
public:
    explicit InfraTrader(const Params&) {}
    const char* name() const override { return "infra_subject"; }
    void on_event(const Event& ev, AgentContext& ctx) override {
        if (pending_ && ev.order_id == pending_) {
            // IOC quantity is exactly one. Ack precedes Fill in the private FIFO.
            // Keep the reservation until the fill, or an unfilled ack/rejection.
            if (ev.kind == EventKind::Fill || ev.kind == EventKind::Rejected ||
                (ev.kind == EventKind::Ack && ev.qty == 0)) pending_ = 0;
        }
        if (ev.kind != EventKind::Trade) return;
        infra_->push(ev.px);
        samples_ = std::min(samples_ + 1, INFRA_WINDOW);
        if (samples_ < INFRA_WINDOW || pending_) return;
        const auto delta = ev.px * static_cast<int64_t>(INFRA_WINDOW) - infra_->sum();
        constexpr int64_t threshold = 2 * INFRA_WINDOW;
        if (delta > threshold && ctx.has_ask() && ctx.position() < 20)
            pending_ = ctx.submit(Side::Buy, ctx.ask_px(), 1, TimeInForce::IOC);
        else if (delta < -threshold && ctx.has_bid() && ctx.position() > -20)
            pending_ = ctx.submit(Side::Sell, ctx.bid_px(), 1, TimeInForce::IOC);
    }
    void run(AgentContext& ctx, const std::atomic<bool>& running) override {
        Event ev;
        while (running.load(std::memory_order_relaxed)) {
            const unsigned batch = std::clamp(infra_->batch_size(), 1u, 4096u);
            unsigned n = 0;
            while (n < batch && ctx.poll(ev)) {
                // Lost market history invalidates the rolling signal. Stop this
                // participant; the runner rejects the run rather than inventing recovery.
                if (ctx.multicast_dropped()) return;
                ctx.dispatch(ev, *this);
                ++n;
            }
            if (!n) infra_->idle();
        }
    }
};
MARKET_PLUGIN(InfraTrader)
