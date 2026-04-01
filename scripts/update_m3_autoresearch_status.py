#!/usr/bin/env python3

import csv
import json
from collections import Counter
from pathlib import Path


def read_tsv(path: Path):
    if not path.exists():
        return []
    with path.open(newline="", encoding="ascii") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def write_status_files(repo_root: Path) -> None:
    autoresearch = repo_root / "autoresearch"
    preflight = read_tsv(autoresearch / "preflight.tsv")
    results = read_tsv(autoresearch / "results.tsv")
    reward_state_path = autoresearch / "reward_state.json"
    reward_state = {}
    if reward_state_path.exists():
        reward_state = json.loads(reward_state_path.read_text(encoding="ascii"))

    recent_preflight = preflight[-5:]
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

    recommended_edit_layer = "implementation"
    recommendations = []
    if screen_timeout_streak >= 2:
        recommended_edit_layer = "screening"
        recommendations.extend(
            [
                "Do not make another hybrid-internal micro-tweak before changing the screen harness or sweep.",
                "Prioritize `benchmarks/benchmark_hybrid_pgm_lipp.cc` or the screen compute script.",
                "Restore measurability first: isolate safer variants and maximize the chance of emitting at least one RESULT line.",
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
                "Continue with one bounded change, but classify the failure layer before editing.",
            ]
        )

    assumptions = []
    if screen_timeout_streak >= 1:
        assumptions.append(
            "Recent screen jobs are timing out before the first RESULT line, so the first screened candidate is likely pathological."
        )
    if not reward_state.get("best_hybrid_throughput"):
        assumptions.append(
            "No full six-workload run has completed yet, so reward and incumbent state remain uninitialized."
        )

    status = {
        "dominant_failure_class": dominant_failure_class,
        "screen_timeout_streak": screen_timeout_streak,
        "consecutive_non_advancing_iterations": consecutive_non_advancing,
        "recommended_edit_layer": recommended_edit_layer,
        "recent_preflight_count": len(recent_preflight),
        "recent_result_count": len(results[-5:]),
    }
    (autoresearch / "current_status.json").write_text(
        json.dumps(status, indent=2, sort_keys=True) + "\n",
        encoding="ascii",
    )

    lines = [
        "# Current Blocker",
        "",
        f"- Dominant failure class: `{dominant_failure_class or 'none'}`",
        f"- Screen timeout streak: `{screen_timeout_streak}`",
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
            "- If a run produces no usable measurement, prioritize restoring observability before optimizing throughput.",
        ]
    )
    (autoresearch / "current_blocker.md").write_text("\n".join(lines) + "\n", encoding="ascii")


def main() -> None:
    repo_root = Path("/auto/u/ef0952/projects/COS568-LI-SP26")
    write_status_files(repo_root)


if __name__ == "__main__":
    main()
