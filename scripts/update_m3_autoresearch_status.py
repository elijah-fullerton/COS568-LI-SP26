#!/usr/bin/env python3

import argparse
import csv
import json
from collections import Counter
from pathlib import Path
from typing import Dict, List


PROGRAM_BRIEF = [
    "Goal: improve Milestone 3 HybridPGMLIPP against DynamicPGM, LIPP, and the Milestone 2 naive hybrid.",
    "Primary workloads: mixed 10% insert / 90% lookup and 90% insert / 10% lookup.",
    "Primary datasets: fb_100M_public_uint64, books_100M_public_uint64, osmc_100M_public_uint64.",
    "Workflow: make one bounded candidate edit, let the outer loop stage and benchmark it, then stop.",
    "Priority: restore measurability first when runs fail before producing useful results.",
]

FAILURE_DOC_PATH = "docs/autoresearch_rl_failure_analysis_and_literature_review.md"


def repo_root_default() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=str(repo_root_default()))
    return parser.parse_args()


def read_tsv(path: Path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="ascii") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def read_jsonl(path: Path) -> List[Dict[str, object]]:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="ascii").splitlines():
        if line.strip():
            rows.append(json.loads(line))
    return rows


def summarize_recent_context(trajectory: List[Dict[str, object]]) -> List[str]:
    lines = ["# Current Context", ""]
    if not trajectory:
        lines.extend(["No completed trajectory entries recorded yet.", ""])
        return lines

    keeps = [row for row in trajectory if row.get("status") == "keep"]
    if keeps:
        best = max(keeps, key=lambda row: float(row.get("full", {}).get("reward", -1e9)))
        candidate = best.get("candidate", {})
        lines.extend(
            [
                "## Best Kept Candidate",
                f"- Iteration: `{best.get('iteration', 'n/a')}`",
                f"- Reward: `{best.get('full', {}).get('reward', 'n/a')}`",
                f"- Mutation family: `{candidate.get('mutation_family', 'n/a')}`",
                f"- Changed files: `{', '.join(candidate.get('changed_files', [])) or 'n/a'}`",
                "",
            ]
        )

    lines.append("## Recent Experiments")
    for row in trajectory[-5:]:
        candidate = row.get("candidate", {})
        full = row.get("full", {})
        screen = row.get("screen", {})
        lines.append(
            "- "
            f"{row.get('iteration', 'n/a')}: status={row.get('status', 'n/a')}, "
            f"family={candidate.get('mutation_family', 'n/a')}, "
            f"novelty={candidate.get('novelty_score', 'n/a')}, "
            f"screen={screen.get('failure_class', 'n/a')}, "
            f"reward={full.get('reward', 'n/a')}"
        )
    lines.append("")
    return lines


def compact_changed_files(changed_files: object, limit: int = 3) -> str:
    if not isinstance(changed_files, list) or not changed_files:
        return "n/a"
    trimmed = [str(item) for item in changed_files[:limit]]
    if len(changed_files) > limit:
        trimmed.append("...")
    return ", ".join(trimmed)


def compact_recent_experiments(trajectory: List[Dict[str, object]], limit: int = 3) -> List[str]:
    lines: List[str] = []
    for row in trajectory[-limit:]:
        candidate = row.get("candidate", {})
        screen = row.get("screen", {})
        full = row.get("full", {})
        lines.append(
            "- "
            f"{row.get('iteration', 'n/a')}: status={row.get('status', 'n/a')}, "
            f"family={candidate.get('mutation_family', 'n/a')}, "
            f"screen={screen.get('failure_class', 'n/a')}, "
            f"reward={full.get('reward', 'n/a')}, "
            f"files={compact_changed_files(candidate.get('changed_files', []))}"
        )
    if not lines:
        lines.append("- No completed experiments recorded yet.")
    return lines


