#pragma once

#include "strategy.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// key=value parameters from an agent spec such as "mm:spread=3,size=5".
struct Params {
    std::map<std::string, std::string> kv;
    double get(const std::string& key, double def) const;
    std::string str(const std::string& key, const std::string& def) const;
};

// "name" or "name:k=v,k=v". Returns false on a malformed spec.
bool parse_agent_spec(const std::string& spec, std::string& name, Params& params);

// nullptr if the name is unknown.
std::unique_ptr<Strategy> make_strategy(const std::string& name, const Params& params);

std::string strategy_help();

// Fixed-window simple moving average with O(1) update.
class RollingSMA {
public:
    explicit RollingSMA(size_t period) : buf_(period, 0.0) {}
    void push(double x) {
        if (n_ < buf_.size()) { ++n_; } else { sum_ -= buf_[i_]; }
        buf_[i_] = x;
        sum_ += x;
        i_ = (i_ + 1) % buf_.size();
    }
    bool full() const { return n_ == buf_.size(); }
    double value() const { return n_ ? sum_ / static_cast<double>(n_) : 0.0; }

private:
    std::vector<double> buf_;
    size_t n_ = 0;
    size_t i_ = 0;
    double sum_ = 0.0;
};
