#!/usr/bin/env python3
"""Summarise a strategy-threads run: per-cell medians and within-round paired comparisons.

Cross-session numbers are medians of per-session percentiles with min-max, not
percentiles pooled over sessions. Paired ratios compare two arrangements that ran
in the same round, minutes apart, which cancels most slow drift in machine load.

Clean sessions: each probe thread times a fixed calculation every ~10 ms. On the M1 a
performance core reads ~1650-2050 ns (lower end cold, upper end after minutes of load)
and an efficiency core ~2420. A session is "clean" when every probe thread's p90 sample
is below E_CORE_NS (fewer than 10% of samples at efficiency-core speed) and the taker's
p99 lateness is under STALL_NS (no machine-wide stall: the taker runs on its own thread,
so when it is late the whole machine was).
"""

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

E_CORE_NS = 2300
STALL_NS = 200_000


def cell_of(row):
    topo = row["topology"]
    return {
        "family": row["family"], "topology": topo,
        "queue": row.get("queue", "spsc" if topo != "single" else "none"),
        "burner": bool(row.get("burner", False)),
        "work": row["work"], "pattern": row["pattern"], "wait": row["wait"],
        "hk": row["hk"], "hk_cost": row["hk_cost"],
    }


def key(row):
    c = cell_of(row)
    return tuple(c[k] for k in ("family", "topology", "queue", "burner", "work", "pattern", "wait", "hk", "hk_cost"))


def arrangement(k):
    _, topo, queue, burner, *_ = k
    if topo == "single":
        return "single + burner" if burner else "single"
    return f"{topo} / {queue}"


def label(k):
    family, topo, queue, burner, work, pattern, wait, hk, cost = k
    if family == "housekeeping" or hk != "none":
        return f"hk {hk}/{cost} {pattern}"
    if family == "handoff":
        return f"{arrangement(k)} work={work} {pattern}"
    return f"{topo} work={work} {pattern} {wait}"


def med(xs):
    return statistics.median(xs) if xs else None


def us(ns):
    return None if ns is None else ns / 1000.0


def slow_fraction(speed):
    n = speed["ns_per_1000"]["count"]
    return speed["slow"] / n if n else None


def session_metrics(row):
    r = row["report"]
    p = r["probe"]
    cpu = p["cpu_ns"]
    wall = r["process"]["wall_s"]
    probe_cpu = cpu["reader"] + cpu["worker"] + cpu["housekeeping"] + cpu.get("burner", 0)
    m = {
        "e2o_p50": p["event_to_order"]["p50"], "e2o_p90": p["event_to_order"]["p90"],
        "e2o_p99": p["event_to_order"]["p99"], "e2o_p999": p["event_to_order"]["p999"],
        "e2o_max": p["event_to_order"]["max"],
        "read_p50": p["read_delay"]["p50"], "handoff_p50": p["handoff"]["p50"], "handoff_p99": p["handoff"]["p99"],
        "compute_p50": p["compute"]["p50"],
        "ack_p50": p["order_to_ack"]["p50"], "ack_p99": p["order_to_ack"]["p99"],
        "probe_cpu_s": probe_cpu / 1e9, "worker_cpu_s": cpu["worker"] / 1e9, "hk_cpu_s": cpu["housekeeping"] / 1e9,
        "probe_cores": probe_cpu / 1e9 / wall, "process_cores": r["process"]["cpu_s"] / wall,
        "taker_cpu_s": cpu.get("taker", 0) / 1e9,
        "internal_full": p["internal_full"], "hk_full": p["hk_full"], "max_worker_run": p["max_worker_run"],
        "wakes": p.get("wakes", 0), "parks": p.get("parks", 0),
        "taker_late_p99": r["taker"]["lateness"]["p99"], "load": row["load_before"][0],
    }
    if "speed" in p:
        fracs = [slow_fraction(s) for s in p["speed"].values() if s["ns_per_1000"]["count"]]
        m["slow_max"] = max(fracs) if fracs else 0.0
        p90s = [s["ns_per_1000"]["p90"] for s in p["speed"].values() if s["ns_per_1000"]["count"]]
        m["speed_p90_max"] = max(p90s) if p90s else 0
        on_p = m["speed_p90_max"] < E_CORE_NS
        m["p_only"] = 1.0 if on_p and m["taker_late_p99"] < STALL_NS else 0.0
        for thread, s in p["speed"].items():
            if s["ns_per_1000"]["count"]:
                m[f"speed_{thread}_p50"] = s["ns_per_1000"]["p50"]
    return m


