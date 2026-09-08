#pragma once

#include "spsc_queue.h"
#include "types.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>

// Off-hot-path CSV writer. The engine pushes records into SPSC queues and
// never blocks; if a queue is full the record is dropped and counted.
class Logger {
public:
    Logger(std::string trade_path, std::string cmd_path);
    ~Logger();

    bool trades_enabled() const { return !trade_path_.empty(); }
    bool cmds_enabled() const { return !cmd_path_.empty(); }

    void start();
    void stop(); // drains, flushes, joins

    // Engine thread only.
    void log_trade(const TradeRecord& r) { if (!trades_q_.try_push(r)) ++dropped_trades_; }
    void log_cmd(const Command& c) { if (!cmds_q_.try_push(c)) ++dropped_cmds_; }

    uint64_t dropped_trades() const { return dropped_trades_; }
    uint64_t dropped_cmds() const { return dropped_cmds_; }
    uint64_t written_trades() const { return written_trades_.load(); }
    uint64_t written_cmds() const { return written_cmds_.load(); }

private:
    void run();

    std::string trade_path_;
    std::string cmd_path_;
    SPSCQueue<TradeRecord, 1 << 16> trades_q_;
    SPSCQueue<Command, 1 << 16> cmds_q_;
    uint64_t dropped_trades_ = 0;
    uint64_t dropped_cmds_ = 0;
    std::atomic<uint64_t> written_trades_{0};
    std::atomic<uint64_t> written_cmds_{0};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
