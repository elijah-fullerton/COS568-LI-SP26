#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/slurm_runs/m3_iter12}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-64G}"
TIME_LIMIT="${TIME_LIMIT:-04:00:00}"
VERIFY_ONLY="${VERIFY_ONLY:-1}"

mkdir -p "${ARCHIVE_ROOT}"

sbatch \
  --job-name=m3i12-canary \
  --partition=all \
  --nodes=1 \
  --ntasks=1 \
  --cpus-per-task="${CPUS}" \
  --mem="${MEMORY}" \
  --time="${TIME_LIMIT}" \
  --output="${ARCHIVE_ROOT}/slurm.%j.out" \
  --error="${ARCHIVE_ROOT}/slurm.%j.err" \
  --export=ALL,REPO_ROOT="${REPO_ROOT}",ARCHIVE_ROOT="${ARCHIVE_ROOT}",VERIFY_ONLY="${VERIFY_ONLY}" \
  "${REPO_ROOT}/scripts/run_iteration12_compute.sh"
