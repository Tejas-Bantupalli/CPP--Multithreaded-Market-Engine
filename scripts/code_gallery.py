#!/usr/bin/env python3
"""Render docs/strategies.html: every agent-written C++ strategy, browsable.

    scripts/code_gallery.py results/exp5_freecode results/exp3_code --out docs/strategies.html

Pulls each generation's strategy.cpp, the class name, the leading comment block
(the agent's own rationale), and that generation's PnL from trajectory.jsonl.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

RUN_LABELS = {
    "exp6_latency": ("Experiment 6", "Latency race. Venue fees, published matching rules, no rival information."),
    "exp5_freecode": ("Experiment 5", "Free code against a tuned champion. Four personas, rivals visible."),
    "exp3_code": ("Experiment 3", "Free code with market statistics only. Two personas."),
}


def rationale_of(src):
    """The leading comment block, which is where the agents put their reasoning."""
    lines = src.splitlines()
    out, started = [], False
    for ln in lines[:120]:
        st = ln.strip()
        if st.startswith("/*") or st.startswith("//") or (started and (st.startswith("*") or st.startswith("//"))):
            started = True
            cleaned = re.sub(r"^\s*(/\*+|\*/|\*|//)\s?", "", ln).rstrip()
            out.append(cleaned)
        elif started and not st:
            out.append("")
        elif started:
            break
    # drop the agents' ascii rules and any leading blank/heading noise
    keep = [ln for ln in out if not re.fullmatch(r"[=\-_*~ ]{6,}", ln.strip())]
    text = "\n".join(keep).strip()
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text


def collect(run_dir):
    run = Path(run_dir)
    name = run.name
    traj = {}
    tp = run / "trajectory.jsonl"
    if tp.exists():
        for line in tp.read_text().splitlines():
            if line.strip():
                r = json.loads(line)
                traj[(r["subject"], r["generation"])] = r
    items = []
    gens = [d for d in run.glob("gen_*") if d.is_dir()]
    for gd in sorted(gens, key=lambda p: int(p.name.split("_")[1])):
        gen = int(gd.name.split("_")[1])
        for ad in sorted(gd.iterdir()):
            f = ad / "strategy.cpp"
            if not f.is_dir() and f.exists():
                src = f.read_text()
                m = re.search(r"MARKET_PLUGIN\(\s*(\w+)\s*\)", src)
                t = traj.get((ad.name, gen), {})
                items.append({
                    "run": name, "agent": ad.name, "generation": gen,
                    "cls": m.group(1) if m else "?",
                    "lines": src.count("\n") + 1,
                    "score": t.get("score"), "hit": t.get("hit_rate"), "fills": t.get("fills"),
                    "std": t.get("std"), "ok": t.get("ok", True),
                    "rationale": rationale_of(src),
                    "src": src,
                })
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", default="docs/strategies.html")
    args = ap.parse_args()

    items = []
    for r in args.runs:
        items += collect(r)
    for i, it in enumerate(items):
        it["id"] = i
    total_lines = sum(it["lines"] for it in items)
    runs_present = [r for r in ("exp6_latency", "exp5_freecode", "exp3_code") if any(i["run"] == r for i in items)]
    payload = json.dumps({"items": items, "labels": RUN_LABELS}, separators=(",", ":"))

    page = f"""<title>Agent Strategy Archive</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans+Condensed:wght@500;600&family=IBM+Plex+Sans:wght@400;500&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
  :root {{
    --bg:#F4F6F9; --panel:#FFFFFF; --panel-2:#E8EDF3; --ink:#1C2530; --muted:#6B7785; --line:#C9D2DC;
    --accent:#C8641E; --accent-soft:#F7E3D3; --event:#2E6F9E; --code-bg:#FBFCFD;
    --good:#0F7B4F; --bad:#B3261E;
    --k:#8250DF; --s:#0A7A5E; --c:#7A8894; --n:#B4531A; --t:#2E6F9E; --f:#1C2530;
  }}
  @media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
    --bg:#141A21; --panel:#1C242E; --panel-2:#25303C; --ink:#E6EBF1; --muted:#97A3B1; --line:#3A4754;
    --accent:#E8853B; --accent-soft:#3D2A1B; --event:#6FAEDD; --code-bg:#171E26;
    --good:#4CC04C; --bad:#F08A80;
    --k:#C29BF5; --s:#5FCFA6; --c:#75818E; --n:#E8955B; --t:#6FAEDD; --f:#E6EBF1;
  }} }}
  :root[data-theme="dark"] {{
    --bg:#141A21; --panel:#1C242E; --panel-2:#25303C; --ink:#E6EBF1; --muted:#97A3B1; --line:#3A4754;
    --accent:#E8853B; --accent-soft:#3D2A1B; --event:#6FAEDD; --code-bg:#171E26;
    --good:#4CC04C; --bad:#F08A80;
    --k:#C29BF5; --s:#5FCFA6; --c:#75818E; --n:#E8955B; --t:#6FAEDD; --f:#E6EBF1;
  }}
  body {{ background:var(--bg); color:var(--ink); font-family:"IBM Plex Sans",system-ui,sans-serif; font-size:15px; line-height:1.5; }}
  .top {{ max-width:1400px; margin:0 auto; padding:28px 24px 0; }}
  .eyebrow {{ font-family:"IBM Plex Mono",monospace; font-size:11.5px; letter-spacing:.08em; text-transform:uppercase; color:var(--accent); }}
  h1 {{ font-family:"IBM Plex Sans Condensed",sans-serif; font-size:34px; font-weight:600; margin:4px 0 0; letter-spacing:-0.01em; }}
  .lede {{ color:var(--muted); max-width:68ch; margin:8px 0 0; }}
  .wrap {{ max-width:1400px; margin:0 auto; padding:20px 24px 60px; display:grid; grid-template-columns:300px minmax(0,1fr); gap:20px; align-items:start; }}
  @media (max-width:900px) {{ .wrap {{ grid-template-columns:1fr; }} .rail {{ position:static !important; max-height:none !important; }} }}
  .rail {{ position:sticky; top:16px; max-height:calc(100vh - 40px); overflow-y:auto; background:var(--panel); border:1px solid var(--line); border-radius:5px; padding:6px; }}
  .rail h2 {{ font-family:"IBM Plex Sans Condensed",sans-serif; font-size:14px; font-weight:600; margin:12px 8px 2px; }}
  .rail .sub {{ font-size:11.5px; color:var(--muted); margin:0 8px 8px; line-height:1.35; }}
  .rail h3 {{ font-family:"IBM Plex Mono",monospace; font-size:11px; text-transform:uppercase; letter-spacing:.06em; color:var(--muted); margin:12px 8px 4px; font-weight:500; }}
  .row {{ display:grid; grid-template-columns:26px 1fr auto; gap:8px; align-items:center; width:100%; text-align:left; background:none; border:0; border-radius:4px; padding:6px 8px; cursor:pointer; color:inherit; font:inherit; }}
  .row:hover {{ background:var(--panel-2); }}
  .row[aria-current="true"] {{ background:var(--accent-soft); }}
  .row .g {{ font-family:"IBM Plex Mono",monospace; font-size:11px; color:var(--muted); }}
  .row .nm {{ font-size:13px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }}
  .row .v {{ font-family:"IBM Plex Mono",monospace; font-size:11.5px; font-variant-numeric:tabular-nums; }}
  .pos {{ color:var(--good); }} .neg {{ color:var(--bad); }}
  .main {{ background:var(--panel); border:1px solid var(--line); border-radius:5px; overflow:hidden; }}
  .hd {{ padding:16px 18px; border-bottom:1px solid var(--line); }}
  .hd .cls {{ font-family:"IBM Plex Sans Condensed",sans-serif; font-size:22px; font-weight:600; }}
  .hd .meta {{ font-size:12.5px; color:var(--muted); margin-top:3px; font-family:"IBM Plex Mono",monospace; }}
  .stats {{ display:flex; flex-wrap:wrap; gap:8px 28px; margin-top:12px; }}
  .stat .l {{ font-size:11px; color:var(--muted); text-transform:uppercase; letter-spacing:.05em; }}
  .stat .n {{ font-family:"IBM Plex Mono",monospace; font-size:17px; font-variant-numeric:tabular-nums; margin-top:1px; }}
  details.why {{ border-bottom:1px solid var(--line); }}
  details.why > summary {{ cursor:pointer; padding:11px 18px; font-size:13px; color:var(--event); list-style:none; user-select:none; }}
  details.why > summary::-webkit-details-marker {{ display:none; }}
  details.why > summary::before {{ content:"▸ "; }}
  details.why[open] > summary::before {{ content:"▾ "; }}
  .why pre {{ margin:0; padding:0 18px 16px; white-space:pre-wrap; font-family:"IBM Plex Mono",monospace; font-size:12px; line-height:1.55; color:var(--muted); max-height:340px; overflow:auto; }}
  .codewrap {{ background:var(--code-bg); overflow:auto; max-height:min(72vh,900px); }}
  pre.code {{ margin:0; padding:14px 0; font-family:"IBM Plex Mono",monospace; font-size:12.5px; line-height:1.55; }}
  pre.code .ln {{ display:block; padding:0 16px 0 62px; position:relative; white-space:pre; }}
  pre.code .ln::before {{ content:attr(data-n); position:absolute; left:0; width:46px; text-align:right; color:var(--line); font-size:11px; user-select:none; }}
  pre.code .ln:hover {{ background:var(--panel-2); }}
  .kw {{ color:var(--k); }} .st {{ color:var(--s); }} .cm {{ color:var(--c); font-style:italic; }}
  .nu {{ color:var(--n); }} .ty {{ color:var(--t); }} .fn {{ color:var(--f); font-weight:500; }}
  .bar {{ display:flex; flex-wrap:wrap; gap:10px; align-items:center; padding:9px 18px; border-top:1px solid var(--line); font-size:12px; color:var(--muted); }}
  .btn {{ font:inherit; font-size:12px; background:var(--panel-2); border:1px solid var(--line); border-radius:4px; padding:4px 10px; cursor:pointer; color:inherit; }}
  .btn:hover {{ border-color:var(--accent); }}
