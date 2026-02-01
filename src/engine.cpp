#include "engine.h"
#include "engine_state.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

// ===================== Helpers =====================

double compute_ma_or_price() {
    std::scoped_lock<std::mutex> lock(price_mtx);
    if (price_history.empty()) return market_price.load(std::memory_order_relaxed);
    double sum = 0.0;
    for (double p : price_history) sum += p;
    return sum / price_history.size();
}

void push_price(double px) {
    std::scoped_lock<std::mutex> lock(price_mtx);
    price_history.push_back(px);
    if (price_history.size() > MA_PERIOD) price_history.pop_front();
}

namespace {
SPSCQueue<Command, QCAP>& order_q_for(uint32_t id) {
    if (id == 0) return oq0;
    if (id == 1) return oq1;
    if (id == 2) return oq2;
    return oq99;
}

SPSCQueue<MarketEvent, QCAP>& md_q_for(uint32_t id) {
    if (id == 0) return md0;
    if (id == 1) return md1;
    if (id == 2) return md2;
    return md99;
}
} // namespace

// ===================== Submit Order (trader -> engine queue) =====================
void submit_order(bool is_buy,
                  int qty,
                  double limit_px,
                  uint32_t trader_id,
                  std::chrono::steady_clock::time_point t_event,
                  std::chrono::steady_clock::time_point t_recv) {
    if (qty <= 0) return;

    Order o{qty, is_buy, limit_px, trader_id, next_seq.fetch_add(1, std::memory_order_relaxed)};
    Command c{o, t_event, t_recv, std::chrono::steady_clock::now()};

    const int idx = idx_for(trader_id);

    record_lat(lat_recv_to_submit[idx],
               (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(c.t_submit - c.t_recv).count());
    record_lat(lat_event_to_submit[idx],
               (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(c.t_submit - c.t_event).count());

    auto& q = order_q_for(trader_id);
    if (q.try_push(c)) {
        pending_orders.fetch_add(1, std::memory_order_release);
        pending_cv.notify_one();
    } else {
        lat_recv_to_submit[idx].dropped.fetch_add(1, std::memory_order_relaxed);
        lat_event_to_submit[idx].dropped.fetch_add(1, std::memory_order_relaxed);
        lat_submit_to_pop[idx].dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ===================== Engine publishes MarketEvent (engine -> traders) =====================
void publish_event(uint64_t ev_seq, double last_trade_px) {
    MarketEvent ev{ev_seq, last_trade_px, std::chrono::steady_clock::now()};
    (void)md0.try_push(ev);
    (void)md1.try_push(ev);
    (void)md2.try_push(ev);
    (void)md99.try_push(ev);
}

// ===================== Engine Loop (single owner of book) =====================
void engine_loop() {
    std::vector<SPSCQueue<Command, QCAP>*> oqs = {&oq0, &oq1, &oq2, &oq99};
    size_t rr = 0;

    std::vector<Command> batch;
    batch.reserve(1024);

    uint64_t ev_seq = 0;

    auto should_stop = [&]() -> bool {
        if (running.load(std::memory_order_relaxed)) return false;
        if (pending_orders.load(std::memory_order_acquire) > 0) return false;
        return true; // keep it simple
    };

    publish_event(++ev_seq, market_price.load(std::memory_order_relaxed));

    while (true) {
        if (pending_orders.load(std::memory_order_acquire) == 0) {
            if (should_stop()) break;
            std::unique_lock<std::mutex> lk(pending_mtx);
            pending_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return pending_orders.load(std::memory_order_acquire) > 0 || !running.load();
            });
        }

        // Drain orders round-robin
        batch.clear();
        for (size_t i = 0; i < oqs.size(); ++i) {
            auto* q = oqs[(rr + i) % oqs.size()];
            Command c;
            int pops = 0;
            while (pops < 256 && q->try_pop(c)) {
                pending_orders.fetch_sub(1, std::memory_order_acq_rel);

                auto t_pop = std::chrono::steady_clock::now();
                record_lat(lat_submit_to_pop[idx_for(c.o.trader_id)],
                           (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t_pop - c.t_submit).count());

                batch.push_back(c);
                ++pops;
            }
        }
        rr = (rr + 1) % oqs.size();

        // Deterministic ordering by seq
        std::sort(batch.begin(), batch.end(),
                  [](const Command& a, const Command& b){ return a.o.seq < b.o.seq; });

        // Apply to book
        for (auto& cmd : batch) {
            if (cmd.o.is_buy) bids.push(cmd.o);
            else asks.push(cmd.o);
        }

        bool did_trade = false;

        // Match
        while (!bids.empty() && !asks.empty() && bids.top().limit_px >= asks.top().limit_px) {
            Order buy  = bids.top(); bids.pop();
            Order sell = asks.top(); asks.pop();

            int traded_qty = std::min(buy.qty, sell.qty);
            if (traded_qty <= 0) continue;

            double trade_px = sell.limit_px; // simple rule
            market_price.store(trade_px, std::memory_order_relaxed);
            push_price(trade_px);

            {
                std::scoped_lock<std::mutex> pl(portfolios_mtx);
                portfolios[buy.trader_id].cash -= traded_qty * trade_px;
                portfolios[buy.trader_id].holdings += traded_qty;

                portfolios[sell.trader_id].cash += traded_qty * trade_px;
                portfolios[sell.trader_id].holdings -= traded_qty;
            }

            buy.qty  -= traded_qty;
            sell.qty -= traded_qty;

            if (buy.qty > 0)  bids.push(buy);
            if (sell.qty > 0) asks.push(sell);

            std::cout << "Trade: " << traded_qty << " @ " << trade_px
                      << " (buyer " << buy.trader_id
                      << ", seller " << sell.trader_id << ")\n";

            did_trade = true;
        }

        if (did_trade) {
            publish_event(++ev_seq, market_price.load(std::memory_order_relaxed));
        }

        if (should_stop()) break;
    }
}

// ===================== Strategies (consume MarketEvent queues) =====================
bool pop_latest_event(uint32_t id, MarketEvent& out) {
    auto& q = md_q_for(id);
    MarketEvent ev;
    bool got = false;
    while (q.try_pop(ev)) {
        out = ev;
        got = true;
    }
    return got;
}

// Baseline MA strategy (like your original 0/1):
// - uses compute_ma_or_price(): lock + sum over deque (extra overhead)
// - uses market_price atomic
void ma_strategy_baseline(uint32_t id) {
    MarketEvent last_ev{};
    while (running.load(std::memory_order_relaxed) && !pop_latest_event(id, last_ev)) {
        std::this_thread::yield();
    }

    while (running.load(std::memory_order_relaxed)) {
        MarketEvent ev = last_ev;
        if (pop_latest_event(id, last_ev)) ev = last_ev;

        auto t_recv = std::chrono::steady_clock::now();

        double ma = compute_ma_or_price(); // lock + O(MA_PERIOD) sum
        double px = market_price.load(std::memory_order_relaxed);

        constexpr int trade_size = 1;
        constexpr double CROSS = 0.02;

        double diff = px - ma;
        if (diff > 0.0001) {
            submit_order(false, trade_size, std::max(0.01, px - CROSS), id, ev.t_event, t_recv);
        } else if (diff < -0.0001) {
            submit_order(true, trade_size, px + CROSS, id, ev.t_event, t_recv);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// FAST MA strategy (very similar logic, but faster C++ path):
// - NO mutex lock
// - NO deque summation
// - uses event.last_trade for px
// - maintains a local rolling SMA in O(1)
void ma_strategy_fast(uint32_t id) {
    MarketEvent last_ev{};
    while (running.load(std::memory_order_relaxed) && !pop_latest_event(id, last_ev)) {
        std::this_thread::yield();
    }

    RollingSMA sma;
    // seed with initial known trade
    if (!std::isnan(last_ev.last_trade)) sma.push(last_ev.last_trade);

    while (running.load(std::memory_order_relaxed)) {
        MarketEvent ev = last_ev;
        if (pop_latest_event(id, last_ev)) ev = last_ev;

        auto t_recv = std::chrono::steady_clock::now();

        // Update SMA from event stream (no locks)
        double px = ev.last_trade;
        if (!std::isnan(px)) sma.push(px);

        double ma = sma.value(px);

        constexpr int trade_size = 1;
        constexpr double CROSS = 0.02;

        double diff = px - ma;
        if (diff > 0.0001) {
            submit_order(false, trade_size, std::max(0.01, px - CROSS), id, ev.t_event, t_recv);
        } else if (diff < -0.0001) {
            submit_order(true, trade_size, px + CROSS, id, ev.t_event, t_recv);
        }

        // keep SAME cadence as 0/1 so you’re isolating “code speed” not “rate”
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Chaos strategy (unchanged behavior, just event-driven for timing)
void chaos(uint32_t id) {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> size_dist(1, 3);
    std::bernoulli_distribution side(0.5);
    std::bernoulli_distribution cross(0.25);
    std::uniform_real_distribution<double> off(0.00, 0.05);

    MarketEvent last_ev{};
    while (running.load(std::memory_order_relaxed) && !pop_latest_event(id, last_ev)) {
        std::this_thread::yield();
    }

    while (running.load(std::memory_order_relaxed)) {
        MarketEvent ev = last_ev;
        if (pop_latest_event(id, last_ev)) ev = last_ev;

        auto t_recv = std::chrono::steady_clock::now();

        double px = market_price.load(std::memory_order_relaxed);
        int qty = size_dist(rng);
        bool is_buy = side(rng);

        double offset = off(rng);
        bool aggressive = cross(rng);

        double limit_px;
        if (is_buy) {
            limit_px = aggressive ? (px + offset) : std::max(0.01, px - offset);
        } else {
            limit_px = aggressive ? std::max(0.01, px - offset) : (px + offset);
        }

        submit_order(is_buy, qty, limit_px, id, ev.t_event, t_recv);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
