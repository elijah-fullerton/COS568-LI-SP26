# Milestone 3 Literature Review and Final Design

## Purpose

This document does four jobs:

1. Reconstruct the empirical history of this project and the recent failed branches.
2. State how to perform a Milestone-3-specific literature review without drifting into invalid designs.
3. Provide a high-level literature review for this project and for modern autoresearch pipelines.
4. Specify a final implementation design intended to beat Milestone 3 while respecting the project rule that no persistent secondary data structure may store the actual keys or values.

## Project Constraint Interpretation

The project spec forbids "auxiliary data structures" other than LIPP and DynamicPGM if those structures are used to retain the data itself. The spec does not forbid internal query filters that store only compressed membership hints and never store the inserted keys or payloads themselves. This distinction matters:

- Allowed: bit filters, blocked Bloom filters, tiny telemetry counters, min/max summaries, occupancy counters.
- Not allowed: hash tables, sorted side buffers, ARTs, B-trees, quotient filters with recoverable fingerprints if they effectively hold the keyset, or any persistent shadow index that materially stores inserted keys/values outside LIPP and DynamicPGM.

The resulting design space is therefore:

- LIPP is the authoritative read structure.
- DynamicPGM is the authoritative deferred-write structure.
- Any extra persistent metadata must be query-routing metadata only.

## What Happened in This Project

### Base Milestone 2 result

The Milestone 2 hybrid used the simplest correct design:

- bulk-load into LIPP
- insert into one DynamicPGM buffer
- probe DynamicPGM first, then LIPP
- flush by extracting all buffered keys and inserting them individually into LIPP

This was correct but predictably weak on lookup-heavy workloads because every query paid an extra DynamicPGM probe and every flush paid foreground LIPP insertion cost.

### Main Milestone 3 line

The Milestone 3 implementation moved to an owner-buffered design:

- bulk-load LIPP
- freeze the LIPP structure for owner assignment
- route inserts to owner-local DynamicPGM overlays
- choose lookup order adaptively
- flush owners into LIPP incrementally

This was a real improvement over DynamicPGM on many mixed workloads, but it still consistently lost to LIPP on the read-heavy workloads, with Books 10% insert / 90% lookup emerging as the main blocker.

### Empirical bottleneck

The strongest stable evidence from the repo history is:

- insert-heavy workloads are usually not the limiting factor
- Facebook and OSMC are more forgiving to hybrid overhead
- Books read-heavy is the decisive failure mode
- the gap to LIPP on Books read-heavy is not just noise; same-node A/B runs still showed LIPP ahead by roughly 1.5 Mops/s or more

This means the system is not primarily "missing a better insert path". It is paying too much lookup-path tax in the one regime where pure LIPP is already extremely strong.

### Bloom-filter branch

The Bloom-filter branch improved five of the six workloads by using a membership-only filter to skip useless overlay probes. That branch demonstrated an important point:

- a query filter that stores only bits can help materially
- but the Bloom filter alone does not close the Books read-heavy gap

The reason is straightforward: once false overlay probes are suppressed, the remaining loss comes from routing, live overlay maintenance, and residual overlay existence on a workload where LIPP should dominate.

### Failed adaptive branch

The next adaptation attempt tried to make the hybrid degenerate toward LIPP by directly inserting into the base LIPP for "base-favored" owners. This failed for a structural reason:

- owner-local routing depends on the static owner-frame assignment created from the frozen LIPP topology
- direct insertions into LIPP invalidate those assumptions
- later owner-based queries hit `find_owner_frame(owner_id, frame)` assertions in `lipp.h`

This is the key negative result from the project history:

> A successful design must change lookup order, overlay maintenance, and flush behavior without violating owner-frame invariants.

That failure strongly narrows the valid design space.

## How to Perform a Literature Review Tailored to Beating Milestone 3

The literature review should not be "general learned indexes". It should be a constrained design review driven by the actual bottleneck and the project rule set. The right workflow is:

1. Start from the failure mode, not the architecture.
   The target is Books read-heavy, where LIPP already wins and hybrid overhead is the problem.

2. Separate valid from invalid inspirations before reading deeply.
   Reject designs that depend on persistent side indexes storing keys.

3. Read four buckets of literature.
   - Read path minimization in learned indexes.
   - Buffered update architectures and compaction control.
   - Negative membership filters and cache-aware filtering.
   - Agentic/autoresearch evaluation loops, because the search procedure itself has been part of the failure.

4. For each paper, extract only the mechanism that survives the project constraints.
   Example: from LSM/Bloom literature, keep "negative probe elimination", not "store another searchable run directory".

5. Map every mechanism to one of the remaining bottlenecks.
   - false overlay probes
   - excess overlay lifetime
   - expensive lookup ordering
   - noisy evaluation causing false wins

6. Prefer mechanisms that let the hybrid collapse toward LIPP without mutating LIPP outside the owner-buffered contract.

7. End with an implementation decision table.
   For every candidate mechanism, record:
   - allowed or disallowed
   - expected effect on Books read-heavy
   - invariants at risk
   - whether it needs benchmark-space support

