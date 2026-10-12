# Recorded experiments

Kept because each run has evaluated generations, a trajectory and raw per-seed
reports. This selection is based on available evidence, not profitability.
Failed evaluated submissions are retained, including their fallback results.
Historical outputs are unchanged; the cleanup does not constitute a new run.

| Run | Question / role | Evaluated generations | Subject rows | Failed submissions |
|---|---|---:|---:|---:|
| [exp1_info](../results/exp1_info/trajectory.jsonl) | Information available to an agent | 8 | 24 | 0 |
| [exp2_persona](../results/exp2_persona/trajectory.jsonl) | Strategy personas | 8 | 24 | 0 |
| [exp3_code](../results/exp3_code/trajectory.jsonl) | AI-written C++ strategies | 5 | 10 | 0 |
| [exp4_model](../results/exp4_model/trajectory.jsonl) | Model comparison | 8 | 24 | 0 |
| [exp5_freecode](../results/exp5_freecode/trajectory.jsonl) | Free-form strategy code | 5 | 20 | 1 |
| [exp6_latency](../results/exp6_latency/trajectory.jsonl) | Strategy latency pilot (two generations) | 2 | 8 | 0 |
| [exp7_hft](../results/exp7_hft/trajectory.jsonl) | HFT agents with custom execution loops | 3 | 12 | 0 |
| [exp8_hft_scratch](../results/exp8_hft_scratch/trajectory.jsonl) | HFT agents starting from scratch | 3 | 12 | 0 |
| [exp9_field_a](../results/exp9_field_a/trajectory.jsonl) | Cheap Seats: field A | 5 | 20 | 0 |
| [exp9_field_b](../results/exp9_field_b/trajectory.jsonl) | Cheap Seats: field B | 5 | 20 | 0 |
| [exp9_field_c](../results/exp9_field_c/trajectory.jsonl) | Cheap Seats: field C | 5 | 20 | 1 |
| [ctrl_bandit](../results/ctrl_bandit/trajectory.jsonl) | Bandit control | 8 | 24 | 0 |
| [ctrl_hillclimb](../results/ctrl_hillclimb/trajectory.jsonl) | Hill-climbing control | 8 | 24 | 0 |
| [ctrl_imitate](../results/ctrl_imitate/trajectory.jsonl) | Imitation control | 8 | 24 | 0 |

Each run directory contains `config.json`, `gen_<n>.json` and
`trajectory.jsonl`. Code-mode runs also retain briefs and generated C++ sources.
Existing plugin binaries are historical artifacts; rebuild from source against
the current headers before making a new comparison. Do not overwrite old scores.

## Presentation material

- [Adaptive traders report](report.html): experiments 1–6 and controls.
- [Generated strategy gallery](strategies.html): inspect actual agent-written code.
- [Latency report](latency.html): recorded strategy latency comparisons.
- [Cheap Seats design](../experiments/cheap_seats/DESIGN.md), [calibration results](../results/exp9_report.json) and [analyst assessments](../results/exp9_analysts/).

## Supporting performance experiments

The systems, architecture, infrastructure and strategy-thread benchmarks also
have recorded results, so their source and result folders remain available.
They support implementation discussions; their measurements are separate from
the AI strategy PnL experiments. See each README under `experiments/`.

## Reading the evidence

Compare multiple seeds, the reported spread and hit rate, and the non-model
controls. Two or three generations are short pilots, not strong evidence of
learning. Concurrent runs distort latency; use interleaved serial comparisons
for latency claims. Synthetic-market PnL is not live-market performance.

## Archived material

Mock-code runs, preliminary `loop-*` demos, loose smoke-test CSVs and unrelated
personal notes remain in the sibling `CPP--Multithreaded-Market-Engine-archive-2026-10-05`
folder. The archive also preserves the original research tree. The active
repository keeps the runners needed to reproduce controls and future experiments.

`results/` remains ignored by Git as before; this index describes the local
evidence accompanying the project. Include those result folders when sharing
the presentation materials outside this checkout.
