from __future__ import annotations

import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt


REPO_ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = REPO_ROOT / "results"
REPORT_DIR = REPO_ROOT / "reports" / "task1"
GENERATED_DIR = REPORT_DIR / "generated"
FIGURES_DIR = REPORT_DIR / "figures"

DATASETS = {
    "fb": "Facebook",
    "books": "Books",
    "osmc": "OSMC",
}
INDEX_ORDER = ["BTree", "DynamicPGM", "LIPP"]
INDEX_COLORS = {
    "BTree": "#4C78A8",
    "DynamicPGM": "#F58518",
    "LIPP": "#54A24B",
}


@dataclass(frozen=True)
class WorkloadSpec:
    slug: str
    short_label: str
    long_label: str
    filename_pattern: str
    metric_columns: tuple[str, ...]
    metric_key: str


WORKLOADS = (
    WorkloadSpec(
        slug="lookup_only",
        short_label="Lookup-only",
        long_label="Lookup-only throughput",
        filename_pattern="{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.000000i_results_table.csv",
        metric_columns=(
            "lookup_throughput_mops1",
            "lookup_throughput_mops2",
            "lookup_throughput_mops3",
        ),
        metric_key="lookup_throughput_mops",
    ),
    WorkloadSpec(
        slug="insert_throughput",
        short_label="Insert throughput (50/50)",
        long_label="Insert throughput, 50% insert / 50% lookup",
        filename_pattern="{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.500000i_0m_results_table.csv",
        metric_columns=(
            "insert_throughput_mops1",
            "insert_throughput_mops2",
            "insert_throughput_mops3",
        ),
        metric_key="insert_throughput_mops",
    ),
    WorkloadSpec(
        slug="post_insert_lookup",
        short_label="Lookup after insert (50/50)",
        long_label="Post-insert lookup throughput, 50% insert / 50% lookup",
        filename_pattern="{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.500000i_0m_results_table.csv",
        metric_columns=(
            "lookup_throughput_mops1",
            "lookup_throughput_mops2",
            "lookup_throughput_mops3",
        ),
        metric_key="lookup_throughput_mops",
    ),
    WorkloadSpec(
        slug="mixed_10_insert",
        short_label="Mixed throughput (10% insert)",
        long_label="Mixed throughput, 10% insert",
        filename_pattern="{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv",
        metric_columns=(
            "mixed_throughput_mops1",
            "mixed_throughput_mops2",
            "mixed_throughput_mops3",
        ),
        metric_key="mixed_throughput_mops",
    ),
    WorkloadSpec(
        slug="mixed_90_insert",
        short_label="Mixed throughput (90% insert)",
        long_label="Mixed throughput, 90% insert",
        filename_pattern="{dataset}_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv",
        metric_columns=(
            "mixed_throughput_mops1",
            "mixed_throughput_mops2",
            "mixed_throughput_mops3",
        ),
        metric_key="mixed_throughput_mops",
    ),
)


def ensure_directories() -> None:
    GENERATED_DIR.mkdir(parents=True, exist_ok=True)
    FIGURES_DIR.mkdir(parents=True, exist_ok=True)


def tex_escape(value: str) -> str:
    replacements = {
        "\\": r"\textbackslash{}",
        "&": r"\&",
        "%": r"\%",
        "$": r"\$",
        "#": r"\#",
        "_": r"\_",
        "{": r"\{",
        "}": r"\}",
    }
    escaped = value
    for old, new in replacements.items():
        escaped = escaped.replace(old, new)
    return escaped


def bytes_to_gib(value: float) -> float:
    return value / float(1024**3)


def format_float(value: float, digits: int = 3) -> str:
    return f"{value:.{digits}f}"


def format_gib(value: float) -> str:
    return f"{bytes_to_gib(value):.2f}"


def parse_result_rows(csv_path: Path) -> list[dict[str, object]]:
    with csv_path.open(newline="", encoding="ascii") as handle:
        reader = csv.reader(handle)
        header = next(reader)
        rows: list[dict[str, object]] = []

        for raw_row in reader:
            if not raw_row:
                continue

            if len(raw_row) == len(header) - 2:
                # LIPP rows omit search_method and value.
                raw_row = raw_row + ["", ""]
            elif len(raw_row) != len(header):
                # Ignore malformed rows instead of guessing missing fields.
                continue

            parsed: dict[str, object] = {}
            for column_name, value in zip(header, raw_row):
                if column_name in {"index_name", "search_method", "value"}:
                    parsed[column_name] = value
                else:
                    parsed[column_name] = float(value) if value else None
            rows.append(parsed)

    return rows


