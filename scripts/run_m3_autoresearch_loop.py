#!/usr/bin/env python3

import argparse
import csv
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


PENALTY_INCOMPLETE = -1000.0
STATE_POLL_SECONDS = 30
ABORT_EXIT_CODE = 42
DEFAULT_EDITABLE_PATHS = [
    "benchmark.h",
    "util.h",
    "benchmarks/benchmark_hybrid_pgm_lipp.cc",
    "benchmarks/benchmark_hybrid_pgm_lipp.h",
    "competitors/hybrid_pgm_lipp.h",
    "competitors/PGM-index/include/pgm_index_dynamic.hpp",
    "competitors/lipp/src/core/lipp.h",
    "scripts/run_m3_autoresearch_screen_compute.sh",
    "scripts/run_m3_autoresearch_full_compute.sh",
    "scripts/analysis_m3_screen.py",
]
DEFAULT_MUTATION_FAMILIES = {
    "implementation": {
        "benchmark.h",
        "util.h",
        "benchmarks/benchmark_hybrid_pgm_lipp.cc",
        "benchmarks/benchmark_hybrid_pgm_lipp.h",
        "competitors/hybrid_pgm_lipp.h",
        "competitors/PGM-index/include/pgm_index_dynamic.hpp",
        "competitors/lipp/src/core/lipp.h",
    },
    "screening": {
        "benchmarks/benchmark_hybrid_pgm_lipp.cc",
        "benchmarks/benchmark_hybrid_pgm_lipp.h",
        "scripts/run_m3_autoresearch_screen_compute.sh",
        "scripts/analysis_m3_screen.py",
    },
    "measurement": {
        "benchmark.h",
        "util.h",
        "scripts/run_m3_autoresearch_screen_compute.sh",
        "scripts/run_m3_autoresearch_full_compute.sh",
        "scripts/analysis_m3_screen.py",
    },
}


def repo_root_default() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        default=str(repo_root_default()),
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=1,
        help="Number of candidate iterations to run. Use 0 for an indefinite loop.",
    )
    parser.add_argument(
        "--edit-command",
        default="",
        help="Optional shell command to mutate the workspace before each candidate.",
    )
    parser.add_argument(
        "--promote-screen",
        default="always",
        choices=["always", "never"],
        help="Whether a successful screen should be promoted to a full run.",
    )
    parser.add_argument(
        "--restore-incumbent-before-edit",
        action="store_true",
        help="Restore autoresearch/incumbent_stage into the workspace before edit-command.",
    )
    parser.add_argument(
        "--sleep-seconds",
        type=int,
        default=0,
        help="Optional pause between iterations.",
    )
    parser.add_argument(
        "--screen-notes",
        default="",
    )
    parser.add_argument(
        "--change-summary",
        default="",
    )
    parser.add_argument(
        "--cpus",
        default="8",
    )
    parser.add_argument(
        "--memory",
        default="64G",
    )
    parser.add_argument(
        "--time-limit",
        default="04:00:00",
    )
    parser.add_argument(
        "--auto-commit-keeps",
        action="store_true",
        help="Create a git commit automatically for kept candidates.",
    )
    parser.add_argument(
        "--commit-branch",
        default="",
        help="Optional branch to use for auto-committed kept candidates.",
    )
    return parser.parse_args()


def run(
    cmd,
    *,
    cwd: Path,
    env: Optional[Dict[str, str]] = None,
    capture_output: bool = False,
    check: bool = True,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        text=True,
        check=check,
        capture_output=capture_output,
    )


def load_json(path: Path, default):
    if not path.exists():
        return default
    with path.open(encoding="ascii") as handle:
        return json.load(handle)


def save_json(path: Path, data) -> None:
    with path.open("w", encoding="ascii") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")


def update_status(repo_root: Path) -> None:
    run(
        ["python3", "scripts/update_m3_autoresearch_status.py", "--repo-root", str(repo_root)],
        cwd=repo_root,
    )


def git_current_branch(repo_root: Path) -> str:
    result = run(
        ["git", "branch", "--show-current"],
        cwd=repo_root,
        capture_output=True,
    )
    return result.stdout.strip() or "detached"


def next_iteration_tag(iterations_dir: Path) -> str:
    max_n = 0
    for path in iterations_dir.glob("m3_iter*_autoresearch_stage"):
        name = path.name
        middle = name[len("m3_iter") : -len("_autoresearch_stage")]
        if middle.isdigit():
            max_n = max(max_n, int(middle))
    return f"m3_iter{max_n + 1:02d}"


