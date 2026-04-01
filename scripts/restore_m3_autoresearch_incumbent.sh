#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
INCUMBENT_DIR="${INCUMBENT_DIR:-${REPO_ROOT}/autoresearch/incumbent_stage}"

if [[ ! -d "${INCUMBENT_DIR}" ]]; then
  echo "No incumbent stage found at ${INCUMBENT_DIR}" >&2
  exit 1
fi

rsync -a "${INCUMBENT_DIR}/" "${REPO_ROOT}/"
echo "Restored incumbent Milestone 3 files from ${INCUMBENT_DIR}"
