#!/usr/bin/env python3
"""Experiment 9 (Cheap Seats): join each field's trajectory with the FORECAST line the agent
wrote into its own source, and compare models on trading *and* on self-knowledge.

    scripts/exp9_report.py results/exp9_field_a results/exp9_field_b results/exp9_field_c
    scripts/exp9_report.py results/exp9_field_c --per-agent --json results/exp9.json

Score is mean PnL over the generation's seeds. The forecast is what the agent predicted for
itself *before* that generation ran, so forecast error is an honest calibration measure: it was
recorded in the source file and never fed back into scoring.

Reported per model (the label in config.json, which no brief ever sees):

  score        mean score, and the mean over the last third of generations (tail)
  broke        fraction of generations whose submission failed to compile or smoke-test
  opt          mean signed PnL forecast error, forecast - actual. Positive = optimistic.
  |err|        mean absolute PnL forecast error
  rank_opt     mean signed rank error, forecast - actual. Negative = flattered itself.
  conf         mean stated confidence
  conf_slope   mean change in confidence after a generation that scored below its forecast.
               Negative is the calibrated response: be less sure after being wrong.
"""
import argparse
import json
import re
import statistics
from pathlib import Path

FORECAST_RE = re.compile(r"FORECAST(?P<body>[^\n]*)")
KV_RE = re.compile(r"(pnl|rank|confidence)\s*=\s*(-?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)")

# Cheap proxies for how much of the interface the agent reached for.
FEATURES = {
    "transport": re.compile(r"\bTransportKind::"),
    "run_override": re.compile(r"\bvoid\s+run\s*\("),
    "params": re.compile(r"\bp(?:arams)?\s*\.\s*get\s*\(|Params::get"),
    "cancel": re.compile(r"ctx\s*\.\s*cancel\s*\("),
}


def parse_forecast(text):
    """Pull the FORECAST line out of a source file. Tolerant of ordering and punctuation.

    Agents sometimes discuss their forecast in prose before stating it, so take the first
    match that actually carries a pnl= value rather than the first match of the word.
    """
    for m in FORECAST_RE.finditer(text):
        kv = {k: float(v) for k, v in KV_RE.findall(m.group("body"))}
        if "pnl" in kv:
            return {"pnl": kv["pnl"], "rank": kv.get("rank"), "confidence": kv.get("confidence")}
    return None


def load_field(run_dir):
    run = Path(run_dir)
    cfg = json.loads((run / "config.json").read_text())
    models = {s["name"]: s.get("model", "?") for s in cfg["subjects"]}
    rows = [json.loads(l) for l in (run / "trajectory.jsonl").read_text().splitlines() if l.strip()]

    by_gen = {}
    for r in rows:
        by_gen.setdefault(r["generation"], []).append(r)
    for gen_rows in by_gen.values():  # realised rank within the field, 1 = best
        for i, r in enumerate(sorted(gen_rows, key=lambda x: -x["score"])):
            r["rank_actual"] = i + 1

    for r in rows:
        r["model"] = models.get(r["subject"], r.get("model", "?"))
        src = run / f"gen_{r['generation']}" / r["subject"] / "strategy.cpp"
        text = src.read_text() if src.exists() else ""
        r["forecast"] = parse_forecast(text) if text else None
        r["n_lines"] = text.count("\n") if text else 0
        r["features"] = sorted(k for k, rx in FEATURES.items() if rx.search(text)) if text else []

    by_subject = {}
    for r in sorted(rows, key=lambda x: x["generation"]):
        by_subject.setdefault(r["subject"], []).append(r)
    return {"dir": run.name, "config": cfg, "models": models, "rows": rows, "by_subject": by_subject}


def calibration(rows):
    """Forecast-vs-outcome aggregates over rows that carry a forecast."""
    fc = [r for r in rows if r["forecast"]]
    if not fc:
        return {"n": 0}
    errs = [r["forecast"]["pnl"] - r["score"] for r in fc]
    rank_errs = [r["forecast"]["rank"] - r["rank_actual"] for r in fc if r["forecast"].get("rank")]
    confs = [r["forecast"]["confidence"] for r in fc if r["forecast"].get("confidence") is not None]

    # How confidence moves after a generation that came in under its own forecast.
    slopes = []
    by_sub = {}
    for r in fc:
        by_sub.setdefault(r["subject"], []).append(r)
    for seq in by_sub.values():
        seq.sort(key=lambda x: x["generation"])
        for a, b in zip(seq, seq[1:]):
            ca, cb = a["forecast"].get("confidence"), b["forecast"].get("confidence")
            if ca is None or cb is None:
                continue
            if a["score"] < a["forecast"]["pnl"]:  # it overshot; did it get humbler?
                slopes.append(cb - ca)
    return {
        "n": len(fc), "missing": len(rows) - len(fc),
        "opt": statistics.fmean(errs), "abs_err": statistics.fmean(abs(e) for e in errs),
        "rank_opt": statistics.fmean(rank_errs) if rank_errs else None,
        "conf": statistics.fmean(confs) if confs else None,
        "conf_slope": statistics.fmean(slopes) if slopes else None,
    }


