#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

BENCHMARK="${BENCHMARK_PATH:-build/benchmark}"
REPEATS="${BENCHMARK_REPEATS:-3}"
DATA_DIR="${DATA_DIR:-./data}"
RESULTS_DIR="${RESULTS_DIR:-results_milestone2}"
DATASET="fb_100M_public_uint64"
INDEXES=(
    "DynamicPGM"
    "LIPP"
    "HybridPGMLIPP"
)
WORKLOADS=(
    "fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix"
    "fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix"
)

if [ ! -x "${BENCHMARK}" ]; then
    echo "Error: ${BENCHMARK} does not exist or is not executable."
    exit 1
fi

mkdir -p "${RESULTS_DIR}"
rm -f "${RESULTS_DIR}"/fb_100M_public_uint64*_results_table.csv

add_csv_header() {
    local file_path="$1"
    local header="$2"
    local tmp_file

    tmp_file="$(mktemp)"
    {
        echo "${header}"
        sed '/^index_name,/d' "${file_path}"
    } > "${tmp_file}"
    mv "${tmp_file}" "${file_path}"
}

for workload in "${WORKLOADS[@]}"; do
    for index_name in "${INDEXES[@]}"; do
        echo "Running ${index_name} on ${workload}"
        TLI_RESULTS_DIR="${RESULTS_DIR}" \
            "${BENCHMARK}" "${DATA_DIR}/${DATASET}" "${DATA_DIR}/${workload}" \
            --through --csv --only "${index_name}" -r "${REPEATS}"
    done
done

for file_path in "${RESULTS_DIR}"/*_results_table.csv; do
    add_csv_header "${file_path}" \
        "index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,search_method,value"
done

echo "Milestone 2 benchmark runs complete."
