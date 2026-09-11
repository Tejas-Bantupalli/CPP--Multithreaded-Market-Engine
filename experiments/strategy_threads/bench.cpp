// Does a strategy that spreads itself over several threads get faster?
//
// One probe strategy runs inside the real engine in one of these arrangements:
//
//   single   one thread polls the ring, computes, submits.
//   forward  a reader thread polls the ring and forwards every event over an
//            internal SPSC queue; a worker thread computes and submits.
//   split    as forward, but the reader also does the first half of the
//            calculation, so the two threads form a two-stage pipeline.
//
// The queue between reader and worker is one of:
//
//   spsc      lock-free ring; the worker spins on it.
//   mutex_cv  mutex-guarded ring; the worker parks on a condition variable.
//   futex     the same lock-free ring plus a compare-and-wait on an address
//             (os_sync_wait_on_address, macOS's public futex). The worker
//             parks; the reader makes a syscall only when the worker is parked.
//
// `single --burner` adds a thread that spins on an empty queue and does nothing
// else: the core footprint of a two-thread strategy without the handoff.
//
// and, independently, where its housekeeping (periodic statistics over recent
// decisions) runs: nowhere, inline on the trading thread, or on its own thread
// fed by another SPSC queue.
//
// macOS cannot pin threads or report where they ran. Every probe thread instead
// times a fixed 1000-iteration calculation about every 10 ms: ~1.6 us means a
// performance core at full speed, ~2.4 us an efficiency core. Placement becomes
// a measured variable rather than hidden noise.
//
// Ownership rules the threaded versions keep:
//   - The reader is the only consumer of the private channel and the only user
//     of the multicast cursor (AgentContext::poll).
//   - The worker is the only thread that calls ctx.apply / ctx.submit, so it is
//     the single producer on the order SPSC and the only writer of the context's
//     books and stats. poll() and apply()/submit() touch disjoint fields.
//   - Child threads are joined inside run(), before the engine reads the context.
//
// Workload. The book is preloaded with a ladder of one-lot asks. A taker lifts
// one per step at a fixed offered rate (smooth, or in bursts at the same
// average). Every lift is exactly one Trade broadcast, so the probe sees the
// same Trade sequence in every run regardless of how the engine batches. The
// probe answers each Trade with a one-lot IOC buy priced far below the ladder:
// it goes through real matching and acknowledgement but never trades, so the
// probe's decisions cannot change its own input. That lets every run be checked
// against a reference replay: same decisions, same housekeeping output.

#include "engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <condition_variable>
#include <mutex>

#include <sys/resource.h>
#include <time.h>
#ifdef __APPLE__
#include <os/os_sync_wait_on_address.h>
#include <pthread/qos.h>
#endif

namespace {

// ------------------------------------------------------------------ config
enum class Topology { Single, Forward, Split };
enum class Housekeeping { None, Inline, Thread };
enum class QueueKind { Spsc, MutexCv, Futex };

struct Config {
    Topology topology = Topology::Single;
    Housekeeping hk = Housekeeping::None;
    QueueKind queue = QueueKind::Spsc;  // forward and split only
    bool burner = false;                // single only
    int64_t work = 0;         // hash iterations per trade, total across stages
    bool burst = false;
    int burst_size = 32;
    double rate = 4000;       // average offered trades per second
    bool spin = true;         // probe trading threads: spin or yield when idle
    int hk_window = 1024;     // decisions each statistics pass sorts
    int hk_every = 256;       // decisions between statistics passes
    int steps = 16000;        // trades per session; multiple of LEVEL_ORDERS
};

const char* topology_name(Topology t) {
    switch (t) {
        case Topology::Single: return "single";
        case Topology::Forward: return "forward";
        case Topology::Split: return "split";
    }
    return "?";
}

const char* queue_name(QueueKind q) {
    switch (q) {
        case QueueKind::Spsc: return "spsc";
        case QueueKind::MutexCv: return "mutex_cv";
        case QueueKind::Futex: return "futex";
    }
    return "?";
}

const char* hk_name(Housekeeping h) {
    switch (h) {
        case Housekeeping::None: return "none";
        case Housekeeping::Inline: return "inline";
        case Housekeeping::Thread: return "thread";
    }
    return "?";
}

constexpr Price LADDER_BASE = 10001;
constexpr int LEVEL_ORDERS = 64;
constexpr Price TAKER_LIMIT = 10450;  // inside the 5% collar, above the whole ladder
constexpr Price PROBE_BASE = 9700;    // probe bids sit far below every ask
constexpr int64_t LADDER_WINDOW = 1024;  // maker keeps at most this many unacked orders

bool g_interactive = false;  // --qos; applies to every thread in the session

void set_thread_qos() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(g_interactive ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED, 0);
#endif
}

int64_t thread_cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

