#include "engine.h"
#include "histogram.h"
#include "multicast_ring.h"
#include "order_book.h"
#include "spsc_queue.h"
#include "transport.h"
#include "strategies.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(#x) + " (line " + std::to_string(__LINE__) + ")"); } while (false)

namespace {

Command make(AgentId agent, OrderId id, Side side, Price px, Qty qty, TimeInForce tif = TimeInForce::GTC) {
    Command c;
    c.kind = CmdKind::New; c.agent = agent; c.id = id; c.side = side; c.px = px; c.qty = qty; c.tif = tif;
    c.t_submit = now_ns();
    return c;
}

struct Scratch {
    std::vector<Fill> fills;
    std::vector<CancelInfo> stp;
    AddResult add(OrderBook& b, const Command& c) { fills.clear(); stp.clear(); return b.add(c, fills, stp); }
};

// ---------- order book ----------
void book_price_time() {
    OrderBook b(1, 1000);
    Scratch s;
    CHECK(s.add(b, make(1, 1, Side::Buy, 100, 1)).resting == 1);
    CHECK(s.add(b, make(2, 2, Side::Buy, 101, 1)).resting == 1);
    CHECK(s.add(b, make(3, 3, Side::Buy, 100, 1)).resting == 1);
    CHECK(b.best_bid() == 101 && b.best_bid_qty() == 1 && b.level_qty(Side::Buy, 100) == 2);
    const AddResult r = s.add(b, make(9, 9, Side::Sell, 100, 3));
    CHECK(r.accepted && r.filled == 3 && r.resting == 0);
    CHECK(s.fills.size() == 3);
    CHECK(s.fills[0].maker_id == 2 && s.fills[0].px == 101); // best price first
    CHECK(s.fills[1].maker_id == 1 && s.fills[1].px == 100); // then FIFO within level
    CHECK(s.fills[2].maker_id == 3 && s.fills[2].px == 100);
    CHECK(s.fills[0].taker_side == Side::Sell && s.fills[0].taker == 9);
    CHECK(!b.has_bid() && !b.has_ask() && b.open_orders() == 0);
}

void book_partial_and_ioc() {
    OrderBook b(1, 1000);
    Scratch s;
    CHECK(s.add(b, make(1, 1, Side::Sell, 100, 5)).resting == 5);
    AddResult r = s.add(b, make(2, 2, Side::Buy, 100, 8, TimeInForce::IOC));
    CHECK(r.accepted && r.filled == 5 && r.resting == 0);
    CHECK(s.fills.size() == 1 && s.fills[0].qty == 5 && s.fills[0].maker_remaining == 0);
    CHECK(b.open_orders() == 0 && !b.has_ask());
    r = s.add(b, make(2, 3, Side::Buy, 100, 8));
    CHECK(r.accepted && r.filled == 0 && r.resting == 8);
    CHECK(b.best_bid() == 100 && b.best_bid_qty() == 8);
    // partial fill of a resting order keeps the remainder at the front
    r = s.add(b, make(3, 4, Side::Sell, 99, 3));
    CHECK(r.filled == 3 && s.fills[0].px == 100 && s.fills[0].maker_remaining == 5);
    CHECK(b.best_bid_qty() == 5 && b.contains(3));
}

void book_walk_levels() {
    OrderBook b(1, 1000);
    Scratch s;
    s.add(b, make(1, 1, Side::Sell, 100, 3));
    s.add(b, make(1, 2, Side::Sell, 101, 3));
    s.add(b, make(1, 3, Side::Sell, 102, 3));
    CHECK(b.best_ask() == 100);
    const AddResult r = s.add(b, make(2, 4, Side::Buy, 105, 10));
    CHECK(r.filled == 9 && r.resting == 1);
    CHECK(s.fills.size() == 3 && s.fills[0].px == 100 && s.fills[1].px == 101 && s.fills[2].px == 102);
    CHECK(!b.has_ask() && b.best_bid() == 105 && b.best_bid_qty() == 1);
}

void book_cancel() {
    OrderBook b(1, 1000);
    Scratch s;
    s.add(b, make(1, 1, Side::Buy, 100, 4));
    s.add(b, make(1, 2, Side::Buy, 98, 2));
    s.add(b, make(2, 3, Side::Buy, 99, 2));
    CancelInfo ci; RejectReason why;
    CHECK(!b.cancel(1, 2, ci, why) && why == RejectReason::NotOwner);
    CHECK(b.cancel(1, 1, ci, why) && ci.remaining == 4);
    CHECK(b.best_bid() == 99); // best advanced across an empty level
    CHECK(!b.cancel(1, 1, ci, why) && why == RejectReason::UnknownOrder);
    CHECK(b.cancel(3, 2, ci, why) && b.best_bid() == 98);
    CHECK(b.cancel(2, 1, ci, why) && !b.has_bid() && b.open_orders() == 0);
    // pool reuse after frees
    for (OrderId id = 10; id < 10 + 5000; ++id) s.add(b, make(1, id, Side::Sell, 200 + (id % 50), 1));
    CHECK(b.open_orders() == 5000);
    for (OrderId id = 10; id < 10 + 5000; ++id) CHECK(b.cancel(id, 1, ci, why));
    CHECK(b.open_orders() == 0 && !b.has_ask());
}

void book_self_trade_prevention() {
    OrderBook b(1, 1000);
    Scratch s;
    s.add(b, make(1, 1, Side::Sell, 100, 5));
    s.add(b, make(2, 2, Side::Sell, 100, 5));
    const AddResult r = s.add(b, make(1, 3, Side::Buy, 100, 7));
    CHECK(r.accepted && r.filled == 5 && r.resting == 2);
    CHECK(s.stp.size() == 1 && s.stp[0].id == 1 && s.stp[0].remaining == 5);
    CHECK(s.fills.size() == 1 && s.fills[0].maker == 2);
    CHECK(!b.has_ask() && b.best_bid() == 100 && b.best_bid_qty() == 2);
}

void book_rejects() {
    OrderBook b(10, 20);
    Scratch s;
    CHECK(s.add(b, make(1, 1, Side::Buy, 15, 0)).reason == RejectReason::BadQty);
    CHECK(s.add(b, make(1, 1, Side::Buy, 9, 1)).reason == RejectReason::OutOfBand);
    CHECK(s.add(b, make(1, 1, Side::Buy, 20, 1)).reason == RejectReason::OutOfBand);
    CHECK(s.add(b, make(1, 1, Side::Buy, 19, 1)).accepted);
    CHECK(s.add(b, make(1, 1, Side::Buy, 18, 1)).reason == RejectReason::DuplicateId);
    CHECK(b.open_orders() == 1);
    const uint64_t c1 = b.checksum();
    s.add(b, make(2, 2, Side::Sell, 19, 1));
    CHECK(b.open_orders() == 0 && b.checksum() == 0 && c1 != 0);
}

// ---------- histogram / queue ----------
void histogram() {
    Histogram h;
    for (int i = 1; i <= 100000; ++i) h.record(i);
    CHECK(h.count() == 100000 && h.max() == 100000 && h.min() == 1);
    const double p50 = static_cast<double>(h.percentile(0.5));
    CHECK(std::fabs(p50 - 50000) / 50000 < 0.05);
    const double p99 = static_cast<double>(h.percentile(0.99));
    CHECK(std::fabs(p99 - 99000) / 99000 < 0.05);
    CHECK(h.percentile(1.0) <= 100000);
    Histogram e;
    CHECK(e.percentile(0.5) == 0 && e.max() == 0);
    for (uint64_t v : {0ull, 1ull, 31ull, 32ull, 33ull, 1000ull, (1ull << 40) + 12345ull}) {
        const int idx = Histogram::index_of(v);
        CHECK(Histogram::lower_bound_of(idx) <= v);
        CHECK(idx + 1 >= Histogram::BUCKETS || Histogram::lower_bound_of(idx + 1) > v);
    }
}

void spsc_wraparound() {
    SPSCQueue<int, 4> q;
    int value;
    for (int round = 0; round < 1000; ++round) {
        CHECK(!q.try_pop(value));
        CHECK(q.try_push(1) && q.try_push(2) && q.try_push(3) && !q.try_push(4));
        for (int expected = 1; expected <= 3; ++expected) CHECK(q.try_pop(value) && value == expected);
    }
}

struct Pair { uint64_t a; uint64_t b; uint64_t pad[8]; }; // 80 bytes, like Event

void multicast_ring() {
    // Single reader, no lapping: every value, in order.
    {
        MulticastRing<Pair, 8> ring;
        MulticastRing<Pair, 8>::Reader rd;
        Pair v;
        CHECK(!ring.try_read(rd, v));
        for (uint64_t i = 1; i <= 5; ++i) ring.publish(Pair{i, ~i, {}});
        for (uint64_t i = 1; i <= 5; ++i) CHECK(ring.try_read(rd, v) && v.a == i && v.b == ~i);
        CHECK(!ring.try_read(rd, v) && rd.dropped == 0);
    }
    // Lapping: a reader that never polled while 100 values went through an 8-slot ring
    // loses the overwritten ones, lands on an intact value, and reads the rest in order.
    {
        MulticastRing<Pair, 8> ring;
        MulticastRing<Pair, 8>::Reader rd;
        Pair v;
        for (uint64_t i = 1; i <= 100; ++i) ring.publish(Pair{i, ~i, {}});
        CHECK(ring.try_read(rd, v));
        CHECK(v.a == 100 + 2 - 8 && rd.dropped == v.a - 1);
        uint64_t last = v.a;
        while (ring.try_read(rd, v)) { CHECK(v.a == last + 1 && v.b == ~v.a); last = v.a; }
        CHECK(last == 100);
    }
    // Concurrent: one writer, three readers of different speeds. Nobody sees a torn value,
    // every reader's sequence is strictly increasing, and the fast readers lose nothing.
    {
        constexpr uint64_t N = 200000;
        auto ring = std::make_unique<MulticastRing<Pair, 1024>>();
        std::atomic<bool> done{false};
        std::atomic<uint64_t> torn{0};
        uint64_t seen[3] = {0, 0, 0}, dropped[3] = {0, 0, 0};
        std::thread readers[3];
        for (int k = 0; k < 3; ++k) readers[k] = std::thread([&, k] {
            MulticastRing<Pair, 1024>::Reader rd;
            Pair v;
            uint64_t last = 0;
            while (true) {
                if (ring->try_read(rd, v)) {
                    if (v.b != ~v.a || v.a <= last) torn.fetch_add(1);
                    last = v.a;
                    ++seen[k];
                    if (k == 2 && (seen[k] % 64) == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
                } else if (done.load(std::memory_order_acquire) && ring->published() == N && rd.next > N) {
                    break;
                } else if (done.load(std::memory_order_acquire) && !ring->try_read(rd, v)) {
                    break;
                }
            }
            dropped[k] = rd.dropped;
        });
        for (uint64_t i = 1; i <= N; ++i) ring->publish(Pair{i, ~i, {}});
        done.store(true, std::memory_order_release);
        for (auto& t : readers) t.join();
        CHECK(torn.load() == 0);
        for (int k = 0; k < 3; ++k) CHECK(seen[k] + dropped[k] == N);
        CHECK(dropped[2] > 0); // the deliberately slow reader was lapped
    }
}


// ---------- transports ----------
// Every variant must behave identically as a queue. Only its cost differs.
void transport_channels() {
    for (TransportKind k : {TransportKind::Spsc, TransportKind::Mutex, TransportKind::MutexCv}) {
        auto ch = make_channel<Command>(k);
        Command c;
        CHECK(!ch->pop(c));                       // empty
        for (int i = 1; i <= 100; ++i) {
            Command in = make(1, static_cast<OrderId>(i), Side::Buy, 100, i);
            CHECK(ch->push(in));
        }
        for (int i = 1; i <= 100; ++i) {          // strict FIFO
            CHECK(ch->pop(c) && c.id == static_cast<OrderId>(i) && c.qty == i);
        }
        CHECK(!ch->pop(c));
        // Every variant must hold the same depth: fill it, then the next push is
        // refused rather than blocking or growing.
        size_t depth = 0;
        while (ch->push(make(1, depth + 1, Side::Buy, 100, 1))) ++depth;
        CHECK(depth == QCAP - 1);
        CHECK(ch->pop(c));                        // one slot freed
        CHECK(ch->push(make(1, 999999, Side::Buy, 100, 1)));
        CHECK(std::string(ch->name()) == transport_name(k));
    }
    // Only the condition-variable transport parks the consumer.
    CHECK(!make_channel<Event>(TransportKind::Spsc)->blocking());
    CHECK(!make_channel<Event>(TransportKind::Mutex)->blocking());
    CHECK(make_channel<Event>(TransportKind::MutexCv)->blocking());

    // wait_pop must time out on an empty channel and wake on a push.
    {
        auto ch = make_channel<Event>(TransportKind::MutexCv);
        Event ev;
        const Ts t0 = now_ns();
        CHECK(!ch->wait_pop(ev, 2000000));        // 2 ms, nothing arrives
        CHECK(now_ns() - t0 >= 1000000);          // it really waited
        std::thread producer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            Event e; e.kind = EventKind::Trade; e.px = 12345; ch->push(e);
        });
        CHECK(ch->wait_pop(ev, 500000000) && ev.px == 12345);
        producer.join();
    }
}

