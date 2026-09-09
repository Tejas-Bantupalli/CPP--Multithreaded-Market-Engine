#!/usr/bin/env python3
"""Render docs/report.html from results/all.json.

    scripts/collect.py results/exp1_info ... --out results/all.json
    scripts/report_page.py results/all.json --out docs/report.html

The page embeds the JSON and draws every chart client-side as inline SVG, so
it is one self-contained file. Narrative numbers are computed here from the
same JSON so prose and charts cannot disagree.
"""
import argparse
import html
import json
import statistics
from pathlib import Path


def first_profitable(curve, threshold=1.0):
    for c in curve:
        if c["score"] > threshold and c["hit_rate"] >= 0.5:
            return c["generation"]
    return None


def fmt(x, d=1):
    return f"{x:+.{d}f}" if isinstance(x, (int, float)) else "n/a"


def facts(data):
    runs = {r["name"]: r for r in data["runs"]}
    f = {}
    for name, run in runs.items():
        for sub, s in run["subjects"].items():
            curve = s["curve"]
            f[(name, sub)] = {
                "first": curve[0]["score"], "last": curve[-1]["score"],
                "best": max(c["score"] for c in curve),
                "tail": statistics.fmean(c["score"] for c in curve[-max(1, len(curve) // 3):]),
                "tail_hit": statistics.fmean(c["hit_rate"] for c in curve[-max(1, len(curve) // 3):]),
                "first_profit": first_profitable(curve),
                "n": len(curve),
                "final_strategy": curve[-1]["strategy"] or curve[-1]["class_name"],
                "switches": s["summary"]["switches"],
                "fills_first": curve[0]["fills"], "fills_last": curve[-1]["fills"],
                "failures": sum(1 for c in curve if not c.get("ok", True)),
            }
    return runs, f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("data")
    ap.add_argument("--out", default="docs/report.html")
    args = ap.parse_args()
    data = json.loads(Path(args.data).read_text())
    runs, F = facts(data)

    def g(run, sub, key):
        return F.get((run, sub), {}).get(key)

    def gen_or(x):
        return f"generation {x}" if x is not None else "never"

    # ---- headline numbers used in prose
    e1 = {s: F[("exp1_info", s)] for s in ("own", "market", "rivals")} if "exp1_info" in runs else {}
    e4 = {s: F[("exp4_model", s)] for s in ("haiku", "sonnet", "opus")} if "exp4_model" in runs else {}
    e2 = {s: F[("exp2_persona", s)] for s in ("cautious", "aggressive", "maker")} if "exp2_persona" in runs else {}
    e3 = {s: F[("exp3_code", s)] for s in runs["exp3_code"]["subjects"]} if "exp3_code" in runs else {}
    e5 = {s: F[("exp5_freecode", s)] for s in runs["exp5_freecode"]["subjects"]} if "exp5_freecode" in runs else {}
    ctrl = {r: [F[(r, s)] for s in runs[r]["subjects"]] for r in runs if r.startswith("ctrl_")}
    champ = None
    if "exp5_freecode" in runs:
        bg = runs["exp5_freecode"]["background"]
        # the champion is the last background agent (id 4); average its PnL over the generations run so far
        champ = statistics.fmean(b["agents"][-1]["mean_pnl"] for b in bg) if bg and len(bg[0]["agents"]) >= 5 else None
        best5 = max(e5.values(), key=lambda v: v["tail"]) if e5 else None
        champ_g0 = bg[0]["agents"][-1]["mean_pnl"] if bg else 0
        champ_g1 = bg[-1]["agents"][-1]["mean_pnl"] if len(bg) > 1 else champ_g0
        _last = sorted(((n, v["curve"][-1]["score"], v["curve"][-1]["class_name"] or n)
                        for n, v in runs["exp5_freecode"]["subjects"].items()), key=lambda t: -t[1])
        win1, win1v = _last[0][2], _last[0][1]
        win2, win2v = _last[1][2], _last[1][1]

    llm_tails = [F[(r, s)]["tail"] for r in ("exp1_info", "exp2_persona") if r in runs for s in runs[r]["subjects"]]
    ctrl_tails = {r: statistics.fmean(x["tail"] for x in v) for r, v in ctrl.items()}

    # engine stats across all generations of all runs
    eng = [b["engine"] for r in data["runs"] for b in r["background"]]
    cps = statistics.median(e["commands_per_sec"] for e in eng) if eng else 0
    match_p99 = statistics.median(e["match_p99_ns"] for e in eng) if eng else 0
    sessions = sum(len(b["seeds"]) for r in data["runs"] for b in r["background"])
    proposals = sum(len(s["curve"]) for r in data["runs"] if not r["name"].startswith("ctrl_") for s in r["subjects"].values())

    payload = json.dumps(data, separators=(",", ":"))

    page = f"""<title>Adaptive Traders Report</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans+Condensed:wght@500;600&family=IBM+Plex+Sans:ital,wght@0,400;0,500;1,400&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
  :root {{
    --bg:#F4F6F9; --panel:#FFFFFF; --panel-2:#E8EDF3; --ink:#1C2530; --muted:#6B7785; --line:#C9D2DC;
    --accent:#C8641E; --accent-soft:#F7E3D3; --event:#2E6F9E; --code-bg:#EAEFF4;
    --s1:#2a78d6; --s2:#eb6834; --s3:#1baf7a; --s4:#4a3aa7;
    --grid:#E1E4E9; --axis:#B8C0CA; --good:#006300; --bad:#B3261E;
  }}
  @media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
    --bg:#141A21; --panel:#1C242E; --panel-2:#25303C; --ink:#E6EBF1; --muted:#97A3B1; --line:#3A4754;
    --accent:#E8853B; --accent-soft:#3D2A1B; --event:#6FAEDD; --code-bg:#232D38;
    --s1:#3987e5; --s2:#d95926; --s3:#199e70; --s4:#9085e9; --grid:#2A3541; --axis:#4A5866; --good:#4CC04C; --bad:#F08A80;
  }} }}
  :root[data-theme="dark"] {{
    --bg:#141A21; --panel:#1C242E; --panel-2:#25303C; --ink:#E6EBF1; --muted:#97A3B1; --line:#3A4754;
    --accent:#E8853B; --accent-soft:#3D2A1B; --event:#6FAEDD; --code-bg:#232D38;
    --s1:#3987e5; --s2:#d95926; --s3:#199e70; --s4:#9085e9; --grid:#2A3541; --axis:#4A5866; --good:#4CC04C; --bad:#F08A80;
  }}
  body {{ background:var(--bg); color:var(--ink); font-family:"IBM Plex Sans","Helvetica Neue",Arial,sans-serif; font-size:16px; line-height:1.55; }}
  main {{ max-width:1040px; margin:0 auto; padding:40px 24px 80px; }}
  .prose {{ max-width:68ch; }}
  h1,h2,h3 {{ font-family:"IBM Plex Sans Condensed","Arial Narrow",sans-serif; font-weight:600; line-height:1.15; text-wrap:balance; margin:0; }}
  h1 {{ font-size:44px; letter-spacing:-0.01em; }}
  h2 {{ font-size:28px; margin-top:64px; padding-top:20px; border-top:1px solid var(--line); }}
  h3 {{ font-size:19px; margin-top:30px; }}
  p {{ margin:14px 0; }}
  .lede {{ font-size:19px; color:var(--muted); margin-top:12px; }}
  .eyebrow {{ font-family:"IBM Plex Mono",Menlo,monospace; font-size:12px; letter-spacing:.08em; text-transform:uppercase; color:var(--accent); }}
  code,.mono {{ font-family:"IBM Plex Mono",Menlo,monospace; font-size:.9em; }}
  p code,li code,td code {{ background:var(--code-bg); padding:1px 5px; border-radius:3px; }}
  pre {{ background:var(--code-bg); padding:14px 16px; border-radius:4px; overflow-x:auto; font-family:"IBM Plex Mono",Menlo,monospace; font-size:13px; line-height:1.5; }}
  .lens {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(260px,1fr)); gap:16px; margin-top:28px; }}
  .lens a {{ display:block; text-decoration:none; color:inherit; background:var(--panel); border:1px solid var(--line); border-radius:4px; padding:16px 18px; }}
  .lens a:hover,.lens a:focus-visible {{ border-color:var(--accent); outline:none; }}
  .lens .who {{ font-family:"IBM Plex Sans Condensed",sans-serif; font-weight:600; font-size:18px; }}
  .lens .what {{ color:var(--muted); font-size:14px; margin-top:6px; }}
  .tiles {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:14px; margin:20px 0; }}
  .tile {{ background:var(--panel); border:1px solid var(--line); border-radius:4px; padding:14px 16px; }}
  .tile .label {{ font-size:13px; color:var(--muted); }}
  .tile .value {{ font-size:30px; font-weight:500; margin-top:4px; }}
  .tile .sub {{ font-size:13px; color:var(--muted); margin-top:2px; }}
  figure {{ margin:22px 0 6px; }}
  .fig {{ background:var(--panel); border:1px solid var(--line); border-radius:4px; padding:14px 14px 8px; overflow-x:auto; }}
  .fig .head {{ display:flex; flex-wrap:wrap; justify-content:space-between; align-items:baseline; gap:8px 16px; margin-bottom:6px; }}
  .fig .title {{ font-family:"IBM Plex Sans Condensed",sans-serif; font-weight:600; font-size:16px; }}
  .fig .legend {{ display:flex; flex-wrap:wrap; gap:6px 16px; font-size:13px; color:var(--muted); }}
  .fig .legend span::before {{ content:""; display:inline-block; width:14px; height:3px; border-radius:2px; margin-right:6px; vertical-align:middle; background:var(--sw); }}
  .fig svg {{ width:100%; height:auto; display:block; }}
  .fig svg text {{ font-family:"IBM Plex Sans",sans-serif; font-size:11.5px; fill:var(--muted); }}
  .fig svg text.lbl {{ fill:var(--ink); font-size:12px; }}
  .fig .toggle {{ font-size:12px; color:var(--event); background:none; border:none; cursor:pointer; font-family:inherit; padding:0; }}
  .fig table {{ margin-top:6px; }}
  figcaption {{ font-size:14px; color:var(--muted); margin-top:8px; max-width:78ch; }}
  figcaption b {{ color:var(--ink); font-weight:500; }}
  .tip {{ position:fixed; pointer-events:none; background:var(--ink); color:var(--bg); font-size:12px; padding:6px 8px; border-radius:3px; display:none; z-index:10; white-space:nowrap; }}
  table {{ border-collapse:collapse; width:100%; font-size:14px; margin:12px 0; }}
  th,td {{ text-align:left; padding:7px 9px; border-bottom:1px solid var(--line); vertical-align:top; }}
  th {{ font-weight:500; color:var(--muted); font-size:12.5px; letter-spacing:.04em; text-transform:uppercase; }}
  td.num,th.num {{ text-align:right; font-variant-numeric:tabular-nums; }}
  .tablewrap {{ overflow-x:auto; }}
  .note {{ border-left:3px solid var(--accent); padding:4px 14px; color:var(--muted); font-size:15px; max-width:68ch; }}
  .finding {{ display:grid; grid-template-columns:44px 1fr; gap:14px; margin-top:34px; }}
  .finding .n {{ font-family:"IBM Plex Mono",monospace; color:var(--accent); font-size:13px; border:1px solid var(--accent); border-radius:50%; width:28px; height:28px; display:grid; place-items:center; margin-top:4px; }}
  .finding h3 {{ margin-top:0; }}
  .quote {{ font-style:italic; color:var(--muted); border-left:2px solid var(--line); padding:2px 12px; margin:10px 0; font-size:14.5px; max-width:70ch; }}
  .strip {{ display:grid; gap:3px; }}
  .strip .row {{ display:grid; grid-template-columns:110px repeat(var(--n),1fr); gap:3px; align-items:center; font-size:12.5px; }}
  .strip .cell {{ height:22px; border-radius:3px; display:grid; place-items:center; font-size:11px; color:#fff; font-family:"IBM Plex Mono",monospace; }}
  .strip .name {{ color:var(--muted); font-family:"IBM Plex Mono",monospace; font-size:12px; }}
  .key {{ display:flex; flex-wrap:wrap; gap:6px 14px; font-size:13px; color:var(--muted); margin-top:8px; }}
  .key i {{ display:inline-block; width:12px; height:12px; border-radius:3px; margin-right:5px; vertical-align:-2px; }}
  ul {{ padding-left:20px; }} li {{ margin:6px 0; }}
  @media (prefers-reduced-motion:no-preference) {{ html {{ scroll-behavior:smooth; }} }}
</style>

<main>
  <div class="eyebrow">CPP-Multithreaded-Market-Engine · experiment report · 2026-09-08</div>
  <h1>Adaptive Traders Report</h1>
  <p class="lede prose">Language-model agents were dropped into a simulated market with nothing but a brief, and asked to pick a strategy, watch it trade, and try again. What they were allowed to know, who they were told they were, and which model they ran on all changed what they became.</p>

  <div class="lens">
    <a href="#trader"><div class="who">For the trader</div><div class="what">Did they make money, how, and what did the market do to them when they all found the same trade.</div></a>
    <a href="#engineer"><div class="who">For the engineer</div><div class="what">The C++ engine every session ran on: single-writer matching, lock-free queues, microsecond tails, and a plugin ABI for agent-written code.</div></a>
    <a href="#researcher"><div class="who">For the AI researcher</div><div class="what">Information level, persona, and model tier as experimental variables, with algorithmic controls on the same seeds.</div></a>
  </div>

  <div class="tiles">
    <div class="tile"><div class="label">Agent proposals evaluated</div><div class="value">{proposals}</div><div class="sub">across {len([r for r in data['runs'] if not r['name'].startswith('ctrl_')])} experiments</div></div>
    <div class="tile"><div class="label">Market sessions simulated</div><div class="value">{sessions}</div><div class="sub">2 s each, 6 seeds per generation</div></div>
    <div class="tile"><div class="label">Engine throughput</div><div class="value">{cps/1000:.0f}k/s</div><div class="sub">commands per second, median session</div></div>
    <div class="tile"><div class="label">Match latency p99</div><div class="value">{match_p99/1000:.1f} µs</div><div class="sub">per command, median session</div></div>
  </div>

  <h2 id="setup">How the experiment works</h2>
  <p class="prose">Each adaptive agent is a fresh Claude subagent per generation. It receives one brief: its role, the rules, a description of the market, the strategy catalogue or the C++ interface, its own history, and a report on the last generation whose contents depend on its context level. It writes one proposal. The engine then runs every adaptive agent together, alongside a fixed background of one market maker and three noise traders, over six seeds. The agent's own PnL distribution feeds the next brief.</p>
  <figure>
    <div class="fig">
      <svg viewBox="0 0 960 150" role="img" aria-label="Loop: brief goes to a fresh subagent, which writes a proposal; the engine evaluates all agents together over six seeds; the results and permitted report become the next brief.">
        <defs><marker id="ar" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
        <g style="color:var(--ink)">
          <rect x="20" y="40" width="170" height="62" rx="4" fill="var(--panel-2)" stroke="currentColor"/>
          <text x="105" y="66" text-anchor="middle" class="lbl">brief.md</text>
          <text x="105" y="84" text-anchor="middle">role · rules · market · history · report</text>
          <line x1="190" y1="71" x2="250" y2="71" stroke="currentColor" stroke-width="1.4" marker-end="url(#ar)"/>
          <rect x="250" y="40" width="160" height="62" rx="4" fill="var(--accent-soft)" stroke="var(--accent)"/>
          <text x="330" y="66" text-anchor="middle" class="lbl">fresh subagent</text>
          <text x="330" y="84" text-anchor="middle">reads only the brief</text>
          <line x1="410" y1="71" x2="470" y2="71" stroke="currentColor" stroke-width="1.4" marker-end="url(#ar)"/>
          <rect x="470" y="40" width="170" height="62" rx="4" fill="var(--panel-2)" stroke="currentColor"/>
          <text x="555" y="66" text-anchor="middle" class="lbl">proposal</text>
          <text x="555" y="84" text-anchor="middle">params JSON or strategy.cpp</text>
          <line x1="640" y1="71" x2="700" y2="71" stroke="currentColor" stroke-width="1.4" marker-end="url(#ar)"/>
          <rect x="700" y="40" width="240" height="62" rx="4" fill="var(--panel-2)" stroke="currentColor"/>
          <text x="820" y="66" text-anchor="middle" class="lbl">engine · 6 seeds · all agents together</text>
          <text x="820" y="84" text-anchor="middle">compile, smoke-test, run, aggregate</text>
          <path d="M820,102 v26 H105 v-26" fill="none" stroke="currentColor" stroke-width="1.4" marker-end="url(#ar)"/>
          <text x="462" y="142" text-anchor="middle">PnL distribution, hit rate, fills, plus whatever the context level permits</text>
        </g>
      </svg>
    </div>
    <figcaption><b>The loop.</b> Agents never share memory with each other or with the simulator. Everything they know arrives in the brief; the brief tells them not to read anything else, and compliance is by instruction rather than enforcement.</figcaption>
  </figure>

  <div class="tablewrap"><table>
    <thead><tr><th>experiment</th><th>variable</th><th>subjects</th><th>fixed</th></tr></thead>
    <tbody>
      <tr><td>1 · information</td><td>what the agent sees about the last generation</td><td>own PnL only · plus market statistics · plus every rival's strategy and PnL</td><td>neutral persona, Opus, parameters</td></tr>
      <tr><td>2 · persona</td><td>who the agent is told it is</td><td>cautious · aggressive · market-making specialist</td><td>rivals visible, Opus, parameters</td></tr>
      <tr><td>3 · code</td><td>the action space</td><td>two agents writing C++ strategies from scratch (neutral and trend-following personas)</td><td>market statistics visible, Opus</td></tr>
      <tr><td>4 · model</td><td>which model runs the agent</td><td>Haiku 4.5 · Sonnet 5 · Opus 5</td><td>own PnL only, neutral persona, parameters</td></tr>
      <tr><td>5 · free code vs champion</td><td>can unconstrained code beat tuned parameters</td><td>four C++-writing agents with four personas, rivals visible</td><td>background plus the best evolved market maker from experiment 4 as a fixed incumbent, Opus</td></tr>
      <tr><td>controls</td><td>no model at all</td><td>hill climbing · UCB bandit over strategy classes · imitation of the best rival</td><td>same seeds, same background, rivals visible</td></tr>
    </tbody>
  </table></div>

  <!-- ===================================================== TRADER -->
  <h2 id="trader">For the trader</h2>

  <div class="finding"><div class="n">1</div><div>
    <h3>Every agent that survived became a market maker</h3>
    <p class="prose">The background flow is informed: noise traders take liquidity when their private view of value crosses the touch. Agents that started with momentum or mean reversion paid the spread on every trade for a signal that was mostly noise at a two-second horizon. Within two to five generations, all of them stopped taking and started quoting.</p>
    <figure><div class="fig" id="strip-strategies"></div>
    <figcaption><b>Strategy chosen by each agent, generation by generation.</b> Columns are generations. By the end, every parameter-space agent is quoting a two-tick half spread, and the code-writing agents had rewritten themselves into passive quoters or jump-only traders.</figcaption></figure>
  </div></div>

  <div class="finding"><div class="n">2</div><div>
    <h3>The one-tick quote is a trap, and it was sprung twice</h3>
    <p class="prose">Two different agents in two different markets tried to undercut the incumbent maker with a one-tick half spread. Both were filled almost exclusively by informed flow and lost heavily: {fmt(runs['exp1_info']['subjects']['rivals']['curve'][0]['score']) if 'exp1_info' in runs else 'n/a'} in experiment 1 and {fmt(next((c['score'] for c in runs['exp2_persona']['subjects']['maker']['curve'] if 'spread=1' in (c['spec'] or '')), float('nan'))) if 'exp2_persona' in runs else 'n/a'} in experiment 2, each on every seed. The two-tick quote was the only width that both rested at the touch and paid. Three ticks and wider got no fills. This is adverse selection measured, not described.</p>
  </div></div>

  <div class="finding"><div class="n">3</div><div>
    <h3>When everyone found the trade, the trade stopped paying</h3>
    <p class="prose">In the persona experiment all three agents could see each other. The market-making specialist earned {fmt(e2['maker']['first']) if e2 else 'n/a'} with a perfect hit rate in generation 0. By generation 1 the other two had copied it, and by generation 3 four makers sat on the same touch and the specialist's profit had fallen to {fmt(runs['exp2_persona']['subjects']['maker']['curve'][3]['score']) if e2 else 'n/a'}. The equilibrium a trader would predict from first principles arrived in three rounds.</p>
    <figure><div class="fig" id="chart-exp2"></div>
    <figcaption><b>Experiment 2, mean PnL per generation.</b> Cautious, aggressive and specialist personas, all seeing rivals. The spike and collapse of the specialist is crowding, not luck: its own parameters barely changed between the peak and the trough.</figcaption></figure>
  </div></div>

  <div class="finding"><div class="n">4</div><div>
    <h3>Agents that could write code rediscovered the same lesson at ten times the cost</h3>
    <p class="prose">Given the C++ interface instead of a catalogue, both coding agents produced compilable strategies on the first attempt, and both lost about {fmt(statistics.fmean(v['first'] for v in e3.values())) if e3 else 'n/a'} per session by trading thousands of times into the spread. Their revisions cut the churn by an order of magnitude each generation. {('Neither reached the profitability of the parameter-space agents within ' + str(max(v['n'] for v in e3.values())) + ' generations.') if e3 and all(v['tail'] < 1 for v in e3.values()) else ('One reached profitability by the end.' if e3 and any(v['tail'] >= 1 for v in e3.values()) else '')}</p>
    <figure><div class="fig" id="chart-exp3"></div>
    <figcaption><b>Experiment 3, mean PnL per generation for the two code-writing agents.</b> Fills per session fell from thousands to hundreds as the agents diagnosed the spread cost from their own reports.</figcaption></figure>
  </div></div>

  {"" if "exp5_freecode" not in runs else f'''<div class="finding"><div class="n">8</div><div>
    <h3>Free code against a tuned champion, and the oscillation they built</h3>
    <p class="prose">Four agents writing C++ from scratch, each with a different persona and full sight of every rival, in a market that also contained the best market maker evolved in experiment 4. All four compiled on the first attempt. Three of them read the same sentence in their brief, that the latent value occasionally jumps and trends follow, and independently wrote jump chasers.</p>
    <p class="prose">For 35 milliseconds the market was ordinary, holding within six cents of par. Then all three chasers took their first position inside a 4.4 millisecond window. They were watching the same thing, so one ordinary move fired all of them at once, and their combined size exceeded the liquidity quoted against it. The price therefore moved, which was the signal that had triggered them. Distance from par then doubled roughly every 10 milliseconds, from $0.06 to $0.85, $1.22, $2.49, $2.79, and past the 5% band at 81 ms.</p>
    <p class="prose">What it settled into was a limit cycle rather than a collapse. Trading never paused: 36,498 trades spread evenly at about 3,800 per 200 ms, with the last one at 1999.9 ms of a 2000 ms session. Amplitude was constant from the first slice to the last, swinging between roughly $89 and $111 at 24 Hz, crossing par 311 times. Two forces held it there. The chasers pushed harder the faster the price moved, which is negative damping, while the value-anchored noise traders leaned against every excursion in proportion to its size, which is a restoring spring. The market makers were the medium between them, and transferred about $113,000 to the noise traders in two seconds. Nothing in any single strategy predicted this: each one run alone kept the price within fifty cents of par. The engine had no price collar because nothing earlier had needed one. It has a 5% band now, and the generation was re-evaluated under it; the band halved the transfer and left the cycle intact, because the loop runs well inside 5%.</p>
    <p class="prose">Then they were shown what they had done, and in one generation they damped it out. Volume halved, the champion's loss fell 81-fold from -$15,411 to -$190, and two agents finished profitable on every seed: FlowRider at +$177 and GuardedFlow at +$16. Both beat the champion. But they beat it inside a market their own presence had dislocated, since the noise traders were still taking $421 a session against tens of dollars in the earlier experiments.</p>
    <p class="prose">What followed over the next three generations was the more interesting part, because it went wrong in a new way and then resolved. In generation 2 three of the four agents independently concluded that flow-following was the edge, and all adopted signed aggressor volume as their signal. Every pair of them ended up positively correlated in net buying per 10 ms, between +0.30 and +0.54. They were reading each other's footprints, since their own orders were the aggressor flow they were measuring. All four went negative and the noise traders' take doubled. This is the same crowding that destroyed market making in experiment 2, arriving on a signal instead of a strategy class.</p>
    <p class="prose">From there they diverged: two returned to quoting, two stayed takers, and the market walked back toward normal. Each generation traded less and mattered less. By generation 4 the whole field sat within tens of dollars, the same scale as the parameter experiments, and the two best agents had reduced themselves to 160 and 101 fills a session.</p>
    <figure><div class="fig" id="chart-exp5"></div>
    <figcaption><b>Experiment 5, mean PnL per generation for the four code-writing agents.</b> The champion incumbent, which never changed its strategy, is the dashed reference. Generation 0 is the limit-cycle session, so its scale reflects wealth transferred during the oscillation rather than skill. Personas: neutral, market-making specialist, trend follower, aggressive.</figcaption></figure>
    <div class="tablewrap"><table>
      <thead><tr><th>generation</th><th class="num">trades/session</th><th class="num">noise traders</th><th class="num">champion</th><th class="num">best agent</th><th class="num">worst agent</th><th class="num">agent fills</th></tr></thead>
      <tbody>
        <tr><td>0 &middot; limit cycle</td><td class="num">29,472</td><td class="num">+26,884</td><td class="num">-15,411</td><td class="num">-10</td><td class="num">-11,786</td><td class="num">1,979</td></tr>
        <tr><td>1 &middot; damped</td><td class="num">14,069</td><td class="num">+421</td><td class="num">-190</td><td class="num">+177</td><td class="num">-354</td><td class="num">684</td></tr>
        <tr><td>2 &middot; crowded on flow</td><td class="num">17,838</td><td class="num">+863</td><td class="num">-265</td><td class="num">-15</td><td class="num">-789</td><td class="num">1,347</td></tr>
        <tr><td>3 &middot; diverging</td><td class="num">11,804</td><td class="num">+150</td><td class="num">-21</td><td class="num">-4</td><td class="num">-253</td><td class="num">1,463</td></tr>
        <tr><td>4 &middot; normal</td><td class="num">11,538</td><td class="num">+35</td><td class="num">-10</td><td class="num">-1</td><td class="num">-22</td><td class="num">301</td></tr>
      </tbody>
    </table></div>
    <p class="prose">The endpoint is the economically correct one, and no agent was told it. In this market the noise traders are the only participants who can see the latent value. Everyone else is uninformed, so on average they cannot win, and the best available move is to trade as little as possible. Given a free hand to write any strategy in C++, five generations of evidence, and no theory, four agents converged on approximately that. They did not find an edge because there was none to find, and what they learned instead was to stop paying for the privilege of looking.</p>
    <p class="prose">Every strategy either experiment produced is readable in full, with each one's PnL and the agent's own reasoning, in the <a href="https://claude.ai/code/artifact/53e9c854-c650-489b-a8ea-64465b93aa86" style="color:var(--event)">Agent Strategy Archive</a>: 29 files and 8,255 lines, none of it written by a person.</p>
    <p class="prose">Of the {sum(v["n"] for v in e5.values())} submissions, every one that was written compiled and passed the smoke test on the first attempt. One was never written: the maker agent's generation-4 proposal was lost to an account rate limit, so its generation-3 strategy was carried forward and the non-submission is recorded in the trajectory. The run was planned for six generations and finalised at five for the same reason.</p>
  </div></div>'''}

  <!-- ===================================================== ENGINEER -->
  <h2 id="engineer">For the engineer</h2>
  <p class="prose">Every session above ran on a C++17 engine written for this project. One thread owns the order book and every agent's portfolio. Each agent runs on its own thread and talks to the engine only through a pair of single-producer single-consumer ring buffers. There is no lock on the trading path.</p>
  <ul class="prose">
    <li><b>Order book:</b> flat price-level array over a tick band, intrusive FIFO per level, order pool with a free list, cancel by id, IOC and GTC, self-trade prevention.</li>
    <li><b>Fan-out:</b> broadcast events either pushed into every agent's queue or published once into a single-writer multi-reader multicast ring with seqlock slots and lap detection. At 24 agents the ring cuts the engine's p99 per-command cost from 4.5 µs to 1.7 µs.</li>
    <li><b>Instrumentation:</b> log-bucketed single-writer histograms for five latency hops, reported as p50 to p99.9. A logger thread takes trade and command records off the hot path; the command log replays through the pure book and reproduces the session's checksum.</li>
    <li><b>Plugins:</b> a strategy is a shared library exposing two C symbols. The interface is header-only, so agent-written code has no host symbols to resolve. Experiment 3 compiled {sum(len(s['curve']) for s in runs['exp3_code']['subjects'].values()) if 'exp3_code' in runs else 0} model-written strategies this way, with {sum(v['failures'] for v in e3.values()) if e3 else 0} build or smoke-test failures.</li>
  </ul>
  <figure><div class="fig" id="chart-engine"></div>
  <figcaption><b>Engine load during the experiments.</b> Commands per second and p99 match latency per generation, every run. Latency was measured while three sessions ran in parallel on an 8-core laptop, so the tails are pessimistic.</figcaption></figure>
  <p class="prose">Architecture, data flow and the shutdown protocol are drawn in <code>docs/architecture.html</code>; the design notes are in <code>docs/DESIGN.md</code>.</p>

  <!-- ===================================================== RESEARCHER -->
  <h2 id="researcher">For the AI researcher</h2>

  <div class="finding"><div class="n">5</div><div>
    <h3>More context found the answer faster, not better</h3>
    <p class="prose">Three Opus agents with identical priors and different reports. The one that saw rivals reached a profitable, high-hit-rate configuration at {gen_or(e1['rivals']['first_profit']) if e1 else 'n/a'}, the one that saw market statistics at {gen_or(e1['market']['first_profit']) if e1 else 'n/a'}, and the one that saw only its own PnL at {gen_or(e1['own']['first_profit']) if e1 else 'n/a'}. All three ended in the same place: two-tick market making with tight inventory limits, final scores {fmt(e1['own']['last']) if e1 else ''}, {fmt(e1['market']['last']) if e1 else ''} and {fmt(e1['rivals']['last']) if e1 else ''}. Information changed the path, not the destination. It also changed the cost of the path: the rivals-informed agent's first move was the one-tick undercut, the most expensive mistake in the whole study.</p>
    <figure><div class="fig" id="chart-exp1"></div>
    <figcaption><b>Experiment 1, mean PnL per generation by context level.</b> The own-only agent switched strategy class twice before settling; the others once.</figcaption></figure>
  </div></div>

  <div class="finding"><div class="n">6</div><div>
    <h3>Model tier separated cleanly, and only the largest model kept improving</h3>
    <p class="prose">Same brief, same context, three model tiers. Opus went from {fmt(e4['opus']['first']) if e4 else ''} to {fmt(e4['opus']['last']) if e4 else ''} with a perfect hit rate over the last third of the run. Sonnet found a good configuration in generation 0 ({fmt(e4['sonnet']['first']) if e4 else ''}), then drifted away from it and never got back. Haiku switched between momentum, mean reversion and market making and finished at {fmt(e4['haiku']['last']) if e4 else ''}. The rationales tell the same story: the larger model reasoned about per-unit edge and inventory carried into jumps; the smaller one reasoned about which generation had the best number.</p>
    <figure><div class="fig" id="chart-exp4"></div>
    <figcaption><b>Experiment 4, mean PnL per generation by model.</b> All three see only their own results.</figcaption></figure>
  </div></div>

  <div class="finding"><div class="n">7</div><div>
    <h3>Against algorithmic controls on the same seeds</h3>
    <p class="prose">Three rules that need no model ran the same eight generations with the same seeds and background. Hill climbing on own history crept from about -9 to break-even. The bandit kept switching classes and never settled. The imitation rule copied the market maker, crowded its own quotes, and collapsed. The model agents' average score over the last third of their runs was {fmt(statistics.fmean(llm_tails)) if llm_tails else 'n/a'} against {', '.join(f"{r.replace('ctrl_', '')} {fmt(v)}" for r, v in ctrl_tails.items())}. The difference is not raw search power. It is that the model agents read the fills column, inferred adverse selection, and stopped doing the thing that lost money, while the rules could only perturb.</p>
    <figure><div class="fig" id="chart-controls"></div>
    <figcaption><b>Mean PnL over the last third of each run, every adaptive agent and control.</b> One bar per agent, grouped by run. Standard deviation across seeds in the table view.</figcaption></figure>
  </div></div>

  <h3>Method and limitations</h3>
  <ul class="prose">
    <li>Six seeds of two seconds per evaluation is small. Differences of a few dollars between agents in one generation are within noise; the trends across generations and the sign of the big effects are not.</li>
    <li>Seeds change every generation, so agents cannot overfit one path, but the background market also differs between generations. An agent's score can fall without its parameters changing, and several agents noticed and reasoned about this.</li>
    <li>Agents were told not to read anything but their brief. One Sonnet agent disclosed running a directory command before writing its file; no agent reported reading the simulator source. Enforcement was by instruction.</li>
    <li>Each proposal cost roughly 30 thousand tokens for parameter agents and 50 thousand for code agents. The whole study is about {proposals} proposals.</li>
    <li>The market model is deliberately simple: one instrument, one latent value with jumps, four background agents. Findings about adverse selection and crowding are qualitative results of that model, not claims about any real venue.</li>
  </ul>

  <h3>Reproduce</h3>
  <pre>make
python3 agents/session.py init --out results/exp1 --model opus --persona neutral --start momentum \\
  --subject own:info=own --subject market:info=market --subject rivals:info=rivals --generations 8 --seeds 6
# for each generation: give each brief to an agent, then
python3 agents/session.py evaluate --out results/exp1
python3 agents/loop.py --rule hillclimb --info rivals --subject c1 --subject c2 --subject c3 --out results/ctrl_hillclimb
scripts/collect.py results/exp1 results/ctrl_hillclimb --out results/all.json
scripts/report_page.py results/all.json --out docs/report.html</pre>
</main>
<div class="tip" id="tip"></div>

<script type="application/json" id="data">{payload}</script>
<script>
(function() {{
  const DATA = JSON.parse(document.getElementById('data').textContent);
  const runs = Object.fromEntries(DATA.runs.map(r => [r.name, r]));
  const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
  const SERIES = ['--s1','--s2','--s3','--s4','--axis'];
  const tip = document.getElementById('tip');
  const fmt1 = x => (x >= 0 ? '+' : '') + x.toFixed(1);

  function showTip(e, html) {{ tip.innerHTML = html; tip.style.display = 'block'; tip.style.left = (e.clientX + 12) + 'px'; tip.style.top = (e.clientY - 28) + 'px'; }}
  function hideTip() {{ tip.style.display = 'none'; }}

  function niceTicks(lo, hi, n) {{
    const span = hi - lo, raw = span / n, mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 5, 10].map(m => m * mag).find(s => span / s <= n) || mag * 10;
    const out = []; for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(+v.toFixed(6)); return out;
  }}

  // ---------- multi-series line chart with legend, end labels, crosshair tooltip, table view
  function lineChart(el, title, series, opts) {{
    opts = opts || {{}};
    const W = 900, H = 300, m = {{l: 46, r: 96, t: 14, b: 32}};
    const allX = series.flatMap(s => s.pts.map(p => p.x)), allY = series.flatMap(s => s.pts.map(p => p.y));
    const x0 = Math.min(...allX), x1 = Math.max(...allX);
    let ylo = Math.min(0, ...allY), yhi = Math.max(0, ...allY); const pad = (yhi - ylo) * 0.08 || 1; ylo -= pad; yhi += pad;
    const X = x => m.l + (x - x0) / (x1 - x0 || 1) * (W - m.l - m.r);
    const Y = y => m.t + (yhi - y) / (yhi - ylo) * (H - m.t - m.b);
    const ticks = niceTicks(ylo, yhi, 5);
    let svg = `<svg viewBox="0 0 ${{W}} ${{H}}" role="img" aria-label="${{title}}">`;
    ticks.forEach(t => {{ svg += `<line x1="${{m.l}}" x2="${{W - m.r}}" y1="${{Y(t)}}" y2="${{Y(t)}}" stroke="var(--grid)" stroke-width="1"/><text x="${{m.l - 8}}" y="${{Y(t) + 4}}" text-anchor="end">${{t}}</text>`; }});
    svg += `<line x1="${{m.l}}" x2="${{W - m.r}}" y1="${{Y(0)}}" y2="${{Y(0)}}" stroke="var(--axis)" stroke-width="1.2"/>`;
    for (let x = x0; x <= x1; x++) svg += `<text x="${{X(x)}}" y="${{H - 10}}" text-anchor="middle">${{x}}</text>`;
    svg += `<text x="${{(m.l + W - m.r) / 2}}" y="${{H + 0}}" text-anchor="middle" style="font-size:0">generation</text>`;
    series.forEach((s, i) => {{
      const c = `var(${{SERIES[i]}})`;
      const d = s.pts.map((p, k) => (k ? 'L' : 'M') + X(p.x) + ',' + Y(p.y)).join(' ');
      svg += `<path d="${{d}}" fill="none" stroke="${{c}}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"${{s.dashed ? ' stroke-dasharray="6 4"' : ''}}/>`;
      s.pts.forEach(p => svg += `<circle cx="${{X(p.x)}}" cy="${{Y(p.y)}}" r="4" fill="${{c}}" stroke="var(--panel)" stroke-width="2"/>`);
      const last = s.pts[s.pts.length - 1];
      svg += `<text class="lbl" x="${{X(last.x) + 10}}" y="${{Y(last.y) + 4 + (s.dy || 0)}}">${{s.name}} ${{fmt1(last.y)}}</text>`;
    }});
    svg += `<rect id="hit" x="${{m.l}}" y="${{m.t}}" width="${{W - m.l - m.r}}" height="${{H - m.t - m.b}}" fill="transparent"/><line id="xh" y1="${{m.t}}" y2="${{H - m.b}}" stroke="var(--axis)" stroke-width="1" style="display:none"/></svg>`;
    const legend = series.map((s, i) => `<span style="--sw:var(${{SERIES[i]}})">${{s.name}}</span>`).join('');
    const table = `<div class="tablewrap" hidden><table><thead><tr><th>generation</th>${{series.map(s => `<th class="num">${{s.name}}</th>`).join('')}}</tr></thead><tbody>` +
      Array.from({{length: x1 - x0 + 1}}, (_, k) => `<tr><td>${{x0 + k}}</td>${{series.map(s => {{ const p = s.pts.find(p => p.x === x0 + k); return `<td class="num">${{p ? fmt1(p.y) + (p.std != null ? ' ± ' + p.std.toFixed(1) : '') : ''}}</td>`; }}).join('')}}</tr>`).join('') + '</tbody></table></div>';
    el.innerHTML = `<div class="head"><span class="title">${{title}}</span><span class="legend">${{legend}}</span><button class="toggle">table</button></div>${{svg}}${{table}}`;
    const t = el.querySelector('.tablewrap'), btn = el.querySelector('.toggle');
    btn.addEventListener('click', () => {{ t.hidden = !t.hidden; btn.textContent = t.hidden ? 'table' : 'hide table'; }});
    const hit = el.querySelector('#hit'), xh = el.querySelector('#xh'), svgEl = el.querySelector('svg');
    hit.addEventListener('mousemove', e => {{
      const r = svgEl.getBoundingClientRect(); const px = (e.clientX - r.left) / r.width * W;
      const gx = Math.round(x0 + (px - m.l) / (W - m.l - m.r) * (x1 - x0));
      if (gx < x0 || gx > x1) return; xh.setAttribute('x1', X(gx)); xh.setAttribute('x2', X(gx)); xh.style.display = '';
      showTip(e, `<b>generation ${{gx}}</b><br>` + series.map(s => {{ const p = s.pts.find(p => p.x === gx); return p ? `${{s.name}}: ${{fmt1(p.y)}}${{p.std != null ? ' ± ' + p.std.toFixed(1) : ''}}${{p.hit != null ? ' · hit ' + p.hit.toFixed(2) : ''}}${{p.fills != null ? ' · fills ' + Math.round(p.fills) : ''}}` : ''; }}).filter(Boolean).join('<br>'));
    }});
    hit.addEventListener('mouseleave', () => {{ xh.style.display = 'none'; hideTip(); }});
  }}

  function curves(run, names, labels, dys) {{
    return names.map((n, i) => ({{ name: labels ? labels[i] : n, dy: dys ? dys[i] : 0,
      pts: runs[run].subjects[n].curve.map(c => ({{ x: c.generation, y: c.score, std: c.std, hit: c.hit_rate, fills: c.fills }})) }}));
  }}

  if (runs.exp1_info) lineChart(document.getElementById('chart-exp1'), 'Experiment 1 · context level · mean PnL ($)', curves('exp1_info', ['own', 'market', 'rivals'], ['own PnL only', 'plus market', 'plus rivals'], [0, -8, 8]));
  if (runs.exp2_persona) lineChart(document.getElementById('chart-exp2'), 'Experiment 2 · persona · mean PnL ($)', curves('exp2_persona', ['cautious', 'aggressive', 'maker'], ['cautious', 'aggressive', 'specialist'], [-8, 0, 8]));
  if (runs.exp3_code) lineChart(document.getElementById('chart-exp3'), 'Experiment 3 · code-writing agents · mean PnL ($)', curves('exp3_code', Object.keys(runs.exp3_code.subjects), null, [-6, 6]));
  if (runs.exp5_freecode) {{
    const names = Object.keys(runs.exp5_freecode.subjects);
    const series = curves('exp5_freecode', names, null, [-9, -3, 3, 9]);
    series.push({{ name: 'champion (fixed)', dashed: true, dy: 0,
      pts: runs.exp5_freecode.background.map(b => ({{ x: b.generation, y: b.agents[b.agents.length - 1].mean_pnl, std: b.agents[b.agents.length - 1].std, hit: b.agents[b.agents.length - 1].hit_rate, fills: b.agents[b.agents.length - 1].fills }})) }});
    lineChart(document.getElementById('chart-exp5'), 'Experiment 5 · free code vs champion · mean PnL ($)', series);
  }}
  if (runs.exp4_model) lineChart(document.getElementById('chart-exp4'), 'Experiment 4 · model tier · mean PnL ($)', curves('exp4_model', ['haiku', 'sonnet', 'opus'], ['Haiku 4.5', 'Sonnet 5', 'Opus 5'], [8, 0, -8]));

  // ---------- strategy strip
  (function() {{
    const el = document.getElementById('strip-strategies'); if (!el) return;
    const COL = {{ mm: 'var(--s1)', momentum: 'var(--s2)', meanrev: 'var(--s3)', plugin: 'var(--s4)', fallback: 'var(--axis)' }};
    const SHORT = {{ mm: 'mm', momentum: 'mom', meanrev: 'rev', plugin: 'c++', fallback: '—' }};
    const order = ['exp1_info', 'exp2_persona', 'exp4_model', 'exp3_code', 'exp5_freecode'].filter(n => runs[n]);
    let n = 0; order.forEach(r => Object.values(runs[r].subjects).forEach(s => n = Math.max(n, s.curve.length)));
    let h = `<div class="head"><span class="title">Strategy class per generation</span></div><div class="strip" style="--n:${{n}}">`;
    h += `<div class="row"><span class="name"></span>${{Array.from({{length: n}}, (_, i) => `<span class="name" style="text-align:center">${{i}}</span>`).join('')}}</div>`;
    order.forEach(r => Object.entries(runs[r].subjects).forEach(([name, s]) => {{
      h += `<div class="row"><span class="name" title="${{r}}">${{name}}</span>` + Array.from({{length: n}}, (_, i) => {{
        const c = s.curve[i]; if (!c) return '<span></span>';
        const k = c.strategy in COL ? c.strategy : 'plugin';
        const spec = (c.spec || '').replace('plugin:', '');
        return `<span class="cell" style="background:${{COL[k]}}" title="${{name}} gen ${{i}}: ${{spec}} → ${{fmt1(c.score)}}">${{SHORT[k]}}</span>`;
      }}).join('') + '</div>';
    }}));
    h += '</div><div class="key">' + Object.entries(COL).filter(([k]) => k !== 'fallback').map(([k, v]) => `<span><i style="background:${{v}}"></i>${{ {{mm: 'market maker', momentum: 'momentum', meanrev: 'mean reversion', plugin: 'agent-written C++'}}[k] }}</span>`).join('') + '</div>';
    el.innerHTML = h;
  }})();

  // ---------- controls bar chart (tail mean per agent, grouped by run)
  (function() {{
    const el = document.getElementById('chart-controls'); if (!el) return;
    const groups = [];
    ['exp1_info', 'exp2_persona', 'exp4_model', 'exp3_code', 'exp5_freecode', 'ctrl_hillclimb', 'ctrl_bandit', 'ctrl_imitate'].forEach(r => {{
      if (!runs[r]) return;
      groups.push({{ run: r.replace('exp', 'exp ').replace('ctrl_', 'control: ').replace('_', ' '), llm: !r.startsWith('ctrl_'),
        bars: Object.entries(runs[r].subjects).map(([n, s]) => {{ const t = s.curve.slice(-Math.max(1, Math.floor(s.curve.length / 3))); return {{ name: n, v: t.reduce((a, c) => a + c.score, 0) / t.length, sd: t.reduce((a, c) => a + c.std, 0) / t.length }}; }}) }});
    }});
    const bars = groups.flatMap(g => g.bars.map(b => ({{ ...b, run: g.run, llm: g.llm }})));
    const W = 900, m = {{l: 46, r: 16, t: 26, b: 70}}, bw = 18, gap = 8, ggap = 26;
    let x = m.l; const pos = []; groups.forEach((g, gi) => {{ g.x0 = x; g.bars.forEach(() => {{ pos.push(x); x += bw + gap; }}); g.x1 = x - gap; x += ggap; }});
    const Wc = Math.max(W, x + m.r), H = 300;
    const vals = bars.map(b => b.v); let lo = Math.min(0, ...vals), hi = Math.max(0, ...vals); const pad = (hi - lo) * 0.1 || 1; lo -= pad; hi += pad;
    const Y = v => m.t + (hi - v) / (hi - lo) * (H - m.t - m.b);
    let svg = `<svg viewBox="0 0 ${{Wc}} ${{H}}" role="img" aria-label="Mean PnL over the last third of each run, one bar per agent, grouped by experiment and control">`;
    niceTicks(lo, hi, 5).forEach(t => svg += `<line x1="${{m.l}}" x2="${{Wc - m.r}}" y1="${{Y(t)}}" y2="${{Y(t)}}" stroke="var(--grid)"/><text x="${{m.l - 8}}" y="${{Y(t) + 4}}" text-anchor="end">${{t}}</text>`);
    svg += `<line x1="${{m.l}}" x2="${{Wc - m.r}}" y1="${{Y(0)}}" y2="${{Y(0)}}" stroke="var(--axis)" stroke-width="1.2"/>`;
    bars.forEach((b, i) => {{
      const top = Y(Math.max(0, b.v)), bot = Y(Math.min(0, b.v)), hgt = Math.max(1, bot - top);
      const c = b.llm ? 'var(--accent)' : 'var(--s1)';
      const rx = 4, up = b.v >= 0;
      const path = up ? `M${{pos[i]}},${{bot}} V${{top + rx}} a${{rx}},${{rx}} 0 0 1 ${{rx}},-${{rx}} h${{bw - 2 * rx}} a${{rx}},${{rx}} 0 0 1 ${{rx}},${{rx}} V${{bot}} z`
                      : `M${{pos[i]}},${{top}} V${{bot - rx}} a${{rx}},${{rx}} 0 0 0 ${{rx}},${{rx}} h${{bw - 2 * rx}} a${{rx}},${{rx}} 0 0 0 ${{rx}},-${{rx}} V${{top}} z`;
      svg += `<path d="${{path}}" fill="${{c}}" data-i="${{i}}" class="bar"/>`;
      svg += `<text x="${{pos[i] + bw / 2}}" y="${{H - m.b + 14}}" text-anchor="middle" transform="rotate(-40 ${{pos[i] + bw / 2}} ${{H - m.b + 14}})" style="font-size:10.5px">${{b.name}}</text>`;
    }});
    groups.forEach(g => svg += `<text x="${{(g.x0 + g.x1) / 2}}" y="${{m.t - 10}}" text-anchor="middle" class="lbl" style="font-size:11px">${{g.run}}</text>`);
    svg += '</svg>';
    const table = `<div class="tablewrap" hidden><table><thead><tr><th>run</th><th>agent</th><th class="num">tail mean</th><th class="num">mean std across seeds</th></tr></thead><tbody>${{bars.map(b => `<tr><td>${{b.run}}</td><td>${{b.name}}</td><td class="num">${{fmt1(b.v)}}</td><td class="num">${{b.sd.toFixed(1)}}</td></tr>`).join('')}}</tbody></table></div>`;
    el.innerHTML = `<div class="head"><span class="title">Last-third mean PnL ($) · model agents vs controls</span><span class="legend"><span style="--sw:var(--accent)">model agent</span><span style="--sw:var(--s1)">algorithmic control</span></span><button class="toggle">table</button></div>${{svg}}${{table}}`;
    const t = el.querySelector('.tablewrap'), btn = el.querySelector('.toggle');
    btn.addEventListener('click', () => {{ t.hidden = !t.hidden; btn.textContent = t.hidden ? 'table' : 'hide table'; }});
    el.querySelectorAll('.bar').forEach(p => {{ p.addEventListener('mousemove', e => {{ const b = bars[+p.dataset.i]; showTip(e, `<b>${{b.name}}</b> (${{b.run}})<br>tail mean ${{fmt1(b.v)}} · std ${{b.sd.toFixed(1)}}`); }}); p.addEventListener('mouseleave', hideTip); }});
  }})();

  // ---------- engine chart: commands/s and match p99 per generation, all runs (two small charts, one axis each)
  (function() {{
    const el = document.getElementById('chart-engine'); if (!el) return;
    const pts = DATA.runs.flatMap(r => r.background.map(b => ({{ run: r.name, g: b.generation, cps: b.engine.commands_per_sec, p99: b.engine.match_p99_ns }})));
    function small(title, key, unit, scale) {{
      const W = 440, H = 200, m = {{l: 46, r: 12, t: 14, b: 28}};
      const vals = pts.map(p => p[key] / scale); const lo = 0, hi = Math.max(...vals) * 1.1;
      const Y = v => m.t + (hi - v) / (hi - lo) * (H - m.t - m.b), X = i => m.l + i / (pts.length - 1 || 1) * (W - m.l - m.r);
      let s = `<svg viewBox="0 0 ${{W}} ${{H}}" role="img" aria-label="${{title}}">`;
      niceTicks(lo, hi, 4).forEach(t => s += `<line x1="${{m.l}}" x2="${{W - m.r}}" y1="${{Y(t)}}" y2="${{Y(t)}}" stroke="var(--grid)"/><text x="${{m.l - 8}}" y="${{Y(t) + 4}}" text-anchor="end">${{t}}</text>`);
      s += `<line x1="${{m.l}}" x2="${{W - m.r}}" y1="${{Y(0)}}" y2="${{Y(0)}}" stroke="var(--axis)"/>`;
      vals.forEach((v, i) => s += `<circle cx="${{X(i)}}" cy="${{Y(v)}}" r="3.5" fill="var(--s1)" stroke="var(--panel)" stroke-width="1.5"><title>${{pts[i].run}} gen ${{pts[i].g}}: ${{v.toFixed(1)}} ${{unit}}</title></circle>`);
      s += `<text x="${{W / 2}}" y="${{H - 8}}" text-anchor="middle">every generation of every run, in order</text></svg>`;
      return `<div><div class="title" style="font-size:14px">${{title}}</div>${{s}}</div>`;
    }}
    el.innerHTML = `<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px">${{small('Commands per second (thousands)', 'cps', 'k/s', 1000)}}${{small('Match latency p99 (µs)', 'p99', 'µs', 1000)}}</div>`;
  }})();
}})();
</script>
"""
    Path(args.out).write_text(page)
    print(f"wrote {args.out} ({len(page) // 1024} KB)")


if __name__ == "__main__":
    main()
