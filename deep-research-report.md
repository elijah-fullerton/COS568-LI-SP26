# High-Throughput Milestone 3 Hybrid Learned Index: Codex Implementation Specification

## Problem framing and hard performance requirement

Milestone 3 requires an “advanced flushing strategy” for a hybrid index that uses Dynamic PGM (DPGM) for inserts and LIPP for (most) lookups, while **never losing keys** and **using no persistent auxiliary data structures other than DPGM and LIPP**. The toughest constraint is the explicit throughput requirement: the final hybrid must achieve **higher throughput than vanilla DPGM and vanilla LIPP on both mixed workloads (10% insert / 90% lookup and 90% insert / 10% lookup), across all three datasets**, and also outperform your own Milestone 2 naive hybrid. Any strategy that blocks foreground operations during flush, amplifies contention, or increases lookup work (e.g., searching many buffers) is likely to lose on at least one workload. citeturn9view2turn7view0

The design below commits to three throughput-first principles derived directly from learned-index and concurrent-index literature:

- **Write buffering + batched migration**: buffering updates and migrating in batches is a classic way to maximize write throughput and amortize rebuild/merge work (LSM-tree lineage; DPGM’s “logarithmic method”). citeturn6search0turn5view2  
- **Avoid blocking reads and writes during migration**: naive compaction blocks and destroys throughput; concurrent learned indexes (e.g., XIndex) use non-blocking/background compaction and publish new structures safely via RCU-style techniques. citeturn1view2turn9view0turn6search13  
- **Bulk integration via subtree rebuild rather than per-key insertion**: LIPP’s own “adjustment” strategy explicitly rebuilds a subtree from collected keys when enough changes accumulate, instead of paying per-key conflict-management costs forever. This is a direct mechanism for *fast flushing* if we trigger it in controlled batches. citeturn3view1turn3view2turn3view3  

The following specification is designed so that:
- **Foreground insert path remains DPGM-only**, fast, and non-blocking. citeturn5view2  
- **Foreground lookup path remains LIPP-fast**, with minimal additional overhead, by keeping delta structures small and using cheap range gating to avoid unnecessary delta lookups. citeturn7view0  
- **Flush work is off the critical path** and is performed as **batched LIPP subtree rebuilds** (bulkload/BuildPartialTree) and **RCU-safe publication**. citeturn3view3turn6search13turn9view0  

## What the literature implies for the winning flush strategy

### DPGM’s dynamic updates are already “LSM-like,” and we should preserve its strength
The dynamic PGM design for arbitrary-position inserts uses the “logarithmic method”: maintain multiple sorted sets (or buffers) of exponentially increasing size; when inserting a key, merge a run of filled sets and rebuild an index on the merged set—yielding **amortized** update guarantees. citeturn5view1turn5view2  
This is conceptually aligned with the LSM-tree’s core idea of **deferring and batching index changes** to reduce per-insert cost. citeturn6search0  

Takeaway: **do not contaminate the insert fast path with LIPP work**. Let DPGM do what it is best at (fast amortized ingestion), and treat LIPP as a mostly-read-optimized structure.

### LIPP already provides the exact mechanism you want for “fast bulk assimilation” inside an existing tree
LIPP inserts avoid in-node searching by predicting precise positions and resolving conflicts by creating child nodes; but excessive conflicts and growth can hurt both lookup and insert, so LIPP includes an **adjustment strategy**: when expansion and conflict criteria are met, it **collects all keys in a subtree (in sorted order), builds a new partial tree via BuildPartialTree (Algorithm 5), and replaces the pointer to that subtree**. citeturn3view1turn3view2turn3view3  

Takeaway: the fastest “flush” into LIPP is not per-key insertion—it is **subtree rebuild + pointer replacement**, which is exactly what LIPP is designed to do for performance stability. citeturn3view2turn3view3  

### Concurrency: publish new structures with RCU-style “remove vs reclaim”
XIndex specifically calls out that delta-buffer + compaction is natural but causes severe slowdowns if (a) every query must search the delta first, and (b) compaction blocks concurrent operations. It uses background compaction and RCU barriers to avoid blocking and to ensure memory safety/consistency during structure replacement. citeturn9view2turn9view1  
RCU’s core concept is that updates split into (1) publishing/removal of references, which can be concurrent with readers, and (2) reclamation after a grace period when readers are done. citeturn6search13  

