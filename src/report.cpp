#include "report.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

LatencySummary LatencySummary::from(const Histogram& h) {
    LatencySummary s;
    s.count = h.count();
    s.mean_ns = h.mean();
    s.p50 = h.percentile(0.50);
    s.p90 = h.percentile(0.90);
    s.p99 = h.percentile(0.99);
    s.p999 = h.percentile(0.999);
    s.max = h.max();
    return s;
}

namespace {

void lat_row(std::ostream& os, const std::string& label, const LatencySummary& s) {
    os << "  " << std::left << std::setw(16) << label << std::right
       << std::setw(10) << s.count
       << std::setw(11) << std::fixed << std::setprecision(0) << s.mean_ns
       << std::setw(10) << s.p50
       << std::setw(10) << s.p90
       << std::setw(10) << s.p99
       << std::setw(10) << s.p999
       << std::setw(12) << s.max << "\n";
}

void lat_header(std::ostream& os) {
    os << "  " << std::left << std::setw(16) << "latency (ns)" << std::right
       << std::setw(10) << "count" << std::setw(11) << "mean" << std::setw(10) << "p50"
       << std::setw(10) << "p90" << std::setw(10) << "p99" << std::setw(10) << "p99.9"
       << std::setw(12) << "max" << "\n";
}

std::string lat_json(const LatencySummary& s) {
    std::ostringstream o;
    o << "{\"count\":" << s.count << ",\"mean_ns\":" << std::fixed << std::setprecision(1) << s.mean_ns
      << ",\"p50\":" << s.p50 << ",\"p90\":" << s.p90 << ",\"p99\":" << s.p99
      << ",\"p999\":" << s.p999 << ",\"max\":" << s.max << "}";
    return o.str();
}

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        o += c;
    }
    return o + "\"";
}

} // namespace

void print_report(const RunReport& r, std::ostream& os) {
    os << std::fixed;
    os << "\n=== SESSION ===\n"
       << "  seconds: " << std::setprecision(3) << r.session_seconds
       << " | seed: " << r.seed << " | fanout: " << r.fanout
       << " | commands: " << r.commands << " (" << std::setprecision(0) << r.commands_per_sec << "/s)"
       << " | trades: " << r.trades << " (" << r.trades_per_sec << "/s)"
       << " | volume: " << r.volume << "\n"
       << "  price: " << std::setprecision(2) << to_dollars(r.initial_px) << " -> " << to_dollars(r.last_px)
       << " | book: " << r.final_bid_qty << " @ " << to_dollars(r.final_bid)
       << " / " << r.final_ask_qty << " @ " << to_dollars(r.final_ask)
       << " | open orders: " << r.open_orders
       << " | checksum: " << std::hex << r.book_checksum << std::dec << "\n"
       << "  events published: " << r.events_published << " | dropped: " << r.events_dropped
       << " | log dropped trades/cmds: " << r.log_dropped_trades << "/" << r.log_dropped_cmds << "\n";

    os << "\n=== PNL (mark @ " << std::setprecision(2) << to_dollars(r.mark_px) << ") ===\n";
    os << "  " << std::left << std::setw(4) << "id" << std::setw(12) << "strategy" << std::right
       << std::setw(12) << "pnl" << std::setw(8) << "pos" << std::setw(14) << "cash"
       << std::setw(8) << "fills" << std::setw(7) << "mkr" << std::setw(7) << "tkr" << std::setw(9) << "fees"
       << std::setw(8) << "volume" << std::setw(10) << "orders"
       << std::setw(8) << "rej" << std::setw(9) << "q_full" << std::setw(9) << "ev_drop" << "\n";
    for (const AgentReport& a : r.agents) {
        os << "  " << std::left << std::setw(4) << a.id << std::setw(12) << a.name << std::right
           << std::setw(12) << std::setprecision(2) << a.pnl
           << std::setw(8) << a.position
           << std::setw(14) << a.cash
           << std::setw(8) << a.fills
           << std::setw(7) << a.maker_fills
           << std::setw(7) << a.taker_fills
           << std::setw(9) << a.fees
           << std::setw(8) << a.volume
           << std::setw(10) << a.orders_processed
           << std::setw(8) << a.orders_rejected
           << std::setw(9) << a.queue_full
           << std::setw(9) << a.events_dropped << "\n";
    }

    os << "\n=== ENGINE LATENCY ===\n";
    lat_header(os);
    lat_row(os, "submit_to_pop", r.submit_to_pop);
    lat_row(os, "match", r.match);

    for (const AgentReport& a : r.agents) {
        os << "\n=== AGENT " << a.id << " (" << a.name << ") ===\n";
        lat_header(os);
        lat_row(os, "delivery", a.delivery);
        lat_row(os, "react", a.react);
        lat_row(os, "event_age", a.event_age);
        lat_row(os, "submit_to_pop", a.submit_to_pop);
    }
    os << "\n";
}

