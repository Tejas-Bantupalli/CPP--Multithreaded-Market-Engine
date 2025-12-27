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

struct Portfolio {
    double cash = 10000.0;
    int holdings = 0;
};

struct Order {
    int qty;
    bool is_buy;
    double limit_px;
    uint32_t trader_id;
    uint64_t seq; // global submission sequence
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

// --- market state ---
std::atomic<double> market_price{100.0};
std::deque<double> price_history;
std::mutex price_mtx;

std::atomic<bool> running{true};

// --- order book (owned by engine thread only) ---
std::priority_queue<Order, std::vector<Order>, BuyComp> bids;
std::priority_queue<Order, std::vector<Order>, SellComp> asks;

// Global sequence for deterministic ordering (not "who got the lock first")
std::atomic<uint64_t> next_seq{0};

// --- portfolios ---
std::unordered_map<uint32_t, Portfolio> portfolios;
std::mutex portfolios_mtx;

// ---------------- latency stats ----------------
//
// We track two latencies per trader:
//
// 1) Reaction latency:   t_submit - t_md_read
//    ("market data read" -> "order submitted into gateway")
//
// 2) Queue/ingress latency: t_engine_pop - t_submit
//    ("order submitted" -> "engine popped command")
//
struct LatAgg {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> sum_ns{0};
    std::atomic<uint64_t> max_ns{0};
    std::atomic<uint64_t> dropped{0};
};

constexpr int NUM_TRADERS = 3;
constexpr uint32_t TRADER_IDS[NUM_TRADERS] = {0, 1, 99};

int idx_for(uint32_t id) {
    if (id == 0) return 0;
    if (id == 1) return 1;
    return 2; // 99 in this demo
}

inline void record_lat(LatAgg& a, uint64_t ns) {
    a.count.fetch_add(1, std::memory_order_relaxed);
    a.sum_ns.fetch_add(ns, std::memory_order_relaxed);

    uint64_t cur = a.max_ns.load(std::memory_order_relaxed);
    while (ns > cur && !a.max_ns.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
        // cur updated by compare_exchange_weak
    }
}

LatAgg reaction_lat[NUM_TRADERS];
LatAgg queue_lat[NUM_TRADERS];

// ---------------- SPSC queue ----------------
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
    // reduce false sharing between producer/consumer
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buf_[CAP];
};

// --- commands from strategies to engine ---
struct Command {
    Order o;
    std::chrono::steady_clock::time_point t_md_read; // when strategy "observed" market
    std::chrono::steady_clock::time_point t_submit;  // when it entered the gateway
};

// One queue per strategy thread (simple explicit mapping)
constexpr size_t QCAP = 1 << 12; // 4096
SPSCQueue<Command, QCAP> q0;
SPSCQueue<Command, QCAP> q1;
SPSCQueue<Command, QCAP> q99;

// pending-count + CV so engine can sleep when no work
std::atomic<int> pending{0};
std::mutex pending_mtx;
std::condition_variable pending_cv;

SPSCQueue<Command, QCAP>& queue_for(uint32_t id) {
    if (id == 0) return q0;
    if (id == 1) return q1;
    return q99; // id==99 in this demo
}

// ---------------- helpers ----------------
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