// ---------- engine ----------
EngineConfig short_cfg(double seconds) {
    EngineConfig c;
    c.session_seconds = seconds;
    c.seed = 7;
    return c;
}

void conservation_with(EngineConfig cfg) {
    Engine e(cfg);
    for (const char* n : {"mm", "noise", "noise", "noise", "momentum", "meanrev"}) e.add_agent(make_strategy(n, Params{}));
    const RunReport r = e.run();
    CHECK(r.trades > 0);
    double cash = 0; long long pos = 0; uint64_t processed = 0, submitted = 0;
    for (const AgentReport& a : r.agents) {
        cash += a.cash; pos += a.position; processed += a.orders_processed; submitted += a.submitted;
        // An agent's own books match the engine's only if it consumed every event
        // addressed to it. The engine drains queued commands after agents have
        // exited, so a session tail can leave fills the agent never saw; a slower
        // transport leaves more of them.
        if (a.events_dropped == 0 && a.events == a.events_sent) {
            CHECK(a.agent_position == a.position);
            CHECK(std::fabs(a.agent_cash - a.cash) < 1e-6);
        }
    }
    // Cash is no longer conserved on its own: venue fees leave the system. Cash plus
    // the signed fees every agent paid must still return the starting total.
    double fees = 0;
    for (const AgentReport& a : r.agents) fees += a.fees;
    CHECK(std::fabs((cash + fees) - INITIAL_CASH * r.agents.size()) < 1e-6);
    CHECK(pos == 0);
    CHECK(processed == r.commands && submitted == r.commands); // nothing lost between agent and engine
    CHECK(r.submit_to_pop.count == r.commands && r.match.count == r.commands);
}

