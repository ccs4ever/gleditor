# M3 C2 independent code-quality review

Snapshot reviewed: `e1c4f517ec68554d8833753b902f5d94ca3978e535e37a4c8e4c7f9449d65bd6`. All 14 paths
in `build/performance/variants/m3-hybrid-unordered-c2/source-backup.json` matched their recorded
SHA-256 hashes at review time. Read-only review; no build or runtime checks performed.

## Findings

### 1. Prepared insertion tokens expose invariants that commit does not validate

`apps/common/xanadu/zigzag/prepared_u32_unordered_map.hpp:47-52,174-193` publishes `PreparedInsert`
as a struct with public `owner`, `generation`, `key`, and PMR `node` fields. `commitInsert()` checks
owner/generation, nonzero key, and nonempty node, but does not check `token.node.key() == token.key`
or that the node allocator is the map's resource.

A caller can change `token.key` after preparation and successfully insert the node under its
original key, despite having prepared a different key. More seriously, a caller can replace
`token.node` with a node extracted from another PMR map. The subsequent node insertion has unequal
allocators, violating the standard node-handle insertion precondition (undefined behavior). This is
a public capability returned by the API, not merely an internal local variable. Keep these fields
private and construct tokens only in the map, or validate key and allocator identity before commit.
A focused test should exercise token tampering if fields remain public.

`ManifoldOpIndexMap::PreparedInsert` in `manifold_lookup.hpp:26-33` has the same exposure. The dense
commit path at lines 148-162 checks owner, generation, and requested operation, but trusts public
`dense` and `offset`. Changing `offset` lets a token prepared for op N publish the value at another
dense slot while incrementing logical size for N. Making the representation private is the simplest
way to preserve the capability invariant. The overflow token itself is private in `FlatU32Map`, so
that lower-level map does not share this specific issue.

### 2. Path walker accepts a non-ancestor endpoint when target is zero

`apps/common/xanadu/osmic_walker.hpp:94-97` returns success for every `toDescendant == 0`, even when
`fromAncestor` is nonzero. For example, `walkPathCustomWithCount(2, 0, ...)` calls `beforeVisit(0)`
and returns true, although the documented contract says false when the first endpoint is not an
ancestor of the second. The zero-path special case should be limited to equal endpoints (the
existing `(0, 0)` behavior remains valid).

`tests/xuzz/manifold_lookup_test.cpp:16-35,72-89` covers equal endpoints and invalid nonzero
ancestry, but not this boundary case. Add `(nonzero, 0)` coverage.

## Checks with no concrete issue found

- The helper types live under `apps/common/xanadu/zigzag` and are namespaced `zigzag::detail`; this
  is an appropriate engine-private layer. `PreparedU32UnorderedMap` serves nontrivial
  `StructureBirthInfo` with PMR node handles, while `FlatU32Map` constrains values to trivially
  copyable types. They have different allocation and value-lifetime contracts, so the separate
  implementations are justified. `geometricCapacityTarget` is only used by its direct unit test, but
  this small public constexpr helper is not itself a correctness blocker.
- `ManifoldOpIndexMap` keeps unknown hints as `std::nullopt`, distinguishes known zero, and rejects
  op index zero at its op-index boundary. `OsmicWalker::walkAncestralWithCount` supplies the counted
  path length before folding; both indexed and fallback Store rebuilds set the count hint. I found
  no sentinel or known/unknown conflation in this flow.
- `FlatU32Map` keeps occupancy separate from key/value, so all `uint32_t` keys remain representable.
  Its prepared token fields are private and commit checks owner/generation. Copy/move invalidation
  and resource selection are explicitly handled. The PMR resources themselves remain non-owning, as
  usual for PMR containers; callers must keep them alive for the map and any extracted node token.
  That lifetime precondition should be documented alongside the token API if external callers are
  expected.
- The new tests are in `tests/xuzz`, matching the engine-private code location. The map-level tests
  cover the container contracts, while `manifold_test.cpp` and `manifold_lookup_test.cpp` cover
  fold/integration behavior; I did not find test duplication that should block this change.
- The touched `Manifold` setters inspected follow the repo's chainable setter convention. I found no
  concrete copy/move or retained-node lifecycle defect beyond the exposed-token invariants above.
