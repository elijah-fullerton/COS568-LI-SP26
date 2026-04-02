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
OPS_CACHE_ROOT="${OPS_CACHE_ROOT:-${DATA_CACHE_ROOT}/ops_cache}"
FULL_BENCHMARK_BUDGET_SECONDS="${FULL_BENCHMARK_BUDGET_SECONDS:-1200}"
WORKLOAD_TIMEOUT_SECONDS="${WORKLOAD_TIMEOUT_SECONDS:-300}"
FULL_VERIFY_LOOKUP_WORKLOADS="${FULL_VERIFY_LOOKUP_WORKLOADS:-0}"
FULL_SCHEDULE_PROFILE="${FULL_SCHEDULE_PROFILE:-rl_fast}"
FULL_DATASETS=(
  "books_100M_public_uint64"
  "fb_100M_public_uint64"
  "osmc_100M_public_uint64"
)
if [[ "${FULL_SCHEDULE_PROFILE}" == "full" ]]; then
  FULL_WORKLOAD_SCHEDULE=(
    "fb_100M_public_uint64:0.100000i"
    "books_100M_public_uint64:0.100000i"
    "osmc_100M_public_uint64:0.100000i"
    "fb_100M_public_uint64:0.900000i"
    "books_100M_public_uint64:0.900000i"
    "osmc_100M_public_uint64:0.900000i"
  )
else
  FULL_WORKLOAD_SCHEDULE=(
    "fb_100M_public_uint64:0.100000i"
    "fb_100M_public_uint64:0.900000i"
  )
fi

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

stage_cached_ops_if_present() {
  local dataset_name="$1"
  local workload_token="$2"
  local cache_path="${OPS_CACHE_ROOT}/${dataset_name}_ops_2M_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  local scratch_path="${SCRATCH_ROOT}/workspace/data/$(basename "${cache_path}")"
  if [[ -f "${scratch_path}" || ! -f "${cache_path}" ]]; then
    return
  fi
  mkdir -p "${OPS_CACHE_ROOT}"
  cp -f "${cache_path}" "${scratch_path}"
}

cache_generated_ops() {
  local dataset_name="$1"
  local workload_token="$2"
  local generated_path="${SCRATCH_ROOT}/workspace/data/${dataset_name}_ops_2M_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  local cache_path="${OPS_CACHE_ROOT}/$(basename "${generated_path}")"
  if [[ -f "${generated_path}" && ! -f "${cache_path}" ]]; then
    mkdir -p "${OPS_CACHE_ROOT}"
    cp -f "${generated_path}" "${cache_path}"
  fi
}