Takeaway: implement flush as **copy/build → atomic publish → epoch grace period → reclaim**, and keep reads lock-free.

### Search-method tuning matters: keep refinement work minimal
Evaluation frameworks for learned indexes explicitly treat the “search method” (linear, binary, exponential, interpolation, SIMD linear) as a critical performance lever for local refinement around predicted positions. citeturn6search3turn7view0  
Your codebase already exposes a `search_method` knob for DPGM/B+tree in benchmarking. Milestone 3 should treat DPGM’s delta lookups as latency-critical overhead and choose the best refinement search method for your DPGM epsilon and workload. citeturn6search3turn7view0  

## Proposed Milestone 3 architecture

### High-level structure

- **Base:** one LIPP instance holding the bulk-loaded data *plus all keys that have been flushed so far*. This is the “read-optimized” main structure. citeturn0search1turn2view0  
- **Delta:** a **double-buffered DPGM**:
  - `dpgm_active`: receives all new inserts.
  - `dpgm_frozen`: a snapshot buffer currently being flushed (readable by lookups, not receiving inserts).
- **Flush engine:** background (or cooperative) loop that migrates keys from `dpgm_frozen` into LIPP by **batched subtree rebuilds** (LIPP BuildPartialTree / bulkload logic), then retires old nodes safely using an epoch/RCU-style grace period. citeturn3view3turn6search13turn9view0  

Key correctness invariant (must always hold):
> For every inserted key, at all times it exists in at least one of {`dpgm_active`, `dpgm_frozen`, published LIPP}.

This permits safe, non-blocking migration even under concurrency.

image_group{"layout":"carousel","aspect_ratio":"16:9","query":["LIPP learned index gapped array model node diagram","PGM index piecewise linear segments tree diagram","XIndex two-phase compaction RCU diagram"],"num_per_query":1}

### Why this design is plausibly throughput-dominant vs both baselines

- Against vanilla **LIPP** on mixed workloads: vanilla LIPP pays per-insert costs (conflict handling, node creation, adjustment triggers) in the foreground, which directly competes with lookups and increases structural churn; hybrid pushes those costs into an amortized, controlled rebuild during flush, keeping the frequently-read LIPP mostly stable and allowing DPGM to absorb write bursts. citeturn3view0turn3view1turn5view2turn7view0  
- Against vanilla **DPGM** on lookup-heavy workloads: vanilla DPGM lookups involve model descent plus bounded searches at each level (and, for fully dynamic, potentially multiple structures/buffers); hybrid serves most reads from LIPP’s precise-position traversal and keeps the DPGM delta small so the “extra check” cost is minimal. citeturn5view0turn5view2turn3view0turn7view0  
- Against both under concurrency/migration: background rebuild + atomic publish avoids compaction stalls that can destroy throughput; this is exactly the concern raised by concurrent learned-index work, motivating RCU-style publication. citeturn9view2turn6search13  

## Codex implementation instructions

This section is written as an **actionable build spec** for a coding agent. It spells out files, APIs, threading, invariants, and tuning knobs.

### Repository touchpoints and file layout

Create/modify exactly these (as required by the project):

- `competitors/hybrid_pgm_lipp.h` (new): hybrid index implementation.
- `benchmarks/benchmark_hybrid_pgm_lipp.h` (new): benchmark wrapper interface.
- `benchmarks/benchmark_hybrid_pgm_lipp.cc` (new): benchmark registration / factory.
- Additionally, expect to modify **LIPP implementation files** under `competitors/` to expose:
  - Subtree key collection (`CollectKeys`).
  - Subtree rebuild (`BuildPartialTree` / bulkload builder).
  - Safe subtree pointer replacement (“publish”).
  - Optional: node-key-range metadata (min/max).  
  These are not “aux data structures”; they are capabilities inside LIPP itself. citeturn3view2turn3view3  

### Define the hybrid public API to match existing index wrappers

In `competitors/hybrid_pgm_lipp.h`, implement a template similar to other competitors (mirror style from `competitors/dynamic_pgm_index.h` mentioned in the assignment). The minimal required operations for benchmarks are typically:

