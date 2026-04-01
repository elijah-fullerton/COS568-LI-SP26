# LI-SP26 Milestone 3 Autoresearch Program

This file adapts the `cos568-autoresearch` loop to the learned-index Milestone 3 project.

## Goal

Iteratively improve the Milestone 3 `HybridPGMLIPP` design until it beats:

- vanilla `DynamicPGM`
- vanilla `LIPP`
- the Milestone 2 naive hybrid

on the two mixed workloads:

- 90% insert / 10% lookup
- 10% insert / 90% lookup

and across the three required datasets:

- `fb_100M_public_uint64`
- `books_100M_public_uint64`
- `osmc_100M_public_uint64`

## In-scope files

The agent may read the whole repo, but it should only edit files that are plausibly part of the Milestone 3 implementation:

- `competitors/hybrid_pgm_lipp.h`
- `benchmarks/benchmark_hybrid_pgm_lipp.cc`
- `benchmarks/benchmark_hybrid_pgm_lipp.h`
- `competitors/PGM-index/include/pgm_index_dynamic.hpp`
- `competitors/lipp/src/core/lipp.h`
- `benchmark.h`
- `util.h`

Avoid broad unrelated refactors.

## Reproducibility rule

Do not benchmark directly from the mutable working tree.

For every experiment:

1. Create a unique iteration tag such as `m3_iter24`.
2. Snapshot the current Milestone 3 files into:
   `iterations/<tag>_autoresearch_stage/`
3. Submit the benchmark job using the staged snapshot, not the live repo files.

Use:

```bash
scripts/stage_m3_autoresearch_iteration.sh <tag>
scripts/submit_m3_autoresearch.sh <tag> screen
scripts/submit_m3_autoresearch.sh <tag> full
```

For the normal autonomous loop, prefer:

```bash
python3 scripts/run_m3_autoresearch_loop.py --iterations 1 --promote-screen always
```

This stages the current candidate, submits the SLURM jobs, waits for completion,
computes reward, updates the incumbent snapshot on `keep`, and records both
preflight and full-run bookkeeping.

If the loop is launched with auto-commit enabled, each kept candidate is also
committed to git using only the Milestone 3 source files.

## Evaluation loop

Always use a two-step loop.

### 1. Screen on Facebook first

Run a cheap canary on the two mixed Facebook workloads:

```bash
scripts/submit_m3_autoresearch.sh <tag> screen
```

Keep a candidate only if it:

- compiles cleanly
- finishes within the timeout
- preserves correctness under `--verify`
- shows a plausible throughput improvement over the current kept design

Screen runs are structured to maximize diagnostic signal:

- the screen build uses a safer single-variant-per-workload configuration
- the insert-heavy and lookup-heavy workloads run under separate timeouts
- the loop records failure class and emitted result count for each screen job

### 2. Promote only promising candidates

Only after the Facebook screen looks competitive:

```bash
scripts/submit_m3_autoresearch.sh <tag> full
```

This runs all three datasets and both mixed workloads.

## Logging

Record every iteration in `autoresearch/results.tsv`.

Columns:

```text
iteration	status	reward	screen_job	full_job	results_dir	change_summary	screen_notes	full_notes
```

Use:

- `keep` for candidates worth building on
- `discard` for regressions or weak ideas
- `crash` for compile/runtime failures

Screen-only canaries are recorded separately in `autoresearch/preflight.tsv`.

The loop also maintains:

- `autoresearch/current_status.json`
- `autoresearch/current_blocker.md`

These summarize recent failure classes, current assumptions, and the
recommended edit layer for the next experiment.

## Reward function

After a full six-workload run, compute the reward from the latest baseline CSVs
and the best hybrid throughput seen so far per workload.

For each workload `w`:

- `h_best_prev[w]`: the best hybrid throughput seen so far before the current
  experiment. If no prior hybrid result exists yet for a workload, treat
  `h_best_prev[w] = 0.0`.
- `h_cur[w]`: the current experiment's hybrid throughput
- `b_dpgm[w]`: the best DynamicPGM throughput from the latest baseline CSV for
  this workload
- `b_lipp[w]`: the best LIPP throughput from the latest baseline CSV for this
  workload
- `delta[w] = h_cur[w] - h_best_prev[w]`

Per-workload reward:

- if `h_best_prev[w] >= b_dpgm[w]` and `h_best_prev[w] >= b_lipp[w]`, use
  `min(0, delta[w])`
- otherwise, use `delta[w]`

Total reward is the sum over all six workloads.

If the agent concludes an experiment without computing `h_cur` for all six
workloads, assign reward `-1e18`.

Use:

```bash
python3 scripts/evaluate_m3_autoresearch_reward.py --iteration <tag>
```

This script:

- recomputes baselines from `results_milestone3/*.csv`
- locates the latest staged full-run results for the iteration unless a
  specific results directory is provided
- computes the reward
- updates `autoresearch/reward_state.json`
- appends one row to `autoresearch/results.tsv`

## Incumbent management

The current accepted candidate is mirrored into:

- `autoresearch/incumbent_stage/`
- `autoresearch/incumbent_results/`

To restore the incumbent code into the working tree:

```bash
bash scripts/restore_m3_autoresearch_incumbent.sh
```

## Auto-commit

The unattended Codex launcher enables auto-commit for kept candidates by
default. Those commits:

- include only the Milestone 3 source files
- skip runtime state, logs, SLURM output, and autoresearch bookkeeping files
- are written to branch `autoresearch/m3-kept` unless overridden

## What counts as success

A final Milestone 3 candidate should satisfy all of:

1. No lost keys and no correctness regressions.
2. No forbidden persistent auxiliary structures outside LIPP and DPGM.
3. Better mixed-workload throughput than `DynamicPGM`, `LIPP`, and the Milestone 2 naive hybrid.
4. Repeated results are averaged over at least 3 runs for the final report.

## Heuristics for the agent

- Prefer changes that reduce lookup miss overhead, because the current compliant owner-buffered design still loses badly to LIPP on read-heavy workloads.
- Treat tiny owner regions and over-frequent flushing as high risk; iteration 22 timed out from this failure mode.
- Preserve a clear rollback path: stage, screen, decide, then promote.
- Keep benchmark variant sweeps small. If the agent wants to test many parameters, do it by changing a few template instantiations in `benchmark_hybrid_pgm_lipp.cc`, not by exploding the search space.
- Classify the failure layer before editing: build, startup, screen harness, benchmark execution, reward/logging, or design performance.
- When a run yields no usable measurement, restore observability before optimizing performance.
- After two similar failures, prefer harness or measurement edits over another core-design micro-tweak.
- After three consecutive non-advancing iterations, shift strategy and update the blocker assumptions before proceeding.
