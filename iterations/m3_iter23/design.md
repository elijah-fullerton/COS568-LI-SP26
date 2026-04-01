# Iteration 23 Design

## Goal

Keep the iteration-22 compliant architecture, but eliminate the pathological
parameter choices that made the Facebook screen time out.

## Change

The hybrid code remains owner-buffered LIPP+DPGM. The parameter sweep is changed
as follows:

- lookup-heavy workload: start at owner sizes `128`, `256`, `512`
- insert-heavy workload: start at owner sizes `512`, `1024`, `2048`
- remove the tiny-owner `64` configuration entirely

Flush thresholds are also scaled up with the owner size, avoiding the worst
case where the hybrid performs many tiny flushes into LIPP.

## Hypothesis

- Larger owners should reduce the number of owner-local buffers and metadata
  lookups.
- Less aggressive flushing should better amortize DPGM-to-LIPP migration cost.
- This should at least make the compliant branch benchmarkable on Facebook,
  allowing us to determine whether the architecture is merely slower than LIPP
  or whether it is still competitive on insert-heavy workloads.
