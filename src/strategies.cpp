#include "strategies.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <sstream>

// ===================== Params / spec parsing =====================
bool parse_agent_spec(const std::string& spec, std::string& name, Params& params) {
    params.kv.clear();
    const size_t colon = spec.find(':');
    name = spec.substr(0, colon);
    if (name.empty()) return false;
    if (colon == std::string::npos) return true;
    std::stringstream ss(spec.substr(colon + 1));
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos || eq == 0) return false;
        params.kv[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return true;
}

namespace {

Price clamp_px(Price p) { return std::max<Price>(1, p); }

// ===================== Market maker =====================
// Quotes both sides around fair value, skews by inventory, requotes on a timer
// and on every book change.
class MarketMaker : public Strategy {
public:
    explicit MarketMaker(const Params& p)
        : half_(static_cast<Price>(p.get("spread", 2))),
          size_(static_cast<Qty>(p.get("size", 5))),
          skew_(p.get("skew", 0.05)),
          max_pos_(static_cast<Qty>(p.get("maxpos", 50))),
          interval_(static_cast<Ts>(p.get("interval_us", 100) * 1000)) {}

    const char* name() const override { return "mm"; }

    void on_event(const Event& ev, AgentContext& ctx) override {
        switch (ev.kind) {
            case EventKind::Fill:
                if (ev.remaining == 0) clear(ev.order_id);
                break;
            case EventKind::Cancelled:
            case EventKind::Rejected:
                clear(ev.order_id);
                break;
            case EventKind::Trade:
            case EventKind::BookUpdate:
            case EventKind::SessionStart:
                requote(ctx);
                break;
            case EventKind::Ack: break;
        }
    }

    void on_idle(AgentContext& ctx) override { requote(ctx); }

private:
    void clear(OrderId id) {
        if (id == bid_id_) bid_id_ = 0;
        if (id == ask_id_) ask_id_ = 0;
    }

    void requote(AgentContext& ctx) {
        const Ts t = ctx.now();
        if (t - last_ < interval_) return;
        last_ = t;
        const Price mid = ctx.fair();
        const Price skew = static_cast<Price>(std::llround(-skew_ * ctx.position()));
        Price bid = clamp_px(mid - half_ + skew);
        Price ask = mid + half_ + skew;
        if (ask <= bid) ask = bid + 1;
        const bool want_bid = ctx.position() < max_pos_;
        const bool want_ask = ctx.position() > -max_pos_;

        if (bid_id_ && (!want_bid || bid_px_ != bid)) { ctx.cancel(bid_id_); bid_id_ = 0; }
        if (!bid_id_ && want_bid) { bid_id_ = ctx.submit(Side::Buy, bid, size_); bid_px_ = bid; }
        if (ask_id_ && (!want_ask || ask_px_ != ask)) { ctx.cancel(ask_id_); ask_id_ = 0; }
        if (!ask_id_ && want_ask) { ask_id_ = ctx.submit(Side::Sell, ask, size_); ask_px_ = ask; }
    }

    Price half_;
    Qty size_;
    double skew_;
    Qty max_pos_;
    Ts interval_;
    Ts last_ = 0;
    OrderId bid_id_ = 0, ask_id_ = 0;
    Price bid_px_ = 0, ask_px_ = 0;
};

// ===================== Latent true value =====================
// A random walk with jumps, stepped once per millisecond of session time.
// Every noise trader builds its own copy from the same market seed, so they all
// agree on the path without sharing any memory. Jumps are what give momentum
// something to catch; the walk between jumps is what mean reversion fades.
class Fundamental {
public:
    Fundamental(uint64_t seed, double x0, double sigma_ms, double jump_per_ms, double jump_ticks, double drift_ms)
        : rng_(seed ^ 0xF00DULL), x_(x0), sigma_(sigma_ms), jump_(jump_per_ms), jump_ticks_(jump_ticks), drift_(drift_ms) {}

