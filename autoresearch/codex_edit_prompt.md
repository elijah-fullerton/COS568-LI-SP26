# Milestone 3 Compact Edit Prompt

Work only in the current repo checkout. Implement exactly one bounded candidate edit and stop.
Do not submit benchmarks yourself; the outer loop will do that after you exit.

Minimal read set before editing:
- `autoresearch/current_blocker.md`
- `autoresearch/current_status.json`
- `autoresearch/mutation_policy.json`
- `docs/autoresearch_rl_failure_analysis_and_literature_review.md` when the blocker is strategy, screening, or measurement
- inspect only the specific source files you plan to change

Program brief:
- Goal: improve Milestone 3 HybridPGMLIPP against DynamicPGM, LIPP, and the Milestone 2 naive hybrid.
- Primary workloads: mixed 10% insert / 90% lookup and 90% insert / 10% lookup.
- Primary datasets: fb_100M_public_uint64, books_100M_public_uint64, osmc_100M_public_uint64.
- Workflow: make one bounded candidate edit, let the outer loop stage and benchmark it, then stop.
- Priority: restore measurability first when runs fail before producing useful results.

Current state:
- Incumbent iteration: `m3_iter34`
- Last completed iteration: `m3_iter34`
- Dominant failure class: `screen_failure_no_result`
- Recommended edit layer: `strategy`
- Consecutive non-advancing iterations: `6`
- Low novelty streak: `5`
- Best tracked hybrid throughput keys: `0`

Allowed edit targets:
- `benchmark.h`
- `util.h`
- `benchmarks/benchmark_hybrid_pgm_lipp.cc`
- `benchmarks/benchmark_hybrid_pgm_lipp.h`
- `competitors/hybrid_pgm_lipp.h`
- `competitors/PGM-index/include/pgm_index_dynamic.hpp`
- `competitors/lipp/src/core/lipp.h`
- `scripts/run_m3_autoresearch_screen_compute.sh`
- `...`

Recent experiments:
- m3_iter228: status=crash, family=implementation, screen=screen_failure_no_result, reward=n/a, files=benchmark.h, util.h, benchmarks/benchmark_hybrid_pgm_lipp.cc, ...
- m3_iter229: status=crash, family=implementation, screen=screen_failure_no_result, reward=n/a, files=benchmark.h, util.h, benchmarks/benchmark_hybrid_pgm_lipp.cc, ...
- m3_iter230: status=crash, family=implementation, screen=screen_failure_no_result, reward=n/a, files=benchmark.h, util.h, benchmarks/benchmark_hybrid_pgm_lipp.cc, ...

Rules:
- Make one coherent improvement only.
- Prefer the failure-local fix over another core-design micro-tweak.
- Keep sweeps small and measurable.
- If recent runs failed before usable results, prioritize measurability and robustness over raw throughput tuning.
- Do not touch unrelated files or broaden scope beyond the allowed paths.

When finished:
- leave edits in the working tree
- write a short summary of the candidate idea and expected effect
- stop
