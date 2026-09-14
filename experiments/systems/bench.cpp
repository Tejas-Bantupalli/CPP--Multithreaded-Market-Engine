// Fixed-message concurrency benchmark. No trading strategy or AI participant.
#include <memory>
#include "transport.h"
#include "multicast_ring.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <time.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

struct Message {
    int64_t scheduled = 0, attempted = 0;
    uint64_t sequence = 0, payload = 0;
    uint32_t producer = 0;
};
// Same ring algorithm and usable capacity as SPSCQueue, but its two indices
// share a cache line. Experimental control for the production queue's padding.
class Unpadded final : public Channel<Message> {
    std::atomic<size_t> head_{0}, tail_{0};
    Message buf_[QCAP];
public:
    bool push(const Message& m) override {
        const auto h=head_.load(std::memory_order_relaxed), next=(h+1)&(QCAP-1);
        if (next==tail_.load(std::memory_order_acquire)) return false;
        buf_[h]=m; head_.store(next,std::memory_order_release); return true;
    }
    bool pop(Message& m) override {
        const auto t=tail_.load(std::memory_order_relaxed);
        if (t==head_.load(std::memory_order_acquire)) return false;
        m=buf_[t]; tail_.store((t+1)&(QCAP-1),std::memory_order_release); return true;
    }
    const char* name() const override { return "spsc_unpadded"; }
};
uint64_t payload(uint64_t seq, unsigned producer) {
    return (seq * 0x9e3779b97f4a7c15ULL) ^ (producer + 0xabc123ULL);
}
int64_t cpu_now() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) throw std::runtime_error("thread CPU clock unavailable");
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
void scheduling() {
#ifdef __APPLE__
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0))
        throw std::runtime_error("could not set worker QoS");
#endif
}
struct Samples {
    std::vector<int64_t> values;
    explicit Samples(size_t n = 0) { values.reserve(n); }
    void add(int64_t n) { values.push_back(n); }
    void print() {
        std::sort(values.begin(), values.end());
        auto p = [&](double q) -> int64_t {
            return values.empty() ? 0 : values[std::min(values.size()-1, size_t(std::ceil(q*values.size())-1))];
        };
        std::cout << "{\"count\":" << values.size() << ",\"p50\":" << p(.5)
                  << ",\"p99\":" << p(.99) << ",\"p999\":" << p(.999) << ",\"max\":" << p(1) << "}";
    }
};
struct Consumer {
    Samples total, handoff, queue, batch_wait;
    std::vector<int64_t> last;
    std::vector<Samples> by_producer;
    uint64_t received = 0, errors = 0, checksum = 0;
    int64_t cpu = 0;
    Consumer(size_t count, size_t producers) : total(count), handoff(count), queue(count),
        batch_wait(count), last(producers, -1) {
        for (size_t i=0; i<producers; ++i) by_producer.emplace_back(count);
    }
    void consume(const Message& m, int64_t popped) {
        const int64_t processing = now_ns();
        if (m.producer >= last.size() || m.payload != payload(m.sequence, m.producer)) ++errors;
        else {
            if (int64_t(m.sequence) <= last[m.producer]) ++errors;
            last[m.producer] = int64_t(m.sequence);
        }
        // Identical small deterministic handler in every variant; no order book.
        checksum ^= m.payload;
        ++received;
        const auto end = now_ns();
        total.add(end - m.scheduled);
        handoff.add(end - m.attempted);
        queue.add(popped - m.attempted);
        batch_wait.add(processing - popped);
        if (m.producer < by_producer.size()) by_producer[m.producer].add(end-m.attempted);
    }
};
struct Producer { uint64_t accepted = 0, dropped = 0; int64_t cpu = 0; Samples late; };
struct Collected { Message message; int64_t popped; };

