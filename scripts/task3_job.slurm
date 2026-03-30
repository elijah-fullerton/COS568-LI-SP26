#!/usr/bin/env bash
#SBATCH --job-name=cos568-m3
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=32G
#SBATCH --time=08:00:00
#SBATCH --output=task3_job.out
#SBATCH --error=task3_job.err

set -euo pipefail

module load anaconda3/2023.3

cmake -S . -B build
cmake --build build -j

bash scripts/run_milestone3.sh
python3 scripts/analysis_milestone3.py
