# Iteration 22 Design

## Goal

Recover strict milestone 3 compliance and then test whether a compliant hybrid
can still beat both baselines.

## Why the previous design failed

The prior approach was fast because it cached flushed keys in auxiliary
structures:

- immutable per-bucket sorted vectors
- per-bucket Bloom filters
- a global Bloom filter
- published overlay LIPP replicas

Those structures reduced negative lookup cost, but they are outside the allowed
spec. Even if the throughput is strong, the result is invalid for milestone 3.

## New compliant design

This iteration replaces the overlay design with an owner-buffered hybrid:

- Bulk-loaded keys stay in one base LIPP.
- The bulk-loaded LIPP is frozen structurally and partitioned into owner
  regions.
- Each owner region gets one DPGM buffer.
- A lookup first probes LIPP, then probes only the owner-local DPGM buffer.
- An insert goes only to the owner-local DPGM buffer.
- When the owner-local DPGM reaches its threshold, the buffered keys are
  inserted into LIPP and the DPGM is reset.

## Why this may work

- Lookup-heavy workloads should stay close to LIPP because only one owner-local
  DPGM is probed on a miss.
- Insert-heavy workloads should still benefit because many recent inserts land
  in DPGM first rather than paying LIPP conflict handling immediately.
- The owner partitioning reduces the penalty of a global mutable buffer, since
  the benchmark touches only one local DPGM per point query.

## Risks

- Flushing by repeated `lipp_.insert(...)` may still be too expensive to beat
  LIPP on lookup-heavy workloads.
- The owner metadata added to LIPP may slightly slow the base lookup path.
- If owner regions are too large, DPGM miss cost grows; if too small, flushes
  become too frequent.

## Experiment plan

1. Run a Facebook hybrid-only screen with several owner sizes and flush
   thresholds for both mixed workloads.
2. If one configuration is competitive, run the full three-dataset screen.
3. If no compliant configuration is competitive, the next iteration should
   restore asynchronous behavior without auxiliary structures, likely with
   double-buffered per-owner DPGMs.
