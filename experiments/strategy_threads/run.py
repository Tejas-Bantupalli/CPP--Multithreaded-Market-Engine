#!/usr/bin/env python3
"""Run the strategy-threads matrix: every cell once per round, shuffled per round.

Each round visits every configuration in a fresh deterministic order, so slow
drift on a shared machine (other apps, thermals) lands on every arrangement
instead of on whichever happened to run last. One process per session; raw
reports are flushed as they arrive.

    python3 experiments/strategy_threads/run.py --family handoff --out results/strategy-threads-handoff-YYYY-MM-DD
    python3 experiments/strategy_threads/analyze.py results/strategy-threads-handoff-YYYY-MM-DD
"""

import argparse
import datetime
import hashlib
import itertools
import json
import os
import platform
import random
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
BINARY = ROOT / "build" / "strategy_threads"
SOURCES = ["experiments/strategy_threads/bench.cpp", "src/engine.cpp", "src/order_book.cpp",
           "src/logger.cpp", "src/report.cpp"]
BUILD = ["c++", "-O2", "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Iinclude", *SOURCES,
         "-o", str(BINARY.relative_to(ROOT))]

HOUSEKEEPING = {"light": (1024, 250), "heavy": (16384, 1000)}  # (window, every)


# (topology, queue, burner) for the handoff family.
HANDOFF_ARRANGEMENTS = [
    ("single", "none", False), ("single", "none", True),
    ("forward", "spsc", False), ("forward", "mutex_cv", False), ("forward", "futex", False),
    ("split", "spsc", False), ("split", "mutex_cv", False), ("split", "futex", False),
]


def cells():
    out = []
    # Data path: where the reading and the calculation run.
    for topo, work, pattern in itertools.product(["single", "forward", "split"], [0, 4000, 16000],
                                                 ["smooth", "burst"]):
        out.append(dict(family="datapath", topology=topo, work=work, pattern=pattern, wait="spin",
                        hk="none", hk_cost=None))
    for topo, work, pattern in itertools.product(["single", "forward", "split"], [0, 16000],
                                                 ["smooth", "burst"]):
        out.append(dict(family="datapath", topology=topo, work=work, pattern=pattern, wait="yield",
                        hk="none", hk_cost=None))
    # Housekeeping: same cheap trading path; periodic statistics nowhere, inline, or on a thread.
    # The "none" control is the datapath cell single/0/spin.
    for hk, cost, pattern in itertools.product(["inline", "thread"], ["light", "heavy"], ["smooth", "burst"]):
        out.append(dict(family="housekeeping", topology="single", work=0, pattern=pattern, wait="spin",
                        hk=hk, hk_cost=cost))
    # Handoff: which queue joins reader and worker, against single and single + an idle spinning thread.
    for (topo, queue, burner), work, pattern in itertools.product(HANDOFF_ARRANGEMENTS, [0, 16000],
                                                                  ["smooth", "burst"]):
        out.append(dict(family="handoff", topology=topo, queue=queue, burner=burner, work=work,
                        pattern=pattern, wait="spin", hk="none", hk_cost=None))
    return out


def command(cell, steps):
    cmd = [str(BINARY), "--topology", cell["topology"], "--hk", cell["hk"], "--work", str(cell["work"]),
           "--pattern", cell["pattern"], "--wait", cell["wait"], "--steps", str(steps)]
    if cell.get("queue", "none") != "none":
        cmd += ["--queue", cell["queue"]]
    if cell.get("burner"):
        cmd += ["--burner"]
    if cell["hk_cost"]:
        window, every = HOUSEKEEPING[cell["hk_cost"]]
        cmd += ["--hk-window", str(window), "--hk-every", str(every)]
    return cmd


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sysctl(name):
    try:
        return subprocess.run(["sysctl", "-n", name], capture_output=True, text=True, timeout=5).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--repeats", type=int, default=6)
    ap.add_argument("--steps", type=int, default=12032)  # multiple of the ladder's 64 orders per level
    ap.add_argument("--seed", type=int, default=20260911)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--family", choices=["handoff", "datapath", "housekeeping"], default="handoff",
                    help="housekeeping also runs its no-housekeeping controls")
    args = ap.parse_args()

    if args.steps <= 0 or args.steps % 64:
        ap.error("--steps must be a positive multiple of 64")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=False)
    if not args.no_build:
        subprocess.run(BUILD, cwd=ROOT, check=True)

    matrix = cells()
    if args.family in ("datapath", "handoff"):
        matrix = [c for c in matrix if c["family"] == args.family]
    elif args.family == "housekeeping":
        matrix = [c for c in matrix if c["family"] == "housekeeping"
                  or (c["topology"] == "single" and c["work"] == 0 and c["wait"] == "spin")]
    compiler = subprocess.run(["c++", "--version"], capture_output=True, text=True).stdout.strip()
    (out / "config.json").write_text(json.dumps({
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "repeats": args.repeats, "steps": args.steps, "seed": args.seed, "family": args.family,
        "housekeeping": HOUSEKEEPING, "cells": matrix,
        "build": BUILD, "compiler": compiler, "binary_sha256": sha256(BINARY),
        "sources_sha256": {s: sha256(ROOT / s) for s in SOURCES + ["include/engine.h", "include/strategy.h",
                                                                   "include/multicast_ring.h", "include/spsc_queue.h",
                                                                   "include/transport.h"]},
        "platform": platform.platform(), "machine": sysctl("hw.model"), "cpu": sysctl("machdep.cpu.brand_string"),
        "perf_cores": sysctl("hw.perflevel0.physicalcpu"), "efficiency_cores": sysctl("hw.perflevel1.physicalcpu"),
    }, indent=2))

    total = len(matrix) * args.repeats
    done = 0
    for rnd in range(args.repeats):
        order = list(range(len(matrix)))
        random.Random(args.seed + rnd).shuffle(order)
        for idx in order:
            cell = matrix[idx]
            cmd = command(cell, args.steps)
            load_before = os.getloadavg()
            started = datetime.datetime.now(datetime.timezone.utc).isoformat()
            try:
                p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
                code, stdout, stderr = p.returncode, p.stdout, p.stderr
            except subprocess.TimeoutExpired as e:
                code, stdout, stderr = "timeout", e.stdout or "", e.stderr or ""
            try:
                report = json.loads(stdout.strip().splitlines()[-1])
            except (ValueError, IndexError):
                report = None
            row = dict(round=rnd, cell=idx, **cell, command=cmd, started_utc=started, load_before=load_before,
                       load_after=os.getloadavg(), exit_code=code, stderr=stderr[-2000:], report=report)
            with (out / "raw.jsonl").open("a") as f:
                f.write(json.dumps(row) + "\n")
            done += 1
            valid = bool(report and report.get("valid"))
            e2o = report["probe"]["event_to_order"]["p50"] / 1000 if report else float("nan")
            print(f"[{done}/{total}] r{rnd} {cell['topology']:7} q={cell.get('queue', '-'):8} "
                  f"burner={int(bool(cell.get('burner')))} work={cell['work']:<5} {cell['pattern']:6} "
                  f"{cell['wait']:5} hk={cell['hk']}/{cell['hk_cost']} valid={valid} e2o_p50={e2o:.1f}us "
                  f"load={load_before[0]:.1f}", flush=True)
            if report is None:
                print(stderr[-2000:], file=sys.stderr)
                if code == 2:
                    sys.exit("bench rejected its arguments; stopping instead of recording every cell as a failure")
            time.sleep(0.1)


if __name__ == "__main__":
    main()
