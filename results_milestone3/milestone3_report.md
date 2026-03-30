# Milestone 3 Report

## Setup

- Workloads: mixed 90% insert / 10% lookup and mixed 10% insert / 90% lookup.
- Datasets: Books, Facebook, and OSMC 100M uint64 datasets.
- Repeats: every data point is the average of 3 runs from the same scratch session.
- Indexes compared: DynamicPGM, LIPP, and the Milestone 3 `HybridPGMLIPP`.

## Hybrid strategy implemented

The milestone 3 hybrid keeps new inserts in an active DynamicPGM buffer, freezes that buffer once it crosses an adaptive threshold, and migrates the frozen keys into LIPP incrementally. Lookups first probe the active and frozen DPGM buffers with min/max range gating, then fall back to LIPP. The main goal of this design is to avoid the synchronous stop-the-world flush from Milestone 2 and to spread migration work across foreground operations.

## Main findings

1. The advanced hybrid improved over DynamicPGM on every dataset and workload in this run, but it did not surpass LIPP on any of the six mixed-workload settings.
2. The best hybrid configuration was workload-sensitive. The 90% insert workload consistently preferred a larger frozen-buffer cap (`m131072`) and lower cooperative flush budget, while the 10% insert workload preferred a smaller cap (`m16384` or `m32768`) and a more aggressive flush budget.
3. LIPP remained the throughput leader in all six settings, despite its much larger memory footprint. DynamicPGM remained the most compact structure by a large margin.

## Best configurations and results

### Books

#### Mixed 90% insert / 10% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | InterpolationSearch, epsilon 256 | 0.406 | 1,700,011,168 |
| LIPP | n/a | 1.179 | 11,753,611,888 |
| HybridPGMLIPP | BinarySearch-e64-t50-m131072-b16 | 0.428 | 11,750,042,232 |

The hybrid beat DynamicPGM by about 5.4%, but it remained far below LIPP on this dataset. This suggests that even with cooperative flushing, the cost of checking the DPGM delta still dominates compared with pure LIPP traversal on Books.

#### Mixed 10% insert / 90% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | InterpolationSearch, epsilon 128 | 0.158 | 1,700,049,908 |
| LIPP | n/a | 3.069 | 11,818,525,760 |
| HybridPGMLIPP | BinarySearch-e128-t50-m32768-b32 | 0.414 | 11,818,401,600 |

This was the strongest relative gain for the hybrid over DynamicPGM on Books, roughly 2.63x, but LIPP still dominated the lookup-heavy setting.

### Facebook

#### Mixed 90% insert / 10% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 64 | 0.399 | 1,705,154,448 |
| LIPP | n/a | 1.564 | 12,656,662,928 |
| HybridPGMLIPP | ExponentialSearch-e128-t50-m131072-b16 | 0.454 | 12,650,407,672 |

The hybrid improved on DynamicPGM by about 13.9% on the insert-heavy Facebook workload. However, it remained well below both LIPP and the stronger Milestone 2 Facebook naive-flush result already stored in the repo.

#### Mixed 10% insert / 90% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 64 | 0.156 | 1,705,226,028 |
| LIPP | n/a | 2.796 | 12,700,946,864 |
| HybridPGMLIPP | ExponentialSearch-e64-t25-m16384-b64 | 0.445 | 12,700,725,744 |

The smaller frozen-buffer cap helped the hybrid the most in the lookup-heavy workload, but the delta probe cost still kept it far from LIPP.

### OSMC

#### Mixed 90% insert / 10% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 256 | 0.360 | 1,700,923,408 |
| LIPP | n/a | 1.091 | 20,408,967,104 |
| HybridPGMLIPP | BinarySearch-e64-t50-m131072-b16 | 0.402 | 20,402,463,448 |

The hybrid beat DynamicPGM by about 11.7% here, but OSMC still strongly favored LIPP for mixed throughput.

#### Mixed 10% insert / 90% lookup

| Index | Best variant | Avg throughput (Mops/s) | Index size (bytes) |
| --- | --- | ---: | ---: |
| DynamicPGM | BinarySearch, epsilon 256 | 0.149 | 1,700,920,428 |
| LIPP | n/a | 2.204 | 20,602,887,136 |
| HybridPGMLIPP | ExponentialSearch-e64-t25-m16384-b64 | 0.449 | 20,602,652,576 |

Again, the more aggressive lookup-oriented hybrid configuration was best, and again LIPP remained the leader.

## Conclusion

The milestone 3 implementation successfully replaced the Milestone 2 synchronous flush with a double-buffered, cooperative-flush design and preserved correctness across all six scratch benchmark runs. In this evaluation, that change was enough to consistently outperform DynamicPGM, but not enough to beat LIPP or the earlier Facebook-only naive milestone 2 peak. The main bottleneck appears to be the foreground cost of carrying and probing the delta buffers: the hybrid reduces flush disruption, but it still pays a noticeable read-path tax compared with pure LIPP.
