#!/usr/bin/env python3
"""Re-run every generation of a session.py experiment back to back, interleaved, for latency.

    scripts/latency_rerun.py results/exp8_hft_scratch [--seeds 8] [--json out.json]

Generations are evaluated minutes or hours apart, and the machine does not hold still in
between: the same binaries on the same seeds can come back several times slower in the tail
an hour later. That makes the latency column of gen_k.json unfit for comparing generations.

This replays each generation's exact specs (the four agents of that generation together, as
they were evaluated) in interleaved rounds: round r runs seed r of generation 0, then of 1,
then of 2, and so on. Drift then lands on every generation alike. Seeds default to the
generation-0 seeds, so every generation also faces the same market randomness.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sweep  # noqa: E402

FIELDS = ["tick_to_trade_p50_ns", "tick_to_trade_p99_ns", "delivery_p50_ns", "delivery_p99_ns",
          "react_p50_ns", "react_p99_ns"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--seeds", type=int, default=None, help="how many of the gen-0 seeds to use (default all)")
    ap.add_argument("--json")
    args = ap.parse_args()

    out = Path(args.out)
    cfg = json.loads((out / "config.json").read_text())
    names = [s["name"] for s in cfg["subjects"]]
    n_bg = len(cfg["background"])
    gens = []
    while (out / f"gen_{len(gens)}.json").exists():
        gens.append(json.loads((out / f"gen_{len(gens)}.json").read_text()))
    seeds = gens[0]["seeds"][: args.seeds]

    runs = {g: [] for g in range(len(gens))}
    for r, seed in enumerate(seeds):
        for g, rec in enumerate(gens):
            runs[g].append(sweep.run_one(cfg["binary"], seed, cfg["seconds"], rec["specs"]))
        print(f"round {r + 1}/{len(seeds)} done", file=sys.stderr)

    result = {"seeds": seeds, "generations": {}}
    for g in runs:
        summ = sweep.summarise(runs[g])
        result["generations"][g] = {
            "engine": {k: summ["engine"][k] for k in ("commands_per_sec", "trades", "submit_to_pop_p50_ns",
                                                      "submit_to_pop_p99_ns")},
            "agents": {name: {"pnl_mean": summ["agents"][n_bg + i]["pnl"]["mean"],
                              **{k: summ["agents"][n_bg + i].get(k) for k in FIELDS}}
                       for i, name in enumerate(names)},
        }

    def us(x):
        return f"{x / 1e3:7.1f}" if x is not None else "      -"

    print(f"interleaved re-run over {len(seeds)} seeds; latencies in microseconds (median across seeds)")
    for name in names:
        print(f"\n== {name}\n gen      pnl  t2t50  t2t99 data50 data99  cmp50  cmp99")
        for g, rec in result["generations"].items():
            a = rec["agents"][name]
            print(f" {g:>3} {a['pnl_mean']:8.0f} " + " ".join(us(a[k]) for k in FIELDS))
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