void engine_conservation() { conservation_with(short_cfg(0.4)); }

// The plumbing must not change what the market does, only what it costs.
void engine_transport_mutex() {
    EngineConfig cfg = short_cfg(0.4);
    cfg.transport = TransportKind::Mutex;
    conservation_with(cfg);
}

void engine_transport_mutex_cv() {
    EngineConfig cfg = short_cfg(0.4);
    cfg.transport = TransportKind::MutexCv;
    conservation_with(cfg);
}

// Mixed plumbing in one market: this is the comparison the benchmark relies on.
void engine_transport_mixed() {
    EngineConfig cfg = short_cfg(0.4);
    Engine e(cfg);
    const TransportKind ks[3] = {TransportKind::Spsc, TransportKind::Mutex, TransportKind::MutexCv};
    e.add_agent(make_strategy("mm", Params{}), ks[0]);
    e.add_agent(make_strategy("noise", Params{}), ks[1]);
    e.add_agent(make_strategy("noise", Params{}), ks[2]);
    const RunReport r = e.run();
    CHECK(r.trades > 0);
    for (int i = 0; i < 3; ++i) {
        CHECK(r.agents[i].transport == transport_name(ks[i]));
        CHECK(r.agents[i].orders_processed > 0);       // every transport delivered work
        CHECK(r.agents[i].react.count > 0);            // and agent reacted to events
    }
    double cash = 0, fees = 0;
    for (const AgentReport& a : r.agents) { cash += a.cash; fees += a.fees; }
    CHECK(std::fabs((cash + fees) - INITIAL_CASH * r.agents.size()) < 1e-6);
}

