#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
ITER_TAG="${ITER_TAG:?ITER_TAG must be set}"
STAGE_DIR="${STAGE_DIR:?STAGE_DIR must be set}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/slurm_runs/${ITER_TAG}}"
SCRATCH_ROOT="${SCRATCH_ROOT:-/scratch/${USER}/${ITER_TAG}_job_${SLURM_JOB_ID}}"
BUILD_JOBS="${BUILD_JOBS:-${SLURM_CPUS_PER_TASK:-8}}"
DATA_CACHE_ROOT="${DATA_CACHE_ROOT:-/scratch/ef0952/cos568_data}"
WORKLOAD_TIMEOUT="${WORKLOAD_TIMEOUT:-7m}"

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
mkdir -p "${SCRATCH_ROOT}/results"

cleanup() {
  local rc=$?
  cp -f "${SCRATCH_ROOT}/job_env.txt" \
    "${ARCHIVE_ROOT}/job_env.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark_status.txt" \
    "${ARCHIVE_ROOT}/benchmark_status.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stdout" \
    "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stdout" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stderr" \
    "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stderr" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/self_eval.json" \
    "${ARCHIVE_ROOT}/self_eval.${SLURM_JOB_ID}.json" 2>/dev/null || true
  cp -rf "${SCRATCH_ROOT}/results" "${ARCHIVE_ROOT}/results.${SLURM_JOB_ID}" \
    2>/dev/null || true
  exit "${rc}"
}
trap cleanup EXIT

{
  echo "hostname=$(hostname)"
  echo "iter_tag=${ITER_TAG}"
  echo "stage_dir=${STAGE_DIR}"
  echo "slurm_job_id=${SLURM_JOB_ID:-}"
} > "${SCRATCH_ROOT}/job_env.txt"

rsync -a --delete \
  --exclude '.git' \
  --exclude 'data' \
  --exclude 'build' \
  --exclude 'build_scratch' \
  --exclude 'results' \
  --exclude 'results_*' \
  --exclude 'slurm_runs' \
  "${REPO_ROOT}/" "${SCRATCH_ROOT}/workspace/"

rsync -a "${STAGE_DIR}/" "${SCRATCH_ROOT}/workspace/"

mkdir -p "${SCRATCH_ROOT}/cos568_data"
ln -sfn "${SCRATCH_ROOT}/cos568_data" "${SCRATCH_ROOT}/workspace/data"
mkdir -p "${SCRATCH_ROOT}/workspace/results"

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

: > "${SCRATCH_ROOT}/benchmark.stdout"
: > "${SCRATCH_ROOT}/benchmark.stderr"
echo '{"decision":"continue","reason":"no self-evaluation run yet"}' > "${SCRATCH_ROOT}/self_eval.json"

run_workload() {
  local dataset="$1"
  local token="$2"
  /usr/bin/time -v \
    timeout "${WORKLOAD_TIMEOUT}" \
    ./build_scratch/benchmark \
      "./data/${dataset}" \
      "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_${token}_0m_mix" \
      --through --verify --csv --only HybridPGMLIPP -r 1 \
      >> "${SCRATCH_ROOT}/benchmark.stdout" \
      2>> "${SCRATCH_ROOT}/benchmark.stderr"
}

set +e
rc=0
for dataset in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64; do
  for token in 0.100000i 0.900000i; do
    run_workload "${dataset}" "${token}"
    rc=$?
    if [[ "${rc}" -ne 0 ]]; then
      break 2
    fi

    python3 ./scripts/self_evaluate_m3_candidate.py \
      --repo-root "${REPO_ROOT}" \
      --results-dir "${SCRATCH_ROOT}/workspace/results" \
      --mode full \
      --output "${SCRATCH_ROOT}/self_eval.json" \
      > /dev/null
    eval_rc=$?
    if [[ "${eval_rc}" -eq 42 ]]; then
      rc=42
      break 2
    fi
    if [[ "${eval_rc}" -ne 0 ]]; then
      rc="${eval_rc}"
      break 2
    fi
  done
done
set -e

echo "${rc}" > "${SCRATCH_ROOT}/benchmark_status.txt"
