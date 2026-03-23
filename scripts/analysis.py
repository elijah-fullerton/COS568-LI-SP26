from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import pandas as pd


RESULTS_DIR = Path("results")
ANALYSIS_DIR = Path("analysis_results")
DATASETS = {
    "fb": "Facebook",
    "books": "Books",
    "osmc": "OSMC",
}
INDEX_ORDER = ["BTree", "DynamicPGM", "LIPP"]
WORKLOADS = [
    {
        "slug": "lookup_only",
        "label": "Lookup-only",
        "filename": "{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.000000i_results_table.csv",
        "metric_columns": ["lookup_throughput_mops1", "lookup_throughput_mops2", "lookup_throughput_mops3"],
        "metric_name": "lookup_throughput_mops",
    },
    {
        "slug": "insert_lookup_insert",
        "label": "Insert throughput (50% insert, 50% lookup)",
        "filename": "{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.500000i_0m_results_table.csv",
        "metric_columns": ["insert_throughput_mops1", "insert_throughput_mops2", "insert_throughput_mops3"],
        "metric_name": "insert_throughput_mops",
    },
    {
        "slug": "insert_lookup_lookup",
        "label": "Post-insert lookup throughput (50% insert, 50% lookup)",
        "filename": "{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.500000i_0m_results_table.csv",
        "metric_columns": ["lookup_throughput_mops1", "lookup_throughput_mops2", "lookup_throughput_mops3"],
        "metric_name": "lookup_throughput_mops",
    },
    {
        "slug": "mixed_10_insert",
        "label": "Mixed throughput (10% insert)",
        "filename": "{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv",
        "metric_columns": ["mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3"],
        "metric_name": "mixed_throughput_mops",
    },
    {
        "slug": "mixed_90_insert",
        "label": "Mixed throughput (90% insert)",
        "filename": "{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv",
        "metric_columns": ["mixed_throughput_mops1", "mixed_throughput_mops2", "mixed_throughput_mops3"],
        "metric_name": "mixed_throughput_mops",
    },
]


def load_results(csv_path: Path) -> pd.DataFrame:
    if not csv_path.exists():
        raise FileNotFoundError(f"Missing benchmark result file: {csv_path}")
    return pd.read_csv(csv_path)


def normalize_variant_columns(frame: pd.DataFrame) -> pd.DataFrame:
    normalized = frame.copy()
    if "search_method" not in normalized.columns:
        normalized["search_method"] = ""
    if "value" not in normalized.columns:
        normalized["value"] = ""
    normalized["search_method"] = normalized["search_method"].fillna("")
    normalized["value"] = normalized["value"].fillna("")
    return normalized


def select_best_rows(frame: pd.DataFrame, metric_columns: list[str]) -> pd.DataFrame:
    enriched = normalize_variant_columns(frame)
    enriched["avg_metric"] = enriched[metric_columns].mean(axis=1)
    enriched["std_metric"] = enriched[metric_columns].std(axis=1, ddof=0)
    best_indices = enriched.groupby("index_name")["avg_metric"].idxmax()
    best = enriched.loc[best_indices].copy()
    return best.sort_values(
        by="index_name",
        key=lambda col: col.map({name: idx for idx, name in enumerate(INDEX_ORDER)}),
    )


