#include <iostream>
#include <thread>
#include <mutex>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <random>
#include <cmath>
#include <queue>
#include <vector>
#include <algorithm>
#include <array>
#include <limits>

struct Portfolio {
    double cash = 10000.0;
    int holdings = 0;
};

struct Order {
    int qty;
    bool is_buy;
    double limit_px;
    uint32_t trader_id;
    uint64_t seq;
};

struct BuyComp {
    bool operator()(const Order& a, const Order& b) const {
        if (a.limit_px != b.limit_px) return a.limit_px < b.limit_px; // higher px first
        return a.seq > b.seq;                                         // lower seq first
    }
};

struct SellComp {
    bool operator()(const Order& a, const Order& b) const {
        if (a.limit_px != b.limit_px) return a.limit_px > b.limit_px; // lower px first
        return a.seq > b.seq;                                         // lower seq first
    }
};

constexpr double INITIAL_CASH = 10000.0;
constexpr size_t MA_PERIOD = 5;

// ===================== Market State =====================
std::atomic<double> market_price{100.0};
std::deque<double> price_history;
std::mutex price_mtx;

std::atomic<bool> running{true};

// ===================== Book (engine owns it) =====================
std::priority_queue<Order, std::vector<Order>, BuyComp> bids;
std::priority_queue<Order, std::vector<Order>, SellComp> asks;

std::atomic<uint64_t> next_seq{0};

// ===================== Portfolios =====================
std::unordered_map<uint32_t, Portfolio> portfolios;
std::mutex portfolios_mtx;

// ===================== Latency Stats =====================
//
// 1) recv->submit  : strategy code path (compute/logic cost)
// 2) event->submit : feed + strategy
// 3) submit->pop   : engine backlog/drain
//
struct LatAgg {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> max_ns{0};
    std::atomic<uint64_t> dropped{0};
};

