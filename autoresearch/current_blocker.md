# Current Blocker

- Dominant failure class: `screen_timeout_no_result`
- Screen timeout streak: `4`
- Consecutive non-advancing iterations: `4`
- Recommended edit layer: `screening`

## Assumptions
- Recent screen jobs are timing out before the first RESULT line, so the first screened candidate is likely pathological.
- No full six-workload run has completed yet, so reward and incumbent state remain uninitialized.

## Recommendations
- Do not make another hybrid-internal micro-tweak before changing the screen harness or sweep.
- Prioritize `benchmarks/benchmark_hybrid_pgm_lipp.cc` or the screen compute script.
- Restore measurability first: isolate safer variants and maximize the chance of emitting at least one RESULT line.

## Escalation Rules
- First repeated failure: fix the same layer if the failure is obvious and local.
- Second similar failure: inspect the harness or measurement setup before changing the core design again.
- Third similar failure: change search strategy or experimental structure, not just parameters.
- If a run produces no usable measurement, prioritize restoring observability before optimizing throughput.