def submit_job(
    repo_root: Path,
    iteration: str,
    mode: str,
    cpus: str,
    memory: str,
    time_limit: str,
) -> str:
    env = os.environ.copy()
    env["SBATCH_PARSABLE"] = "1"
    env["CPUS"] = cpus
    env["MEMORY"] = memory
    env["TIME_LIMIT"] = time_limit
    result = run(
        [
            "bash",
            "scripts/submit_m3_autoresearch.sh",
            iteration,
            mode,
        ],
        cwd=repo_root,
        env=env,
        capture_output=True,
    )
    return result.stdout.strip().split(";")[0]


def poll_job(job_id: str, archive_root: Path) -> Tuple[str, Optional[int]]:
    while True:
        squeue = subprocess.run(
            ["squeue", "-h", "-j", job_id, "-o", "%T"],
            text=True,
            capture_output=True,
        )
        if squeue.returncode == 0 and squeue.stdout.strip():
            time.sleep(STATE_POLL_SECONDS)
            continue

        sacct = subprocess.run(
            ["sacct", "-j", job_id, "--format=State", "--noheader"],
            text=True,
            capture_output=True,
        )
        state = ""
        if sacct.returncode == 0:
            states = [line.strip() for line in sacct.stdout.splitlines() if line.strip()]
            if states:
                state = states[0].split()[0]

        benchmark_status_path = archive_root / f"benchmark_status.{job_id}.txt"
        rc = None
        if benchmark_status_path.exists():
            text = benchmark_status_path.read_text(encoding="ascii").strip()
            if text:
                rc = int(text)
        return state, rc


def inspect_benchmark_output(archive_root: Path, job_id: str) -> int:
    stdout_path = archive_root / f"benchmark.{job_id}.stdout"
    if not stdout_path.exists():
        return 0
    stdout_text = stdout_path.read_text(encoding="ascii", errors="ignore")
    return stdout_text.count("RESULT:")


def latest_results_dir(archive_root: Path) -> Optional[Path]:
    candidates = [p for p in archive_root.glob("results.*") if p.is_dir()]
    if not candidates:
        return None
    return max(candidates, key=lambda p: p.stat().st_mtime)


def append_preflight(
    preflight_path: Path,
    iteration: str,
    screen_job: str,
    screen_status: str,
    screen_rc: int,
    failure_class: str,
    result_count: int,
    screen_results_dir: str,
    notes: str,
) -> None:
    with preflight_path.open("a", newline="", encoding="ascii") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(
            [
                iteration,
                screen_job,
                screen_status,
                screen_rc,
                failure_class,
                result_count,
                screen_results_dir,
                notes,
            ]
        )


def promote_incumbent(repo_root: Path, stage_dir: Path, full_results_dir: Path) -> None:
    incumbent_dir = repo_root / "autoresearch" / "incumbent_stage"
    incumbent_results_dir = repo_root / "autoresearch" / "incumbent_results"
    if incumbent_dir.exists():
        shutil.rmtree(incumbent_dir)
    if incumbent_results_dir.exists():
        shutil.rmtree(incumbent_results_dir)
    shutil.copytree(stage_dir, incumbent_dir)
    shutil.copytree(full_results_dir, incumbent_results_dir)


def auto_commit_keep(repo_root: Path, iteration: str, reward: float, status: str, branch: str) -> None:
    run(
        [
            "bash",
            "scripts/commit_m3_kept_candidate.sh",
            iteration,
            f"{reward:.12g}",
            status,
            branch,
        ],
        cwd=repo_root,
    )


def restore_incumbent(repo_root: Path) -> None:
    incumbent_dir = repo_root / "autoresearch" / "incumbent_stage"
    if not incumbent_dir.exists():
        return
    run(
        ["bash", "scripts/restore_m3_autoresearch_incumbent.sh"],
        cwd=repo_root,
    )


