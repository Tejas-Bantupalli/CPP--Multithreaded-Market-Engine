# Multithreaded Market Engine (C++17)

A multi-agent market simulator. Strategy threads trade against each other
through a single-threaded matching engine over lock-free queues. The engine
reports PnL per agent plus latency percentiles for every hop between an agent
and the book.

See `docs/DESIGN.md` for the architecture.

## What it does

- Price-time priority limit order book over integer ticks: flat price-level
  array, intrusive FIFO per level, order pool with free list, cancel by id,
  GTC and IOC, self-trade prevention. `src/order_book.cpp`
- One engine thread owns the book and portfolios. Per-agent SPSC queues in
  both directions. No locks on the hot path.
- Agents implement a three-method `Strategy` interface and run in their own
  thread. They see their own acks, fills, cancels and rejects, plus broadcast
  trades and top-of-book updates.
- Included agents: market maker, noise trader, momentum, mean reversion.
  Any number of each, parameterised from the command line.
- Two broadcast paths, selectable at runtime: per-agent SPSC pushes, or a
  single-writer multi-reader multicast ring with seqlock slots and lap
  detection. At 24 agents the ring cuts the engine's p99 per-command cost from
  4.5 µs to 1.7 µs (`scripts/bench_fanout.py`, table in `docs/DESIGN.md`).
- Log-bucketed latency histograms, single writer, reported as p50/p90/p99/p99.9.
- Trade and command logs written by a separate logger thread. The command log
  replays through the pure book (`build/replay`) and reproduces the session's
  trade count and book checksum.
- JSON report for scripting many sessions.
- Strategies as compiled plugins (`make plugin SRC=...`, `--plugin path.so`),
  which is how model-written strategies get in.
- `scripts/sweep.py` runs a configuration over many seeds and reports PnL
  distributions. `agents/loop.py` runs the generational experiment where
  agents (Claude, or a mock control) propose parameters or write C++
  strategies, get evaluated, see the results, and try again. See
  `docs/EXPERIMENTS.md`.

## Build and run

Requires a C++17 compiler with pthreads, and Python 3 for the test runner.

```
make            # build/market_engine and build/replay
make test       # 14 tests: book, histogram, queues, multicast ring, engine invariants, plugin load
./build/market_engine --seconds 5
```

Options:

```
--seconds S          session length (default 5)
--seed N             base seed for agent RNGs
--agent SPEC         add an agent; repeatable. SPEC = name[:k=v,k=v]
--px P               initial price in dollars (default 100.00)
--idle MODE          agent idle policy: spin | yield | sleep (default yield)
--engine-idle MODE   engine idle policy
--pin                pin threads to cores (Linux only)
--fanout MODE        broadcast path: spsc | multicast (default spsc)
--market k=v,...     defaults applied to every agent, e.g. the shared fundamental's jump=0.01
--plugin PATH[:k=v]  add an agent from a compiled plugin
--json PATH          write the report as JSON
--trades PATH        trade log CSV
--cmdlog PATH        command log CSV, replayable with build/replay
--quiet              one summary line
```

Example with a custom agent set:

```
./build/market_engine --seconds 3 --seed 11 \
  --agent mm:spread=3,size=10 --agent noise --agent noise \
  --agent momentum:fast=3,slow=15 --agent meanrev:thr=3 \
  --json results/run.json --cmdlog results/cmds.csv
./build/replay results/cmds.csv
```

Run `./build/market_engine --help` for every strategy parameter.

## Sample output

Two-second session, default agents, on an 8-core laptop with the default
`yield` idle policy:

```
=== SESSION ===
  seconds: 2.001 | seed: 3 | commands: 62236 (31106/s) | trades: 12838 (6416/s) | volume: 21313
  price: 100.00 -> 99.48 | book: 20 @ 99.44 / 2 @ 99.48 | open orders: 53 | checksum: b21cdc4fa6602d62

=== PNL (mark @ 99.46) ===
  id  strategy             pnl     pos          cash   fills  volume    orders     rej   q_full  ev_drop
  0   mm                190.80      -6     100787.56    7296   13272     15816      20        0        0
  1   noise             -26.02       3      99675.60    5003    7909     14480       2        0        0
  ...

=== ENGINE LATENCY ===
  latency (ns)         count       mean       p50       p90       p99     p99.9         max
  submit_to_pop        62236       5655      3776     11520     30208     79872      559375
  match                62236        771       496      1600      3264     16128       27666
```

`match` is the time the engine spends on one command: book update, portfolio
update, and pushing every resulting event. `submit_to_pop` is queue wait plus
scheduling. The throughput above is bounded by the agents' configured
cadences, not by the engine.

Every session is one sample from a nondeterministic system. Compare strategies
over many seeds, not one run.

## Adding a strategy

```cpp
class MyStrategy : public Strategy {
    const char* name() const override { return "mine"; }
    void on_start(AgentContext& ctx) override {}
    void on_event(const Event& ev, AgentContext& ctx) override {
        if (ev.kind == EventKind::Trade && ctx.has_ask() && ctx.position() < 10)
            ctx.submit(Side::Buy, ctx.ask_px(), 1, TimeInForce::IOC);
    }
    void on_idle(AgentContext& ctx) override {}
};
```

Register it in `make_strategy` in `src/strategies.cpp`, or keep it out of tree:
end the file with `MARKET_PLUGIN(MyStrategy)`, build with
`make plugin SRC=plugins/my_strategy.cpp`, and run with `--plugin build/plugins/my_strategy.so`. `AgentContext` gives
you `submit`, `cancel`, `position`, `cash`, the last top of book, `fair()`,
a seeded RNG, and a clock. Everything a strategy knows arrives through events.

## Layout

```
include/   types, order_book, histogram, spsc_queue, multicast_ring, strategy (interface + AgentContext),
           engine, report, logger, plugin (ABI), plugin_loader
src/       order_book, engine, strategies, report, logger, plugin_loader, main
plugins/   example strategy plugin
tools/     replay.cpp
scripts/   sweep.py (multi-seed distributions), bench_fanout.py
agents/    generational loop: catalogue, proposers (Claude and mock), code-writing proposers
tests/     engine_tests.cpp and the runner
docs/      DESIGN.md, EXPERIMENTS.md, architecture.html
```

## Known limits

- Thread pinning only on Linux. On macOS with more threads than cores, `spin`
  is slower than `yield`.
- No engine-side risk limits; strategies police their own inventory.
- The background market is simple. Tune it before drawing conclusions about
  strategy quality.
