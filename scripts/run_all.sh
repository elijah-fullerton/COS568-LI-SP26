#!/bin/bash

set -euo pipefail

echo "=== Starting Task 1 pipeline ==="

chmod +x scripts/*.sh

if [ "${SKIP_DOWNLOAD:-0}" != "1" ]; then
    echo "Step 1/6: Downloading datasets..."
    ./scripts/download_dataset.sh
else
    echo "Step 1/6: Skipping dataset download."
fi

echo "Step 2/6: Ensuring Task 1 CMake configuration is in place..."
./scripts/create_minimal_cmake.sh

if [ "${SKIP_WORKLOADS:-0}" != "1" ]; then
    echo "Step 3/6: Generating workloads..."
    ./scripts/generate_workloads.sh
else
    echo "Step 3/6: Skipping workload generation."
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "Step 4/6: Building benchmark binaries..."
    ./scripts/build_benchmark.sh
else
    echo "Step 4/6: Skipping build."
fi

if [ "${SKIP_BENCHMARKS:-0}" != "1" ]; then
    echo "Step 5/6: Running benchmarks..."
    ./scripts/run_benchmarks.sh
else
    echo "Step 5/6: Skipping benchmark execution."
fi

if [ "${SKIP_ANALYSIS:-0}" != "1" ]; then
    echo "Step 6/6: Analyzing benchmark results..."
    python3 scripts/analysis.py
else
    echo "Step 6/6: Skipping analysis."
fi

echo "=== Task 1 pipeline completed successfully ==="
echo "Raw CSVs: results/"
echo "Plots and summaries: analysis_results/"
