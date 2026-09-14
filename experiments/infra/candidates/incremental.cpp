// Generation 1 hypothesis after the unchanged control:
// delivery dominates p99; a running sum may reduce compute but will probably
// have a modest effect on the full path. Preserve yield and batch 256 to isolate it.
#include "api.h"
#include <array>
#include <thread>
class Incremental final : public InfraPolicy {
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
    void idle() override { std::this_thread::yield(); }
};
extern "C" InfraPolicy* make_infra() { return new Incremental; }
