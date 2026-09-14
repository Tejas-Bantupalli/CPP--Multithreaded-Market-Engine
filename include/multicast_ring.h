#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// Single-writer, multi-reader broadcast ring.
//
// The writer publishes each value once; every reader sees every value through
// its own cursor. The writer never waits for readers. If a reader falls a full
// ring behind it is lapped: it detects this, counts what it lost, and jumps to
// the oldest value still intact. Compare with fanning out into N SPSC queues,
// where a publish costs N pushes and N cache lines.
//
// Each slot is a small seqlock. The writer marks the slot as in-progress,
// writes the payload word by word, then stores the publish number. A reader
// checks the sequence before and after copying the payload and retries if the
// writer overtook it. Payload words are relaxed atomics, so the copy is not a
// data race under the C++ memory model; the seq stores and loads carry the
// acquire/release ordering.
//
// Publish numbers are 1-based; publish n lives in slot n & (CAP - 1).
template <typename T, size_t CAP>
class MulticastRing {
    static_assert(CAP >= 2 && (CAP & (CAP - 1)) == 0, "CAP must be a power of 2 >= 2");
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    static constexpr size_t WORDS = (sizeof(T) + 7) / 8;
    static constexpr uint64_t WRITING = uint64_t(1) << 63;

public:
    struct Reader {
        uint64_t next = 1;      // publish number this reader wants next
        uint64_t dropped = 0;   // values lost to lapping
    };

    // Writer thread only.
    void publish(const T& v) {
        const uint64_t n = head_.load(std::memory_order_relaxed) + 1;
        Slot& s = slots_[n & (CAP - 1)];
        uint64_t buf[WORDS] = {};
        std::memcpy(buf, &v, sizeof(T));
        s.seq.store(n | WRITING, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < WORDS; ++i) s.words[i].store(buf[i], std::memory_order_relaxed);
        s.seq.store(n, std::memory_order_release);
        head_.store(n, std::memory_order_release);
    }

    uint64_t published() const { return head_.load(std::memory_order_acquire); }

    // Any reader thread, with its own Reader. Returns false when nothing new is available.
    bool try_read(Reader& r, T& out) const {
        for (int attempt = 0; attempt < 4; ++attempt) {
            const uint64_t n = r.next;
            const Slot& s = slots_[n & (CAP - 1)];
            const uint64_t s1 = s.seq.load(std::memory_order_acquire);
            if (s1 == n) {
                uint64_t buf[WORDS];
                for (size_t i = 0; i < WORDS; ++i) buf[i] = s.words[i].load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (s.seq.load(std::memory_order_relaxed) == n) {
                    std::memcpy(&out, buf, sizeof(T));
                    r.next = n + 1;
                    return true;
                }
                // torn: the writer came around while we were copying, fall through to lap handling
            } else {
                const uint64_t m = s1 & ~WRITING;
                if (m < n || (m == n && (s1 & WRITING))) return false; // not published yet
            }
            // Lapped. The writer may currently be overwriting publish head+1-CAP, so the
            // oldest safe value is head+2-CAP.
            const uint64_t h = head_.load(std::memory_order_acquire);
            const uint64_t oldest = h + 2 > CAP ? h + 2 - CAP : 1;
            if (oldest > r.next) {
                r.dropped += oldest - r.next;
                r.next = oldest;
            } else {
                r.next = n + 1; // torn read of the value itself; count it as lost
                ++r.dropped;
            }
        }
        return false;
    }

private:
    struct Slot {
        std::atomic<uint64_t> seq{0};
        std::atomic<uint64_t> words[WORDS];
    };

    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) Slot slots_[CAP];
};
