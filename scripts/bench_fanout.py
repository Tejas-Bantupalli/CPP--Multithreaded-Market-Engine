#!/usr/bin/env python3
"""Compare broadcast fan-out paths: per-agent SPSC pushes vs one multicast ring.

    scripts/bench_fanout.py --agents 6 24 --seeds 3 --seconds 2

Runs sequentially (latency numbers are meaningless under parallel load) and
prints, per (agents, mode): engine match p50/p99, agent delivery p50/p99 (median
over agents and runs), broadcast events dropped, and throughput.
"""
import argparse
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sweep  # noqa: E402


def specs_for(n):
    base = ["mm", "noise", "noise", "noise", "momentum", "meanrev"]
    extra = ["noise", "momentum", "meanrev", "mm:spread=4"]
    out = list(base)
    while len(out) < n:
        out.append(extra[len(out) % len(extra)])
    return out[:n]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agents", type=int, nargs="+", default=[6, 24])
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    args = ap.parse_args()

    print(f"{'agents':>6} {'fanout':<10} {'cmds/s':>8} {'events/s':>9} {'match p50':>10} {'match p99':>10}"
          f" {'deliv p50':>10} {'deliv p99':>10} {'dropped':>8}")
    for n in args.agents:
        specs = specs_for(n)
        for mode in ("spsc", "multicast"):
            runs = [sweep.run_one(args.binary, s, args.seconds, specs, "", ["--fanout", mode])
                    for s in range(1, args.seeds + 1)]
            match_p50 = statistics.median(r["latency"]["match"]["p50"] for r in runs)
            match_p99 = statistics.median(r["latency"]["match"]["p99"] for r in runs)
            d50 = statistics.median(a["latency"]["delivery"]["p50"] for r in runs for a in r["agents"])
            d99 = statistics.median(a["latency"]["delivery"]["p99"] for r in runs for a in r["agents"])
            dropped = sum(r["events_dropped"] for r in runs)
            cps = statistics.fmean(r["commands_per_sec"] for r in runs)
            eps = statistics.fmean(r["events_published"] / r["session_seconds"] for r in runs)
            print(f"{n:>6} {mode:<10} {cps:>8.0f} {eps:>9.0f} {match_p50:>10.0f} {match_p99:>10.0f}"
                  f" {d50:>10.0f} {d99:>10.0f} {dropped:>8}")


if __name__ == "__main__":
    main()