- `bulk_load(keys, values)` or constructor that takes sorted initial data
- `find(key, &value)` or `lookup(key)`
- `insert(key, value)`
- `size_in_bytes()` (for index size plots)
- destructors/cleanup

If the benchmark framework expects a specific interface (likely an abstract base), **match existing competitors exactly**.

### Core data structure design

Implement:

```cpp
struct HybridConfig {
  // Flush thresholds
  double frozen_trigger_frac;        // e.g., 0.01–0.10 of total keys
  size_t frozen_trigger_min_keys;    // absolute safety floor
  size_t max_frozen_keys;            // hard cap to bound lookup overhead

  // Migration granularity
  size_t rebuild_target_max_keys;    // max keys in a subtree rebuild batch
  size_t rebuild_target_min_keys;    // avoid tiny rebuild overhead

  // Background flush policy
  bool enable_background_thread;
  size_t flush_work_budget_per_op;   // if cooperative flushing is used

  // DPGM tuning
  int pgm_epsilon;                   // error bound
  SearchMethod pgm_search_method;    // binary/exponential/etc

  // LIPP rebuild tuning (gap factor, node size)
  double lipp_gap_factor;            // corresponds to α in LIPP Algorithm 5
  size_t lipp_max_node_bytes;        // e.g. align with LIPP’s “16MB” discussion

  // Adaptive mode
  bool enable_adaptive_trigger;
};
```

Store:

- `std::atomic<LIPPIndex*> lipp_ptr;` (published, read-only during normal operation)
- `DPGMIndex dp_active;`
- `std::atomic<DPGMIndex*> dp_frozen_ptr;` (null if no flush pending)
- A lightweight **epoch manager** for safe reclamation (described below)
- Atomic counters:
  - `insert_count`, `lookup_count` (for adaptation)
  - `active_keys`, `frozen_keys`, `lipp_keys`

**Do not store any additional persistent map/hash/bloom filter.** Temporary vectors used only during rebuild/merge are okay but must be freed and must not become persistent indexing structures (to avoid violating “no auxiliary data structures”). citeturn6search13turn3view3  

### Fast-path operations

#### Insert fast path: DPGM-only + cheap trigger check

Algorithm:

1. Insert into `dp_active` using the existing DPGM insertion method.
2. Increment `insert_count`.
3. If `dp_active.size() >= trigger_threshold()` and `dp_frozen_ptr == nullptr`:
   - Atomically “swap” buffers:
     - Move `dp_active` into a heap-allocated `DPGMIndex* new_frozen`.
     - Replace `dp_active` with a fresh empty DPGM instance (same config).
     - Publish `dp_frozen_ptr = new_frozen` with release semantics.
   - Start (or wake) flush worker if background enabled.

This is effectively the “double-buffered delta index” idea the assignment hints at, and mirrors the “freeze buffer then compact” approach used in concurrent learned-index designs. citeturn9view0turn9view2  

Correctness note: **Never clear the frozen buffer until after the LIPP publish + grace period** (detailed below).

#### Lookup fast path: range-gated delta checks then LIPP

Order for membership/value retrieval:

1. Check `dp_active` (always).
2. Load `dp_frozen_ptr` once (acquire). If non-null, check `dp_frozen`.
3. If not found, search published LIPP via `lipp_ptr` (acquire).

To minimize overhead, add **range gating** for each delta buffer:

- Maintain `min_key` and `max_key` for `dp_active` and `dp_frozen` (updated during inserts; for `dp_frozen` computed once when frozen).
- If query key < min_key or > max_key, skip searching that delta buffer (constant-time check).
- This reduces wasted delta searches for negative lookups and for most existing-key lookups when delta is small. This is important because “search both index and buffers” is a known throughput killer for delta-buffer designs. citeturn7view0turn9view2  

#### Cooperative flush work (optional but recommended)
To avoid oversubscribing cores, support a mode where **each insert/lookup contributes a small deterministic amount of flush work** if a frozen buffer exists, e.g., `flush_work_budget_per_op` keys migrated per operation. This mirrors incremental compaction ideas and avoids having a background thread steal CPU from throughput-critical benchmark threads on fully utilized cores. citeturn6search0turn9view2  

