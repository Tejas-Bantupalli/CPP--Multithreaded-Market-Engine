# Multithreaded Market Engine

A C++17 market simulator for macOS. Independent strategy threads submit orders
through lock-free single-producer/single-consumer (SPSC) queues. One matching
thread owns the order book and portfolios, so matching needs no shared book lock.

## Build and run

Requires the Xcode Command Line Tools (`xcode-select --install`) and Python 3
for tests. There are no third-party libraries.

```sh
make
make test
./build/market_engine --seconds 5
```

The default session runs six traders: a market maker, three noise traders,
a momentum trader and a mean-reversion trader. The report shows trades,
positions, fees, PnL, queue drops and latency percentiles.

## Architecture

```text
Strategy thread  -- commands --> SPSC queue --┐
Strategy thread  -- commands --> SPSC queue --┼--> Matching thread --> Order book
Strategy thread  -- commands --> SPSC queue --┘         |
       ^                                               |
       └------------- per-agent SPSC events -----------┘
                                                       |
                                            optional SPSC logs
                                                       |
                                                  Logger thread
```

- **Order book:** integer prices, price-time priority, partial fills, GTC/IOC,
  cancellation by order ID and self-trade prevention.
- **Engine:** bounded queue draining, timestamp ordering within each batch,
  portfolio accounting, maker/taker fees and a price collar.
- **Strategies:** built-in traders and compiled C++ plugins share the same
  interface. AI experiments can change parameters, write strategies, choose a
  transport and override their event loop. The engine owns authoritative state.
- **Measurement:** queue wait, matching and strategy reaction histograms.
  An optional logger writes CSV away from the matching thread.

## A short engine demo

```sh
mkdir -p results
./build/market_engine --seconds 3 --seed 11 --collar 0 \
  --agent mm:spread=3,size=10 --agent noise --agent noise \
  --agent momentum:fast=3,slow=15 --agent meanrev:thr=3 \
  --json results/run.json --cmdlog results/commands.csv
./build/replay results/commands.csv
```

Compare the session and replay trade count and book checksum. Replay runs the
recorded commands through the order book. Use `--collar 0` for this demo because
replay does not implement the engine's price collar, and check that
`log dropped trades/cmds` reports zero command drops.

Use `./build/market_engine --help` for strategy parameters and options. Useful
controls include `--seed`, `--seconds`, repeatable `--agent`, `--market`, `--px`,
`--idle`, `--engine-idle`, `--qos`, `--collar`, `--maker-fee`, `--taker-fee`,
`--json`, `--trades`, `--cmdlog`, `--plugin`, `--transport`, `--fanout` and `--quiet`. The default idle policy is `yield`;
macOS QoS expresses a scheduling preference, not a guaranteed core assignment.

## AI strategy experiments

AI agents receive a brief, propose parameters or C++ strategy code, trade over
several seeds, then receive feedback for the next generation. The model runs
between sessions; the C++ engine executes the resulting strategies.

Start with [the recorded results index](docs/RESULTS.md). It links the evaluated
AI experiments, controls, source code and reports. Mock runs and preliminary
loop demos are archived separately. Evaluated failures remain in the evidence.

```sh
# Read existing results; these commands do not run a model or alter the runs.
python3 scripts/trajectory.py results/exp1_info results/exp5_freecode
python3 scripts/exp9_report.py results/exp9_field_a results/exp9_field_b results/exp9_field_c
```

`agents/session.py` provides `init`, `status` and `evaluate` for externally
written proposals. `agents/loop.py` provides automated adaptation and control
runs. See [experiment instructions](docs/EXPERIMENTS.md) for both workflows.

```sh
# A small hand-written plugin demo using the same interface as AI strategies.
make plugin SRC=plugins/example_breakout.cpp
./build/market_engine --seconds 1 --agent mm --agent noise \
  --plugin build/plugins/example_breakout.so
```

The default transport is SPSC. `--transport mutex|mutex_cv` compares locking
and blocking channels; `--fanout multicast` compares shared-ring broadcasts.
These remain available because the recorded experiments use them.

## Code walkthrough

| Start here | Responsibility |
|---|---|
| `src/main.cpp` | Parse options, create strategies, run a session |
| `src/engine.cpp` | Thread lifecycle, command processing and event delivery |
| `src/order_book.cpp` | Matching and cancellation |
| `include/spsc_queue.h` | Bounded lock-free queue and memory ordering |
| `include/strategy.h` | Strategy interface and agent-local state |
| `src/strategies.cpp` | Four built-in strategies and their parameters |
| `src/logger.cpp`, `src/report.cpp` | CSV logs and session reports |
| `tools/replay.cpp` | Replay a command log on one thread |
| `agents/`, `scripts/` | AI experiment orchestration and analysis |
| `experiments/` | Benchmark implementations with recorded results |
| `tests/` | Book, queue, accounting, shutdown and replay checks |

Headers live in `include/`, implementations in `src/`, and generated files in
`build/` and `results/`. See [the design notes](docs/DESIGN.md) for concurrency
contracts and tradeoffs. To add a strategy, subclass `Strategy` and register it
in `make_strategy` in `src/strategies.cpp`, or build it as a plugin with
`MARKET_PLUGIN(MyStrategy)`.

## What the results mean

This is a single-instrument, in-process simulation. There is no network feed or
exchange connection. Live runs remain scheduling-dependent even with a fixed
seed. Latencies include local scheduling effects, and observed throughput is
limited by the configured strategy cadences. Synthetic-market PnL is not
an estimate of live trading performance.
