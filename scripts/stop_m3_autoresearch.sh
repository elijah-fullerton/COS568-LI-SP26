#!/usr/bin/env bash

set -euo pipefail

SESSION_NAME="${1:-m3-autoresearch}"

if tmux has-session -t "${SESSION_NAME}" 2>/dev/null; then
  tmux kill-session -t "${SESSION_NAME}"
  echo "Stopped tmux session: ${SESSION_NAME}"
else
  echo "No tmux session named ${SESSION_NAME}" >&2
fi

pkill -f '/auto/u/ef0952/projects/COS568-LI-SP26/scripts/run_m3_autoresearch_loop.py' 2>/dev/null || true
pkill -f '/auto/u/ef0952/projects/COS568-LI-SP26/scripts/m3_autoresearch_codex_edit.sh' 2>/dev/null || true
pkill -f 'codex exec' 2>/dev/null || true

job_ids="$(squeue -h -u "$USER" -o '%A %j' | awk '/m3_iter.*-(screen|full)/ {print $1}')"
if [[ -n "${job_ids}" ]]; then
  # shellcheck disable=SC2086
  scancel ${job_ids}
  echo "Cancelled SLURM jobs: ${job_ids//$'\n'/ }"
else
  echo "No matching SLURM jobs to cancel."
fi
