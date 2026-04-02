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
WORKLOAD_TIMEOUT="${WORKLOAD_TIMEOUT:-3m}"
SCREEN_BASELINE_TIMEOUT="${SCREEN_BASELINE_TIMEOUT:-90s}"
SCREEN_BASELINE_RESULTS_DIR="${SCREEN_BASELINE_RESULTS_DIR:-${REPO_ROOT}/results_milestone3}"
SCREEN_USE_CACHED_BASELINES="${SCREEN_USE_CACHED_BASELINES:-1}"
SCREEN_LOOKUP_OPS="${SCREEN_LOOKUP_OPS:-25000}"
SCREEN_INSERT_OPS="${SCREEN_INSERT_OPS:-25000}"
SCREEN_SCALE_LOOKUP_OPS="${SCREEN_SCALE_LOOKUP_OPS:-5000}"
SCREEN_SCALE_DATASET_KEYS="${SCREEN_SCALE_DATASET_KEYS:-250000}"
SCREEN_SCALE_DATASET_NAME="${SCREEN_SCALE_DATASET_NAME:-fb_100M_public_scale_uint64}"
SCREEN_DATASET_KEYS="${SCREEN_DATASET_KEYS:-200000}"
SCREEN_DATASET_NAME="${SCREEN_DATASET_NAME:-fb_100M_public_screen_uint64}"
SCREEN_SMOKE_TIMEOUT="${SCREEN_SMOKE_TIMEOUT:-45s}"
SCREEN_SCALE_TIMEOUT="${SCREEN_SCALE_TIMEOUT:-90s}"
SCREEN_SMOKE_DATASET_KEYS="${SCREEN_SMOKE_DATASET_KEYS:-50000}"
SCREEN_SMOKE_LOOKUP_OPS="${SCREEN_SMOKE_LOOKUP_OPS:-2500}"
SCREEN_SMOKE_INSERT_OPS="${SCREEN_SMOKE_INSERT_OPS:-2500}"
SCREEN_SMOKE_DATASET_NAME="${SCREEN_SMOKE_DATASET_NAME:-fb_100M_public_smoke_uint64}"
SCREEN_ENABLE_SCALE_CANARY="${SCREEN_ENABLE_SCALE_CANARY:-0}"

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

shrink_uint64_dataset_in_place() {
  local dataset_path="$1"
  local keep_keys="$2"
  python3 - "$dataset_path" "$keep_keys" <<'PY'
import os
import struct
import sys

dataset_path = sys.argv[1]
keep_keys = int(sys.argv[2])

with open(dataset_path, "rb") as handle:
    raw_count = handle.read(8)
    if len(raw_count) != 8:
        raise SystemExit(f"failed to read key count from {dataset_path}")
    total_keys = struct.unpack("<Q", raw_count)[0]
    keep_keys = min(keep_keys, total_keys)
    payload = handle.read(keep_keys * 8)
    if len(payload) != keep_keys * 8:
        raise SystemExit(
            f"failed to read {keep_keys} keys from {dataset_path}; "
            f"read {len(payload) // 8}"
        )

tmp_path = dataset_path + ".screen_tmp"
with open(tmp_path, "wb") as handle:
    handle.write(struct.pack("<Q", keep_keys))
    handle.write(payload)
os.replace(tmp_path, dataset_path)
print(
    f"screen_dataset_keys={keep_keys} "
    f"(truncated from {total_keys})"
)
PY
}

resolve_ops_path() {
  local dataset_name="$1"
  local ops_count="$2"
  local workload_token="$3"
  local nice_ops_count
  nice_ops_count="$(format_ops_count "${ops_count}")"

  local pattern="./data/${dataset_name}_ops_${nice_ops_count}_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  local matches=()
  shopt -s nullglob
  matches=(${pattern})
  shopt -u nullglob

  if [[ "${#matches[@]}" -eq 0 ]]; then
    echo "unable to find generated ops file matching ${pattern}" >&2
    return 1
  fi

  printf '%s\n' "${matches[0]}"
}

