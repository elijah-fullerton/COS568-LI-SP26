# Iteration 23 Literature Review

## Why iteration 22 failed

Iteration 22 recovered compliance, but it paired that compliant structure with
an overly fine-grained partitioning of the base LIPP. The result was not just
lower throughput; the smallest owner-buffer configuration was so slow that the
screen run timed out.

## Research guidance

Work on learned and hierarchical indexes consistently shows that overpartitioning
can destroy performance when each query pays routing overhead before reaching a
small local structure. In model-based indexes, partition granularity is useful
only until local metadata and dispatch overhead dominate the savings from
smaller search regions.

The PGM literature also reinforces that buffered levels work best when merges
are sufficiently amortized. If buffers are too small, merge work becomes
effectively synchronous and destroys throughput. The same principle applies to
our owner-local DPGM buffers: aggressive flushing turns the hybrid into a stream
of tiny merges into LIPP.

From the systems side, LSM-tree and component-index papers suggest that one
should first remove pathological fan-out choices before changing the underlying
architecture. That is the right move here: the current compliant architecture
should be re-screened with coarser regions before we conclude that compliance
itself makes milestone 3 impossible.

## Design implication

Iteration 23 should keep the compliant LIPP+DPGM-only architecture but move to
coarser owner regions and less frequent flushing. That directly targets the
amortization problem exposed by iteration 22 without reintroducing forbidden
auxiliary structures.
