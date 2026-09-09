#!/usr/bin/env python3
"""Measure what the agent<->engine plumbing costs, holding everything else fixed.

    scripts/bench_transport.py --seeds 15 --seconds 2 --out results/transport.json

Two modes:

  isolated   one transport for every agent, one run per transport. Clean latency
             numbers, no cross-talk, but the agents are not competing.
  head2head  identical strategies in ONE market, each wired to a different
             transport. Any difference in fills or PnL is caused by the plumbing
             alone, because the trading logic is byte-identical.

Reports tick-to-trade (engine publish -> agent submit) which is the number a real
desk quotes, plus its two halves: delivery (publish -> pop) and react (pop -> submit).
"""
import argparse
import json
import statistics
import subprocess
import sys
import tempfile
import os
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sweep  # noqa: E402

TRANSPORTS = ["spsc", "mutex", "mutex_cv"]
# The strategy every measured agent runs. Identical across transports on purpose:
# it is the control that makes the comparison causal.
PROBE = "mm:spread=2,size=5,interval_us=50"
BACKGROUND = ["noise", "noise", "noise"]


def run(binary, seed, seconds, specs, extra=()):
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as t:
        out = t.name
    cmd = [str(binary), "--seconds", str(seconds), "--seed", str(seed), "--json", out, "--quiet"]
    for sp in specs:
        cmd += ["--agent", sp]
    cmd += list(extra)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 90)
        if p.returncode != 0:
            raise RuntimeError(f"seed {seed}: exit {p.returncode}\n{p.stderr[:400]}")
        with open(out) as f:
            return json.load(f)
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def agent_stats(reports, idx):
    """Median-of-seeds latency percentiles plus mean economics for one agent slot."""
    def med(path):
        vals = []
        for r in reports:
            a = r["agents"][idx]
            v = a
            for k in path:
                v = v[k]
            vals.append(v)
        return statistics.median(vals)

    def mean(key):
        return statistics.fmean(r["agents"][idx][key] for r in reports)

    return {
        "transport": reports[0]["agents"][idx]["transport"],
        "tick_to_trade_p50": med(["latency", "event_age", "p50"]),
        "tick_to_trade_p99": med(["latency", "event_age", "p99"]),
        "tick_to_trade_p999": med(["latency", "event_age", "p999"]),
        "tick_to_trade_max": med(["latency", "event_age", "max"]),
        "delivery_p50": med(["latency", "delivery", "p50"]),
        "delivery_p99": med(["latency", "delivery", "p99"]),
        "delivery_p999": med(["latency", "delivery", "p999"]),
        "react_p50": med(["latency", "react", "p50"]),
        "react_p99": med(["latency", "react", "p99"]),
        "fills": mean("fills"),
        "maker_fills": mean("maker_fills"),
        "pnl_mean": mean("pnl"),
        "pnl_std": statistics.pstdev([r["agents"][idx]["pnl"] for r in reports]) if len(reports) > 1 else 0.0,
        "events": mean("events"),
        "events_dropped": sum(r["agents"][idx]["events_dropped"] for r in reports),
        "orders": mean("orders_processed"),
    }


def fmt_ns(v):
    return f"{v/1000:.2f}us" if v >= 10000 else f"{v:.0f}ns"


