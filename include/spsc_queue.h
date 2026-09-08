#pragma once

#include <atomic>
#include <cstddef>

template <typename T, size_t CAP>
class SPSCQueue {
    static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0, "CAP must be a power of 2 >= 2");
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