void engine_multicast() {
    EngineConfig cfg = short_cfg(0.4);
    cfg.fanout = Fanout::Multicast;
    conservation_with(cfg);
}

// Submits non-crossing bids as fast as it can; used to prove the drain-on-shutdown contract.
struct Flood : Strategy {
    const char* name() const override { return "flood"; }
    void on_idle(AgentContext& ctx) override { ctx.submit(Side::Buy, 50, 1); }
    void on_event(const Event&, AgentContext&) override {}
};

void engine_shutdown_drain() {
    EngineConfig cfg = short_cfg(0.2);
    cfg.collar = 0; // the flood bids at 50 ticks on purpose; this test is about draining, not prices
    Engine e(cfg);
    for (int i = 0; i < 3; ++i) e.add_agent(std::make_unique<Flood>());
    const RunReport r = e.run();
    uint64_t submitted = 0;
    for (const AgentReport& a : r.agents) { submitted += a.submitted; CHECK(a.orders_rejected == 0); }
    CHECK(submitted > 1000);
    CHECK(r.commands == submitted);
    CHECK(r.open_orders == submitted && r.trades == 0);
    CHECK(r.final_bid == 50 && r.final_bid_qty == static_cast<Qty>(submitted));
}

// Quotes far outside the collar; every order must be rejected with reason Collar.
struct FarQuoter : Strategy {
    int collar_rejects = 0;
    const char* name() const override { return "far"; }
    void on_idle(AgentContext& ctx) override { ctx.submit(Side::Buy, 8000, 1); ctx.submit(Side::Sell, 12000, 1); }
    void on_event(const Event& ev, AgentContext&) override { if (ev.kind == EventKind::Rejected && ev.reason == RejectReason::Collar) ++collar_rejects; }
};

