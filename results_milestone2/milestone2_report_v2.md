# Milestone 2 Report V2

## 1. Summary of What We Did for This Milestone

For Milestone 2, we implemented a naive hybrid of DynamicPGM and LIPP.

- Initial bulk-loaded keys are stored in LIPP.
- Incoming inserts are buffered in DynamicPGM.
- Each lookup checks DynamicPGM first and then falls back to LIPP.
- When the DynamicPGM buffer reaches a flush threshold, every staged key is extracted from DynamicPGM and inserted individually into LIPP, after which the DynamicPGM buffer is reset.

This implementation intentionally prioritizes correctness and simplicity over optimization. Before the timed experiments, the hybrid implementation was checked with `--verify` on both Facebook mixed workloads to ensure that no keys were lost during flushing.

All Milestone 2 experiments were run on the Facebook dataset only, as required. The raw underlying benchmark CSVs are:

- [90% insert mixed workload CSV](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix_results_table.csv)
- [10% insert mixed workload CSV](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/fb_100M_public_uint64_ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix_results_table.csv)
- [Best configuration summary CSV](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/milestone2_best_configs.csv)

## 2. Results

### 2.1 Mixed Workload Throughput: 90% Insert, 10% Lookup

| Method | Best config used | Run 1 (Mops/s) | Run 2 (Mops/s) | Run 3 (Mops/s) | Average (Mops/s) |
| --- | --- | ---: | ---: | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 128 | 2.751390 | 2.821680 | 2.829220 | 2.800763 |
| LIPP | n/a | 1.153880 | 2.335960 | 1.781320 | 1.757053 |
| HybridPGMLIPP | flush_bps 100 | 2.174080 | 1.748240 | 1.829840 | 1.917387 |

![Throughput 90% insert](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/throughput_mixed_90_insert.png)

### 2.2 Mixed Workload Index Size: 90% Insert, 10% Lookup

| Method | Best config used | Index size (bytes) |
| --- | --- | ---: |
| DynamicPGM | BinarySearch, epsilon 128 | 1702518268 |
| LIPP | n/a | 12656662928 |
| HybridPGMLIPP | flush_bps 100 | 12603367236 |

![Index size 90% insert](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/index_size_mixed_90_insert.png)

### 2.3 Mixed Workload Throughput: 10% Insert, 90% Lookup

| Method | Best config used | Run 1 (Mops/s) | Run 2 (Mops/s) | Run 3 (Mops/s) | Average (Mops/s) |
| --- | --- | ---: | ---: | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 64 | 0.810448 | 0.843494 | 0.805440 | 0.819794 |
| LIPP | n/a | 13.392000 | 7.593110 | 8.317890 | 9.767667 |
| HybridPGMLIPP | flush_bps 50 | 1.068710 | 1.099200 | 0.911793 | 1.026568 |

![Throughput 10% insert](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/throughput_mixed_10_insert.png)

### 2.4 Mixed Workload Index Size: 10% Insert, 90% Lookup

| Method | Best config used | Index size (bytes) |
| --- | --- | ---: |
| DynamicPGM | BinarySearch, epsilon 64 | 1705226028 |
| LIPP | n/a | 12700946864 |
| HybridPGMLIPP | flush_bps 50 | 12687700704 |

![Index size 10% insert](/auto/u/ef0952/projects/COS568-LI-SP26/results_milestone2/index_size_mixed_10_insert.png)

## 3. Discussion of Why the Differences Observed Likely Occurred

The throughput results line up with the expected strengths of the component indexes.

- In the 90% insert workload, DynamicPGM is best because most operations are inserts, and DynamicPGM is designed to absorb inserts efficiently. The hybrid is better than plain LIPP because it avoids paying the full LIPP insertion cost on every insert, but it still loses to DynamicPGM because its flushes eventually translate buffered inserts into many individual LIPP insertions.
- In the 10% insert workload, LIPP is best by a large margin because the workload is lookup-heavy and LIPP is optimized for direct model-guided lookup once the index is built. The hybrid performs better than DynamicPGM here, but it still trails LIPP because every lookup must first check DynamicPGM and may still need to check LIPP afterward, which adds overhead without enough insert pressure to justify the extra layer.
- The hybrid index size stays close to LIPP rather than DynamicPGM because most keys live in LIPP after bulk loading, and even after buffering some inserts in DynamicPGM the combined structure is still dominated by the LIPP footprint. DynamicPGM alone is much smaller because its representation is substantially more compact.
- The large run-to-run variation for LIPP, especially in the lookup-heavy workload, likely reflects sensitivity to runtime effects such as cache state and memory layout. The hybrid inherits some of that behavior, but its extra DynamicPGM probe and flush overhead keep it from matching LIPP’s best-case lookup performance.

Overall, the naive hybrid behaves as expected for a correctness-first Milestone 2 implementation: it does not beat the strongest baseline on either workload, but it demonstrates the required DPGM-plus-LIPP design and preserves all keys through flushing.
