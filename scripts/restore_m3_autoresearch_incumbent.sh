#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
INCUMBENT_DIR="${INCUMBENT_DIR:-${REPO_ROOT}/autoresearch/incumbent_stage}"

if [[ ! -d "${INCUMBENT_DIR}" ]]; then
  echo "No incumbent stage found at ${INCUMBENT_DIR}" >&2
  exit 1
fi

rsync -a "${INCUMBENT_DIR}/" "${REPO_ROOT}/"
echo "Restored incumbent Milestone 3 files from ${INCUMBENT_DIR}"
