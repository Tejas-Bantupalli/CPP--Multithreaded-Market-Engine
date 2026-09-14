#include "api.h"
#include <cassert>
#include <deque>
#include <memory>
#include <random>
#include <numeric>

// Compare candidate state after every operation with an independent reference.
// Includes warmup, repeated wraparound, negative values and independent instances.
int main() {
    std::mt19937_64 rng(72381);
    for (int trial = 0; trial < 4; ++trial) {
        std::unique_ptr<InfraPolicy> a(make_infra()), b(make_infra());
        assert(a && b && a->sum() == 0 && b->sum() == 0);
        assert(a->batch_size() >= 1 && a->batch_size() <= 4096);
        std::deque<int64_t> reference;
        for (int i = 0; i < 100000; ++i) {
            const int64_t p = static_cast<int64_t>(rng() % 200001) - 100000;
            reference.push_back(p);
            if (reference.size() > INFRA_WINDOW) reference.pop_front();
            a->push(p);
            assert(a->sum() == std::accumulate(reference.begin(), reference.end(), int64_t{0}));
            assert(b->sum() == 0);
        }
        a->idle();
    }
}
