#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
STAGE_DIR="${STAGE_DIR:-${REPO_ROOT}/iterations/m3_iter09_slurm_stage}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/slurm_runs/m3_iter09}"
SCRATCH_ROOT="${SCRATCH_ROOT:-/scratch/${USER}/m3_iter09_job_${SLURM_JOB_ID}}"
BUILD_JOBS="${BUILD_JOBS:-${SLURM_CPUS_PER_TASK:-8}}"
VERIFY_ONLY="${VERIFY_ONLY:-1}"
DATA_CACHE_ROOT="${DATA_CACHE_ROOT:-/scratch/ef0952/cos568_data}"

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
    rsync -a "${DATA_CACHE_ROOT}/${dataset_name}" "${SCRATCH_ROOT}/cos568_data/${dataset_name}"
  fi
}

mkdir -p "${SCRATCH_ROOT}" "${ARCHIVE_ROOT}"
mkdir -p "${SCRATCH_ROOT}/artifacts" "${SCRATCH_ROOT}/logs" "${SCRATCH_ROOT}/results"

cleanup() {
  local rc=$?
  mkdir -p "${ARCHIVE_ROOT}"
  cp -f "${SCRATCH_ROOT}/job_env.txt" "${ARCHIVE_ROOT}/job_env.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark_status.txt" "${ARCHIVE_ROOT}/benchmark_status.${SLURM_JOB_ID}.txt" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stdout" "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stdout" 2>/dev/null || true
  cp -f "${SCRATCH_ROOT}/benchmark.stderr" "${ARCHIVE_ROOT}/benchmark.${SLURM_JOB_ID}.stderr" 2>/dev/null || true
  cp -rf "${SCRATCH_ROOT}/results" "${ARCHIVE_ROOT}/results.${SLURM_JOB_ID}" 2>/dev/null || true
  exit "${rc}"
}
trap cleanup EXIT

{
  echo "hostname=$(hostname)"
  echo "verify_only=${VERIFY_ONLY}"
  echo "slurm_job_id=${SLURM_JOB_ID:-}"
  echo "slurm_cpus_per_task=${SLURM_CPUS_PER_TASK:-}"
  echo "data_cache_root=${DATA_CACHE_ROOT}"
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

cp "${STAGE_DIR}/hybrid_pgm_lipp.h" \
  "${SCRATCH_ROOT}/workspace/competitors/hybrid_pgm_lipp.h"
cp "${STAGE_DIR}/benchmark_hybrid_pgm_lipp.cc" \
  "${SCRATCH_ROOT}/workspace/benchmarks/benchmark_hybrid_pgm_lipp.cc"
cp "${STAGE_DIR}/analysis_milestone3.py" \
  "${SCRATCH_ROOT}/workspace/scripts/analysis_milestone3.py"

mkdir -p "${SCRATCH_ROOT}/cos568_data"
ln -sfn "${SCRATCH_ROOT}/cos568_data" "${SCRATCH_ROOT}/workspace/data"
mkdir -p "${SCRATCH_ROOT}/workspace/results"

cd "${SCRATCH_ROOT}/workspace"

if [[ "${VERIFY_ONLY}" == "1" ]]; then
  stage_dataset_if_present "fb_100M_public_uint64"
  download_if_missing \
    "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" \
    "https://www.dropbox.com/scl/fi/hngvfbz1a2tkwpebjngb9/fb_100M_public_uint64?rlkey=px31l6wj9tnic4z604bt6s55n&st=d3iuhhgx&dl=0"
else
  for dataset in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64; do
    stage_dataset_if_present "${dataset}"
  done
  if [[ ! -f "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" || \
        ! -f "${SCRATCH_ROOT}/cos568_data/books_100M_public_uint64" || \
        ! -f "${SCRATCH_ROOT}/cos568_data/osmc_100M_public_uint64" ]]; then
    bash ./scripts/download_dataset.sh
  fi
fi

cmake -S . -B build_scratch -DCMAKE_BUILD_TYPE=Release
cmake --build build_scratch -j "${BUILD_JOBS}"

set +e
if [[ "${VERIFY_ONLY}" == "1" ]]; then
  ./build_scratch/generate ./data/fb_100M_public_uint64 2000000 --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
  /usr/bin/time -v \
    timeout 20m \
    ./build_scratch/benchmark \
      ./data/fb_100M_public_uint64 \
      ./data/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix \
      --through --verify --csv --only HybridPGMLIPP -r 1 \
      > "${SCRATCH_ROOT}/benchmark.stdout" \
      2> "${SCRATCH_ROOT}/benchmark.stderr"
  rc=$?
else
  for dataset in fb_100M_public_uint64 books_100M_public_uint64 osmc_100M_public_uint64; do
    ./build_scratch/generate "./data/${dataset}" 2000000 --insert-ratio 0.9 --negative-lookup-ratio 0.5 --mix
    ./build_scratch/generate "./data/${dataset}" 2000000 --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
  done
  /usr/bin/time -v \
    timeout 8h \
    env BENCHMARK_PATH=build_scratch/benchmark DATA_DIR=./data RESULTS_DIR="${SCRATCH_ROOT}/results" BENCHMARK_REPEATS=3 \
    bash ./scripts/run_milestone3.sh \
      > "${SCRATCH_ROOT}/benchmark.stdout" \
      2> "${SCRATCH_ROOT}/benchmark.stderr"
  rc=$?
fi
set -e

echo "${rc}" > "${SCRATCH_ROOT}/benchmark_status.txt"
