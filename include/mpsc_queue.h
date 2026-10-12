#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Bounded multi-producer single-consumer ring (Vyukov's bounded queue, with the
// consumer side simplified for a single reader). Every cell carries a sequence
// number, so producers claim a slot with one CAS on the shared tail and then
// publish it independently.
//
// The trade against SPSCQueue:
//   - push is a CAS on a cache line that every producer writes, so it costs more
//     and gets slower as producers are added;
//   - the queue itself defines one global arrival order, so there is nothing to
//     merge on the consumer side and one place to poll;
//   - a producer that is preempted between claiming a cell and publishing it
//     blocks the consumer, because cells are consumed strictly in claim order.
//     try_pop counts the polls that hit that case in head_stalls().
template <typename T, size_t CAP>
class MPSCQueue {
    static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0, "CAP must be a power of 2 >= 2");
public:
    MPSCQueue() {
        for (size_t i = 0; i < CAP; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    bool try_push(const T& v) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        Cell* c;
        for (;;) {
            c = &cells_[pos & (CAP - 1)];
            const size_t seq = c->seq.load(std::memory_order_acquire);
            const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            if (dif == 0) {
                // The cell is free for this lap: claim it. On failure pos is reloaded.
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (dif < 0) {
                return false; // full: the consumer has not freed this cell from the last lap
            } else {
                pos = tail_.load(std::memory_order_relaxed); // another producer got here first
            }
        }
        c->v = v;
        c->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Single consumer only.
    bool try_pop(T& out) {
        Cell& c = cells_[head_ & (CAP - 1)];
        if (c.seq.load(std::memory_order_acquire) != head_ + 1) {
            // Not ready. If the tail has moved past us, a producer claimed this cell
            // and has not published it yet: everything behind it waits.
            if (tail_.load(std::memory_order_relaxed) != head_) ++head_stalls_;
            return false;
        }
        out = c.v;
        c.seq.store(head_ + CAP, std::memory_order_release);
        ++head_;
        return true;
    }

    // Consumer-side counter; read it after the consumer has stopped.
    uint64_t head_stalls() const { return head_stalls_; }

private:
    struct Cell {
        std::atomic<size_t> seq;
        T v;
    };
    alignas(64) std::atomic<size_t> tail_{0}; // producers
    alignas(64) size_t head_ = 0;             // consumer
    uint64_t head_stalls_ = 0;
    alignas(64) Cell cells_[CAP];
};