def evaluate_reward(
    repo_root: Path,
    iteration: str,
    results_dir: Path,
    screen_job: str,
    full_job: str,
    status: str,
    change_summary: str,
    screen_notes: str,
    full_notes: str,
    novelty_score: float,
    self_eval_abort: bool,
    no_log: bool = False,
) -> Dict[str, Any]:
    cmd = [
        "python3",
        "scripts/evaluate_m3_autoresearch_reward.py",
        "--repo-root",
        str(repo_root),
        "--iteration",
        iteration,
        "--results-dir",
        str(results_dir),
        "--screen-job",
        screen_job,
        "--full-job",
        full_job,
        "--status",
        status,
        "--change-summary",
        change_summary,
        "--screen-notes",
        screen_notes,
        "--full-notes",
        full_notes,
        "--novelty-score",
        f"{novelty_score:.12g}",
    ]
    if self_eval_abort:
        cmd.append("--self-eval-abort")
    if no_log:
        cmd.append("--no-log")
    output = run(cmd, cwd=repo_root, capture_output=True)
    return json.loads(output.stdout)


def send_update_email(
    repo_root: Path,
    iteration: str,
    results_dir: Path,
    reward: float,
    status: str,
    screen_job: str,
    full_job: str,
    full_notes: str,
    change_summary: str,
) -> None:
    reward_arg = format(reward, ".12g")
    try:
        run(
            [
                "python3",
                "scripts/send_m3_autoresearch_update.py",
                "--repo-root",
                str(repo_root),
                "--iteration",
                iteration,
                "--results-dir",
                str(results_dir),
                f"--reward={reward_arg}",
                "--status",
                status,
                "--screen-job",
                screen_job,
                "--full-job",
                full_job,
                "--full-notes",
                full_notes,
                "--change-summary",
                change_summary,
            ],
            cwd=repo_root,
        )
    except subprocess.CalledProcessError as exc:
        print(
            f"Warning: failed to send autoresearch email for {iteration}: {exc}",
            file=sys.stderr,
            flush=True,
        )


def send_screen_failure_email(
    repo_root: Path,
    iteration: str,
    reward: float,
    status: str,
    screen_job: str,
    full_notes: str,
    change_summary: str,
) -> None:
    send_update_email(
        repo_root=repo_root,
        iteration=iteration,
        results_dir=Path("."),
        reward=reward,
        status=status,
        screen_job=screen_job,
        full_job="",
        full_notes=full_notes,
        change_summary=change_summary,
    )


def maybe_run_edit_command(repo_root: Path, edit_command: str) -> None:
    if not edit_command:
        return
    subprocess.run(
        edit_command,
        cwd=str(repo_root),
        shell=True,
        text=True,
        check=True,
    )


def classify_screen_outcome(screen_rc: Optional[int], result_count: int) -> str:
    if screen_rc == 0:
        return "screen_success" if result_count > 0 else "screen_completed_no_result"
    if screen_rc == 124:
        return "screen_timeout_partial" if result_count > 0 else "screen_timeout_no_result"
    if screen_rc == ABORT_EXIT_CODE:
        return "screen_self_eval_abort"
    return "screen_failure_partial" if result_count > 0 else "screen_failure_no_result"


def load_mutation_policy(repo_root: Path) -> Dict[str, Any]:
    policy_path = repo_root / "autoresearch" / "mutation_policy.json"
    if not policy_path.exists():
        return {
            "editable_paths": DEFAULT_EDITABLE_PATHS,
            "mutation_families": {k: sorted(v) for k, v in DEFAULT_MUTATION_FAMILIES.items()},
        }
    return load_json(policy_path, {})


def tracked_changes(repo_root: Path) -> set[str]:
    result = run(
        ["git", "diff", "--name-only"],
        cwd=repo_root,
        capture_output=True,
    )
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def newly_touched_disallowed_paths(
    before: set[str],
    after: set[str],
    allowed_paths: List[str],
) -> List[str]:
    allowed = tuple(allowed_paths)
    disallowed = []
    for relpath in sorted(after - before):
        if relpath.startswith("autoresearch/") or relpath.startswith("iterations/"):
            continue
        if relpath in allowed:
            continue
        disallowed.append(relpath)
    return disallowed


def diff_numstat(repo_root: Path, relpaths: List[str]) -> Tuple[int, int]:
    if not relpaths:
        return 0, 0
    result = run(
        ["git", "diff", "--numstat", "--", *relpaths],
        cwd=repo_root,
        capture_output=True,
    )
    added = 0
    deleted = 0
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        try:
            added += int(parts[0])
            deleted += int(parts[1])
        except ValueError:
            continue
    return added, deleted


def current_candidate_files(repo_root: Path, editable_paths: List[str]) -> List[str]:
    changed = tracked_changes(repo_root)
    return [path for path in editable_paths if path in changed]


