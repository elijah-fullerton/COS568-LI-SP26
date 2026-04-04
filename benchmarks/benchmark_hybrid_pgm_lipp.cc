#include "benchmarks/benchmark_hybrid_pgm_lipp.h"

#include "benchmark.h"
#include "benchmarks/common.h"
#include "competitors/hybrid_pgm_lipp.h"
#include "competitors/hybrid_pgm_lipp_classic.h"

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
  if (filename.find("0.100000i") != std::string::npos) {
    // Point the read-heavy screen canary at the more aggressive bloom-style
    // lookup-heavy lane so it better predicts whether early overlay draining
    // and miss filtering will close the remaining gap to LIPP.
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         16, 1 << 20, 1 << 12>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    // Preserve the insert-heavy canary that keeps almost all updates in one
    // deferred overlay and has remained the strongest full-run point.
    benchmark.template Run<
        HybridPGMLIPPClassic<uint64_t, BranchingBinarySearch<record>, 256,
                             1 << 27, 1 << 27>>();
    return;
  }
#else
  if (filename.find("0.100000i") != std::string::npos) {
    // Keep the current large-overlay lookup-heavy winners with the bloom-based
    // miss filter and owner telemetry. The smaller-owner aggressive-flush
    // points were pathological under verify and are excluded from the stable
    // full-run sweep.
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         16, 1 << 27, 1 << 27>>();
    benchmark.template Run<HybridPGMLIPP<uint64_t, BranchingBinarySearch<record>,
                                         32, 1 << 27, 1 << 27>>();
    return;
  }

  if (filename.find("0.900000i") != std::string::npos) {
    benchmark.template Run<
        HybridPGMLIPPClassic<uint64_t, BranchingBinarySearch<record>, 256,
                             1 << 27, 1 << 27>>();
  }
#endif
}

template void benchmark_64_hybrid_pgm_lipp<0>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<1>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
template void benchmark_64_hybrid_pgm_lipp<2>(
    tli::Benchmark<uint64_t>& benchmark, const std::string& filename);
