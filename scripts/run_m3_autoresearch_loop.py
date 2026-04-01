#!/usr/bin/env python3

import argparse
import csv
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Optional, Tuple


PENALTY_INCOMPLETE = -1e18
STATE_POLL_SECONDS = 30


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        default="/auto/u/ef0952/projects/COS568-LI-SP26",
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
) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        env=env,
        text=True,
        check=True,
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
    screen_results_dir: str,
    notes: str,
) -> None:
    with preflight_path.open("a", newline="", encoding="ascii") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(
            [iteration, screen_job, screen_status, screen_results_dir, notes]
        )


def promote_incumbent(repo_root: Path, stage_dir: Path, iteration: str, full_results_dir: Path) -> None:
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
    full_results_dir: Path,
    screen_job: str,
    full_job: str,
    status: str,
    change_summary: str,
    screen_notes: str,
    full_notes: str,
) -> float:
    output = run(
        [
            "python3",
            "scripts/evaluate_m3_autoresearch_reward.py",
            "--iteration",
            iteration,
            "--results-dir",
            str(full_results_dir),
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
        ],
        cwd=repo_root,
        capture_output=True,
    )
    payload = json.loads(output.stdout)
    return float(payload["reward"])


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
    run(
        [
            "python3",
            "scripts/send_m3_autoresearch_update.py",
            "--iteration",
            iteration,
            "--results-dir",
            str(results_dir),
            "--reward",
            str(reward),
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


def log_forced_penalty(
    repo_root: Path,
    iteration: str,
    screen_job: str,
    full_job: str,
    status: str,
    change_summary: str,
    screen_notes: str,
    full_notes: str,
) -> None:
    run(
        [
            "python3",
            "scripts/evaluate_m3_autoresearch_reward.py",
            "--iteration",
            iteration,
            "--force-penalty",
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
        ],
        cwd=repo_root,
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


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root)
    autoresearch_dir = repo_root / "autoresearch"
    iterations_dir = repo_root / "iterations"
    loop_state_path = autoresearch_dir / "loop_state.json"
    preflight_path = autoresearch_dir / "preflight.tsv"

    loop_state = load_json(
        loop_state_path,
        {
            "incumbent_iteration": "",
            "last_completed_iteration": "",
            "last_full_job": "",
            "last_screen_job": "",
        },
    )

    iteration_count = 0
    while args.iterations == 0 or iteration_count < args.iterations:
        if args.restore_incumbent_before_edit:
            restore_incumbent(repo_root)

        maybe_run_edit_command(repo_root, args.edit_command)

        iteration = next_iteration_tag(iterations_dir)
        stage_dir = iterations_dir / f"{iteration}_autoresearch_stage"
        archive_root = repo_root / "slurm_runs" / iteration

        run(
            ["bash", "scripts/stage_m3_autoresearch_iteration.sh", iteration],
            cwd=repo_root,
        )

        screen_job = submit_job(
            repo_root,
            iteration,
            "screen",
            args.cpus,
            args.memory,
            args.time_limit,
        )
        loop_state["last_screen_job"] = screen_job
        save_json(loop_state_path, loop_state)

        screen_state, screen_rc = poll_job(screen_job, archive_root)
        screen_results_dir = latest_results_dir(archive_root)
        screen_notes = args.screen_notes or f"screen_state={screen_state},screen_rc={screen_rc}"
        append_preflight(
            preflight_path,
            iteration,
            screen_job,
            screen_state or "unknown",
            str(screen_results_dir) if screen_results_dir else "",
            screen_notes,
        )

        if screen_rc != 0 or args.promote_screen == "never":
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
        loop_state["last_full_job"] = full_job
        save_json(loop_state_path, loop_state)

        full_state, full_rc = poll_job(full_job, archive_root)
        full_results_dir = latest_results_dir(archive_root)
        full_notes = f"full_state={full_state},full_rc={full_rc}"

        if full_rc != 0 or full_results_dir is None:
            log_forced_penalty(
                repo_root=repo_root,
                iteration=iteration,
                screen_job=screen_job,
                full_job=full_job,
                status="crash",
                change_summary=args.change_summary,
                screen_notes=screen_notes,
                full_notes=full_notes,
            )
            send_update_email(
                repo_root=repo_root,
                iteration=iteration,
                results_dir=Path("."),
                reward=PENALTY_INCOMPLETE,
                status="crash",
                screen_job=screen_job,
                full_job=full_job,
                full_notes=full_notes,
                change_summary=args.change_summary,
            )
            iteration_count += 1
            if args.sleep_seconds:
                time.sleep(args.sleep_seconds)
            continue

        reward_preview = run(
            [
                "python3",
                "scripts/evaluate_m3_autoresearch_reward.py",
                "--iteration",
                iteration,
                "--results-dir",
                str(full_results_dir),
                "--no-log",
            ],
            cwd=repo_root,
            capture_output=True,
        )
        reward_value = float(json.loads(reward_preview.stdout)["reward"])
        status = "keep" if reward_value >= 0 else "discard"
        reward = evaluate_reward(
            repo_root=repo_root,
            iteration=iteration,
            full_results_dir=full_results_dir,
            screen_job=screen_job,
            full_job=full_job,
            status=status,
            change_summary=args.change_summary,
            screen_notes=screen_notes,
            full_notes=full_notes,
        )

        if reward >= 0:
            promote_incumbent(repo_root, stage_dir, iteration, full_results_dir)
            loop_state["incumbent_iteration"] = iteration
            if args.auto_commit_keeps:
                auto_commit_keep(
                    repo_root=repo_root,
                    iteration=iteration,
                    reward=reward,
                    status=status,
                    branch=args.commit_branch,
                )

        loop_state["last_completed_iteration"] = iteration
        save_json(loop_state_path, loop_state)
        send_update_email(
            repo_root=repo_root,
            iteration=iteration,
            results_dir=full_results_dir,
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
