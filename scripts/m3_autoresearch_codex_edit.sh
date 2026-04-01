#!/usr/bin/env bash

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/auto/u/ef0952/projects/COS568-LI-SP26}"
PROMPT_FILE="${PROMPT_FILE:-${REPO_ROOT}/autoresearch/codex_edit_prompt.md}"
MODEL="${MODEL:-gpt-5.4}"
OUTPUT_FILE="${OUTPUT_FILE:-${REPO_ROOT}/autoresearch/last_codex_message.txt}"

if [[ ! -f "${PROMPT_FILE}" ]]; then
  echo "Missing prompt file: ${PROMPT_FILE}" >&2
  exit 1
fi

cd "${REPO_ROOT}"

codex exec \
  --cd "${REPO_ROOT}" \
  --model "${MODEL}" \
  --dangerously-bypass-approvals-and-sandbox \
  --output-last-message "${OUTPUT_FILE}" \
  - < "${PROMPT_FILE}"
