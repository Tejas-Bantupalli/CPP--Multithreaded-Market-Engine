# Concurrency and architecture measurements

This suite tests systems choices before returning to AI strategy experiments.
There is no AI participant. Production engine sources and defaults are unchanged.

## Comparisons

| Family | Variants | Workload |
|---|---|---|
| Queue synchronization | SPSC, mutex, mutex+notification polled, mutex+CV blocking | 1, 4, 12 independent producer/consumer pairs |
| Waiting | spin, yield, 50-us sleep | SPSC at the same pair counts |
| Cache layout | production padded indices vs same ring algorithm with adjacent indices | same SPSC workload |
| Fanout | N private SPSC pushes vs one multicast publish | 1 producer, 1/4/12 consumers |
| Order collection | per-queue budgets 1/16/256, sorting enabled/disabled | 4/12 producers, one consumer; bursts of 32 |
| Actual engine | SPSC/mutex/CV, SPSC/multicast public feed, yield/spin | 4/12 fixed built-in traders |

Sorting is a semantics tradeoff, not a free optimization: unsorted processing
follows rotating queue collection order; sorting honors submission timestamps
within each collected batch. Neither variant guarantees global timestamp ordering
across batches. Per-producer latency summaries are retained to inspect skew.

The unpadded control uses the same acquire/release ring operations and QCAP-1
usable capacity as the production SPSC queue, but places the indices together.
Its changed layout also changes where payload storage begins. It is a layout
comparison, not a hardware performance-counter diagnosis of cache misses.

## Reproduce

From the repository root:

```sh
make -j4 all
c++ -std=c++17 -O2 -pthread -Wall -Wextra -Iinclude \
  experiments/systems/bench.cpp -o build/systems_bench
python3 experiments/systems/test.py
python3 experiments/systems/run.py --out results/systems-study \
  --repeats 3 --count 10000 --rate 10000 --engine-seconds 2
```

Output directories must be new. `--only micro` or `--only engine` runs a subset.
The defaults cover 49 configurations, repeated three times, plus six sentinel
sessions: **153 measured processes**, all serial. Each round has a reproducibly
shuffled configuration order and starts/ends with the same SPSC-yield sentinel.
There is a 100-ms gap between processes; this is not a thermal-reset guarantee.

For sanitizer checks:

```sh
c++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude experiments/systems/bench.cpp \
  -o build/systems_bench_sanitized
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  python3 experiments/systems/test.py build/systems_bench_sanitized
```

## Fixed-message harness

The harness directly reuses the project's channels and multicast ring. Each
message contains a scheduled time, attempt time, sequence, payload checksum and
producer ID (the executable reports the actual struct size). Each producer offers
10,000 messages at 10,000/second by default; order tests offer groups of 32 at the
same average rate. Producers share an epoch. Thus increasing producer count also
increases aggregate offered load; it does not isolate thread count alone.

Threads and queues are initialized before the timed epoch. Producers sleep until
20 us before their next scheduled release, then poll the clock. Missed deadlines
are retained rather than shifting the schedule. Scheduling delay can produce
catch-up bursts. There is no separate per-cell warmup; all messages are measured.

A consumer validates payload/order and performs the same tiny checksum handler.
It is not a matching engine or network stack. Sampling arrays are reserved before
measurement. Stable sorting may allocate its standard-library temporary buffer;
that cost is intentionally included in the sorted order-collection variant.

All variants use the same virtual channel boundary where applicable. The multicast
variant uses its native interface. Queues hold QCAP-1 messages, while multicast
has QCAP physical slots and can overwrite slow readers. A producer never retries a
full queue: failed attempts and multicast losses are counted. Performance results
with loss are marked invalid, not silently treated as faster.

On macOS worker threads request user-initiated QoS, consistently across variants.
This is a scheduling request, not pinned core placement. Elsewhere the ordinary
scheduler is used. Consumer and producer **thread CPU time** are measured separately.

## Latency boundaries

Exact nearest-rank percentiles are computed after joining workers:

- `scheduled_to_complete_ns`: intended release to end of handler; includes late producers.
- `attempt_to_complete_ns`: timestamp immediately before a push/publish attempt to handler completion.
- `attempt_to_pop_ns`: the same attempt timestamp to successful consumer pop.
- `batch_wait_ns`: pop to start of processing; captures collection/sorting waits.
- `producer_lateness_ns`: intended release to the producer's attempt timestamp.

Attempt time is not a confirmed-enqueue timestamp: it includes a push-side lock
wait or preemption. Fanout uses one timestamp before publishing to all readers,
and SPSC recipients are visited in fixed index order. All latencies include timer
and instrumentation overhead. Published p99.9 values need many more independent
long runs before making strong tail claims.

Micro summaries pool messages across consumers within each session; raw reports
also retain each consumer and producer's distributions. Cross-session summaries
give median/min/max of session percentiles, not a percentile over all sessions.
The throughput number is delivered messages divided by elapsed time including
draining; these paced runs are not maximum-capacity throughput tests.

## Actual-engine checks

One fixed market maker and N-1 fixed noise traders run with identical parameters
and a common seed per round. Sessions still differ because concurrent scheduling
and market feedback are nondeterministic. The runner requests initiated QoS and
records the actual configured transport/fanout returned by the engine.

The engine's histogram summaries have approximately 3% bucket resolution. Its
`submit_to_pop` ends at dequeue, excluding the subsequent collection/sort wait;
`match` measures a command's processing, not the entire order journey. These two
percentiles cannot be added to construct an end-to-end percentile. The synthetic
order test explicitly measures the missing batch-wait interval. Engine throughput
is workload-dependent, not its maximum matching capacity.

## Records and correctness

`config.json` records source/binary hashes, compiler, platform, configuration and
randomization. `raw.jsonl` is flushed after every session and contains commands,
UTC timestamps, system load before/after, validity reasons, counts, CPU and latency
summaries. `summary.json` includes invalid-session counts and does not silently
discard invalid observations.

Tests cover every micro configuration, exact received totals/checksums, monotonic
per-producer sequences, ring wraparound, saturation loss accounting, and rejection
of corrupted/accounting-invalid reports. Nonzero process exits are recorded as
failures and stop the suite. Native code and the runner have process timeouts.

Three rounds on a shared laptop are exploratory evidence. Sentinel drift, system
load and min/max spans must accompany any comparison. Do not attribute a small
difference to architecture when it is similar to run-to-run noise. CPU use,
delivery reliability and ordering semantics belong beside latency in any choice.
