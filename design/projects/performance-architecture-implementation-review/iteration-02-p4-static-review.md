# Static review: actual M2 P4 working-tree diff

Reviewed the current six-file diff against HEAD, the accepted plan §6.2, and
`/tmp/perf-m2-p4-draft-review.md`. No source edits, builds, tests, or timing were performed.

## Verdict

**The actual diff matches the intended accessor-only P4 scope and is ready for the queued
build/measurement gate.** It moves exactly the two `SegmentedOpsSpool::get(uint32_t)` overload
bodies, `Manifold::slot`, and `ArenaManifold::slot` into their class headers.
`SegmentedOpsSpool::get(const MicroversionId&)` remains out of line. No other source, test, layout,
or behavioral change appears in the diff.

## Source and type review

- `SegmentedOpsSpool::get(uint32_t) const` and the mutable overload preserve the prior conditions
  and indexing exactly: return null for index zero, index above `opCount`, or missing arena base;
  otherwise return `arena.base()[index]`. Signatures and `[[nodiscard]]` are retained. The ID
  overload remains in `segmented_ops_spool.cpp` and delegates to the index overload; in its const
  method, normal overload resolution selects the const overload.
- `Manifold::slot` retains the `denseOf(ref)` optional check and returns the same `SlotRef` for a
  valid dense index, including dense index zero. It does not introduce a sentinel or alter
  ephemeral/unknown-reference behavior.
- `ArenaManifold::slot` retains local dense-slot precedence, base-manifold fallback for nonlocal
  refs, and empty optional when no base exists. Its body matches the removed implementation.
- Header dependencies are already sufficient: the spool header includes complete `CompactOpNode` and
  `VirtualMemoryArena`; `manifold.hpp` defines `CellSlot` and `SlotRef` before the `Manifold` body;
  `arena_manifold.hpp` includes `manifold.hpp`, so both `CellSlot` and `Manifold::slot` are complete
  at the inline body. The optional-reference type comes from the existing `<gleditor/cpp26.hpp>`
  selection; this change introduces no facility selection or new dependency.
- In-class definitions are inline and ODR-safe. The definitions are removed from the corresponding
  `.cpp` files, so the old out-of-line symbols are no longer emitted by this source. That is a
  binary compatibility change for stale external objects linking these non-exported/common engine
  methods; the project’s full rebuild/artifact identity gate is the appropriate treatment. There is
  no class layout, persisted format, or `CompactOpNode` change.

## Diff shape and remaining gate

The working-tree diff contains only these six files: `segmented_ops_spool.hpp/.cpp`,
`manifold.hpp/.cpp`, and `arena_manifold.hpp/.cpp`. The three removed bodies match the new inline
bodies; no test file was changed. Existing tests should exercise the unchanged semantics, but this
review does not claim they passed.

The performance decision remains open until the paired P4 measurements complete. Under ThinLTO the
old out-of-line functions may already be imported/inlined, so record actual call-site inlining and
text-size changes, keep the P3 code fixed across both P4 variants, and compare fresh-source-matched
ThinLTO and ThinLTO+PGO profiles as planned. Retain the out-of-line alternative if results do not
justify the header change.
