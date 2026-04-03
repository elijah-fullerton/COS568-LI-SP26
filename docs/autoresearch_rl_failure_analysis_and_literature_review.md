# Autoresearch-RL Failure Analysis and Literature Review

## Executive Summary

The `autoresearch-rl` branch did not merely fail to find an improving learned-index design. It spent most of its recent budget in a control-loop pathology where the loop misattributed infrastructure failures to candidate-level screening failures, then used that misdiagnosis to bias the next mutation toward the same implementation layer. The dominant recent pattern is:

1. The loop restores the incumbent.
2. Codex makes another implementation-heavy mutation to the same small file set.
3. The screen job fails before producing any benchmark output.
4. The loop records the outcome as `screen_failure_no_result`.
5. The status generator infers "implementation" or "strategy" rather than a harness/build blocker.
6. The prompt sent to the next edit pass still discourages harness changes.

The result is a classic closed-loop observability failure: the system lost the ability to discriminate among compile failure, job-environment failure, benchmark crash, and true performance regression, so it kept exploring the wrong part of the search space.

## What Actually Went Wrong in This Branch

### 1. The recent failures were primarily harness failures, not algorithmic failures

The decisive evidence is in the archived SLURM stderr for recent screen jobs:

- `m3_iter230` job `2697522` fails with `ccache: command not found`.
- `m3_iter231` job `2697528` fails with the same error.

In both cases the screen script hard-coded `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache`, but the cluster node did not have `ccache` installed. That means the benchmark binary was never built. The loop then recorded these as `screen_failure_no_result` because no `RESULT:` lines were emitted and no `benchmark_status` file was produced.

This is a classification error with major downstream consequences. A build-system failure should have forced a screening/harness repair. Instead, it was blended into the same bucket as candidate crashes.

### 2. The loop kept optimizing the design layer despite low-information failures

The trajectory after the incumbent at `m3_iter34` shows a long run of:

- mutation family: `implementation`
- novelty score: `0.35`
- file set: mostly `benchmark.h`, `util.h`, `benchmarks/benchmark_hybrid_pgm_lipp.cc`, `competitors/hybrid_pgm_lipp.h`
- outcome: `screen_failure_no_result`

This is not healthy exploration. It is a narrow repeated search around one architectural hypothesis with almost no new information entering the loop. Once the failure mode shifted from design-level to harness-level, continued implementation-only exploration had near-zero expected value.

### 3. The mutation policy and prompting regime were too restrictive

The checked-in mutation policy only allowed the `implementation` family, even though the loop code itself supports `screening` and `measurement` families. That mismatch matters:

- the orchestration code was built to support multi-layer recovery,
- but the policy delivered to the editing model prohibited that recovery,
- and the compact prompt explicitly said to prefer implementation changes over harness churn.

So the system had the software hooks for recovery, but the policy layer disabled them exactly when they were needed.

### 4. The loop confused "no result" with "performance pathology"

`screen_failure_no_result` is too coarse as a terminal category. From an ML-for-systems perspective, this collapses several distinct latent causes:

- compile failure,
- startup failure,
- dataset staging failure,
- benchmark crash before first metric,
- timeout before first result,
- pathological candidate that hangs in verify or construction.

These have very different optimal interventions. Aggregating them into one label destroys the credit-assignment signal needed for useful policy adaptation.

### 5. The screening pipeline paid unnecessary fixed costs every iteration

Recent jobs were re-downloading the 763 MB Facebook dataset into per-job scratch and rebuilding from scratch. Even when this was not the immediate root cause of failure, it inflated iteration latency and reduced the fraction of wall-clock budget spent on informative evaluation. In any autoresearch system, fixed-cost overhead acts like an exploration tax; if that tax is too high, the search becomes dominated by infrastructure churn instead of hypothesis testing.

### 6. The branch had an incumbent, but the loop behaved as though it had no reliable baseline context

The last completed and incumbent iteration is `m3_iter34`, and that iteration did produce screen and full outputs. So the loop did have a working baseline snapshot. However, the later prompt/context state drifted toward a generalized "strategy" warning rather than a precise causal diagnosis grounded in the build artifacts. That weakened the exploitation side of the search: the system stopped asking, "why did the known-good path fail to even build under the current screen harness?"