inline void idle(bool spin) {
    if (!spin) std::this_thread::yield();
}

// ------------------------------------------------------------------ the calculation
// A dependent chain: each iteration needs the previous one, so it cannot be
// vectorised or skipped, and cost scales linearly with the iteration count.
inline uint64_t chain(uint64_t x, int64_t iters) {
    for (int64_t i = 0; i < iters; ++i) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        x ^= x >> 29;
    }
    return x;
}

Price ladder_px(int k) { return LADDER_BASE + k / LEVEL_ORDERS; }

// Best ask quantity immediately after the k-th lift.
Qty ask_qty_after(int k, int steps) {
    const int left = LEVEL_ORDERS - 1 - k % LEVEL_ORDERS;
    if (left > 0) return left;
    return k + 1 < steps ? LEVEL_ORDERS : 0;
}

// Stage A: features from one market event.
inline uint64_t stage_a(int k, Price px, Qty ask_qty, int64_t iters) {
    return chain((uint64_t(k) << 32) ^ (uint64_t(px) << 8) ^ uint64_t(ask_qty), iters);
}

// Stage B: fold features into running state and pick a price.
struct Signal {
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    Price decide(uint64_t feature, int64_t iters) {
        state = chain(state ^ feature, iters);
        return PROBE_BASE + Price(state & 255);
    }
};

inline uint64_t fold(uint64_t sum, uint64_t v) { return (sum ^ v) * 1099511628211ULL; }

// splitmix64 finaliser. With no work configured a feature is nearly the step
// number, and sorting already-sorted input is close to free.
inline uint64_t scramble(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// Periodic statistics over recent decisions: copy a window, sort it, read
// percentiles. The expensive-but-not-urgent work a desk runs alongside trading.
class Housekeeper {
public:
    Housekeeper(int window, int every) : window_(window), every_(every), ring_(window), scratch_(window) {}

    void add(uint64_t feature) {
        ring_[n_ % window_] = scramble(feature);
        ++n_;
        if (n_ % every_ == 0) summarize();
    }

    uint64_t checksum() const { return checksum_; }
    uint64_t passes() const { return passes_; }

private:
    void summarize() {
        const size_t m = std::min<uint64_t>(n_, window_);
        std::copy(ring_.begin(), ring_.begin() + m, scratch_.begin());
        std::sort(scratch_.begin(), scratch_.begin() + m);
        checksum_ = fold(fold(checksum_, scratch_[m / 2]), scratch_[m * 99 / 100]);
        ++passes_;
    }

    size_t window_, every_;
    std::vector<uint64_t> ring_, scratch_;
    uint64_t n_ = 0, checksum_ = 0, passes_ = 0;
};

struct Reference {
    uint64_t decisions = 0;
    uint64_t housekeeping = 0;
};

// The same calculation replayed on the known Trade sequence, single-threaded,
// with no engine. Every valid session must reproduce it exactly.
Reference replay(const Config& cfg) {
    const int64_t wa = cfg.work / 2, wb = cfg.work - cfg.work / 2;
    Signal sig;
    Housekeeper hk(cfg.hk_window, cfg.hk_every);
    Reference r;
    for (int k = 0; k < cfg.steps; ++k) {
        const uint64_t f = stage_a(k, ladder_px(k), ask_qty_after(k, cfg.steps), wa);
        r.decisions = fold(r.decisions, uint64_t(sig.decide(f, wb)));
        hk.add(f);
    }
    r.housekeeping = hk.checksum();
    return r;
}

// ------------------------------------------------------------------ measurement
struct Dist {
    std::vector<int64_t> v;
    void reserve(size_t n) { v.reserve(n); }
    void add(int64_t x) { v.push_back(x); }
};

std::string dist_json(Dist d) {
    auto& v = d.v;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) -> int64_t {
        if (v.empty()) return 0;
        size_t rank = size_t(std::ceil(p * double(v.size())));
        return v[std::min(v.size(), std::max<size_t>(rank, 1)) - 1];
    };
    double mean = 0;
    for (auto x : v) mean += double(x);
    if (!v.empty()) mean /= double(v.size());
    std::ostringstream os;
    os << "{\"count\":" << v.size() << ",\"mean\":" << int64_t(mean) << ",\"p50\":" << pct(0.50)
       << ",\"p90\":" << pct(0.90) << ",\"p99\":" << pct(0.99) << ",\"p999\":" << pct(0.999)
       << ",\"max\":" << (v.empty() ? 0 : v.back()) << "}";
    return os.str();
}

// ------------------------------------------------------------------ core speed
// Times the same calculation the strategy runs, at a fixed length, on whatever
// core the thread currently has. Rate limited; callers check it rarely.
constexpr int64_t SPEED_ITERS = 1000;
constexpr Ts SPEED_EVERY_NS = 10'000'000;
constexpr Ts SPEED_SLOW_NS = 2300;  // M1: P core ~1650 cold to ~2050 hot, E core ~2420

