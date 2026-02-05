# Multithreaded Market Engine (C++)

A compact, single-process matching engine with multiple trader threads. The goal
is to compare strategy code paths while keeping market
behavior and order cadence the same.

## What this does

- Single-threaded matching engine owning the order book
- Per-trader lock-free SPSC queues for orders and market events
- Simple strategies (MA baseline, MA fast, chaos) running in their own threads
- Latency aggregation for:
  - strategy recv -> submit
  - event -> submit (end-to-end)
  - submit -> engine pop (backlog)

## Build and run

Requirements:
- g++ with C++17 support
- pthreads

Build:
```
make
```

Run:
```
./a.out
```

## Architecture (high level)

- Engine thread:
  - drains per-trader order queues round-robin
  - matches crossing orders using price-time priority
  - publishes market events on trade
- Trader threads:
  - consume market events from per-trader queues
  - submit orders back to the engine

## Strategies included

- Trader 0/1: baseline moving average
  - uses a mutex-protected deque for MA and an atomic price
- Trader 2: fast moving average
  - lock-free O(1) rolling SMA using event prices
  - same cadence as baseline to isolate code-path speed
- Trader 99: chaos
  - random size, side, and aggressiveness

## Key files

- `src/engine.cpp`: engine loop, matching, strategy threads, latency stats
- `src/engine_state.cpp`: shared state and queues
- `src/main.cpp`: startup, strategy launch, results printing
- `include/market_types.h`: core types and latency structures