generate_ops_if_missing() {
  local dataset_name="$1"
  local workload_token="$2"
  shift 2
  stage_cached_ops_if_present "${dataset_name}" "${workload_token}"
  local scratch_path="${SCRATCH_ROOT}/workspace/data/${dataset_name}_ops_2M_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  if [[ -f "${scratch_path}" ]]; then
    return
  fi
  ./build_scratch/generate "./data/${dataset_name}" 2000000 "$@"
  cache_generated_ops "${dataset_name}" "${workload_token}"
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

format_timeout_seconds() {
  local seconds="$1"
  if (( seconds <= 0 )); then
    seconds=1
  fi
  printf '%ss\n' "${seconds}"
}

compute_workload_timeout_seconds() {
  local remaining_seconds="$1"
  local workloads_remaining="$2"
  local timeout_seconds="${WORKLOAD_TIMEOUT_SECONDS}"
  local fair_share_seconds=1

  if (( workloads_remaining > 0 )); then
    fair_share_seconds="$(( remaining_seconds / workloads_remaining ))"
    if (( fair_share_seconds <= 0 )); then
      fair_share_seconds=1
    fi
  fi

  if (( timeout_seconds > fair_share_seconds )); then
    timeout_seconds="${fair_share_seconds}"
  fi
  if (( timeout_seconds > remaining_seconds )); then
    timeout_seconds="${remaining_seconds}"
  fi

  printf '%s\n' "${timeout_seconds}"
}

{
  echo "hostname=$(hostname)"
  echo "iter_tag=${ITER_TAG}"
  echo "stage_dir=${STAGE_DIR}"
  echo "slurm_job_id=${SLURM_JOB_ID:-}"
  echo "full_verify_lookup_workloads=${FULL_VERIFY_LOOKUP_WORKLOADS}"
  printf 'full_workload_schedule=%s\n' "${FULL_WORKLOAD_SCHEDULE[*]}"
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

for dataset in "${FULL_DATASETS[@]}"; do
  stage_dataset_if_present "${dataset}"
done

download_if_missing \
  "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" \
  "https://www.dropbox.com/scl/fi/hngvfbz1a2tkwpebjngb9/fb_100M_public_uint64?rlkey=px31l6wj9tnic4z604bt6s55n&st=d3iuhhgx&dl=0"

if [[ ! -f "${SCRATCH_ROOT}/cos568_data/books_100M_public_uint64" || \
      ! -f "${SCRATCH_ROOT}/cos568_data/osmc_100M_public_uint64" ]]; then
  bash ./scripts/download_dataset.sh
fi

cmake -S . -B build_scratch -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build_scratch -j "${BUILD_JOBS}"

for dataset in "${FULL_DATASETS[@]}"; do
  generate_ops_if_missing "${dataset}" "0.900000i" --insert-ratio 0.9 --negative-lookup-ratio 0.5 --mix
  generate_ops_if_missing "${dataset}" "0.100000i" --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
done

: > "${SCRATCH_ROOT}/benchmark.stdout"
: > "${SCRATCH_ROOT}/benchmark.stderr"
echo '{"decision":"continue","reason":"no self-evaluation run yet"}' > "${SCRATCH_ROOT}/self_eval.json"

run_workload() {
  local dataset="$1"
  local token="$2"
  local timeout_budget="$3"
  local verify_flag="$4"
  /usr/bin/time -v \
    timeout "${timeout_budget}" \
    env TLI_RESULTS_DIR="${SCRATCH_ROOT}/results" \
    ./build_scratch/benchmark \
      "./data/${dataset}" \
      "./data/${dataset}_ops_2M_0.000000rq_0.500000nl_${token}_0m_mix" \
      --through --csv --only HybridPGMLIPP -r 1 \
      ${verify_flag:+"${verify_flag}"} \
      >> "${SCRATCH_ROOT}/benchmark.stdout" \
      2>> "${SCRATCH_ROOT}/benchmark.stderr"
}

set +e
rc=0
benchmark_deadline_epoch="$(( $(date +%s) + FULL_BENCHMARK_BUDGET_SECONDS ))"
total_workloads="${#FULL_WORKLOAD_SCHEDULE[@]}"
for (( workload_index=0; workload_index<total_workloads; workload_index++ )); do
  workload_spec="${FULL_WORKLOAD_SCHEDULE[workload_index]}"
  dataset="${workload_spec%%:*}"
  token="${workload_spec##*:}"

  now_epoch="$(date +%s)"
  remaining_seconds="$(( benchmark_deadline_epoch - now_epoch ))"
  if (( remaining_seconds <= 0 )); then
    rc=124
    break
  fi
  workloads_remaining="$(( total_workloads - workload_index ))"
  timeout_seconds="$(compute_workload_timeout_seconds "${remaining_seconds}" "${workloads_remaining}")"

  verify_flag=""
  if [[ "${FULL_VERIFY_LOOKUP_WORKLOADS}" == "1" && "${token}" == "0.100000i" ]]; then
    # Screening already exercises verified mixed workloads. Full runs default
    # to throughput collection so they can emit comparable CSVs before the
    # global budget expires, but retain an override for focused diagnosis.
    verify_flag="--verify"
  fi

  run_workload "${dataset}" "${token}" "$(format_timeout_seconds "${timeout_seconds}")" "${verify_flag}"
  rc=$?
  if [[ "${rc}" -ne 0 ]]; then
    break
  fi

  python3 ./scripts/self_evaluate_m3_candidate.py \
    --repo-root "${REPO_ROOT}" \
    --results-dir "${SCRATCH_ROOT}/results" \
    --mode full \
    --output "${SCRATCH_ROOT}/self_eval.json" \
    > /dev/null
  eval_rc=$?
  if [[ "${eval_rc}" -eq 42 ]]; then
    rc=42
    break
  fi
  if [[ "${eval_rc}" -ne 0 ]]; then
    rc="${eval_rc}"
    break
  fi
done
set -e

echo "${rc}" > "${SCRATCH_ROOT}/benchmark_status.txt"