</style>

<div class="top">
  <div class="eyebrow">CPP-Multithreaded-Market-Engine · agent-written source</div>
  <h1>Agent Strategy Archive</h1>
  <p class="lede">Every C++ trading strategy written by a language-model agent in this study. {len(items)} files, {total_lines:,} lines, each compiled as a shared library and run in the market against the others. Nothing here was written by a person.</p>
</div>

<div class="wrap">
  <nav class="rail" id="rail" aria-label="Strategies"></nav>
  <div class="main">
    <div class="hd">
      <div class="cls" id="cls">&nbsp;</div>
      <div class="meta" id="meta"></div>
      <div class="stats" id="stats"></div>
    </div>
    <details class="why" id="why"><summary>The agent's own reasoning</summary><pre id="whytext"></pre></details>
    <div class="codewrap"><pre class="code" id="code"></pre></div>
    <div class="bar"><span id="path"></span><button class="btn" id="copy">Copy source</button></div>
  </div>
</div>

<script type="application/json" id="data">{payload}</script>
<script>
(function() {{
  const D = JSON.parse(document.getElementById('data').textContent);
  const items = D.items, labels = D.labels;
  const esc = s => s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

  const KW = new Set(('alignas alignof auto break case catch class const constexpr continue default delete do else enum explicit export extern false final for friend goto if inline mutable namespace new noexcept nullptr operator override private protected public return sizeof static static_cast struct switch template this throw true try typedef typename union using virtual void volatile while reinterpret_cast dynamic_cast const_cast').split(' '));
  const TY = new Set(('bool char double float int long short signed unsigned size_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t Price Qty Ts OrderId AgentId Side Event Command Strategy AgentContext Params TimeInForce EventKind RejectReason std vector deque array string').split(' '));

  // tokenizer: comments and strings first, then words and numbers
  function hl(line) {{
    let out = '', i = 0;
    while (i < line.length) {{
      const rest = line.slice(i);
      let m;
      if ((m = rest.match(/^\\/\\/.*/))) {{ out += '<span class="cm">' + esc(m[0]) + '</span>'; i += m[0].length; continue; }}
      if ((m = rest.match(/^\\/\\*[\\s\\S]*?(\\*\\/|$)/))) {{ out += '<span class="cm">' + esc(m[0]) + '</span>'; i += m[0].length; continue; }}
      if ((m = rest.match(/^"(\\\\.|[^"\\\\])*"/)) || (m = rest.match(/^'(\\\\.|[^'\\\\])*'/))) {{ out += '<span class="st">' + esc(m[0]) + '</span>'; i += m[0].length; continue; }}
      if ((m = rest.match(/^#[a-z]+/))) {{ out += '<span class="kw">' + esc(m[0]) + '</span>'; i += m[0].length; continue; }}
      if ((m = rest.match(/^[0-9][0-9a-fA-FxX._+-]*/))) {{ out += '<span class="nu">' + esc(m[0]) + '</span>'; i += m[0].length; continue; }}
      if ((m = rest.match(/^[A-Za-z_][A-Za-z0-9_]*/))) {{
        const w = m[0];
        const cls = KW.has(w) ? 'kw' : TY.has(w) ? 'ty' : (rest[w.length] === '(' ? 'fn' : null);
        out += cls ? '<span class="' + cls + '">' + esc(w) + '</span>' : esc(w);
        i += w.length; continue;
      }}
      out += esc(rest[0]); i++;
    }}
    return out;
  }}

  function render(it) {{
    document.getElementById('cls').textContent = it.cls;
    const lab = labels[it.run] ? labels[it.run][0] : it.run;
    document.getElementById('meta').textContent = `${{lab}} · agent "${{it.agent}}" · generation ${{it.generation}} · ${{it.lines}} lines`;
    const s = document.getElementById('stats');
    const cell = (l, n, cls) => `<div class="stat"><div class="l">${{l}}</div><div class="n ${{cls||''}}">${{n}}</div></div>`;
    const sign = v => v == null ? '' : (v > 0 ? 'pos' : v < 0 ? 'neg' : '');
    s.innerHTML =
      cell('mean PnL', it.score == null ? '—' : (it.score >= 0 ? '+' : '') + it.score.toFixed(1), sign(it.score)) +
      cell('hit rate', it.hit == null ? '—' : it.hit.toFixed(2)) +
      cell('fills / session', it.fills == null ? '—' : Math.round(it.fills).toLocaleString()) +
      cell('std across seeds', it.std == null ? '—' : it.std.toFixed(1)) +
      (it.ok ? '' : cell('build', 'failed', 'neg'));
    const w = document.getElementById('why'), wt = document.getElementById('whytext');
    if (it.rationale) {{ w.style.display = ''; wt.textContent = it.rationale; }} else {{ w.style.display = 'none'; }}
    document.getElementById('code').innerHTML =
      it.src.split('\\n').map((l, i) => `<span class="ln" data-n="${{i + 1}}">${{hl(l) || ' '}}</span>`).join('');
    document.getElementById('path').textContent = `results/${{it.run}}/gen_${{it.generation}}/${{it.agent}}/strategy.cpp`;
    document.querySelectorAll('.row').forEach(r => r.setAttribute('aria-current', String(+r.dataset.id === it.id)));
    document.querySelector('.codewrap').scrollTop = 0;
  }}

  // rail: group by run, then agent
  const rail = document.getElementById('rail');
  let html = '';
  for (const run of ['exp6_latency', 'exp5_freecode', 'exp3_code']) {{
    const mine = items.filter(i => i.run === run);
    if (!mine.length) continue;
    html += `<h2>${{labels[run] ? labels[run][0] : run}}</h2><p class="sub">${{labels[run] ? labels[run][1] : ''}}</p>`;
    const agents = [...new Set(mine.map(i => i.agent))];
    for (const a of agents) {{
      html += `<h3>${{a}}</h3>`;
      for (const it of mine.filter(i => i.agent === a).sort((x, y) => x.generation - y.generation)) {{
        const v = it.score == null ? '—' : (it.score >= 0 ? '+' : '') + it.score.toFixed(0);
        const cls = it.score == null ? '' : it.score > 0 ? 'pos' : 'neg';
        html += `<button class="row" data-id="${{it.id}}"><span class="g">g${{it.generation}}</span><span class="nm">${{it.cls}}</span><span class="v ${{cls}}">${{v}}</span></button>`;
      }}
    }}
  }}
  rail.innerHTML = html;
  rail.addEventListener('click', e => {{
    const b = e.target.closest('.row');
    if (b) render(items[+b.dataset.id]);
  }});

  document.getElementById('copy').addEventListener('click', async () => {{
    const cur = document.querySelector('.row[aria-current="true"]');
    if (!cur) return;
    const btn = document.getElementById('copy');
    try {{ await navigator.clipboard.writeText(items[+cur.dataset.id].src); btn.textContent = 'Copied'; }}
    catch {{ btn.textContent = 'Select the code to copy'; }}
    setTimeout(() => {{ btn.textContent = 'Copy source'; }}, 1600);
  }});

  // open on the profitable one if there is one, else the first
  const best = items.reduce((a, b) => (b.score != null && (a == null || b.score > a.score) ? b : a), null);
  render(best || items[0]);
}})();
</script>
"""
    Path(args.out).write_text(page)
    print(f"wrote {args.out} ({len(page)//1024} KB, {len(items)} strategies, {total_lines:,} lines)")


if __name__ == "__main__":
    main()
