#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/hybrid_pgm_lipp.h"

template <typename Searcher>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark,
                                  bool pareto, const std::vector<int>& params) {
  if (!pareto) {
    util::fail("HybridPGMLIPP hyperparameters are selected by workload presets");
  } else {
    benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 64, 100000>>(params);
    benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 128, 100000>>(params);
    benchmark.template Run<HybridPGMLipp<uint64_t, Searcher, 512, 100000>>(params);
  }
}

template <int record>
void benchmark_64_hybrid_pgm_lipp(tli::Benchmark<uint64_t>& benchmark, const std::string& filename) {
  if (filename.find("fb_100M") == std::string::npos || filename.find("mix") == std::string::npos) {
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64, 50000>>();
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 100000>>();
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 512, 200000>>();
  } else if (filename.find("0.100000i") != std::string::npos) {
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 64, 50000>>();
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 128, 100000>>();
    benchmark.template Run<HybridPGMLipp<uint64_t, BranchingBinarySearch<record>, 512, 200000>>();
  }
}

INSTANTIATE_TEMPLATES_MULTITHREAD(benchmark_64_hybrid_pgm_lipp, uint64_t);
