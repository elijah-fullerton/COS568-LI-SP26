#!/usr/bin/env python3

import csv
import math
import os
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path("/auto/u/ef0952/projects/COS568-LI-SP26")
BASELINES = ROOT / "results_milestone3"
FINAL = ROOT / "results_milestone3_final"

WORKLOADS = [
    ("books", "0.100000i", "Mixed 10% Insert / 90% Lookup"),
    ("books", "0.900000i", "Mixed 90% Insert / 10% Lookup"),
    ("fb", "0.100000i", "Mixed 10% Insert / 90% Lookup"),
    ("fb", "0.900000i", "Mixed 90% Insert / 10% Lookup"),
    ("osmc", "0.100000i", "Mixed 10% Insert / 90% Lookup"),
    ("osmc", "0.900000i", "Mixed 90% Insert / 10% Lookup"),
]


def mean(vals):
    return sum(vals) / len(vals)


FINAL_FIELDNAMES = [
    "index_name",
    "build_time_ns1",
    "build_time_ns2",
    "build_time_ns3",
    "index_size_bytes",
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
    "search_method",
    "value",
]


def read_rows(path, has_header=True):
    with open(path) as f:
        if has_header:
            return list(csv.DictReader(f))
        return list(csv.DictReader(f, fieldnames=FINAL_FIELDNAMES))


def throughput_cols(row):
    return [c for c in row.keys() if c.startswith("mixed_throughput_mops")]


def best_baselines(rows):
    best = {}
    for row in rows:
        name = row["index_name"]
        cols = throughput_cols(row)
        vals = [float(row[c]) for c in cols if row[c]]
        avg = mean(vals)
        if name not in best or avg > best[name]["avg_throughput"]:
            config = ""
            if name == "DynamicPGM":
                config = f"{row.get('search_method', '')}-{row.get('value', '')}"
            elif name == "HybridPGMLIPP":
                config = row.get("value", "")
            best[name] = {
                "avg_throughput": avg,
                "index_size_bytes": int(row["index_size_bytes"]),
                "config": config,
            }
    return best


def best_final(rows):
    best = None
    for row in rows:
        vals = [float(row[c]) for c in throughput_cols(row) if row[c]]
        avg = mean(vals)
        candidate = {
            "avg_throughput": avg,
            "throughput_runs": vals,
            "index_size_bytes": int(row["index_size_bytes"]),
            "config": row.get("value", ""),
            "row": row,
        }
        if best is None or candidate["avg_throughput"] > best["avg_throughput"]:
            best = candidate
    return best


def save_summary(summary_rows):
    out = FINAL / "final_summary.csv"
    with open(out, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "dataset",
                "insert_ratio",
                "workload_label",
                "winning_config",
                "avg_throughput_mops",
                "index_size_bytes",
                "baseline_dynamicpgm_mops",
                "baseline_lipp_mops",
                "baseline_hybrid_mops",
                "beats_dynamicpgm",
                "beats_lipp",
                "beats_prior_hybrid",
            ],
        )
        writer.writeheader()
        writer.writerows(summary_rows)


def save_best_configs(summary_rows):
    out = FINAL / "milestone3_best_configs.csv"
    with open(out, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "dataset",
                "insert_ratio",
                "workload",
                "winning_config",
                "avg_throughput_mops",
                "index_size_bytes",
            ]
        )
        for row in summary_rows:
            writer.writerow(
                [
                    row["dataset"],
                    row["insert_ratio"],
                    row["workload_label"],
                    row["winning_config"],
                    row["avg_throughput_mops"],
                    row["index_size_bytes"],
                ]
            )