void submit_order(bool is_buy,
                  int qty,
                  double limit_px,
                  uint32_t trader_id,
                  std::chrono::steady_clock::time_point t_md_read) {
    if (qty <= 0) return;

    const uint64_t seq = next_seq.fetch_add(1, std::memory_order_relaxed); // before any queue ops
    const Order o{qty, is_buy, limit_px, trader_id, seq};

    const auto t_submit = std::chrono::steady_clock::now();
    Command c{o, t_md_read, t_submit};

    auto& q = queue_for(trader_id);
    const int idx = idx_for(trader_id);

    if (q.try_push(c)) {
        const uint64_t react_ns =
            (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t_submit - t_md_read).count();
        record_lat(reaction_lat[idx], react_ns);

        pending.fetch_add(1, std::memory_order_release);
        pending_cv.notify_one();
    } else {
        reaction_lat[idx].dropped.fetch_add(1, std::memory_order_relaxed);
        queue_lat[idx].dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---------------- engine (single owner of book) ----------------
void engine_loop() {
    std::vector<SPSCQueue<Command, QCAP>*> qs = { &q0, &q1, &q99 };
    size_t rr = 0;

    std::vector<Command> batch;
    batch.reserve(1024);

    auto should_stop = [&]() -> bool {
        if (running.load(std::memory_order_relaxed)) return false;
        if (pending.load(std::memory_order_acquire) > 0) return false;
        if (bids.empty() || asks.empty()) return true;
        return bids.top().limit_px < asks.top().limit_px;
    };

    while (true) {
        // Sleep when idle (reduces jitter vs spinning)
        if (pending.load(std::memory_order_acquire) == 0) {
            if (should_stop()) break;

            std::unique_lock<std::mutex> lk(pending_mtx);
            pending_cv.wait_for(lk, std::chrono::milliseconds(1), [&] {
                return pending.load(std::memory_order_acquire) > 0 || !running.load();
            });
        }

        // Drain a batch round-robin (bounded per-queue per cycle for fairness)
        batch.clear();

        for (size_t i = 0; i < qs.size(); ++i) {
            auto* q = qs[(rr + i) % qs.size()];
            Command c;
            int pops = 0;

            while (pops < 256 && q->try_pop(c)) {
                pending.fetch_sub(1, std::memory_order_acq_rel);

                // Queue/ingress latency: submit -> engine pop
                const auto t_pop = std::chrono::steady_clock::now();
                const uint64_t q_ns =
                    (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t_pop - c.t_submit).count();
                record_lat(queue_lat[idx_for(c.o.trader_id)], q_ns);

                batch.push_back(c);
                ++pops;
            }
        }
        rr = (rr + 1) % qs.size();

        // Normalize ordering across queues: deterministic by global seq
        std::sort(batch.begin(), batch.end(),
                  [](const Command& a, const Command& b) { return a.o.seq < b.o.seq; });

        // Apply to book (engine owns bids/asks)
        for (auto& cmd : batch) {
            if (cmd.o.is_buy) bids.push(cmd.o);
            else asks.push(cmd.o);
        }

        // Match as much as possible
        while (!bids.empty() && !asks.empty() &&
               bids.top().limit_px >= asks.top().limit_px) {

            Order buy = bids.top();  bids.pop();
            Order sell = asks.top(); asks.pop();

            const int traded_qty = std::min(buy.qty, sell.qty);
            if (traded_qty <= 0) continue;

            // Simple rule for now (you’ll probably change this later):
            // trade at sell price when crossing.
            const double trade_px = sell.limit_px;
            market_price.store(trade_px, std::memory_order_relaxed);

            {
                std::scoped_lock<std::mutex> pl(portfolios_mtx);
                portfolios[buy.trader_id].cash -= traded_qty * trade_px;
                portfolios[buy.trader_id].holdings += traded_qty;

                portfolios[sell.trader_id].cash += traded_qty * trade_px;
                portfolios[sell.trader_id].holdings -= traded_qty;
            }

            push_price(trade_px);

            buy.qty  -= traded_qty;
            sell.qty -= traded_qty;

            if (buy.qty > 0)  bids.push(buy);
            if (sell.qty > 0) asks.push(sell);

            std::cout << "Trade: " << traded_qty << " @ " << trade_px
                      << " (buyer " << buy.trader_id
                      << ", seller " << sell.trader_id << ")\n";
        }

        if (should_stop()) break;
    }
}

// ---------------- strategies ----------------
void ma_strategy(uint32_t id) {
    while (running.load(std::memory_order_relaxed)) {
        // "market event observation time" for reaction-lat measurement
        const auto t_md = std::chrono::steady_clock::now();

        const double ma = compute_ma_or_price();
        const double px = market_price.load(std::memory_order_relaxed);

        constexpr int trade_size = 1;
        constexpr double CROSS = 0.02;

        const double diff = px - ma;
        if (diff > 0.0001) {
            submit_order(false, trade_size, std::max(0.01, px - CROSS), id, t_md);
        } else if (diff < -0.0001) {
            submit_order(true, trade_size, px + CROSS, id, t_md);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Chaos provides liquidity and occasionally crosses
void chaos(uint32_t id) {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> size_dist(1, 3);
    std::bernoulli_distribution side(0.5);
    std::bernoulli_distribution cross(0.25); // 25% aggressive
    std::uniform_real_distribution<double> off(0.00, 0.05);

    while (running.load(std::memory_order_relaxed)) {
        const auto t_md = std::chrono::steady_clock::now();

        const double px = market_price.load(std::memory_order_relaxed);
        const int qty = size_dist(rng);
        const bool is_buy = side(rng);

        const double offset = off(rng);
        const bool aggressive = cross(rng);

        double limit_px;
        if (is_buy) {
            limit_px = aggressive ? (px + offset) : std::max(0.01, px - offset);
        } else {
            limit_px = aggressive ? std::max(0.01, px - offset) : (px + offset);
        }

        submit_order(is_buy, qty, limit_px, id, t_md);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

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
    // init portfolios
    portfolios[0]  = Portfolio();
    portfolios[1]  = Portfolio();
    portfolios[99] = Portfolio();

    // seed price history
    push_price(market_price.load(std::memory_order_relaxed));

    // Start engine (owns book)
    std::thread eng(engine_loop);

    // Seed initial crossing orders (through queues)
    {
        const auto t_md = std::chrono::steady_clock::now();
        submit_order(true,  1, 100.05, 0, t_md);
        submit_order(false, 1,  99.95, 1, t_md);
    }

    // Strategies
    std::thread t1(ma_strategy, 0);
    std::thread t2(ma_strategy, 1);
    std::thread t3(chaos, 99);

    std::this_thread::sleep_for(std::chrono::seconds(10));
    running.store(false, std::memory_order_relaxed);
    pending_cv.notify_all();

    t1.join();
    t2.join();
    t3.join();
    eng.join();

    const double last_px = market_price.load(std::memory_order_relaxed);

    struct Result { uint32_t id; double value; double pnl; int holdings; double cash; };
    std::vector<Result> results;

    {
        std::scoped_lock<std::mutex> pl(portfolios_mtx);
        for (auto &kv : portfolios) {
            const uint32_t id = kv.first;
            const Portfolio &p = kv.second;
            const double value = p.cash + p.holdings * last_px;
            const double pnl = value - INITIAL_CASH;
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

    // Latency stats
    print_lat_block("REACTION LATENCY (t_submit - t_md_read)", reaction_lat);
    print_lat_block("QUEUE LATENCY (t_engine_pop - t_submit)", queue_lat);

    return 0;
}
