#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <iteration-tag> <screen|full>" >&2
  exit 1
fi

ITER_TAG="$1"
MODE="$2"
REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
STAGE_DIR="${STAGE_DIR:-${REPO_ROOT}/iterations/${ITER_TAG}_autoresearch_stage}"
ARCHIVE_ROOT="${ARCHIVE_ROOT:-${REPO_ROOT}/slurm_runs/${ITER_TAG}}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-64G}"
TIME_LIMIT="${TIME_LIMIT:-04:00:00}"

if [[ ! -d "${STAGE_DIR}" ]]; then
  echo "Missing stage dir: ${STAGE_DIR}" >&2
  echo "Run scripts/stage_m3_autoresearch_iteration.sh ${ITER_TAG} first." >&2
  exit 1
fi

case "${MODE}" in
  screen)
    COMPUTE_SCRIPT="${REPO_ROOT}/scripts/run_m3_autoresearch_screen_compute.sh"
    JOB_NAME="${ITER_TAG}-screen"
    ;;
  full)
    COMPUTE_SCRIPT="${REPO_ROOT}/scripts/run_m3_autoresearch_full_compute.sh"
    JOB_NAME="${ITER_TAG}-full"
    ;;
  *)
    echo "Mode must be screen or full." >&2
    exit 1
    ;;
esac

mkdir -p "${ARCHIVE_ROOT}"

SBATCH_CMD=(sbatch)
if [[ "${SBATCH_PARSABLE:-0}" == "1" ]]; then
  SBATCH_CMD+=(--parsable)
fi

"${SBATCH_CMD[@]}" \
  --job-name="${JOB_NAME}" \
  --partition=all \
  --nodes=1 \
  --ntasks=1 \
  --cpus-per-task="${CPUS}" \
  --mem="${MEMORY}" \
  --time="${TIME_LIMIT}" \
  --output="${ARCHIVE_ROOT}/slurm.%j.out" \
  --error="${ARCHIVE_ROOT}/slurm.%j.err" \
  --export=ALL,REPO_ROOT="${REPO_ROOT}",ITER_TAG="${ITER_TAG}",STAGE_DIR="${STAGE_DIR}",ARCHIVE_ROOT="${ARCHIVE_ROOT}" \
  "${COMPUTE_SCRIPT}"
