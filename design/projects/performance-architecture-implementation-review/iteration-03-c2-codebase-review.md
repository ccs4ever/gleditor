# C2 codebase review: optional hint and no-op wrapper

Scope reviewed: `/tmp/perf-m3-c2-correction` as observed on 2026-11-16, against the current
repository M3 source. Static review only; no build, tests, or benchmark was run. I did not modify
the isolated draft or repository.

## Verdict

I found no correctness blocker in the reviewed hint lifecycle, map lookup, or non-Structure wrapper
behavior. The changed paths preserve the previous fold semantics while removing lookup work for
non-Structure operations that cannot touch a cell. The optional hint distinction is necessary:
unknown state keeps the conservative 4096-operation dense floor, while a known path uses its exact
count. This changes allocation policy for known short paths and should be stated in the
design/measurement notes after the C2 measurement gate.

This is a codebase review, not integration or runtime approval. Tests and full engine gates remain
necessary.

## Findings

- `Store::rebuildManifoldFromIndex()` supplies the ancestral path count before folding; the fallback
  `rebuildManifold()` path counts only extant operations in that path and supplies the same kind of
  exact hint. Sibling operations therefore do not inflate the known-path bound.
  `Manifold::advance()` keeps unknown as `nullopt`, increments a known count when possible, and
  restores the exact previous known/unknown state on `LookupCapacityUnavailable`. Semantic refusals
  still consume the attempted step, matching the previous policy-state behavior.
- `ManifoldOpIndexMap::find()` checks an extant dense entry and then always queries `overflow_`,
  independent of the current hint/bound. Changing a hint does not hide prior overflow entries;
  increasing the hint cannot duplicate an already-present overflow key because `prepareInsert()`
  checks `contains()` first. High operation indices continue to use the sparse map. Zero remains
  rejected before offset arithmetic.
- Copy/move construction and assignment carry the optional hint with the map. The setter invalidates
  prepared tokens by advancing `generation_` when the hint actually changes. The current equality
  intentionally compares mappings, not allocation-policy hints, which is consistent with its prior
  behavior.
- `applyStructure()` now fast-paths non-Structure operations when the map is empty or the operation
  context cannot be resolved. In both cases it advances `foldedThrough_` and returns success without
  changing `refusedOps_`, matching the former guarded path's behavior when `denseOf()` returned
  empty. For a resolvable context it passes the already-resolved dense index into the
  implementation, avoiding a second lookup.
- The wrapper preserves context-dependent refusals: a `PageBreak` on a cell still yields
  `WrongContextKind`, and a scratch span in a resolved cell context still yields `ScratchAddress`.
  As before, an operation with no resolvable cell context does not reach those checks. Structure
  operations still use the unchanged implementation path. `opIndex == 0` is refused before the
  wrapper fast paths; this increments refusal count without moving `foldedThrough_`, equivalent to
  the previous guard's `max(old, 0)` outcome.
- The added tests cover known/unknown distinctions, a short known path placing keys in overflow,
  preservation across hint change, high-index ancestry, unknown-hint preservation after failed
  advance/retry, empty-manifold text no-op behavior, and zero-index rejection. Existing
  lookup-failure tests continue to target preflight and state rollback.

## Follow-up before acceptance

The 4096 constant now applies only as the fallback for an unknown count; it is no longer a lower
bound for known hints. Update the nearby comment/design description (and any P2 report prose that
describes `max(hint, 4096)`) after the measurement decision. Keep the distinction explicit:
`known zero` is a zero dense bound, while `unknown` retains the 4096-op bound. The current new tests
encode this distinction, so they should remain with the change.

I found no other production call site or test assumption in the isolated tree that still required
the old `max(hint, 4096)` rule. No P2 threshold conclusion or performance claim is made here.
