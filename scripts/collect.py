#!/usr/bin/env python3
"""Aggregate every experiment under results/ into one JSON for reporting.

    scripts/collect.py results/exp1_info results/exp2_persona ... --out results/all.json

For each run: config, per-subject curves (score, std, hit rate, fills per
generation), the trajectory summary from scripts/trajectory.py, the specs and
rationales chosen each generation, and per-generation background-agent PnL so
the adaptive agents can be compared against the market they traded in.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from trajectory import load, summarise  # noqa: E402


def collect_run(run_dir):
    run = Path(run_dir)
    cfg = json.loads((run / "config.json").read_text()) if (run / "config.json").exists() else {}
    by = load(run)
    subjects = {}
    for name, rows in by.items():
        subjects[name] = {
            "summary": summarise(name, rows),
            "meta": {k: rows[0].get(k) for k in ("info", "rule", "persona", "mode", "model", "start")},
            "curve": [{
                "generation": r["generation"], "score": r["score"], "std": r["std"], "hit_rate": r["hit_rate"],
                "median": r.get("median"), "min": r.get("min"), "max": r.get("max"),
                "fills": r["fills"], "volume": r.get("volume"), "ok": r.get("ok", True),
                "spec": r.get("spec"), "strategy": r["proposal"].get("strategy"),
                "class_name": r["proposal"].get("class_name"),
                "params": r["proposal"].get("params", {}),
                "rationale": (r["proposal"].get("rationale") or "")[:1200],
            } for r in rows],
        }
    # background agents per generation, from gen_k.json summaries
    background = []
    k = 0
    while (run / f"gen_{k}.json").exists():
        g = json.loads((run / f"gen_{k}.json").read_text())
        n_bg = len(cfg.get("background", [])) if cfg.get("background") else len(g["specs"]) - len(by)
        background.append({
            "generation": k,
            "agents": [{"id": a["id"], "name": a["name"], "mean_pnl": a["pnl"]["mean"], "std": a["pnl"]["std"],
                        "hit_rate": a["pnl"]["hit_rate"], "fills": a["fills_mean"]}
                       for a in g["summary"]["agents"][:n_bg]],
            "engine": {"commands_per_sec": g["summary"]["engine"]["commands_per_sec"]["mean"],
                       "trades_per_run": g["summary"]["engine"]["trades"]["mean"],
                       "match_p99_ns": g["summary"]["engine"]["match_p99_ns"],
                       "submit_to_pop_p99_ns": g["summary"]["engine"]["submit_to_pop_p99_ns"]},
            "seeds": g["seeds"],
        })
        k += 1
    return {"name": run.name, "config": cfg, "subjects": subjects, "background": background}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    data = {"runs": [collect_run(r) for r in args.runs]}
    Path(args.out).write_text(json.dumps(data, indent=1))
    for r in data["runs"]:
        print(f"{r['name']}: {len(r['subjects'])} subjects, {len(r['background'])} generations")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