def build_compact_prompt(
    repo_root: Path,
    status: Dict[str, object],
    loop_state: Dict[str, object],
    trajectory: List[Dict[str, object]],
    reward_state: Dict[str, object],
    mutation_policy: Dict[str, object],
) -> str:
    editable_paths = mutation_policy.get("editable_paths", [])
    recent_lines = compact_recent_experiments(trajectory)
    keeps = [row for row in trajectory if row.get("status") == "keep"]
    best_keep = None
    if keeps:
        best_keep = max(keeps, key=lambda row: float(row.get("full", {}).get("reward", -1e9)))

    lines = [
        "# Milestone 3 Compact Edit Prompt",
        "",
        "Work only in the current repo checkout. Implement exactly one bounded candidate edit and stop.",
        "Do not submit benchmarks yourself; the outer loop will do that after you exit.",
        "",
        "Minimal read set before editing:",
        "- `autoresearch/current_blocker.md`",
        "- `autoresearch/current_status.json`",
        "- `autoresearch/mutation_policy.json`",
        f"- `{FAILURE_DOC_PATH}` when the blocker is strategy, screening, or measurement",
        "- inspect only the specific source files you plan to change",
        "",
        "Program brief:",
    ]
    lines.extend(f"- {item}" for item in PROGRAM_BRIEF)
    lines.extend(
        [
            "",
            "Current state:",
            f"- Incumbent iteration: `{loop_state.get('incumbent_iteration', '') or 'none'}`",
            f"- Last completed iteration: `{loop_state.get('last_completed_iteration', '') or 'none'}`",
            f"- Dominant failure class: `{status.get('dominant_failure_class', 'none') or 'none'}`",
            f"- Recommended edit layer: `{status.get('recommended_edit_layer', 'implementation')}`",
            f"- Consecutive non-advancing iterations: `{status.get('consecutive_non_advancing_iterations', 0)}`",
            f"- Low novelty streak: `{status.get('low_novelty_streak', 0)}`",
            f"- Best tracked hybrid throughput keys: `{len(reward_state.get('best_hybrid_throughput', {}))}`",
            "",
            "Allowed edit targets:",
        ]
    )
    lines.extend(f"- `{path}`" for path in editable_paths[:8])
    if len(editable_paths) > 8:
        lines.append("- `...`")
    lines.extend(
        [
            "",
            "Recent experiments:",
        ]
    )
    lines.extend(recent_lines)
    if best_keep is not None:
        candidate = best_keep.get("candidate", {})
        lines.extend(
            [
                "",
                "Best kept reference:",
                f"- Iteration: `{best_keep.get('iteration', 'n/a')}`",
                f"- Reward: `{best_keep.get('full', {}).get('reward', 'n/a')}`",
                f"- Mutation family: `{candidate.get('mutation_family', 'n/a')}`",
                f"- Changed files: `{compact_changed_files(candidate.get('changed_files', []), limit=4)}`",
            ]
        )
    lines.extend(
        [
            "",
            "Rules:",
            "- Make one coherent improvement only.",
            "- Prefer the failure-local fix over another core-design micro-tweak.",
            "- Keep sweeps small and measurable.",
            "- If recent runs failed before usable results, prioritize measurability and robustness over raw throughput tuning.",
            "- Do not touch unrelated files or broaden scope beyond the allowed paths.",
            "",
            "When finished:",
            "- leave edits in the working tree",
            "- write a short summary of the candidate idea and expected effect",
            "- stop",
            "",
        ]
    )
    return "\n".join(lines)


