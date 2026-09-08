#!/usr/bin/env python3
"""Generational strategy loop: agents propose, the engine evaluates, agents see results, repeat.

    agents/loop.py --generations 6 --seeds 8 --seconds 2 \
        --subject alpha:trend --subject beta:cautious --info rivals --proposer claude \
        --out results/loop-1

Each --subject is name[:persona]. All subjects trade in the same market at the same
time, alongside a fixed background (--background). What each subject is told
about the last generation is controlled by --info:

  own      its own PnL distribution, fills, and volume
  market   plus market-wide statistics: trades, price path, engine throughput
  rivals   plus every other agent's strategy, parameters, and PnL distribution

Output per run directory: gen_<k>.json (proposals + sweep summary), trajectory.jsonl
(one line per subject per generation), and config.json.
"""
import argparse
import json
import statistics
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "scripts"))

from catalogue import to_spec  # noqa: E402
from proposer import ClaudeProposer, MockProposer  # noqa: E402
import sweep  # noqa: E402

DEFAULT_BACKGROUND = ["mm", "noise", "noise", "noise"]


def agent_report(row):
    p = row["pnl"]
    return {
        "mean_pnl": round(p["mean"], 2), "std_pnl": round(p["std"], 2), "median_pnl": round(p["median"], 2),
        "min_pnl": round(p["min"], 2), "max_pnl": round(p["max"], 2), "hit_rate": round(p["hit_rate"], 2),
        "fills_per_session": round(row["fills_mean"], 1), "volume_per_session": round(row["volume_mean"], 1),
    }


def build_report(info, subject_idx, summary, runs, specs):
    """What one subject gets to see. `specs` is the full agent spec list by id."""
    rows = summary["agents"]
    me = rows[subject_idx]
    report = {"you": agent_report(me)}
    if info in ("market", "rivals"):
        e = summary["engine"]
        price_moves = [r["last_px"] - r["initial_px"] for r in runs]
        report["market"] = {
            "sessions": e["runs"],
            "trades_per_session": round(e["trades"]["mean"]),
            "commands_per_sec": round(e["commands_per_sec"]["mean"]),
            "price_change_mean": round(statistics.fmean(price_moves), 2),
            "price_change_std": round(statistics.pstdev(price_moves), 2) if len(price_moves) > 1 else 0.0,
            "final_spread_ticks_median": statistics.median(
                (r["final_ask"] - r["final_bid"]) / 0.01 for r in runs if r["final_bid_qty"] and r["final_ask_qty"]) if any(
                r["final_bid_qty"] and r["final_ask_qty"] for r in runs) else None,
        }
    if info == "rivals":
        report["others"] = [
            {"id": row["id"], "spec": specs[row["id"]], **agent_report(row)}
            for row in rows if row["id"] != subject_idx
        ]
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--generations", type=int, default=5)
    ap.add_argument("--seeds", type=int, default=8, help="seeds per evaluation")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--subject", action="append", default=[], help="name[:persona], repeatable")
    ap.add_argument("--background", default=",".join(DEFAULT_BACKGROUND), help="comma-separated fixed agent specs")
    ap.add_argument("--info", choices=["own", "market", "rivals"], default="own")
    ap.add_argument("--proposer", choices=["claude", "mock"], default="claude")
    ap.add_argument("--model", default="claude-opus-5")
    ap.add_argument("--effort", default="high")
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--out", required=True)
    ap.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    args = ap.parse_args()

    subjects = []
    for i, s in enumerate(args.subject or ["agent"]):
        name, _, persona = s.partition(":")
        subjects.append((name, persona or "neutral"))
    background = [b for b in args.background.split(",") if b]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "config.json").write_text(json.dumps(vars(args), indent=1))

    proposers = {}
    for i, (name, persona) in enumerate(subjects):
        if args.proposer == "mock":
            proposers[name] = MockProposer(seed=i, start_strategy=["momentum", "meanrev", "mm"][i % 3])
        else:
            proposers[name] = ClaudeProposer(model=args.model, persona=persona, effort=args.effort)

    history = {name: [] for name, _ in subjects}
    last_summary, last_runs, last_specs = None, None, None
    n_bg = len(background)

    for gen in range(args.generations):
        t0 = time.time()
        proposals = {}
        for i, (name, persona) in enumerate(subjects):
            idx = n_bg + i
            report = build_report(args.info, idx, last_summary, last_runs, last_specs) if last_summary else None
            proposal, warnings = proposers[name].propose(name, history[name], report)
            proposals[name] = proposal
            for w in warnings:
                print(f"  [{name}] warning: {w}")
            print(f"gen {gen} {name:<10} -> {to_spec(proposal.strategy, proposal.params)}")
            print(f"      {proposal.rationale[:300]}")

        specs = background + [to_spec(proposals[n].strategy, proposals[n].params) for n, _ in subjects]
        seeds = list(range(gen * 1000 + 1, gen * 1000 + 1 + args.seeds))
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=max(1, args.parallel)) as pool:
            runs = list(pool.map(lambda s: sweep.run_one(args.binary, s, args.seconds, specs, [], "", []), seeds))
        summary = sweep.summarise(runs)

        gen_record = {"generation": gen, "specs": specs, "seeds": seeds, "proposals": {}, "summary": summary}
        for i, (name, _) in enumerate(subjects):
            row = summary["agents"][n_bg + i]
            entry = {
                "proposal": dict(proposals[name]),
                "score": row["pnl"]["mean"], "std": row["pnl"]["std"], "hit_rate": row["pnl"]["hit_rate"],
                "fills": row["fills_mean"],
            }
            history[name].append(entry)
            gen_record["proposals"][name] = entry
            with open(out / "trajectory.jsonl", "a") as f:
                f.write(json.dumps({"generation": gen, "subject": name, "info": args.info, **entry}) + "\n")
        (out / f"gen_{gen}.json").write_text(json.dumps({**gen_record, "runs": runs}, indent=1))

        sweep.print_summary(summary, specs)
        print(f"generation {gen} done in {time.time() - t0:.0f}s\n")
        last_summary, last_runs, last_specs = summary, runs, specs

    print("trajectory:")
    for name, _ in subjects:
        print(f"  {name}: " + " -> ".join(f"{h['score']:.1f}" for h in history[name]))


if __name__ == "__main__":
    main()