def best_rows_for_workload(dataset_slug: str, workload: WorkloadSpec) -> list[dict[str, object]]:
    csv_path = RESULTS_DIR / workload.filename_pattern.format(dataset=dataset_slug)
    rows = parse_result_rows(csv_path)
    best_by_index: dict[str, dict[str, object]] = {}

    for row in rows:
        metric_values = [row[column] for column in workload.metric_columns]
        if any(value is None for value in metric_values):
            continue

        average_metric = statistics.mean(float(value) for value in metric_values)
        std_metric = statistics.pstdev(float(value) for value in metric_values)

        enriched = dict(row)
        enriched["dataset"] = dataset_slug
        enriched["dataset_label"] = DATASETS[dataset_slug]
        enriched["workload"] = workload.slug
        enriched["workload_short_label"] = workload.short_label
        enriched["workload_long_label"] = workload.long_label
        enriched["metric_name"] = workload.metric_key
        enriched["avg_metric"] = average_metric
        enriched["std_metric"] = std_metric
        enriched["search_method"] = str(enriched.get("search_method", "") or "")
        enriched["value"] = str(enriched.get("value", "") or "")

        index_name = str(enriched["index_name"])
        current = best_by_index.get(index_name)
        if current is None or float(enriched["avg_metric"]) > float(current["avg_metric"]):
            best_by_index[index_name] = enriched

    return [best_by_index[index_name] for index_name in INDEX_ORDER]


def collect_best_configs() -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
    for dataset_slug in DATASETS:
        for workload in WORKLOADS:
            records.extend(best_rows_for_workload(dataset_slug, workload))
    return records


def write_csv(records: list[dict[str, object]], output_path: Path, fieldnames: list[str]) -> None:
    with output_path.open("w", newline="", encoding="ascii") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for record in records:
            writer.writerow({field: record.get(field, "") for field in fieldnames})


def build_lookup(records: list[dict[str, object]]) -> dict[tuple[str, str, str], dict[str, object]]:
    return {
        (str(record["dataset"]), str(record["workload"]), str(record["index_name"])): record
        for record in records
    }


def write_overall_averages(records: list[dict[str, object]]) -> list[dict[str, object]]:
    overall_rows: list[dict[str, object]] = []
    for workload in WORKLOADS:
        workload_records = [record for record in records if record["workload"] == workload.slug]
        for index_name in INDEX_ORDER:
            values = [
                float(record["avg_metric"])
                for record in workload_records
                if record["index_name"] == index_name
            ]
            overall_rows.append(
                {
                    "workload": workload.slug,
                    "workload_short_label": workload.short_label,
                    "index_name": index_name,
                    "overall_avg_mops": statistics.mean(values),
                }
            )

    write_csv(
        overall_rows,
        GENERATED_DIR / "task1_overall_averages.csv",
        ["workload", "workload_short_label", "index_name", "overall_avg_mops"],
    )
    return overall_rows


def render_table(lines: list[str], output_path: Path) -> None:
    output_path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_throughput_table(records: list[dict[str, object]]) -> None:
    lookup = build_lookup(records)
    lines = [
        r"\begin{tabular}{llrrr}",
        r"\toprule",
        r"Dataset & Workload & BTree & DynamicPGM & LIPP \\",
        r"\midrule",
    ]

    for dataset_slug, dataset_label in DATASETS.items():
        dataset_rows = []
        for workload in WORKLOADS:
            values = {
                index_name: float(lookup[(dataset_slug, workload.slug, index_name)]["avg_metric"])
                for index_name in INDEX_ORDER
            }
            best_value = max(values.values())
            formatted_values = []
            for index_name in INDEX_ORDER:
                cell = format_float(values[index_name], 3)
                if math.isclose(values[index_name], best_value):
                    cell = rf"\textbf{{{cell}}}"
                formatted_values.append(cell)
            dataset_rows.append(
                f"{tex_escape(dataset_label)} & {tex_escape(workload.short_label)} & "
                + " & ".join(formatted_values)
                + r" \\"
            )
        lines.extend(dataset_rows)
        if dataset_slug != list(DATASETS.keys())[-1]:
            lines.append(r"\midrule")

    lines.extend([r"\bottomrule", r"\end{tabular}"])
    render_table(lines, GENERATED_DIR / "throughput_table.tex")