### Flush engine: subtree rebuild migration + RCU-safe publish

This is the heart of Milestone 3. The design goal is to make flush cheaper than “extract keys and call LIPP::insert repeatedly” (Milestone 2 naive), and to avoid blocking lookups/inserts.

#### Step 1: Extract and sort frozen keys in chunks

From `dp_frozen`, repeatedly extract a **sorted run** of up to `CHUNK = rebuild_target_max_keys` keys.

- If DPGM already stores sorted arrays by level, implement an iterator to produce keys in sorted order without extra indexing structures.
- If easiest, materialize a `std::vector<Key>` and sort for that chunk only. This is transient scratch space, not a persistent index.

Aim to process in chunks to bound rebuild time and memory.

#### Step 2: Choose the LIPP subtree rebuild target for each chunk

For a given chunk’s key range `[k_first, k_last]`, choose a target subtree root `T` in LIPP such that:

- `T`’s key range fully covers the chunk range (or you split the chunk by multiple targets).
- `T`’s total element count is within `rebuild_target_max_keys` to avoid huge rebuilds (consistent with LIPP’s own guidance to restrict large adjustments). citeturn3view1turn3view2  

Implementation choices (Codex should implement in order of complexity):

**Option A (recommended): rebuild at a fixed depth under the LIPP root**
- During initial bulkload, ensure LIPP root has reasonably high fanout (it already uses an array of entries and a model). citeturn2view1turn3view3  
- For each key, compute its root slot (using root model) and bucket chunk keys by root-slot id.  
- Rebuild each affected child subtree independently (this resembles XIndex’s notion of range-partitioned “groups,” but implemented entirely within LIPP’s tree structure). citeturn9view0turn9view2  

**Option B: dynamic target selection via “lookup path sampling”**
- For the chunk’s first key, run a LIPP lookup that records the traversal path (node pointers + slot indices).
- Select the highest node on that path whose subtree size ≤ `rebuild_target_max_keys`.
- Use node metadata `.element_num` if available (LIPP keeps such statistics for adjustment decisions). citeturn3view1turn3view2  

Codex must implement at least Option A; Option B is a refinement if the codebase exposes element counts.

#### Step 3: Collect existing keys from the target subtree

Implement a LIPP internal API:

- `void CollectKeys(Node* root, std::vector<KeyValue>& out_sorted)`

It must traverse:
- For each entry of `root`:
  - If type == DATA: append its key/payload.
  - If type == NODE: recurse on child.
  - If NULL: skip.
- The traversal order must preserve global key order (the tree is sorted), matching LIPP’s statement that subtree keys are in order after sequential traversal. citeturn3view2turn3view3  

Optimization requirements:
- Use the node’s bitvector/type array to skip NULL ranges quickly if possible (LIPP uses bitvectors for gap skipping in scans). citeturn3view0turn2view1  
- Avoid per-entry heap allocation; reuse vectors, reserve capacity.

#### Step 4: Merge “existing subtree keys” with “flush chunk keys”

Now you have:
- `A`: sorted key list from subtree `T`.
- `B`: sorted chunk keys being migrated.

Merge into `C = merge(A, B)` in linear time, de-duplicating if needed.

De-duplication rule:
- If keys are unique by workload, duplicates should not occur. If duplicates can occur, decide a stable resolution policy (e.g., the most recent payload wins—i.e., chunk B overwrites A). This is consistent with delta-buffer semantics. (If your project workload never inserts duplicates, you can assert and skip overhead.)

#### Step 5: Build a fresh subtree with LIPP’s BuildPartialTree / bulkload logic

Expose a builder:

- `Node* BuildPartialTree(const KeyValue* begin, const KeyValue* end, BuildParams params)`

This should be derived from LIPP Algorithm 5 (“partial tree building”) which:
- creates a node with `num_entries = α * num_keys` to leave gaps
- learns the node model using FMCD (Fastest Minimum Conflict Degree)
- places keys; recursively builds child nodes for conflicts
- initializes node statistics for future adjustments citeturn3view3turn2view0turn6search2  

