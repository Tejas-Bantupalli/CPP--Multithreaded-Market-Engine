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
#include "engine.cpp"


template <typename T, size_t CAP>
class SPSCQueue {
    static_assert((CAP & (CAP - 1)) == 0, "CAP must be power of 2");
public:
    bool try_push(const T& v) {
        auto h = head_.load(std::memory_order_relaxed);
        auto n = (h + 1) & (CAP - 1);
        if (n == tail_.load(std::memory_order_acquire)) return false; 
        buf_[h] = v;
        head_.store(n, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return false; 
        out = buf_[t];
        tail_.store((t + 1) & (CAP - 1), std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buf_[CAP];
};



struct Command {
    Order o;
    std::chrono::steady_clock::time_point t_submit;
};
