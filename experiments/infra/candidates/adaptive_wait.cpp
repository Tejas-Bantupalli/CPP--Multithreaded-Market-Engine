// Generation 2, after generation 1 reduced p99 reaction 328 -> 288 ns but
// failed to improve publish-to-submit. Test a bounded spin-then-yield policy
// aimed at delivery delay. Trade events reset a 64-empty-poll spin budget.
#include "api.h"
#include <array>
#include <thread>
class AdaptiveWait final : public InfraPolicy {
    std::array<int64_t, INFRA_WINDOW> prices_{};
    std::size_t next_ = 0;
    int64_t total_ = 0;
    unsigned empty_ = 0;
public:
    void push(int64_t p) override {
        total_ += p - prices_[next_];
        prices_[next_] = p;
        next_ = (next_ + 1) % INFRA_WINDOW;
        empty_ = 0;
    }
    int64_t sum() const override { return total_; }
    unsigned batch_size() const override { return 256; }
    void idle() override {
        if (empty_ < 64) { ++empty_; return; }
        std::this_thread::yield();
    }
};
extern "C" InfraPolicy* make_infra() { return new AdaptiveWait; }
