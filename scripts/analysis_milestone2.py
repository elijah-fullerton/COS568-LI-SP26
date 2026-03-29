from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import pandas as pd


RESULTS_DIR = Path("results")
ANALYSIS_DIR = Path("analysis_results")
DATASET = "fb"
DATASET_LABEL = "Facebook"
INDEX_ORDER = ["DynamicPGM", "LIPP", "HybridPGMLIPP"]
WORKLOADS = [
    {
        "slug": "mixed_90_insert",
        "label": "Mixed workload (90% insert, 10% lookup)",
        "filename": f"{DATASET}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv",
    },
    {
        "slug": "mixed_10_insert",
        "label": "Mixed workload (10% insert, 90% lookup)",
        "filename": f"{DATASET}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv",
    },
]
METRIC_COLUMNS = [
    "mixed_throughput_mops1",
    "mixed_throughput_mops2",
    "mixed_throughput_mops3",
]


def load_results(csv_path: Path) -> pd.DataFrame:
    if not csv_path.exists():
        raise FileNotFoundError(f"Missing benchmark result file: {csv_path}")
    frame = pd.read_csv(csv_path)
    frame["search_method"] = frame.get("search_method", "").fillna("")
    frame["value"] = frame.get("value", "").fillna("")
    return frame


def summarize_frame(frame: pd.DataFrame) -> pd.DataFrame:
    summarized = frame.copy()
    summarized["avg_metric"] = summarized[METRIC_COLUMNS].mean(axis=1)
    summarized["std_metric"] = summarized[METRIC_COLUMNS].std(axis=1, ddof=0)
    return summarized


def select_best_configs(frame: pd.DataFrame) -> pd.DataFrame:
    summarized = summarize_frame(frame)
    best_indices = summarized.groupby("index_name")["avg_metric"].idxmax()
    best = summarized.loc[best_indices].copy()
    order = {name: idx for idx, name in enumerate(INDEX_ORDER)}
    return best.sort_values(by="index_name", key=lambda col: col.map(order))


def build_summary_tables() -> tuple[pd.DataFrame, pd.DataFrame]:
    all_configs = []
    best_rows = []
    for workload in WORKLOADS:
        csv_path = RESULTS_DIR / workload["filename"]
        raw = summarize_frame(load_results(csv_path))
        raw["dataset"] = DATASET
        raw["dataset_label"] = DATASET_LABEL
        raw["workload"] = workload["slug"]
        raw["workload_label"] = workload["label"]
        all_configs.append(raw)

        best = select_best_configs(raw)
        best["dataset"] = DATASET
        best["dataset_label"] = DATASET_LABEL
        best["workload"] = workload["slug"]
        best["workload_label"] = workload["label"]
        best_rows.append(best)

    return pd.concat(all_configs, ignore_index=True), pd.concat(best_rows, ignore_index=True)


def save_tables(all_configs: pd.DataFrame, best_configs: pd.DataFrame) -> None:
    ANALYSIS_DIR.mkdir(exist_ok=True)
    all_configs.to_csv(ANALYSIS_DIR / "milestone2_all_configs.csv", index=False)
    best_configs.to_csv(ANALYSIS_DIR / "milestone2_best_configs.csv", index=False)

    pivot = best_configs.pivot_table(
        index=["dataset", "dataset_label", "index_name"],
        columns="workload_label",
        values=["avg_metric", "index_size_bytes"],
    )
    pivot.to_csv(ANALYSIS_DIR / "milestone2_summary_table.csv")


