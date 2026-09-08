#include "engine.h"

#include "logger.h"
#include "order_book.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace {

void pin_to(int cpu) {
#ifdef __linux__
    const int n = static_cast<int>(std::thread::hardware_concurrency());
    if (n <= 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu % n, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

void idle(IdlePolicy p) {
    switch (p) {
        case IdlePolicy::Spin: break;
        case IdlePolicy::Yield: std::this_thread::yield(); break;
        case IdlePolicy::Sleep: std::this_thread::sleep_for(std::chrono::microseconds(50)); break;
    }
}

} // namespace

// ===================== Per-agent slot =====================
struct Engine::Slot {
    AgentId id;
    std::unique_ptr<Strategy> strategy;
    SPSCQueue<Command, QCAP> in;  // agent -> engine
    SPSCQueue<Event, QCAP> out;   // engine -> agent
    AgentContext ctx;
    std::thread thread;
    // Engine-side truth. Single writer: the engine thread.
    double cash = INITIAL_CASH;
    Qty position = 0;
    uint64_t fills = 0;
    Qty volume = 0;
    uint64_t orders_processed = 0;
    uint64_t orders_rejected = 0;
    uint64_t events_dropped = 0;
    Histogram submit_to_pop;

    Slot(AgentId i, std::unique_ptr<Strategy> s, Price px, uint64_t seed, uint64_t market_seed)
        : id(i), strategy(std::move(s)), ctx(i, in, out, px, seed, market_seed) {}
};

// ===================== Engine-thread state =====================
struct Engine::Impl {
    OrderBook book;
    Logger logger;
    Seq ev_seq = 0;
    uint64_t commands = 0;
    uint64_t trades = 0;
    uint64_t events_published = 0;
    Qty volume = 0;
    Price last_px;
    Histogram match;
    std::vector<Fill> fills;
    std::vector<CancelInfo> stp;
    std::vector<Command> batch;
    Price top_bid = -1, top_ask = -1;
    Qty top_bq = 0, top_aq = 0;

    explicit Impl(const EngineConfig& c)
        : book(c.min_px, c.max_px), logger(c.trade_log, c.cmd_log), last_px(c.initial_px) {
        fills.reserve(256);
        stp.reserve(64);
        batch.reserve(4096);
    }
};

Engine::Engine(EngineConfig cfg) : cfg_(std::move(cfg)), impl_(std::make_unique<Impl>(cfg_)) {
    if (cfg_.min_px < 0 || cfg_.max_px <= cfg_.min_px) throw std::invalid_argument("bad price band");
    if (cfg_.initial_px < cfg_.min_px || cfg_.initial_px >= cfg_.max_px) throw std::invalid_argument("initial_px outside band");
}

Engine::~Engine() = default;

AgentId Engine::add_agent(std::unique_ptr<Strategy> s) {
    if (running_.load()) throw std::logic_error("add_agent during run");
    const AgentId id = static_cast<AgentId>(slots_.size());
    const uint64_t seed = cfg_.seed * 1000003ULL + id * 7919ULL + 1;
    slots_.push_back(std::make_unique<Slot>(id, std::move(s), cfg_.initial_px, seed, cfg_.seed));
    return id;
}

// ===================== Event helpers =====================
void Engine::fill_top(Event& ev) const {
    const OrderBook& b = impl_->book;
    ev.bid_px = b.has_bid() ? b.best_bid() : 0;
    ev.bid_qty = b.best_bid_qty();
    ev.ask_px = b.has_ask() ? b.best_ask() : 0;
    ev.ask_qty = b.best_ask_qty();
}

void Engine::broadcast(Event ev) {
    ev.seq = ++impl_->ev_seq;
    ev.ts = now_ns();
    ++impl_->events_published;
    for (auto& s : slots_)
        if (!s->out.try_push(ev)) ++s->events_dropped;
}

void Engine::send(AgentId to, Event ev) {
    ev.seq = ++impl_->ev_seq;
    ev.ts = now_ns();
    ++impl_->events_published;
    Slot& s = *slots_[to];
    if (!s.out.try_push(ev)) ++s.events_dropped;
}

// ===================== Command processing =====================
void Engine::process(const Command& c) {
    Impl& im = *impl_;
    Slot& owner = *slots_[c.agent];
    ++owner.orders_processed;
    ++im.commands;
    im.logger.log_cmd(c);

    if (c.kind == CmdKind::Cancel) {
        CancelInfo ci;
        RejectReason why;
        Event ev;
        ev.order_id = c.id;
        if (im.book.cancel(c.id, c.agent, ci, why)) {
            ev.kind = EventKind::Cancelled;
            ev.remaining = ci.remaining;
        } else {
            ev.kind = EventKind::Rejected;
            ev.reason = why;
            ++owner.orders_rejected;
        }
        send(c.agent, ev);
        return;
    }

    im.fills.clear();
    im.stp.clear();
    const AddResult res = im.book.add(c, im.fills, im.stp);

    for (const CancelInfo& ci : im.stp) {
        Event ev;
        ev.kind = EventKind::Cancelled;
        ev.order_id = ci.id;
        ev.remaining = ci.remaining;
        send(ci.agent, ev);
    }

    if (!res.accepted) {
        ++owner.orders_rejected;
        Event ev;
        ev.kind = EventKind::Rejected;
        ev.order_id = c.id;
        ev.reason = res.reason;
        ev.side = c.side;
        ev.px = c.px;
        ev.qty = c.qty;
        send(c.agent, ev);
        return;
    }

    {
        Event ack;
        ack.kind = EventKind::Ack;
        ack.order_id = c.id;
        ack.side = c.side;
        ack.px = c.px;
        ack.qty = res.filled;
        ack.remaining = res.resting;
        send(c.agent, ack);
    }

    Qty taker_remaining = c.qty;
    for (const Fill& f : im.fills) {
        taker_remaining -= f.qty;
        const AgentId buyer = f.taker_side == Side::Buy ? f.taker : f.maker;
        const AgentId seller = f.taker_side == Side::Buy ? f.maker : f.taker;
        const double notional = to_dollars(f.px) * f.qty;

        Slot& b = *slots_[buyer];
        Slot& s = *slots_[seller];
        b.cash -= notional; b.position += f.qty; ++b.fills; b.volume += f.qty;
        s.cash += notional; s.position -= f.qty; ++s.fills; s.volume += f.qty;

        Event mk;
        mk.kind = EventKind::Fill;
        mk.order_id = f.maker_id;
        mk.side = opposite(f.taker_side);
        mk.is_maker = 1;
        mk.px = f.px;
        mk.qty = f.qty;
        mk.remaining = f.maker_remaining;
        send(f.maker, mk);

        Event tk;
        tk.kind = EventKind::Fill;
        tk.order_id = f.taker_id;
        tk.side = f.taker_side;
        tk.is_maker = 0;
        tk.px = f.px;
        tk.qty = f.qty;
        tk.remaining = taker_remaining;
        send(f.taker, tk);

        Event tr;
        tr.kind = EventKind::Trade;
        tr.side = f.taker_side;
        tr.px = f.px;
        tr.qty = f.qty;
        fill_top(tr);
        broadcast(tr);

        ++im.trades;
        im.volume += f.qty;
        im.last_px = f.px;
        im.logger.log_trade(TradeRecord{now_ns(), im.ev_seq, f.px, f.qty, buyer, seller, f.taker_side});
    }
}

// ===================== Engine thread =====================
void Engine::engine_loop() {
    if (cfg_.pin_threads) pin_to(1);
    Impl& im = *impl_;

    {
        Event start;
        start.kind = EventKind::SessionStart;
        start.px = cfg_.initial_px;
        fill_top(start);
        broadcast(start);
    }

    const size_t n = slots_.size();
    size_t rr = 0;
    std::vector<Command>& batch = im.batch;

    while (true) {
        // Load before draining: anything pushed before producers_done was set is visible.
        const bool done = producers_done_.load(std::memory_order_acquire);
        batch.clear();
        for (size_t i = 0; i < n; ++i) {
            Slot& s = *slots_[(rr + i) % n];
            Command c;
            int pops = 0;
            while (pops < 256 && s.in.try_pop(c)) {
                s.submit_to_pop.record(now_ns() - c.t_submit);
                batch.push_back(c);
                ++pops;
            }
        }
        rr = (rr + 1) % n;

        if (batch.empty()) {
            if (done) break;
            idle(cfg_.engine_idle);
            continue;
        }

        std::stable_sort(batch.begin(), batch.end(),
                         [](const Command& a, const Command& b) { return a.t_submit < b.t_submit; });

        for (const Command& c : batch) {
            const Ts t0 = now_ns();
            process(c);
            im.match.record(now_ns() - t0);
        }

        const OrderBook& b = im.book;
        const Price bb = b.has_bid() ? b.best_bid() : -1;
        const Price ba = b.has_ask() ? b.best_ask() : -1;
        const Qty bq = b.best_bid_qty(), aq = b.best_ask_qty();
        if (bb != im.top_bid || ba != im.top_ask || bq != im.top_bq || aq != im.top_aq) {
            im.top_bid = bb; im.top_ask = ba; im.top_bq = bq; im.top_aq = aq;
            Event ev;
            ev.kind = EventKind::BookUpdate;
            ev.px = im.last_px;
            fill_top(ev);
            broadcast(ev);
        }
    }
}

// ===================== Agent thread =====================
void Engine::agent_loop(Slot& s) {
    if (cfg_.pin_threads) pin_to(2 + static_cast<int>(s.id));
    s.strategy->on_start(s.ctx);
    Event ev;
    while (running_.load(std::memory_order_relaxed)) {
        int n = 0;
        while (n < 256 && s.ctx.poll(ev)) {
            s.ctx.dispatch(ev, *s.strategy);
            ++n;
        }
        if (n == 0) {
            s.strategy->on_idle(s.ctx);
            idle(cfg_.agent_idle);
        }
    }
}

// ===================== Session =====================
RunReport Engine::run() {
    if (slots_.empty()) throw std::logic_error("no agents registered");
    running_.store(true, std::memory_order_release);
    producers_done_.store(false, std::memory_order_release);
    stop_requested_.store(false, std::memory_order_release);
    impl_->logger.start();

    const Ts t_start = now_ns();
    for (auto& s : slots_) s->ctx.begin_session(t_start);
    std::thread eng([this] { engine_loop(); });
    for (auto& s : slots_) s->thread = std::thread([this, &s] { agent_loop(*s); });

    const Ts deadline = t_start + static_cast<Ts>(cfg_.session_seconds * 1e9);
    while (now_ns() < deadline && !stop_requested_.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // 1) stop producers, 2) join them, 3) tell the engine no more input, 4) drain and join.
    running_.store(false, std::memory_order_release);
    for (auto& s : slots_) s->thread.join();
    producers_done_.store(true, std::memory_order_release);
    eng.join();
    const Ts t_end = now_ns();
    impl_->logger.stop();

    return build_report(t_start, t_end);
}

RunReport Engine::build_report(Ts t_start, Ts t_end) {
    Impl& im = *impl_;
    RunReport r;
    r.session_seconds = static_cast<double>(t_end - t_start) / 1e9;
    r.seed = cfg_.seed;
    r.commands = im.commands;
    r.trades = im.trades;
    r.volume = im.volume;
    r.events_published = im.events_published;
    r.log_dropped_trades = im.logger.dropped_trades();
    r.log_dropped_cmds = im.logger.dropped_cmds();
    r.initial_px = cfg_.initial_px;
    r.last_px = im.last_px;
    r.final_bid = im.book.has_bid() ? im.book.best_bid() : 0;
    r.final_ask = im.book.has_ask() ? im.book.best_ask() : 0;
    r.final_bid_qty = im.book.best_bid_qty();
    r.final_ask_qty = im.book.best_ask_qty();
    r.mark_px = (im.book.has_bid() && im.book.has_ask()) ? (r.final_bid + r.final_ask) / 2 : im.last_px;
    r.open_orders = im.book.open_orders();
    r.book_checksum = im.book.checksum();
    r.commands_per_sec = r.session_seconds > 0 ? r.commands / r.session_seconds : 0;
    r.trades_per_sec = r.session_seconds > 0 ? r.trades / r.session_seconds : 0;
    r.match = LatencySummary::from(im.match);

    Histogram merged;
    for (auto& sp : slots_) {
        Slot& s = *sp;
        AgentReport a;
        a.id = s.id;
        a.name = s.strategy->name();
        a.cash = s.cash;
        a.position = s.position;
        a.pnl = s.cash + to_dollars(r.mark_px) * s.position - INITIAL_CASH;
        a.fills = s.fills;
        a.volume = s.volume;
        a.orders_processed = s.orders_processed;
        a.orders_rejected = s.orders_rejected;
        a.events_dropped = s.events_dropped;
        r.events_dropped += s.events_dropped;
        const AgentStats& st = s.ctx.stats();
        a.agent_position = s.ctx.position();
        a.agent_cash = s.ctx.cash();
        a.submitted = st.submitted;
        a.queue_full = st.queue_full;
        a.cancels_sent = st.cancels_sent;
        a.events = st.events;
        a.delivery = LatencySummary::from(st.delivery);
        a.react = LatencySummary::from(st.react);
        a.event_age = LatencySummary::from(st.event_age);
        a.submit_to_pop = LatencySummary::from(s.submit_to_pop);
        merged.merge(s.submit_to_pop);
        r.agents.push_back(std::move(a));
    }
    r.submit_to_pop = LatencySummary::from(merged);
    return r;
}
