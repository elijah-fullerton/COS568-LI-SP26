# Milestone 3 Edit Prompt

Work only in `/auto/u/ef0952/projects/COS568-LI-SP26`.

You are responsible for exactly one bounded Milestone 3 candidate edit for the
hybrid learned index. Do not run the benchmark submission scripts yourself. The
outer orchestration loop will do that after you exit.

Before editing, read:

- `autoresearch/m3_program.md`
- `autoresearch/current_blocker.md`
- `autoresearch/current_status.json`
- `autoresearch/reward_state.json`
- `autoresearch/loop_state.json`
- `autoresearch/results.tsv`
- `autoresearch/preflight.tsv`

If `autoresearch/incumbent_stage/` exists, treat it as the current accepted
baseline. The outer loop may already have restored it into the working tree.

Scope:

- edit only the Milestone 3 implementation files described in
  `autoresearch/m3_program.md`
- make one coherent improvement only
- keep the parameter sweep small
- choose the edit layer based on the blocker, not just the last file edited

Do not:

- touch unrelated files
- run the full autoresearch loop
- stop after a screen-only result
- introduce forbidden persistent auxiliary structures outside LIPP and DPGM

Focus:

- reducing lookup miss overhead
- reducing flush disruption
- avoiding tiny owner regions and over-aggressive flush thresholds
- keeping the design milestone-3 compliant
- restoring measurability before optimizing throughput when a run yields no usable result

Escalation rules:

- First repeated failure: fix the same layer only if the failure is obviously local.
- Second similar failure: inspect the harness or screening setup before changing the core design again.
- Third similar failure: change strategy or experiment structure, not just parameters.
- If two consecutive screen runs time out without a `RESULT` line, the next edit
  should target screening, sweep ordering, or measurement reliability rather
  than another micro-tweak inside `hybrid_pgm_lipp.h`.

When finished:

- leave the code edits in the working tree
- write a short summary of the candidate idea and expected effect
- stop
