from pathlib import Path

import pandas as pd


WORKLOADS = [
    ("0.100000i", "Mixed Throughput (10% Insert, 90% Lookup)"),
    ("0.900000i", "Mixed Throughput (90% Insert, 10% Lookup)"),
]


def summarize(csv_path: Path) -> dict:
    frame = pd.read_csv(csv_path)
    metric_cols = [col for col in frame.columns if col.startswith("mixed_throughput_mops")]
    frame["avg_mops"] = frame[metric_cols].mean(axis=1)
    best = frame.loc[frame.groupby("index_name")["avg_mops"].idxmax()].copy()
    result = {}
    for _, row in best.iterrows():
      result[row["index_name"]] = {
          "throughput": float(row["avg_mops"]),
          "variant": f"{row.get('search_method', '') or 'n/a'} / {row.get('value', '') or 'n/a'}",
      }
    return result


def main() -> None:
    results_dir = Path("results")
    lines = ["# Screening Summary", ""]
    success = True
    for workload_token, workload_name in WORKLOADS:
        csv_path = results_dir / (
            f"fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_{workload_token}_0m_mix_results_table.csv"
        )
        summary = summarize(csv_path)
        lines.append(f"## {workload_name}")
        for index_name in ["DynamicPGM", "LIPP", "HybridPGMLIPP"]:
            row = summary[index_name]
            lines.append(
                f"- {index_name}: {row['throughput']:.3f} Mops/s ({row['variant']})"
            )
        hybrid = summary["HybridPGMLIPP"]["throughput"]
        dpgm = summary["DynamicPGM"]["throughput"]
        lipp = summary["LIPP"]["throughput"]
        passed = hybrid > dpgm and hybrid > lipp
        success &= passed
        lines.append(f"- Hybrid beats both baselines: {'yes' if passed else 'no'}")
        lines.append("")

    lines.append(f"Overall screening success: {'yes' if success else 'no'}")
    (results_dir / "screening_summary.md").write_text("\n".join(lines), encoding="ascii")


if __name__ == "__main__":
    main()
