#!/usr/bin/env python3

import argparse
import csv
from pathlib import Path


WORKLOADS = [
    ("books", "0.100000i", "books_mixed_10_insert"),
    ("books", "0.900000i", "books_mixed_90_insert"),
    ("fb", "0.100000i", "fb_mixed_10_insert"),
    ("fb", "0.900000i", "fb_mixed_90_insert"),
    ("osmc", "0.100000i", "osmc_mixed_10_insert"),
    ("osmc", "0.900000i", "osmc_mixed_90_insert"),
]


def avg(values):
    return sum(values) / len(values)


def csv_name(dataset_prefix: str, workload_token: str) -> str:
    return (
        f"{dataset_prefix}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_"
        f"{workload_token}_0m_mix_results_table.csv"
    )


def load_rows(path: Path):
    with path.open(newline="", encoding="ascii") as handle:
        rows = list(csv.DictReader(handle))
    return rows


def best_baseline_rows(baseline_dir: Path):
    baselines = {}
    for dataset, token, label in WORKLOADS:
        rows = load_rows(baseline_dir / csv_name(dataset, token))
        best_dpgm = max(
            avg(
                [
                    float(r["mixed_throughput_mops1"]),
                    float(r["mixed_throughput_mops2"]),
                    float(r["mixed_throughput_mops3"]),
                ]
            )
            for r in rows
            if r["index_name"] == "DynamicPGM"
        )
        best_lipp = max(
            avg(
                [
                    float(r["mixed_throughput_mops1"]),
                    float(r["mixed_throughput_mops2"]),
                    float(r["mixed_throughput_mops3"]),
                ]
            )
            for r in rows
            if r["index_name"] == "LIPP"
        )
        baselines[label] = {"DynamicPGM": best_dpgm, "LIPP": best_lipp}
    return baselines


def milestone2_naive_rows(m2_dir: Path):
    naive = {
        "fb_mixed_90_insert": 1.917387,
        "fb_mixed_10_insert": 1.026568,
    }
    path = m2_dir / "milestone2_best_configs.csv"
    if path.exists():
        rows = load_rows(path)
        for row in rows:
            workload = row.get("workload", "")
            index_name = row.get("index_name", "")
            if index_name != "HybridPGMLIPP":
                continue
            avg_val = float(row.get("avg_throughput_mops", "0"))
            if workload == "mixed_90_insert":
                naive["fb_mixed_90_insert"] = avg_val
            elif workload == "mixed_10_insert":
                naive["fb_mixed_10_insert"] = avg_val
    return naive


def current_hybrid_rows(results_dir: Path):
    current = {}
    for dataset, token, label in WORKLOADS:
        rows = load_rows(results_dir / csv_name(dataset, token))
        hybrids = [
            (
                avg(
                    [
                        float(r["mixed_throughput_mops1"]),
                        float(r["mixed_throughput_mops2"]),
                        float(r["mixed_throughput_mops3"]),
                    ]
                ),
                r.get("search_method", ""),
                r.get("value", ""),
            )
            for r in rows
            if r["index_name"] == "HybridPGMLIPP"
        ]
        current[label] = max(hybrids, key=lambda x: x[0])
    return current


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--results-dir", required=True)
    args = parser.parse_args()

    repo_root = Path(args.repo_root)
    results_dir = Path(args.results_dir)
    baselines = best_baseline_rows(repo_root / "results_milestone3")
    naive = milestone2_naive_rows(repo_root / "results_milestone2")
    current = current_hybrid_rows(results_dir)

    print("workload,hybrid_avg,baseline_dpgm,baseline_lipp,naive_m2,beats_dpgm,beats_lipp,beats_m2")
    for _, _, label in WORKLOADS:
        hybrid_avg, search_method, value = current[label]
        baseline_dpgm = baselines[label]["DynamicPGM"]
        baseline_lipp = baselines[label]["LIPP"]
        naive_val = naive.get(label, float("nan"))
        beats_m2 = hybrid_avg > naive_val if naive_val == naive_val else ""
        print(
            f"{label},{hybrid_avg:.6f},{baseline_dpgm:.6f},{baseline_lipp:.6f},"
            f"{naive_val if naive_val == naive_val else ''},"
            f"{int(hybrid_avg > baseline_dpgm)},{int(hybrid_avg > baseline_lipp)},"
            f"{int(beats_m2) if beats_m2 != '' else ''}"
        )
        print(f"best_variant[{label}]={search_method},{value}")


if __name__ == "__main__":
    main()
