#pragma once

#include "report.h"
#include "transport.h"
#include "strategy.h"
#include "types.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

enum class IdlePolicy { Spin, Yield, Sleep };
// How broadcast events (Trade, BookUpdate, SessionStart) reach agents.
//   Spsc:      one push per agent into its private queue (O(agents) per broadcast)
//   Multicast: one publish into a shared ring; each agent reads with its own cursor
enum class Fanout { Spsc, Multicast };

struct EngineConfig {
    Price initial_px = 10000;   // 100.00
    Price min_px = 1;
    Price max_px = 100000;      // 1000.00 exclusive
    double session_seconds = 5.0;
    uint64_t seed = 1;
    IdlePolicy agent_idle = IdlePolicy::Yield;
    IdlePolicy engine_idle = IdlePolicy::Yield;
    bool pin_threads = false;   // Linux only
    Fanout fanout = Fanout::Spsc;
    // Limit-up/limit-down: reject new orders priced more than this fraction away from
    // the initial price. 0 disables. Keeps a feedback loop between agents from
    // dislocating a thin book by tens of percent inside one session.
    double collar = 0.05;
    // Venue fees in dollars per unit, applied on every fill. Signed: negative is a
    // rebate paid to the agent, positive is a fee charged. Real venues pay resting
    // liquidity and charge liquidity takers, which is what makes queue position
    // worth money and therefore what makes speed pay.
    TransportKind transport = TransportKind::Spsc;  // default for agents added without one
    double maker_fee = -0.002;
    double taker_fee = 0.003;
    std::string trade_log;      // CSV path or empty
    std::string cmd_log;        // CSV path or empty
};

// Owns the book, the agents, their queues, and the session lifecycle.
class Engine {
public:
    explicit Engine(EngineConfig cfg);
    ~Engine();

    // Register before run(). Returns the agent id.
    AgentId add_agent(std::unique_ptr<Strategy> s);
    // Same, but this agent's private channels use the given plumbing.
    AgentId add_agent(std::unique_ptr<Strategy> s, TransportKind t);
    size_t agent_count() const { return slots_.size(); }

    // Blocks for the session, shuts down cleanly, returns the report.
    RunReport run();

    // Optional early stop from another thread.
    void request_stop() { stop_requested_.store(true, std::memory_order_release); }

private:
    struct Slot;
    struct Impl;
    void engine_loop();
    void agent_loop(Slot& s);
    void process(const Command& c);
    void broadcast(Event ev);
    void send(AgentId to, Event ev);
    void fill_top(Event& ev) const;
    RunReport build_report(Ts t_start, Ts t_end);

    EngineConfig cfg_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<bool> producers_done_{false};
    std::atomic<bool> stop_requested_{false};
};
