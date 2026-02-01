#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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

constexpr int NUM_TRADERS = 4;
constexpr uint32_t TRADER_IDS[NUM_TRADERS] = {0, 1, 2, 99};

inline int idx_for(uint32_t id) {
    if (id == 0) return 0;
    if (id == 1) return 1;
    if (id == 2) return 2;
    return 3; // 99
}

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

struct Command {
    Order o;
    std::chrono::steady_clock::time_point t_event;
    std::chrono::steady_clock::time_point t_recv;
    std::chrono::steady_clock::time_point t_submit;
};

constexpr size_t QCAP = 1 << 12;

struct MarketEvent {
    uint64_t ev_seq = 0;
    double last_trade = std::numeric_limits<double>::quiet_NaN();
    std::chrono::steady_clock::time_point t_event{};
};

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
