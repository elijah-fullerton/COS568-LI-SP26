# Milestone 3 Edit Prompt

Work only in `/auto/u/ef0952/projects/COS568-LI-SP26`.

You are responsible for exactly one bounded Milestone 3 candidate edit for the
hybrid learned index. Do not run the benchmark submission scripts yourself. The
outer orchestration loop will do that after you exit.

Before editing, read:

- `autoresearch/m3_program.md`
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

When finished:

- leave the code edits in the working tree
- write a short summary of the candidate idea and expected effect
- stop