def write_size_table(records: list[dict[str, object]]) -> None:
    lookup = build_lookup(records)
    lines = [
        r"\begin{tabular}{llrrr}",
        r"\toprule",
        r"Dataset & Workload & BTree & DynamicPGM & LIPP \\",
        r"\midrule",
    ]

    for dataset_slug, dataset_label in DATASETS.items():
        dataset_rows = []
        for workload in WORKLOADS:
            values = {
                index_name: float(lookup[(dataset_slug, workload.slug, index_name)]["index_size_bytes"])
                for index_name in INDEX_ORDER
            }
            best_value = min(values.values())
            formatted_values = []
            for index_name in INDEX_ORDER:
                cell = format_gib(values[index_name])
                if math.isclose(values[index_name], best_value):
                    cell = rf"\textbf{{{cell}}}"
                formatted_values.append(cell)
            dataset_rows.append(
                f"{tex_escape(dataset_label)} & {tex_escape(workload.short_label)} & "
                + " & ".join(formatted_values)
                + r" \\"
            )
        lines.extend(dataset_rows)
        if dataset_slug != list(DATASETS.keys())[-1]:
            lines.append(r"\midrule")

    lines.extend([r"\bottomrule", r"\end{tabular}"])
    render_table(lines, GENERATED_DIR / "size_table.tex")


def write_overall_table(overall_rows: list[dict[str, object]]) -> None:
    lines = [
        r"\begin{tabular}{lrrr}",
        r"\toprule",
        r"Workload & BTree & DynamicPGM & LIPP \\",
        r"\midrule",
    ]

    for workload in WORKLOADS:
        values = {
            row["index_name"]: float(row["overall_avg_mops"])
            for row in overall_rows
            if row["workload"] == workload.slug
        }
        best_value = max(values.values())
        cells = []
        for index_name in INDEX_ORDER:
            cell = format_float(values[index_name], 3)
            if math.isclose(values[index_name], best_value):
                cell = rf"\textbf{{{cell}}}"
            cells.append(cell)
        lines.append(
            f"{tex_escape(workload.short_label)} & " + " & ".join(cells) + r" \\"
        )

    lines.extend([r"\bottomrule", r"\end{tabular}"])
    render_table(lines, GENERATED_DIR / "overall_averages_table.tex")


def write_hyperparameter_table(records: list[dict[str, object]]) -> None:
    lookup = build_lookup(records)
    lines = [
        r"\begin{tabular}{lll}",
        r"\toprule",
        r"Dataset / Workload & Best BTree configuration & Best DynamicPGM configuration \\",
        r"\midrule",
    ]

    for dataset_slug, dataset_label in DATASETS.items():
        for workload in WORKLOADS:
            btree_row = lookup[(dataset_slug, workload.slug, "BTree")]
            pgm_row = lookup[(dataset_slug, workload.slug, "DynamicPGM")]
            btree_cfg = f"{btree_row['search_method']} / {btree_row['value']}"
            pgm_cfg = f"{pgm_row['search_method']} / {pgm_row['value']}"
            lines.append(
                f"{tex_escape(dataset_label)} / {tex_escape(workload.short_label)} & "
                f"{tex_escape(btree_cfg)} & {tex_escape(pgm_cfg)} \\\\"
            )
        if dataset_slug != list(DATASETS.keys())[-1]:
            lines.append(r"\midrule")

    lines.extend([r"\bottomrule", r"\end{tabular}"])
    render_table(lines, GENERATED_DIR / "hyperparameters_table.tex")


def write_summary_csv(records: list[dict[str, object]]) -> None:
    fieldnames = [
        "dataset",
        "dataset_label",
        "workload",
        "workload_short_label",
        "workload_long_label",
        "index_name",
        "metric_name",
        "avg_metric",
        "std_metric",
        "index_size_bytes",
        "search_method",
        "value",
    ]
    write_csv(records, GENERATED_DIR / "task1_best_configs.csv", fieldnames)


