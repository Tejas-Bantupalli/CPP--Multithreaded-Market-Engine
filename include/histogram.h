#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

// Log-bucketed latency histogram, roughly 3% relative resolution.
// Single writer, no atomics: each histogram belongs to exactly one thread and is
// read only after that thread has been joined.
class Histogram {
public:
    static constexpr int SUB_BITS = 5;
    static constexpr int SUB = 1 << SUB_BITS;
    static constexpr int BUCKETS = (64 - SUB_BITS + 1) * SUB;

    Histogram() { std::memset(counts_, 0, sizeof(counts_)); }

    void record(int64_t v_signed) {
        const uint64_t v = v_signed < 0 ? 0 : static_cast<uint64_t>(v_signed);
        ++counts_[index_of(v)];
        ++count_;
        sum_ += v;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
    }

    uint64_t count() const { return count_; }
    uint64_t max() const { return count_ ? max_ : 0; }
    uint64_t min() const { return count_ ? min_ : 0; }
    double mean() const { return count_ ? static_cast<double>(sum_) / static_cast<double>(count_) : 0.0; }

    // Lower bound of the bucket containing the p-th percentile (p in [0,1]).
    uint64_t percentile(double p) const {
        if (count_ == 0) return 0;
        p = std::min(1.0, std::max(0.0, p));
        const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_));
        uint64_t cum = 0;
        for (int i = 0; i < BUCKETS; ++i) {
            cum += counts_[i];
            if (cum >= target && cum > 0) return lower_bound_of(i);
        }
        return max_;
    }

    void merge(const Histogram& o) {
        for (int i = 0; i < BUCKETS; ++i) counts_[i] += o.counts_[i];
        count_ += o.count_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    static int index_of(uint64_t v) {
        if (v < static_cast<uint64_t>(SUB)) return static_cast<int>(v);
        const int e = 63 - __builtin_clzll(v);
        const int sub = static_cast<int>((v >> (e - SUB_BITS)) & (SUB - 1));
        return (e - SUB_BITS + 1) * SUB + sub;
    }

    static uint64_t lower_bound_of(int idx) {
        if (idx < SUB) return static_cast<uint64_t>(idx);
        const int e = idx / SUB + SUB_BITS - 1;
        const int sub = idx % SUB;
        return (uint64_t(1) << e) | (static_cast<uint64_t>(sub) << (e - SUB_BITS));
    }

private:
    uint64_t counts_[BUCKETS];
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t max_ = 0;
    uint64_t min_ = UINT64_MAX;
};
