# Architecture laboratory

This extends the systems pilot with controlled queue layouts, complete strategy
paths, blocking SPSC readers, and a synthetic pipeline-balance experiment. No AI
trader participates. These programs do not modify the production exchange engine.

## Build and run

From the repository root, create `build/` if necessary:

```sh
c++ -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude experiments/architecture/queue_bench.cpp -o build/queue_lab
c++ -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude experiments/architecture/strategy_bench.cpp -o build/strategy_lab
c++ -O2 -std=c++20 -pthread -Wall -Wextra -Iinclude experiments/architecture/wait_bench.cpp -o build/wait_lab
c++ -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude experiments/architecture/pipeline_balance.cpp -o build/pipeline_balance
c++ -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude experiments/architecture/pipeline_capacity.cpp -o build/pipeline_capacity
python3 experiments/architecture/run_queues.py --out results/architecture-queues
python3 experiments/architecture/run_strategies.py --out results/architecture-strategies
python3 experiments/architecture/run_waits.py --out results/architecture-waits
python3 experiments/architecture/run_balance.py --out results/architecture-balance
python3 experiments/architecture/run_capacity.py --out results/architecture-capacity
```

Run the suites serially. Each runner refuses an existing output directory, uses
five rounds by default, shuffles case order deterministically per round, and
saves commands, UTC times, machine load, hashes and complete session summaries.
There are 70 queue, 160 strategy, 150 waiting, 70 pipeline-balance and 40 queue-depth
sessions: 490 timed runs in total.
The queue runner queries hardware metadata at runtime and uses null for unavailable
fields. The first run recorded the original MacBookAir10,1 values obtained with a
separate sysctl query. Runtime metadata discovery was added after that run; its
original runner is preserved under the result's `measured-source/` directory.
Timed C++ source and binaries were not changed by this metadata improvement.

## Queue layouts and cached indices

The original host reports 128-byte cache lines (`sysctl hw.cachelinesize`). The
production SPSC uses 64-byte alignment, which alone does not guarantee separate
128-byte lines. It remains an unchanged control in this experiment.

`queues.h` implements controlled 8/64/128-byte index separation, with and without
cached remote indices. The storage block and payload begin on 128-byte boundaries,
so a vtable cannot accidentally shift the tested indices onto different lines.
Actual index distance is reported. Cached producer/consumer state is privately
owned; the opposite index is refreshed with an acquire load at full/empty
boundaries. The publication/reclamation stores remain release operations.

All queue-layout cases have 1,023 usable entries, exact FIFO validation, no drops
(a full producer retries), and two spinning threads. Sustained tests transfer
500,000 messages; paced tests transfer 50,000 at 50,000/s. Latency samples are
every 257th message, avoiding alignment with the ring's power-of-two period.
The message start timestamp precedes the initial enqueue attempt and includes
full-queue retry time. Queue elapsed time includes joins; thread CPU is separate.

Normal and ASan/UBSan tests cover capacities 2, 8 and 1,024 for all seven variants,
with an independent deque model and concurrent FIFO checks. Source:
`queue_tests.cpp` (compile/run like queue_bench, or with sanitizer flags).

## Strategy-path factorial

The fixed path converts four big-endian words, applies a price-level update,
updates a 128-trade signal, and creates an order intent when the price differs
from the rolling average by more than two ticks. No order is sent to an exchange;
there are no fills or P&L in this experiment.

Factors:

- Inline processing versus producer/consumer pipeline, with either the original
  SPSC or the controlled 128-byte cached-index queue.
- `std::map` level storage versus fixed arrays with cached best-level indices.
- Rescanning the 128-price window versus an exact incremental integer sum.
- `new`/`delete` per intent versus a stack object. Allocation functions are
  deliberately no-inline to prevent allocation elision; there is no pool test.