def classify_mutation_family(changed_files: List[str], families: Dict[str, List[str]]) -> str:
    if not changed_files:
        return "no_change"
    scores = []
    changed = set(changed_files)
    for family, relpaths in families.items():
        overlap = len(changed & set(relpaths))
        scores.append((overlap, family))
    scores.sort(reverse=True)
    if len(scores) >= 2 and scores[0][0] == scores[1][0] and scores[0][0] > 0:
        return "mixed"
    if scores and scores[0][0] > 0:
        return scores[0][1]
    return "mixed"


def compute_stage_fingerprint(stage_dir: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(stage_dir.rglob("*")):
        if not path.is_file() or path.name == "MANIFEST.txt":
            continue
        digest.update(str(path.relative_to(stage_dir)).encode("ascii"))
        digest.update(path.read_bytes())
    return digest.hexdigest()


def load_trajectory(trajectory_path: Path) -> List[Dict[str, Any]]:
    if not trajectory_path.exists():
        return []
    entries = []
    for line in trajectory_path.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        entries.append(json.loads(line))
    return entries


def assess_novelty(
    trajectory: List[Dict[str, Any]],
    fingerprint: str,
    changed_files: List[str],
    mutation_family: str,
) -> Dict[str, Any]:
    duplicate_iteration = ""
    novelty_score = 1.0
    changed_set = set(changed_files)
    for entry in reversed(trajectory):
        candidate = entry.get("candidate", {})
        if candidate.get("file_fingerprint") == fingerprint:
            duplicate_iteration = entry.get("iteration", "")
            novelty_score = 0.0
            break
        prior_files = set(candidate.get("changed_files", []))
        if prior_files == changed_set and candidate.get("mutation_family") == mutation_family:
            duplicate_iteration = entry.get("iteration", "")
            novelty_score = 0.35
            break
    return {
        "novelty_score": novelty_score,
        "duplicate_iteration": duplicate_iteration,
    }


def append_trajectory_entry(path: Path, entry: Dict[str, Any]) -> None:
    with path.open("a", encoding="ascii") as handle:
        handle.write(json.dumps(entry, sort_keys=True) + "\n")


def run_self_evaluator(repo_root: Path, results_dir: Path, mode: str) -> Tuple[int, Dict[str, Any]]:
    cmd = [
        "python3",
        "scripts/self_evaluate_m3_candidate.py",
        "--repo-root",
        str(repo_root),
        "--results-dir",
        str(results_dir),
        "--mode",
        mode,
    ]
    result = run(cmd, cwd=repo_root, capture_output=True, check=False)
    payload = {}
    if result.stdout.strip():
        payload = json.loads(result.stdout)
    return result.returncode, payload


def update_loop_state(loop_state_path: Path, loop_state: Dict[str, Any], **updates: Any) -> None:
    loop_state.update(updates)
    save_json(loop_state_path, loop_state)


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root)
    autoresearch_dir = repo_root / "autoresearch"
    iterations_dir = repo_root / "iterations"
    loop_state_path = autoresearch_dir / "loop_state.json"
    preflight_path = autoresearch_dir / "preflight.tsv"
    trajectory_path = autoresearch_dir / "trajectory.jsonl"
    branch = git_current_branch(repo_root)
    policy = load_mutation_policy(repo_root)
    editable_paths = policy.get("editable_paths", DEFAULT_EDITABLE_PATHS)
    mutation_families = policy.get(
        "mutation_families",
        {k: sorted(v) for k, v in DEFAULT_MUTATION_FAMILIES.items()},
    )

    loop_state = load_json(
        loop_state_path,
        {
            "active_branch": branch,
            "active_full_job": "",
            "active_iteration": "",
            "active_phase": "idle",
            "active_screen_job": "",
            "incumbent_iteration": "",
            "last_completed_iteration": "",
            "last_full_job": "",
            "last_screen_job": "",
            "last_self_eval_decision": "",
            "repo_root": str(repo_root),
        },
    )
    update_loop_state(
        loop_state_path,
        loop_state,
        active_branch=branch,
        active_phase="idle",
        repo_root=str(repo_root),
    )

    iteration_count = 0
    while args.iterations == 0 or iteration_count < args.iterations:
        trajectory = load_trajectory(trajectory_path)
        update_status(repo_root)
        before_changes = tracked_changes(repo_root)
        if args.restore_incumbent_before_edit:
            restore_incumbent(repo_root)
        before_changes = tracked_changes(repo_root)

        update_loop_state(
            loop_state_path,
            loop_state,
            active_iteration="pending",
            active_phase="editing",
            active_screen_job="",
            active_full_job="",
        )
        maybe_run_edit_command(repo_root, args.edit_command)

        after_changes = tracked_changes(repo_root)
        disallowed_paths = newly_touched_disallowed_paths(before_changes, after_changes, editable_paths)
        if disallowed_paths:
            raise RuntimeError(
                "Edit command touched files outside the allowed mutation policy: "
                + ", ".join(disallowed_paths)
            )

        iteration = next_iteration_tag(iterations_dir)
        stage_dir = iterations_dir / f"{iteration}_autoresearch_stage"
        archive_root = repo_root / "slurm_runs" / iteration
        update_loop_state(
            loop_state_path,
            loop_state,
            active_iteration=iteration,
            active_phase="staging",
        )

        run(
            ["bash", "scripts/stage_m3_autoresearch_iteration.sh", iteration],
            cwd=repo_root,
        )

        changed_files = current_candidate_files(repo_root, editable_paths)
        mutation_family = classify_mutation_family(changed_files, mutation_families)
        added_lines, deleted_lines = diff_numstat(repo_root, changed_files)
        file_fingerprint = compute_stage_fingerprint(stage_dir)
        novelty = assess_novelty(trajectory, file_fingerprint, changed_files, mutation_family)
        candidate_metadata = {
            "added_lines": added_lines,
            "changed_files": changed_files,
            "deleted_lines": deleted_lines,
            "duplicate_iteration": novelty["duplicate_iteration"],
            "file_fingerprint": file_fingerprint,
            "mutation_family": mutation_family,
            "novelty_score": novelty["novelty_score"],
        }

        screen_job = submit_job(
            repo_root,
            iteration,
            "screen",
            args.cpus,
            args.memory,
            args.time_limit,
        )
        update_loop_state(
            loop_state_path,
            loop_state,
            active_phase="screen",
            active_screen_job=screen_job,
            last_screen_job=screen_job,
        )

        screen_state, screen_rc = poll_job(screen_job, archive_root)
        screen_results_dir = latest_results_dir(archive_root)
        result_count = inspect_benchmark_output(archive_root, screen_job)
        self_eval_payload: Dict[str, Any] = {}
        self_eval_rc = 0
        if screen_rc == 0 and screen_results_dir is not None:
            self_eval_rc, self_eval_payload = run_self_evaluator(repo_root, screen_results_dir, "screen")
        failure_class = classify_screen_outcome(screen_rc if self_eval_rc == 0 else self_eval_rc, result_count)
        screen_notes = args.screen_notes or f"screen_state={screen_state},screen_rc={screen_rc}"
        if self_eval_payload:
            screen_notes = (
                f"{screen_notes};self_eval={self_eval_payload.get('decision')}"
                f";self_eval_reason={self_eval_payload.get('reason', '')}"
            )
        append_preflight(
            preflight_path,
            iteration,
            screen_job,
            screen_state or "unknown",
            int(screen_rc or -1),
            failure_class,
            result_count,
            str(screen_results_dir) if screen_results_dir else "",
            screen_notes,
        )
        update_loop_state(
            loop_state_path,
            loop_state,
            last_self_eval_decision=self_eval_payload.get("decision", ""),
        )
        update_status(repo_root)

        if screen_rc != 0 or args.promote_screen == "never" or self_eval_rc == ABORT_EXIT_CODE:
            if screen_rc != 0:
                send_screen_failure_email(
                    repo_root=repo_root,
                    iteration=iteration,
                    reward=PENALTY_INCOMPLETE,
                    status="crash",
                    screen_job=screen_job,
                    full_notes=screen_notes,
                    change_summary=args.change_summary,
                )
            elif self_eval_rc == ABORT_EXIT_CODE:
                send_screen_failure_email(
                    repo_root=repo_root,
                    iteration=iteration,
                    reward=-0.25,
                    status="discard",
                    screen_job=screen_job,
                    full_notes=screen_notes,
                    change_summary=args.change_summary,
                )

            append_trajectory_entry(
                trajectory_path,
                {
                    "branch": branch,
                    "candidate": candidate_metadata,
                    "full": {},
                    "iteration": iteration,
                    "screen": {
                        "failure_class": failure_class,
                        "job": screen_job,
                        "notes": screen_notes,
                        "result_count": result_count,
                        "self_eval": self_eval_payload,
                        "state": screen_state,
                    },
                    "status": "crash" if screen_rc != 0 else "discard",
                },
            )
            update_loop_state(
                loop_state_path,
                loop_state,
                active_iteration="",
                active_phase="idle",
                active_screen_job="",
                active_full_job="",
            )
            iteration_count += 1
            if args.sleep_seconds:
                time.sleep(args.sleep_seconds)
            continue

        full_job = submit_job(
            repo_root,
            iteration,
            "full",
            args.cpus,
            args.memory,
            args.time_limit,
        )
        update_loop_state(
            loop_state_path,
            loop_state,
            active_phase="full",
            active_full_job=full_job,
            last_full_job=full_job,
        )

        full_state, full_rc = poll_job(full_job, archive_root)
        full_results_dir = latest_results_dir(archive_root)
        self_eval_abort = full_rc == ABORT_EXIT_CODE
        full_notes = f"full_state={full_state},full_rc={full_rc}"

        reward_payload: Dict[str, Any]
        if full_results_dir is not None:
            preview_status = "discard" if self_eval_abort else ("crash" if full_rc not in (0, None) else "keep")
            reward_payload = evaluate_reward(
                repo_root=repo_root,
                iteration=iteration,
                results_dir=full_results_dir,
                screen_job=screen_job,
                full_job=full_job,
                status=preview_status,
                change_summary=args.change_summary,
                screen_notes=screen_notes,
                full_notes=full_notes,
                novelty_score=float(candidate_metadata["novelty_score"]),
                self_eval_abort=self_eval_abort,
                no_log=True,
            )
        else:
            reward_payload = {"reward": PENALTY_INCOMPLETE}

        reward_value = float(reward_payload["reward"])
        if self_eval_abort:
            status = "discard"
        elif full_rc not in (0, None):
            status = "crash"
        else:
            status = "keep" if reward_value >= 0 else "discard"

        if full_results_dir is not None:
            reward_payload = evaluate_reward(
                repo_root=repo_root,
                iteration=iteration,
                results_dir=full_results_dir,
                screen_job=screen_job,
                full_job=full_job,
                status=status,
                change_summary=args.change_summary,
                screen_notes=screen_notes,
                full_notes=full_notes,
                novelty_score=float(candidate_metadata["novelty_score"]),
                self_eval_abort=self_eval_abort,
            )
            reward = float(reward_payload["reward"])
        else:
            reward = PENALTY_INCOMPLETE

        if status == "keep" and reward >= 0 and full_results_dir is not None and full_rc == 0:
            promote_incumbent(repo_root, stage_dir, full_results_dir)
            loop_state["incumbent_iteration"] = iteration
            if args.auto_commit_keeps:
                auto_commit_keep(
                    repo_root=repo_root,
                    iteration=iteration,
                    reward=reward,
                    status=status,
                    branch=args.commit_branch,
                )

        append_trajectory_entry(
            trajectory_path,
            {
                "branch": branch,
                "candidate": candidate_metadata,
                "full": {
                    "job": full_job,
                    "notes": full_notes,
                    "results_dir": str(full_results_dir) if full_results_dir else "",
                    "reward": reward,
                    "self_eval_abort": self_eval_abort,
                    "state": full_state,
                },
                "iteration": iteration,
                "screen": {
                    "failure_class": failure_class,
                    "job": screen_job,
                    "notes": screen_notes,
                    "result_count": result_count,
                    "self_eval": self_eval_payload,
                    "state": screen_state,
                },
                "status": status,
            },
        )

        update_loop_state(
            loop_state_path,
            loop_state,
            active_iteration="",
            active_phase="idle",
            active_screen_job="",
            active_full_job="",
            last_completed_iteration=iteration,
        )
        update_status(repo_root)
        send_update_email(
            repo_root=repo_root,
            iteration=iteration,
            results_dir=full_results_dir if full_results_dir is not None else Path("."),
            reward=reward,
            status=status,
            screen_job=screen_job,
            full_job=full_job,
            full_notes=full_notes,
            change_summary=args.change_summary,
        )

        iteration_count += 1
        if args.sleep_seconds:
            time.sleep(args.sleep_seconds)


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as exc:
        cmd = exc.cmd if isinstance(exc.cmd, str) else " ".join(shlex.quote(str(part)) for part in exc.cmd)
        print(f"Command failed: {cmd}", file=sys.stderr)
        if exc.stdout:
            print(exc.stdout, file=sys.stderr)
        if exc.stderr:
            print(exc.stderr, file=sys.stderr)
        raise
