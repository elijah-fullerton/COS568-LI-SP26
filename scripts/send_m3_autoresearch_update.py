#!/usr/bin/env python3

import argparse
import base64
import csv
import json
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt


WORKLOADS: List[Tuple[str, str]] = [
    ("books", "0.100000i"),
    ("books", "0.900000i"),
    ("fb", "0.100000i"),
    ("fb", "0.900000i"),
    ("osmc", "0.100000i"),
    ("osmc", "0.900000i"),
]
WORKLOAD_LABELS = {
    ("books", "0.100000i"): "books_mixed_10_insert",
    ("books", "0.900000i"): "books_mixed_90_insert",
    ("fb", "0.100000i"): "fb_mixed_10_insert",
    ("fb", "0.900000i"): "fb_mixed_90_insert",
    ("osmc", "0.100000i"): "osmc_mixed_10_insert",
    ("osmc", "0.900000i"): "osmc_mixed_90_insert",
}


def repo_root_default() -> Path:
    return Path(__file__).resolve().parents[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iteration", required=True)
    parser.add_argument(
        "--repo-root",
        default=str(repo_root_default()),
    )
    parser.add_argument(
        "--results-dir",
        default="",
    )
    parser.add_argument(
        "--reward",
        type=float,
        required=True,
    )
    parser.add_argument(
        "--status",
        required=True,
        choices=["keep", "discard", "crash"],
    )
    parser.add_argument(
        "--screen-job",
        default="",
    )
    parser.add_argument(
        "--full-job",
        default="",
    )
    parser.add_argument(
        "--full-notes",
        default="",
    )
    parser.add_argument(
        "--change-summary",
        default="",
    )
    return parser.parse_args()


def csv_name(dataset_prefix: str, workload_token: str) -> str:
    return (
        f"{dataset_prefix}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_"
        f"{workload_token}_0m_mix_results_table.csv"
    )


def average_throughput(row: Dict[str, str]) -> float:
    cols = [
        "mixed_throughput_mops1",
        "mixed_throughput_mops2",
        "mixed_throughput_mops3",
    ]
    vals = [float(row[c]) for c in cols if row.get(c, "") != ""]
    if not vals:
        raise ValueError("No mixed throughput columns found")
    return sum(vals) / len(vals)


def load_csv_rows(path: Path) -> List[Dict[str, str]]:
    with path.open(newline="", encoding="ascii") as handle:
        return list(csv.DictReader(handle, delimiter="," if path.suffix == ".csv" else "\t"))


def read_results_tsv(path: Path) -> List[Dict[str, str]]:
    with path.open(newline="", encoding="ascii") as handle:
        return list(csv.DictReader(handle, delimiter="\t"))


def best_baselines(baseline_dir: Path) -> Dict[str, Dict[str, float]]:
    baselines: Dict[str, Dict[str, float]] = {}
    for dataset_prefix, workload_token in WORKLOADS:
        path = baseline_dir / csv_name(dataset_prefix, workload_token)
        rows = load_csv_rows(path)
        best_dpgm = None
        best_lipp = None
        for row in rows:
            avg = average_throughput(row)
            if row["index_name"] == "DynamicPGM":
                best_dpgm = avg if best_dpgm is None else max(best_dpgm, avg)
            elif row["index_name"] == "LIPP":
                best_lipp = avg if best_lipp is None else max(best_lipp, avg)
        if best_dpgm is None or best_lipp is None:
            raise ValueError(f"Missing baselines in {path}")
        baselines[WORKLOAD_LABELS[(dataset_prefix, workload_token)]] = {
            "DynamicPGM": best_dpgm,
            "LIPP": best_lipp,
        }
    return baselines


def current_hybrid_throughputs(results_dir: Path) -> Dict[str, float]:
    current: Dict[str, float] = {}
    for dataset_prefix, workload_token in WORKLOADS:
        path = results_dir / csv_name(dataset_prefix, workload_token)
        if not path.exists():
            continue
        rows = load_csv_rows(path)
        best_hybrid = None
        for row in rows:
            if row["index_name"] != "HybridPGMLIPP":
                continue
            avg = average_throughput(row)
            best_hybrid = avg if best_hybrid is None else max(best_hybrid, avg)
        if best_hybrid is not None:
            current[WORKLOAD_LABELS[(dataset_prefix, workload_token)]] = best_hybrid
    return current


def workload_gap_sum(
    baselines: Dict[str, Dict[str, float]],
    current: Dict[str, float],
) -> float:
    if any(label not in current for label in baselines):
        return float("nan")
    total = 0.0
    for label, base in baselines.items():
        h_cur = current[label]
        total += max(0.0, max(base["DynamicPGM"], base["LIPP"]) - h_cur)
    return total


def compute_history_points(
    rows: List[Dict[str, str]],
    baselines: Dict[str, Dict[str, float]],
) -> List[Tuple[int, str, float]]:
    points = []
    for idx, row in enumerate(rows, start=1):
        results_dir_str = row.get("results_dir", "").strip()
        if not results_dir_str or row.get("status") == "crash":
            continue
        results_dir = Path(results_dir_str)
        try:
            current = current_hybrid_throughputs(results_dir)
            gap = workload_gap_sum(baselines, current)
        except Exception:
            continue
        if gap == gap:
            points.append((idx, row["iteration"], gap))
    return points


def make_progress_figure(
    output_path: Path,
    points: List[Tuple[int, str, float]],
    current_iteration: str,
) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig, ax = plt.subplots(figsize=(9, 5))
    if points:
        xs = [p[0] for p in points]
        ys = [p[2] for p in points]
        best_so_far = []
        running = None
        for y in ys:
            running = y if running is None else min(running, y)
            best_so_far.append(running)
        ax.plot(xs, ys, marker="o", linewidth=1.5, label="per experiment")
        ax.plot(xs, best_so_far, linestyle="--", linewidth=1.5, label="best so far")
        ax.annotate(current_iteration, (xs[-1], ys[-1]), xytext=(6, 6), textcoords="offset points")
        ax.legend()
    ax.set_title("Autoresearch Progress")
    ax.set_xlabel("Experiment Index")
    ax.set_ylabel("Gap Sum vs Best Baseline Across 6 Workloads")
    ax.grid(alpha=0.25)
    fig.tight_layout()
    fig.savefig(output_path, dpi=160)
    plt.close(fig)


def read_optional_text(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="ascii", errors="ignore").strip()


def solved_workloads(
    baselines: Dict[str, Dict[str, float]],
    reward_state_path: Path,
) -> Tuple[int, int, List[str]]:
    if reward_state_path.exists():
        state = json.loads(reward_state_path.read_text(encoding="ascii"))
        best = state.get("best_hybrid_throughput", {})
    else:
        best = {}
    solved = []
    for label, base in baselines.items():
        h = best.get(label, 0.0)
        if h >= base["DynamicPGM"] and h >= base["LIPP"]:
            solved.append(label)
    return len(solved), len(baselines), solved


def git_current_branch(repo_root: Path) -> str:
    result = subprocess.run(
        ["git", "branch", "--show-current"],
        cwd=repo_root,
        text=True,
        capture_output=True,
        check=True,
    )
    return result.stdout.strip() or "detached"


def compose_body(
    iteration: str,
    branch: str,
    status: str,
    reward: float,
    change_summary: str,
    prior_experiment_summary: str,
    context_summary: str,
    points: List[Tuple[int, str, float]],
    current_gap_sum: float,
    solved_count: int,
    total_workloads: int,
    solved_labels: List[str],
    screen_job: str,
    full_job: str,
    full_notes: str,
) -> str:
    best_gap = min((p[2] for p in points), default=float("nan"))
    current_gap_text = "n/a" if current_gap_sum != current_gap_sum else f"{current_gap_sum:.6f}"
    best_gap_text = "n/a" if best_gap != best_gap else f"{best_gap:.6f}"
    lines = [
        f"Iteration: {iteration}",
        f"Branch: {branch}",
        f"Status: {status}",
        f"Reward: {reward:.12g}",
        f"Current gap sum: {current_gap_text}",
        f"Best gap sum so far: {best_gap_text}",
        f"Solved workloads: {solved_count}/{total_workloads}",
        f"Screen job: {screen_job or 'n/a'}",
        f"Full job: {full_job or 'n/a'}",
    ]
    if full_notes:
        lines.extend(["", "Run notes:", full_notes])
    if solved_labels:
        lines.extend(["", "Solved workloads:", *[f"- {label}" for label in solved_labels]])
    if change_summary:
        lines.extend(["", "Change summary:", change_summary])
    if prior_experiment_summary:
        lines.extend(["", "What we did in the prior experiment:", prior_experiment_summary])
    if context_summary:
        lines.extend(["", "Current context summary:", context_summary])
    lines.extend(
        [
            "",
            "Progress summary:",
            "- Lower gap sum is better.",
            "- The progress figure is attached.",
            "- The email subject includes the branch to distinguish concurrent autoresearch loops.",
        ]
    )
    return "\n".join(lines) + "\n"


def build_mime_message(
    to_addr: str,
    subject: str,
    body: str,
    attachment_name: str,
    attachment_bytes: bytes,
) -> bytes:
    boundary = "===============m3autoresearch=="
    encoded = base64.b64encode(attachment_bytes).decode("ascii")
    chunks = [encoded[i : i + 76] for i in range(0, len(encoded), 76)]
    lines = [
        f"To: {to_addr}",
        f"Subject: {subject}",
        "MIME-Version: 1.0",
        f'Content-Type: multipart/mixed; boundary="{boundary}"',
        "",
        f"--{boundary}",
        'Content-Type: text/plain; charset="utf-8"',
        "Content-Transfer-Encoding: 8bit",
        "",
        body,
        f"--{boundary}",
        f'Content-Type: image/png; name="{attachment_name}"',
        "Content-Transfer-Encoding: base64",
        f'Content-Disposition: attachment; filename="{attachment_name}"',
        "",
        *chunks,
        f"--{boundary}--",
        "",
    ]
    return "\n".join(lines).encode("utf-8")


def send_mail(message: bytes) -> None:
    subprocess.run(
        ["/usr/sbin/sendmail", "-t", "-oi"],
        input=message,
        check=True,
    )


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root)
    branch = git_current_branch(repo_root)
    autoresearch_dir = repo_root / "autoresearch"
    baseline_dir = repo_root / "results_milestone3"
    results_dir = Path(args.results_dir) if args.results_dir else Path("")
    email_config = json.loads((autoresearch_dir / "email_config.json").read_text(encoding="ascii"))
    to_addr = email_config["to"]

    baselines = best_baselines(baseline_dir)
    results_rows = read_results_tsv(autoresearch_dir / "results.tsv")
    points = compute_history_points(results_rows, baselines)
    current = current_hybrid_throughputs(results_dir) if results_dir and str(results_dir) != "." else {}
    current_gap_sum = workload_gap_sum(baselines, current) if current else float("nan")
    solved_count, total_workloads, solved_labels = solved_workloads(
        baselines, autoresearch_dir / "reward_state.json"
    )

    figure_path = autoresearch_dir / "progress.png"
    make_progress_figure(figure_path, points, args.iteration)

    prior_experiment_summary = read_optional_text(autoresearch_dir / "last_codex_message.txt")
    context_summary = read_optional_text(autoresearch_dir / "current_context.md")
    body = compose_body(
        iteration=args.iteration,
        branch=branch,
        status=args.status,
        reward=args.reward,
        change_summary=args.change_summary,
        prior_experiment_summary=prior_experiment_summary,
        context_summary=context_summary,
        points=points,
        current_gap_sum=current_gap_sum,
        solved_count=solved_count,
        total_workloads=total_workloads,
        solved_labels=solved_labels,
        screen_job=args.screen_job,
        full_job=args.full_job,
        full_notes=args.full_notes,
    )
    subject = f"[LI-SP26 autoresearch:{branch}] {args.iteration} {args.status} reward={args.reward:.6g}"
    message = build_mime_message(
        to_addr=to_addr,
        subject=subject,
        body=body,
        attachment_name=figure_path.name,
        attachment_bytes=figure_path.read_bytes(),
    )
    send_mail(message)

    log_entry = {
        "branch": branch,
        "iteration": args.iteration,
        "reward": args.reward,
        "sent_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": args.status,
        "subject": subject,
        "to": to_addr,
    }
    with (autoresearch_dir / "email_log.jsonl").open("a", encoding="ascii") as handle:
        handle.write(json.dumps(log_entry, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
