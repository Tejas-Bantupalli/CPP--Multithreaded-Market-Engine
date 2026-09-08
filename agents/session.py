#!/usr/bin/env python3
"""Generational experiment driven by external agents (Claude Code subagents, or people).

The loop is split in two so that any agent that can read and write files can
take part, without the simulator calling a model:

  session.py init --out DIR --subject NAME:info=..,persona=..,mode=params|code ...
      writes DIR/config.json, DIR/state.json and a brief for every subject at
      DIR/gen_0/<subject>/brief.md

  <an agent reads each brief and writes proposal.json (params mode) or strategy.cpp (code mode)>

  session.py evaluate --out DIR
      validates or compiles each proposal, runs all subjects together over the
      generation's seeds, appends trajectory.jsonl, writes gen_k.json, and
      writes the next generation's briefs (with compiler errors when a build failed)

  session.py status --out DIR
      where the experiment is and which proposals are still missing

What a subject is told about the previous generation is set by info=own|market|rivals.
Everything the subject knows is in its brief; the brief tells it not to read
anything else. Compliance is by instruction, not enforcement.
"""
import argparse
import json
import shutil
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "scripts"))

from catalogue import CATALOGUE, MARKET_DESCRIPTION, catalogue_text, to_spec  # noqa: E402
from codegen import compile_plugin, interface_text, smoke_test  # noqa: E402
from loop import DEFAULT_BACKGROUND, build_report  # noqa: E402
from proposer import PERSONAS, normalise  # noqa: E402
import sweep  # noqa: E402

SUBJECT_KEYS = {"info", "persona", "mode", "start", "model"}


# ============================================================ config / state
def parse_subject(spec, defaults):
    name, _, rest = spec.partition(":")
    sub = dict(defaults, name=name)
    for item in filter(None, rest.split(",")):
        k, _, v = item.partition("=")
        if k not in SUBJECT_KEYS:
            raise SystemExit(f"subject {name}: unknown key {k} (allowed {sorted(SUBJECT_KEYS)})")
        sub[k] = v
    if sub["info"] not in ("own", "market", "rivals"):
        raise SystemExit(f"subject {name}: bad info {sub['info']}")
    if sub["mode"] not in ("params", "code"):
        raise SystemExit(f"subject {name}: bad mode {sub['mode']}")
    if sub["persona"] not in PERSONAS:
        raise SystemExit(f"subject {name}: unknown persona {sub['persona']} (known {sorted(PERSONAS)})")
    return sub


def load_config(out):
    return json.loads((out / "config.json").read_text())


def load_state(out):
    return json.loads((out / "state.json").read_text())


def save_state(out, state):
    (out / "state.json").write_text(json.dumps(state, indent=1))


def load_history(out):
    hist = {}
    path = out / "trajectory.jsonl"
    if path.exists():
        for line in path.read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                hist.setdefault(r["subject"], []).append(r)
    return hist


# ============================================================ briefs
RULES_OF_THE_GAME = """RULES
- You control exactly one agent. Each generation you make one proposal; it is then run over several
  random seeds against the same background market and you are told how it did.
- Your objective is the highest mean PnL across seeds with a hit rate above one half. Consistency
  matters: a strategy that wins on most seeds beats one that wins big on one seed.
- Use only the information in this brief. Do not open, search, or read any other file in this
  repository: the experiment measures what you can do with the information you are given. If you
  did read something else, say so in your rationale.
- Explain your reasoning in the rationale: what you expect the change to do and why.
"""


def history_block(rows):
    if not rows:
        return "This is generation 0. There is no performance history yet.\n"
    lines = [f"Your history, oldest first (score = mean PnL in dollars over the generation's seeds):"]
    for r in rows:
        p = r["proposal"]
        what = p.get("class_name") or p.get("strategy")
        params = json.dumps(p.get("params", {}), sort_keys=True) if p.get("params") else ""
        status = "" if r.get("ok", True) else f"  [FAILED: {r.get('error_short', 'no working strategy')}; ran fallback]"
        lines.append(f"  gen {r['generation']}: {what} {params} -> mean {r['score']:.2f}, std {r['std']:.2f}, "
                     f"median {r['median']:.2f}, hit {r['hit_rate']:.2f}, fills {r['fills']:.0f}{status}")
    return "\n".join(lines) + "\n"