def build_summary_tables() -> tuple[pd.DataFrame, pd.DataFrame]:
    best_config_rows = []
    plot_rows = []

    for dataset_slug, dataset_label in DATASETS.items():
        for workload in WORKLOADS:
            csv_path = RESULTS_DIR / workload["filename"].format(dataset=dataset_slug)
            best_rows = select_best_rows(load_results(csv_path), workload["metric_columns"])

            for _, row in best_rows.iterrows():
                best_config_rows.append(
                    {
                        "dataset": dataset_slug,
                        "dataset_label": dataset_label,
                        "workload": workload["slug"],
                        "workload_label": workload["label"],
                        "index_name": row["index_name"],
                        "metric_name": workload["metric_name"],
                        "avg_metric": row["avg_metric"],
                        "std_metric": row["std_metric"],
                        "index_size_bytes": row["index_size_bytes"],
                        "build_time_ns1": row["build_time_ns1"],
                        "build_time_ns2": row["build_time_ns2"],
                        "build_time_ns3": row["build_time_ns3"],
                        "search_method": row["search_method"],
                        "value": row["value"],
                    }
                )
                plot_rows.append(
                    {
                        "dataset": dataset_slug,
                        "dataset_label": dataset_label,
                        "workload": workload["slug"],
                        "workload_label": workload["label"],
                        "index_name": row["index_name"],
                        "avg_metric": row["avg_metric"],
                    }
                )

    best_configs = pd.DataFrame(best_config_rows)
    plot_frame = pd.DataFrame(plot_rows)
    return best_configs, plot_frame


def save_summary_outputs(best_configs: pd.DataFrame) -> None:
    ANALYSIS_DIR.mkdir(exist_ok=True)

    best_configs.to_csv(ANALYSIS_DIR / "task1_best_configs.csv", index=False)

    pivot = best_configs.pivot_table(
        index=["dataset", "dataset_label", "index_name"],
        columns="workload_label",
        values="avg_metric",
    ).reset_index()
    pivot.to_csv(ANALYSIS_DIR / "task1_summary_table.csv", index=False)

    lines = [
        "# Task 1 Benchmark Summary",
        "",
        "This file lists the best-performing configuration for each index under each dataset/workload pair.",
        "",
    ]

    for workload in WORKLOADS:
        lines.append(f"## {workload['label']}")
        subset = best_configs[best_configs["workload"] == workload["slug"]]
        for dataset_slug, dataset_label in DATASETS.items():
            lines.append(f"### {dataset_label}")
            dataset_rows = subset[subset["dataset"] == dataset_slug]
            if dataset_rows.empty:
                lines.append("No data found.")
                lines.append("")
                continue
            for _, row in dataset_rows.iterrows():
                lines.append(
                    f"- {row['index_name']}: {row['avg_metric']:.3f} Mops/s "
                    f"(search_method={row['search_method'] or 'n/a'}, value={row['value'] or 'n/a'}, "
                    f"index_size_bytes={int(row['index_size_bytes'])})"
                )
            lines.append("")

    (ANALYSIS_DIR / "task1_report_summary.md").write_text("\n".join(lines), encoding="ascii")


def plot_results(plot_frame: pd.DataFrame) -> None:
    ANALYSIS_DIR.mkdir(exist_ok=True)

    fig, axes = plt.subplots(3, 2, figsize=(16, 16))
    axes = axes.flatten()
    colors = ["#4C78A8", "#F58518", "#54A24B"]

    for plot_index, workload in enumerate(WORKLOADS):
        ax = axes[plot_index]
        subset = plot_frame[plot_frame["workload"] == workload["slug"]]
        pivot = subset.pivot(index="index_name", columns="dataset_label", values="avg_metric")
        pivot = pivot.reindex(INDEX_ORDER)
        pivot = pivot.reindex(columns=[DATASETS[key] for key in DATASETS.keys()])

        pivot.plot(kind="bar", ax=ax, color=colors, width=0.8)
        ax.set_title(workload["label"])
        ax.set_xlabel("Index")
        ax.set_ylabel("Throughput (Mops/s)")
        ax.tick_params(axis="x", rotation=0)
        ax.legend(title="Dataset")

    axes[-1].axis("off")
    fig.suptitle("Task 1 Throughput Comparison", fontsize=16)
    fig.tight_layout(rect=[0, 0, 1, 0.98])
    fig.savefig(ANALYSIS_DIR / "task1_throughput_summary.png", dpi=300)
    plt.close(fig)


def main() -> None:
    best_configs, plot_frame = build_summary_tables()
    save_summary_outputs(best_configs)
    plot_results(plot_frame)
    print("Wrote analysis outputs to analysis_results/")


if __name__ == "__main__":
    main()