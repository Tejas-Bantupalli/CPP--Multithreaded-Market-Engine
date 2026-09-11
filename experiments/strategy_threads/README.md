# Strategy threads: does a strategy get faster with child threads?

One probe strategy runs in the real engine (engine sources unchanged) in several
arrangements, with identical decisions verified in every session.

| arrangement | threads | what moves |
|---|---|---|
| `single` | 1 | reads the ring, computes, submits |
| `forward` | 2 | reader forwards events over an internal SPSC queue; worker computes and submits |
| `split` | 2 | as forward, but the reader also runs stage A of the calculation |
| `--hk inline` / `thread` | 1 / 2 | periodic statistics (copy + sort a window) on the trading thread, or on a helper fed by SPSC |
| `single --burner` | 2 | single, plus a thread spinning on an empty queue: two cores' footprint, no handoff |

Reader→worker queue (`--queue`, forward and split):

| queue | worker waits by | reader signals by |
|---|---|---|
| `spsc` | spinning on the lock-free ring | nothing |
| `mutex_cv` | parking on a condition variable (as `MutexCvChannel` in include/transport.h) | `notify_one` on every push |
| `futex` | `os_sync_wait_on_address` on a flag (macOS's public futex, 14.4+) after re-checking the ring | a wake syscall only when the flag says the worker is parked |

The futex handshake is checked two ways: ThreadSanitizer, and a lost-wakeup counter (a
park that times out with work queued invalidates the session). With the reader's wake
call deliberately removed, that counter fires and the session is rejected.

## Fairness without CPU pinning

macOS on Apple Silicon has no thread affinity, and QoS only states a preference: at
`user-initiated`, threads spill onto efficiency cores once busy threads outnumber the four
performance cores (for this calculation 1.56 ns/iteration on a P core, 2.42 on an E core;
a thread-level `background` QoS does not confine to E cores, `taskpolicy -b` does). So:

- **Core budget.** The taker sleeps ~60% of each gap and spins only the tail (macOS
  oversleeps ~30%), so the only full-time spinners are the engine and the probe threads.
- **Equal-footprint control.** `single --burner` occupies two cores without a handoff, so
  forward/split can be compared against the same core pressure.
- **Speedometer.** Every probe thread times a 1000-iteration run of the same calculation
  every ~10 ms (~1.7 µs = full-speed P core). A session is "P-only" when no probe thread had
  more than 5% slow samples; reports show all sessions and the P-only subset.

Ownership in the threaded versions: the reader is the only consumer of the private
channel and multicast cursor (`ctx.poll`); the worker is the only caller of
`ctx.apply` / `ctx.submit`, so it stays the single producer on the order queue.
Children are joined inside `run()`. ThreadSanitizer: clean (a deliberate race was
detected as a positive control).

## Workload and checks

A maker rests a ladder of one-lot asks; a taker lifts one per step at 4,000/s, either
evenly or in bursts of 32. Each lift is exactly one Trade broadcast, so the input is
identical in every session. (BookUpdates are coalesced per engine batch and would
not be.) The probe answers each Trade with a non-crossing IOC buy: real matching and
acks, but its orders never change its input. Every session must match a
single-threaded reference replay (decision and housekeeping checksums), with no
dropped events, no full queues, and every ack received.

`--work N` is the hash-chain length per trade (~1.6-2 ns per iteration; 16,000 ≈ 30 µs).

## Run

```sh
python3 experiments/strategy_threads/run.py --family handoff --out results/strategy-threads-handoff-DATE
python3 experiments/strategy_threads/analyze.py results/strategy-threads-handoff-DATE
# the earlier families:
python3 experiments/strategy_threads/run.py --family datapath --out ...
python3 experiments/strategy_threads/run.py --family housekeeping --repeats 8 --out ...
```

The datapath and housekeeping results below predate the taker pacing change and the
speedometer; the handoff family is the controlled rerun.

Result directories (raw.jsonl, config.json with source and binary hashes, report.md) are
written under `results/`, which is gitignored; the tables in this README are the committed
record of those runs.

Rounds visit every cell in a fresh shuffled order; `analyze.py` reports medians of
session percentiles with min-max, and within-round paired ratios.

## Handoff results, 2026-09-11 (M1, 4P+4E, macOS 15.2; unpinned)

`results/strategy-threads-handoff-2026-09-11b`: 192/192 sessions valid, zero lost wakeups.
(The first handoff run had a futex lost-wakeup race; see its NOTE.md.) Speedometer: no probe
thread in any session had 10% of samples at E-core speed, so the core budget held; P cores
ran ~15% slower late in the run than when cold.

Event→order latency, median of 6 sessions (µs), and paired ratio vs `single` (rounds faster):

| arrangement | no work, smooth | no work, burst | 16k work, smooth | 16k work, burst | strategy CPU |
|---|---:|---:|---:|---:|---:|
| single | 0.2 | 0.2 | 31.0 | 488 | 1 core |
| single + burner | 0.2 | 0.2 | 31.0 | 498 | 2 cores |
| forward / spsc | 0.4 (2.3×, 0/6) | 0.3 (2.0×) | 31.2 (1.01×) | 508 (1.03×) | 2 cores |
| forward / mutex_cv | 3.0 (17×) | 9.7 (66×) | 33.1 (1.09×) | 497 (1.03×) | ~1 core |
| forward / futex | 2.8 (17×) | 2.8 (17×) | 33.5 (1.08×) | 501 (1.03×) | ~1 core |
| split / spsc | 0.4 | 0.3 | 31.2 | **260 (0.54×, 6/6)** | 2 cores |
| split / mutex_cv | 3.0 | 8.4 | 44.2 (1.43×) | **272 (0.56×, 6/6)** | ~1 core |
| split / futex | 2.8 | 2.6 | 40.6 (1.31×) | **278 (0.57×, 6/6)** | ~1 core |

- **A second thread never made the reader→decision path faster** unless it took half the
  work and events were queueing (split under bursts, ~0.55× with every queue, 6/6 rounds).
- **Spinning SPSC costs ~0.2 µs per handoff and a whole core.** Blocking costs ~2.5-3 µs per
  wake and almost no CPU (worker 0.01-0.45 CPU-s per session versus 3 s spinning).
- **futex vs mutex+cv** (same round): similar wake cost when the worker parks for every
  event (handoff p50 0.93-0.95×), but futex wins where it can skip the syscall: in bursts
  it made ~1,600 wakes where mutex+cv notified ~25,000 times, handoff p50 0.31-0.38× (6/6).
  Acks reached the worker ~4× sooner (ack p50 0.21-0.27×, 6/6 in every smooth case).
- **Tails:** with no work and smooth arrivals p99 was single 0.4, spsc 3.3, futex 23, mutex+cv
  79 µs. Busy work dilutes the difference (16k smooth: 35 / 41 / 50 clean / 75 µs).
- **The equal-footprint control matters.** single + burner had the same p50 as single but
  higher tails (p99 2.4× with no work, and 3 of 6 sessions stalled at 16k): part of what a
  second spinning thread costs is core pressure, not the handoff.
- 3 of 6 forward/futex 16k-smooth sessions hit machine-wide stalls (the taker was also
  0.5-1.8 ms late), inflating that cell's p99 to 283 µs; the clean sessions give ~50 µs.

## Earlier results, 2026-09-11 (M1, 4P+4E, macOS; unpinned)

Data path: `results/strategy-threads-2026-09-11` (228/228 valid).
Housekeeping: `results/strategy-threads-2026-09-11-housekeeping` (80/80 valid; the
first run's housekeeping rows sorted near-sorted input and are superseded, see its NOTE.md).

Event→order latency, median across 6 sessions, and paired ratio vs `single` (rounds faster):

| workload | single p50 | forward p50 | split p50 | split / single |
|---|---:|---:|---:|---|
| no work, smooth, spin | 0.2 µs | 0.6 | 0.6 | 2.2× slower, 0/6 |
| 16k work, smooth, spin | 34 µs | 44 | 44 | 1.12× slower, 0/6 |
| 4k work, burst, spin | 138 µs | 154 | 76 | **0.58×, 6/6** (p99 0.55×, 5/6) |
| 16k work, burst, spin | 573 µs | 653 | 333 | **0.58×, 6/6** (p99 0.58×, 6/6) |
| no work, smooth, yield | 0.3 µs | 1.9 | 2.0 | 5.7× slower, 0/6 (p99 ~20×) |

- **Forwarding never paid.** It added ~0.2-0.4 µs of handoff and, under bursts, just moved
  the backlog from the multicast ring into the internal queue. It costs a second core.
- **Splitting the calculation paid only when events queue.** Under bursts, two stages drain
  the backlog about twice as fast (p50 and p99 both ~0.58×, every round). With evenly spaced
  events there is no backlog to drain and it was ~12% slower.
- **Core scarcity is part of the cost.** The same 16k calculation took ~31 µs in some
  sessions and 46-62 µs in others, stable within a session, and more often slow on the
  child worker. QoS `user-initiated` prefers P cores but spills to E cores when they are busy,
  and a threaded session already runs ~4 busy threads. macOS cannot pin or report placement,
  so this is inferred, not proven.
- **Yielding instead of spinning** saves little CPU on a mostly idle Mac (1.74 vs 2.0 cores)
  and costs ~1 µs per handoff and ~20× at p99.
- **Housekeeping** (noisier run): inline statistics passes clearly hurt tails under bursts
  (p99 8-14× the control, worse in 8/8 rounds). Moving them to a helper thread recovered
  most of that (p99 ~0.2× of inline, 5/8 rounds). But with smooth arrivals the helper's
  yield-then-sleep loop burned ~0.4 cores and made tails worse than inline for light work:
  the helper needs to park, not poll.

Not claimed: Linux or pinned-core behaviour, network-scale latencies, or anything about P&L.
