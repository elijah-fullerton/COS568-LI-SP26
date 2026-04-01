# Iteration 22 Failure Analysis

## Outcome

Iteration 22 restored milestone-3 compliance by removing the non-compliant
overlay, Bloom-filter, and copied-key structures. However, the Facebook screen
run timed out before completing even the first mixed-workload benchmark.

## Evidence

- Slurm job: `2687155`
- Archive: `slurm_runs/m3_iter22`
- Exit status: `124`
- The benchmark emitted no `RESULT` line before timeout.

## Root cause

The failure was not a correctness crash or a benchmark verification bug. The
throughput path in `benchmark.h` does not enable per-operation verification.
Instead, the problem was a pathological parameter choice for the compliant
owner-buffered design.

Local reproduction on a tiny Facebook-shaped workload showed:

- `BinarySearch-e64-s64-f8` was catastrophically slow
- larger-owner variants completed normally

This indicates that making owner regions too small creates too many DPGM
buffers and too much routing/flush overhead. On the real Facebook workload, the
first screened configuration was slow enough to consume the full 25-minute
timeout before the benchmark reached the remaining variants.

## Implication for the next iteration

The owner-buffered design remains viable as a compliant baseline, but the sweep
must avoid tiny owner regions. The next iteration should:

1. remove the `s64-f8` style configurations
2. bias toward larger owner regions and less aggressive flush thresholds
3. rescreen on Facebook before attempting full multi-dataset runs