stage_cached_ops_if_present() {
  local dataset_name="$1"
  local ops_count="$2"
  local workload_token="$3"
  local nice_ops_count
  nice_ops_count="$(format_ops_count "${ops_count}")"
  local cache_path="${OPS_CACHE_ROOT}/${dataset_name}_ops_${nice_ops_count}_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  local scratch_path="${SCRATCH_ROOT}/workspace/data/$(basename "${cache_path}")"
  if [[ -f "${scratch_path}" || ! -f "${cache_path}" ]]; then
    return
  fi
  mkdir -p "${OPS_CACHE_ROOT}"
  cp -f "${cache_path}" "${scratch_path}"
}

cache_generated_ops() {
  local dataset_name="$1"
  local ops_count="$2"
  local workload_token="$3"
  local nice_ops_count
  nice_ops_count="$(format_ops_count "${ops_count}")"
  local generated_path="${SCRATCH_ROOT}/workspace/data/${dataset_name}_ops_${nice_ops_count}_0.000000rq_0.500000nl_${workload_token}_0m_mix"
  local cache_path="${OPS_CACHE_ROOT}/$(basename "${generated_path}")"
  if [[ -f "${generated_path}" && ! -f "${cache_path}" ]]; then
    mkdir -p "${OPS_CACHE_ROOT}"
    cp -f "${generated_path}" "${cache_path}"
  fi
}

generate_ops_if_missing() {
  local dataset_name="$1"
  local ops_count="$2"
  local workload_token="$3"
  shift 3
  stage_cached_ops_if_present "${dataset_name}" "${ops_count}" "${workload_token}"
  if resolve_ops_path "${dataset_name}" "${ops_count}" "${workload_token}" >/dev/null 2>&1; then
    return
  fi
  ./build_scratch/generate "./data/${dataset_name}" "${ops_count}" "$@"
  cache_generated_ops "${dataset_name}" "${ops_count}" "${workload_token}"
}

format_ops_count() {
  local ops_count="$1"
  local nice_ops_count="${ops_count}"
  if (( ops_count >= 1000000000 )) && (( ops_count % 1000000000 == 0 )); then
    nice_ops_count="$((ops_count / 1000000000))B"
  elif (( ops_count >= 1000000 )) && (( ops_count % 1000000 == 0 )); then
    nice_ops_count="$((ops_count / 1000000))M"
  elif (( ops_count >= 1000 )) && (( ops_count % 1000 == 0 )); then
    nice_ops_count="$((ops_count / 1000))K"
  fi
  printf '%s\n' "${nice_ops_count}"
}

alias_results_csv() {
  local src_dataset_name="$1"
  local src_ops_count="$2"
  local workload_token="$3"
  local dst_dataset_name="$4"
  local dst_ops_token="$5"
  local actual_ops_token
  actual_ops_token="$(format_ops_count "${src_ops_count}")"
  local src_path="${SCRATCH_ROOT}/results/${src_dataset_name}_ops_${actual_ops_token}_0.000000rq_0.500000nl_${workload_token}_0m_mix_results_table.csv"
  local dst_path="${SCRATCH_ROOT}/results/${dst_dataset_name}_ops_${dst_ops_token}_0.000000rq_0.500000nl_${workload_token}_0m_mix_results_table.csv"
  if [[ -f "${src_path}" ]]; then
    cp -f "${src_path}" "${dst_path}"
  fi
}

append_cached_baseline_rows() {
  local src_path="$1"
  local dst_path="$2"
  python3 - "$src_path" "$dst_path" <<'PY'
import csv
import sys
from pathlib import Path

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
if not src.exists():
    raise SystemExit(1)

with src.open(newline="", encoding="ascii") as handle:
    rows = list(csv.reader(handle))

if rows and rows[0] and rows[0][0] == "index_name":
    data_rows = rows[1:]
else:
    data_rows = rows

selected = [
    row for row in data_rows
    if row and row[0] in {"DynamicPGM", "LIPP"}
]
if not selected:
    raise SystemExit(2)

with dst.open("a", newline="", encoding="ascii") as handle:
    writer = csv.writer(handle)
    writer.writerows(selected)
PY
}