Codex must follow these constraints:
- `α` (gap factor) should default to ~2 (as in the paper’s example), but allow tuning. citeturn3view3  
- Cap node size similar to LIPP’s discussion (it references a node size upper bound such as 16MB, borrowed from ALEX design considerations). Use the existing LIPP constants if present. citeturn3view2turn0search6  

This builder should be used for **flush integration**, not `LIPP::insert` in a loop.

#### Step 6: Publish the rebuilt subtree pointer without stopping readers

Implement a publish API:

- `void ReplaceChild(Node* parent, int slot, Node* new_child)`

Publication must be:
- a single pointer store with release semantics (or CAS if needed), replacing the old child pointer.
- immediately safe for new readers: after publication, future lookups may traverse into new subtree.

Correctness requirement during publication:
- The new subtree is a **superset** of the old subtree’s keys (plus migrated keys). Therefore, whether a concurrent reader sees old or new child, it will still find any old key. Newly migrated keys must still remain in `dp_frozen` until reclamation is safe (next step), preventing misses if a reader sees an old subtree. citeturn6search13turn9view1  

#### Step 7: RCU-style grace period and reclamation

You must prevent use-after-free of old LIPP nodes and prevent key loss when finally clearing `dp_frozen`.

Implement a simple epoch system:

- Global `std::atomic<uint64_t> global_epoch;`
- Per-thread `thread_local uint64_t local_epoch;` and `thread_local bool active;`
- `EnterLippRead()` sets `active=true`, `local_epoch=global_epoch.load(acquire)`
- `ExitLippRead()` sets `active=false`

When publishing a new subtree (or a set of subtree replacements):
1. Increment `global_epoch` (release) after all pointer publishes are done.
2. Add old subtree roots to a retire list with the publish epoch.
3. Periodically, flush thread checks whether all threads either:
   - are inactive, or
   - have `local_epoch >= retire_epoch`
4. Once true, it is safe to delete retired old nodes and, crucially, to remove the corresponding migrated keys from `dp_frozen`.

This matches the RCU idea of “removal/publish” vs “reclamation after readers finish.” citeturn6search13turn9view1  

Implementation simplification if multi-threading is not used in your benchmark:
- You can compile-time disable epoch tracking (fast path), but only if the benchmark truly runs single-thread. Otherwise keep it.

#### Step 8: Clearing the frozen DPGM safely

Only after all migrated keys are guaranteed visible in LIPP for all readers (grace period passed) can you:

- delete `dp_frozen_ptr` and set it to null, or
- clear/reuse it as the next frozen buffer.

Never clear earlier.

### Adaptive trigger policy to win both workloads

A fixed “flush at 5%” policy may win lookup-heavy but lose insert-heavy due to compaction CPU contention (or vice versa). The evaluation literature explicitly notes learned index performance is sensitive to workload characteristics and delta-buffer overhead. citeturn7view0turn1view2  

Implement a lightweight adaptive trigger:

- Maintain rolling counters in atomics: `ins`, `look`.
- Compute `write_ratio = ins / (ins + look)` over a window (e.g., every 50k ops).
- If `write_ratio >= 0.7` (insert-heavy):
  - increase `frozen_trigger_frac`
  - reduce/disable background flush thread (or reduce budget)
  - goal: match/beat vanilla DPGM insertion throughput by avoiding migration overhead
- If `write_ratio <= 0.3` (lookup-heavy):
  - decrease `frozen_trigger_frac` (flush more often so delta stays tiny)
  - increase migration budget to keep delta minimal
  - goal: approach LIPP-like lookup throughput while avoiding LIPP’s per-insert churn

This is consistent with the general finding that DPGM tends to do well in write-heavy settings and LIPP in lookup-heavy settings (no range queries), so an adaptive hybrid should dominate both. citeturn7view0  

### DPGM hyperparameters and search method selection inside the hybrid

Codex must expose the DPGM `epsilon` and `search_method` for the delta buffers and sweep them. Use the same choices that the evaluation framework supports (binary, exponential, interpolation, etc.). citeturn6search3turn7view0  

Guidance:
- Smaller `epsilon` typically shrinks the final search interval but increases model/segment overhead; larger `epsilon` reduces model size but increases refinement range. Your delta DPGM buffer will usually be small, so the optimal `epsilon` may differ from vanilla DPGM tuned for a huge structure.
- Exponential search is often strong when the true position is near the prediction and the refinement window is small-to-medium, because it quickly finds bounds then binary-searches within them; this is why multiple search methods exist in modern learned-index testbeds. citeturn6search3turn7view0  