## Failure Taxonomy

At a systems level, the branch exhibited four coupled failures.

### A. Failure of observability

The loop lacked a reliable per-iteration decomposition of:

- edit success,
- stage success,
- compile success,
- smoke success,
- screen success,
- full-run success.

Without that decomposition, the reward model and prompt model were conditioning on a lossy proxy instead of true pipeline state.

### B. Failure of action-space design

The policy restricted edits to implementation files even though the harness was part of the effective environment. In RL terms, the action space was misspecified: the agent could change the policy subject, but not enough of the environment interface to restore measurement.

### C. Failure of exploration scheduling

Low novelty was recognized, but the response was weak. The search continued to revisit nearly identical edit signatures. This is a quality-diversity problem: once a region of the search space is repeatedly producing null observations, the controller should force migration to a different mutation family or diagnostic layer.

### D. Failure of infrastructure robustness

A cluster-local missing binary (`ccache`) was sufficient to derail hundreds of iterations. A robust autoresearch pipeline should treat optional accelerators as optional, not as hard dependencies.

## Why This Matters for the Learned-Index Project Specifically

This project is not a toy coding task. It sits in a difficult regime for autonomous optimization:

- performance is hardware- and cache-sensitive,
- correctness requires preserving ordered-map semantics under updates,
- the space of design changes includes structural, scheduling, and measurement choices,
- the optimization signal is sparse and noisy,
- screening uses reduced workloads whose fidelity to full-run behavior is imperfect.

For such problems, the first obligation of the controller is to preserve measurement integrity. Until the harness can reliably answer "did the candidate build, run, and produce a trustworthy throughput sample?", design-space search is mostly wasted compute.

## Comprehensive Literature Review

### Part I: Literature Relevant to This Learned-Index Project

#### 1. The original learned-index framing

Kraska et al., "The Case for Learned Index Structures" reframed index lookup as function approximation: if sorted keys approximately follow a cumulative distribution function, a learned model can predict rank and replace part of a classical index. The key conceptual move was to convert indexing from exact structural routing to approximate position prediction plus bounded local search. That framing is still the intellectual base of this project because `HybridPGMLIPP` is ultimately trying to trade model quality, local search cost, and update cost more effectively than either DynamicPGM or LIPP alone.

Implication for this branch: throughput is determined less by asymptotics than by the micro-architecture of misprediction repair, update buffering, and cache behavior.

Source:
- Kraska et al., "The Case for Learned Index Structures," 2018. https://arxiv.org/abs/1712.01208

#### 2. Dynamic and updatable learned indexes

The central challenge after the original learned-index papers was updates. Static RMIs are relatively easy; practical update-heavy systems are not.

`ALEX` treats the index as an adaptive hierarchy of model nodes with gapped array storage, explicitly optimizing for inserts and local reorganization. Its core lesson is that update-friendly learned indexes need local slack, adaptive splitting, and data-layout awareness rather than pure prediction accuracy.

Source:
- Ding et al., "ALEX: An Updatable Adaptive Learned Index," SIGMOD 2020. https://arxiv.org/abs/1905.08898

`LIPP` pushes harder on update efficiency by preserving precise positions through a tree structure that avoids expensive global maintenance. The important design lesson from LIPP is that the update path, not just the lookup predictor, must be first-class. That is directly relevant here because `HybridPGMLIPP` inherits its mutable substrate from LIPP and therefore inherits its sensitivity to buffer ownership, flush policy, and routing consistency.

Source:
- Wu et al., "Updatable Learned Index with Precise Positions," VLDB 2021. https://dl.acm.org/doi/10.14778/3407790.3407795

`XIndex` adds concurrency and multicore scalability, showing that a learned index good enough for single-thread throughput is not automatically viable under realistic systems constraints. Its design emphasizes versioning, staged retraining, and scalable access paths.

Source:
- Tang et al., "XIndex: A Scalable Learned Index for Multicore Data Storage," PPoPP 2020. https://dl.acm.org/doi/10.1145/3332466.3374528

For this project, the high-level takeaway is that update-friendly learned indexes are governed by maintenance cost decomposition:

- prediction quality,
- repair-search cost,
- insert-path locality,
- split/merge frequency,
- retraining or rebuild cadence,
- concurrency/cache side effects.