cache_screen_baselines() {
  local cached_results_dir="$1"
  local cached_ops_token="2M"
  local cached_lookup_csv="${cached_results_dir}/fb_100M_public_uint64_ops_${cached_ops_token}_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv"
  local cached_insert_csv="${cached_results_dir}/fb_100M_public_uint64_ops_${cached_ops_token}_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv"
  local dst_lookup_csv="${SCRATCH_ROOT}/results/fb_100M_public_uint64_ops_$(format_ops_count "${SCREEN_LOOKUP_OPS}")_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv"
  local dst_insert_csv="${SCRATCH_ROOT}/results/fb_100M_public_uint64_ops_$(format_ops_count "${SCREEN_INSERT_OPS}")_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv"

  append_cached_baseline_rows "${cached_lookup_csv}" "${dst_lookup_csv}" || return 1
  append_cached_baseline_rows "${cached_insert_csv}" "${dst_insert_csv}" || return 1
}

cache_scale_baselines() {
  local cached_results_dir="$1"
  local cached_lookup_csv="${cached_results_dir}/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv"
  local dst_scale_csv="${SCRATCH_ROOT}/results/fb_100M_public_uint64_ops_$(format_ops_count "${SCREEN_SCALE_LOOKUP_OPS}")_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv"
  append_cached_baseline_rows "${cached_lookup_csv}" "${dst_scale_csv}" || return 1
}

run_workload() {
  local phase_name="$1"
  local timeout_budget="$2"
  local dataset_path="$3"
  local ops_path="$4"
  local workload_token="$5"
  local index_name="$6"
  local verify_flag="$7"
  shift 7

  echo "== ${phase_name} ${workload_token} ${index_name} ==" >> "${SCRATCH_ROOT}/benchmark.stdout"
  /usr/bin/time -v \
    timeout "${timeout_budget}" \
    env TLI_RESULTS_DIR="${SCRATCH_ROOT}/results" \
    ./build_scratch/benchmark \
      "${dataset_path}" \
      "${ops_path}" \
      --through --csv --only "${index_name}" -r 1 ${verify_flag:+"${verify_flag}"} "$@" \
      >> "${SCRATCH_ROOT}/benchmark.stdout" \
      2>> "${SCRATCH_ROOT}/benchmark.stderr"
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

stage_dataset_if_present "fb_100M_public_uint64"
download_if_missing \
  "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" \
  "https://www.dropbox.com/scl/fi/hngvfbz1a2tkwpebjngb9/fb_100M_public_uint64?rlkey=px31l6wj9tnic4z604bt6s55n&st=d3iuhhgx&dl=0"

cp -f \
  "${SCRATCH_ROOT}/cos568_data/fb_100M_public_uint64" \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_SCALE_DATASET_NAME}"
shrink_uint64_dataset_in_place \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_SCALE_DATASET_NAME}" \
  "${SCREEN_SCALE_DATASET_KEYS}" | tee -a "${SCRATCH_ROOT}/job_env.txt"

cp -f \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_SCALE_DATASET_NAME}" \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_DATASET_NAME}"
shrink_uint64_dataset_in_place \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_DATASET_NAME}" \
  "${SCREEN_DATASET_KEYS}" | tee -a "${SCRATCH_ROOT}/job_env.txt"

cp -f \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_DATASET_NAME}" \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_SMOKE_DATASET_NAME}"
shrink_uint64_dataset_in_place \
  "${SCRATCH_ROOT}/cos568_data/${SCREEN_SMOKE_DATASET_NAME}" \
  "${SCREEN_SMOKE_DATASET_KEYS}" | tee -a "${SCRATCH_ROOT}/job_env.txt"

cmake -S . -B build_scratch -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_FLAGS="-DAUTORESEARCH_SCREEN_SAFE=1"
cmake --build build_scratch -j "${BUILD_JOBS}"

generate_ops_if_missing "${SCREEN_DATASET_NAME}" "${SCREEN_LOOKUP_OPS}" "0.100000i" --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
generate_ops_if_missing "${SCREEN_DATASET_NAME}" "${SCREEN_INSERT_OPS}" "0.900000i" --insert-ratio 0.9 --negative-lookup-ratio 0.5 --mix
generate_ops_if_missing "${SCREEN_SCALE_DATASET_NAME}" "${SCREEN_SCALE_LOOKUP_OPS}" "0.100000i" --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
generate_ops_if_missing "${SCREEN_SMOKE_DATASET_NAME}" "${SCREEN_SMOKE_LOOKUP_OPS}" "0.100000i" --insert-ratio 0.1 --negative-lookup-ratio 0.5 --mix
generate_ops_if_missing "${SCREEN_SMOKE_DATASET_NAME}" "${SCREEN_SMOKE_INSERT_OPS}" "0.900000i" --insert-ratio 0.9 --negative-lookup-ratio 0.5 --mix

