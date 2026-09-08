# Experiments

Every session is one sample. Nothing below is meaningful from a single run.

## Comparing configurations

```
scripts/sweep.py --seeds 20 --seconds 2 --parallel 3 \
  --agent mm --agent noise --agent noise --agent noise \
  --agent momentum:thr=3,slow=400 --agent meanrev:thr=4 --out results/sweep-a.json
```

Prints mean, standard deviation, median, hit rate, and a naive Sharpe per agent,
plus engine throughput and latency medians. `--parallel` above 1 distorts the
latency numbers; leave it at 1 when those matter.

## The generational loop

`agents/loop.py` runs the adaptation experiment. Each subject is an agent
controlled by a proposer. Per generation:

1. Each subject receives a report on the previous generation and proposes a
   strategy and parameters from `agents/catalogue.py`.
2. All subjects are placed in one market with the fixed background agents and
   evaluated over N seeds.
3. Results are recorded and become the next generation's report.

The experimental variables:

| flag | values | question it answers |
|---|---|---|
| `--info` | `own`, `market`, `rivals` | does more information about the market or competitors change what agents converge to? |
| `--subject name:persona` | `neutral`, `cautious`, `aggressive`, `market_maker`, `trend` | do different priors reach different equilibria from the same evidence? |
| `--proposer` | `claude`, `mock` | `mock` is a keep-the-best random perturbation, the control condition |

Example, two personas competing with full information:

```
agents/loop.py --generations 8 --seeds 10 --seconds 2 \
  --subject hawk:aggressive --subject dove:cautious --info rivals \
  --proposer claude --out results/loop-hawk-dove
```

The Claude proposer needs credentials: an `ANTHROPIC_API_KEY` in the
environment, or an `ant auth login` profile. Install the SDK with
`pip install anthropic`. The default model is `claude-opus-5`; change it with
`--model`. The mock proposer needs nothing and is the way to test the loop.

Output in `--out`:

- `trajectory.jsonl`: one line per subject per generation with the proposal,
  the score (mean PnL), spread, hit rate, and fills. This is the file to analyse.
- `gen_<k>.json`: the full sweep summary and raw per-seed reports.
- `config.json`: the arguments used.

## Agents that write code

`--proposer claude-code` replaces the catalogue with a C++ action space. Each
generation the agent returns one source file defining a `Strategy` subclass
that ends with `MARKET_PLUGIN(ClassName)`. The loop compiles it as a shared
library, runs it for 0.3 s in a small market to catch crashes and hangs, and
feeds compiler or runtime errors back for up to three attempts. The compiled
library becomes the agent for that generation. Sources and libraries land in
`<out>/code`; `--proposer mock-code` exercises the same pipeline offline by
perturbing `plugins/example_breakout.cpp`.

Generated code is compiled and executed on your machine with your privileges.
The engine gives a plugin nothing beyond the `AgentContext` interface, but the
language does not enforce that. Read the sources before reusing them and do
not run this on a machine holding anything you care about without a sandbox.

Writing a plugin by hand:

```
make plugin SRC=plugins/my_strategy.cpp
./build/market_engine --plugin build/plugins/my_strategy.so:size=3 --agent mm --agent noise
```

## Reading a trajectory

Score is mean PnL in dollars over the generation's seeds. Compare the score of
the last generation with the first, but also look at the standard deviation: an
agent that found +20 with std 5 has learned something; one at +40 with std 60
has been lucky. Hit rate below 0.5 with positive mean means a few big wins are
carrying the average.

## Ideas that fit the framework

- Hold the seeds fixed across generations to reduce evaluation noise (change
  `seeds` in `agents/loop.py`).
- Vary the market with `--market jump=0.01,jumpsize=40` to see whether agents
  re-adapt after a regime change mid-run.
- Give one subject `--info rivals` and another `--info own` in separate runs
  and compare convergence speed.
