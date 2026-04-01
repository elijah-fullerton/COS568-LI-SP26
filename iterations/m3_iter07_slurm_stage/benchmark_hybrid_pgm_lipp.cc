#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/hybrid_pgm_lipp.h"

template <int record>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                  const std::string& filename) {
  const bool supported_dataset =
      filename.find("books_100M") != std::string::npos ||
      filename.find("fb_100M") != std::string::npos ||
      filename.find("osmc_100M") != std::string::npos;
  if (!supported_dataset || filename.find("mix") == std::string::npos) {
    return;
  }

  if (filename.find("0.100000i") != std::string::npos) {
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 4096, 200>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 8192, 300>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, ExponentialSearch<record>, 256, 16384, 400>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 4096, 100>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 256, 8192, 200>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, ExponentialSearch<record>, 256, 16384, 300>>();
  }
}

template void benchmark_64_hybrid_pgm_lipp<0>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