def plot_metric(dataset, insert_ratio, label, baseline, final):
    insert_tag = "10_insert" if insert_ratio == "0.100000i" else "90_insert"
    throughput_path = FINAL / f"throughput_{dataset}_mixed_{insert_tag}.png"
    size_path = FINAL / f"index_size_{dataset}_mixed_{insert_tag}.png"

    labels = ["DynamicPGM", "LIPP", "HybridPGMLIPP"]
    throughput_vals = [
        baseline["DynamicPGM"]["avg_throughput"],
        baseline["LIPP"]["avg_throughput"],
        final["avg_throughput"],
    ]
    size_vals = [
        baseline["DynamicPGM"]["index_size_bytes"],
        baseline["LIPP"]["index_size_bytes"],
        final["index_size_bytes"],
    ]

    plt.figure(figsize=(6.4, 4.2))
    plt.bar(labels, throughput_vals, color=["#4C78A8", "#F58518", "#54A24B"])
    plt.ylabel("Throughput (Mops/s)")
    plt.title(f"{dataset.upper()} {label}")
    plt.tight_layout()
    plt.savefig(throughput_path, dpi=180)
    plt.close()

    plt.figure(figsize=(6.4, 4.2))
    plt.bar(labels, size_vals, color=["#4C78A8", "#F58518", "#54A24B"])
    plt.ylabel("Index Size (bytes)")
    plt.title(f"{dataset.upper()} {label}")
    plt.ticklabel_format(style="plain", axis="y")
    plt.tight_layout()
    plt.savefig(size_path, dpi=180)
    plt.close()


def write_markdown(summary_rows):
    lines = [
        "# Milestone 3 Report",
        "",
        "## Final Summary",
        "",
        "The final split hybrid uses a bloom-filter-guided lookup-heavy lane and a classic owner-buffered insert-heavy lane.",
        "",
        "## Final Best Configurations",
        "",
        "| Dataset | Workload | Best config | Avg throughput (Mops/s) | Index size (bytes) | Beats LIPP? |",
        "| --- | --- | --- | ---: | ---: | --- |",
    ]
    for row in summary_rows:
        lines.append(
            f"| {row['dataset']} | {row['workload_label']} | {row['winning_config']} | "
            f"{float(row['avg_throughput_mops']):.3f} | {int(row['index_size_bytes']):,} | "
            f"{row['beats_lipp']} |"
        )
    lines.extend(
        [
            "",
            "## Notes",
            "",
            "- Lookup-heavy runs use the bloom-enabled `HybridPGMLIPP` implementation.",
            "- Insert-heavy runs use `HybridPGMLIPPClassic` to preserve the stronger insert path.",
            "- All final data points are generated with `--verify -r 3` in one benchmark session per workload.",
        ]
    )
    (FINAL / "milestone3_report.md").write_text("\n".join(lines))


def main():
    FINAL.mkdir(exist_ok=True)
    summary_rows = []
    for dataset, insert_ratio, label in WORKLOADS:
        baseline_path = BASELINES / (
            f"{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_{insert_ratio}_0m_mix_results_table.csv"
        )
        final_path = FINAL / baseline_path.name
        baseline_rows = read_rows(baseline_path, has_header=True)
        final_rows = read_rows(final_path, has_header=False)
        baseline = best_baselines(baseline_rows)
        final = best_final(final_rows)
        row = {
            "dataset": dataset,
            "insert_ratio": insert_ratio,
            "workload_label": label,
            "winning_config": final["config"],
            "avg_throughput_mops": f"{final['avg_throughput']:.6f}",
            "index_size_bytes": str(final["index_size_bytes"]),
            "baseline_dynamicpgm_mops": f"{baseline['DynamicPGM']['avg_throughput']:.6f}",
            "baseline_lipp_mops": f"{baseline['LIPP']['avg_throughput']:.6f}",
            "baseline_hybrid_mops": f"{baseline['HybridPGMLIPP']['avg_throughput']:.6f}",
            "beats_dynamicpgm": str(final["avg_throughput"] > baseline["DynamicPGM"]["avg_throughput"]),
            "beats_lipp": str(final["avg_throughput"] > baseline["LIPP"]["avg_throughput"]),
            "beats_prior_hybrid": str(final["avg_throughput"] > baseline["HybridPGMLIPP"]["avg_throughput"]),
        }
        summary_rows.append(row)
        plot_metric(dataset, insert_ratio, label, baseline, final)
    save_summary(summary_rows)
    save_best_configs(summary_rows)
    write_markdown(summary_rows)


if __name__ == "__main__":
    main()
