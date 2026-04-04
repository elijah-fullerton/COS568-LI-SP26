#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26-m3_adaptive_filter}"
RUN_TAG="${RUN_TAG:-m3_adaptive_filter_search}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/eval_runs/${RUN_TAG}}"
SCRATCH_ROOT="${SCRATCH_ROOT:-/scratch/${USER}/${RUN_TAG}_${SLURM_JOB_ID}}"
BUILD_JOBS="${BUILD_JOBS:-${SLURM_CPUS_PER_TASK:-8}}"
DATA_CACHE_ROOT="${DATA_CACHE_ROOT:-/scratch/ef0952/cos568_data}"
RESULTS_DIR_NAME="${RESULTS_DIR_NAME:-results_m3_adaptive_filter_search}"
REPEATS="${REPEATS:-1}"
WORKLOAD_TIMEOUT="${WORKLOAD_TIMEOUT:-20m}"

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

download_if_missing() {
  local output_path="$1"
  local url="$2"
  if [[ -f "${output_path}" ]]; then
    return
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -O "${output_path}" "${url}"
  else
    curl -L "${url}" -o "${output_path}"
  fi
}

stage_dataset_if_present() {
  local dataset_name="$1"
  mkdir -p "${SCRATCH_ROOT}/cos568_data"
  if [[ -f "${SCRATCH_ROOT}/cos568_data/${dataset_name}" ]]; then
    return
  fi
  if [[ -f "${DATA_CACHE_ROOT}/${dataset_name}" ]]; then
    rsync -a "${DATA_CACHE_ROOT}/${dataset_name}" \
      "${SCRATCH_ROOT}/cos568_data/${dataset_name}"
  fi
}

mkdir -p "${SCRATCH_ROOT}" "${ARCHIVE_ROOT}"

cleanup() {
  local rc=$?
  mkdir -p "${ARCHIVE_ROOT}"
  cp -f "${SCRATCH_ROOT}/job_env.txt" \
    "${ARCHIVE_ROOT}/job_env.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stdout" \
    "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stdout" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stderr" \
    "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stderr" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark_status.txt" \
    "${ARCHIVE_ROOT}/benchmark_status.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/summary.txt" \
    "${ARCHIVE_ROOT}/summary.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  if [[ -d "${SCRATCH_ROOT}/workspace/${RESULTS_DIR_NAME}" ]]; then
    rm -rf "${ARCHIVE_ROOT}/results.${SLURM_JOB_ID}"
    rsync -a "${SCRATCH_ROOT}/workspace/${RESULTS_DIR_NAME}/" \
      "${ARCHIVE_ROOT}/results.${SLURM_JOB_ID}/"
  fi
  exit "${rc}"
}
trap cleanup EXIT

{
  echo "hostname=$(hostname)"
  echo "job_id=${SLURM_JOB_ID:-}"
  echo "run_tag=${RUN_TAG}"
  echo "repo_root=${REPO_ROOT}"
  echo "scratch_root=${SCRATCH_ROOT}"
  echo "repeats=${REPEATS}"
  echo "workload_timeout=${WORKLOAD_TIMEOUT}"
} > "${SCRATCH_ROOT}/job_env.txt"

rsync -a --delete \
  --exclude '.git' \
  --exclude 'data' \
  --exclude 'build' \
  --exclude 'build_*' \
  --exclude 'results' \
  --exclude 'results_*' \
  --exclude 'slurm_runs' \
  --exclude 'eval_runs' \
  "${REPO_ROOT}/" "${SCRATCH_ROOT}/workspace/"

mkdir -p "${SCRATCH_ROOT}/cos568_data"
ln -sfn "${SCRATCH_ROOT}/cos568_data" "${SCRATCH_ROOT}/workspace/data"

cd "${SCRATCH_ROOT}/workspace"

for dataset in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64; do
  stage_dataset_if_present "${dataset}"
done

download_if_missing \
  "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" \
  "https://www.dropbox.com/scl/fi/hngvfbz1a2tkwpebjngb9/fb_100M_public_uint64?rlkey=px31l6wj9tnic4z604bt6s55n&st=d3iuhhgx&dl=0"

if [[ ! -f "${SCRATCH_ROOT}/cos568_data/books_100M_public_uint64" || \
      ! -f "${SCRATCH_ROOT}/cos568_data/osmc_100M_public_uint64" ]]; then
  bash ./scripts/download_dataset.sh
fi

cmake -S . -B build_scratch -DCMAKE_BUILD_TYPE=Release
cmake --build build_scratch -j "${BUILD_JOBS}"

for dataset in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64; do
  ./build_scratch/generate "./data/${dataset}" 2000000 --insert-ratio 0.9 --negative-lookup-ratio 0.5 --mix
  ./build_scratch/generate "./data/${dataset}" 2000000 --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
done

mkdir -p "${RESULTS_DIR_NAME}"
rm -f "${RESULTS_DIR_NAME}"/*_results_table.csv

set +e
{
  for dataset in books_100M_public_uint64 fb_100M_public_uint64 osmc_100M_public_uint64; do
    for workload in 0.900000i 0.100000i; do
      echo "Running HybridPGMLIPP on ${dataset} ${workload}"
      TLI_RESULTS_DIR="${RESULTS_DIR_NAME}" \
      timeout "${WORKLOAD_TIMEOUT}" \
        ./build_scratch/benchmark \
          "./data/${dataset}" \
          "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_${workload}_0m_mix" \
          --through --csv --only HybridPGMLIPP -r "${REPEATS}"
    done
  done
} > "${SCRATCH_ROOT}/benchmark.stdout" \
  2> "${SCRATCH_ROOT}/benchmark.stderr"
rc=$?
set -e

echo "${rc}" > "${SCRATCH_ROOT}/benchmark_status.txt"

if [[ "${rc}" -eq 0 ]]; then
  for file_path in "${RESULTS_DIR_NAME}"/*_results_table.csv; do
    add_csv_header "${file_path}"
  done
  python3 ./scripts/summarize_m3_search_against_frozen_baselines.py \
    --repo-root "${REPO_ROOT}" \
    --results-dir "${SCRATCH_ROOT}/workspace/${RESULTS_DIR_NAME}" \
    > "${SCRATCH_ROOT}/summary.txt"
fi

exit "${rc}"
