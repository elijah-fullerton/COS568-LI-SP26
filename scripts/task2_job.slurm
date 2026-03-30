#!/usr/bin/env bash
#SBATCH --job-name=cos568-m2
#SBATCH --output=results_milestone2/slurm-%j.out
#SBATCH --error=results_milestone2/slurm-%j.err
#SBATCH --time=02:00:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G

set -euo pipefail

mkdir -p results_milestone2

if command -v module >/dev/null 2>&1; then
    module load anaconda3/2023.3 || true
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8

./scripts/run_milestone2.sh
python3 ./scripts/analysis_milestone2.py