#### 3. PGM-family indexes and the role of piecewise approximation

The PGM-index shows that strong piecewise-linear geometry can deliver compact and performant index structures with explicit error guarantees. Dynamic PGM extends that idea to updates, but the update path is still structurally different from a write-optimized learned tree like LIPP.

Source:
- Ferragina and Vinciguerra, "The PGM-index: a fully-dynamic compressed learned index with provable worst-case bounds," VLDB 2020. https://pgm.di.unipi.it/

For `HybridPGMLIPP`, the entire research hypothesis is that a front-end or substructure inspired by PGM can reduce search cost where LIPP alone is weak, while LIPP-style mutability preserves insert performance. That is plausible, but only if the hybrid boundary does not create excessive routing overhead or stale ownership metadata.

#### 4. Cache-aware and workload-aware learned indexing

`CARMI` is especially relevant because it argues that practical learned-index performance is dominated by partitioning and cache behavior, not just abstract model accuracy. It uses a cost-based construction algorithm and explicitly targets real-system effects.

Source:
- Zhang and Gao, "CARMI: A Cache-Aware Learned Index with a Cost-based Construction Algorithm," 2021. https://arxiv.org/abs/2103.00858

This matters because the branch has been screening on mixed workloads where lookup latency, insert buffering, and memory locality interact nonlinearly. If `HybridPGMLIPP` loses to LIPP on read-heavy mixes, the likely causes are not only predictor miss error but also additional pointer chasing, poor owner locality, and branch-heavy fallback logic.

#### 5. Project-specific synthesis

For this milestone, the literature implies the following design priorities:

- First restore stable measurement and build determinism.
- Then isolate the lookup-repair cost introduced by the hybrid boundary.
- Treat buffer ownership and flush behavior as core algorithmic variables, not implementation details.
- Prefer smaller, diagnostically separable structural changes over large mixed edits.
- Benchmark with a screening workload whose dominant costs are predictive of the full six-workload objective.

### Part II: Literature on State-of-the-Art Autoresearch and Algorithm-Discovery Pipelines

#### 1. Evolutionary program search with verifiable evaluators

The strongest recent autoresearch systems do not rely on one-shot code generation. They pair LLM mutation with an external evaluator, then use an evolutionary or quality-diversity controller to maintain populations, preserve diversity, and exploit objective feedback.

DeepMind's `AlphaEvolve` is the clearest reference point. The published system description emphasizes:

- prompt assembly over prior programs,
- automated verification and scoring,
- evolutionary selection over a program database,
- objective-driven search on domains with crisp evaluators.

Source:
- DeepMind, "AlphaEvolve: A Gemini-powered coding agent for designing advanced algorithms," 2025. https://deepmind.google/blog/alphaevolve-a-gemini-powered-coding-agent-for-designing-advanced-algorithms/

The open-source `OpenEvolve` implementation makes the architectural bias explicit: MAP-Elites-style quality-diversity, island populations, ensemble LLMs, and artifact feedback. Even if one discounts some README claims, the important systems lesson is correct: stagnation is fought through diversity maintenance and structured evaluator feedback, not repeated local edits from a single incumbent trajectory.

Source:
- OpenEvolve repository. https://github.com/algorithmicsuperintelligence/openevolve

For this branch, that translates into a concrete diagnosis: the loop was missing the quality-diversity safeguards that would have forced a switch away from repeated implementation-level near-duplicates once the return from that family collapsed.

#### 2. Fully automated research-paper pipelines

`The AI Scientist` is broader than this project's objective, but it is informative because it exposes the brittleness of end-to-end autonomous research stacks. The system generates ideas, runs code, produces plots, writes papers, and even simulates review. Its main lesson is not that full autonomy is solved; rather, it demonstrates how many separate subsystems must work reliably before higher-level novelty matters.

Source:
- Lu et al., "The AI Scientist: Towards Fully Automated Open-Ended Scientific Discovery," 2024. https://github.com/SakanaAI/AI-Scientist

`Agent Laboratory` pushes a similar agenda: multi-agent orchestration across the research process. The key relevance here is that multi-stage autonomy amplifies low-level infrastructure defects. If the execution harness is brittle, additional agentic sophistication mostly increases the rate at which the system produces invalid or uninterpretable trials.

