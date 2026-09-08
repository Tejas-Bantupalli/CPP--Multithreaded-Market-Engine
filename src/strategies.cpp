#include "strategies.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include <sstream>

// ===================== Params / spec parsing =====================
double Params::get(const std::string& key, double def) const {
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stod(it->second); } catch (...) { return def; }
}

std::string Params::str(const std::string& key, const std::string& def) const {
    auto it = kv.find(key);
    return it == kv.end() ? def : it->second;
}

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

// ===================== Noise trader =====================
// Private fair value random-walks and is pulled toward the observed mid.
// Posts limit orders around it, sometimes crosses the spread, expires stale orders.
class NoiseTrader : public Strategy {
public:
    explicit NoiseTrader(const Params& p)
        : interval_(static_cast<Ts>(p.get("interval_us", 200) * 1000)),
          sigma_(p.get("sigma", 0.5)),
          max_size_(static_cast<int>(p.get("size", 3))),
          p_aggr_(p.get("aggr", 0.3)),
          blend_(p.get("blend", 0.05)),
          ttl_(static_cast<Ts>(p.get("ttl_ms", 5) * 1000000)),
          max_pos_(static_cast<Qty>(p.get("maxpos", 100))),
          depth_(static_cast<int>(p.get("depth", 10))) {}

    const char* name() const override { return "noise"; }

    void on_start(AgentContext& ctx) override { fund_ = static_cast<double>(ctx.fair()); }

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

        fund_ += sigma_ * normal_(rng);
        fund_ += blend_ * (static_cast<double>(ctx.fair()) - fund_);
        const Price f = clamp_px(static_cast<Price>(std::llround(fund_)));

        while (!live_.empty() && t - live_.front().ts > ttl_) {
            ctx.cancel(live_.front().id);
            live_.pop_front();
        }

        Side side;
        if (ctx.position() >= max_pos_) side = Side::Sell;
        else if (ctx.position() <= -max_pos_) side = Side::Buy;
        else side = coin_(rng) ? Side::Buy : Side::Sell;
        const Qty qty = 1 + static_cast<Qty>(rng() % static_cast<uint64_t>(std::max(1, max_size_)));

        if (uni_(rng) < p_aggr_) {
            Price px;
            if (side == Side::Buy) px = ctx.has_ask() ? ctx.ask_px() : f + depth_;
            else px = ctx.has_bid() ? ctx.bid_px() : clamp_px(f - depth_);
            ctx.submit(side, clamp_px(px), qty, TimeInForce::IOC);
        } else {
            const Price off = 1 + static_cast<Price>(rng() % static_cast<uint64_t>(std::max(1, depth_)));
            const Price px = side == Side::Buy ? clamp_px(f - off) : f + off;
            const OrderId id = ctx.submit(side, px, qty, TimeInForce::GTC);
            if (id) live_.push_back(Live{id, t});
        }
    }

    Ts interval_;
    double sigma_;
    int max_size_;
    double p_aggr_;
    double blend_;
    Ts ttl_;
    Qty max_pos_;
    int depth_;
    double fund_ = 0;
    Ts last_ = 0;
    std::deque<Live> live_;
    std::normal_distribution<double> normal_{0.0, 1.0};
    std::bernoulli_distribution coin_{0.5};
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
};

// ===================== Momentum =====================
// Fast/slow SMA crossover over trade prices; takes liquidity with IOC orders.
class Momentum : public Strategy {
public:
    explicit Momentum(const Params& p)
        : fast_(static_cast<size_t>(std::max(1.0, p.get("fast", 5)))),
          slow_(static_cast<size_t>(std::max(2.0, p.get("slow", 20)))),
          thr_(p.get("thr", 1.0)),
          size_(static_cast<Qty>(p.get("size", 2))),
          max_pos_(static_cast<Qty>(p.get("maxpos", 20))),
          cooldown_(static_cast<Ts>(p.get("cooldown_us", 500) * 1000)) {}

    const char* name() const override { return "momentum"; }

    void on_event(const Event& ev, AgentContext& ctx) override {
        if (ev.kind != EventKind::Trade) return;
        fast_.push(static_cast<double>(ev.px));
        slow_.push(static_cast<double>(ev.px));
        if (!slow_.full()) return;
        const Ts t = ctx.now();
        if (t - last_ < cooldown_) return;
        const double d = fast_.value() - slow_.value();
        if (d > thr_ && ctx.position() < max_pos_ && ctx.has_ask()) {
            ctx.submit(Side::Buy, ctx.ask_px(), size_, TimeInForce::IOC);
            last_ = t;
        } else if (d < -thr_ && ctx.position() > -max_pos_ && ctx.has_bid()) {
            ctx.submit(Side::Sell, ctx.bid_px(), size_, TimeInForce::IOC);
            last_ = t;
        }
    }

private:
    RollingSMA fast_, slow_;
    double thr_;
    Qty size_;
    Qty max_pos_;
    Ts cooldown_;
    Ts last_ = 0;
};

// ===================== Mean reversion =====================
// Fades trades that stray from the SMA; takes liquidity with IOC orders.
class MeanReversion : public Strategy {
public:
    explicit MeanReversion(const Params& p)
        : sma_(static_cast<size_t>(std::max(2.0, p.get("period", 20)))),
          thr_(p.get("thr", 2.0)),
          size_(static_cast<Qty>(p.get("size", 2))),
          max_pos_(static_cast<Qty>(p.get("maxpos", 20))),
          cooldown_(static_cast<Ts>(p.get("cooldown_us", 500) * 1000)) {}

    const char* name() const override { return "meanrev"; }

    void on_event(const Event& ev, AgentContext& ctx) override {
        if (ev.kind != EventKind::Trade) return;
        sma_.push(static_cast<double>(ev.px));
        if (!sma_.full()) return;
        const Ts t = ctx.now();
        if (t - last_ < cooldown_) return;
        const double d = static_cast<double>(ev.px) - sma_.value();
        if (d > thr_ && ctx.position() > -max_pos_ && ctx.has_bid()) {
            ctx.submit(Side::Sell, ctx.bid_px(), size_, TimeInForce::IOC);
            last_ = t;
        } else if (d < -thr_ && ctx.position() < max_pos_ && ctx.has_ask()) {
            ctx.submit(Side::Buy, ctx.ask_px(), size_, TimeInForce::IOC);
            last_ = t;
        }
    }

private:
    RollingSMA sma_;
    double thr_;
    Qty size_;
    Qty max_pos_;
    Ts cooldown_;
    Ts last_ = 0;
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
        "  noise     interval_us=200 sigma=0.5 size=3 aggr=0.3 blend=0.05 ttl_ms=5 maxpos=100 depth=10\n"
        "  momentum  fast=5 slow=20 thr=1 size=2 maxpos=20 cooldown_us=500\n"
        "  meanrev   period=20 thr=2 size=2 maxpos=20 cooldown_us=500\n";
}