__attribute__((noinline)) uint64_t speed_work(uint64_t x) { return chain(x, SPEED_ITERS); }

struct Speedometer {
    Dist samples;
    uint64_t slow = 0;
    uint64_t x = 1;
    Ts next = 0;
    uint32_t calls = 0;

    Speedometer() { samples.reserve(4096); }

    // Cheap enough for an idle loop: a counter, and a clock read every 64 calls.
    void tick() {
        if ((++calls & 63) != 0) return;
        const Ts t0 = now_ns();
        if (t0 < next) return;
        x = speed_work(x);
        const Ts dt = now_ns() - t0;
        samples.add(dt);
        slow += dt > SPEED_SLOW_NS;
        next = t0 + SPEED_EVERY_NS;
    }
};

std::string speed_json(const Speedometer& s) {
    std::ostringstream os;
    os << "{\"slow\":" << s.slow << ",\"ns_per_1000\":" << dist_json(s.samples) << "}";
    return os.str();
}

// ------------------------------------------------------------------ reader -> worker queues
// Same capacity and payload in all three; they differ only in how the worker
// waits and how the reader tells it there is work. Counters are single-writer:
// `wakes` belongs to the reader, `parks` to the worker.

template <typename T>
class SpinQueue {
public:
    static constexpr bool BLOCKING = false;
    bool push(const T& v) { return q_.try_push(v); }
    bool try_pop(T& out) { return q_.try_pop(out); }
    bool wait_pop(T& out, int64_t) { return q_.try_pop(out); }
    void close() {}
    uint64_t wakes = 0, parks = 0, lost_wakeups = 0;

private:
    SPSCQueue<T, QCAP> q_;
};

// Mutex + condition variable, as in include/transport.h (MutexCvChannel) with
// counters. Notifies on every push, which is how it is usually written.
template <typename T>
class MutexCvQueue {
public:
    static constexpr bool BLOCKING = true;
    static constexpr size_t USABLE = QCAP - 1;  // match the lock-free ring

    MutexCvQueue() : buf_(QCAP) {}

