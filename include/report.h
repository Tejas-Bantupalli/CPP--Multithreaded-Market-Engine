#pragma once

#include "histogram.h"
#include "types.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

struct LatencySummary {
    uint64_t count = 0;
    double mean_ns = 0;
    uint64_t p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
    static LatencySummary from(const Histogram& h);
};

struct AgentReport {
    AgentId id = 0;
    std::string name;
    // Engine-side truth.
    double cash = 0;
    Qty position = 0;
    double pnl = 0;
    uint64_t fills = 0;
    Qty volume = 0;
    double fees = 0;            // signed; negative means net rebates earned
    uint64_t maker_fills = 0;
    uint64_t taker_fills = 0;
    uint64_t orders_processed = 0; // commands popped by the engine
    uint64_t orders_rejected = 0;  // by the book
    uint64_t events_dropped = 0;   // inbound queue full
    // Agent-side view.
    Qty agent_position = 0;
    double agent_cash = 0;
    uint64_t submitted = 0;
    uint64_t queue_full = 0;
    uint64_t cancels_sent = 0;
    uint64_t events = 0;
    LatencySummary delivery, react, event_age, submit_to_pop;
};

struct RunReport {
    double session_seconds = 0;
    uint64_t seed = 0;
    std::string fanout;
    uint64_t commands = 0;
    uint64_t trades = 0;
    Qty volume = 0;
    uint64_t events_published = 0;
    uint64_t events_dropped = 0;
    uint64_t log_dropped_trades = 0;
    uint64_t log_dropped_cmds = 0;
    Price initial_px = 0;
    Price last_px = 0;
    Price mark_px = 0;
    Price final_bid = 0, final_ask = 0;
    Qty final_bid_qty = 0, final_ask_qty = 0;
    size_t open_orders = 0;
    uint64_t book_checksum = 0;
    double commands_per_sec = 0;
    double trades_per_sec = 0;
    LatencySummary match;          // engine pop -> done
    LatencySummary submit_to_pop;  // merged over agents
    std::vector<AgentReport> agents;
};

void print_report(const RunReport& r, std::ostream& os);
std::string to_json(const RunReport& r);
bool write_json(const RunReport& r, const std::string& path);
