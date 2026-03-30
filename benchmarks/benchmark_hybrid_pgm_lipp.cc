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
        HybridPGMLIPP<uint64_t, ExponentialSearch<record>, 64, 25, 16384, 64>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 50, 32768,
                      32>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, InterpolationSearch<record>, 128, 25, 32768,
                      64>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 50, 131072,
                      16>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 128, 100, 262144,
                      8>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, ExponentialSearch<record>, 128, 50, 131072,
                      16>>();
  }
}

template void benchmark_64_hybrid_pgm_lipp<0>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
