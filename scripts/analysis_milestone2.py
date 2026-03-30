from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import pandas as pd


RESULTS_DIR = Path("results_milestone2")
INDEX_ORDER = ["DynamicPGM", "LIPP", "HybridPGMLIPP"]
WORKLOADS = [
    {
        "slug": "mixed_90_insert",
        "label": "Mixed Throughput (90% Insert, 10% Lookup)",
        "filename": "fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv",
    },
    {
        "slug": "mixed_10_insert",
        "label": "Mixed Throughput (10% Insert, 90% Lookup)",
        "filename": "fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv",
    },
]


def load_results(csv_path: Path) -> pd.DataFrame:
    if not csv_path.exists():
        raise FileNotFoundError(f"Missing benchmark result file: {csv_path}")
    frame = pd.read_csv(csv_path)
    frame["search_method"] = frame.get("search_method", "").fillna("")
    frame["value"] = frame.get("value", "").fillna("")
    frame["avg_throughput_mops"] = frame[
        [
            "mixed_throughput_mops1",
            "mixed_throughput_mops2",
            "mixed_throughput_mops3",
        ]
    ].mean(axis=1)
    frame["std_throughput_mops"] = frame[
        [
            "mixed_throughput_mops1",
            "mixed_throughput_mops2",
            "mixed_throughput_mops3",
        ]
    ].std(axis=1, ddof=0)
    return frame


def select_best_rows(frame: pd.DataFrame) -> pd.DataFrame:
    best_indices = frame.groupby("index_name")["avg_throughput_mops"].idxmax()
    best = frame.loc[best_indices].copy()
    return best.sort_values(
        by="index_name",
        key=lambda col: col.map({name: idx for idx, name in enumerate(INDEX_ORDER)}),
    )


def build_summary() -> pd.DataFrame:
    rows = []
    for workload in WORKLOADS:
        best = select_best_rows(load_results(RESULTS_DIR / workload["filename"]))
        for _, row in best.iterrows():
            rows.append(
                {
                    "workload": workload["slug"],
                    "workload_label": workload["label"],
                    "index_name": row["index_name"],
                    "avg_throughput_mops": row["avg_throughput_mops"],
                    "std_throughput_mops": row["std_throughput_mops"],
                    "index_size_bytes": row["index_size_bytes"],
                    "search_method": row["search_method"],
                    "value": row["value"],
                }
            )
    summary = pd.DataFrame(rows)
    summary.to_csv(RESULTS_DIR / "milestone2_best_configs.csv", index=False)
    return summary


def save_report(summary: pd.DataFrame) -> None:
    lines = [
        "# Milestone 2 Report",
        "",
        "Facebook mixed-workload results comparing DynamicPGM, LIPP, and the hybrid DPGM+LIPP index.",
        "",
    ]

    for workload in WORKLOADS:
        lines.append(f"## {workload['label']}")
        subset = summary[summary["workload"] == workload["slug"]]
        for _, row in subset.iterrows():
            lines.append(
                f"- {row['index_name']}: throughput={row['avg_throughput_mops']:.3f} Mops/s, "
                f"index_size={int(row['index_size_bytes'])} bytes, "
                f"variant=({row['search_method'] or 'n/a'}, {row['value'] or 'n/a'})"
            )
        lines.append("")

    (RESULTS_DIR / "milestone2_report.md").write_text("\n".join(lines), encoding="ascii")


def plot_metric(summary: pd.DataFrame, workload: dict, metric: str, ylabel: str, output_name: str) -> None:
    subset = summary[summary["workload"] == workload["slug"]].copy()
    subset["index_name"] = pd.Categorical(subset["index_name"], INDEX_ORDER, ordered=True)
    subset = subset.sort_values("index_name")

    fig, ax = plt.subplots(figsize=(8, 5))
    colors = ["#4C78A8", "#F58518", "#54A24B"]
    ax.bar(subset["index_name"], subset[metric], color=colors, width=0.65)
    ax.set_title(workload["label"])
    ax.set_xlabel("Index")
    ax.set_ylabel(ylabel)
    ax.tick_params(axis="x", rotation=0)
    fig.tight_layout()
    fig.savefig(RESULTS_DIR / output_name, dpi=300)
    plt.close(fig)


def plot_overview(summary: pd.DataFrame) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    colors = ["#4C78A8", "#F58518", "#54A24B"]

    for row_index, metric in enumerate(
        [
            ("avg_throughput_mops", "Throughput (Mops/s)"),
            ("index_size_bytes", "Index Size (bytes)"),
        ]
    ):
        for col_index, workload in enumerate(WORKLOADS):
            subset = summary[summary["workload"] == workload["slug"]].copy()
            subset["index_name"] = pd.Categorical(subset["index_name"], INDEX_ORDER, ordered=True)
            subset = subset.sort_values("index_name")

            ax = axes[row_index][col_index]
            ax.bar(subset["index_name"], subset[metric[0]], color=colors, width=0.65)
            ax.set_title(workload["label"])
            ax.set_xlabel("Index")
            ax.set_ylabel(metric[1])
            ax.tick_params(axis="x", rotation=0)

    fig.tight_layout()
    fig.savefig(RESULTS_DIR / "milestone2_summary.png", dpi=300)
    plt.close(fig)


def main() -> None:
    summary = build_summary()
    save_report(summary)

    plot_metric(
        summary,
        WORKLOADS[0],
        "avg_throughput_mops",
        "Throughput (Mops/s)",
        "throughput_mixed_90_insert.png",
    )
    plot_metric(
        summary,
        WORKLOADS[1],
        "avg_throughput_mops",
        "Throughput (Mops/s)",
        "throughput_mixed_10_insert.png",
    )
    plot_metric(
        summary,
        WORKLOADS[0],
        "index_size_bytes",
        "Index Size (bytes)",
        "index_size_mixed_90_insert.png",
    )
    plot_metric(
        summary,
        WORKLOADS[1],
        "index_size_bytes",
        "Index Size (bytes)",
        "index_size_mixed_10_insert.png",
    )
    plot_overview(summary)
    print("Wrote Milestone 2 outputs to results_milestone2/")


if __name__ == "__main__":
    main()