lookup_ops_path="$(resolve_ops_path "${SCREEN_DATASET_NAME}" "${SCREEN_LOOKUP_OPS}" "0.100000i")"
insert_ops_path="$(resolve_ops_path "${SCREEN_DATASET_NAME}" "${SCREEN_INSERT_OPS}" "0.900000i")"
scale_lookup_ops_path="$(resolve_ops_path "${SCREEN_SCALE_DATASET_NAME}" "${SCREEN_SCALE_LOOKUP_OPS}" "0.100000i")"
smoke_lookup_ops_path="$(resolve_ops_path "${SCREEN_SMOKE_DATASET_NAME}" "${SCREEN_SMOKE_LOOKUP_OPS}" "0.100000i")"
smoke_insert_ops_path="$(resolve_ops_path "${SCREEN_SMOKE_DATASET_NAME}" "${SCREEN_SMOKE_INSERT_OPS}" "0.900000i")"

: > "${SCRATCH_ROOT}/benchmark.stdout"
: > "${SCRATCH_ROOT}/benchmark.stderr"

set +e
run_workload "smoke" "${SCREEN_SMOKE_TIMEOUT}" "./data/${SCREEN_SMOKE_DATASET_NAME}" "${smoke_lookup_ops_path}" "0.100000i" "HybridPGMLIPP" ""
rc_lookup=$?

run_workload "smoke" "${SCREEN_SMOKE_TIMEOUT}" "./data/${SCREEN_SMOKE_DATASET_NAME}" "${smoke_insert_ops_path}" "0.900000i" "HybridPGMLIPP" ""
rc_insert=$?

if [[ "${rc_lookup}" -eq 0 && "${rc_insert}" -eq 0 ]]; then
  run_workload "screen" "${WORKLOAD_TIMEOUT}" "./data/${SCREEN_DATASET_NAME}" "${lookup_ops_path}" "0.100000i" "HybridPGMLIPP" "--verify"
  rc_lookup=$?
  if [[ "${rc_lookup}" -eq 0 ]]; then
    alias_results_csv \
      "${SCREEN_DATASET_NAME}" \
      "${SCREEN_LOOKUP_OPS}" \
      "0.100000i" \
      "fb_100M_public_uint64" \
      "$(format_ops_count "${SCREEN_LOOKUP_OPS}")"
  fi
fi

if [[ "${rc_lookup}" -eq 0 && "${rc_insert}" -eq 0 ]]; then
  run_workload "screen" "${WORKLOAD_TIMEOUT}" "./data/${SCREEN_DATASET_NAME}" "${insert_ops_path}" "0.900000i" "HybridPGMLIPP" "--verify"
  rc_insert=$?
  if [[ "${rc_insert}" -eq 0 ]]; then
    alias_results_csv \
      "${SCREEN_DATASET_NAME}" \
      "${SCREEN_INSERT_OPS}" \
      "0.900000i" \
      "fb_100M_public_uint64" \
      "$(format_ops_count "${SCREEN_INSERT_OPS}")"
  fi
fi

