#include <deque>
#include <mutex>
#include "engine.h"
#include <thread>

std::deque<double> price_history;
std::mutex price_mtx;
const size_t MA_PERIOD = 5;

void ma_strategy(uint32_t id) {
    while (true) {
        double ma = 0.0;
        {
            std::lock_guard<std::mutex> lock(price_mtx);
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
            std::lock_guard<std::mutex> lock(sell_mtx);
            sell_queue.push(o);
        } else if (current_price < ma) {
            o.volume = trade_size; 
            std::lock_guard<std::mutex> lock(buy_mtx);
            buy_queue.push(o);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
