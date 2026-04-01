#!/usr/bin/env python3

import argparse
import csv
import json
from pathlib import Path
from typing import Dict, List, Tuple


PENALTY_INCOMPLETE = -1000.0
PARTIAL_COMPLETION_WEIGHT = 2.0
PARTIAL_MISSING_WEIGHT = 3.0
NOVELTY_WEIGHT = 1.0
SELF_EVAL_EFFICIENCY_BONUS = 0.25
PROGRESS_SCALE = 8.0
PER_WORKLOAD_RELATIVE_CAP = 0.5
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
        "--baseline-dir",
        default=None,
        help="Defaults to <repo-root>/results_milestone3",
    )
    parser.add_argument(
        "--results-dir",
        default=None,
        help="Defaults to latest slurm_runs/<iteration>/results.* directory",
    )
    parser.add_argument(
        "--screen-job",
        default="",
        help="Optional job id or note for results.tsv",
    )
    parser.add_argument(
        "--full-job",
        default="",
        help="Optional job id or note for results.tsv",
    )
    parser.add_argument(
        "--status",
        default="keep",
        choices=["keep", "discard", "crash"],
    )
    parser.add_argument(
        "--change-summary",
        default="",
    )
    parser.add_argument(
        "--screen-notes",
        default="",
    )
    parser.add_argument(
        "--full-notes",
        default="",
    )
    parser.add_argument(
        "--novelty-score",
        type=float,
        default=1.0,
    )
    parser.add_argument(
        "--self-eval-abort",
        action="store_true",
    )
    parser.add_argument(
        "--no-log",
        action="store_true",
        help="Compute reward without updating reward_state.json or results.tsv",
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


def parse_raw_result_row(values: List[str], path: Path) -> Dict[str, str]:
    width = len(values)
    if width < 4:
        raise ValueError(f"Unrecognized row width {width} in {path}: {values}")

    row = {"index_name": values[0]}
    if width <= 6:
        row["build_time_ns1"] = values[1]
        row["index_size_bytes"] = values[2]
        row["mixed_throughput_mops1"] = values[3]
        if width >= 5:
            row["search_method"] = values[4]
        if width >= 6:
            row["value"] = values[5]
        return row

    if width <= 10:
        row["build_time_ns1"] = values[1]
        row["build_time_ns2"] = values[2]
        row["build_time_ns3"] = values[3]
        row["index_size_bytes"] = values[4]
        row["mixed_throughput_mops1"] = values[5]
        row["mixed_throughput_mops2"] = values[6]
        row["mixed_throughput_mops3"] = values[7]
        if width >= 9:
            row["search_method"] = values[8]
        if width >= 10:
            row["value"] = values[9]
        return row

    raise ValueError(f"Unrecognized row width {width} in {path}: {values}")


def load_csv_rows(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        raise FileNotFoundError(f"Missing CSV: {path}")
    with path.open(newline="", encoding="ascii") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        return []
    if rows[0] and rows[0][0] == "index_name":
        header = rows[0]
        return [dict(zip(header, row)) for row in rows[1:]]
    return [parse_raw_result_row(row, path) for row in rows]


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
            raise ValueError(f"Missing baseline rows in {path}")
        baselines[WORKLOAD_LABELS[(dataset_prefix, workload_token)]] = {
            "DynamicPGM": best_dpgm,
            "LIPP": best_lipp,
        }
    return baselines


def latest_results_dir(slurm_run_dir: Path) -> Path:
    candidates = [p for p in slurm_run_dir.glob("results.*") if p.is_dir()]
    if not candidates:
        raise FileNotFoundError(f"No results.* directories in {slurm_run_dir}")
    return max(candidates, key=lambda p: p.stat().st_mtime)


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


def load_reward_state(state_path: Path) -> Dict[str, Dict[str, float]]:
    if not state_path.exists():
        return {"best_hybrid_throughput": {}}
    with state_path.open(encoding="ascii") as handle:
        data = json.load(handle)
    if "best_hybrid_throughput" not in data:
        data["best_hybrid_throughput"] = {}
    return data


def compute_reward(
    baselines: Dict[str, Dict[str, float]],
    previous_best: Dict[str, float],
    current: Dict[str, float],
    novelty_score: float,
    self_eval_abort: bool,
) -> Tuple[float, Dict[str, Dict[str, float]], Dict[str, float]]:
    details: Dict[str, Dict[str, float]] = {}
    if not current:
        return PENALTY_INCOMPLETE, details, {"completion_ratio": 0.0}

    progress_reward = 0.0
    for label, h_cur in current.items():
        base = baselines[label]
        h_best_prev = previous_best.get(label, 0.0)
        delta = h_cur - h_best_prev
        reference_scale = max(h_best_prev, base["DynamicPGM"], base["LIPP"], 1e-9)
        relative_delta = delta / reference_scale
        capped_relative_delta = max(
            -PER_WORKLOAD_RELATIVE_CAP,
            min(PER_WORKLOAD_RELATIVE_CAP, relative_delta),
        )
        normalized_delta = PROGRESS_SCALE * capped_relative_delta
        solved = h_best_prev >= base["DynamicPGM"] and h_best_prev >= base["LIPP"]
        reward = min(0.0, normalized_delta) if solved else normalized_delta
        progress_reward += reward
        details[label] = {
            "baseline_dpgm": base["DynamicPGM"],
            "baseline_lipp": base["LIPP"],
            "capped_relative_delta": capped_relative_delta,
            "delta": delta,
            "h_best_prev": h_best_prev,
            "h_cur": h_cur,
            "normalized_delta": normalized_delta,
            "reference_scale": reference_scale,
            "relative_delta": relative_delta,
            "reward": reward,
            "solved_before": 1.0 if solved else 0.0,
        }

    completion_ratio = len(current) / len(baselines)
    completion_bonus = PARTIAL_COMPLETION_WEIGHT * completion_ratio
    missing_penalty = 0.0 if completion_ratio == 1.0 else PARTIAL_MISSING_WEIGHT * (1.0 - completion_ratio)
    novelty_bonus = NOVELTY_WEIGHT * (novelty_score - 0.5)
    efficiency_bonus = SELF_EVAL_EFFICIENCY_BONUS if self_eval_abort and current else 0.0
    total = progress_reward + completion_bonus - missing_penalty + novelty_bonus + efficiency_bonus
    components = {
        "completion_bonus": completion_bonus,
        "completion_ratio": completion_ratio,
        "efficiency_bonus": efficiency_bonus,
        "missing_penalty": missing_penalty,
        "novelty_bonus": novelty_bonus,
        "progress_reward": progress_reward,
    }
    return total, details, components


def append_results_row(
    results_tsv_path: Path,
    iteration: str,
    status: str,
    reward: float,
    screen_job: str,
    full_job: str,
    results_dir: Path,
    change_summary: str,
    screen_notes: str,
    full_notes: str,
) -> None:
    with results_tsv_path.open("a", newline="", encoding="ascii") as handle:
        writer = csv.writer(handle, delimiter="\t")
        writer.writerow(
            [
                iteration,
                status,
                f"{reward:.12g}",
                screen_job,
                full_job,
                str(results_dir),
                change_summary,
                screen_notes,
                full_notes,
            ]
        )


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root)
    baseline_dir = Path(args.baseline_dir) if args.baseline_dir else repo_root / "results_milestone3"
    reward_dir = repo_root / "autoresearch"
    state_path = reward_dir / "reward_state.json"
    results_tsv_path = reward_dir / "results.tsv"

    if args.results_dir:
        results_dir = Path(args.results_dir)
    else:
        results_dir = latest_results_dir(repo_root / "slurm_runs" / args.iteration)

    try:
        baselines = best_baselines(baseline_dir)
        state = load_reward_state(state_path)
        previous_best = state["best_hybrid_throughput"]
        current = current_hybrid_throughputs(results_dir)
        reward, details, components = compute_reward(
            baselines,
            previous_best,
            current,
            args.novelty_score,
            args.self_eval_abort,
        )
        output = {
            "components": components,
            "details": details,
            "iteration": args.iteration,
            "results_dir": str(results_dir),
            "reward": reward,
        }
    except (FileNotFoundError, KeyError, ValueError, OSError) as exc:
        reward = PENALTY_INCOMPLETE
        current = {}
        previous_best = {}
        state = {"best_hybrid_throughput": {}}
        output = {
            "components": {"completion_ratio": 0.0},
            "details": {},
            "error": str(exc),
            "iteration": args.iteration,
            "results_dir": str(results_dir),
            "reward": reward,
        }

    print(json.dumps(output, indent=2, sort_keys=True))

    if args.no_log:
        return

    if reward != PENALTY_INCOMPLETE and len(current) == len(WORKLOADS):
        for label, h_cur in current.items():
            prev = previous_best.get(label, 0.0)
            if h_cur > prev:
                previous_best[label] = h_cur
        state["best_hybrid_throughput"] = previous_best
        with state_path.open("w", encoding="ascii") as handle:
            json.dump(state, handle, indent=2, sort_keys=True)
            handle.write("\n")

    append_results_row(
        results_tsv_path=results_tsv_path,
        iteration=args.iteration,
        status=args.status,
        reward=reward,
        screen_job=args.screen_job,
        full_job=args.full_job,
        results_dir=results_dir,
        change_summary=args.change_summary,
        screen_notes=args.screen_notes,
        full_notes=args.full_notes,
    )


if __name__ == "__main__":
    main()
