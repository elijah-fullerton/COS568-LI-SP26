# Iteration 22 Literature Review

## Prior failure mode

The previous fast design improved mixed throughput by adding a published overlay
LIPP, a global Bloom filter, and per-bucket Bloom filters. That helped lookup
heavy workloads, but it violated the milestone 3 requirement in `README.md`:
no auxiliary data structures are allowed other than LIPP and DPGM. The next
iteration therefore has to recover compliance first, even if that means giving
 up some performance.

## Relevant research

The PGM paper argues that logarithmic buffering is attractive because inserts
can be staged cheaply and merged later into a structure optimized for search.
That observation still applies here: DPGM remains the natural write buffer,
especially when we need to preserve sorted-order semantics for later migration
into LIPP.

The LIPP paper emphasizes that lookup performance comes from precise placement:
once the model predicts a slot, there is no corrective search inside the node.
That suggests a compliant hybrid should preserve LIPP as the primary lookup
structure and avoid adding new lookup-time machinery that is not itself LIPP or
DPGM.

Learned-index and LSM-style systems papers repeatedly show the same systems
pattern: good mixed performance comes from separating write-optimized staging
from read-optimized serving, then making routing cheap. For this project, the
important lesson is not to add another serving structure, but to make the
LIPP-to-buffer routing as localized as possible so point lookups only check one
small mutable component after consulting LIPP.

Papers on learned indexes with local adaptation also support partitioned update
handling. When updates are spatially localized, rebuilding or flushing only the
affected learned region is usually better than perturbing the whole index.
Applied here, that motivates owner-routed DPGM buffers attached to frozen LIPP
regions rather than one global mutable buffer.

## Design implication

The compliant direction is therefore:

1. Keep LIPP as the only serving index for the bulk-loaded base keys.
2. Use only DPGM instances as mutable write buffers.
3. Route each key to one owner region inside the frozen LIPP topology, so
   lookups consult at most one owner-local DPGM after probing LIPP.
4. Flush a buffer by inserting its contents into LIPP, not by materializing any
   third structure such as a sorted vector, Bloom filter, or overlay index.

This is weaker than the previously non-compliant overlay design, but it is the
cleanest way to satisfy the README while preserving the core LIPP+DPGM idea.