    bool push(const T& v) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (count_ == USABLE) return false;
            buf_[tail_] = v;
            tail_ = (tail_ + 1) & (QCAP - 1);
            ++count_;
        }
        cv_.notify_one();
        ++wakes;
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(m_);
        return take(out);
    }

    bool wait_pop(T& out, int64_t timeout_ns) {
        std::unique_lock<std::mutex> g(m_);
        if (count_ == 0) {
            ++parks;
            cv_.wait_for(g, std::chrono::nanoseconds(timeout_ns), [this] { return count_ != 0 || closed_; });
        }
        return take(out);
    }

    void close() {
        {
            std::lock_guard<std::mutex> g(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    uint64_t wakes = 0, parks = 0, lost_wakeups = 0;  // the predicate wait cannot miss one

private:
    bool take(T& out) {
        if (count_ == 0) return false;
        out = buf_[head_];
        head_ = (head_ + 1) & (QCAP - 1);
        --count_;
        return true;
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::vector<T> buf_;
    size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
};

// Lock-free ring plus a futex-style park. The worker announces it is about to
// sleep, re-checks the ring, and only then waits on `sleeping_` still being 1.
// The reader publishes into the ring, then checks `sleeping_`. Both sides store
// before they load, so at least one sees the other: either the worker finds the
// item, or the reader sees the flag, clears it and wakes. Clearing it first means
// a worker that has not reached the wait yet returns from it immediately.
//
// "Store then load sees the other side" only holds if each store is visible to
// the other core before the load that follows it. Without the fences below it was
// not: in about 1% of parks the worker slept with an Ack queued until the next
// trade arrived (order->ack p99 pinned at the 250 us trade spacing). The fence
// (`dmb ish` on arm64) is the same barrier multicast_ring.h uses.
template <typename T>
class FutexQueue {
public:
    static constexpr bool BLOCKING = true;

    bool push(const T& v) {
        if (!q_.try_push(v)) return false;
        std::atomic_thread_fence(std::memory_order_release);
        if (sleeping_.load(std::memory_order_acquire) != 0 &&
            sleeping_.exchange(0) != 0) {
            os_sync_wake_by_address_any(&sleeping_, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
            ++wakes;
        }
        return true;
    }

    bool try_pop(T& out) { return q_.try_pop(out); }

    bool wait_pop(T& out, int64_t timeout_ns) {
        if (q_.try_pop(out)) return true;
        sleeping_.store(1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        if (q_.try_pop(out)) {
            sleeping_.store(0, std::memory_order_release);
            return true;
        }
        ++parks;
        const int rc = os_sync_wait_on_address_with_timeout(&sleeping_, 1, sizeof(uint32_t),
                                                            OS_SYNC_WAIT_ON_ADDRESS_NONE,
                                                            OS_CLOCK_MACH_ABSOLUTE_TIME, uint64_t(timeout_ns));
        const bool timed_out = rc < 0 && errno == ETIMEDOUT;
        sleeping_.store(0, std::memory_order_release);
        const bool got = q_.try_pop(out);
        // Slept the whole backstop with work waiting: the handshake missed a wake.
        if (timed_out && got) ++lost_wakeups;
        return got;
    }

    void close() {
        sleeping_.store(0, std::memory_order_release);
        os_sync_wake_by_address_any(&sleeping_, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }

    uint64_t wakes = 0, parks = 0, lost_wakeups = 0;

private:
    SPSCQueue<T, QCAP> q_;
    alignas(64) std::atomic<uint32_t> sleeping_{0};
};

// ------------------------------------------------------------------ market participants
struct Shared {
    std::atomic<bool> ladder_ready{false};
    std::atomic<bool> probe_done{false};
    Engine* engine = nullptr;
};

// Rests `steps` one-lot asks, a bounded window at a time so neither of its
// queues can overflow, then just drains its fills.
class Maker final : public Strategy {
public:
    Maker(const Config& cfg, Shared& sh) : steps_(cfg.steps), sh_(sh) {}
    const char* name() const override { return "maker"; }
    void on_event(const Event&, AgentContext&) override {}

    void run(AgentContext& ctx, const std::atomic<bool>& running) override {
        set_thread_qos();
        Event ev;
        while (running.load(std::memory_order_relaxed)) {
            bool busy = false;
            while (ctx.poll(ev)) {
                ctx.apply(ev);
                busy = true;
                if (ev.kind == EventKind::Ack) ++acks;
                if (ev.kind == EventKind::Rejected) ++errors;
                if (ev.kind == EventKind::Fill) ++fills;
            }
            while (submitted < steps_ && submitted - acks < LADDER_WINDOW) {
                if (!ctx.submit(Side::Sell, ladder_px(int(submitted)), 1)) { ++errors; break; }
                ++submitted;
                busy = true;
            }
            if (acks == steps_ && !sh_.ladder_ready.load(std::memory_order_relaxed))
                sh_.ladder_ready.store(true, std::memory_order_release);
            if (!busy) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    int64_t submitted = 0, acks = 0, fills = 0, errors = 0;

private:
    int64_t steps_;
    Shared& sh_;
};

// Lifts the ladder on an open-loop schedule: a late step does not delay the
// ones after it. It sleeps most of each gap and spins only the tail, so it does
// not take a whole core away from the threads being measured. macOS oversleeps
// by roughly 30% of the request, hence sleeping 60% of the remaining gap.
class Taker final : public Strategy {
public:
    Taker(const Config& cfg, Shared& sh) : cfg_(cfg), sh_(sh) { lateness.reserve(cfg.steps); }
    const char* name() const override { return "taker"; }
    void on_event(const Event&, AgentContext&) override {}

    void run(AgentContext& ctx, const std::atomic<bool>& running) override {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        struct CpuOnExit {
            Taker& t; int64_t cpu0;
            ~CpuOnExit() { t.cpu_ns = thread_cpu_ns() - cpu0; }
        } cpu_on_exit{*this, cpu0};
        Event ev;
        auto drain = [&] {
            while (ctx.poll(ev)) {
                ctx.apply(ev);
                if (ev.kind == EventKind::Fill) ++fills;
                if (ev.kind == EventKind::Rejected) ++errors;
            }
        };
        while (running.load(std::memory_order_relaxed) && !sh_.ladder_ready.load(std::memory_order_acquire)) {
            drain();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const Ts t0 = now_ns() + 50'000'000;  // let the ladder's book updates settle
        const double period_ns = 1e9 / cfg_.rate;
        int i = 0;
        while (running.load(std::memory_order_relaxed)) {
            drain();
            if (i >= cfg_.steps) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            const int slot = cfg_.burst ? (i / cfg_.burst_size) * cfg_.burst_size : i;
            const Ts due = t0 + Ts(slot * period_ns);
            const Ts now = now_ns();
            if (now >= due) {
                if (ctx.submit(Side::Buy, TAKER_LIMIT, 1, TimeInForce::IOC)) {
                    lateness.add(now - due);
                    ++i;
                } else {
                    ++retries;
                }
            } else if (due - now > 60'000) {
                std::this_thread::sleep_for(std::chrono::nanoseconds((due - now) * 6 / 10));
            }
        }
    }

    int64_t fills = 0, errors = 0, retries = 0, cpu_ns = 0;
    Dist lateness;

private:
    const Config& cfg_;
    Shared& sh_;
};

// ------------------------------------------------------------------ the probe
class Probe final : public Strategy {
public:
    Probe(const Config& cfg, Shared& sh)
        : cfg_(cfg), sh_(sh), wa_(cfg.work / 2), wb_(cfg.work - cfg.work / 2),
          hk_(cfg.hk_window, cfg.hk_every), submit_ts_(size_t(cfg.steps) + 2, 0) {
        for (Dist* d : {&read_delay, &handoff, &compute, &event_to_order, &order_to_ack, &ack_read,
                        &worker_run, &ack_handoff})
            d->reserve(size_t(cfg.steps));
    }

    const char* name() const override { return "probe"; }
    void on_event(const Event&, AgentContext&) override {}

    void run(AgentContext& ctx, const std::atomic<bool>& running) override {
        if (cfg_.hk == Housekeeping::Thread) hk_thread_ = std::thread([this] { housekeeping_loop(); });
        std::atomic<bool> burner_stop{false};
        std::thread burner;
        if (cfg_.burner) burner = std::thread([&] { burner_loop(burner_stop); });

        if (cfg_.topology == Topology::Single) {
            run_single(ctx, running);
        } else {
            switch (cfg_.queue) {
                case QueueKind::Spsc: run_pipeline(std::make_unique<SpinQueue<Msg>>(), ctx, running); break;
                case QueueKind::MutexCv: run_pipeline(std::make_unique<MutexCvQueue<Msg>>(), ctx, running); break;
                case QueueKind::Futex: run_pipeline(std::make_unique<FutexQueue<Msg>>(), ctx, running); break;
            }
        }

        if (burner.joinable()) {
            burner_stop.store(true, std::memory_order_release);
            burner.join();
        }
        if (hk_thread_.joinable()) {
            hk_stop_.store(true, std::memory_order_release);
            hk_thread_.join();
        }
    }

    // Results, read after the engine has joined this agent.
    uint64_t trades = 0, orders = 0, acks = 0, errors = 0, decisions = 0;
    uint64_t internal_full = 0, hk_full = 0, max_worker_run = 0, wakes = 0, parks = 0, lost_wakeups = 0;
    int64_t cpu_reader_ns = 0, cpu_worker_ns = 0, cpu_hk_ns = 0, cpu_burner_ns = 0;
    Dist read_delay, handoff, compute, event_to_order, order_to_ack, ack_read, worker_run, ack_handoff;
    Speedometer speed_reader, speed_worker, speed_burner;  // one writer each

    uint64_t hk_checksum() const { return hk_.checksum(); }
    uint64_t hk_passes() const { return hk_.passes(); }

private:
    struct Msg {
        Event ev;
        Ts t_read = 0;          // when the reading thread took it off the ring or channel
        uint64_t feature = 0;   // stage A result, split topology only
    };

    // Everything that happens on the thread that owns the context.
    void handle(const Msg& m, Ts t_take, AgentContext& ctx) {
        const Event& ev = m.ev;
        ctx.apply(ev);
        switch (ev.kind) {
            case EventKind::Trade: {
                const int k = int(trades++);
                if (k >= cfg_.steps || ev.px != ladder_px(k) || ev.ask_qty != ask_qty_after(k, cfg_.steps)) {
                    ++errors;
                    return;
                }
                const uint64_t f = cfg_.topology == Topology::Split ? m.feature : stage_a(k, ev.px, ev.ask_qty, wa_);
                const Price px = signal_.decide(f, wb_);
                decisions = fold(decisions, uint64_t(px));
                const Ts t_submit = now_ns();
                const OrderId id = ctx.submit(Side::Buy, px, 1, TimeInForce::IOC);
                if (!id) { ++errors; return; }
                ++orders;
                submit_ts_[id & ((OrderId(1) << 40) - 1)] = t_submit;
                read_delay.add(m.t_read - ev.ts);
                handoff.add(t_take - m.t_read);
                compute.add(t_submit - t_take);
                event_to_order.add(t_submit - ev.ts);
                record_housekeeping(f);
                break;
            }
            case EventKind::Ack: {
                const Ts t = t_take;
                const uint64_t seq = ev.order_id & ((OrderId(1) << 40) - 1);
                if (seq >= submit_ts_.size() || submit_ts_[seq] == 0) { ++errors; return; }
                order_to_ack.add(t - submit_ts_[seq]);
                ack_handoff.add(t_take - m.t_read);
                ack_read.add(m.t_read - submit_ts_[seq]);
                submit_ts_[seq] = 0;
                if (++acks == uint64_t(cfg_.steps)) {
                    sh_.probe_done.store(true, std::memory_order_release);
                    sh_.engine->request_stop();
                }
                break;
            }
            case EventKind::Rejected:
            case EventKind::Fill:
            case EventKind::Cancelled:
                ++errors;
                break;
            default:
                break;
        }
    }

    void record_housekeeping(uint64_t f) {
        switch (cfg_.hk) {
            case Housekeeping::None: break;
            case Housekeeping::Inline: hk_.add(f); break;
            case Housekeeping::Thread:
                // No loss allowed: a full queue pushes back on the trading thread, and that is counted.
                while (!hk_q_->try_push(f)) ++hk_full;
                break;
        }
    }

    void run_single(AgentContext& ctx, const std::atomic<bool>& running) {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        Msg m;
        while (running.load(std::memory_order_relaxed)) {
            if (ctx.poll(m.ev)) {
                m.t_read = now_ns();
                handle(m, m.t_read, ctx);
            } else {
                speed_reader.tick();
                idle(cfg_.spin);
            }
        }
        cpu_reader_ns = thread_cpu_ns() - cpu0;
    }

    // Occupies a core exactly as an idle spinning worker would, and nothing else.
    void burner_loop(const std::atomic<bool>& stop) {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        SPSCQueue<uint64_t, 2> empty;
        uint64_t v;
        while (!stop.load(std::memory_order_relaxed)) {
            if (!empty.try_pop(v)) speed_burner.tick();
        }
        cpu_burner_ns = thread_cpu_ns() - cpu0;
    }

    template <typename Q>
    void run_pipeline(std::unique_ptr<Q> qp, AgentContext& ctx, const std::atomic<bool>& running) {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        Q& q = *qp;
        std::atomic<bool> reader_done{false};
        std::thread worker([&] { worker_loop(q, ctx, reader_done); });

        const bool split = cfg_.topology == Topology::Split;
        int k = 0;
        Msg m;
        while (running.load(std::memory_order_relaxed)) {
            if (!ctx.poll(m.ev)) {
                speed_reader.tick();
                idle(cfg_.spin);
                continue;
            }
            m.t_read = now_ns();
            m.feature = 0;
            if (split && m.ev.kind == EventKind::Trade) {
                m.feature = stage_a(k, m.ev.px, m.ev.ask_qty, wa_);
                ++k;
            }
            // No loss allowed here either; while this spins the multicast ring can
            // lap us, which the engine counts as a drop and invalidates the run.
            while (!q.push(m)) ++internal_full;
        }
        reader_done.store(true, std::memory_order_release);
        q.close();
        worker.join();
        cpu_reader_ns = thread_cpu_ns() - cpu0;
        wakes = q.wakes;
        parks = q.parks;
        lost_wakeups = q.lost_wakeups;
    }

    template <typename Q>
    void worker_loop(Q& q, AgentContext& ctx, const std::atomic<bool>& reader_done) {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        constexpr int64_t PARK_TIMEOUT_NS = 20'000'000;  // backstop only; wakes come from the reader
        Msg m;
        uint64_t run = 0;
        while (true) {
            // A blocking queue returns at once when it has data and parks when it does not.
            if (Q::BLOCKING ? q.wait_pop(m, PARK_TIMEOUT_NS) : q.try_pop(m)) {
                handle(m, now_ns(), ctx);
                ++run;
                if (Q::BLOCKING) speed_worker.tick();  // a parking worker rarely sees an idle loop
                continue;
            }
            if (run) {
                worker_run.add(int64_t(run));
                max_worker_run = std::max(max_worker_run, run);
                run = 0;
            }
            if (reader_done.load(std::memory_order_acquire)) {
                while (q.try_pop(m)) handle(m, now_ns(), ctx);
                break;
            }
            if (!Q::BLOCKING) {
                speed_worker.tick();
                idle(cfg_.spin);
            }
        }
        cpu_worker_ns = thread_cpu_ns() - cpu0;
    }

    // Not latency critical: poll briefly, then sleep. CPU is part of the result.
    void housekeeping_loop() {
        set_thread_qos();
        const int64_t cpu0 = thread_cpu_ns();
        uint64_t f;
        int empty = 0;
        while (true) {
            if (hk_q_->try_pop(f)) {
                hk_.add(f);
                empty = 0;
                continue;
            }
            if (hk_stop_.load(std::memory_order_acquire)) {
                while (hk_q_->try_pop(f)) hk_.add(f);
                break;
            }
            if (++empty < 256) std::this_thread::yield();
            else std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        cpu_hk_ns = thread_cpu_ns() - cpu0;
    }

    const Config& cfg_;
    Shared& sh_;
    const int64_t wa_, wb_;
    Signal signal_;            // worker thread only
    Housekeeper hk_;           // trading thread (inline) or housekeeping thread only
    std::vector<Ts> submit_ts_;
    std::unique_ptr<SPSCQueue<uint64_t, QCAP>> hk_q_ = std::make_unique<SPSCQueue<uint64_t, QCAP>>();
    std::thread hk_thread_;
    std::atomic<bool> hk_stop_{false};
};

// ------------------------------------------------------------------ main
[[noreturn]] void usage() {
    std::cerr << "usage: strategy_threads --topology single|forward|split --hk none|inline|thread\n"
                 "         --work N --pattern smooth|burst --wait spin|yield [--steps N] [--rate R]\n"
                 "         [--burst-size B] [--hk-window W] [--hk-every E] [--qos initiated|interactive]\n"
                 "         [--queue spsc|mutex_cv|futex] [--burner] [--reference]\n";
    std::exit(2);
}

int64_t rusage_cpu_ns() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return (int64_t(ru.ru_utime.tv_sec) + ru.ru_stime.tv_sec) * 1000000000 +
           (int64_t(ru.ru_utime.tv_usec) + ru.ru_stime.tv_usec) * 1000;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    bool reference_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
        if (a == "--topology") {
            const std::string v = val();
            if (v == "single") cfg.topology = Topology::Single;
            else if (v == "forward") cfg.topology = Topology::Forward;
            else if (v == "split") cfg.topology = Topology::Split;
            else usage();
        } else if (a == "--queue") {
            const std::string v = val();
            if (v == "spsc") cfg.queue = QueueKind::Spsc;
            else if (v == "mutex_cv") cfg.queue = QueueKind::MutexCv;
            else if (v == "futex") cfg.queue = QueueKind::Futex;
            else usage();
        } else if (a == "--burner") {
            cfg.burner = true;
        } else if (a == "--hk") {
            const std::string v = val();
            if (v == "none") cfg.hk = Housekeeping::None;
            else if (v == "inline") cfg.hk = Housekeeping::Inline;
            else if (v == "thread") cfg.hk = Housekeeping::Thread;
            else usage();
        } else if (a == "--pattern") {
            const std::string v = val();
            if (v != "smooth" && v != "burst") usage();
            cfg.burst = v == "burst";
        } else if (a == "--wait") {
            const std::string v = val();
            if (v != "spin" && v != "yield") usage();
            cfg.spin = v == "spin";
        } else if (a == "--work") cfg.work = std::stoll(val());
        else if (a == "--steps") cfg.steps = std::stoi(val());
        else if (a == "--rate") cfg.rate = std::stod(val());
        else if (a == "--burst-size") cfg.burst_size = std::stoi(val());
        else if (a == "--hk-window") cfg.hk_window = std::stoi(val());
        else if (a == "--hk-every") cfg.hk_every = std::stoi(val());
        else if (a == "--qos") {
            const std::string v = val();
            if (v != "initiated" && v != "interactive") usage();
            g_interactive = v == "interactive";
        }
        else if (a == "--reference") reference_only = true;
        else usage();
    }
    if (cfg.burner && cfg.topology != Topology::Single) usage();
    if (cfg.steps <= 0 || cfg.steps % LEVEL_ORDERS || ladder_px(cfg.steps - 1) >= TAKER_LIMIT) {
        std::cerr << "--steps must be a positive multiple of " << LEVEL_ORDERS << " and fit under the taker limit\n";
        return 2;
    }

    const Ts t_ref = now_ns();
    const Reference ref = replay(cfg);
    const double ref_ns_per_step = double(now_ns() - t_ref) / cfg.steps;
    if (reference_only) {
        std::cout << "{\"decisions\":" << ref.decisions << ",\"housekeeping\":" << ref.housekeeping
                  << ",\"ns_per_step\":" << ref_ns_per_step << "}\n";
        return 0;
    }

    EngineConfig ec;
    ec.fanout = Fanout::Multicast;
    ec.qos = g_interactive ? QosClass::UserInteractive : QosClass::UserInitiated;
    ec.engine_idle = IdlePolicy::Spin;
    ec.agent_idle = IdlePolicy::Sleep;  // every agent here runs its own loop
    ec.collar = 0.05;
    // Timeout only: the probe stops the session once it holds every ack.
    ec.session_seconds = 2.0 + cfg.steps / cfg.rate + 5.0;

    Shared sh;
    Engine engine(ec);
    sh.engine = &engine;
    auto maker_p = std::make_unique<Maker>(cfg, sh);
    auto taker_p = std::make_unique<Taker>(cfg, sh);
    auto probe_p = std::make_unique<Probe>(cfg, sh);
    Maker& maker = *maker_p;
    Taker& taker = *taker_p;
    Probe& probe = *probe_p;
    engine.add_agent(std::move(maker_p));
    engine.add_agent(std::move(taker_p));
    engine.add_agent(std::move(probe_p));

    const int64_t cpu0 = rusage_cpu_ns();
    const Ts wall0 = now_ns();
    RunReport r = engine.run();
    const double wall_s = double(now_ns() - wall0) / 1e9;
    const double cpu_s = double(rusage_cpu_ns() - cpu0) / 1e9;

    std::vector<std::string> invalid;
    auto require = [&](bool ok, const char* why) { if (!ok) invalid.push_back(why); };
    const uint64_t steps = uint64_t(cfg.steps);
    require(sh.probe_done.load(), "probe did not receive every ack before the timeout");
    require(probe.trades == steps, "probe trade count");
    require(probe.orders == steps, "probe order count");
    require(probe.acks == steps, "probe ack count");
    require(probe.errors == 0, "probe saw an unexpected event or a mismatched trade");
    require(probe.decisions == ref.decisions, "decision checksum differs from reference");
    require(cfg.hk == Housekeeping::None || probe.hk_checksum() == ref.housekeeping,
            "housekeeping checksum differs from reference");
    require(probe.lost_wakeups == 0, "worker slept through a wake with work queued");
    require(r.trades == steps, "engine trade count");
    require(r.events_dropped == 0, "engine or multicast dropped events");
    require(maker.errors == 0 && taker.errors == 0, "maker or taker saw a reject");
    for (const auto& a : r.agents) require(a.queue_full == 0, "an order queue was full");

    std::string engine_json = to_json(r);
    std::replace(engine_json.begin(), engine_json.end(), '\n', ' ');  // one session, one line

    std::ostringstream inv;
    inv << "[";
    for (size_t i = 0; i < invalid.size(); ++i) inv << (i ? "," : "") << "\"" << invalid[i] << "\"";
    inv << "]";

    std::cout << "{\"valid\":" << (invalid.empty() ? "true" : "false") << ",\"invalid\":" << inv.str()
              << ",\"config\":{\"topology\":\"" << topology_name(cfg.topology) << "\",\"hk\":\""
              << hk_name(cfg.hk) << "\",\"queue\":\"" << (cfg.topology == Topology::Single ? "none" : queue_name(cfg.queue))
              << "\",\"burner\":" << (cfg.burner ? "true" : "false") << ",\"work\":" << cfg.work << ",\"pattern\":\""
              << (cfg.burst ? "burst" : "smooth") << "\",\"wait\":\"" << (cfg.spin ? "spin" : "yield")
              << "\",\"qos\":\"" << (g_interactive ? "interactive" : "initiated")
              << "\",\"steps\":" << cfg.steps << ",\"rate\":" << cfg.rate << ",\"burst_size\":" << cfg.burst_size
              << ",\"hk_window\":" << cfg.hk_window << ",\"hk_every\":" << cfg.hk_every << "}"
              << ",\"reference\":{\"decisions\":" << ref.decisions << ",\"housekeeping\":" << ref.housekeeping
              << ",\"ns_per_step\":" << ref_ns_per_step << "}"
              << ",\"probe\":{\"trades\":" << probe.trades << ",\"orders\":" << probe.orders
              << ",\"acks\":" << probe.acks << ",\"errors\":" << probe.errors
              << ",\"decisions\":" << probe.decisions << ",\"hk_checksum\":" << probe.hk_checksum()
              << ",\"hk_passes\":" << probe.hk_passes() << ",\"internal_full\":" << probe.internal_full
              << ",\"hk_full\":" << probe.hk_full << ",\"max_worker_run\":" << probe.max_worker_run
              << ",\"cpu_ns\":{\"reader\":" << probe.cpu_reader_ns << ",\"worker\":" << probe.cpu_worker_ns
              << ",\"housekeeping\":" << probe.cpu_hk_ns << ",\"burner\":" << probe.cpu_burner_ns
              << ",\"taker\":" << taker.cpu_ns << "}"
              << ",\"wakes\":" << probe.wakes << ",\"parks\":" << probe.parks
              << ",\"lost_wakeups\":" << probe.lost_wakeups
              << ",\"speed\":{\"reader\":" << speed_json(probe.speed_reader) << ",\"worker\":"
              << speed_json(probe.speed_worker) << ",\"burner\":" << speed_json(probe.speed_burner) << "}"
              << ",\"read_delay\":" << dist_json(probe.read_delay)
              << ",\"handoff\":" << dist_json(probe.handoff)
              << ",\"compute\":" << dist_json(probe.compute)
              << ",\"event_to_order\":" << dist_json(probe.event_to_order)
              << ",\"order_to_ack\":" << dist_json(probe.order_to_ack)
              << ",\"ack_read\":" << dist_json(probe.ack_read)
              << ",\"ack_handoff\":" << dist_json(probe.ack_handoff)
              << ",\"worker_run\":" << dist_json(probe.worker_run) << "}"
              << ",\"taker\":{\"retries\":" << taker.retries << ",\"lateness\":" << dist_json(taker.lateness) << "}"
              << ",\"process\":{\"wall_s\":" << wall_s << ",\"cpu_s\":" << cpu_s << "}"
              << ",\"engine\":" << engine_json << "}\n";
    return invalid.empty() ? 0 : 1;
}