def plot_metric(best_configs: pd.DataFrame, value_column: str, ylabel: str, output_name: str) -> None:
    colors = {
        "DynamicPGM": "#F58518",
        "LIPP": "#54A24B",
        "HybridPGMLIPP": "#4C78A8",
    }
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))

    for ax, workload in zip(axes, WORKLOADS):
        subset = best_configs[best_configs["workload"] == workload["slug"]].copy()
        subset = subset.set_index("index_name").reindex(INDEX_ORDER).reset_index()
        ax.bar(
            subset["index_name"],
            subset[value_column],
            color=[colors[index_name] for index_name in subset["index_name"]],
        )
        ax.set_title(workload["label"])
        ax.set_xlabel("Index")
        ax.set_ylabel(ylabel)
        ax.tick_params(axis="x", rotation=0)

    fig.suptitle(f"Milestone 2 on {DATASET_LABEL}: {ylabel}", fontsize=14)
    fig.tight_layout(rect=[0, 0, 1, 0.95])
    fig.savefig(ANALYSIS_DIR / output_name, dpi=300)
    plt.close(fig)


def write_report(best_configs: pd.DataFrame) -> str:
    dpgm_rows = best_configs[best_configs["index_name"] == "DynamicPGM"]
    hybrid_rows = best_configs[best_configs["index_name"] == "HybridPGMLIPP"]

    lines = [
        "# Milestone 2 Report",
        "",
        "## Scope",
        "",
        "This milestone implements a naive hybrid of DynamicPGM and LIPP for the Facebook dataset only.",
        "The benchmarked workloads are the two mixed workloads required by the assignment:",
        "- Mixed workload with 90% inserts and 10% lookups.",
        "- Mixed workload with 10% inserts and 90% lookups.",
        "",
        "The hybrid design is intentionally simple:",
        "- Bulk-load the initial data into LIPP.",
        "- Route each new insert into a DynamicPGM buffer.",
        "- Search the DynamicPGM buffer first during lookups, then fall back to LIPP.",
        "- When the buffer reaches a fixed threshold, iterate over the buffered keys and insert them into LIPP one by one, then reset the buffer.",
        "",
        "## Best DynamicPGM configurations",
        "",
    ]

    for _, row in dpgm_rows.iterrows():
        lines.append(
            f"- {row['workload_label']}: search_method={row['search_method'] or 'n/a'}, "
            f"value={row['value'] or 'n/a'}, throughput={row['avg_metric']:.3f} Mops/s, "
            f"index_size_bytes={int(row['index_size_bytes'])}"
        )

    lines.extend(
        [
            "",
            "## Hybrid configuration",
            "",
        ]
    )

    for _, row in hybrid_rows.iterrows():
        lines.append(
            f"- {row['workload_label']}: variant={row['search_method'] or 'n/a'} "
            f"{row['value'] or ''}".strip()
            + f", throughput={row['avg_metric']:.3f} Mops/s, index_size_bytes={int(row['index_size_bytes'])}"
        )

    lines.extend(
        [
            "",
            "## Results summary",
            "",
            "The required four plots were generated in analysis_results/:",
            "- milestone2_throughput.png",
            "- milestone2_index_size.png",
            "",
            "The corresponding best-config tables were also written to:",
            "- analysis_results/milestone2_best_configs.csv",
            "- analysis_results/milestone2_summary_table.csv",
            "",
            "## Interpretation",
            "",
            "This naive hybrid is expected to trade off between the two baselines rather than dominate them.",
            "LIPP should remain strongest when lookups dominate, while DynamicPGM should stay strongest when inserts dominate.",
            "The hybrid pays an extra flush cost because buffered keys are migrated into LIPP one record at a time.",
        ]
    )

    report_text = "\n".join(lines)
    (ANALYSIS_DIR / "milestone2_report.md").write_text(report_text, encoding="ascii")
    return report_text


def main() -> None:
    all_configs, best_configs = build_summary_tables()
    save_tables(all_configs, best_configs)
    plot_metric(best_configs, "avg_metric", "Throughput (Mops/s)", "milestone2_throughput.png")
    plot_metric(best_configs, "index_size_bytes", "Index size (bytes)", "milestone2_index_size.png")
    report_text = write_report(best_configs)
    print(report_text)
    print("Wrote milestone 2 outputs to analysis_results/")


if __name__ == "__main__":
    main()
