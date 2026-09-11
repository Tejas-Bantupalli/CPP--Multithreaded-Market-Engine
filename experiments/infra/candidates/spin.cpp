// Generation 3: bounded spin-then-yield did not improve full-path latency.
// Keep exact incremental calculation and batch 256. Test continuous polling
// to expose the latency/CPU/contention tradeoff, without assuming it wins.
#include "api.h"
#include <array>
class Spin final : public InfraPolicy {
    std::array<int64_t, INFRA_WINDOW> prices_{};
    std::size_t next_ = 0;
    int64_t total_ = 0;
public:
    void push(int64_t p) override {
        total_ += p - prices_[next_];
        prices_[next_] = p;
        next_ = (next_ + 1) % INFRA_WINDOW;
    }
    int64_t sum() const override { return total_; }
    unsigned batch_size() const override { return 256; }
    void idle() override {}
};
extern "C" InfraPolicy* make_infra() { return new Spin; }
