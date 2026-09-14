#include "logger.h"

#include <chrono>
#include <cstdio>

Logger::Logger(std::string trade_path, std::string cmd_path)
    : trade_path_(std::move(trade_path)), cmd_path_(std::move(cmd_path)) {}

Logger::~Logger() { stop(); }

void Logger::start() {
    if (!trades_enabled() && !cmds_enabled()) return;
    if (thread_.joinable()) return;
    stopping_.store(false);
    thread_ = std::thread([this] { run(); });
}

void Logger::stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true, std::memory_order_release);
    thread_.join();
}

void Logger::run() {
    std::FILE* tf = trades_enabled() ? std::fopen(trade_path_.c_str(), "w") : nullptr;
    std::FILE* cf = cmds_enabled() ? std::fopen(cmd_path_.c_str(), "w") : nullptr;
    if (tf) std::fputs("ts_ns,seq,px_ticks,qty,buyer,seller,aggressor\n", tf);
    if (cf) std::fputs("kind,agent,order_id,side,tif,px_ticks,qty,t_submit_ns\n", cf);

    TradeRecord r;
    Command c;
    while (true) {
        bool any = false;
        while (trades_q_.try_pop(r)) {
            any = true;
            if (tf) std::fprintf(tf, "%lld,%llu,%lld,%d,%u,%u,%s\n",
                                 (long long)r.ts, (unsigned long long)r.seq, (long long)r.px, r.qty,
                                 r.buyer, r.seller, side_name(r.aggressor));
            written_trades_.fetch_add(1, std::memory_order_relaxed);
        }
        while (cmds_q_.try_pop(c)) {
            any = true;
            if (cf) std::fprintf(cf, "%s,%u,%llu,%s,%s,%lld,%d,%lld\n",
                                 c.kind == CmdKind::New ? "new" : "cancel", c.agent,
                                 (unsigned long long)c.id, side_name(c.side),
                                 c.tif == TimeInForce::GTC ? "gtc" : "ioc",
                                 (long long)c.px, c.qty, (long long)c.t_submit);
            written_cmds_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!any) {
            if (stopping_.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    if (tf) std::fclose(tf);
    if (cf) std::fclose(cf);
}
