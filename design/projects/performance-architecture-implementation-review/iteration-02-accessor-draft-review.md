# M2 P4 accessor draft review

**Review target:** `/tmp/perf-m2-p4-draft.patch` against §6.2 of
`design/performance-architecture-implementation-plan.md`.

**Code review disposition: ready for runtime/performance gates.** This means the draft is suitable
to proceed to the prescribed focused tests, full compilation, and release comparisons. It is not
evidence that those gates passed or that inlining improves performance.

## Findings

- **Scope matches §6.2.** The patch inlines both `SegmentedOpsSpool::get(std::uint32_t)` overloads,
  `Manifold::slot`, and `ArenaManifold::slot`. `get(const MicroversionId&)` remains out of line and
  still delegates to the index overload.
- **Behavior and bounds are preserved.** The spool definitions are textually equivalent to the
  removed definitions: index zero, indices above `opCount`, and a null arena base return null; valid
  indices address the same 1-based node array. The spool's sentinel/index-zero layout is consistent
  with `append()` writing `opsArray[newIndex]`.
- **Manifold lookup behavior is preserved.** `Manifold::slot` calls `denseOf`, returns an empty
  `SlotRef` on absence, and uses `slots[*dense]` on a present mapping. This preserves valid dense
  index zero and the existing rejection of `noCell` and ephemeral refs in `denseOf`.
- **Arena overlay behavior is preserved.** `ArenaManifold::slot` returns an arena-owned/shadow slot
  when `denseOf` succeeds; otherwise it forwards to `base_->slot(ref)` when a base exists, and
  returns empty without a base. This matches the removed out-of-line body.
- **ODR and header completeness look sound.** These are ordinary in-class definitions, therefore
  implicitly inline and ODR-safe. The spool header already includes the complete `CompactOpNode` and
  `VirtualMemoryArena` definitions needed by the bodies. The manifold header contains the complete
  `CellSlot`, `SlotRef`, and `Manifold` declarations; the arena header includes `manifold.hpp` and
  owns `std::vector<CellSlot>`. No new bencode/hash implementation dependency is introduced.
- **API/ABI note:** moving these member bodies into headers removes the prior out-of-line
  definitions from the library object. A full rebuild of consumers is required; mixing old
  binaries/objects against the changed library could expose symbol compatibility issues. That is
  normal for this in-tree source change, but should be included in release artifact identity checks.
  No class layout, persisted format, or operation layout changes are present.

## Performance interpretation

The source change makes the short accessors visible to callers across translation units even without
LTO. Under ThinLTO, the original out-of-line functions may already have been imported/inlined, so
this can be redundant or change only import/inlining decisions and code size. The draft itself
establishes no speedup. Follow §6.2 by comparing identical-source variants that isolate header
inlining from LTO/PGO, inspecting actual call-site inlining and text size, and reporting workload
variation/regressions. Keep the existing out-of-line alternative if measurement favors it.

## Remaining gates

Focused arena/spool/manifold tests, full compilation, release comparisons, and inspection of actual
inlining/text size have not been run as part of this review. No runtime or performance pass is
claimed.
