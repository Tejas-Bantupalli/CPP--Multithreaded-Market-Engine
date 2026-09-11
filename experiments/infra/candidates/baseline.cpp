// Baseline: compact fixed storage, rescanned sum, batch 256 and yield on idle.
#include "api.h"
#include <array>
#include <thread>
class Baseline final : public InfraPolicy {
    std::array<int64_t, INFRA_WINDOW> prices_{};
    std::size_t next_ = 0;
public:
    void push(int64_t p) override { prices_[next_] = p; next_ = (next_ + 1) % INFRA_WINDOW; }
    int64_t sum() const override { int64_t s = 0; for (auto p : prices_) s += p; return s; }
    unsigned batch_size() const override { return 256; }
    void idle() override { std::this_thread::yield(); }
};
extern "C" InfraPolicy* make_infra() { return new Baseline; }
