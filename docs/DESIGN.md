# Design

A multi-agent market where strategy threads trade against each other through a
single-threaded matching engine. The engine is the product; there is no
backtester. Reproducibility comes from treating every session as a sample and
from an optional command log that can be replayed through the pure book.

## Threads and data flow

```
 agent thread i  --Command-->  SPSC(oq_i)  --->  engine thread  ---> OrderBook
 agent thread i  <--Event----  SPSC(eq_i)  <---  engine thread
                                                 engine thread  --TradeRecord/Command--> SPSC --> logger thread --> files
```

* One engine thread owns the book, the authoritative portfolios, and the event
  sequence. It never blocks on agents.
* Each agent has one inbound and one outbound SPSC queue. Producer and consumer
  are fixed per queue, so no locks anywhere on the hot path.
* The engine drains queues round-robin in bounded batches and orders a batch by
  submit timestamp, so arrival order is honoured without a shared sequence
  counter.
* The logger thread receives trade records and processed commands from the
  engine through SPSC queues and writes CSV. If the logger falls behind, the
  engine drops records and counts them rather than stalling.

## Order book (`include/order_book.h`)

* Prices are integer ticks (`Price`), one tick = 0.01. No floating point in
  the book.
* Levels live in a flat array over a configurable price band, so price lookup
  is O(1). Orders outside the band are rejected.
* Each level is an intrusive FIFO of order nodes drawn from a pool with a free
  list. Nodes are addressed by index so the pool can grow.
* Order id lookup for cancels goes through a hash map.
* Matching is price-time priority, trades at the maker's price, supports
  partial fills, GTC and IOC.
* Self-trade prevention: when an incoming order would match a resting order
  from the same agent, the resting order is cancelled and matching continues.
* `add` and `cancel` are pure with respect to threads and report fills through
  out-parameters. The engine turns those into events. `tools/replay.cpp` feeds
  a command log through the same class to reproduce a session's book.

## Agents (`include/strategy.h`)

```cpp
class Strategy {
  virtual void on_start(AgentContext&);
  virtual void on_event(const Event&, AgentContext&) = 0;
  virtual void on_idle(AgentContext&);
};
```

`AgentContext` is the strategy's only view of the world. It owns the queues,
assigns order ids, tracks the agent's position and cash from its own fill
events, keeps the last top-of-book, and records agent-side latency histograms.
Strategies are registered with the engine before the session and run in their
own thread. Nothing about the agent set is hardcoded.

## Strategy plugins (`include/plugin.h`)

A strategy can be a shared library: one .cpp with a `Strategy` subclass and
`MARKET_PLUGIN(ClassName)`, built with `make plugin SRC=...` and loaded with
`--plugin path.so[:k=v]`. The host resolves two C symbols (`market_plugin_api_version`,
`market_plugin_create`), checks the API version, constructs the strategy, and
never closes the library because the object's vtable lives there. Everything
a plugin touches (`Strategy`, `AgentContext`, `Params`, `RollingSMA`) is
header-only, so plugins have no host symbols to resolve and the ABI is just
"same compiler, same headers". This is what lets the generational loop
(`agents/loop.py --proposer claude-code`) run model-written strategies.

## Broadcast fan-out (`include/multicast_ring.h`)

Two paths, selected with `--fanout`:

* `spsc` (default): the engine pushes each broadcast event into every agent's
  private queue. Simple, but a publish costs one push and one cache line per
  agent, so the engine's per-command cost grows with the agent count.
* `multicast`: one single-writer, multi-reader ring. The engine writes each
  event once; every agent reads it through its own cursor. Each slot is a
  seqlock: the writer marks the slot in progress, stores the payload as
  relaxed atomic words, then stores the publish number with release. A reader
  loads the sequence with acquire, copies, fences, and re-checks; a mismatch
  means the writer lapped it. A lapped reader jumps to the oldest intact value
  and counts what it lost, so a slow agent never slows the engine or the
  other agents. Private events (fills, acks) still use the agent's SPSC queue.

Measured on an 8-core laptop, 2 s sessions, median of 3 seeds, `match` is the
engine's per-command cost including publishing:

| agents | fanout    | match p50 | match p99 | delivery p50 | delivery p99 |
|-------:|-----------|----------:|----------:|-------------:|-------------:|
|      6 | spsc      |    456 ns |   3776 ns |      2016 ns |     24064 ns |
|      6 | multicast |    496 ns |   3008 ns |      1456 ns |     26112 ns |
|     12 | spsc      |    288 ns |   3584 ns |      7104 ns |     22016 ns |
|     12 | multicast |    328 ns |   2112 ns |      7168 ns |     22528 ns |
|     24 | spsc      |    208 ns |   4480 ns |     17920 ns |     48128 ns |
|     24 | multicast |    208 ns |   1696 ns |     16384 ns |     43008 ns |

The median is unchanged because most commands (cancels, non-crossing adds) do
not broadcast. The tail is where fan-out shows up, and it stops growing with
the agent count once publishing is a single write. Reproduce with
`scripts/bench_fanout.py`.

## Events

Private to one agent: `Ack`, `Fill`, `Cancelled`, `Rejected`.
Broadcast: `SessionStart`, `Trade`, `BookUpdate`.
All events are one POD struct so they fit the SPSC queue.

## Latency measurements

Histograms are log-bucketed (about 3% relative resolution) and single-writer,
so recording is a couple of integer ops. Reported as p50/p90/p99/p99.9/max.

| name            | writer | from                       | to                        |
|-----------------|--------|----------------------------|---------------------------|
| delivery        | agent  | engine publishes event     | agent pops it             |
| react           | agent  | agent pops event           | agent submits an order    |
| event_age       | agent  | engine publishes event     | agent submits an order    |
| submit_to_pop   | engine | agent submits              | engine pops the command   |
| match           | engine | engine pops                | book and events done      |

## Shutdown lifecycle
1. `running = false`. Agents finish their current loop and exit.
2. Join all agent threads. No producer can submit after this.
3. `producers_done = true`. The engine keeps draining until every inbound
   queue is empty in one full pass, then exits.
4. Join the engine and the logger.

## Reproducibility

Runs are nondeterministic by construction. Compare strategies over many seeds
and report distributions. The `--cmdlog` option records every processed
command in engine order; `build/replay` replays it through `OrderBook` and
reports the trade count and a book checksum, which must match the session's.

## Not done yet

* Thread pinning is Linux only (`--pin`).
* No risk limits at the engine; strategies police their own inventory.
* The background market (market maker plus noise traders) is deliberately
  simple. Tune it before drawing conclusions about strategy quality.

## Background market

The noise traders share a latent true value: a random walk with jumps stepped
once per millisecond of session time (`Fundamental` in `src/strategies.cpp`).
Each noise trader builds its own copy from the session's market seed, so they
agree on the path without sharing memory. Each sees it through private
mean-reverting noise and takes liquidity when its view is beyond the touch.
Jumps create trends for momentum; the walk between jumps is what mean
reversion fades; the market maker earns the spread and is picked off on jumps.
Tune with `--market fsigma=..,jump=..,jumpsize=..,drift=..`.

`scripts/sweep.py` runs a configuration over many seeds and reports PnL mean,
spread, hit rate, and engine latency medians. Use it for every comparison.
