#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <chrono>

struct Portfolio {
    double cash = 10000.0;
    int holdings = 0;
};

struct Order {
    int volume;       
    uint32_t thread_id;
};

std::atomic<double> market_price{100.0};
std::deque<double> price_history;
std::mutex price_mtx;

std::queue<Order> buy_queue;
std::queue<Order> sell_queue;
std::mutex buy_mtx, sell_mtx;

std::unordered_map<uint32_t, Portfolio> portfolios;

const size_t MA_PERIOD = 5;

void matcher() {
    while (true) {
        Order buy_order, sell_order;
        {
            std::scoped_lock<std::mutex> bl(buy_mtx);
            std::scoped_lock<std::mutex> sl(sell_mtx);
            if (buy_queue.empty() || sell_queue.empty()) continue;
            buy_order = buy_queue.front(); buy_queue.pop();
            sell_order = sell_queue.front(); sell_queue.pop();
        }

        int traded_qty = std::min(buy_order.volume, -sell_order.volume);
        double trade_price = market_price.load();

        portfolios[buy_order.thread_id].cash -= traded_qty * trade_price;
        portfolios[buy_order.thread_id].holdings += traded_qty;

        portfolios[sell_order.thread_id].cash += traded_qty * trade_price;
        portfolios[sell_order.thread_id].holdings -= traded_qty;

        market_price.store(trade_price);

        {
            std::scoped_lock<std::mutex> lock(price_mtx);
            price_history.push_back(trade_price);
            if (price_history.size() > MA_PERIOD) price_history.pop_front();
        }

        std::cout << "Trade: " << traded_qty << " @ " << trade_price << "\n";
    }
}

void ma_strategy(uint32_t id) {
    while (true) {
        double ma = 0.0;
        {
            std::scoped_lock<std::mutex> lock(price_mtx);
            if (!price_history.empty()) {
                for (auto p : price_history) ma += p;
                ma /= price_history.size();
            }
        }

        Order o;
        o.thread_id = id;
        int trade_size = 1;

        double current_price = market_price.load();
        if (current_price > ma) {
            o.volume = -trade_size; 
            std::scoped_lock<std::mutex> lock(sell_mtx);
            sell_queue.push(o);
        } else if (current_price < ma) {
            o.volume = trade_size; 
            std::scoped_lock<std::mutex> lock(buy_mtx);
            buy_queue.push(o);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int main() {
    portfolios[0] = Portfolio();
    portfolios[1] = Portfolio();

    std::thread t1(ma_strategy, 0);
    std::thread t2(ma_strategy, 1);
    std::thread match_thread(matcher);

    t1.join();
    t2.join();
    match_thread.join();
}
