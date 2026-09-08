#!/usr/bin/env python3
"""Summarise one or more agents/loop.py runs: how each subject's trading changed over generations.

    scripts/trajectory.py results/loop-a [results/loop-b ...] [--csv out.csv]

Per subject: rule, context level, first and last score, best score and when, mean of the
last third of generations, hit rate, number of strategy switches, imitation events,
and the mean absolute parameter change per generation (how much it moved).
"""
import argparse
import json
import math
import statistics
from pathlib import Path


def load(run_dir):
    rows = [json.loads(l) for l in (Path(run_dir) / "trajectory.jsonl").read_text().splitlines() if l.strip()]
    by = {}
    for r in rows:
        by.setdefault(r["subject"], []).append(r)
    for v in by.values():
        v.sort(key=lambda r: r["generation"])
    return by


def param_distance(a, b):
    """Mean |log ratio| over shared parameters; 0 if strategies differ (counted as a switch instead)."""
    if a["strategy"] != b["strategy"]:
        return 0.0
    ks = [k for k in a["params"] if k in b["params"] and a["params"][k] > 0 and b["params"][k] > 0]
    if not ks:
        return 0.0
    return statistics.fmean(abs(math.log(b["params"][k] / a["params"][k])) for k in ks)


def summarise(name, rows):
    scores = [r["score"] for r in rows]
    n = len(scores)
    best_i = max(range(n), key=lambda i: scores[i])
    tail = scores[-max(1, n // 3):]
    switches = sum(1 for i in range(1, n) if rows[i]["proposal"]["strategy"] != rows[i - 1]["proposal"]["strategy"])
    imitations = sum(1 for r in rows if r["proposal"].get("rationale", "").startswith("imitate: copy"))
    moves = [param_distance(rows[i - 1]["proposal"], rows[i]["proposal"]) for i in range(1, n)]
    return {
        "subject": name, "rule": rows[0].get("rule") or f"llm/{rows[0].get('model', '?')}", "info": rows[0].get("info", "?"),
        "generations": n, "first": scores[0], "last": scores[-1], "best": scores[best_i], "best_gen": best_i,
        "tail_mean": statistics.fmean(tail), "tail_hit": statistics.fmean(r["hit_rate"] for r in rows[-len(tail):]),
        "improvement": statistics.fmean(tail) - scores[0],
        "switches": switches, "imitations": imitations,
        "move": statistics.fmean(moves) if moves else 0.0,
        "final_strategy": rows[-1]["proposal"]["strategy"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--csv", default="")
    args = ap.parse_args()

    rows_out = []
    print(f"{'run':<18} {'subject':<10} {'rule':<9} {'info':<7} {'gens':>4} {'first':>8} {'last':>8} {'best':>8} "
          f"{'@':>3} {'tail':>8} {'improve':>8} {'hit':>5} {'sw':>3} {'imit':>4} {'move':>5} final")
    for run in args.runs:
        by = load(run)
        for name, rows in by.items():
            s = summarise(name, rows)
            s["run"] = Path(run).name
            rows_out.append(s)
            print(f"{s['run'][:18]:<18} {name[:10]:<10} {s['rule']:<9} {s['info']:<7} {s['generations']:>4} "
                  f"{s['first']:>8.2f} {s['last']:>8.2f} {s['best']:>8.2f} {s['best_gen']:>3} {s['tail_mean']:>8.2f} "
                  f"{s['improvement']:>8.2f} {s['tail_hit']:>5.2f} {s['switches']:>3} {s['imitations']:>4} "
                  f"{s['move']:>5.2f} {s['final_strategy']}")
        print(f"  {Path(run).name} curves:")
        for name, rows in by.items():
            print(f"    {name:<10} " + " ".join(f"{r['score']:6.1f}" for r in rows))
    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows_out[0].keys()))
            w.writeheader()
            w.writerows(rows_out)
        print(f"wrote {args.csv}")


if __name__ == "__main__":
    main()