    double value(Ts elapsed_ns) {
        const int64_t idx = elapsed_ns / 1000000;
        while (cur_ < idx) {
            x_ += drift_ + sigma_ * normal_(rng_);
            if (uni_(rng_) < jump_) x_ += jump_ticks_ * (0.5 + uni_(rng_)) * (uni_(rng_) < 0.5 ? -1.0 : 1.0);
            ++cur_;
        }
        return x_;
    }

private:
    std::mt19937_64 rng_;
    double x_;
    double sigma_, jump_, jump_ticks_, drift_;
    int64_t cur_ = 0;
    std::normal_distribution<double> normal_{0.0, 1.0};
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

// ===================== Noise trader =====================
// Sees the shared fundamental through private noise. Posts limit orders around
// its view; when its view is beyond the touch it takes liquidity with some
// probability. Expires stale orders. This is the informed-plus-uninformed flow
// that the market maker earns from and gets picked off by.
class NoiseTrader : public Strategy {
public:
    explicit NoiseTrader(const Params& p)
        : interval_(static_cast<Ts>(p.get("interval_us", 200) * 1000)),
          nsigma_(p.get("nsigma", 2.0)),
          pull_(p.get("pull", 0.1)),
          max_size_(static_cast<int>(p.get("size", 3))),
          p_aggr_(p.get("aggr", 0.4)),
          blend_(p.get("blend", 0.02)),
          ttl_(static_cast<Ts>(p.get("ttl_ms", 5) * 1000000)),
          max_pos_(static_cast<Qty>(p.get("maxpos", 100))),
          depth_(static_cast<int>(p.get("depth", 10))),
          fsigma_(p.get("fsigma", 0.15)),
          fjump_(p.get("jump", 0.003)),
          fjump_ticks_(p.get("jumpsize", 25)),
          fdrift_(p.get("drift", 0.0)) {}

    const char* name() const override { return "noise"; }

    void on_start(AgentContext& ctx) override {
        fundamental_ = std::make_unique<Fundamental>(ctx.market_seed(), static_cast<double>(ctx.initial_px()),
                                                     fsigma_, fjump_, fjump_ticks_, fdrift_);
        fund_ = static_cast<double>(ctx.fair());
    }

    void on_event(const Event& ev, AgentContext& ctx) override {
        switch (ev.kind) {
            case EventKind::Fill: if (ev.remaining == 0) forget(ev.order_id); break;
            case EventKind::Cancelled:
            case EventKind::Rejected: forget(ev.order_id); break;
            case EventKind::Trade: step(ctx); break;
            default: break;
        }
    }

    void on_idle(AgentContext& ctx) override { step(ctx); }

private:
    struct Live { OrderId id; Ts ts; };

    void forget(OrderId id) {
        for (auto it = live_.begin(); it != live_.end(); ++it)
            if (it->id == id) { live_.erase(it); return; }
    }

    void step(AgentContext& ctx) {
        const Ts t = ctx.now();
        if (t - last_ < interval_) return;
        last_ = t;
        auto& rng = ctx.rng();

        // private view = shared fundamental + mean-reverting private noise, nudged toward the mid
        noise_ += -pull_ * noise_ + nsigma_ * normal_(rng);
        fund_ = fundamental_->value(ctx.elapsed()) + noise_;
        fund_ += blend_ * (static_cast<double>(ctx.fair()) - fund_);
        const Price f = clamp_px(static_cast<Price>(std::llround(fund_)));

        while (!live_.empty() && t - live_.front().ts > ttl_) {
            ctx.cancel(live_.front().id);
            live_.pop_front();
        }

        Side side;
        if (ctx.position() >= max_pos_) side = Side::Sell;
        else if (ctx.position() <= -max_pos_) side = Side::Buy;
        else if (ctx.has_ask() && f > ctx.ask_px()) side = Side::Buy;   // view above the offer
        else if (ctx.has_bid() && f < ctx.bid_px()) side = Side::Sell;  // view below the bid
        else side = coin_(rng) ? Side::Buy : Side::Sell;
        const Qty qty = 1 + static_cast<Qty>(rng() % static_cast<uint64_t>(std::max(1, max_size_)));

        const bool informed = (side == Side::Buy && ctx.has_ask() && f > ctx.ask_px()) ||
                              (side == Side::Sell && ctx.has_bid() && f < ctx.bid_px());
        if (informed && uni_(rng) < p_aggr_) {
            const Price px = side == Side::Buy ? ctx.ask_px() : ctx.bid_px();
            ctx.submit(side, clamp_px(px), qty, TimeInForce::IOC);
        } else {
            const Price off = 1 + static_cast<Price>(rng() % static_cast<uint64_t>(std::max(1, depth_)));
            const Price px = side == Side::Buy ? clamp_px(f - off) : f + off;
            const OrderId id = ctx.submit(side, px, qty, TimeInForce::GTC);
            if (id) live_.push_back(Live{id, t});
        }
    }

    Ts interval_;
    double nsigma_;
    double pull_;
    int max_size_;
    double p_aggr_;
    double blend_;
    Ts ttl_;
    Qty max_pos_;
    int depth_;
    double fsigma_, fjump_, fjump_ticks_, fdrift_;
    std::unique_ptr<Fundamental> fundamental_;
    double fund_ = 0;
    double noise_ = 0;
    Ts last_ = 0;
    std::deque<Live> live_;
    std::normal_distribution<double> normal_{0.0, 1.0};
    std::bernoulli_distribution coin_{0.5};
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

// ===================== Signal traders =====================
// Momentum and mean reversion share one skeleton: sample the mid on a clock,
// keep moving averages over those samples, derive a target position, and trade
// toward it with IOC orders at the touch, one chunk per cooldown.
class SignalTrader : public Strategy {
public:
    explicit SignalTrader(const Params& p)
        : thr_(p.get("thr", 2.0)),
          max_pos_(static_cast<Qty>(p.get("maxpos", 30))),
          sample_(static_cast<Ts>(p.get("sample_us", 1000) * 1000)),
          size_(static_cast<Qty>(p.get("size", 5))),
          cooldown_(static_cast<Ts>(p.get("cooldown_us", 10000) * 1000)) {}

