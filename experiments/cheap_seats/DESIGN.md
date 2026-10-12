# Experiment 9 — Cheap Seats

Does the model answering the brief show up in the PnL?

Every prior experiment varied what the agent was *told* (info), who it was told it *was*
(persona), or what it was allowed to *write* (params vs code). This one holds all of that
fixed and varies only one thing: **which model is on the other end of the brief.**

## The three fields

Identical config in all three: `persona=neutral`, `mode=code`, `info=rivals`, 5 generations,
6 seeds of 2 s, `parallel=1`, default background (`mm,noise,noise,noise`).

| field | agents | question |
|---|---|---|
| `results/exp9_field_a` | 4 × haiku | what does a market made entirely of cheap models converge to? |
| `results/exp9_field_b` | 4 × sonnet | the reference field, same market, better writers |
| `results/exp9_field_c` | 2 × haiku + 2 × sonnet | put them in the same book — does the stronger model take money off the weaker one? |

## Blinding

All three fields use the same four agent names — `wren`, `lark`, `finch`, `teal` — so nothing
in a rival's spec path (`.../exp9_field_c/gen_2/lark/lib/strategy.so`) reveals which model wrote
it. With `info=rivals` every agent sees every rival's spec and PnL, and in field C it cannot
tell which two rivals are the cheap ones.

**Field C mapping (not visible to any agent):**

| agent | model |
|---|---|
| wren | haiku |
| lark | sonnet |
| finch | haiku |
| teal | sonnet |

The `model` key in `config.json` is a label only — `session.py` never puts it in a brief. The
dispatcher spawns the matching subagent model for each agent each generation.

## The forecast instrument

`experiments/cheap_seats/forecast_block.md` is appended verbatim to every brief (new
`session.py --brief-extra` flag). Each agent must emit one line in its header comment:

```
// FORECAST pnl=<number> rank=<integer> confidence=<0..1>
```

It is recorded and never fed back into scoring. This turns calibration into a measurable
quantity: predicted PnL vs realised PnL, predicted rank vs realised rank, and whether
confidence moves in the right direction after a bad generation. The hypothesis worth testing is
not "the cheap model trades worse" but "the cheap model *knows* less about how it is doing."

## The analyst phase (after the runs)

Field C's trajectory, with names left blind and code withheld, goes to a panel of analyst
agents — some haiku, some sonnet. Each is asked which two of the four were the cheap model and
why. A model that cannot beat a stronger one at trading might still be able to recognise one
from its trading record; or it might not. Either result is worth having.
