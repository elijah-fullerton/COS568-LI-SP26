#!/usr/bin/env bash

set -euo pipefail
shopt -s nullglob

BENCHMARK="build/benchmark"
REPEATS="${BENCHMARK_REPEATS:-3}"
DATASETS=(
    "fb_100M_public_uint64"
    "books_100M_public_uint64"
    "osmc_100M_public_uint64"
)
INDEXES=(
    "LIPP"
    "BTree"
    "DynamicPGM"
)

if [ ! -f "${BENCHMARK}" ]; then
    echo "Error: ${BENCHMARK} does not exist. Run scripts/build_benchmark.sh first."
    exit 1
fi

mkdir -p results
rm -f results/*_results_table.csv

execute_uint64_100M() {
    local dataset="$1"
    local index_name="$2"

    echo "Running ${index_name} on ${dataset}..."

    "${BENCHMARK}" "./data/${dataset}" "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_0.000000i" --through --csv --only "${index_name}" -r "${REPEATS}"
    "${BENCHMARK}" "./data/${dataset}" "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_0.500000i_0m" --through --csv --only "${index_name}" -r "${REPEATS}"
    "${BENCHMARK}" "./data/${dataset}" "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix" --through --csv --only "${index_name}" -r "${REPEATS}"
    "${BENCHMARK}" "./data/${dataset}" "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix" --through --csv --only "${index_name}" -r "${REPEATS}"
}

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

echo "Executing Task 1 benchmarks..."
for dataset in "${DATASETS[@]}"; do
    for index_name in "${INDEXES[@]}"; do
        execute_uint64_100M "${dataset}" "${index_name}"
    done
done

for file_path in results/*_results_table.csv; do
    if [[ "${file_path}" == *"_0.000000i_results_table.csv" ]]; then
        add_csv_header "${file_path}" \
            "index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,lookup_throughput_mops1,lookup_throughput_mops2,lookup_throughput_mops3,search_method,value"
    elif [[ "${file_path}" == *"_mix_results_table.csv" ]]; then
        add_csv_header "${file_path}" \
            "index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,search_method,value"
    else
        add_csv_header "${file_path}" \
            "index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,insert_throughput_mops1,lookup_throughput_mops1,insert_throughput_mops2,lookup_throughput_mops2,insert_throughput_mops3,lookup_throughput_mops3,search_method,value"
    fi
    echo "Prepared CSV header for ${file_path}"
done

echo "Task 1 benchmarking complete."