All variants are checked against a separate map/deque reference and must produce
the same order count and rolling decision checksum for the same input. The full
factorial has 24 variants. Six paced cases and two narrower-price-range controls
bring the total to 32. The input label `sparse` means the wider 4,096-level price
range, not sparse occupancy; `dense` uses 256 possible levels. The fixed array
reserves roughly 32 KiB regardless of occupancy, while the tree allocates nodes.
The array design assumes a known bounded tick range; it is not a general-purpose
replacement for an unbounded book.

Replay uses 200,000 events. Paced cases use 25,000 events at 50,000/s. Reference
calculation, data generation and object setup happen before timing. Worker CPU
time and elapsed time through the last handler completion are separate metrics;
elapsed time can include late initial wakeup. Sampled per-event latency starts
before decoding and ends after the strategy handler, including any queue wait.
Samples include events that produce no order. Integer sums preserve exact results.

`strategy_tests.py` checks all 48 combinations of topology/book/signal/allocation/
input range plus three paced paths, and accepts an optional sanitizer binary path.

## SPSC with atomic waiting

`wait_bench.cpp` compares spinning, yielding, C++20 atomic wait/notify, 64 extra
empty polls before atomic waiting, and the existing mutex+condition-variable
queue. The data queue for the first four is the same unchanged SPSC. All use the
same 16,383-entry usable capacity. Each pair has a dedicated producer/consumer.

The consumer observes a notification generation before checking the queue. The
producer publishes data before incrementing and notifying that generation. This
prevents the empty-check/park lost-wakeup race. Every successful push notifies in
the atomic variants; notification cost is part of the comparison. The library
may itself spin or use an OS wait mechanism. `wait_calls` counts API calls, not
confirmed kernel sleeps. This is **not** a raw Linux futex experiment.

Cases cover 1/4/12 pairs and single-message versus 32-message bursts at the same
10,000 messages/s per producer. All 5,000 messages per pair are timestamped and
validated. CPU includes active waiting. Full queues retry and are counted; there
is no silent message loss. Higher pair counts also mean higher aggregate traffic.
`wait_tests.py` covers every case plus saturation and wraparound, with an optional
sanitizer binary argument.

## When a pipeline can earn its cost

`pipeline_balance.cpp` is a synthetic two-stage dependency-chain calculation,
not a claim about real feed-parser cost. Both the single-thread and pipeline
versions perform and validate identical work. Stage sizes range from zero to
1,024 dependent hash iterations, including intentionally imbalanced stages.

It measures both throughput and sampled message latency under sustained offered
work. A pipeline can improve throughput while increasing individual latency due
to queue backlog. Do not treat those objectives as interchangeable.

`pipeline_capacity.cpp` holds the stage work fixed and varies queue storage
capacity across 2/16/128/1024 entries (usable capacity is one less). It tests both
balanced and imbalanced stages. A full producer retries rather than dropping work;
the latency timestamp precedes those retries. Thus low-capacity results include
backpressure, and high-capacity results include the allowed backlog. This is a
capacity-versus-throughput/latency experiment, not an estimate of queue-copy cost.

## Affinity, noise and interpretation

All Mac worker threads request user-initiated QoS. None is pinned. `common.h`
provides Linux affinity hooks for queue and strategy executables and verifies the
requested mask; those Linux branches have not been exercised on this Mac. A Mac
pinning request is rejected explicitly. No Linux futex or pinned-Linux result is
claimed. CPU pinning is not CPU isolation.

Samples use `steady_clock`; sub-100-ns measurements are close to this host's timer
granularity and should not be overinterpreted. CPU nanoseconds/event amortize
timing over the full loop and are not per-event latency percentiles. Raw session
values and min/max spans accompany medians. Five short local repetitions support
exploration, not production guarantees or causal claims about cache misses.

The implementations deliberately keep workload semantics fixed within a family.
Queue capacities, workload rates and measurement boundaries differ across families;
do not rank numbers from different families as though they measured the same thing.