int main(int argc, char** argv) {
    try {
        std::string mode = "pair", transport = "spsc", wait = "yield", fanout = "spsc";
        unsigned n = 1, count = 10000, rate = 10000, batch = 256, burst = 1;
        bool sort = true;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (++i == argc) throw std::runtime_error("missing option value");
            std::string value = argv[i];
            if (key == "--mode") mode = value;
            else if (key == "--transport") transport = value;
            else if (key == "--wait") wait = value;
            else if (key == "--fanout") fanout = value;
            else if (key == "--n") n = std::stoul(value);
            else if (key == "--count") count = std::stoul(value);
            else if (key == "--rate") rate = std::stoul(value);
            else if (key == "--batch") batch = std::stoul(value);
            else if (key == "--burst") burst = std::stoul(value);
            else if (key == "--sort") sort = std::stoi(value) != 0;
            else throw std::runtime_error("unknown option");
        }
        TransportKind kind = TransportKind::Spsc;
        if ((!parse_transport(transport, kind) && transport != "spsc_unpadded") || !n || n > 64 || !count || count > 1000000 ||
            !batch || batch > 4096 || !burst || burst > count || (mode != "pair" && mode != "orders" && mode != "fanout") ||
            (wait != "spin" && wait != "yield" && wait != "sleep" && wait != "block") ||
            (fanout != "spsc" && fanout != "multicast")) throw std::runtime_error("invalid configuration");
        if (wait == "block" && (transport != "mutex_cv" || mode != "pair"))
            throw std::runtime_error("blocking supported only by mutex_cv pair tests");
        const unsigned np = mode == "fanout" ? 1 : n;
        const unsigned nc = mode == "orders" ? 1 : n;
        std::vector<std::unique_ptr<Channel<Message>>> queues;
        for (unsigned i = 0; i < n; ++i) {
            if (transport == "spsc_unpadded") queues.push_back(std::make_unique<Unpadded>());
            else queues.push_back(make_channel<Message>(kind));
        }
        auto ring = std::make_unique<MulticastRing<Message, QCAP>>();
        std::vector<Consumer> consumers;
        for (unsigned i = 0; i < nc; ++i) consumers.emplace_back(size_t(count) * (mode == "orders" ? n : 1), np);
        std::vector<Producer> producers(np);
        for (auto& p : producers) p.late.values.reserve(count);
        std::vector<uint64_t> lapped(nc, 0);
        std::atomic<unsigned> ready{0}, done{0};
        std::atomic<bool> go{false};
        int64_t epoch = 0;
        auto gate = [&] {
            scheduling();
            ready.fetch_add(1);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(epoch)));
        };
        auto idle = [&] {
            if (wait == "yield") std::this_thread::yield();
            else if (wait == "sleep") std::this_thread::sleep_for(std::chrono::microseconds(50));
        };
        std::vector<std::thread> threads;
        for (unsigned id = 0; id < nc; ++id) threads.emplace_back([&, id] {
            auto& c = consumers[id];
            typename MulticastRing<Message, QCAP>::Reader reader;
            std::vector<Collected> collected;
            collected.reserve(size_t(n) * batch);
            unsigned rr = 0;
            gate();
            const auto cpu_start = cpu_now();
            for (;;) {
                // Observe completion before draining: never quit on a stale empty read.
                const bool finished = done.load(std::memory_order_acquire) == np;
                unsigned found = 0;
                if (mode == "orders") {
                    collected.clear();
                    for (unsigned k = 0; k < n; ++k) {
                        Message m;
                        for (unsigned j = 0; j < batch && queues[(rr+k)%n]->pop(m); ++j)
                            collected.push_back({m, now_ns()});
                    }
                    rr = (rr + 1) % n;
                    if (sort) std::stable_sort(collected.begin(), collected.end(), [](const auto& a, const auto& b) {
                        return a.message.attempted < b.message.attempted;
                    });
                    for (const auto& item : collected) c.consume(item.message, item.popped);
                    found = unsigned(collected.size());
                } else {
                    Message m;
                    for (unsigned j = 0; j < batch; ++j) {
                        bool ok;
                        if (mode == "fanout" && fanout == "multicast") ok = ring->try_read(reader, m);
                        else if (wait == "block") ok = queues[id]->wait_pop(m, 50000);
                        else ok = queues[id]->pop(m);
                        if (!ok) break;
                        c.consume(m, now_ns());
                        ++found;
                    }
                }
                if (!found) { if (finished) break; idle(); }
            }
            c.cpu = cpu_now() - cpu_start;
            lapped[id] = reader.dropped;
        });
        for (unsigned id = 0; id < np; ++id) threads.emplace_back([&, id] {
            auto& p = producers[id];
            gate();
            const auto cpu_start = cpu_now();
            for (unsigned seq = 0; seq < count; ++seq) {
                const int64_t target = epoch + (rate ? int64_t(seq/burst)*burst*1000000000/rate : 0);
                if (rate && now_ns() + 20000 < target)
                    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(target - 20000)));
                while (rate && now_ns() < target) {}
                Message m{target, now_ns(), seq, payload(seq, id), id};
                p.late.add(m.attempted-target);
                if (mode == "fanout") {
                    if (fanout == "multicast") { ring->publish(m); p.accepted += n; }
                    else for (auto& q : queues) { if (q->push(m)) ++p.accepted; else ++p.dropped; }
                } else { if (queues[id]->push(m)) ++p.accepted; else ++p.dropped; }
            }
            p.cpu = cpu_now()-cpu_start;
            done.fetch_add(1, std::memory_order_release);
        });
        while (ready.load() != np+nc) std::this_thread::yield();
        epoch = now_ns()+50000000;
        go.store(true, std::memory_order_release);
        for (auto& t : threads) t.join();
        const int64_t elapsed = now_ns()-epoch;
        uint64_t accepted=0, dropped=0, received=0, errors=0, laps=0;
        int64_t pcpu=0, ccpu=0;
        Samples total, handoff, queue, bwait, lateness;
        auto merge = [](Samples& dst, const Samples& src) { dst.values.insert(dst.values.end(), src.values.begin(), src.values.end()); };
        for (const auto& p : producers) { accepted+=p.accepted; dropped+=p.dropped; pcpu+=p.cpu; merge(lateness,p.late); }
        for (unsigned i=0; i<nc; ++i) {
            const auto& c=consumers[i]; received+=c.received; errors+=c.errors; ccpu+=c.cpu; laps+=lapped[i];
            merge(total,c.total); merge(handoff,c.handoff); merge(queue,c.queue); merge(bwait,c.batch_wait);
        }
        const bool accounting = accepted == received+laps;
        std::cout << "{\"mode\":\"" << mode << "\",\"n\":" << n << ",\"message_bytes\":" << sizeof(Message)
                  << ",\"count_per_producer\":" << count << ",\"rate_per_producer\":" << rate
                  << ",\"burst\":" << burst
                  << ",\"accepted\":" << accepted << ",\"received\":" << received
                  << ",\"drops\":" << dropped << ",\"lapped\":" << laps << ",\"errors\":" << errors
                  << ",\"accounting_ok\":" << (accounting ? "true" : "false")
                  << ",\"elapsed_ns\":" << elapsed << ",\"producer_cpu_ns\":" << pcpu << ",\"consumer_cpu_ns\":" << ccpu
                  << ",\"scheduled_to_complete_ns\":"; total.print();
        std::cout << ",\"attempt_to_complete_ns\":"; handoff.print();
        std::cout << ",\"attempt_to_pop_ns\":"; queue.print();
        std::cout << ",\"batch_wait_ns\":"; bwait.print();
        std::cout << ",\"producer_lateness_ns\":"; lateness.print();
        std::cout << ",\"consumers\":[";
        for (unsigned i=0; i<nc; ++i) {
            if (i) std::cout << ',';
            auto& c=consumers[i];
            std::cout << "{\"id\":" << i << ",\"received\":" << c.received << ",\"cpu_ns\":" << c.cpu
                      << ",\"checksum\":" << c.checksum << ",\"attempt_to_complete_ns\":";
            c.handoff.print();
            std::cout << ",\"by_producer\":[";
            for (unsigned p=0; p<np; ++p) {
                if (p) std::cout << ',';
                c.by_producer[p].print();
            }
            std::cout << "]}";
        }
        std::cout << "]}\n";
        return errors || !accounting ? 2 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