std::string to_json(const RunReport& r) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(4);
    o << "{\n"
      << "  \"session_seconds\": " << r.session_seconds << ",\n"
      << "  \"seed\": " << r.seed << ",\n"
      << "  \"fanout\": \"" << r.fanout << "\",\n"
      << "  \"commands\": " << r.commands << ",\n"
      << "  \"trades\": " << r.trades << ",\n"
      << "  \"volume\": " << r.volume << ",\n"
      << "  \"commands_per_sec\": " << r.commands_per_sec << ",\n"
      << "  \"trades_per_sec\": " << r.trades_per_sec << ",\n"
      << "  \"events_published\": " << r.events_published << ",\n"
      << "  \"events_dropped\": " << r.events_dropped << ",\n"
      << "  \"log_dropped_trades\": " << r.log_dropped_trades << ",\n"
      << "  \"log_dropped_cmds\": " << r.log_dropped_cmds << ",\n"
      << "  \"initial_px\": " << to_dollars(r.initial_px) << ",\n"
      << "  \"last_px\": " << to_dollars(r.last_px) << ",\n"
      << "  \"mark_px\": " << to_dollars(r.mark_px) << ",\n"
      << "  \"final_bid\": " << to_dollars(r.final_bid) << ",\n"
      << "  \"final_bid_qty\": " << r.final_bid_qty << ",\n"
      << "  \"final_ask\": " << to_dollars(r.final_ask) << ",\n"
      << "  \"final_ask_qty\": " << r.final_ask_qty << ",\n"
      << "  \"open_orders\": " << r.open_orders << ",\n"
      << "  \"book_checksum\": \"" << std::hex << r.book_checksum << std::dec << "\",\n"
      << "  \"latency\": {\"submit_to_pop\": " << lat_json(r.submit_to_pop)
      << ", \"match\": " << lat_json(r.match) << "},\n"
      << "  \"agents\": [\n";
    for (size_t i = 0; i < r.agents.size(); ++i) {
        const AgentReport& a = r.agents[i];
        o << "    {\"id\": " << a.id << ", \"name\": " << json_str(a.name)
          << ", \"pnl\": " << a.pnl << ", \"position\": " << a.position << ", \"cash\": " << a.cash
          << ", \"fills\": " << a.fills << ", \"volume\": " << a.volume
          << ", \"fees\": " << a.fees << ", \"maker_fills\": " << a.maker_fills
          << ", \"taker_fills\": " << a.taker_fills
          << ", \"orders_processed\": " << a.orders_processed << ", \"orders_rejected\": " << a.orders_rejected
          << ", \"submitted\": " << a.submitted << ", \"queue_full\": " << a.queue_full
          << ", \"cancels_sent\": " << a.cancels_sent << ", \"events\": " << a.events
          << ", \"events_dropped\": " << a.events_dropped
          << ", \"agent_position\": " << a.agent_position << ", \"agent_cash\": " << a.agent_cash
          << ",\n     \"latency\": {\"delivery\": " << lat_json(a.delivery)
          << ", \"react\": " << lat_json(a.react)
          << ", \"event_age\": " << lat_json(a.event_age)
          << ", \"submit_to_pop\": " << lat_json(a.submit_to_pop) << "}}"
          << (i + 1 < r.agents.size() ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    return o.str();
}

bool write_json(const RunReport& r, const std::string& path) {
    std::ofstream f(path);
    if (!f) return false;
    f << to_json(r);
    return static_cast<bool>(f);
}