void engine_collar() {
    EngineConfig cfg = short_cfg(0.2);
    cfg.collar = 0.05; // initial 10000 -> [9500, 10500]
    Engine e(cfg);
    auto* fq = new FarQuoter();
    e.add_agent(std::unique_ptr<Strategy>(fq));
    e.add_agent(std::make_unique<Flood>()); // bids at 50 ticks: also outside the collar
    const RunReport r = e.run();
    CHECK(r.open_orders == 0 && r.trades == 0);
    CHECK(r.agents[0].orders_rejected == r.agents[0].orders_processed && r.agents[0].orders_processed > 0);
    CHECK(r.agents[1].orders_rejected == r.agents[1].orders_processed);
    CHECK(fq->collar_rejects > 0);
}

// Seller rests asks; buyer lifts them one at a time as it sees them.
struct Seller : Strategy {
    const char* name() const override { return "seller"; }
    void on_start(AgentContext& ctx) override { for (int i = 0; i < 20; ++i) ctx.submit(Side::Sell, 10000 + i, 1); }
    void on_event(const Event&, AgentContext&) override {}
};
struct Buyer : Strategy {
    int bought = 0;
    const char* name() const override { return "buyer"; }
    void on_event(const Event& ev, AgentContext& ctx) override {
        if (ev.kind == EventKind::Fill) ++bought;
        if ((ev.kind == EventKind::BookUpdate || ev.kind == EventKind::Trade) && ctx.has_ask() && ctx.position() < 20)
            ctx.submit(Side::Buy, ctx.ask_px(), 1, TimeInForce::IOC);
    }
};

