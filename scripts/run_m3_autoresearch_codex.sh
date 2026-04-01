#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
ITERATIONS="${ITERATIONS:-0}"
PROMOTE_SCREEN="${PROMOTE_SCREEN:-always}"
CPUS="${CPUS:-8}"
MEMORY="${MEMORY:-64G}"
TIME_LIMIT="${TIME_LIMIT:-04:00:00}"
MODEL="${MODEL:-gpt-5.4}"
SLEEP_SECONDS="${SLEEP_SECONDS:-0}"

cd "${REPO_ROOT}"

python3 scripts/run_m3_autoresearch_loop.py \
  --iterations "${ITERATIONS}" \
  --restore-incumbent-before-edit \
  --promote-screen "${PROMOTE_SCREEN}" \
  --cpus "${CPUS}" \
  --memory "${MEMORY}" \
  --time-limit "${TIME_LIMIT}" \
  --sleep-seconds "${SLEEP_SECONDS}" \
  --edit-command "MODEL=${MODEL} REPO_ROOT=${REPO_ROOT} bash scripts/m3_autoresearch_codex_edit.sh"
