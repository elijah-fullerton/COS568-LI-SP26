import argparse
import csv
from pathlib import Path

WORKLOADS = [
    ("0.100000i", "Mixed Throughput (10% Insert, 90% Lookup)"),
    ("0.900000i", "Mixed Throughput (90% Insert, 10% Lookup)"),
]
ABORT_EXIT_CODE = 42
DEFAULT_ABORT_AVG_RELATIVE_THRESHOLD = -0.20
DEFAULT_ABORT_MAX_RELATIVE_THRESHOLD = -0.10


def csv_name(ops_token: str, workload_token: str) -> str:
    return (
        "fb_100M_public_uint64_ops_"
        f"{ops_token}_0.000000rq_0.500000nl_{workload_token}_0m_mix_results_table.csv"
    )


def find_results_csv(results_dir: Path, ops_token: str, workload_token: str) -> Path:
    matches = sorted(
        results_dir.glob(csv_name(ops_token, workload_token))
    )
    if not matches:
        raise FileNotFoundError(
            "missing screening CSV for workload token "
            f"{workload_token} with ops token {ops_token}"
        )
    return matches[-1]


def summarize(csv_path: Path) -> dict:
    with csv_path.open(newline="", encoding="ascii") as handle:
        rows = list(csv.reader(handle))

    if not rows:
        raise ValueError(f"empty screening CSV: {csv_path}")

    if rows[0] and rows[0][0] == "index_name":
        parsed_rows = [dict(zip(rows[0], row)) for row in rows[1:]]
    else:
        parsed_rows = []
        for values in rows:
            if len(values) < 4:
                raise ValueError(f"unrecognized row width {len(values)} in {csv_path}")
            row = {
                "index_name": values[0],
                "mixed_throughput_mops1": values[3],
                "search_method": values[4] if len(values) >= 5 else "",
                "value": values[5] if len(values) >= 6 else "",
            }
            parsed_rows.append(row)

    result = {}
    for row in parsed_rows:
        throughput_cols = [
            "mixed_throughput_mops1",
            "mixed_throughput_mops2",
            "mixed_throughput_mops3",
            "mixed_throughput_mops",
        ]
        throughput_values = [
            float(row[col])
            for col in throughput_cols
            if row.get(col, "") not in ("", None)
        ]
        if not throughput_values:
            raise ValueError(f"missing mixed throughput columns in {csv_path}")
        throughput = sum(throughput_values) / len(throughput_values)
        name = row["index_name"]
        best = result.get(name)
        if best is None or throughput > best["throughput"]:
            result[name] = {
                "throughput": throughput,
                "variant": (
                    f"{row.get('search_method', '') or 'n/a'} / "
                    f"{row.get('value', '') or 'n/a'}"
                ),
            }
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results-dir", default="results")
    parser.add_argument("--require-hybrid-win", action="store_true")
    parser.add_argument("--abort-on-hybrid-loss", action="store_true")
    parser.add_argument("--screen-ops-token", default="250000")
    parser.add_argument("--scale-lookup-ops-token", default="")
    parser.add_argument(
        "--abort-avg-relative-threshold",
        type=float,
        default=DEFAULT_ABORT_AVG_RELATIVE_THRESHOLD,
    )
    parser.add_argument(
        "--abort-max-relative-threshold",
        type=float,
        default=DEFAULT_ABORT_MAX_RELATIVE_THRESHOLD,
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    results_dir = Path(args.results_dir)
    lines = ["# Screening Summary", ""]
    success = True
    relative_gaps = []
    loses_to_both_workloads = []
    workload_specs = [
        (args.screen_ops_token, workload_token, workload_name)
        for workload_token, workload_name in WORKLOADS
    ]
    if args.scale_lookup_ops_token:
        workload_specs.append(
            (
                args.scale_lookup_ops_token,
                "0.100000i",
                "Scale Canary Throughput (10% Insert, 90% Lookup)",
            )
        )

    for ops_token, workload_token, workload_name in workload_specs:
        csv_path = find_results_csv(results_dir, ops_token, workload_token)
        summary = summarize(csv_path)
        lines.append(f"## {workload_name}")
        lines.append(f"- Ops token: {ops_token}")
        for index_name in ["DynamicPGM", "LIPP", "HybridPGMLIPP"]:
            row = summary[index_name]
            lines.append(
                f"- {index_name}: {row['throughput']:.3f} Mops/s ({row['variant']})"
            )
        hybrid = summary["HybridPGMLIPP"]["throughput"]
        dpgm = summary["DynamicPGM"]["throughput"]
        lipp = summary["LIPP"]["throughput"]
        best_baseline = max(dpgm, lipp)
        relative_gap = (hybrid / best_baseline) - 1.0 if best_baseline > 0.0 else 0.0
        relative_gaps.append(relative_gap)
        loses_to_both = hybrid <= dpgm and hybrid <= lipp
        loses_to_both_workloads.append(loses_to_both)
        passed = hybrid > dpgm and hybrid > lipp
        success &= passed
        lines.append(f"- Hybrid beats both baselines: {'yes' if passed else 'no'}")
        lines.append(
            f"- Hybrid loses to both baselines: {'yes' if loses_to_both else 'no'}"
        )
        lines.append(
            f"- Hybrid vs best baseline: {relative_gap * 100.0:+.1f}%"
        )
        lines.append("")

    avg_relative_gap = (
        sum(relative_gaps) / len(relative_gaps) if relative_gaps else float("-inf")
    )
    max_relative_gap = max(relative_gaps) if relative_gaps else float("-inf")
    uniform_loss = bool(loses_to_both_workloads) and all(loses_to_both_workloads)
    strong_loss = (
        relative_gaps
        and uniform_loss
        and avg_relative_gap <= args.abort_avg_relative_threshold
        and max_relative_gap <= args.abort_max_relative_threshold
    )

    lines.append(f"Overall screening success: {'yes' if success else 'no'}")
    lines.append(
        "Uniform loss vs both baselines: "
        + ("yes" if uniform_loss else "no")
    )
    lines.append(
        "Strong-loss abort gate: "
        + ("triggered" if strong_loss else "not triggered")
    )
    lines.append(
        "Strong-loss thresholds: "
        f"avg<={args.abort_avg_relative_threshold * 100.0:+.1f}% and "
        f"best<={args.abort_max_relative_threshold * 100.0:+.1f}%"
    )
    (results_dir / "screening_summary.md").write_text("\n".join(lines), encoding="ascii")
    if not success:
        if args.abort_on_hybrid_loss and strong_loss:
            raise SystemExit(ABORT_EXIT_CODE)
        if args.require_hybrid_win:
            raise SystemExit(2)


if __name__ == "__main__":
    main()
