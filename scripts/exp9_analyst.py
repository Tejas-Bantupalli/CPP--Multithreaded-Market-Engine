#!/usr/bin/env python3
"""Build the blind analyst dossiers for Experiment 9's mixed field.

    scripts/exp9_analyst.py results/exp9_field_c --panel h1,h2,s1,s2 --out results/exp9_analysts

Two of the four agents in the mixed field were written by a cheap model and two by a stronger
one. The dossier gives an analyst the *trading record only* — scores, distributions, fills,
execution mix, and each agent's own forecast of itself — with the source code withheld and the
agent names left as they were, which carry no model information. The analyst must say which two
were the cheap ones and why.

The answer key lives in the field's config.json and is deliberately not written into any
dossier. Analysts are spawned as `trader` subagents, which can only read the file they are
pointed at, so the dossier is their entire world.
"""
import argparse
import json
import statistics
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from exp9_report import load_field  # noqa: E402

TASK = """# Analyst brief — {name}

You are a quantitative analyst. Below is the complete trading record of four automated agents
that competed in the same simulated market over {gens} generations. Each agent rewrote its own
trading strategy in C++ before every generation, from a report on how the previous generation
went. You are not being shown the code, only the record of what the code did.

Two of these four agents were written by a **small, cheap language model**. The other two were
written by a **larger, more capable one**. The agent names were assigned at random and tell you
nothing. Neither did the agents know which of their rivals was which.

Each generation an agent also had to record a FORECAST: the mean PnL in dollars it expected to
score, the rank it expected to finish (1 = best of the four), and its confidence from 0 to 1.
The forecast was written before the generation ran and never affected scoring, so it is an
honest record of what each agent believed about itself.

## Definitions

- `score` — mean PnL in dollars over the generation's {seeds} random seeds. This is the objective.
- `std` / `median` / `hit` — spread of PnL across those seeds, and the fraction of seeds with
  positive PnL. High mean with low hit rate means a few seeds carried the average.
- `fills` — trades executed per session. `passive` / `aggressive` split them into orders that
  rested and were hit, versus orders that crossed the spread and paid for immediacy.
- `lines` — length of the strategy source that generation. `features` — which optional parts of
  the engine interface that source touched.
- `FAILED` — the submission did not compile or crashed a test run; the agent's previous working
  strategy traded in its place that generation, and the error was reported back to it.

## The record

{record}

## What to write

Write exactly one file: `{out}`

Markdown, with these sections and nothing else:

1. **Verdict** — a line of the exact form `CHEAP: <name>, <name>` naming the two agents you
   believe were written by the small model, then `CONFIDENCE: <0.0-1.0>`.
2. **Evidence** — the three strongest specific observations behind that call, each tied to
   numbers in the record above.
3. **What separated the strong pair from the weak pair** — your account of the mechanism, not
   just the correlation. If you think the difference shows up in *self-knowledge* rather than in
   *trading*, say so and show where.
4. **What you could not tell** — the part of the record that is genuinely ambiguous, and what
   one extra column would have resolved it.

Do not read any other file. The record above is your entire world; the answer is not written
anywhere you can reach, and guessing from this record is the whole task.
"""


def render_record(field):
    lines = []
    for name, seq in field["by_subject"].items():
        lines.append(f"\n### Agent `{name}`\n")
        lines.append("| gen | score | std | median | hit | fills | passive | aggressive | lines | features | forecast pnl | forecast rank | conf | note |")
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for r in seq:
            f = r["forecast"] or {}
            note = "FAILED" if not r["ok"] else ""
            gen_json = field["gen_cache"].get(r["generation"], {})
            ex = gen_json.get(name, {})
            lines.append(
                f"| {r['generation']} | {r['score']:.2f} | {r['std']:.2f} | {r['median']:.2f} | "
                f"{r['hit_rate']:.2f} | {r['fills']:.0f} | {ex.get('passive', '?')} | "
                f"{ex.get('aggressive', '?')} | {r['n_lines']} | {','.join(r['features']) or '-'} | "
                f"{f.get('pnl', '—') if f else '—'} | {f.get('rank', '—') if f else '—'} | "
                f"{f.get('confidence', '—') if f else '—'} | {note} |")
    lines.append("\n### Final standings (mean score across all generations)\n")
    tot = {n: statistics.fmean(r["score"] for r in seq) for n, seq in field["by_subject"].items()}
    for n, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        lines.append(f"- `{n}`: {v:.2f}")
    return "\n".join(lines)


def execution_cache(run):
    """Per-generation passive/aggressive fill split, pulled from each gen_<k>.json."""
    cache = {}
    for path in sorted(Path(run).glob("gen_*.json")):
        data = json.loads(path.read_text())
        gen = data["generation"]
        n_bg = len(data["specs"]) - len(data["proposals"])
        cache[gen] = {}
        for i, (name, _) in enumerate(data["proposals"].items()):
            row = data["summary"]["agents"][n_bg + i]
            cache[gen][name] = {"passive": round(row.get("maker_fills_mean", 0)),
                                "aggressive": round(row.get("taker_fills_mean", 0))}
    return cache


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--panel", default="h1,h2,s1,s2", help="comma list of analyst names")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    field = load_field(args.run)
    field["gen_cache"] = execution_cache(args.run)
    record = render_record(field)
    gens = len({r["generation"] for r in field["rows"]})

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "answer_key.json").write_text(json.dumps({
        "field": field["dir"], "key": field["models"],
        "note": "Not visible to any analyst. Dossiers withhold the model labels and the source.",
    }, indent=1))

    for name in [x for x in args.panel.split(",") if x]:
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "brief.md").write_text(TASK.format(
            name=name, gens=gens, seeds=field["config"]["seeds"],
            record=record, out=(d / "verdict.md").resolve()))
        print(f"{(d / 'brief.md').resolve()}  -> verdict.md")


if __name__ == "__main__":
    main()
