# M4 OperandList pre-integration review

## Finding

One concrete integration blocker remains in the isolated patch:
`tests/xuzz/vql_conformance_test.cpp` still compares `core.inputsOf(...)` directly with
`std::vector<CellRef>`. The new `OperandList` has no vector conversion and only same-type equality,
so the `EXPECT_EQ` at the existing HomeAnchor assertion will not compile. Compare spans/ranges or
individual elements in that assertion. Repository-wide search found several remaining vector
declarations at migrated consumers in the current checkout, but the patch converts the Vortex VM and
VQL/VPL compiler assignments; the conformance assertion is the direct missed test boundary found
here.

## Static API assessment

No other concrete `OperandList` correctness defect found. It preserves contiguous mutable access
needed by `VortexVM::{inputsOf,outputsOf}` to apply `runtimeSlot`, provides an explicit span for the
read-only `resolveCell` boundary, keeps the spill unbounded, preserves order, and leaves moved-from
lists empty and reusable. Spill allocation failure is staged in a temporary vector before switching
representation; spilled copy assignment likewise copies before changing the destination. The empty,
always-equal allocator constraint supports the vector swap/move assumptions used by the
implementation. The 0/8/9/1024 tests cover basic boundaries and failure recovery.

The exact `sizeof(OperandList)==64` assertion is a deliberate ABI target but `inplace_vector` layout
is implementation-defined. The isolated g++ native and fallback checks support this host; before
integration, confirm the size and move traits under the release Clang/library selection and both
cpp26 native/fallback selection where supported. The actual vector-producing rank pipeline is
unsized (`RankView` is a forward view without `size()`), so the probe's vector `push_back` baseline
is a reasonable allocation control for the current `ranges::to<vector>` expression.

## Experiment validity and limits

`nm` on the primary timing executable shows only undefined references to standard
`operator new/delete`, not replacement definitions; allocation counting is separated into the
diagnostic probe. The primary phases compare the same constructed values and mutable
rewrite/checksum across vector and inline-4/8 for build/return, copy and move. The no-reserve vector
producer is consistent with the unsized `RankView` baseline. The 64-byte inline-8 object versus
24-byte vector object is a real per-result stack/object-size cost and must be reported alongside
allocation savings.

Caller-span allocates exactly the known arity for spill; `RankView` does not provide that arity in
advance, so its one-allocation large-rank rows are a favorable lower bound rather than a drop-in
measured implementation. Visitor directly recomputes values and offers no mutable contiguous
sequence; it is an API alternative, not a semantically equal OperandList replacement. Keep those
alternatives separate from the primary owned-list comparison.

The timing binary is a suitable bounded container-kernel harness once the compile blocker is fixed.
Its current one-process aggregate timings alone are not acceptance evidence: use paired/repeated
order, include the selected real capacity and 0/8/9/1024 and longer cohorts, and weight conclusions
only from measured application arity/allocation data. Do not claim integrated Vortex performance
until full application integration, tests, and end-to-end probes complete.