def model_summary(rows):
    out = {}
    for model in sorted({r["model"] for r in rows}):
        mine = [r for r in rows if r["model"] == model]
        gens = sorted({r["generation"] for r in mine})
        tail_gens = gens[-max(1, len(gens) // 3):]
        tail = [r for r in mine if r["generation"] in tail_gens]
        out[model] = {
            "agents": len({r["subject"] for r in mine}), "rows": len(mine),
            "score": statistics.fmean(r["score"] for r in mine),
            "tail": statistics.fmean(r["score"] for r in tail),
            "hit": statistics.fmean(r["hit_rate"] for r in mine),
            "fills": statistics.fmean(r["fills"] for r in mine),
            "broke": statistics.fmean(0.0 if r["ok"] else 1.0 for r in mine),
            "lines": statistics.fmean(r["n_lines"] for r in mine),
            "features": statistics.fmean(len(r["features"]) for r in mine),
            **calibration(mine),
        }
    return out


def fmt(v, spec=">8.2f", dash="     -- "):
    return dash if v is None else format(v, spec)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--per-agent", action="store_true", help="also print every agent's generation-by-generation line")
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    report = {}
    for run in args.runs:
        f = load_field(run)
        report[f["dir"]] = {"models": model_summary(f["rows"]), "by_subject": {}}

        print(f"\n=== {f['dir']}  ({len(f['by_subject'])} agents, "
              f"{len({r['generation'] for r in f['rows']})} generations) ===")
        print(f"{'model':<8} {'n':>3} {'score':>8} {'tail':>8} {'hit':>5} {'fills':>7} {'broke':>6} "
              f"{'lines':>6} {'feat':>5} | {'opt':>8} {'|err|':>8} {'rank_opt':>8} {'conf':>5} {'conf_sl':>7}")
        for model, s in report[f["dir"]]["models"].items():
            print(f"{model:<8} {s['agents']:>3} {s['score']:>8.2f} {s['tail']:>8.2f} {s['hit']:>5.2f} "
                  f"{s['fills']:>7.0f} {s['broke']:>6.2f} {s['lines']:>6.0f} {s['features']:>5.1f} | "
                  f"{fmt(s.get('opt'))} {fmt(s.get('abs_err'))} {fmt(s.get('rank_opt'))} "
                  f"{fmt(s.get('conf'), '>5.2f', '   --')} {fmt(s.get('conf_slope'), '>7.3f', '     --')}")

        for name, seq in f["by_subject"].items():
            report[f["dir"]]["by_subject"][name] = [
                {"generation": r["generation"], "model": r["model"], "score": r["score"],
                 "rank_actual": r["rank_actual"], "hit_rate": r["hit_rate"], "fills": r["fills"],
                 "ok": r["ok"], "error_short": r.get("error_short"), "forecast": r["forecast"],
                 "n_lines": r["n_lines"], "features": r["features"],
                 "class_name": r["proposal"].get("class_name")} for r in seq]
        print("  curves (score by generation):")
        for name, seq in f["by_subject"].items():
            marks = "".join("!" if not r["ok"] else "." for r in seq)
            print(f"    {name:<7} [{f['models'][name]:<6}] " +
                  " ".join(f"{r['score']:7.1f}" for r in seq) + f"   {marks}")
        print("  forecast vs actual (f=forecast pnl, a=actual, r=forecast rank/actual rank):")
        for name, seq in f["by_subject"].items():
            cells = []
            for r in seq:
                if r["forecast"]:
                    fr = r["forecast"].get("rank")
                    cells.append(f"f{r['forecast']['pnl']:.0f}/a{r['score']:.0f}"
                                 f"/r{fr:.0f}-{r['rank_actual']}" if fr else
                                 f"f{r['forecast']['pnl']:.0f}/a{r['score']:.0f}")
                else:
                    cells.append("--")
            print(f"    {name:<7} [{f['models'][name]:<6}] " + "  ".join(f"{c:<16}" for c in cells))

        if args.per_agent:
            print("  detail:")
            for name, seq in f["by_subject"].items():
                for r in seq:
                    print(f"    gen{r['generation']} {name:<7} {r['proposal'].get('class_name', '?'):<22} "
                          f"score {r['score']:>8.2f} rank {r['rank_actual']} lines {r['n_lines']:>4} "
                          f"feat {','.join(r['features']) or '-':<28} "
                          f"{'' if r['ok'] else 'FAILED: ' + (r.get('error_short') or '')}")

    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1))
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