rc_gate=0
if [[ "${rc_lookup}" -eq 0 && "${rc_insert}" -eq 0 ]]; then
  if [[ "${SCREEN_USE_CACHED_BASELINES}" -eq 1 ]] && \
     cache_screen_baselines "${SCREEN_BASELINE_RESULTS_DIR}"; then
    echo "screen_baselines=cached:${SCREEN_BASELINE_RESULTS_DIR}" \
      >> "${SCRATCH_ROOT}/job_env.txt"
  else
    for index_name in DynamicPGM LIPP; do
      run_workload "baseline-scale-canary" "${SCREEN_BASELINE_TIMEOUT}" "./data/${SCREEN_SCALE_DATASET_NAME}" "${scale_lookup_ops_path}" "0.100000i" "${index_name}" ""
      rc_gate=$?
      if [[ "${rc_gate}" -ne 0 ]]; then
        break
      fi
      alias_results_csv \
        "${SCREEN_SCALE_DATASET_NAME}" \
        "${SCREEN_SCALE_LOOKUP_OPS}" \
        "0.100000i" \
        "fb_100M_public_uint64" \
        "$(format_ops_count "${SCREEN_SCALE_LOOKUP_OPS}")"

      run_workload "baseline" "${SCREEN_BASELINE_TIMEOUT}" "./data/${SCREEN_DATASET_NAME}" "${lookup_ops_path}" "0.100000i" "${index_name}" ""
      rc_gate=$?
      if [[ "${rc_gate}" -ne 0 ]]; then
        break
      fi
      alias_results_csv \
        "${SCREEN_DATASET_NAME}" \
        "${SCREEN_LOOKUP_OPS}" \
        "0.100000i" \
        "fb_100M_public_uint64" \
        "$(format_ops_count "${SCREEN_LOOKUP_OPS}")"

      run_workload "baseline" "${SCREEN_BASELINE_TIMEOUT}" "./data/${SCREEN_DATASET_NAME}" "${insert_ops_path}" "0.900000i" "${index_name}" ""
      rc_gate=$?
      if [[ "${rc_gate}" -ne 0 ]]; then
        break
      fi
      alias_results_csv \
        "${SCREEN_DATASET_NAME}" \
        "${SCREEN_INSERT_OPS}" \
        "0.900000i" \
        "fb_100M_public_uint64" \
        "$(format_ops_count "${SCREEN_INSERT_OPS}")"
    done
    if [[ "${rc_gate}" -eq 0 ]]; then
      echo "screen_baselines=live" >> "${SCRATCH_ROOT}/job_env.txt"
    fi
  fi
fi

rc_scale=0
if [[ "${SCREEN_ENABLE_SCALE_CANARY}" -eq 1 && "${rc_lookup}" -eq 0 && "${rc_insert}" -eq 0 && "${rc_gate}" -eq 0 ]]; then
  # Run the larger lookup canary only after the verified screen and baselines
  # have produced the main screening signal. This keeps slow candidates
  # measurable instead of failing early in diagnostics.
  run_workload "scale-canary" "${SCREEN_SCALE_TIMEOUT}" "./data/${SCREEN_SCALE_DATASET_NAME}" "${scale_lookup_ops_path}" "0.100000i" "HybridPGMLIPP" ""
  rc_scale=$?
  if [[ "${rc_scale}" -eq 0 ]]; then
    alias_results_csv \
      "${SCREEN_SCALE_DATASET_NAME}" \
      "${SCREEN_SCALE_LOOKUP_OPS}" \
      "0.100000i" \
      "fb_100M_public_uint64" \
      "$(format_ops_count "${SCREEN_SCALE_LOOKUP_OPS}")"
    if [[ "${SCREEN_USE_CACHED_BASELINES}" -eq 1 ]]; then
      cache_scale_baselines "${SCREEN_BASELINE_RESULTS_DIR}" || true
    fi
  else
    echo "scale_canary_status=nonfatal_rc_${rc_scale}" >> "${SCRATCH_ROOT}/job_env.txt"
  fi
fi

if [[ "${rc_lookup}" -eq 0 && "${rc_insert}" -eq 0 && "${rc_gate}" -eq 0 ]]; then
  analysis_args=(
    --results-dir "${SCRATCH_ROOT}/results"
    --screen-ops-token "$(format_ops_count "${SCREEN_LOOKUP_OPS}")"
    --abort-on-hybrid-loss
  )
  if [[ "${rc_scale}" -eq 0 ]]; then
    analysis_args+=(--scale-lookup-ops-token "$(format_ops_count "${SCREEN_SCALE_LOOKUP_OPS}")")
  fi
  python3 ./scripts/analysis_m3_screen.py \
    "${analysis_args[@]}" \
    >> "${SCRATCH_ROOT}/benchmark.stdout" \
    2>> "${SCRATCH_ROOT}/benchmark.stderr"
  rc_gate=$?
fi
set -e

if [[ "${rc_gate}" -ne 0 ]]; then
  echo "${rc_gate}" > "${SCRATCH_ROOT}/benchmark_status.txt"
elif [[ "${rc_insert}" -ne 0 ]]; then
  echo "${rc_insert}" > "${SCRATCH_ROOT}/benchmark_status.txt"
elif [[ "${rc_lookup}" -ne 0 ]]; then
  echo "${rc_lookup}" > "${SCRATCH_ROOT}/benchmark_status.txt"
else
  echo "0" > "${SCRATCH_ROOT}/benchmark_status.txt"
fi