Source:
- Agent Laboratory, 2025. https://arxiv.org/pdf/2501.04227

#### 3. Benchmarks for long-horizon objective-driven optimization

Recent benchmarks such as `MLE-bench` and `ALE-Bench` emphasize the difference between short-horizon pass/fail coding and long-horizon iterative optimization. The latter regime is much closer to this project: the agent must interpret noisy feedback, preserve working baselines, and search over trajectories rather than answers.

`ALE-Bench` is particularly relevant because it explicitly frames the task as long-horizon objective-driven algorithm engineering and highlights the gap between single-shot competence and sustained iterative improvement.

Source:
- Imajuku et al., "ALE-Bench: A Benchmark for Long-Horizon Objective-Driven Algorithm Engineering," 2025. https://arxiv.org/abs/2506.09050

The broad lesson is that controller design matters as much as base-model quality. Evaluation artifacts, budget allocation, and adaptive search policies are first-order determinants of success.

#### 4. Reflection and verbal reinforcement learning

A related but distinct line of work, including `Reflexion`, studies how language agents can use verbalized feedback as a memory and policy-improvement signal. The useful connection here is not the exact method, but the principle: the feedback representation must preserve the cause of failure with enough fidelity to change the next action.

Source:
- Shinn et al., "Reflexion: Language Agents with Verbal Reinforcement Learning," 2023. https://arxiv.org/abs/2303.11366

In this branch, the feedback abstraction was too lossy. "screen failure, no result" is not a sufficient verbal reinforcement signal for choosing the next mutation family.

## Implications for Updating This Branch

The literature and branch evidence point to a consistent remediation plan.

### Immediate controller and harness fixes

- Make `ccache` optional, not mandatory.
- Cache staged datasets across jobs to reduce fixed per-iteration cost.
- Expand the mutation policy so screening and measurement edits are legal.
- Update prompting and status generation so no-result screens trigger harness inspection before more design churn.

### Medium-term search-policy fixes

- Split `screen_failure_no_result` into finer subtypes when possible.
- Record compile/build failure explicitly from SLURM stderr or build artifacts.
- Force mutation-family diversification after repeated low-novelty null-result iterations.
- Promote candidates only after the screen path is known to be diagnostically valid.

### Research-direction fixes for the learned-index objective

- Benchmark the hybrid boundary directly: owner lookup, buffer probe, fallback search, flush path.
- Use smaller ablations that isolate routing and buffering rather than broad mixed edits.
- Prefer hypotheses derived from update-path and cache-path decomposition over generic parameter perturbation.

## Changes Derived from This Analysis

This document was created to serve as persistent context for the branch. The code changes made alongside it are intended to convert the postmortem into operational policy:

- the build harness now tolerates missing `ccache`,
- the screen and full compute scripts reuse cached datasets,
- the mutation policy now allows screening and measurement repairs,
- the status/prompt generator now advises failure-local fixes rather than defaulting to implementation churn.

## Source Links

- Kraska et al., "The Case for Learned Index Structures." https://arxiv.org/abs/1712.01208
- Ding et al., "ALEX: An Updatable Adaptive Learned Index." https://arxiv.org/abs/1905.08898
- Wu et al., "Updatable Learned Index with Precise Positions (LIPP)." https://dl.acm.org/doi/10.14778/3407790.3407795
- Ferragina and Vinciguerra, "PGM-index." https://pgm.di.unipi.it/
- Zhang and Gao, "CARMI." https://arxiv.org/abs/2103.00858
- Tang et al., "XIndex." https://dl.acm.org/doi/10.1145/3332466.3374528
- DeepMind, "AlphaEvolve." https://deepmind.google/blog/alphaevolve-a-gemini-powered-coding-agent-for-designing-advanced-algorithms/
- OpenEvolve repository. https://github.com/algorithmicsuperintelligence/openevolve
- Lu et al., "The AI Scientist." https://github.com/SakanaAI/AI-Scientist
- Imajuku et al., "ALE-Bench." https://arxiv.org/abs/2506.09050
- Shinn et al., "Reflexion." https://arxiv.org/abs/2303.11366
