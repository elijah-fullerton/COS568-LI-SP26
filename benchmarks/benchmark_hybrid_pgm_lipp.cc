#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/hybrid_pgm_lipp.h"

template <int record>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                  const std::string& filename) {
  if (filename.find("fb_100M") == std::string::npos ||
      filename.find("mix") == std::string::npos) {
    return;
  }

  benchmark.template Run<
      HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 512, 10>>();
  benchmark.template Run<
      HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 512, 50>>();
  benchmark.template Run<
      HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>, 512, 100>>();
}

template void benchmark_64_hybrid_pgm_lipp<0>(tli::Benchmark<uint64_t>& benchmark,
                                              const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(tli::Benchmark<uint64_t>& benchmark,
                                              const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(tli::Benchmark<uint64_t>& benchmark,
                                              const std::string& filename);
