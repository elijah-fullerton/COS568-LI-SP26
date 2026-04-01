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
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 32, 32>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 64, 32>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 128, 32>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 256, 32>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 128, 128>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 256, 128>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 64, 512, 128>>();
    benchmark.template Run<
        HybridPGMLIPP<uint64_t, ExponentialSearch<record>, 128, 512, 256>>();
  }
}

template void benchmark_64_hybrid_pgm_lipp<0>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
