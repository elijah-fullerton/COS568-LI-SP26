#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <iteration-tag>" >&2
  exit 1
fi

ITER_TAG="$1"
REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
STAGE_DIR="${STAGE_DIR:-${REPO_ROOT}/iterations/${ITER_TAG}_autoresearch_stage}"

FILES=(
  "benchmark.h"
  "util.h"
  "benchmarks/benchmark_hybrid_pgm_lipp.cc"
  "benchmarks/benchmark_hybrid_pgm_lipp.h"
  "competitors/hybrid_pgm_lipp.h"
  "competitors/PGM-index/include/pgm_index_dynamic.hpp"
  "competitors/lipp/src/core/lipp.h"
)

mkdir -p "${STAGE_DIR}"

for relpath in "${FILES[@]}"; do
  mkdir -p "${STAGE_DIR}/$(dirname "${relpath}")"
  cp "${REPO_ROOT}/${relpath}" "${STAGE_DIR}/${relpath}"
done

cat > "${STAGE_DIR}/MANIFEST.txt" <<EOF
iter_tag=${ITER_TAG}
created_at=$(date -Is)
repo_root=${REPO_ROOT}
files=
$(printf '%s\n' "${FILES[@]}")
EOF

echo "Staged Milestone 3 files in ${STAGE_DIR}"
