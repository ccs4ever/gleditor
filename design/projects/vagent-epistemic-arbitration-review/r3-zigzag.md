# Round 3 critique: Xanadu / ZigZag Purist (epistemic arbitration)

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Paths below are relative to the repository root (`/data/git/gleditor`). The v1.2 file is dirty in
the supplied worktree and was reviewed in place; no repository files were edited. Tally: 0 BLOCKING,
0 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2 zigzag: CONVERGED (0 BLOCKING, 0 MAJOR). All R1 topology and trail-safety blockers were
  resolved. Its three topology/pricing minors below remain unchanged; the beta comment was corrected
  in v1.2.
- R2 writer: no report (quota), per `r2-disposition.md`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### 1. (§6.1, lines 867–873) One token source view cannot represent multiple corroborating sources

**Problem:** `kTokenSources` is specified as a single clone-view cell, yet the text says it links to
the asserting source profile masters (plural). A clone-view cell has one neighbor on `d.clone`; a
second master cannot share that endpoint without evicting the first under the strict two-degree
invariant. The multiple-token `+d.step` chain covers distinct belief tokens, not the several sources
supporting one belief.

**Evidence:** §6.1 lines 867–873; `ArenaManifold::link()` maintains reciprocal two-ended links and
evicts the prior occupant on either end (`xanadu/zigzag/arena_manifold.cpp:1344–1377`).

**Fix:** Specify a source-view rank per token: token → first source view on one dimension, then
chain subsequent source-view cells on `+d.step`; each view cell links by `+d.clone` to exactly one
master.

### 2. (§8.5, line 1114) Token storage price is still inconsistent with its fields

**Problem:** One token and three field cells require 256 bytes at 64 bytes each, before source-view
cells for a multi-source belief. The stated ~128 bytes only covers two cells.

**Evidence:** §6.1 lines 867–870, §8.5 line 1114, and `xanadu/compact_op.hpp:78`.

**Fix:** Update the fixed price and add per-source view-cell and edge costs using the explicit rank
topology.

### 3. Weight vector's first edge remains ambiguous against the list ABI

**Full title and location:** 3. (§2.1, lines 297–300; §2.3, line 341) Weight vector's first edge
remains ambiguous against the list ABI

**Problem:** The `$weights` head points to its first scalar by `+d.step`, while later scalars also
chain by `+d.step`. If this is a ZigZag list, the ABI assigns the first element to `+d.grab`; if it
is a custom vector rank, label it as such and avoid presenting it as a list. The R2 zigzag minor
remains unfixed.

**Evidence:** The diagram uses `F_Weights -->|+d.step| W1` and follows with another `+d.step`
(`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md:297–300`). The runtime ABI's
list convention is described in the P18 design prerequisite; that P18 document exists historically
at `f033eda8:design/vagent-extended-capabilities-and-belief-vlog-unification.md` but not in the
current tree. Current `ArenaManifold::link()` source confirms a cell has one successor per directed
dimension (`xanadu/zigzag/arena_manifold.cpp:1359–1377`).

**Fix:** Either use the established list shape (head to first element on `+d.grab`, elements chained
on `+d.step`) or explicitly designate a custom rank and define its traversal/termination convention.

VERDICT: NOT CONVERGED
