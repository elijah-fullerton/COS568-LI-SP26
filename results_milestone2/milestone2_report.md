# Milestone 2 Report

Facebook mixed-workload results comparing DynamicPGM, LIPP, and the hybrid DPGM+LIPP index.

## Mixed Throughput (90% Insert, 10% Lookup)
- DynamicPGM: throughput=2.801 Mops/s, index_size=1702518268 bytes, variant=(BinarySearch, 128.0)
- LIPP: throughput=1.757 Mops/s, index_size=12656662928 bytes, variant=(n/a, n/a)
- HybridPGMLIPP: throughput=1.917 Mops/s, index_size=12603367236 bytes, variant=(flush_bps, 100.0)

## Mixed Throughput (10% Insert, 90% Lookup)
- DynamicPGM: throughput=0.820 Mops/s, index_size=1705226028 bytes, variant=(BinarySearch, 64.0)
- LIPP: throughput=9.768 Mops/s, index_size=12700946864 bytes, variant=(n/a, n/a)
- HybridPGMLIPP: throughput=1.027 Mops/s, index_size=12687700704 bytes, variant=(flush_bps, 50.0)
