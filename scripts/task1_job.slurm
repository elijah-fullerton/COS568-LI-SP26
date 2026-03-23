#!/bin/bash
#SBATCH --job-name=cos568-task1
#SBATCH --output=task1-%j.out
#SBATCH --error=task1-%j.err
#SBATCH --time=02:00:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G

set -euo pipefail

module load anaconda3/2023.3

export MPLBACKEND=Agg
export OMP_NUM_THREADS=1

echo "Running Task 1 pipeline from $(pwd)"
bash scripts/run_all.sh