inline void record_lat(LatAgg& a, uint64_t ns) {
    a.count.fetch_add(1, std::memory_order_relaxed);
    a.sum_ns.fetch_add(ns, std::memory_order_relaxed);
    uint64_t cur = a.max_ns.load(std::memory_order_relaxed);
    while (ns > cur && !a.max_ns.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {}
}

constexpr int NUM_TRADERS = 4;
constexpr uint32_t TRADER_IDS[NUM_TRADERS] = {0, 1, 2, 99};

int idx_for(uint32_t id) {
    if (id == 0) return 0;
    if (id == 1) return 1;
    if (id == 2) return 2;
    return 3; // 99
}

LatAgg lat_recv_to_submit[NUM_TRADERS];
LatAgg lat_event_to_submit[NUM_TRADERS];
LatAgg lat_submit_to_pop[NUM_TRADERS];

// ===================== SPSC Queue =====================
template <typename T, size_t CAP>
class SPSCQueue {
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of 2");
public:
    bool try_push(const T& v) {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto n = (h + 1) & (CAP - 1);
        if (n == tail_.load(std::memory_order_acquire)) return false; // full
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false; // empty
        out = buf_[t];
        tail_.store((t + 1) & (CAP - 1), std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buf_[CAP];
};

// ===================== Two Queue Networks =====================
// 1) Orders: trader -> engine  (per-trader SPSC)
// 2) Events: engine -> trader  (per-trader SPSC)

// ----- Orders (Trader -> Engine)
struct Command {
    Order o;
    std::chrono::steady_clock::time_point t_event;
    std::chrono::steady_clock::time_point t_recv;
    std::chrono::steady_clock::time_point t_submit;
};

constexpr size_t QCAP = 1 << 12;

SPSCQueue<Command, QCAP> oq0, oq1, oq2, oq99;

SPSCQueue<Command, QCAP>& order_q_for(uint32_t id) {
    if (id == 0) return oq0;
    if (id == 1) return oq1;
    if (id == 2) return oq2;
    return oq99;
}

// Engine sleep/wake when no orders
std::atomic<int> pending_orders{0};
std::mutex pending_mtx;
std::condition_variable pending_cv;

// ----- Events (Engine -> Trader)
struct MarketEvent {
    uint64_t ev_seq = 0;
    double last_trade = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point t_event{};
};

SPSCQueue<MarketEvent, QCAP> md0, md1, md2, md99;

SPSCQueue<MarketEvent, QCAP>& md_q_for(uint32_t id) {
    if (id == 0) return md0;
    if (id == 1) return md1;
    if (id == 2) return md2;
    return md99;
}

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

// A lock-free O(1) rolling SMA for the fast strategy (local state per thread)
struct RollingSMA {
    std::array<double, MA_PERIOD> buf{};
    size_t n = 0;
    size_t i = 0;
    double sum = 0.0;

    void push(double x) {
        if (n < MA_PERIOD) {
            buf[i] = x;
            sum += x;
            ++n;
            i = (i + 1) % MA_PERIOD;
        } else {
            sum -= buf[i];
            buf[i] = x;
            sum += x;
            i = (i + 1) % MA_PERIOD;
        }
    }

    double value(double fallback) const {
        return (n ? (sum / static_cast<double>(n)) : fallback);
    }
};

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

// ===================== Printing =====================
static void print_lat_block(const char* title, const LatAgg agg[NUM_TRADERS]) {
    std::cout << "\n=== " << title << " ===\n";
    for (int i = 0; i < NUM_TRADERS; ++i) {
        const uint32_t id = TRADER_IDS[i];
        const uint64_t c  = agg[i].count.load(std::memory_order_relaxed);
        const uint64_t s  = agg[i].sum_ns.load(std::memory_order_relaxed);
        const uint64_t mx = agg[i].max_ns.load(std::memory_order_relaxed);
        const uint64_t dr = agg[i].dropped.load(std::memory_order_relaxed);

        const double avg = (c ? (double)s / (double)c : 0.0);
        std::cout << "Trader " << id
                  << " | count: " << c
                  << " | avg(ns): " << avg
                  << " | max(ns): " << mx
                  << " | dropped: " << dr
                  << "\n";
    }
}

int main() {
    portfolios[0]  = Portfolio();
    portfolios[1]  = Portfolio();
    portfolios[2]  = Portfolio(); // NEW fast MA trader
    portfolios[99] = Portfolio();

    push_price(market_price.load(std::memory_order_relaxed));

    // Start engine
    std::thread eng(engine_loop);

    // Seed initial crossing orders (with a "fake" event time)
    auto t0 = std::chrono::steady_clock::now();
    submit_order(true,  1, 100.05, 0, t0, t0);
    submit_order(false, 1,  99.95, 1, t0, t0);

    // Strategies:
    // - Trader 0 baseline MA
    // - Trader 1 baseline MA
    // - Trader 2 FAST MA (same idea, faster C++ path: no lock, O(1) MA update, uses event px)
    // - Trader 99 chaos
    std::thread s0(ma_strategy_baseline, 0);
    std::thread s1(ma_strategy_baseline, 1);
    std::thread s2(ma_strategy_fast, 2);
    std::thread s99(chaos, 99);

    std::this_thread::sleep_for(std::chrono::seconds(10));
    running.store(false, std::memory_order_relaxed);
    pending_cv.notify_all();

    s0.join();
    s1.join();
    s2.join();
    s99.join();
    eng.join();

    double last_px = market_price.load(std::memory_order_relaxed);

    struct Result { uint32_t id; double value; double pnl; int holdings; double cash; };
    std::vector<Result> results;

    {
        std::scoped_lock<std::mutex> pl(portfolios_mtx);
        for (auto &kv : portfolios) {
            uint32_t id = kv.first;
            const Portfolio &p = kv.second;
            double value = p.cash + p.holdings * last_px;
            double pnl = value - INITIAL_CASH;
            results.push_back({id, value, pnl, p.holdings, p.cash});
        }
    }

    std::sort(results.begin(), results.end(),
              [](const Result& a, const Result& b){ return a.pnl > b.pnl; });

    std::cout << "\n=== RESULTS (mark-to-market @ " << last_px << ") ===\n";
    for (auto &r : results) {
        std::cout << "Trader " << r.id
                  << " | PnL: " << r.pnl
                  << " | Value: " << r.value
                  << " | Cash: " << r.cash
                  << " | Holdings: " << r.holdings
                  << "\n";
    }

    std::cout << "\nWinner: Trader " << results.front().id
              << " with PnL " << results.front().pnl << "\n";

    print_lat_block("STRATEGY: RECV->SUBMIT (strategy code path)", lat_recv_to_submit);
    print_lat_block("END-TO-END: EVENT->SUBMIT (feed + strategy)", lat_event_to_submit);
    print_lat_block("SYSTEM: SUBMIT->ENGINE_POP (engine backlog)", lat_submit_to_pop);

    std::cout << "\nNote:\n"
              << "- Trader 2 is intentionally 'same idea as 0/1' but faster C++ hot path:\n"
              << "  * no mutex lock for MA\n"
              << "  * no deque summation\n"
              << "  * O(1) rolling SMA + uses event.last_trade\n"
              << "- All three MA strategies keep the same 10ms cadence to isolate code-speed effects.\n";

    return 0;
}
