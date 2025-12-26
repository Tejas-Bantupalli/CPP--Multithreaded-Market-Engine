
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
    uint64_t seq;      
};

struct BuyComp {
    bool operator()(const Order& a, const Order& b) const {
        if (a.limit_px != b.limit_px) return a.limit_px < b.limit_px;
        return a.seq > b.seq;
    }
};

struct SellComp {
    bool operator()(const Order& a, const Order& b) const {
        if (a.limit_px != b.limit_px) return a.limit_px > b.limit_px;
        return a.seq > b.seq;
    }
};

constexpr double INITIAL_CASH = 10000.0;
constexpr size_t MA_PERIOD = 5;

std::atomic<double> market_price{100.0};
std::deque<double> price_history;
std::mutex price_mtx;

std::atomic<bool> running{true};

std::priority_queue<Order, std::vector<Order>, BuyComp> bids;
std::priority_queue<Order, std::vector<Order>, SellComp> asks;
std::atomic<uint64_t> next_seq{0};

std::mutex book_mtx;
std::condition_variable book_cv;

std::unordered_map<uint32_t, Portfolio> portfolios;
std::mutex portfolios_mtx;

double compute_ma_or_price() {
    std::scoped_lock<std::mutex> lock(price_mtx);
    if (price_history.empty()) return market_price.load();

    double sum = 0.0;
    for (double p : price_history) sum += p;
    return sum / price_history.size();
}

void push_price(double px) {
    std::scoped_lock<std::mutex> lock(price_mtx);
    price_history.push_back(px);
    if (price_history.size() > MA_PERIOD) price_history.pop_front();
}

void submit_order(bool is_buy, int qty, double limit_px, uint32_t trader_id) {
    if (qty <= 0) return;
    Order o{qty, is_buy, limit_px, trader_id, next_seq.fetch_add(1, std::memory_order_relaxed)};
    {
        std::scoped_lock<std::mutex> lk(book_mtx);
        if (is_buy) bids.push(o);
        else asks.push(o);
    }
    book_cv.notify_one();
}

void matcher() {
    while (true) {
        Order buy{}, sell{};

        {
            std::unique_lock<std::mutex> lk(book_mtx);
            book_cv.wait(lk, [] {
                if (!running.load()) return true;
                if (bids.empty() || asks.empty()) return false;
                return bids.top().limit_px >= asks.top().limit_px;
            });

            if (!running.load()) {
                if (bids.empty() || asks.empty()) break;
                if (bids.top().limit_px < asks.top().limit_px) break;
            }

            if (bids.empty() || asks.empty()) continue;
            if (bids.top().limit_px < asks.top().limit_px) continue;

            buy = bids.top(); bids.pop();
            sell = asks.top(); asks.pop();
        }

        int traded_qty = std::min(buy.qty, sell.qty);
        if (traded_qty <= 0) continue;

        double trade_px = sell.limit_px;
        market_price.store(trade_px);

        {
            std::scoped_lock<std::mutex> pl(portfolios_mtx);
            portfolios[buy.trader_id].cash -= traded_qty * trade_px;
            portfolios[buy.trader_id].holdings += traded_qty;

            portfolios[sell.trader_id].cash += traded_qty * trade_px;
            portfolios[sell.trader_id].holdings -= traded_qty;
        }

        push_price(trade_px);

        buy.qty -= traded_qty;
        sell.qty -= traded_qty;

        if (buy.qty > 0 || sell.qty > 0) {
            std::scoped_lock<std::mutex> lk(book_mtx);
            if (buy.qty > 0) bids.push(buy);
            if (sell.qty > 0) asks.push(sell);
        }

        std::cout << "Trade: " << traded_qty << " @ " << trade_px
                  << " (buyer " << buy.trader_id
                  << ", seller " << sell.trader_id << ")\n";
    }
}

void ma_strategy(uint32_t id) {
    while (running.load()) {
        double ma = compute_ma_or_price();
        double px = market_price.load();

        constexpr int trade_size = 1;
        constexpr double CROSS = 0.02; 

        double diff = px - ma;
        if (diff > 0.0001) {
            submit_order(false, trade_size, std::max(0.01, px - CROSS), id);
        } else if (diff < -0.0001) {
            submit_order(true, trade_size, px + CROSS, id);
        } else {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Chaos provides liquidity and occasionally crosses
// It posts random bids/asks around current px with some chance to cross.
void chaos(uint32_t id) {
    thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> size_dist(1, 3);
    std::bernoulli_distribution side(0.5);
    std::bernoulli_distribution cross(0.25);     // 25% aggressive
    std::uniform_real_distribution<double> off(0.00, 0.05);

    while (running.load()) {
        double px = market_price.load();
        int qty = size_dist(rng);
        bool is_buy = side(rng);

        double offset = off(rng);
        bool aggressive = cross(rng);

        double limit_px;
        if (is_buy) {
            // passive bid a bit below px; aggressive bid a bit above px
            limit_px = aggressive ? (px + offset) : std::max(0.01, px - offset);
        } else {
            // passive ask a bit above px; aggressive ask a bit below px
            limit_px = aggressive ? std::max(0.01, px - offset) : (px + offset);
        }

        submit_order(is_buy, qty, limit_px, id);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main() {
    portfolios[0]  = Portfolio();
    portfolios[1]  = Portfolio();
    portfolios[99] = Portfolio();

    push_price(market_price.load());

    submit_order(true,  1, 100.05, 0);
    submit_order(false, 1,  99.95, 1);

    std::thread t1(ma_strategy, 0);
    std::thread t2(ma_strategy, 1);
    std::thread t3(chaos, 99);
    std::thread match_thread(matcher);

    std::this_thread::sleep_for(std::chrono::seconds(10));
    running.store(false);
    book_cv.notify_all();

    t1.join();
    t2.join();
    t3.join();
    match_thread.join();

    double last_px = market_price.load();

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

    return 0;
}
