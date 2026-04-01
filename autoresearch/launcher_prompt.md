# Milestone 3 Launcher Prompt

Use this prompt when launching an autonomous coding agent inside
`/auto/u/ef0952/projects/COS568-LI-SP26`.

```text
Work only on Milestone 3 HybridPGMLIPP autoresearch.

Start by reading:
- autoresearch/m3_program.md
- autoresearch/reward_state.json
- autoresearch/loop_state.json

Treat autoresearch/incumbent_stage as the incumbent candidate when it exists.
Before making a new candidate, restore the incumbent if needed.

Use this loop indefinitely:

1. Propose and implement one bounded Milestone 3 improvement in the allowed files.
2. Stage it with scripts/stage_m3_autoresearch_iteration.sh.
3. Run scripts/run_m3_autoresearch_loop.py --iterations 1 --promote-screen always
4. Read autoresearch/results.tsv and autoresearch/reward_state.json.
5. If the candidate was discarded, restore the incumbent and try a different idea.
6. Never stop on your own. Keep iterating until interrupted.

Rules:
- Do not edit unrelated files.
- Do not conclude an experiment without a full six-workload result.
- Prefer ideas that reduce lookup miss overhead or flush disruption.
- Avoid tiny owner partitions and over-aggressive flush thresholds.
```

For unattended operation without an already-open interactive agent, prefer the
Codex CLI hook:

```bash
bash scripts/run_m3_autoresearch_codex.sh
```

To keep it running after you disconnect from the terminal, prefer the tmux
launcher:

```bash
bash scripts/start_m3_autoresearch_tmux.sh
```

Useful tmux commands:

```bash
tmux attach -t m3-autoresearch
bash scripts/stop_m3_autoresearch.sh
```
