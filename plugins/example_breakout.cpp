// Example strategy plugin: channel breakout.
//
// Samples the mid on a clock, keeps the highest and lowest sample over a
// window, and goes long when the mid breaks above the previous window's high
// (short below its low). Holds until the mid returns inside the channel.
//
// Build:  make plugin SRC=plugins/example_breakout.cpp
// Run:    ./build/market_engine --plugin build/plugins/example_breakout.so:window=300,size=5
#include "plugin.h"

#include <algorithm>
#include <deque>

class Breakout : public Strategy {
public:
    explicit Breakout(const Params& p)
        : window_(static_cast<size_t>(std::max(2.0, p.get("window", 300)))),
          sample_(static_cast<Ts>(p.get("sample_us", 1000) * 1000)),
          size_(static_cast<Qty>(p.get("size", 5))),
          max_pos_(static_cast<Qty>(p.get("maxpos", 30))),
          cooldown_(static_cast<Ts>(p.get("cooldown_us", 10000) * 1000)) {}

    const char* name() const override { return "breakout"; }

    void on_event(const Event&, AgentContext& ctx) override { tick(ctx); }
    void on_idle(AgentContext& ctx) override { tick(ctx); }

private:
    void tick(AgentContext& ctx) {
        const Ts t = ctx.now();
        if (t - last_sample_ < sample_) return;
        last_sample_ = t;

        const Price px = ctx.fair();
        // Channel from samples strictly before this one.
        Price hi = 0, lo = 0;
        const bool warm = samples_.size() >= window_;
        if (warm) {
            hi = *std::max_element(samples_.begin(), samples_.end());
            lo = *std::min_element(samples_.begin(), samples_.end());
        }
        samples_.push_back(px);
        if (samples_.size() > window_) samples_.pop_front();
        if (!warm || t - last_trade_ < cooldown_) return;

        const Qty pos = ctx.position();
        Qty want = pos;
        if (px > hi) want = max_pos_;
        else if (px < lo) want = -max_pos_;
        else if ((pos > 0 && px < (hi + lo) / 2) || (pos < 0 && px > (hi + lo) / 2)) want = 0;

        const Qty diff = want - pos;
        if (diff > 0 && ctx.has_ask()) {
            ctx.submit(Side::Buy, ctx.ask_px(), std::min(size_, diff), TimeInForce::IOC);
            last_trade_ = t;
        } else if (diff < 0 && ctx.has_bid()) {
            ctx.submit(Side::Sell, ctx.bid_px(), std::min(size_, -diff), TimeInForce::IOC);
            last_trade_ = t;
        }
    }

    size_t window_;
    Ts sample_;
    Qty size_;
    Qty max_pos_;
    Ts cooldown_;
    std::deque<Price> samples_;
    Ts last_sample_ = 0;
    Ts last_trade_ = 0;
};

MARKET_PLUGIN(Breakout)
