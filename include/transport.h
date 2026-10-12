#pragma once

#include "mpsc_queue.h"
#include "spsc_queue.h"
#include "types.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Interchangeable channels for transport experiments. All have QCAP-1 usable
// slots (mpsc: QCAP, shared); only synchronization and waiting differ. SPSC is
// the default.

template <typename T>
struct Channel {
    virtual ~Channel() = default;

    // Producer side. False means full; the caller counts the drop.
    virtual bool push(const T& v) = 0;

    // Consumer side. Never blocks.
    virtual bool pop(T& out) = 0;

    // Consumer side. Blocking transports sleep until an item arrives or the
    // timeout expires. Polling transports just try once, so callers can use
    // this uniformly and let the transport decide how to wait.
    virtual bool wait_pop(T& out, int64_t /*timeout_ns*/) { return pop(out); }

    // True if wait_pop actually parks the thread. The agent runner uses this to
    // decide between its idle policy and letting the transport do the waiting.
    virtual bool blocking() const { return false; }

    virtual const char* name() const = 0;
};

// ---------------------------------------------------------------- spsc
// Lock-free single-producer single-consumer ring. One writer, one reader, two
// atomic indices, no kernel involvement on any path. The default.
template <typename T>
class SpscChannel final : public Channel<T> {
public:
    bool push(const T& v) override { return q_.try_push(v); }
    bool pop(T& out) override { return q_.try_pop(out); }
    const char* name() const override { return "spsc"; }

private:
    SPSCQueue<T, QCAP> q_;
};

// ---------------------------------------------------------------- mpsc
// One lock-free queue shared by every agent that picks it. Each agent gets its
// own handle, but all handles push into the same ring and the engine drains it
// once instead of polling one ring per agent. Only agent -> engine traffic can be
// multi-producer; engine -> agent always has one producer, so that direction
// stays SPSC (the engine wires it that way).
template <typename T>
class MpscChannel final : public Channel<T> {
public:
    using Queue = MPSCQueue<T, QCAP>;

    // A standalone channel owns its own queue; the engine passes a shared one.
    MpscChannel() : q_(std::make_shared<Queue>()) {}
    explicit MpscChannel(std::shared_ptr<Queue> q) : q_(std::move(q)) {}

    bool push(const T& v) override { return q_->try_push(v); }
    bool pop(T& out) override { return q_->try_pop(out); }
    const char* name() const override { return "mpsc"; }

private:
    std::shared_ptr<Queue> q_;
};

// ---------------------------------------------------------------- mutex
// A mutex-guarded ring of the same capacity, polled by the consumer. Storage is
// preallocated exactly as above, so the only thing this measures against spsc is
// the cost of the lock itself: the atomic exchange when uncontended, and the
// contention when the engine and the agent reach for it at once.
template <typename T>
class MutexChannel : public Channel<T> {
public:
    // The lock-free ring keeps one slot free to tell full from empty, so it holds
    // QCAP-1. Match that exactly: the variants must differ in synchronisation,
    // not in how much they can buffer.
    static constexpr size_t USABLE = QCAP - 1;

    MutexChannel() : buf_(QCAP) {}

    bool push(const T& v) override {
        std::lock_guard<std::mutex> g(m_);
        if (count_ == USABLE) return false;
        buf_[tail_] = v;
        tail_ = (tail_ + 1) & (QCAP - 1);
        ++count_;
        return true;
    }

    bool pop(T& out) override {
        std::lock_guard<std::mutex> g(m_);
        return pop_locked(out);
    }

    const char* name() const override { return "mutex"; }

protected:
    // Caller holds m_; shared by polling and blocking consumers.
    bool pop_locked(T& out) {
        if (count_ == 0) return false;
        out = buf_[head_];
        head_ = (head_ + 1) & (QCAP - 1);
        --count_;
        return true;
    }

    std::mutex m_;
    std::vector<T> buf_;
    size_t head_ = 0, tail_ = 0, count_ = 0;
};

// ---------------------------------------------------------------- mutex_cv
// Mutex plus condition variable: the consumer parks instead of polling, and the
// producer signals. This is the version most people write first. It is correct,
// it parks while idle; scheduler wakeups add latency.
template <typename T>
class MutexCvChannel final : public MutexChannel<T> {
    using Base = MutexChannel<T>;

public:
    bool push(const T& v) override {
        const bool ok = Base::push(v); // releases the lock before notification
        if (ok) cv_.notify_one();
        return ok;
    }

    bool wait_pop(T& out, int64_t timeout_ns) override {
        std::unique_lock<std::mutex> g(this->m_);
        if (this->count_ == 0) {
            cv_.wait_for(g, std::chrono::nanoseconds(timeout_ns),
                         [this] { return this->count_ != 0; });
        }
        return this->pop_locked(out);
    }

    bool blocking() const override { return true; }
    const char* name() const override { return "mutex_cv"; }

    // Let the shutdown path wake a parked consumer so it can observe the flag.
    void wake_all() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
};

// ---------------------------------------------------------------- selection
enum class TransportKind { Spsc, Mutex, MutexCv, Mpsc };

inline bool parse_transport(const std::string& s, TransportKind& out) {
    if (s == "spsc") { out = TransportKind::Spsc; return true; }
    if (s == "mutex") { out = TransportKind::Mutex; return true; }
    if (s == "mutex_cv" || s == "mutexcv") { out = TransportKind::MutexCv; return true; }
    if (s == "mpsc") { out = TransportKind::Mpsc; return true; }
    return false;
}

inline const char* transport_name(TransportKind k) {
    switch (k) {
        case TransportKind::Spsc: return "spsc";
        case TransportKind::Mutex: return "mutex";
        case TransportKind::MutexCv: return "mutex_cv";
        case TransportKind::Mpsc: return "mpsc";
    }
    return "spsc";
}

template <typename T>
std::unique_ptr<Channel<T>> make_channel(TransportKind k) {
    switch (k) {
        case TransportKind::Mutex: return std::make_unique<MutexChannel<T>>();
        case TransportKind::MutexCv: return std::make_unique<MutexCvChannel<T>>();
        case TransportKind::Mpsc: return std::make_unique<MpscChannel<T>>(); // standalone; see Engine
        case TransportKind::Spsc: break;
    }
    return std::make_unique<SpscChannel<T>>();
}
