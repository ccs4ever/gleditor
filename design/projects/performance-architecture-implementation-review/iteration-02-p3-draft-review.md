# Iteration 2 P3 draft review

## Verdict: ready for runtime and performance gates

Reviewed `/tmp/perf-m2-p3-draft.patch` and its copied source against the live `ArenaManifold`
implementation and the accepted P3 requirements in
`design/performance-architecture-implementation-plan.md` §6.1 and
`design/performance-architecture-proposals.md` §2.3. I found no semantic defect in this draft.

### Semantic review

- `linked()` keeps the missing-dense/base fallback unchanged. For arena slots, it loads `valueKind`
  once. Non-`ExternRef` slots reach the shared local scan without an occurrence-map lookup; only
  ephemeral `ExternRef` slots can enter proxy delegation. This matches the intended optimization
  boundary.
- `isQuoteOccurrence(from)` remains ahead of foreign resolution within that branch. An
  `ExternRef`-valued quote occurrence therefore scans only its own local links and cannot inherit
  the canonical proxy's foreign links. This is necessary because `setValueBits()` can change an
  occurrence's kind and `foreignOf()` canonicalizes occurrences.
- The `isEphemeral(from)` guard preserves the persistent-shadow case. A persistent cell shadowed in
  this arena may have `ValueKind::ExternRef`, but is not an arena proxy; it continues through its
  local overlay links rather than being interpreted as a foreign reference. `denseOf()` already
  distinguishes these persistent overlay refs from ephemeral slot refs.
- The regression test establishes a canonical proxy with a foreign positive edge, then checks an
  occurrence with no local edge in both directions. It gives the occurrence a different local
  positive edge and checks that value changes to `Int64` and back to `ExternRef` preserve local-only
  lookup. The mark/release assertions verify both sides are empty again after rollback. Finally it
  shadows a persistent document cell with `ExternRef` and checks that its original local positive
  edge remains visible.

The normal non-`ExternRef` local path is the same shared scan retained for all slots after the
branch, and existing arena tests should continue to cover its ordinary link behavior. No
flags/layout change or foreign-resolution behavior change is introduced.

### Gates still required

This review is source-level only; the isolated tests were not executed here. Before accepting P3,
run the focused `ArenaManifoldTest` suite and the full compilation gate. Then perform the plan's
release traversal comparison on identical source variants, record the qualifying-call distribution,
and separate P3 effects from P4 inlining and ThinLTO/PGO. Do not report a speedup until those
measurements exist. No performance result is inferred from the patch or from syntax checking.
