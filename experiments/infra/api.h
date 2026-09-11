#pragma once
#include <cstdint>
#include <cstddef>

// Candidate-only interface. No engine context, market seed, order submission,
// portfolio or rival state. Implement in one separately compiled source file.
constexpr std::size_t INFRA_WINDOW = 128;
class InfraPolicy {
public:
    virtual ~InfraPolicy() = default;
    virtual void push(std::int64_t price) = 0;
    // Exact integer sum of the most recent min(number of pushes, 128) prices.
    virtual std::int64_t sum() const = 0;
    // Host evaluates the fixed trading rule on EVERY event, regardless of batch.
    virtual unsigned batch_size() const = 0; // 1..4096
    virtual void idle() = 0; // bounded waiting; no additional threads or I/O
};
extern "C" InfraPolicy* make_infra();
