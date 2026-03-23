#!/bin/bash

set -euo pipefail

if [ ! -f "scripts/task1_job.slurm" ]; then
    echo "Error: scripts/task1_job.slurm does not exist."
    exit 1
fi

sbatch scripts/task1_job.slurm
