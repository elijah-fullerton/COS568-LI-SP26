#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

BENCHMARK="build/benchmark"
REPEATS="${BENCHMARK_REPEATS:-3}"
DATASET="fb_100M_public_uint64"
INDEXES=(
    "LIPP"
    "DynamicPGM"
    "HybridPGMLIPP"
)

if [ ! -f "${BENCHMARK}" ]; then
    echo "Error: ${BENCHMARK} does not exist. Run scripts/build_benchmark.sh first."
    exit 1
fi

mkdir -p results
rm -f results/"${DATASET}"_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv
rm -f results/"${DATASET}"_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv

run_workload() {
    local index_name="$1"
    local ops_file="$2"

    echo "Running ${index_name} on ${ops_file}..."
    "${BENCHMARK}" "./data/${DATASET}" "${ops_file}" --through --csv --only "${index_name}" -r "${REPEATS}"
}

add_csv_header() {
    local file_path="$1"
    local tmp_file

    tmp_file="$(mktemp)"
    {
        echo "index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,search_method,value"
        sed '/^index_name,/d' "${file_path}"
    } > "${tmp_file}"
    mv "${tmp_file}" "${file_path}"
}

echo "Executing Milestone 2 Facebook mixed-workload benchmarks..."
for index_name in "${INDEXES[@]}"; do
    run_workload "${index_name}" "./data/${DATASET}_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix"
    run_workload "${index_name}" "./data/${DATASET}_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix"
done

for file_path in \
    results/"${DATASET}"_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv \
    results/"${DATASET}"_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv; do
    add_csv_header "${file_path}"
    echo "Prepared CSV header for ${file_path}"
done

echo "Milestone 2 benchmarking complete."
