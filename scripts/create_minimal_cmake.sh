#!/bin/bash

set -euo pipefail

if [ ! -f CMakeLists.txt ]; then
    echo "Error: run this script from the repository root."
    exit 1
fi

if grep -q 'benchmarks/benchmark_dynamic_pgm.cc' CMakeLists.txt \
    && grep -q 'benchmarks/benchmark_lipp.cc' CMakeLists.txt \
    && grep -q 'benchmarks/benchmark_hybrid_pgm_lipp.cc' CMakeLists.txt \
    && grep -q 'benchmarks/benchmark_btree.cc' CMakeLists.txt \
    && grep -q 'add_executable(generate generate.cc' CMakeLists.txt; then
    echo "CMakeLists.txt already contains the Task 1 minimal benchmark build."
    exit 0
fi

if [ ! -f CMakeLists.txt.original ]; then
    echo "Backing up the current CMakeLists.txt to CMakeLists.txt.original..."
    cp CMakeLists.txt CMakeLists.txt.original
fi

echo "Writing Task 1 minimal CMakeLists.txt..."
cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.10)
project(WOSD)

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffast-math -Wall -Wfatal-errors -march=native")

# Enable OpenMP if available
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fopenmp HAS_OPENMP)
if (HAS_OPENMP)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")
endif()

set(CMAKE_CXX_STANDARD 17)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_subdirectory(dtl)

if (${APPLE})
    include_directories(/usr/local/include/) # required by Mac OS to find boost
endif ()

set(SOURCE_FILES util.h)

# We only include what we need for DynamicPGM, B+Tree
set(BENCH_SOURCES
    "benchmarks/benchmark_dynamic_pgm.cc"
    "benchmarks/benchmark_pgm.cc"
    "benchmarks/benchmark_lipp.cc"
    "benchmarks/benchmark_hybrid_pgm_lipp.cc"
    "benchmarks/benchmark_btree.cc")

file(GLOB_RECURSE SEARCH_SOURCES "searches/*.h" "searches/search.cpp")

add_executable(benchmark benchmark.cc ${SOURCE_FILES} ${BENCH_SOURCES} ${SEARCH_SOURCES})
add_executable(generate generate.cc ${SOURCE_FILES})

target_compile_definitions(benchmark PRIVATE NDEBUGGING)

target_include_directories(benchmark
        PRIVATE "competitors/PGM-index/include"  # For PGM
        PRIVATE "competitors/stx-btree-0.9/include") 

target_link_libraries(benchmark
        PRIVATE Threads::Threads dtl)
        
target_link_libraries(benchmark
        PRIVATE ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(benchmark
        PRIVATE dl)

target_include_directories(generate PRIVATE competitors/finedex/include)
EOF

echo "Task 1 minimal CMakeLists.txt is ready."
