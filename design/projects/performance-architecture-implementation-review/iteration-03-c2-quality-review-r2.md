# M3 C2 r2 focused re-review

Reviewed the requested r2 source changes for the two findings from the first audit. No builds or
source edits were made.

## Converged: prepared-token representation

Both `PreparedU32UnorderedMap<Value>::PreparedInsert` and `ManifoldOpIndexMap::PreparedInsert` are
now non-aggregate, move-only classes with private construction and private
owner/generation/key-or-op/payload fields. Only the owning map class can construct them. The
unordered-map commit also checks that the node key matches its private token key. This closes the
earlier public-field tampering paths, including the cross-resource node injection that could violate
the node-handle allocator precondition.

Tests assert non-aggregate and non-copyable/move-constructible traits for both token types.
`PreparedU32UnorderedMapTest.PreparedTokensAreOpaqueAndOwnerBound` and
`ManifoldOpIndexMapTest.PreparedTokensAreOpaqueAndOwnerBound` verify a token prepared by one map is
rejected by another while remaining usable by its owner. I found no remaining instance of the two
reported token-invariant defects.

## Converged: zero-target path boundary

`OsmicWalker::walkPathCustomWithCount` now takes the empty-path fast path only when endpoints are
equal. For `(fromAncestor, toDescendant) == (2, 0)`, traversal does not call the count callback or
visitor and returns false after the ancestry check.
`OsmicWalkerCountTest.NonzeroToZeroIsInvalidWithoutCallbacks` exercises exactly this case. This
closes the reported boundary bug.

The unused `geometricCapacityTarget` helper is absent from the reviewed source and test. No further
findings in this narrowed scope.