def write_brief(out, cfg, sub, gen, rows, report, error=None):
    d = out / f"gen_{gen}" / sub["name"]
    d.mkdir(parents=True, exist_ok=True)
    mode = sub["mode"]
    target = d / ("proposal.json" if mode == "params" else "strategy.cpp")
    parts = [f"# Brief for agent '{sub['name']}', generation {gen}\n",
             "ROLE\n" + PERSONAS[sub["persona"]] + "\n",
             RULES_OF_THE_GAME,
             "MARKET\n" + MARKET_DESCRIPTION + "\n",
             f"Each generation is evaluated over {cfg['seeds']} seeds of {cfg['seconds']} s each. "
             f"Background agents: {', '.join(cfg['background'])}. There are {len(cfg['subjects'])} adaptive agents "
             f"in the market including you.\n"]
    if mode == "params":
        parts.append("STRATEGY CATALOGUE (your action space)\n" + catalogue_text() + "\n")
    else:
        parts.append("ACTION SPACE\nYou write the complete C++17 source of a strategy plugin: one file that "
                     "defines a class deriving from Strategy and ends with MARKET_PLUGIN(YourClass).\n"
                     "- Include only \"plugin.h\" plus standard headers.\n"
                     "- on_event and on_idle run on your own thread and must return quickly: no sleeping, "
                     "no blocking, no I/O, no unbounded loops. Throttle with ctx.now().\n"
                     "- Keep positions bounded. Read tunables via Params::get(key, default).\n"
                     "- It is compiled with g++ -std=c++17 -Wall -Wextra; errors come back to you next generation "
                     "and your previous working strategy (or the default momentum strategy) trades in the meantime.\n\n"
                     "INTERFACE (verbatim)\n" + interface_text() + "\n")
    parts.append("HISTORY\n" + history_block(rows))
    if report:
        parts.append("LATEST REPORT (what you are allowed to see about the last generation)\n"
                     + json.dumps(report, indent=1, sort_keys=True) + "\n")
    if error:
        parts.append("YOUR LAST SUBMISSION FAILED\n" + error + "\n")
    if mode == "params":
        parts.append(f"OUTPUT\nWrite exactly one file: {target}\n"
                     "JSON with keys: \"strategy\" (one of " + ", ".join(CATALOGUE) + "), "
                     "\"params\" (object of name -> number; omitted parameters take defaults), "
                     "\"rationale\" (string). Nothing else. Then stop.\n")
    else:
        parts.append(f"OUTPUT\nWrite exactly one file: {target}\n"
                     "It must be the complete plugin source. Put your rationale in a comment block at the top "
                     "of the file. Then stop.\n")
    (d / "brief.md").write_text("\n".join(parts))
    return d / "brief.md"