def summarise(ms):
    out = {}
    for m in (ms[0].keys() if ms else []):
        xs = [x[m] for x in ms if m in x]
        if xs:
            out[m] = {"median": med(xs), "min": min(xs), "max": max(xs)}
    return out


def main():
    out = Path(sys.argv[1])
    rows = [json.loads(line) for line in (out / "raw.jsonl").read_text().splitlines() if line.strip()]
    config = json.loads((out / "config.json").read_text())

    by_cell = defaultdict(list)
    for row in rows:
        by_cell[key(row)].append(row)
    order = []  # keep config order for tables
    for c in config["cells"]:
        k = key(c)
        if k in by_cell and k not in order:
            order.append(k)

    summary, p_only = {}, {}
    for k in order:
        rs = by_cell[k]
        valid = [r for r in rs if r["report"] and r["report"]["valid"]]
        ms = [session_metrics(r) for r in valid]
        cell = {"sessions": len(rs), "valid": len(valid),
                "invalid_reasons": sorted({why for r in rs if r["report"] for why in r["report"]["invalid"]}
                                          | {f"exit {r['exit_code']}" for r in rs if not r["report"]})}
        cell.update(summarise(ms))
        summary[label(k)] = cell
        clean = [m for m in ms if m.get("p_only") == 1.0]
        p_only[label(k)] = {"sessions": len(clean), **summarise(clean)}

    by_round = defaultdict(dict)
    for row in rows:
        if row["report"] and row["report"]["valid"]:
            by_round[row["round"]][key(row)] = session_metrics(row)

    def paired(a, b, metric):
        ratios, wins = [], 0
        for cells in by_round.values():
            if a in cells and b in cells and cells[a][metric] > 0:
                ratios.append(cells[b][metric] / cells[a][metric])
                wins += cells[b][metric] < cells[a][metric]
        return {"rounds": len(ratios), "median_ratio": med(ratios), "b_better_rounds": wins}

    comparisons = []

    def compare(a, b, metrics):
        if a in by_cell and b in by_cell:
            for metric in metrics:
                comparisons.append({"a": label(a), "b": label(b), "metric": metric, **paired(a, b, metric)})

    for k in order:
        family, topo, queue, burner, work, pattern, wait, hk, cost = k
        if family == "datapath" and topo != "single":
            compare(("datapath", "single", "none", False, work, pattern, wait, "none", None), k,
                    ["e2o_p50", "e2o_p99", "e2o_p999", "ack_p50", "ack_p99"])
        if family == "handoff" and not (topo == "single" and not burner):
            base = ("handoff", "single", "none", False, work, pattern, wait, "none", None)
            compare(base, k, ["e2o_p50", "e2o_p99", "e2o_p999", "ack_p50", "ack_p99"])
            if topo != "single":
                compare(("handoff", "single", "none", True, work, pattern, wait, "none", None), k,
                        ["e2o_p50", "e2o_p99", "e2o_p999"])
    for pattern in ["smooth", "burst"]:
        none = ("datapath", "single", "none", False, 0, pattern, "spin", "none", None)
        for cost in ["light", "heavy"]:
            inline = ("housekeeping", "single", "none", False, 0, pattern, "spin", "inline", cost)
            thread = ("housekeeping", "single", "none", False, 0, pattern, "spin", "thread", cost)
            for a, b in [(none, inline), (inline, thread), (none, thread)]:
                compare(a, b, ["e2o_p50", "e2o_p99", "e2o_p999", "e2o_max"])

    (out / "summary.json").write_text(json.dumps({"cells": summary, "p_only": p_only, "paired": comparisons,
                                                  "e_core_ns": E_CORE_NS, "stall_ns": STALL_NS}, indent=2))

    # ---------------------------------------------------------------- markdown
    lines = [f"# Strategy threads: {out.name}", "",
             f"{config['repeats']} rounds x {len(order)} cells, {config['steps']} trades per session, "
             f"{config['cpu']} ({config['perf_cores']}P+{config['efficiency_cores']}E). "
             "Latency cells: median of session percentiles in µs, [min-max] across valid sessions.", ""]

    def fmt(cell, m, scale=us, digits=1):
        c = cell.get(m)
        if not c or c["median"] is None:
            return "—"
        return f"{scale(c['median']):.{digits}f} [{scale(c['min']):.{digits}f}–{scale(c['max']):.{digits}f}]"

    def plain(cell, m, digits=2):
        c = cell.get(m)
        return "—" if not c else f"{c['median']:.{digits}f}"

    handoff = [k for k in order if k[0] == "handoff"]
    if handoff:
        lines += ["## Handoff", "",
                  "| arrangement | valid | event→order p50 | p99 | p99.9 | handoff p50 | order→ack p50 | ack p99 | "
                  "probe CPU s | worker CPU s | wakes | parks | clean sessions | clean e2o p50 | clean p99 |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for work in sorted({k[4] for k in handoff}):
            for pattern in ["smooth", "burst"]:
                for k in [k for k in handoff if k[4] == work and k[5] == pattern]:
                    c, po = summary[label(k)], p_only[label(k)]
                    lines.append(
                        f"| {label(k)} | {c['valid']}/{c['sessions']} | {fmt(c, 'e2o_p50')} | {fmt(c, 'e2o_p99')} | "
                        f"{fmt(c, 'e2o_p999')} | {fmt(c, 'handoff_p50')} | {fmt(c, 'ack_p50')} | {fmt(c, 'ack_p99')} | "
                        f"{plain(c, 'probe_cpu_s')} | {plain(c, 'worker_cpu_s')} | {plain(c, 'wakes', 0)} | "
                        f"{plain(c, 'parks', 0)} | {po['sessions']}/{c['valid']} | {fmt(po, 'e2o_p50')} | "
                        f"{fmt(po, 'e2o_p99')} |")
        lines += ["", "Core speed (ns per 1000 iterations, median across sessions; ~1600 P core, ~2400 E core) "
                  "; worst probe thread's p90 sample (median, max across sessions; E core above ~2300):", "",
                  "| arrangement | reader/single | worker | burner | worst p90 (median, max) | taker CPU s |",
                  "|---|---:|---:|---:|---:|---:|"]
        for k in handoff:
            c = summary[label(k)]
            w = c.get("speed_p90_max")
            slow_s = "—" if not w else f"{w['median']:.0f}, {w['max']:.0f}"
            lines.append(f"| {label(k)} | {plain(c, 'speed_reader_p50', 0)} | {plain(c, 'speed_worker_p50', 0)} | "
                         f"{plain(c, 'speed_burner_p50', 0)} | {slow_s} | {plain(c, 'taker_cpu_s')} |")
        lines.append("")

    datapath = [k for k in order if k[0] == "datapath"]
    if datapath:
        lines += ["## Data path", "",
                  "| arrangement | valid | event→order p50 | p99 | p99.9 | handoff p50 | order→ack p50 | ack p99 | probe cores |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for k in datapath:
            c = summary[label(k)]
            lines.append(f"| {label(k)} | {c['valid']}/{c['sessions']} | {fmt(c, 'e2o_p50')} | "
                         f"{fmt(c, 'e2o_p99')} | {fmt(c, 'e2o_p999')} | {fmt(c, 'handoff_p50')} | "
                         f"{fmt(c, 'ack_p50')} | {fmt(c, 'ack_p99')} | {plain(c, 'probe_cores')} |")
        lines.append("")

    hk_keys = [k for k in order if k[0] == "housekeeping" or (k[0] == "datapath" and any(
        h[0] == "housekeeping" for h in order) and k[1] == "single" and k[4] == 0 and k[6] == "spin")]
    if any(k[0] == "housekeeping" for k in hk_keys):
        lines += ["## Housekeeping", "",
                  "| arrangement | valid | event→order p50 | p99 | p99.9 | max | probe cores | hk thread cpu s |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for k in sorted(hk_keys, key=lambda k: (k[5], k[0] != "datapath", str(k[8]), k[7])):
            c = summary[label(k)]
            lines.append(f"| {label(k)} | {c['valid']}/{c['sessions']} | {fmt(c, 'e2o_p50')} | {fmt(c, 'e2o_p99')} | "
                         f"{fmt(c, 'e2o_p999')} | {fmt(c, 'e2o_max')} | {plain(c, 'probe_cores')} | "
                         f"{plain(c, 'hk_cpu_s')} |")
        lines.append("")

    lines += ["## Paired within-round ratios (b / a; < 1 means b is faster)", "",
              "| a | b | metric | rounds | median ratio | rounds b faster |", "|---|---|---|---:|---:|---:|"]
    for c in comparisons:
        r = "—" if c["median_ratio"] is None else f"{c['median_ratio']:.2f}"
        lines.append(f"| {c['a']} | {c['b']} | {c['metric']} | {c['rounds']} | {r} | {c['b_better_rounds']} |")
    invalid = {lab: c["invalid_reasons"] for lab, c in summary.items() if c["valid"] < c["sessions"]}
    lines += ["", "## Invalid sessions", ""]
    lines += [f"- {lab}: {', '.join(reasons)}" for lab, reasons in invalid.items()] or ["None."]
    (out / "report.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
