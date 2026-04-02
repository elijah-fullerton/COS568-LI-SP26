#!/usr/bin/env python3

import argparse
import csv
import json
import sys
from pathlib import Path
from typing import Dict, List, Tuple


ABORT_EXIT_CODE = 42
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
    parser.add_argument("--repo-root", default=str(repo_root_default()))
    parser.add_argument("--results-dir", required=True)
    parser.add_argument("--mode", choices=["screen", "full"], required=True)
    parser.add_argument("--min-workloads", type=int, default=None)
    parser.add_argument("--abort-relative-threshold", type=float, default=None)
    parser.add_argument("--hard-abort-relative-threshold", type=float, default=None)
    parser.add_argument("--output", default="")
    return parser.parse_args()


def default_thresholds(mode: str) -> Tuple[int, float, float]:
    if mode == "screen":
        return 2, -0.04, -0.08
    return 1, -0.03, -0.06


def csv_name(dataset_prefix: str, workload_token: str) -> str:
    return (
        f"{dataset_prefix}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_"
        f"{workload_token}_0m_mix_results_table.csv"
    )


def screen_csv_name(workload_token: str) -> str:
    return (
        "fb_100M_public_uint64_ops_250000_0.000000rq_0.500000nl_"
        f"{workload_token}_0m_mix_results_table.csv"
    )


def parse_raw_result_row(values: List[str], path: Path) -> Dict[str, str]:
    width = len(values)
    if width < 4:
        raise ValueError(f"Unrecognized row width {width} in {path}")

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


def load_csv_rows(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="ascii") as handle:
        rows = list(csv.reader(handle))
    if not rows:
        return []
    if rows[0] and rows[0][0] == "index_name":
        return [dict(zip(rows[0], row)) for row in rows[1:]]
    return [parse_raw_result_row(row, path) for row in rows]


def average_throughput(row: Dict[str, str]) -> float:
    cols = [
        "mixed_throughput_mops1",
        "mixed_throughput_mops2",
        "mixed_throughput_mops3",
    ]
    vals = [float(row[c]) for c in cols if row.get(c, "")]
    if not vals:
        raise ValueError("No mixed throughput columns found")
    return sum(vals) / len(vals)


def load_hybrid_results(results_dir: Path, mode: str) -> Dict[str, float]:
    current: Dict[str, float] = {}
    if mode == "screen":
        workloads = [("fb", "0.100000i"), ("fb", "0.900000i")]
        for _, workload_token in workloads:
            path = results_dir / screen_csv_name(workload_token)
            rows = load_csv_rows(path)
            best = None
            for row in rows:
                if row["index_name"] != "HybridPGMLIPP":
                    continue
                avg = average_throughput(row)
                best = avg if best is None else max(best, avg)
            if best is not None:
                current[WORKLOAD_LABELS[("fb", workload_token)]] = best
        return current

    for dataset_prefix, workload_token in WORKLOADS:
        path = results_dir / csv_name(dataset_prefix, workload_token)
        rows = load_csv_rows(path)
        best = None
        for row in rows:
            if row["index_name"] != "HybridPGMLIPP":
                continue
            avg = average_throughput(row)
            best = avg if best is None else max(best, avg)
        if best is not None:
            current[WORKLOAD_LABELS[(dataset_prefix, workload_token)]] = best
    return current


def load_incumbent_results(repo_root: Path) -> Dict[str, float]:
    incumbent_results_dir = repo_root / "autoresearch" / "incumbent_results"
    if not incumbent_results_dir.exists():
        return {}
    return load_hybrid_results(incumbent_results_dir, "full")


def build_decision(
    repo_root: Path,
    results_dir: Path,
    mode: str,
    min_workloads: int,
    abort_relative_threshold: float,
    hard_abort_relative_threshold: float,
) -> Dict[str, object]:
    current = load_hybrid_results(results_dir, mode)
    incumbent = load_incumbent_results(repo_root)
    compared = []
    for label, cur in current.items():
        inc = incumbent.get(label)
        if inc is None or inc <= 0.0:
            continue
        rel = (cur / inc) - 1.0
        compared.append((label, cur, inc, rel))

    decision = {
        "mode": mode,
        "available_workloads": len(current),
        "compared_workloads": len(compared),
        "decision": "continue",
        "reason": "insufficient evidence",
        "details": [
            {
                "label": label,
                "current": cur,
                "incumbent": inc,
                "relative_delta": rel,
            }
            for label, cur, inc, rel in compared
        ],
    }

    if len(compared) < min_workloads:
        return decision

    avg_rel = sum(rel for _, _, _, rel in compared) / len(compared)
    max_rel = max(rel for _, _, _, rel in compared)
    if max_rel <= 0.0 and avg_rel <= hard_abort_relative_threshold:
        decision["decision"] = "abort"
        decision["reason"] = (
            "all compared workloads are below the incumbent and the average "
            "relative delta is strongly negative"
        )
    elif avg_rel <= abort_relative_threshold and len(compared) >= min_workloads + 1:
        decision["decision"] = "abort"
        decision["reason"] = (
            "the candidate remains materially below the incumbent after the "
            "minimum evidence window"
        )
    else:
        decision["reason"] = "candidate remains competitive enough to continue"

    decision["average_relative_delta"] = avg_rel if compared else None
    return decision


def main() -> None:
    args = parse_args()
    repo_root = Path(args.repo_root)
    results_dir = Path(args.results_dir)
    min_workloads, abort_relative_threshold, hard_abort_relative_threshold = default_thresholds(args.mode)
    if args.min_workloads is not None:
        min_workloads = args.min_workloads
    if args.abort_relative_threshold is not None:
        abort_relative_threshold = args.abort_relative_threshold
    if args.hard_abort_relative_threshold is not None:
        hard_abort_relative_threshold = args.hard_abort_relative_threshold
    decision = build_decision(
        repo_root=repo_root,
        results_dir=results_dir,
        mode=args.mode,
        min_workloads=min_workloads,
        abort_relative_threshold=abort_relative_threshold,
        hard_abort_relative_threshold=hard_abort_relative_threshold,
    )
    payload = json.dumps(decision, indent=2, sort_keys=True) + "\n"
    if args.output:
        Path(args.output).write_text(payload, encoding="ascii")
    sys.stdout.write(payload)
    if decision["decision"] == "abort":
        raise SystemExit(ABORT_EXIT_CODE)


if __name__ == "__main__":
    main()