void engine_matching_pair() {
    EngineConfig mp = short_cfg(0.3);
    mp.maker_fee = 0; mp.taker_fee = 0;  // this test is about matching, not fees
    Engine e(mp);
    e.add_agent(std::make_unique<Seller>());
    e.add_agent(std::make_unique<Buyer>());
    const RunReport r = e.run();
    CHECK(r.trades == 20 && r.volume == 20);
    CHECK(r.agents[0].position == -20 && r.agents[1].position == 20);
    CHECK(r.agents[0].agent_position == -20 && r.agents[1].agent_position == 20);
    CHECK(r.open_orders == 0 && r.last_px == 10019);
    double expected = 0; for (int i = 0; i < 20; ++i) expected += to_dollars(10000 + i);
    CHECK(std::fabs((r.agents[0].cash - INITIAL_CASH) - expected) < 1e-6);
    CHECK(std::fabs((INITIAL_CASH - r.agents[1].cash) - expected) < 1e-6);
    CHECK(r.agents[1].react.count >= 20 && r.agents[1].fills == 20 && r.agents[1].delivery.count > 0);
}

// One maker, one taker, one fill: the rebate and the fee must land on the right sides.
void engine_fees() {
    EngineConfig cfg = short_cfg(0.3);
    cfg.maker_fee = -0.01;  // 1 cent per unit paid TO the resting side
    cfg.taker_fee = 0.02;   // 2 cents per unit charged to the aggressor
    Engine e(cfg);
    e.add_agent(std::make_unique<Seller>());  // rests 20 asks
    e.add_agent(std::make_unique<Buyer>());   // lifts them one at a time
    const RunReport r = e.run();
    CHECK(r.trades == 20 && r.volume == 20);
    const AgentReport& mk = r.agents[0];
    const AgentReport& tk = r.agents[1];
    CHECK(mk.maker_fills == 20 && mk.taker_fills == 0);
    CHECK(tk.taker_fills == 20 && tk.maker_fills == 0);
    CHECK(std::fabs(mk.fees - (-0.01 * 20)) < 1e-9);  // earned a rebate
    CHECK(std::fabs(tk.fees - (0.02 * 20)) < 1e-9);   // paid a fee
    // The rebate raises the maker's cash and the fee lowers the taker's, against a
    // fee-free run of the same trades.
    double expected = 0;
    for (int i = 0; i < 20; ++i) expected += to_dollars(10000 + i);
    CHECK(std::fabs((mk.cash - INITIAL_CASH) - (expected + 0.20)) < 1e-6);
    CHECK(std::fabs((INITIAL_CASH - tk.cash) - (expected + 0.40)) < 1e-6);
}


// A strategy that owns its whole stack: picks its transport, drives its own
// loop, and does its own bookkeeping via apply(). This is the contract the
// HFT agents are given, so it has to be exercised.
struct OwnStack : Strategy {
    std::atomic<uint64_t> loops{0};
    uint64_t consumed = 0;
    const char* name() const override { return "ownstack"; }
    std::optional<TransportKind> transport() const override { return TransportKind::MutexCv; }
    void on_event(const Event&, AgentContext&) override {}  // unused: run() owns dispatch

