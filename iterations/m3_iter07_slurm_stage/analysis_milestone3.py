from pathlib import Path
import os

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import pandas as pd


RESULTS_DIR = Path(os.environ.get("RESULTS_DIR", "results_milestone3"))
INDEX_ORDER = ["DynamicPGM", "LIPP", "HybridPGMLIPP"]
DATASETS = [
    ("books", "Books"),
    ("fb", "Facebook"),
    ("osmc", "OSMC"),
]
WORKLOADS = [
    ("mixed_90_insert", "Mixed Throughput (90% Insert, 10% Lookup)", "0.900000i"),
    ("mixed_10_insert", "Mixed Throughput (10% Insert, 90% Lookup)", "0.100000i"),
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


def dataset_csv_name(dataset_prefix: str, workload_token: str) -> str:
    return (
        f"{dataset_prefix}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_"
        f"{workload_token}_0m_mix_results_table.csv"
    )


def build_summary() -> pd.DataFrame:
    rows = []
    for dataset_prefix, dataset_label in DATASETS:
        for workload_slug, workload_label, workload_token in WORKLOADS:
            best = select_best_rows(
                load_results(RESULTS_DIR / dataset_csv_name(dataset_prefix, workload_token))
            )
            for _, row in best.iterrows():
                rows.append(
                    {
                        "dataset": dataset_prefix,
                        "dataset_label": dataset_label,
                        "workload": workload_slug,
                        "workload_label": workload_label,
                        "index_name": row["index_name"],
                        "avg_throughput_mops": row["avg_throughput_mops"],
                        "std_throughput_mops": row["std_throughput_mops"],
                        "index_size_bytes": row["index_size_bytes"],
                        "search_method": row["search_method"],
                        "value": row["value"],
                    }
                )
    summary = pd.DataFrame(rows)
    summary.to_csv(RESULTS_DIR / "milestone3_best_configs.csv", index=False)
    return summary


def plot_metric(summary: pd.DataFrame, dataset_prefix: str, dataset_label: str,
                workload_slug: str, workload_label: str, metric: str,
                ylabel: str, output_name: str) -> None:
    subset = summary[
        (summary["dataset"] == dataset_prefix) & (summary["workload"] == workload_slug)
    ].copy()
    subset["index_name"] = pd.Categorical(subset["index_name"], INDEX_ORDER, ordered=True)
    subset = subset.sort_values("index_name")

    fig, ax = plt.subplots(figsize=(8, 5))
    colors = ["#4C78A8", "#F58518", "#54A24B"]
    ax.bar(subset["index_name"], subset[metric], color=colors, width=0.65)
    ax.set_title(f"{dataset_label}: {workload_label}")
    ax.set_xlabel("Index")
    ax.set_ylabel(ylabel)
    ax.tick_params(axis="x", rotation=0)
    fig.tight_layout()
    fig.savefig(RESULTS_DIR / output_name, dpi=300)
    plt.close(fig)


def save_report(summary: pd.DataFrame) -> None:
    lines = [
        "# Milestone 3 Report",
        "",
        "Best mixed-workload configurations across Books, Facebook, and OSMC.",
        "",
    ]

    for dataset_prefix, dataset_label in DATASETS:
        lines.append(f"## {dataset_label}")
        lines.append("")
        for workload_slug, workload_label, _ in WORKLOADS:
            lines.append(f"### {workload_label}")
            subset = summary[
                (summary["dataset"] == dataset_prefix)
                & (summary["workload"] == workload_slug)
            ]
            for _, row in subset.iterrows():
                lines.append(
                    f"- {row['index_name']}: throughput={row['avg_throughput_mops']:.3f} Mops/s, "
                    f"index_size={int(row['index_size_bytes'])} bytes, "
                    f"variant=({row['search_method'] or 'n/a'}, {row['value'] or 'n/a'})"
                )
            lines.append("")

    (RESULTS_DIR / "milestone3_report.md").write_text("\n".join(lines), encoding="ascii")


def main() -> None:
    summary = build_summary()
    save_report(summary)

    for dataset_prefix, dataset_label in DATASETS:
        for workload_slug, workload_label, _ in WORKLOADS:
            plot_metric(
                summary,
                dataset_prefix,
                dataset_label,
                workload_slug,
                workload_label,
                "avg_throughput_mops",
                "Throughput (Mops/s)",
                f"throughput_{dataset_prefix}_{workload_slug}.png",
            )
            plot_metric(
                summary,
                dataset_prefix,
                dataset_label,
                workload_slug,
                workload_label,
                "index_size_bytes",
                "Index Size (bytes)",
                f"index_size_{dataset_prefix}_{workload_slug}.png",
            )

    print(f"Wrote Milestone 3 outputs to {RESULTS_DIR}/")


if __name__ == "__main__":
    main()