def table(title, rows, keys):
    print(f"\n{title}")
    head = f"{'transport':<10}" + "".join(f"{k:>13}" for k, _ in keys)
    print(head)
    print("-" * len(head))
    for r in rows:
        line = f"{r['transport']:<10}"
        for k, f in keys:
            line += f"{f(r[k]):>13}"
        print(line)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    ap.add_argument("--seeds", type=int, default=15)
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--idle", default="yield", help="idle policy for polling transports")
    ap.add_argument("--pressure", default="", help="comma list of agent counts to sweep, e.g. 3,5,7,9,12,16")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f"binary not found: {binary} (run make)")
    seeds = list(range(1, args.seeds + 1))
    result = {"config": vars(args), "isolated": [], "head2head": []}

    # ---- isolated: every agent on one transport, one run per transport
    print(f"isolated: {PROBE} against {len(BACKGROUND)} noise traders, "
          f"{args.seeds} seeds x {args.seconds}s, all agents on the same transport")
    for t in TRANSPORTS:
        specs = [f"{PROBE},transport={t}"] + [f"{b}:transport={t}" for b in BACKGROUND]
        reps = [run(binary, s, args.seconds, specs, ["--idle", args.idle]) for s in seeds]
        result["isolated"].append(agent_stats(reps, 0))
        print(f"  {t} done")

    # ---- head to head: identical strategies, different plumbing, same market
    print(f"\nhead2head: {len(TRANSPORTS)} identical market makers in one market, "
          f"one per transport, competing for the same queue positions")
    specs = [f"{PROBE},transport={t}" for t in TRANSPORTS] + BACKGROUND
    reps = [run(binary, s, args.seconds, specs, ["--idle", args.idle]) for s in seeds]
    for i in range(len(TRANSPORTS)):
        result["head2head"].append(agent_stats(reps, i))

    table("ISOLATED  tick-to-trade = engine publishes an event -> agent's order is submitted",
          result["isolated"],
          [("tick_to_trade_p50", fmt_ns), ("tick_to_trade_p99", fmt_ns),
           ("tick_to_trade_p999", fmt_ns), ("tick_to_trade_max", fmt_ns)])
    table("ISOLATED  the two halves: delivery (publish->pop) and react (pop->submit)",
          result["isolated"],
          [("delivery_p50", fmt_ns), ("delivery_p99", fmt_ns), ("delivery_p999", fmt_ns),
           ("react_p50", fmt_ns), ("react_p99", fmt_ns)])
    table("HEAD TO HEAD  identical strategy, same market, competing for the same fills",
          result["head2head"],
          [("tick_to_trade_p50", fmt_ns), ("tick_to_trade_p99", fmt_ns),
           ("maker_fills", lambda v: f"{v:.0f}"), ("pnl_mean", lambda v: f"{v:+.2f}"),
           ("pnl_std", lambda v: f"{v:.2f}")])

    base = result["head2head"][0]
    print(f"\nrelative to spsc in the same market:")
    for r in result["head2head"][1:]:
        dp = (r["maker_fills"] / base["maker_fills"] - 1) * 100 if base["maker_fills"] else 0
        print(f"  {r['transport']:<9} tick-to-trade p99 {r['tick_to_trade_p99']/base['tick_to_trade_p99']:.2f}x"
              f"   passive fills {dp:+.1f}%   pnl {r['pnl_mean'] - base['pnl_mean']:+.2f}")

    # ---- pressure sweep: does the lock-free advantage survive core oversubscription?
    if args.pressure:
        counts = [int(x) for x in args.pressure.split(",") if x]
        result["pressure"] = []
        ncpu = os.cpu_count() or 0
        print(f"\npressure sweep on {ncpu} logical cores: N identical agents all on one transport.\n"
              f"Threads in flight = N agents + 1 engine + 1 main.")
        print(f"{'agents':>7} {'threads':>8} " + "".join(f"{t + ' p50':>13}{t + ' p99':>13}" for t in TRANSPORTS))
        for n in counts:
            row = {"agents": n, "threads": n + 2}
            cells = ""
            for t in TRANSPORTS:
                specs = [f"{PROBE},transport={t}"] + [f"{BACKGROUND[i % len(BACKGROUND)]}:transport={t}"
                                                      for i in range(n - 1)]
                reps = [run(binary, sd, args.seconds, specs, ["--idle", args.idle]) for sd in seeds]
                st = agent_stats(reps, 0)
                row[t] = {"p50": st["tick_to_trade_p50"], "p99": st["tick_to_trade_p99"],
                          "delivery_p50": st["delivery_p50"], "react_p50": st["react_p50"]}
                cells += f"{fmt_ns(st['tick_to_trade_p50']):>13}{fmt_ns(st['tick_to_trade_p99']):>13}"
            result["pressure"].append(row)
            print(f"{n:>7} {n + 2:>8} " + cells)

    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(json.dumps(result, indent=1))
        print(f"\nwrote {args.out}")


if __name__ == "__main__":
    main()
