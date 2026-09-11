#!/usr/bin/env python3
"""How each code-writing agent's latency and stack choices changed over generations.

    scripts/latency_trajectory.py results/exp8_hft_scratch [--json out.json]

Per agent, per generation: PnL, fills, the tick-to-trade split (data path vs compute),
and what the agent's source chose for its stack: transport, whether it owns run(),
and how it waits (spin, yield, sleep, park). Source features are regex reads of the
strategy file, so treat them as a guide and confirm by reading the code.
"""
import argparse
import json
import re
from pathlib import Path


def stack_features(src):
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)  # ignore rationale comments
    m = re.search(r"TransportKind::(Spsc|MutexCv|Mutex)", code)
    return {
        "transport": m.group(1).lower() if m else "default",
        "owns_run": bool(re.search(r"\bvoid\s+run\s*\(\s*AgentContext", code)),
        "yield": "yield()" in code,
        "sleep": "sleep_for" in code or "sleep_until" in code,
        "spin_hint": bool(re.search(r"__builtin_ia32_pause|_mm_pause|cpu_relax|spin", code, re.I)),
        "wait_poll": "wait_poll" in code,
        "lines": src.count("\n"),
    }


def fmt_ns(x):
    if x is None:
        return "-"
    return f"{x/1e6:.1f}ms" if x >= 1e6 else f"{x/1e3:.1f}us" if x >= 1e4 else f"{x:.0f}ns"


def load(out):
    out = Path(out)
    cfg = json.loads((out / "config.json").read_text())
    names = [s["name"] for s in cfg["subjects"]]
    n_bg = len(cfg["background"])
    rows = []
    for g in range(cfg["generations"]):
        p = out / f"gen_{g}.json"
        if not p.exists():
            break
        rec = json.loads(p.read_text())
        for i, name in enumerate(names):
            a = rec["summary"]["agents"][n_bg + i]
            prop = rec["proposals"][name]
            src_path = out / f"gen_{g}" / name / "strategy.cpp"
            feats = stack_features(src_path.read_text()) if src_path.exists() else {}
            rows.append({
                "generation": g, "agent": name, "ok": prop["ok"], "class": prop["proposal"].get("class_name"),
                "pnl_mean": a["pnl"]["mean"], "hit": a["pnl"]["hit_rate"], "fills": a["fills_mean"],
                "maker": a.get("maker_fills_mean"), "taker": a.get("taker_fills_mean"),
                "t2t_p50": a.get("tick_to_trade_p50_ns"), "t2t_p99": a.get("tick_to_trade_p99_ns"),
                "data_p50": a.get("delivery_p50_ns"), "data_p99": a.get("delivery_p99_ns"),
                "compute_p50": a.get("react_p50_ns"), "compute_p99": a.get("react_p99_ns"),
                "orders": a.get("react_count_mean"), **feats,
            })
    return names, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--json")
    args = ap.parse_args()
    names, rows = load(args.out)
    for name in names:
        print(f"\n== {name}")
        print(f"{'gen':>3} {'ok':>2} {'class':<22} {'pnl':>10} {'hit':>4} {'fills':>7} {'mk/tk':>11} "
              f"{'t2t50':>8} {'t2t99':>8} {'data50':>8} {'data99':>8} {'cmp50':>8} {'cmp99':>8} "
              f"{'transport':<9} run wait")
        for r in (r for r in rows if r["agent"] == name):
            wait = "+".join(k for k in ("yield", "sleep", "wait_poll", "spin_hint") if r.get(k)) or "-"
            mk = f"{r['maker'] or 0:.0f}/{r['taker'] or 0:.0f}"
            print(f"{r['generation']:>3} {'y' if r['ok'] else 'N':>2} {str(r['class'])[:22]:<22} "
                  f"{r['pnl_mean']:>10.2f} {r['hit']:>4.2f} {r['fills']:>7.0f} {mk:>11} "
                  f"{fmt_ns(r['t2t_p50']):>8} {fmt_ns(r['t2t_p99']):>8} {fmt_ns(r['data_p50']):>8} "
                  f"{fmt_ns(r['data_p99']):>8} {fmt_ns(r['compute_p50']):>8} {fmt_ns(r['compute_p99']):>8} "
                  f"{r.get('transport', '-'):<9} {'own' if r.get('owns_run') else 'dflt'} {wait}")
    if args.json:
        Path(args.json).write_text(json.dumps(rows, indent=1))


if __name__ == "__main__":
    main()
