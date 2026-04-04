#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26-m3_adaptive_filter}"
RUN_TAG="${1:-m3_adaptive_filter_eval}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/eval_runs/${RUN_TAG}}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-64G}"
TIME_LIMIT="${TIME_LIMIT:-04:00:00}"

mkdir -p "${ARCHIVE_ROOT}"

sbatch \
  --job-name="${RUN_TAG}" \
  --partition=all \
  --nodes=1 \
  --ntasks=1 \
  --cpus-per-task="${CPUS}" \
  --mem="${MEMORY}" \
  --time="${TIME_LIMIT}" \
  --exclusive \
  --output="${ARCHIVE_ROOT}/slurm.%j.out" \
  --error="${ARCHIVE_ROOT}/slurm.%j.err" \
  --export=ALL,REPO_ROOT="${REPO_ROOT}",RUN_TAG="${RUN_TAG}",ARCHIVE_ROOT="${ARCHIVE_ROOT}" \
  "${REPO_ROOT}/scripts/run_m3_adaptive_filter_eval_compute.sh"