This review process is better than a broad survey because it rules out invalid or irrelevant ideas early.

## Literature Review for This Project

### 1. Dynamic learned indexes and buffered updates

Ferragina and Vinciguerra's PGM-index work shows why DynamicPGM is valuable in the first place: dynamic updates are made tractable by a logarithmic, merge-based organization that keeps writes amortized rather than paying a large global rebuild cost each time ([PGM-index](https://arxiv.org/abs/1910.06145)). For Milestone 3, the relevant implication is not "use more PGM", but:

- preserve DynamicPGM as the write-optimized transient structure
- do not contaminate the insert fast path with expensive foreground LIPP work

This aligns with the project's earlier empirical results: insert-heavy runs are not the dominant failure once owner buffering is in place.

### 2. LIPP and why Books read-heavy is hard

LIPP is specifically designed to offer precise-position lookup with minimal refinement work and to rebalance by local restructuring rather than global search overhead ([LIPP](https://arxiv.org/abs/2104.05520)). This matters because the project is losing on a dataset/workload pair where pure LIPP is already close to the ideal behavior:

- high lookup fraction
- point-query dominant workload
- enough distribution regularity that LIPP's routing is already efficient

Therefore any hybrid that retains a meaningful overlay or performs routine overlay-first checks is competing against a very strong baseline. The literature prediction matches the empirical outcome: if the read path is even modestly more complex than LIPP's direct path, the hybrid will lose.

### 3. Concurrent/adaptive learned indexes

Work such as XIndex shows that buffered designs can beat static read-optimized structures only if compaction and publication are engineered so that reads do not routinely pay heavy indirection or blocking costs ([XIndex](https://www.vldb.org/pvldb/vol15/p1-luo.pdf)). The key lesson for this project is not the full concurrency architecture; it is the cost-model lesson:

- a buffered layer helps only when its write savings exceed its read-path tax
- when the buffered layer is not paying for itself, the system should collapse toward the base structure

The failed adaptive branch applied the right idea but in the wrong place. The proper adaptation point is the overlay policy, not direct mutation of the base structure that breaks owner routing invariants.

### 4. ALEX and the value of adaptivity

ALEX emphasizes adaptive structural control under changing workloads rather than one static learned-index shape ([ALEX](https://arxiv.org/abs/1905.08898)). The direct takeaway for Milestone 3 is:

- fixed flush thresholds and fixed probe ordering are unlikely to win both insert-heavy and lookup-heavy regimes
- adaptation must be driven by online signals rather than hard-coded dataset labels

However, ALEX-style adaptation in this project must be limited to controls that preserve the existing LIPP/DPGM authority split.

### 5. Bloom filters and negative probe elimination

Bloom filters remain one of the most efficient ways to avoid unnecessary negative lookups when the filter stores only compact membership information rather than the keys themselves ([Bloom 1970](https://dl.acm.org/doi/10.1145/362686.362692)). Later systems literature, especially in LSM-tree engines, reinforces the same point: read-heavy performance improves when you can cheaply prove a miss before touching a more expensive search path.

For Milestone 3, the relevant implication is narrow but important:

- the filter should only be used to suppress overlay misses
- it should be cache-conscious and disabled when overlays are too small to justify its maintenance cost

This is exactly why the earlier Bloom branch improved five workloads but did not solve the final one: negative-probe elimination helps, but it does not help enough if the overlay remains alive too long on a workload where it is rarely useful.

### 6. What the literature says about the remaining gap

Putting the learned-index and filter literature together yields a clear diagnosis:

- The hybrid should keep DynamicPGM for burst absorption.
- The hybrid should keep a query filter because negative overlay probes are genuinely expensive.
- The hybrid should aggressively shorten overlay lifetime in read-dominant, low-overlay-value regimes.
- The hybrid must not insert directly into LIPP under owner-buffered mode.

So the winning design is not "more filter" or "more buffering". It is:

> query-filtered overlays with online utility control and owner-safe micro-flush.

## Literature Review for Modern Autoresearch Pipelines

The second relevant literature thread is the search process itself. Recent agentic research systems such as The AI Scientist emphasize iterative hypothesis generation, execution, and revision loops, but they also show that uncontrolled search can drift toward local artifacts if evaluation is not tightly coupled to the task objective ([The AI Scientist](https://arxiv.org/abs/2408.06292)). Agent Laboratory makes a similar point at the workflow level: autonomous research systems need structured decomposition and explicit evaluation checkpoints rather than open-ended idea churn ([Agent Laboratory](https://arxiv.org/abs/2501.04227)).

For engineering-centered pipelines, MLE-bench formalizes the lesson even more sharply: many plausible changes are indistinguishable without strong experimental discipline, same-session evaluation, and metrics tied directly to the real benchmark objective ([MLE-bench](https://arxiv.org/abs/2410.07095)). In the context of this project, that literature implies:

- screen runs must test the real bottleneck quickly
- candidate promotion should depend on same-session evidence
- cross-job comparisons should not be treated as wins
- the search loop should optimize for observability first when no results are emitted

That last point exactly matches the branch history here: several iterations failed not because the core idea was unsound, but because the harness or result path broke and the loop kept mutating design parameters anyway.

The practical implication for Milestone 3 is that the evaluation protocol is part of the design:

- one build
- one compute node
- one session
- at least three repeats
- all three indexes measured under the same conditions

Anything weaker risks overfitting to noise again.

## Final Design Intended to Beat Milestone 3

### Design summary

The final design is a sound, label-free, owner-safe adaptive hybrid with three components:

1. Owner-local DynamicPGM overlays remain the only place new keys live before flush.
2. A membership-only blocked Bloom filter is maintained per owner only when that owner's overlay is large enough to justify it.
3. An online utility controller shortens overlay lifetime and changes lookup order when an owner is read-dominant and the overlay is not paying for itself.

This preserves the project rule and preserves LIPP owner invariants.

### Core invariant

At all times, every inserted key is in exactly one of:

- that owner's DynamicPGM overlay, or
- the authoritative LIPP after flush

No key is ever stored in any auxiliary persistent index.

### Data structures

Persistent state:

- `LIPP<Key, Value>` base index
- `vector<DynamicPGM>` owner overlays
- `vector<size_t>` owner overlay sizes
- `vector<size_t>` owner region sizes
- `vector<OwnerTelemetry>` owner-local rolling counters
- `vector<BlockedBloomFilter>` owner query filters storing only bits

`BlockedBloomFilter` stores:

- bit blocks only
- inserted count
- no recoverable key payloads

### Online telemetry

For each owner, maintain decayed counters:

- recent lookups
- recent inserts
- overlay probes
- overlay hits
- Bloom positives
- Bloom negatives

Global decayed counters maintain the recent lookup/insert regime.

These counters are small metadata and are valid under the project rules.

### Adaptation logic

Define an owner as `base_favored` when:

- global recent lookups dominate inserts
- owner-local recent lookups dominate owner inserts
- overlay hit rate is low
- buffered occupancy is sparse relative to the owner region

This is intentionally label-free. It never checks dataset name or workload token.

### Lookup path

For `overlay_favored` owners:

- if overlay occupancy is high enough, probe overlay first
- use the Bloom filter to skip definite misses
- otherwise fall back to LIPP

For `base_favored` owners:

- search LIPP first
- only consider the overlay on a miss
- if a Bloom filter exists and says "definite miss", skip overlay entirely

This is the most important behavioral change. It makes read-heavy owners act much closer to pure LIPP without breaking owner routing invariants.

### Insert path

All inserts still go into the owner's DynamicPGM overlay.

There is no direct insertion into LIPP from the fast path. This avoids the exact owner-frame bug that broke the previous adaptive branch.

### Flush policy

The controller computes a per-owner effective flush threshold.

For `overlay_favored` owners:

- use the incumbent threshold logic or a mild deferred threshold

For `base_favored` owners:

- use an aggressively smaller threshold
- flush once the overlay exceeds a tiny absolute cap or a small occupancy fraction of the owner region

This creates a "micro-buffer then flush" behavior that approximates degeneration to LIPP without invalidating the static owner partition.

### Filter maintenance policy

Do not always maintain the filter.

Maintain a blocked Bloom filter only when:

- buffered entries exceed a minimum absolute floor
- the owner has seen enough overlay probes that negative filtering is useful

If the overlay is tiny, disable filter maintenance and skip its cost.

### Why this design can beat Milestone 3

The design is targeted precisely at the remaining gap:

- insert-heavy cases remain buffered and should preserve the current wins over DynamicPGM
- read-heavy cases stop paying repeated useless overlay probes
- owner-safe micro-flush keeps overlays from living long enough to impose large LIPP-relative read overhead
- the design never uses dataset labels and never breaks LIPP owner metadata

The design's best-case behavior on the bad regime is "almost pure LIPP plus a tiny recent-write buffer". That is the only credible path left inside the project's rules.

## Implementation Checklist

1. Start from the stable owner-buffered incumbent, not the broken direct-insert branch.
2. Add owner telemetry with decay.
3. Add blocked Bloom filters that store only bits.
4. Use telemetry to choose:
   - probe order
   - filter maintenance
   - micro-flush aggressiveness
5. Never call direct `lipp_.insert(...)` as a bypass from the fast path outside normal owner flush.
6. Keep the benchmark search space small:
   - one insert-heavy variant
   - a few read-heavy variants with smaller owner spans and flush thresholds
7. Evaluate on a single compute node and a single session.

## Evaluation Protocol

To minimize noise and avoid false wins:

1. Build once on the compute node.
2. Stage all data into the node-local scratch directory.
3. Run all six workloads in one session.
4. Compare `DynamicPGM`, `LIPP`, and `HybridPGMLIPP` in the same job.
5. Use at least 3 repeats per workload in the same session.
6. Treat milestone success as valid only if all six workloads beat both baselines under that same-session run.

That protocol is not optional. The project history already showed that cross-job comparisons can create fake wins, especially on Books read-heavy.
