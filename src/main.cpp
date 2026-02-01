#include "engine.h"
#include "engine_state.h"
#include "market_types.h"

#include <algorithm>
#include <iostream>
#include <thread>
#include <vector>

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
