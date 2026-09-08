#include "engine.h"
#include "plugin_loader.h"
#include "strategies.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void usage() {
    std::cout <<
        "usage: market_engine [options]\n"
        "  --seconds S        session length (default 5)\n"
        "  --seed N           base seed for agent RNGs (default 1)\n"
        "  --agent SPEC       add an agent; repeatable. SPEC = name[:k=v,k=v]\n"
        "  --plugin PATH[:k=v] add an agent from a compiled plugin (see include/plugin.h); repeatable\n"
        "  --px P             initial price in dollars (default 100.00)\n"
        "  --idle MODE        agent idle policy: spin | yield | sleep (default yield)\n"
        "  --engine-idle MODE engine idle policy (default yield)\n"
        "  --pin              pin threads to cores (Linux only)\n"
        "  --fanout MODE      broadcast path: spsc (one push per agent) | multicast (shared ring, default spsc)\n"
        "  --json PATH        write the report as JSON\n"
        "  --trades PATH      write a trade log CSV (off the hot path)\n"
        "  --cmdlog PATH      write every processed command as CSV, replayable with build/replay\n"
        "  --market k=v,...   default parameters applied to every agent (e.g. the noise traders'\n"
        "                     shared fundamental: fsigma, jump, jumpsize, drift)\n"
        "  --quiet            print only the summary line\n"
        "strategies and their parameters:\n" << strategy_help() <<
        "default agent set: mm noise noise noise momentum meanrev\n";
}

bool parse_idle(const std::string& s, IdlePolicy& out) {
    if (s == "spin") { out = IdlePolicy::Spin; return true; }
    if (s == "yield") { out = IdlePolicy::Yield; return true; }
    if (s == "sleep") { out = IdlePolicy::Sleep; return true; }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    EngineConfig cfg;
    struct AgentSpec { bool plugin; std::string spec; };
    std::vector<AgentSpec> specs;
    std::string json_path;
    std::string market_spec;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << a << " needs " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--seconds") cfg.session_seconds = std::stod(need("seconds"));
        else if (a == "--seed") cfg.seed = std::stoull(need("a seed"));
        else if (a == "--agent") specs.push_back({false, need("a spec")});
        else if (a == "--plugin") specs.push_back({true, need("a path[:k=v]")});
        else if (a == "--px") cfg.initial_px = to_ticks(std::stod(need("a price")));
        else if (a == "--idle") { if (!parse_idle(need("a mode"), cfg.agent_idle)) { std::cerr << "bad idle mode\n"; return 2; } }
        else if (a == "--engine-idle") { if (!parse_idle(need("a mode"), cfg.engine_idle)) { std::cerr << "bad idle mode\n"; return 2; } }
        else if (a == "--pin") cfg.pin_threads = true;
        else if (a == "--fanout") {
            const std::string f = need("spsc|multicast");
            if (f == "spsc") cfg.fanout = Fanout::Spsc;
            else if (f == "multicast") cfg.fanout = Fanout::Multicast;
            else { std::cerr << "bad fanout mode\n"; return 2; }
        }
        else if (a == "--json") json_path = need("a path");
        else if (a == "--trades") cfg.trade_log = need("a path");
        else if (a == "--cmdlog") cfg.cmd_log = need("a path");
        else if (a == "--market") market_spec = need("k=v,k=v");
        else if (a == "--quiet") quiet = true;
        else { std::cerr << "unknown option " << a << "\n"; usage(); return 2; }
    }

    if (specs.empty())
        for (const char* d : {"mm", "noise", "noise", "noise", "momentum", "meanrev"}) specs.push_back({false, d});

    // --market parameters are defaults for every agent; an agent's own spec overrides them.
    Params market;
    if (!market_spec.empty()) {
        std::string dummy;
        if (!parse_agent_spec("market:" + market_spec, dummy, market)) { std::cerr << "bad --market spec\n"; return 2; }
    }

    Engine engine(cfg);
    for (const AgentSpec& as : specs) {
        std::string name;
        Params params;
        if (!parse_agent_spec(as.spec, name, params)) { std::cerr << "bad agent spec: " << as.spec << "\n"; return 2; }
        for (const auto& kv : market.kv) params.kv.emplace(kv.first, kv.second);
        std::unique_ptr<Strategy> s;
        if (as.plugin) {
            std::string err;
            s = load_plugin(name, params, err);
            if (!s) { std::cerr << "plugin " << name << ": " << err << "\n"; return 2; }
        } else {
            s = make_strategy(name, params);
            if (!s) { std::cerr << "unknown strategy: " << name << "\n"; return 2; }
        }
        engine.add_agent(std::move(s));
    }

    const RunReport r = engine.run();

    if (quiet) {
        std::cout << "seconds=" << r.session_seconds << " commands=" << r.commands
                  << " trades=" << r.trades << " last_px=" << to_dollars(r.last_px)
                  << " submit_to_pop_p99=" << r.submit_to_pop.p99 << "ns match_p99=" << r.match.p99 << "ns\n";
    } else {
        print_report(r, std::cout);
    }
    if (!json_path.empty() && !write_json(r, json_path)) {
        std::cerr << "could not write " << json_path << "\n";
        return 1;
    }
    return 0;
}