def write_status_files(repo_root: Path) -> None:
    autoresearch = repo_root / "autoresearch"
    preflight = read_tsv(autoresearch / "preflight.tsv")
    results = read_tsv(autoresearch / "results.tsv")
    trajectory = read_jsonl(autoresearch / "trajectory.jsonl")
    loop_state_path = autoresearch / "loop_state.json"
    mutation_policy_path = autoresearch / "mutation_policy.json"
    reward_state_path = autoresearch / "reward_state.json"
    loop_state = {}
    mutation_policy = {}
    if loop_state_path.exists():
        loop_state = json.loads(loop_state_path.read_text(encoding="ascii"))
    if mutation_policy_path.exists():
        mutation_policy = json.loads(mutation_policy_path.read_text(encoding="ascii"))
    reward_state = {}
    if reward_state_path.exists():
        reward_state = json.loads(reward_state_path.read_text(encoding="ascii"))

    recent_preflight = preflight[-5:]
    recent_trajectory = trajectory[-5:]
    failure_counter = Counter(
        row.get("failure_class", "") for row in recent_preflight if row.get("failure_class")
    )
    dominant_failure_class = ""
    if failure_counter:
        dominant_failure_class = failure_counter.most_common(1)[0][0]

    screen_timeout_streak = 0
    for row in reversed(preflight):
        if row.get("failure_class") == "screen_timeout_no_result":
            screen_timeout_streak += 1
        else:
            break

    consecutive_non_advancing = 0
    for row in reversed(results):
        if row.get("status") == "keep":
            break
        consecutive_non_advancing += 1
    if not results:
        consecutive_non_advancing = screen_timeout_streak

    low_novelty_streak = 0
    self_eval_abort_streak = 0
    for row in reversed(recent_trajectory):
        candidate = row.get("candidate", {})
        if float(candidate.get("novelty_score", 1.0)) <= 0.35:
            low_novelty_streak += 1
        else:
            break
    for row in reversed(recent_trajectory):
        full = row.get("full", {})
        if full.get("self_eval_abort"):
            self_eval_abort_streak += 1
        else:
            break

    recent_families = Counter(
        row.get("candidate", {}).get("mutation_family", "")
        for row in recent_trajectory
        if row.get("candidate", {}).get("mutation_family")
    )
    dominant_family = recent_families.most_common(1)[0][0] if recent_families else ""

    recommended_edit_layer = "implementation"
    recommendations = []
    if low_novelty_streak >= 2:
        recommended_edit_layer = "strategy"
        recommendations.extend(
            [
                "Recent experiments are repeating the same edit signature or mutation family.",
                "Do not spend the next iteration on another near-duplicate patch.",
                "Switch mutation family or broaden the diagnostic path before trying again.",
            ]
        )
    elif self_eval_abort_streak >= 2:
        recommended_edit_layer = "screening"
        recommendations.extend(
            [
                "Recent full runs were aborted early by the self-evaluator for trailing the incumbent.",
                "Tighten screening or choose a different design move before spending another full run.",
            ]
        )
    elif screen_timeout_streak >= 6:
        recommended_edit_layer = "design_family"
        recommendations.extend(
            [
                "Six consecutive screen timeouts with no RESULT line detected.",
                "Pivot toward a measurability-restoring design change: drastically cheaper canary behavior, smaller screen workload, or a different compliant design family.",
            ]
        )
    elif screen_timeout_streak >= 2:
        recommended_edit_layer = "screening"
        recommendations.extend(
            [
                "Do not make another hybrid-internal micro-tweak before changing the screen harness or sweep.",
                "Prioritize screening or measurement reliability over core-design tuning.",
            ]
        )
    elif dominant_failure_class in {"screen_timeout_no_result", "screen_timeout_partial"}:
        recommended_edit_layer = "measurement"
        recommendations.extend(
            [
                "The current blocker is measurement reliability, not throughput optimization.",
                "Choose the next edit that maximizes diagnostic information per unit time.",
            ]
        )
    elif dominant_failure_class == "screen_failure_no_result":
        recommended_edit_layer = "screening"
        recommendations.extend(
            [
                "The screen is failing before it emits a measurable result.",
                "Inspect build logs, launcher assumptions, and archived benchmark status before changing HybridPGMLIPP again.",
            ]
        )
    elif consecutive_non_advancing >= 3:
        recommended_edit_layer = "strategy"
        recommendations.extend(
            [
                "Three consecutive non-advancing iterations detected.",
                "Shift strategy: inspect the harness, sweep, or failure attribution layer before touching the core design again.",
            ]
        )
    else:
        recommendations.extend(
            [
                "No dominant blocker currently detected.",
                "Continue with one bounded change, but use the recent-context summary to avoid duplicates.",
            ]
        )

    assumptions = []
    if screen_timeout_streak >= 1:
        assumptions.append(
            "Recent screen jobs are timing out before the first RESULT line, so the first screened candidate is likely pathological."
        )
    if low_novelty_streak >= 1:
        assumptions.append(
            "Recent mutations are reusing the same file set or design layer and may be under-exploring the search space."
        )
    if dominant_failure_class == "screen_failure_no_result":
        assumptions.append(
            "At least one recent no-result screen may actually be a harness or build failure rather than a benchmark-level regression."
        )
    if not reward_state.get("best_hybrid_throughput"):
        assumptions.append(
            "No full six-workload run has populated the incumbent reward state in this clone yet."
        )

    status = {
        "consecutive_non_advancing_iterations": consecutive_non_advancing,
        "dominant_failure_class": dominant_failure_class,
        "dominant_mutation_family": dominant_family,
        "low_novelty_streak": low_novelty_streak,
        "recent_preflight_count": len(recent_preflight),
        "recent_result_count": len(results[-5:]),
        "recommended_edit_layer": recommended_edit_layer,
        "screen_timeout_streak": screen_timeout_streak,
        "self_eval_abort_streak": self_eval_abort_streak,
    }
    (autoresearch / "current_status.json").write_text(
        json.dumps(status, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )

    lines = [
        "# Current Blocker",
        "",
        f"- Dominant failure class: `{dominant_failure_class or 'none'}`",
        f"- Dominant mutation family: `{dominant_family or 'none'}`",
        f"- Screen timeout streak: `{screen_timeout_streak}`",
        f"- Self-eval abort streak: `{self_eval_abort_streak}`",
        f"- Low novelty streak: `{low_novelty_streak}`",
        f"- Consecutive non-advancing iterations: `{consecutive_non_advancing}`",
        f"- Recommended edit layer: `{recommended_edit_layer}`",
        "",
        "## Assumptions",
    ]
    if assumptions:
        lines.extend([f"- {item}" for item in assumptions])
    else:
        lines.append("- No active assumptions recorded.")
    lines.extend(["", "## Recommendations"])
    lines.extend([f"- {item}" for item in recommendations])
    lines.extend(
        [
            "",
            "## Escalation Rules",
            "- First repeated failure: fix the same layer if the failure is obvious and local.",
            "- Second similar failure: inspect the harness or measurement setup before changing the core design again.",
            "- Third similar failure: change search strategy or experimental structure, not just parameters.",
            "- Low novelty on consecutive iterations: switch mutation family or changed-file scope before trying another micro-variation.",
            "- If a run produces no usable measurement, prioritize restoring observability before optimizing throughput.",
        ]
    )
    (autoresearch / "current_blocker.md").write_text("\n".join(lines) + "\n", encoding="ascii")
    (autoresearch / "current_context.md").write_text(
        "\n".join(summarize_recent_context(trajectory)) + "\n",
        encoding="ascii",
    )
    (autoresearch / "codex_edit_prompt.md").write_text(
        build_compact_prompt(
            repo_root=repo_root,
            status=status,
            loop_state=loop_state,
            trajectory=trajectory,
            reward_state=reward_state,
            mutation_policy=mutation_policy,
        ),
        encoding="ascii",
    )


def main() -> None:
    args = parse_args()
    write_status_files(Path(args.repo_root))


if __name__ == "__main__":
    main()