def plot_metric_panels(
    records: list[dict[str, object]],
    output_path: Path,
    *,
    metric_field: str,
    y_label: str,
    title: str,
    use_log_scale: bool,
) -> None:
    fig, axes = plt.subplots(3, 2, figsize=(15, 12))
    flattened_axes = axes.flatten()

    dataset_positions = list(range(len(DATASETS)))
    bar_width = 0.24

    for axis_index, workload in enumerate(WORKLOADS):
        axis = flattened_axes[axis_index]
        workload_records = [record for record in records if record["workload"] == workload.slug]

        for index_offset, index_name in enumerate(INDEX_ORDER):
            values = []
            for dataset_slug in DATASETS:
                record = next(
                    item
                    for item in workload_records
                    if item["dataset"] == dataset_slug and item["index_name"] == index_name
                )
                raw_value = float(record[metric_field])
                values.append(bytes_to_gib(raw_value) if metric_field == "index_size_bytes" else raw_value)

            x_positions = [
                position + (index_offset - 1) * bar_width
                for position in dataset_positions
            ]
            axis.bar(
                x_positions,
                values,
                width=bar_width,
                color=INDEX_COLORS[index_name],
                label=index_name,
            )

        axis.set_title(workload.short_label)
        axis.set_xticks(dataset_positions)
        axis.set_xticklabels(list(DATASETS.values()))
        axis.set_ylabel(y_label)
        if use_log_scale:
            axis.set_yscale("log")
        axis.grid(axis="y", linestyle="--", linewidth=0.5, alpha=0.5)

    flattened_axes[-1].axis("off")
    handles = [
        plt.Rectangle((0, 0), 1, 1, color=INDEX_COLORS[index_name]) for index_name in INDEX_ORDER
    ]
    fig.legend(handles, INDEX_ORDER, loc="upper center", ncol=3, frameon=False)
    fig.suptitle(title, fontsize=16, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    fig.savefig(output_path, bbox_inches="tight")
    plt.close(fig)


def write_highlights(records: list[dict[str, object]], overall_rows: list[dict[str, object]]) -> None:
    lookup = build_lookup(records)

    def overall_value(workload_slug: str, index_name: str) -> float:
        return next(
            float(row["overall_avg_mops"])
            for row in overall_rows
            if row["workload"] == workload_slug and row["index_name"] == index_name
        )

    lipp_lookup = overall_value("lookup_only", "LIPP")
    btree_lookup = overall_value("lookup_only", "BTree")
    lipp_mixed10 = overall_value("mixed_10_insert", "LIPP")
    btree_mixed10 = overall_value("mixed_10_insert", "BTree")
    pgm_insert = overall_value("insert_throughput", "DynamicPGM")
    btree_insert = overall_value("insert_throughput", "BTree")
    pgm_mixed90 = overall_value("mixed_90_insert", "DynamicPGM")
    lipp_mixed90 = overall_value("mixed_90_insert", "LIPP")

    fb_lipp_lookup_size = float(lookup[("fb", "lookup_only", "LIPP")]["index_size_bytes"])
    fb_pgm_lookup_size = float(lookup[("fb", "lookup_only", "DynamicPGM")]["index_size_bytes"])
    osmc_lipp_lookup_size = float(lookup[("osmc", "lookup_only", "LIPP")]["index_size_bytes"])
    osmc_pgm_lookup_size = float(lookup[("osmc", "lookup_only", "DynamicPGM")]["index_size_bytes"])

    lines = [
        f"lookup_speedup_lipp_vs_btree={lipp_lookup / btree_lookup:.1f}",
        f"mixed10_speedup_lipp_vs_btree={lipp_mixed10 / btree_mixed10:.1f}",
        f"insert_speedup_pgm_vs_btree={pgm_insert / btree_insert:.1f}",
        f"mixed90_speedup_pgm_vs_lipp={pgm_mixed90 / lipp_mixed90:.2f}",
        f"fb_lookup_size_ratio_lipp_vs_pgm={fb_lipp_lookup_size / fb_pgm_lookup_size:.2f}",
        f"osmc_lookup_size_ratio_lipp_vs_pgm={osmc_lipp_lookup_size / osmc_pgm_lookup_size:.2f}",
    ]
    (GENERATED_DIR / "highlights.txt").write_text("\n".join(lines) + "\n", encoding="ascii")


def main() -> None:
    ensure_directories()
    records = collect_best_configs()
    write_summary_csv(records)
    overall_rows = write_overall_averages(records)
    write_throughput_table(records)
    write_size_table(records)
    write_overall_table(overall_rows)
    write_hyperparameter_table(records)
    write_highlights(records, overall_rows)

    plot_metric_panels(
        records,
        FIGURES_DIR / "task1_throughput_overview.pdf",
        metric_field="avg_metric",
        y_label="Throughput (Mops/s)",
        title="Task 1 throughput comparison (log scale)",
        use_log_scale=True,
    )
    plot_metric_panels(
        records,
        FIGURES_DIR / "task1_throughput_overview.png",
        metric_field="avg_metric",
        y_label="Throughput (Mops/s)",
        title="Task 1 throughput comparison (log scale)",
        use_log_scale=True,
    )
    plot_metric_panels(
        records,
        FIGURES_DIR / "task1_index_size_overview.pdf",
        metric_field="index_size_bytes",
        y_label="Index size (GiB)",
        title="Task 1 index size comparison",
        use_log_scale=False,
    )
    plot_metric_panels(
        records,
        FIGURES_DIR / "task1_index_size_overview.png",
        metric_field="index_size_bytes",
        y_label="Index size (GiB)",
        title="Task 1 index size comparison",
        use_log_scale=False,
    )

    print(f"Wrote report assets to {REPORT_DIR}")


if __name__ == "__main__":
    main()
