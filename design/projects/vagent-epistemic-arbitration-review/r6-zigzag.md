# Round 6 critique: Xanadu / ZigZag Purist

Source shorthand `xanadu/` means `apps/common/xanadu/`.

Reviewed the full frozen approved input, identical to the current design target, against current
`ArenaManifold` rank behavior. HEAD is `5ee5f6ffb7ee2877cfdf1473d0c36602bafdab53`. Approved P19
proposals are reviewed as designs, not reported as absent implementation. No repository files were
edited.

Tally: **0 BLOCKING, 2 MAJOR, 1 MINOR.**

## Status of previous BLOCKING/MAJOR findings (Rounds 3–5)

- Multi-source clone-view topology, list head/element links, truthful unlink/compact lifecycle, and
  ephemeral scoped-result handling — **RESOLVED** in R3/R4. This is a full fresh topology pass.

## BLOCKING

None.

## MAJOR

### 1. (§2.1, §3.1) Dependence groups have no ZigZag record or topology

**Problem:** A candidate score is grouped by `G`; `rho_G` and audited independence affect its value.
But the representation defines no dependence-group record, membership edge, provenance/version
field, or `rho` value cell. An observation's source identity cannot stand in for an error-dependence
group: §5.3 explicitly says error independence is separate from source authentication. With two
sources whose errors share a map/model, and a third independently audited source, the graph has no
specified path to distinguish those relationships. Implementers would need hidden C++/external
lookup state or invent a topology, contrary to the cell-grounding contract. The same schema
ambiguity affects per-predicate/domain Beta and SGD state.

**Evidence:** `ArenaManifold::link()` maintains one reciprocal occupant per dimension/direction and
evicts an endpoint when another link replaces it (`xanadu/zigzag/arena_manifold.cpp:1344–1377`), so
membership requires explicit rank/clone-view cells. The proposal's complete record list is §2.1
lines 76–81; group math/independence declarations are §3.1 lines 146–159 and §5.3 lines 458–464,
with no group record or link contract.

**Fix:** Define policy-owned dependence-group records on `d.epistemic`, their stable
ID/provenance/version and `rho_G`, and one owned membership occurrence per source/domain epoch
linked by authentic clone rank. Unassigned identities must remain in the shared `rho=1` group.
Specify field/list topology with `d.grab`/`d.step`, validate `rho_G ∈ [0,1]`, and include the
cells/clone walks in P19.1/P19.2 cost.

### 2. (§5.3) Oversized connected-component topology is ambiguous

**Problem:** The text builds single-linkage connected components, then says a component above
maximum diameter remains “separate clause alternatives.” It does not define which cells become those
alternatives or whether it cuts the component into arbitrary sub-ranks. A partition that links
selected clauses together would affect both the topological alternatives and their score, with no
deterministic ZigZag structure rule.

**Evidence:** §5.3 lines 441–445. Distinct components are defined as disjunctive hypotheses, but the
over-diameter case gives no cell-level decomposition. `link()` enforces a two-ended edge, so
many-to-one membership cannot be implied from source clauses (`arena_manifold.cpp:1344–1377`).

**Fix:** For an over-diameter component, keep each original clause as its own alternative rather
than minting an inferred partition; define stable score/order behavior and test a connected chain.
If a deterministic partition is intended, give its explicit algorithm and node/rank topology.

## MINOR

### 1. (§2.1, lines 68–81; §6.3, lines 567–573) Superseded verification link is unspecified

A versioned verification record can reverse an old verdict, but the record schema has no
`supersedes` link or active-version edge. Even after choosing model recomputation, define the typed
link from the replacement verification to the superseded record so ZigZag provenance remains
inspectable. This supports the codebase/architect MAJOR above.

VERDICT: **NOT CONVERGED**

VERDICT: NOT CONVERGED
