---
name: market-run
description: Run a generational market experiment in which fresh Claude subagents write C++ trading strategies that are compiled into plugins and traded against each other. Use when asked to run N agents with given personas or strategies for G generations, to start a new experiment under results/, or to resume an existing experiment directory.
---

# market-run

Orchestrates `agents/session.py`. Each generation: briefs are written, one **fresh** `trader`
subagent per agent reads its brief and writes one file, that file is compiled into a `.so`
plugin, and every agent trades together in the same market over several seeds.

You are the **dispatcher**, not a participant. You never propose strategies.

## Translate the request into this vocabulary

| Field | Values |
|---|---|
| `persona` | `neutral`, `cautious`, `aggressive`, `market_maker`, `trend`, `hft` |
| `mode` | `code` (agent writes C++ → compiled plugin), `params` (agent picks from the catalogue) |
| `info` | `own` (its own PnL only), `market` (+ market stats), `rivals` (+ every rival's spec and PnL) |
| `start` | `mm`, `momentum`, `meanrev` — the fallback strategy before anything works |

"Two market makers and two momentum agents" → two subjects with `persona=market_maker`
and two with `persona=trend`, or in `params` mode `start=mm` / `start=momentum`.
Ask only if the request is genuinely ambiguous; otherwise pick the obvious mapping and say what you chose.

## Step 0 — preconditions

`build/market_engine` must exist. If it does not, run `make`.

## Step 1 — init

```bash
python3 agents/session.py init --out results/<name> \
  --subject a1:persona=hft,mode=code,info=rivals \
  --subject a2:persona=market_maker,mode=code,info=rivals \
  --generations 6 --seeds 6 --seconds 2
```

- One `--subject` per agent; names must be unique.
- Keys left off a subject fall back to the run-wide `--info` / `--persona` / `--mode` / `--start` flags.
- `--background` defaults to `mm,noise,noise,noise` (the fixed market the agents trade against).
- `--force` **wipes** the output directory. Never pass it without asking first.

## Step 2 — the generation loop

Repeat until `status` reports the experiment finished:

**a.** `python3 agents/session.py status --out results/<name>`
Prints, per agent, its brief path and the file it owes.

**b.** Spawn every agent for this generation **in one message**, as parallel `Agent` calls with
`subagent_type: "trader"`. Each prompt contains the absolute brief path and nothing else:

> Read the brief at `<abs path>` and follow it exactly. Write only the one output file it names.

**c.** `python3 agents/session.py evaluate --out results/<name>`
Compiles, smoke-tests, runs all agents together over the generation's seeds, appends
`trajectory.jsonl`, and writes the next generation's briefs.

### Rules that protect the experiment

- **One fresh subagent per agent per generation.** Never reuse a subagent across generations and
  never pass one any context beyond its brief path. The brief is the only channel between
  generations — that isolation is the experiment's central control.
- **Never write `proposal.json` or `strategy.cpp` yourself**, and never hint at what an agent
  should choose. If a trader fails to produce its file, let it fail.
- **Wait for every trader before `evaluate`.** A missing or broken file is already handled —
  that agent falls back to its last working plugin and the error text is fed into its next
  brief — but blocking keeps generations comparable.
- **Do not read the agents' `strategy.cpp` before `evaluate`.** Report on them afterwards.
- Compile failures are expected and interesting. Surface them; do not fix them.

## Step 3 — report

```bash
python3 scripts/trajectory.py results/<name>
```

Summarise for the user: each agent's score trajectory across generations, any compile or
smoke-test failures, strategy switches, and anything notable in how the agents interacted
(crowding at the touch, oscillation, one agent dominating).

## Mechanics worth knowing

- Generated `.so` files land in `results/<name>/gen_<k>/<agent>/lib/`.
- Each generation launches a **new** engine process per seed; plugins are passed as
  `--plugin <abs path>.so` on the command line and `dlopen`ed at startup. Nothing is hot-reloaded.
- No API keys are involved. The subagents *are* the model calls; the engine never calls a model.
- Agent-written C++ is compiled and executed on this machine with the user's privileges.
  Treat `results/` as untrusted output.
