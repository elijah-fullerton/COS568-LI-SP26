# Milestone 3 Report

## Final Summary

The final split hybrid uses a bloom-filter-guided lookup-heavy lane and a classic owner-buffered insert-heavy lane.

## Final Best Configurations

| Dataset | Workload | Best config | Avg throughput (Mops/s) | Index size (bytes) | Beats LIPP? |
| --- | --- | --- | ---: | ---: | --- |
| books | Mixed 10% Insert / 90% Lookup | BinarySearch-e32-s2147483648-f134217728-bf | 3.881 | 12,164,578,912 | True |
| books | Mixed 90% Insert / 10% Lookup | BinarySearch-e256-s2147483648-f134217728 | 3.068 | 12,028,076,860 | True |
| fb | Mixed 10% Insert / 90% Lookup | BinarySearch-e32-s2147483648-f134217728-bf | 4.449 | 12,976,751,712 | True |
| fb | Mixed 90% Insert / 10% Lookup | BinarySearch-e256-s2147483648-f134217728 | 2.994 | 12,810,513,316 | True |
| osmc | Mixed 10% Insert / 90% Lookup | BinarySearch-e32-s2147483648-f134217728-bf | 3.484 | 21,086,949,760 | True |
| osmc | Mixed 90% Insert / 10% Lookup | BinarySearch-e256-s2147483648-f134217728 | 2.419 | 20,761,659,652 | True |

## Notes

- Lookup-heavy runs use the bloom-enabled `HybridPGMLIPP` implementation.
- Insert-heavy runs use `HybridPGMLIPPClassic` to preserve the stronger insert path.
- All final data points are generated with `--verify -r 3` in one benchmark session per workload.