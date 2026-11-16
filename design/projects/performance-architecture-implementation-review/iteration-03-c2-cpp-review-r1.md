# C2 isolated implementation: static review

## Findings

No concrete correctness blocker found in the isolated `/tmp/perf-m3-c2-correction/apps`
implementation.

- `applyStructure()` keeps the zero-index refusal first. The non-Structure fast path updates
  `foldedThrough_` exactly as the former guard did for ignored operations. It skips the heavy helper
  only when the byRef map is empty or the context does not resolve to a stored cell. Cell-context
  text still enters the original validation/preflight path, including page-break and scratch-scroll
  refusals. Structure operations always enter the helper. This relies on the existing invariant that
  a live cell always has its birth in byRef; the MakeCell commit sequence preserves that invariant
  (preflight before mutation, then terminating commit).
- The `try_emplace`/immediate `extract(iterator)` birth preflight is sound for the private trivial
  `StructureBirthInfo` value: after a successful temporary insertion it returns with the key absent
  and a same-resource node handle. Bucket capacity established for the prospective final size is
  retained; reinsertion at the unchanged generation restores the same size and needs no further
  bucket growth. Allocation/length failures leave the logical map unchanged. The existing generation
  token rejects commits after intervening successful map mutations. Copy/move construction and
  assignment preserve the optional hint and invalidate map-owned preparations through
  owner/generation identity.
- The optional hint preserves the old default dense cutoff when unknown and allows Store cold folds
  to set an exact branch-path count. `advance()` increments only a known hint; lookup-resource
  refusal restores the prior optional state, while semantic refusals retain the increment matching
  the advanced path position.

## Focused coverage to retain in the completed patch

Add or retain a regression asserting that a no-cell ignored text operation advances `foldedThrough_`
without requesting a PMR allocation, alongside zero-index and cell-context refusal coverage. The new
helper tests should continue checking absent-after-prepare, all allocation-failure ordinals
(including bucket growth), allocation-free commit across automatic growth, and stale-token
rejection. I inspected the current isolated sources statically and did not run these tests.

## Scope

This is not a runtime, full-test, or performance acceptance. The C2 timings and final frozen-source
review remain separate gates.