# ============================================================ commands
def cmd_init(args):
    out = Path(args.out)
    if out.exists() and any(out.iterdir()) and not args.force:
        raise SystemExit(f"{out} exists and is not empty (use --force to wipe)")
    if out.exists() and args.force:
        shutil.rmtree(out)
    out.mkdir(parents=True)
    starts = [x for x in args.start.split(",") if x]
    subjects = []
    for i, spec in enumerate(args.subject):
        defaults = {"info": args.info, "persona": args.persona, "mode": args.mode,
                    "start": starts[i % len(starts)], "model": args.model}
        subjects.append(parse_subject(spec, defaults))
    names = [s["name"] for s in subjects]
    if len(set(names)) != len(names):
        raise SystemExit("subject names must be unique")
    cfg = {
        "subjects": subjects, "background": [b for b in args.background.split(",") if b],
        "seeds": args.seeds, "seconds": args.seconds, "generations": args.generations,
        "fixed_seeds": args.fixed_seeds, "parallel": args.parallel, "binary": args.binary,
        "created": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    (out / "config.json").write_text(json.dumps(cfg, indent=1))
    save_state(out, {"generation": 0, "last_specs": {s["name"]: None for s in subjects}, "done": False})
    for sub in subjects:
        write_brief(out, cfg, sub, 0, [], None)
    cmd_status(args)


def cmd_status(args):
    out = Path(args.out)
    cfg, state = load_config(out), load_state(out)
    gen = state["generation"]
    if state.get("done"):
        print(f"{out}: finished after {gen} generations. Analyse with scripts/trajectory.py {out}")
        return
    print(f"{out}: generation {gen} of {cfg['generations']} awaiting proposals")
    for sub in cfg["subjects"]:
        d = out / f"gen_{gen}" / sub["name"]
        target = d / ("proposal.json" if sub["mode"] == "params" else "strategy.cpp")
        print(f"  {sub['name']:<10} [{sub['mode']}/{sub['info']}/{sub['persona']}/{sub['model']}] "
              f"{'READY' if target.exists() else 'missing'}  brief: {d / 'brief.md'}  -> {target.name}")


def resolve_proposal(out, cfg, sub, gen, last_spec):
    """Turn what the agent wrote into an engine spec. Returns (proposal dict, spec, ok, error)."""
    d = out / f"gen_{gen}" / sub["name"]
    fallback_spec = last_spec or to_spec(sub["start"], {})
    if sub["mode"] == "params":
        path = d / "proposal.json"
        try:
            data = json.loads(path.read_text())
            proposal, warnings = normalise(data["strategy"], data.get("params", {}), data.get("rationale", ""))
            return dict(proposal), to_spec(proposal["strategy"], proposal["params"]), True, "\n".join(warnings) or None
        except FileNotFoundError:
            err = f"{path} was not written"
        except (ValueError, KeyError, json.JSONDecodeError) as e:
            err = f"proposal.json could not be used: {e}"
        return {"strategy": "fallback", "params": {}, "rationale": err}, fallback_spec, False, err

    path = d / "strategy.cpp"
    if not path.exists():
        err = f"{path} was not written"
        return {"strategy": "plugin", "class_name": "fallback", "params": {}, "rationale": err}, fallback_spec, False, err
    src_text = path.read_text()
    rationale = src_text[:1500]
    so, output = compile_plugin(path, d / "lib")
    if so is None:
        err = "Compilation failed:\n" + output
        return {"strategy": "plugin", "class_name": "fallback", "params": {}, "rationale": err,
                "source_path": str(path)}, fallback_spec, False, err
    ok, output = smoke_test(so, cfg["binary"])
    if not ok:
        err = "Compiled, but a short test run failed:\n" + output
        return {"strategy": "plugin", "class_name": "fallback", "params": {}, "rationale": err,
                "source_path": str(path)}, fallback_spec, False, err
    class_name = "plugin"
    for line in src_text.splitlines():
        if "MARKET_PLUGIN(" in line:
            class_name = line.split("MARKET_PLUGIN(")[1].split(")")[0].strip()
    return ({"strategy": "plugin", "class_name": class_name, "params": {}, "rationale": rationale,
             "source_path": str(path)}, f"plugin:{so}", True, None)


def cmd_evaluate(args):
    out = Path(args.out)
    cfg, state = load_config(out), load_state(out)
    if state.get("done"):
        raise SystemExit("experiment already finished")
    gen = state["generation"]
    history = load_history(out)
    subjects = cfg["subjects"]
    n_bg = len(cfg["background"])

    resolved = {}
    for sub in subjects:
        proposal, spec, ok, err = resolve_proposal(out, cfg, sub, gen, state["last_specs"].get(sub["name"]))
        resolved[sub["name"]] = (proposal, spec, ok, err)
        flag = "" if ok else "  FAILED -> fallback " + spec
        print(f"gen {gen} {sub['name']:<10} -> {spec}{flag}")
        if err and ok:
            print(f"      note: {err}")

    specs = cfg["background"] + [resolved[s["name"]][1] for s in subjects]
    base = 1 if cfg["fixed_seeds"] else gen * 1000 + 1
    seeds = list(range(base, base + cfg["seeds"]))
    t0 = time.time()
    with ThreadPoolExecutor(max_workers=max(1, cfg["parallel"])) as pool:
        runs = list(pool.map(lambda s: sweep.run_one(cfg["binary"], s, cfg["seconds"], specs), seeds))
    summary = sweep.summarise(runs)
    sweep.print_summary(summary, specs)
    print(f"evaluated in {time.time() - t0:.0f}s")

    gen_record = {"generation": gen, "specs": specs, "seeds": seeds, "proposals": {}, "summary": summary}
    for i, sub in enumerate(subjects):
        proposal, spec, ok, err = resolved[sub["name"]]
        row = summary["agents"][n_bg + i]
        entry = {
            "generation": gen, "subject": sub["name"], "info": sub["info"], "persona": sub["persona"],
            "mode": sub["mode"], "model": sub["model"], "start": sub["start"], "spec": spec, "ok": ok,
            "error_short": (err or "").splitlines()[0][:120] if err and not ok else None,
            "proposal": proposal,
            "score": row["pnl"]["mean"], "std": row["pnl"]["std"], "hit_rate": row["pnl"]["hit_rate"],
            "median": row["pnl"]["median"], "min": row["pnl"]["min"], "max": row["pnl"]["max"],
            "fills": row["fills_mean"], "volume": row["volume_mean"],
        }
        history.setdefault(sub["name"], []).append(entry)
        gen_record["proposals"][sub["name"]] = entry
        with open(out / "trajectory.jsonl", "a") as f:
            f.write(json.dumps(entry) + "\n")
        if ok:
            state["last_specs"][sub["name"]] = spec
    (out / f"gen_{gen}.json").write_text(json.dumps({**gen_record, "runs": runs}, indent=1))

    state["generation"] = gen + 1
    if gen + 1 >= cfg["generations"]:
        state["done"] = True
        save_state(out, state)
        print(f"finished {cfg['generations']} generations. Analyse with scripts/trajectory.py {out}")
        return
    for i, sub in enumerate(subjects):
        report = build_report(sub["info"], n_bg + i, summary, runs, specs)
        _, _, ok, err = resolved[sub["name"]]
        write_brief(out, cfg, sub, gen + 1, history[sub["name"]], report, error=None if ok else err)
    save_state(out, state)
    cmd_status(args)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)

    a = sp.add_parser("init")
    a.add_argument("--out", required=True)
    a.add_argument("--subject", action="append", required=True,
                   help="name[:info=own|market|rivals,persona=..,mode=params|code,start=..,model=..]")
    a.add_argument("--info", default="own")
    a.add_argument("--persona", default="neutral")
    a.add_argument("--mode", default="params", choices=["params", "code"])
    a.add_argument("--model", default="unspecified", help="label only: which agent model will answer the briefs")
    a.add_argument("--start", default="momentum,meanrev,mm")
    a.add_argument("--background", default=",".join(DEFAULT_BACKGROUND))
    a.add_argument("--generations", type=int, default=6)
    a.add_argument("--seeds", type=int, default=6)
    a.add_argument("--seconds", type=float, default=2.0)
    a.add_argument("--fixed-seeds", action="store_true")
    a.add_argument("--parallel", type=int, default=3)
    a.add_argument("--binary", default=str(sweep.DEFAULT_BINARY))
    a.add_argument("--force", action="store_true")
    a.set_defaults(fn=cmd_init)

    e = sp.add_parser("evaluate")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_evaluate)

    s = sp.add_parser("status")
    s.add_argument("--out", required=True)
    s.set_defaults(fn=cmd_status)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
