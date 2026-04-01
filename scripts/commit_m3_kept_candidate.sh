#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <iteration> <reward> <status> [branch]" >&2
  exit 1
fi

ITERATION="$1"
REWARD="$2"
STATUS="$3"
BRANCH="${4:-}"
REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"

FILES=(
  "benchmark.h"
  "util.h"
  "benchmarks/benchmark_hybrid_pgm_lipp.cc"
  "benchmarks/benchmark_hybrid_pgm_lipp.h"
  "competitors/hybrid_pgm_lipp.h"
  "competitors/PGM-index/include/pgm_index_dynamic.hpp"
  "competitors/lipp/src/core/lipp.h"
)

cd "${REPO_ROOT}"

if [[ -n "${BRANCH}" ]]; then
  current_branch="$(git branch --show-current)"
  if [[ "${current_branch}" != "${BRANCH}" ]]; then
    if git show-ref --verify --quiet "refs/heads/${BRANCH}"; then
      git checkout "${BRANCH}"
    else
      git checkout -b "${BRANCH}"
    fi
  fi
fi

git add "${FILES[@]}"

if git diff --cached --quiet; then
  echo "No staged Milestone 3 source changes to commit."
  exit 0
fi

git commit -m "Keep ${ITERATION} reward=${REWARD} status=${STATUS}"
