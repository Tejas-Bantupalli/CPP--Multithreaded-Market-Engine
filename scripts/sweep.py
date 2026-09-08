#!/usr/bin/env python3
"""Run one market configuration over many seeds and report PnL distributions.

Every session is a sample from a nondeterministic system, so a single run says
nothing. This runs N seeds, collects the JSON reports, and prints mean, standard
deviation, and hit rate per agent, plus engine throughput and latency medians.

    scripts/sweep.py --seeds 20 --seconds 2 --agent mm --agent noise --agent noise \
        --agent momentum --agent meanrev --out results/sweep.json

Use --parallel > 1 only when you care about PnL and not about latency numbers.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = ROOT / "build" / "market_engine"


def run_one(binary, seed, seconds, agents, plugins, market, extra):
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tmp:
        out = tmp.name
    cmd = [str(binary), "--seconds", str(seconds), "--seed", str(seed), "--json", out, "--quiet"]
    for a in agents:
        cmd += ["--agent", a]
    for p in plugins:
        cmd += ["--plugin", p]
    if market:
        cmd += ["--market", market]
    cmd += extra
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 60)
        if proc.returncode != 0:
            raise RuntimeError(f"seed {seed}: exit {proc.returncode}\n{proc.stderr}")
        with open(out) as f:
            report = json.load(f)
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass
    report["seed"] = seed
    return report


def summarise(reports):
    """Aggregate a list of run reports into per-agent and engine statistics."""
    n = len(reports)
    agents = {}
    for r in reports:
        for a in r["agents"]:
            key = (a["id"], a["name"])
            agents.setdefault(key, {"pnl": [], "fills": [], "volume": [], "events_dropped": [], "queue_full": []})
            agents[key]["pnl"].append(a["pnl"])
            agents[key]["fills"].append(a["fills"])
            agents[key]["volume"].append(a["volume"])
            agents[key]["events_dropped"].append(a["events_dropped"])
            agents[key]["queue_full"].append(a["queue_full"])

    def stats(xs):
        return {
            "mean": statistics.fmean(xs),
            "std": statistics.pstdev(xs) if len(xs) > 1 else 0.0,
            "min": min(xs),
            "max": max(xs),
            "median": statistics.median(xs),
        }

    agent_rows = []
    for (aid, name), d in sorted(agents.items()):
        pnl = stats(d["pnl"])
        pnl["hit_rate"] = sum(1 for x in d["pnl"] if x > 0) / n
        pnl["sharpe"] = (pnl["mean"] / pnl["std"]) if pnl["std"] > 0 else 0.0
        agent_rows.append({
            "id": aid, "name": name, "pnl": pnl,
            "fills_mean": statistics.fmean(d["fills"]),
            "volume_mean": statistics.fmean(d["volume"]),
            "events_dropped_total": sum(d["events_dropped"]),
            "queue_full_total": sum(d["queue_full"]),
        })

    engine = {
        "runs": n,
        "commands_per_sec": stats([r["commands_per_sec"] for r in reports]),
        "trades_per_sec": stats([r["trades_per_sec"] for r in reports]),
        "trades": stats([r["trades"] for r in reports]),
        "last_px": stats([r["last_px"] for r in reports]),
        "match_p50_ns": statistics.median(r["latency"]["match"]["p50"] for r in reports),
        "match_p99_ns": statistics.median(r["latency"]["match"]["p99"] for r in reports),
        "submit_to_pop_p50_ns": statistics.median(r["latency"]["submit_to_pop"]["p50"] for r in reports),
        "submit_to_pop_p99_ns": statistics.median(r["latency"]["submit_to_pop"]["p99"] for r in reports),
        "events_dropped_total": sum(r["events_dropped"] for r in reports),
    }
    return {"engine": engine, "agents": agent_rows}


def print_summary(summary, agents_spec):
    e = summary["engine"]
    print(f"\n{e['runs']} runs | commands/s {e['commands_per_sec']['mean']:.0f} | trades/run {e['trades']['mean']:.0f}"
          f" | match p50/p99 {e['match_p50_ns']:.0f}/{e['match_p99_ns']:.0f} ns"
          f" | submit_to_pop p50/p99 {e['submit_to_pop_p50_ns']:.0f}/{e['submit_to_pop_p99_ns']:.0f} ns"
          f" | events dropped {e['events_dropped_total']}")
    print(f"{'id':>3} {'agent':<12} {'mean pnl':>10} {'std':>9} {'median':>9} {'min':>9} {'max':>9} {'hit':>5} {'sharpe':>7} {'fills':>7}")
    for a in summary["agents"]:
        p = a["pnl"]
        print(f"{a['id']:>3} {a['name']:<12} {p['mean']:>10.2f} {p['std']:>9.2f} {p['median']:>9.2f} {p['min']:>9.2f}"
              f" {p['max']:>9.2f} {p['hit_rate']:>5.2f} {p['sharpe']:>7.2f} {a['fills_mean']:>7.0f}")
    if agents_spec:
        print("agents: " + "  ".join(f"{i}={s}" for i, s in enumerate(agents_spec)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", default=str(DEFAULT_BINARY))
    ap.add_argument("--seeds", type=int, default=10, help="number of seeds (runs)")
    ap.add_argument("--seed0", type=int, default=1, help="first seed")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--agent", action="append", default=[], help="agent spec, repeatable")
    ap.add_argument("--plugin", action="append", default=[], help="plugin spec, repeatable")
    ap.add_argument("--market", default="", help="--market k=v,... passed through")
    ap.add_argument("--parallel", type=int, default=1)
    ap.add_argument("--out", default="", help="write the aggregated summary (and raw reports) as JSON")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("extra", nargs="*", help="extra args after -- are passed to the engine")
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f"binary not found: {binary} (run make)")

    seeds = list(range(args.seed0, args.seed0 + args.seeds))
    with ThreadPoolExecutor(max_workers=max(1, args.parallel)) as pool:
        reports = list(pool.map(
            lambda s: run_one(binary, s, args.seconds, args.agent, args.plugin, args.market, args.extra), seeds))

    summary = summarise(reports)
    summary["config"] = {
        "seconds": args.seconds, "seeds": seeds, "agents": args.agent, "plugins": args.plugin,
        "market": args.market, "extra": args.extra,
    }
    if not args.quiet:
        print_summary(summary, args.agent + args.plugin)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        with open(args.out, "w") as f:
            json.dump({"summary": summary, "runs": reports}, f, indent=1)
        if not args.quiet:
            print(f"wrote {args.out}")
    return summary


if __name__ == "__main__":
    main()