    void on_event(const Event&, AgentContext& ctx) override { tick(ctx); }
    void on_idle(AgentContext& ctx) override { tick(ctx); }

protected:
    // Return the desired position given the latest sample; called once per sample.
    virtual Qty target(double px, Qty pos) = 0;
    virtual bool warm() const = 0;
    virtual void push(double px) = 0;

    double thr_;
    Qty max_pos_;

private:
    void tick(AgentContext& ctx) {
        const Ts t = ctx.now();
        if (t - last_sample_ < sample_) return;
        last_sample_ = t;
        push(static_cast<double>(ctx.fair()));
        if (!warm()) return;
        if (t - last_trade_ < cooldown_) return;
        const Qty pos = ctx.position();
        const Qty want = target(static_cast<double>(ctx.fair()), pos);
        const Qty diff = want - pos;
        if (diff > 0 && ctx.has_ask()) {
            ctx.submit(Side::Buy, ctx.ask_px(), std::min(size_, diff), TimeInForce::IOC);
            last_trade_ = t;
        } else if (diff < 0 && ctx.has_bid()) {
            ctx.submit(Side::Sell, ctx.bid_px(), std::min(size_, -diff), TimeInForce::IOC);
            last_trade_ = t;
        }
    }

    Ts sample_;
    Qty size_;
    Ts cooldown_;
    Ts last_sample_ = 0;
    Ts last_trade_ = 0;
};

// Fast/slow moving-average crossover on sampled mids. Holds while the fast
// average stays on the same side of the slow one; flattens when it crosses back.
class Momentum : public SignalTrader {
public:
    explicit Momentum(const Params& p)
        : SignalTrader(p),
          fast_(static_cast<size_t>(std::max(1.0, p.get("fast", 20)))),
          slow_(static_cast<size_t>(std::max(2.0, p.get("slow", 200)))) {}

    const char* name() const override { return "momentum"; }

protected:
    void push(double px) override { fast_.push(px); slow_.push(px); }
    bool warm() const override { return slow_.full(); }
    Qty target(double, Qty pos) override {
        const double d = fast_.value() - slow_.value();
        if (d > thr_) return max_pos_;
        if (d < -thr_) return -max_pos_;
        if ((d > 0 && pos > 0) || (d < 0 && pos < 0)) return pos; // still on side, hold
        return 0;
    }

private:
    RollingSMA fast_, slow_;
};

// Fades the sampled mid when it strays from its moving average; flattens once
// it has come most of the way back.
class MeanReversion : public SignalTrader {
public:
    explicit MeanReversion(const Params& p)
        : SignalTrader(p),
          sma_(static_cast<size_t>(std::max(2.0, p.get("period", 200)))),
          exit_(p.get("exit", 0.25)) {}

    const char* name() const override { return "meanrev"; }

protected:
    void push(double px) override { sma_.push(px); }
    bool warm() const override { return sma_.full(); }
    Qty target(double px, Qty pos) override {
        const double d = px - sma_.value();
        if (d > thr_) return -max_pos_;
        if (d < -thr_) return max_pos_;
        if (std::fabs(d) < thr_ * exit_) return 0;
        return pos;
    }

private:
    RollingSMA sma_;
    double exit_;
};

} // namespace

std::unique_ptr<Strategy> make_strategy(const std::string& name, const Params& p) {
    if (name == "mm") return std::make_unique<MarketMaker>(p);
    if (name == "noise") return std::make_unique<NoiseTrader>(p);
    if (name == "momentum") return std::make_unique<Momentum>(p);
    if (name == "meanrev") return std::make_unique<MeanReversion>(p);
    return nullptr;
}

std::string strategy_help() {
    return
        "  mm        spread=2 size=5 skew=0.05 maxpos=50 interval_us=100\n"
        "  noise     interval_us=200 nsigma=2 pull=0.1 size=3 aggr=0.4 blend=0.02 ttl_ms=5 maxpos=100 depth=10\n"
        "            shared fundamental (same for every noise trader): fsigma=0.15 jump=0.003 jumpsize=25 drift=0\n"
        "  momentum  sample_us=1000 fast=20 slow=200 thr=2 size=5 maxpos=30 cooldown_us=10000\n"
        "  meanrev   sample_us=1000 period=200 thr=2 exit=0.25 size=5 maxpos=30 cooldown_us=10000\n";
}
