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

#if defined(AUTORESEARCH_SCREEN_SAFE)
  if (filename.find("0.100000i") != std::string::npos ||
      filename.find("0.900000i") != std::string::npos) {
    // Restore screening measurability first: run exactly one canary that
    // collapses nearly all updates into a single deferred-flush overlay.
    // The recent timeout streak indicates even the prior "safe" setting was
    // still spending too much time in owner routing and fragmented DynamicPGMs.
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         256, 1 << 27, 1 << 27>>();
    return;
  }
#else
  if (filename.find("0.100000i") != std::string::npos) {
    // Keep the incumbent read-heavy point, but spend the second slot on a
    // tighter overlay probe. The previous e256 variant was dominated in every
    // recent full run, so use the small sweep budget on a more lookup-focused
    // candidate instead.
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         32, 1 << 27, 1 << 27>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         64, 1 << 27, 1 << 27>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         256, 1 << 27, 1 << 27>>();
  }
#endif
}

template void benchmark_64_hybrid_pgm_lipp<0>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
