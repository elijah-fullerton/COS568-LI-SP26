#!/bin/bash

set -euo pipefail

if [ ! -f CMakeLists.txt ]; then
    echo "Error: run this script from the repository root."
    exit 1
fi

if command -v nproc >/dev/null 2>&1; then
    BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
else
    BUILD_JOBS="${BUILD_JOBS:-8}"
fi

echo "Configuring the benchmark build in build/..."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

echo "Building benchmark and workload generator with ${BUILD_JOBS} job(s)..."
cmake --build build --parallel "${BUILD_JOBS}"

if [ ! -f build/benchmark ]; then
    echo "Error: build/benchmark was not produced."
    exit 1
fi

if [ ! -f build/generate ]; then
    echo "Error: build/generate was not produced."
    exit 1
fi

echo "Build complete."
echo "Benchmark binary: $(pwd)/build/benchmark"
echo "Generator binary: $(pwd)/build/generate"