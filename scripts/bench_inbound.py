#!/usr/bin/env python3
"""Private SPSC rings per agent vs one shared MPSC queue, for agent -> engine orders.

    scripts/bench_inbound.py --agents 2,4,8,16,32 --seeds 5 --seconds 1 --out results/inbound.json

For each agent count N, run the same market twice: every agent on its own spsc
ring, then every agent pushing into the one shared mpsc queue. Nothing else
changes; engine -> agent events stay SPSC in both, so the difference is the
inbound path alone.

Reports, as medians over seeds:
  submit_to_pop   agent push -> engine pop, all agents merged: the inbound latency
  tick_to_trade   probe agent, engine publish -> its order submitted
  out_of_order    % of commands the engine processed after one submitted later
  reorder_lag     p99 of how much later that one was
  head_stalls     empty mpsc polls blocked by a producer mid-push (mpsc only)
  cmds/s          engine throughput
"""
import argparse
import json
import os
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sweep  # noqa: E402
from bench_transport import run, fmt_ns  # noqa: E402

TRANSPORTS = ["spsc", "mpsc"]
PROBE = "mm:spread=2,size=5,interval_us=50"


def summarize(reports):
    def med(f):
        return statistics.median(f(r) for r in reports)
    return {
        "submit_to_pop_p50": med(lambda r: r["latency"]["submit_to_pop"]["p50"]),
        "submit_to_pop_p99": med(lambda r: r["latency"]["submit_to_pop"]["p99"]),
        "submit_to_pop_p999": med(lambda r: r["latency"]["submit_to_pop"]["p999"]),
        "tick_to_trade_p50": med(lambda r: r["agents"][0]["latency"]["event_age"]["p50"]),
        "tick_to_trade_p99": med(lambda r: r["agents"][0]["latency"]["event_age"]["p99"]),
        "out_of_order_pct": med(lambda r: 100.0 * r["sequencing"]["out_of_order"] / max(r["commands"], 1)),
        "reorder_lag_p99": med(lambda r: r["sequencing"]["reorder_lag"]["p99"]),
        "head_stalls": med(lambda r: r["sequencing"]["mpsc_head_stalls"]),
        "commands_per_sec": med(lambda r: r["commands_per_sec"]),
        "queue_full": med(lambda r: sum(a["queue_full"] for a in r["agents"])),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    ap.add_argument("--agents", default="2,4,8,16,32", help="comma list of agent counts (probe included)")
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--noise-us", type=int, default=50, help="noise traders' order interval; lower = more load")
    ap.add_argument("--idle", default="yield")
    ap.add_argument("--qos", default="inherit")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f"binary not found: {binary} (run make)")
    counts = [int(x) for x in args.agents.split(",") if x]
    seeds = list(range(1, args.seeds + 1))
    bg = f"noise:interval_us={args.noise_us}"
    print(f"{os.cpu_count()} logical cores, {args.seeds} seeds x {args.seconds}s, "
          f"probe {PROBE} + N-1 x {bg}, idle={args.idle} qos={args.qos}")

    rows = []
    for n in counts:
        row = {"agents": n}
        for t in TRANSPORTS:
            specs = [f"{PROBE},transport={t}"] + [f"{bg},transport={t}"] * (n - 1)
            reps = [run(binary, s, args.seconds, specs, ["--idle", args.idle, "--qos", args.qos]) for s in seeds]
            row[t] = summarize(reps)
        rows.append(row)
        print(f"  N={n} done")

    cols = [("submit_to_pop_p50", "s2p p50", fmt_ns), ("submit_to_pop_p99", "s2p p99", fmt_ns),
            ("submit_to_pop_p999", "s2p p999", fmt_ns), ("tick_to_trade_p99", "t2t p99", fmt_ns),
            ("out_of_order_pct", "ooo %", lambda v: f"{v:.3f}"), ("reorder_lag_p99", "lag p99", fmt_ns),
            ("head_stalls", "stalls", lambda v: f"{v:.0f}"),
            ("commands_per_sec", "cmds/s", lambda v: f"{v/1000:.0f}k")]
    head = f"{'N':>4} {'queue':<6}" + "".join(f"{h:>11}" for _, h, _ in cols)
    print("\n" + head + "\n" + "-" * len(head))
    for row in rows:
        for t in TRANSPORTS:
            st = row[t]
            print(f"{row['agents']:>4} {t:<6}" + "".join(f"{f(st[k]):>11}" for k, _, f in cols))
        print()

    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps({"config": vars(args), "rows": rows}, indent=1))
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
