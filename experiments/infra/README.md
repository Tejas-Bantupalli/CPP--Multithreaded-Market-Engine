# Single-participant infrastructure experiment

One compiled C++ participant runs alongside fixed background trader threads. One
interactive LLM supplies a source revision between generations. There is no LLM
in the trading hot path and this runner makes no paid model API calls.

The LLM's role is a C++ engineer specializing in low-latency trading systems.
Its objective is measured latency improvement with identical signal semantics,
not selecting a fashionable implementation or maximizing a single P&L sample.

## Fixed and editable components

The engine is unchanged. All order and private-event channels use SPSC queues;
public market events use the existing multicast ring. Matching, fees, background
rules, seed lists and host source are frozen by hashes at initialization.

`host.cpp` owns a momentum rule: compare the current trade price with the exact
average of the latest 128 trades, use a strict two-tick threshold, submit one-unit
IOC orders at the observed opposite best price, and limit inventory to +/-20.
Only one order can be outstanding. A filled acknowledgement keeps its reservation
until the fill arrives. Every trade updates the signal, including while an order
is outstanding. Every consumed event goes through the normal context bookkeeping.
Batch size never suppresses a trading decision.

The candidate implements `api.h` in a separately compiled source file. For this
first study it controls rolling-history storage/calculation, batch size and idle
waiting. It receives trade prices, not AgentContext, the market seed or rival
state. This narrow first version does not offer a full local-depth-book API or
arbitrary order management. Broader infrastructure can be studied separately.

Candidate code is reviewed against a one-thread, 64-KiB storage budget. These
resource limits and the absence of I/O are source-review requirements, not an OS
sandbox. Native code in the same process is not a security boundary. The pilot's
LLM also built this harness and had previously inspected the repository, so it is
not a blinded study of an independently isolated model.

## Running iterations

From the repository root:

```sh
make -j4 test
python3 experiments/infra/run.py init --out results/my-infra-study --seconds 3 --seeds 3
python3 experiments/infra/run.py evaluate --out results/my-infra-study \
  --candidate experiments/infra/candidates/baseline.cpp \
  --rationale 'Unchanged control to measure session variation'
```

The LLM reads `brief.md`, `feedback.json`, its own previous source and `api.h`,
then writes the next candidate. Call `evaluate` again with that source and its
rationale. The runner snapshots it under `gen_N/`, compiles it, performs reference
and host tests with AddressSanitizer/UndefinedBehaviorSanitizer, and evaluates it.
Compiler errors and test failures stop evaluation; they are not silently replaced
with a fallback. Failed generation directories remain as evidence.

After choosing a generation using training results only:

```sh
python3 experiments/infra/run.py finalize --out results/my-infra-study \
  --generation 1 --rationale 'Chosen using training results only'
```

Selection is recorded before held-out sessions start, then no more training is
allowed in that directory. Use a fresh output directory for a new experiment.

## Measurement and evidence

Two fixed backgrounds: ordinary has five background threads; busy has eight,
with faster maker/noise cadences. The subject adds one thread, plus the existing
engine and logger machinery. Backgrounds are synthetic scenarios, not calibrated
models of real markets.

Each generation runs baseline and candidate separately, serially, on three common
training seeds per regime, alternating execution order. Seeds do not make thread
scheduling or live feedback deterministic. Held-out seeds start at 10001. Three
short sessions per cell are a pilot, not a robust estimate of an extreme tail.

`config.json` records the platform, compiler, engine/source hashes, seed lists and
flags. Each generation retains candidate source, rationale, build commands,
binary hashes, sanitizer logs and one complete report per session in `raw.jsonl`.
Feedback records median session p50/p99 publish-to-submit latency, p99 delivery,
reaction and order-queue delay, order sample counts, fills, P&L and drops. Complete
histogram summaries remain in raw reports; individual event timestamps are not
recorded. Process CPU seconds include all simulated traders and the engine, not
just the candidate thread. Timed intervals use the engine's existing clock and
histograms, and include histogram bookkeeping.

Publish-to-submit stops when an order enters its SPSC queue. Order-queue delay is
reported separately; adding independent percentiles does not give an end-to-end
percentile. Reaction latency is conditional on an order being submitted, so a
change in which market opportunities occur can change its distribution.

Any lost events, full order queues, transport mismatch, final subject inventory
outside the cap, or fewer than 20 order samples marks a session invalid. Raw data
and invalid reasons remain visible; do not claim improvements from invalid runs.
Summaries include all sessions and their valid counts, rather than silently
discarding bad ones. A multicast gap stops the subject; this pilot rejects that
run rather than introducing an untested history reconstruction policy.

Use engine-side final positions/cash/P&L: the existing engine stops agent threads
before draining remaining commands, so agent-local final bookkeeping can lag.
This experiment does not change that lifecycle. A faster computation, lower tail
latency and better P&L are separate findings; none implies the others.
