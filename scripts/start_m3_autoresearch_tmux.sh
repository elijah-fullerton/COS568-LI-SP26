#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
SESSION_NAME="${SESSION_NAME:-m3-autoresearch}"
ITERATIONS="${ITERATIONS:-0}"
PROMOTE_SCREEN="${PROMOTE_SCREEN:-always}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-64G}"
TIME_LIMIT="${TIME_LIMIT:-04:00:00}"
MODEL="${MODEL:-gpt-5.4}"
SLEEP_SECONDS="${SLEEP_SECONDS:-0}"
LOG_DIR="${LOG_DIR:-${REPO_ROOT}/autoresearch/logs}"
LOG_FILE="${LOG_FILE:-${LOG_DIR}/${SESSION_NAME}.log}"

mkdir -p "${LOG_DIR}"

if tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
  echo "tmux session already exists: ${SESSION_NAME}" >&2
  echo "Attach with: tmux attach -t ${SESSION_NAME}" >&2
  exit 1
fi

CMD=$(
  cat <<EOF
cd ${REPO_ROOT} && \
env ITERATIONS=$(printf '%q' "${ITERATIONS}") \
    PROMOTE_SCREEN=$(printf '%q' "${PROMOTE_SCREEN}") \
    CPUS=$(printf '%q' "${CPUS}") \
    MEMORY=$(printf '%q' "${MEMORY}") \
    TIME_LIMIT=$(printf '%q' "${TIME_LIMIT}") \
    MODEL=$(printf '%q' "${MODEL}") \
    SLEEP_SECONDS=$(printf '%q' "${SLEEP_SECONDS}") \
    REPO_ROOT=$(printf '%q' "${REPO_ROOT}") \
    bash scripts/run_m3_autoresearch_codex.sh >> $(printf '%q' "${LOG_FILE}") 2>&1
EOF
)

tmux new-session -d -s "${SESSION_NAME}" "${CMD}"

echo "Started tmux session: ${SESSION_NAME}"
echo "Log file: ${LOG_FILE}"
echo "Attach: tmux attach -t ${SESSION_NAME}"
echo "Stop: bash scripts/stop_m3_autoresearch.sh ${SESSION_NAME}"