    void run(AgentContext& ctx, const std::atomic<bool>& running) override {
        Event ev;
        while (running.load(std::memory_order_relaxed)) {
            loops.fetch_add(1, std::memory_order_relaxed);
            bool got = false;
            while (ctx.poll(ev)) {                 // drain without a batch cap
                ctx.apply(ev);                     // bookkeeping, no on_event
                ++consumed;
                got = true;
                if (ev.kind == EventKind::Trade && ctx.has_ask() && ctx.position() < 10)
                    ctx.submit(Side::Buy, ctx.ask_px(), 1, TimeInForce::IOC);
            }
            if (!got && !ctx.wait_poll(ev, 20000)) continue;
            if (!got) { ctx.apply(ev); ++consumed; }
        }
    }
};

void engine_custom_run_loop() {
    EngineConfig cfg = short_cfg(0.3);
    cfg.transport = TransportKind::Spsc;          // the strategy must override this
    Engine e(cfg);
    e.add_agent(make_strategy("mm", Params{}));
    e.add_agent(make_strategy("noise", Params{}));
    auto* own = new OwnStack();
    e.add_agent(std::unique_ptr<Strategy>(own));
    const RunReport r = e.run();
    CHECK(r.agents[2].transport == std::string("mutex_cv"));   // its own choice won
    CHECK(own->loops.load() > 0);                              // its loop really ran
    CHECK(own->consumed > 0);                                  // and consumed events
    CHECK(r.agents[2].events == own->consumed);                // apply() did the counting
    if (r.agents[2].events_dropped == 0 && r.agents[2].events == r.agents[2].events_sent)
        CHECK(r.agents[2].agent_position == r.agents[2].position);
    double cash = 0, fees = 0;
    for (const AgentReport& a : r.agents) { cash += a.cash; fees += a.fees; }
    CHECK(std::fabs((cash + fees) - INITIAL_CASH * r.agents.size()) < 1e-6);
}

void agent_spec() {
    std::string name; Params p;
    CHECK(parse_agent_spec("mm", name, p) && name == "mm" && p.kv.empty());
    CHECK(parse_agent_spec("noise:sigma=1.5,size=4", name, p) && name == "noise");
    CHECK(p.get("sigma", 0) == 1.5 && p.get("size", 0) == 4 && p.get("missing", 9) == 9);
    CHECK(!parse_agent_spec(":x=1", name, p));
    CHECK(!parse_agent_spec("mm:novalue", name, p));
    CHECK(make_strategy("nope", p) == nullptr);
    for (const char* n : {"mm", "noise", "momentum", "meanrev"}) CHECK(make_strategy(n, p) != nullptr);
}

} // namespace

int main(int argc, char** argv) {
    try {
        CHECK(argc == 2);
        const std::string name = argv[1];
        if (name == "book_price_time") book_price_time();
        else if (name == "book_partial_and_ioc") book_partial_and_ioc();
        else if (name == "book_walk_levels") book_walk_levels();
        else if (name == "book_cancel") book_cancel();
        else if (name == "book_self_trade_prevention") book_self_trade_prevention();
        else if (name == "book_rejects") book_rejects();
        else if (name == "histogram") histogram();
        else if (name == "spsc_wraparound") spsc_wraparound();
        else if (name == "engine_conservation") engine_conservation();
        else if (name == "transport_channels") transport_channels();
        else if (name == "engine_transport_mutex") engine_transport_mutex();
        else if (name == "engine_transport_mutex_cv") engine_transport_mutex_cv();
        else if (name == "engine_transport_mixed") engine_transport_mixed();
        else if (name == "engine_custom_run_loop") engine_custom_run_loop();
        else if (name == "engine_multicast") engine_multicast();
        else if (name == "engine_collar") engine_collar();
        else if (name == "engine_fees") engine_fees();
        else if (name == "multicast_ring") multicast_ring();
        else if (name == "engine_shutdown_drain") engine_shutdown_drain();
        else if (name == "engine_matching_pair") engine_matching_pair();
        else if (name == "agent_spec") agent_spec();
        else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << (argc == 2 ? argv[1] : "?") << ": " << e.what() << '\n';
        return 1;
    }
}