Codex should implement a configuration system so benchmarks can instantiate multiple hybrid variants.

## Throughput-oriented tuning protocol for “must beat both baselines”

### What to sweep

For each dataset (Books, FB, OSMC) and each workload (10% insert, 90% insert), sweep:

- `frozen_trigger_frac`: {0.5%, 1%, 2%, 5%, 10%}
- `rebuild_target_max_keys`: {16k, 64k, 256k}
- `lipp_gap_factor α`: {1.5, 2.0, 3.0} (bounded by node-size constraints) citeturn3view3  
- `dpgm_epsilon`: reuse baseline candidate epsilons + a few smaller values
- `dpgm_search_method`: try at least {binary, exponential, interpolation} citeturn6search3turn7view0  
- Flush scheduling:
  - background thread on/off
  - cooperative budget per operation: {0, 1, 4, 16 migrated keys/op}

### How to decide “winner settings” without overfitting

Per dataset/workload:
- Compare average throughput (≥3 runs, same session as required).
- Validate “no keys left behind” by:
  - Counting total inserted keys
  - Running a verification pass after workload: sample or full-check that every inserted key is findable (in at least one structure).
- Report index size:
  - Only count current live LIPP + live DPGM buffers. Retired nodes must be reclaimed; otherwise you will inflate size and may lose points.

### Guardrails that prevent common performance failures

- If lookup-heavy throughput is below vanilla LIPP:
  - delta is too large or too costly to search → lower flush threshold and/or tighten range gating; ensure delta checks are cheap and frequently skipped for keys outside delta range. citeturn9view2turn7view0  
- If insert-heavy throughput is below vanilla DPGM:
  - flush CPU contention is harming inserts → disable background flush, rely on large thresholds, or use cooperative micro-budget so inserts never stall; this is consistent with deferring merges in LSM-like systems to protect ingestion throughput. citeturn6search0turn5view2  
- If flush dominates time:
  - you are rebuilding too large a subtree too often → reduce rebuild granularity or rebuild at a deeper level (smaller subtrees). LIPP itself warns that adjusting an overly large node (e.g., root) may be unacceptable, motivating size caps. citeturn3view1turn3view2  

## Why this approach is likely to outperform vanilla DPGM and LIPP

This design is specifically built to avoid the two classic “hybrid learned index” pitfalls called out in the literature:

- **Pitfall: every lookup always searches the delta buffer first**, inflating lookup latency.  
  Mitigation: keep delta buffers small via adaptive flush in lookup-heavy mode, and add constant-time min/max range gating to skip delta searches aggressively for most keys and negative lookups. This speaks directly to the delta-buffer overhead critique in concurrency/compaction discussions. citeturn9view2turn7view0  

- **Pitfall: compaction blocks or contends heavily with foreground operations**, destroying throughput.  
  Mitigation: double-buffer DPGM (freeze & swap) and publish rebuilt LIPP subtrees with RCU-like grace periods. XIndex’s two-phase compaction and explicit RCU barrier approach demonstrates why this matters and how to do it without blocking readers/writers. citeturn9view0turn9view1turn6search13  

The key “flush acceleration” commitment—**subtree rebuild instead of per-key insertion**—is grounded in LIPP’s own design. LIPP rebuilds a subtree by collecting keys and calling BuildPartialTree (Algorithm 5) when expansion/conflicts justify it; bulkload is explicitly described as following the same partial-tree procedure. Using exactly this machinery for DPGM→LIPP migration is the most direct way to reduce flush cost while retaining LIPP’s lookup advantages. citeturn3view1turn3view3turn2view0  

Finally, the hybrid’s adaptive behavior aligns with broad evaluation findings: delta-buffer approaches help inserts, in-place approaches help reads, and different learned indexes dominate in different workload regimes (e.g., DPGM in write-heavy, LIPP in lookup-only/point-query regimes). A hybrid that dynamically shifts flush aggressiveness to keep the “right” structure dominant in each regime is one of the few realistic paths to “beat both” across both mixed workloads. citeturn7view0