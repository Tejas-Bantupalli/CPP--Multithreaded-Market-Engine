#!/usr/bin/env python3
"""Generational strategy loop: agents adapt, the engine evaluates, agents see results, repeat.

    agents/loop.py --generations 10 --seeds 8 --seconds 2 \
        --subject solo --subject copycat --info rivals --rule imitate \
        --out results/loop-1

Each --subject is a name (name:persona only matters for the llm rules). All
subjects trade in the same market at the same time, alongside a fixed background
(--background). What each subject is told about the last generation is
controlled by --info:

  own      its own PnL distribution, fills, and volume
  market   plus market-wide statistics: trades, price path, engine throughput
  rivals   plus every other agent's strategy, parameters, and PnL distribution

--rule selects how a subject adapts. hillclimb, imitate, and bandit are
algorithmic and need nothing installed (see agents/adapt.py). llm and llm-code
ask a model and need credentials; they are off unless asked for.

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
from adapt import RULES  # noqa: E402
import sweep  # noqa: E402

DEFAULT_BACKGROUND = ["mm", "noise", "noise", "noise"]


def spec_of(proposal):
    """Engine spec for a proposal: a compiled plugin if it carries one, else a catalogue strategy."""
    if proposal.get("spec"):
        return proposal["spec"]
    return to_spec(proposal.strategy, proposal.params)


def agent_report(row, full=False):
    """`full` adds the agent's own execution and latency telemetry: things a real desk
    measures about itself. Rival rows never get it."""
    p = row["pnl"]
    out = {
        "mean_pnl": round(p["mean"], 2), "std_pnl": round(p["std"], 2), "median_pnl": round(p["median"], 2),
        "min_pnl": round(p["min"], 2), "max_pnl": round(p["max"], 2), "hit_rate": round(p["hit_rate"], 2),
        "fills_per_session": round(row["fills_mean"], 1), "volume_per_session": round(row["volume_mean"], 1),
    }
    if full:
        out["execution"] = {
            "passive_fills_per_session": round(row.get("maker_fills_mean", 0), 1),
            "aggressive_fills_per_session": round(row.get("taker_fills_mean", 0), 1),
            "venue_fees_per_session": round(row.get("fees_mean", 0), 3),
            "_fees_note": "signed dollars: negative means you were paid rebates for resting liquidity",
        }
        out["your_latency_ns"] = {
            "react_p50": round(row.get("react_p50_ns", 0)),
            "react_p99": round(row.get("react_p99_ns", 0)),
            "react_max": round(row.get("react_max_ns", 0)),
            "orders_measured": round(row.get("react_count_mean", 0)),
            "_note": "nanoseconds from your on_event/on_idle call receiving an event to your order "
                     "reaching the outbound queue. This is your own code's execution time.",
        }
    return out


def build_report(info, subject_idx, summary, runs, specs):
    """What one subject gets to see. `specs` is the full agent spec list by id."""
    rows = summary["agents"]
    me = rows[subject_idx]
    report = {"you": agent_report(me, full=True)}
    if info in ("market", "rivals"):
        e = summary["engine"]
        price_moves = [r["last_px"] - r["initial_px"] for r in runs]
        report["market"] = {
            "sessions": e["runs"],
            "trades_per_session": round(e["trades"]["mean"]),
            "commands_per_sec": round(e["commands_per_sec"]["mean"]),
            "price_change_mean": round(statistics.fmean(price_moves), 2),
            "price_change_std": round(statistics.pstdev(price_moves), 2) if len(price_moves) > 1 else 0.0,
            "final_spread_ticks_median": round(statistics.median(
                (r["final_ask"] - r["final_bid"]) / 0.01 for r in runs if r["final_bid_qty"] and r["final_ask_qty"]), 2)
            if any(r["final_bid_qty"] and r["final_ask_qty"] for r in runs) else None,
        }
    if info == "rivals":
        report["others"] = [
            {"id": row["id"], "spec": specs[row["id"]], **agent_report(row)}
            for row in rows if row["id"] != subject_idx
        ]
    return report


SUBJECT_KEYS = {"info", "rule", "start", "sigma", "persona"}


def parse_subject(spec, defaults):
    """name[:k=v,...] with keys info, rule, start, sigma, persona. Unset keys take the run defaults."""
    name, _, rest = spec.partition(":")
    sub = dict(defaults, name=name)
    for item in filter(None, rest.split(",")):
        k, _, v = item.partition("=")
        if k not in SUBJECT_KEYS:
            raise SystemExit(f"subject {name}: unknown key {k} (allowed: {sorted(SUBJECT_KEYS)})")
        sub[k] = float(v) if k == "sigma" else v
    if sub["info"] not in ("own", "market", "rivals"):
        raise SystemExit(f"subject {name}: bad info {sub['info']}")
    if sub["rule"] not in list(RULES) + ["llm", "llm-code", "mock-code"]:
        raise SystemExit(f"subject {name}: bad rule {sub['rule']}")
    return sub


def make_proposer(i, sub, args, code_dir):
    if sub["rule"] in RULES:
        return RULES[sub["rule"]](seed=i, start_strategy=sub["start"], sigma=sub["sigma"])
    if sub["rule"] == "mock-code":
        from codegen import MockCodeProposer
        return MockCodeProposer(code_dir, seed=i, binary=args.binary)
    if sub["rule"] == "llm":
        from proposer import ClaudeProposer
        return ClaudeProposer(model=args.model, persona=sub["persona"], effort=args.effort)
    from codegen import ClaudeCodeProposer
    return ClaudeCodeProposer(code_dir, model=args.model, persona=sub["persona"], effort=args.effort, binary=args.binary)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--generations", type=int, default=5)
    ap.add_argument("--seeds", type=int, default=8, help="seeds per evaluation")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--subject", action="append", default=[],
                    help="name[:info=..,rule=..,start=..,sigma=..]; repeatable. Unset keys use the flags below")
    ap.add_argument("--background", default=",".join(DEFAULT_BACKGROUND), help="comma-separated fixed agent specs")
    ap.add_argument("--info", choices=["own", "market", "rivals"], default="own", help="default context level")
    ap.add_argument("--rule", choices=list(RULES) + ["llm", "llm-code", "mock-code"], default="hillclimb",
                    help="default adaptation rule. hillclimb/imitate/bandit are algorithmic; "
                         "llm/llm-code ask a model and need credentials; mock-code tests the plugin pipeline")
    ap.add_argument("--sigma", type=float, default=0.3, help="default mutation size (log-normal)")
    ap.add_argument("--start", default="momentum,meanrev,mm",
                    help="comma list of default starting strategies, assigned to subjects in order")
    ap.add_argument("--fixed-seeds", action="store_true",
                    help="evaluate every generation on the same seeds (less noise, more overfitting)")
    ap.add_argument("--model", default="claude-opus-5")
    ap.add_argument("--effort", default="high")
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--out", required=True)
    ap.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    args = ap.parse_args()

    starts = [x for x in args.start.split(",") if x]
    subjects = []
    for i, spec in enumerate(args.subject or ["agent"]):
        defaults = {"info": args.info, "rule": args.rule, "start": starts[i % len(starts)],
                    "sigma": args.sigma, "persona": "neutral"}
        subjects.append(parse_subject(spec, defaults))
    names = [s["name"] for s in subjects]
    if len(set(names)) != len(names):
        raise SystemExit("subject names must be unique")
    background = [b for b in args.background.split(",") if b]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "config.json").write_text(json.dumps({**vars(args), "subjects": subjects, "background": background}, indent=1))

    code_dir = out / "code"
    proposers = {s["name"]: make_proposer(i, s, args, code_dir) for i, s in enumerate(subjects)}
    history = {s["name"]: [] for s in subjects}
    last_summary, last_runs, last_specs = None, None, None
    n_bg = len(background)

    for gen in range(args.generations):
        t0 = time.time()
        proposals = {}
        for i, sub in enumerate(subjects):
            name = sub["name"]
            idx = n_bg + i
            report = build_report(sub["info"], idx, last_summary, last_runs, last_specs) if last_summary else None
            proposal, warnings = proposers[name].propose(name, history[name], report)
            proposals[name] = proposal
            for w in warnings:
                print(f"  [{name}] warning: {w}")
            print(f"gen {gen} {name:<10} [{sub['rule']}/{sub['info']}] -> {spec_of(proposal)}")
            print(f"      {proposal.rationale[:300]}")

        specs = background + [spec_of(proposals[s["name"]]) for s in subjects]
        base = 1 if args.fixed_seeds else gen * 1000 + 1
        seeds = list(range(base, base + args.seeds))
        from concurrent.futures import ThreadPoolExecutor
        with ThreadPoolExecutor(max_workers=max(1, args.parallel)) as pool:
            runs = list(pool.map(lambda s: sweep.run_one(args.binary, s, args.seconds, specs), seeds))
        summary = sweep.summarise(runs)

        gen_record = {"generation": gen, "specs": specs, "seeds": seeds, "proposals": {}, "summary": summary}
        for i, sub in enumerate(subjects):
            name = sub["name"]
            row = summary["agents"][n_bg + i]
            entry = {
                "proposal": dict(proposals[name]),
                "score": row["pnl"]["mean"], "std": row["pnl"]["std"], "hit_rate": row["pnl"]["hit_rate"],
                "median": row["pnl"]["median"], "min": row["pnl"]["min"], "max": row["pnl"]["max"],
                "fills": row["fills_mean"], "volume": row["volume_mean"],
            }
            history[name].append(entry)
            gen_record["proposals"][name] = entry
            with open(out / "trajectory.jsonl", "a") as f:
                f.write(json.dumps({"generation": gen, "subject": name, "info": sub["info"], "rule": sub["rule"],
                                    "start": sub["start"], **entry}) + "\n")
        (out / f"gen_{gen}.json").write_text(json.dumps({**gen_record, "runs": runs}, indent=1))

        sweep.print_summary(summary, specs)
        print(f"generation {gen} done in {time.time() - t0:.0f}s\n")
        last_summary, last_runs, last_specs = summary, runs, specs

    print("trajectory:")
    for sub in subjects:
        print(f"  {sub['name']:<10} [{sub['rule']}/{sub['info']}]: "
              + " -> ".join(f"{h['score']:.1f}" for h in history[sub["name"]]))
    print(f"analyse with: scripts/trajectory.py {out}")


if __name__ == "__main__":
    main()
