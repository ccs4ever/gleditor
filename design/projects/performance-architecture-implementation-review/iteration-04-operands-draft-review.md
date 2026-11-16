# M4 OperandList draft review

**Disposition:** The isolated container implementation is sound for the inspected invariants and
passes small native/fallback compilation and unit checks. One requested regression gap remains
before this is ready for integration: copy-assignment allocation failure is not tested.

## Actual checks run

Using the project compiler and standard (`clang++ -std=c++2c`) against the isolated copy:

- Compiled and ran a small program asserting `sizeof(OperandList) == 64`, contiguous/sized range
  concepts, no implicit vector conversion, ninth-element spill, span access, copy/move, and
  moved-from reuse.
- Ran all four isolated `operand_list_test.cpp` tests against the native `std::inplace_vector` path:
  **4 passed**.
- Repeated the same four tests with `-DGLEDITOR_CPP26_FORCE_FALLBACK` against the pinned Beman
  inplace-vector fallback: **4 passed**.

These are isolated checks only. No repository integration build, app/test consumer compilation, full
suite, formatter, or performance comparison was run.

## Findings

- The ninth append is failure-atomic: it builds a temporary spill, reserves nine cells, copies/moves
  the eight trivial `CellRef`s, appends the ninth, swaps only after success, and only then clears
  inline storage. If reserve throws, the object remains inline and unchanged.
- Spill mode is distinguishable from inline mode using vector capacity because the first spill
  reserves at least nine elements. `clear()` intentionally retains spill capacity; subsequent
  appends remain in spill mode and reuse it. Move operations reset both members on the source, and
  tests confirm moved-from reuse.
- The empty, nothrow-copyable, always-equal allocator constraints make vector move/swap/reset
  compatible with the explicit noexcept paths. The test allocator satisfies those constraints and
  injects failure at the first spill allocation.
- `data()`, mutable/const `span()`, and span-derived iterators form a contiguous sized range in both
  native and fallback configurations. Mutable span writes are observed through indexing. Direct
  conversion to vector is absent; the existing VQL/VPL diagnostic path remains an explicit
  `ranges::to<std::vector<...>>()` boundary.
- `VortexCore::wingOf` still preserves rank order and the existing operand/master transform while
  appending to the result. `VortexVM::inputsOf`/`outputsOf` preserve the in-place `runtimeSlot`
  rewrite. The `resolveCell` boundary now accepts `span<const CellRef>`; inspected callers pass
  named lists/vectors, so the span does not outlive a temporary. `getOpcodeKind` and its
  register/map/numeric precedence are untouched in this patch.
- The source changes replace vector ownership at the public core/VM return boundaries, so consumers
  must all be rebuilt together. No layout/persistence concern was identified; the local native and
  fallback ABI checks both met the 64-byte provisional target.

## Required test addition

The implementation gives spilled copy assignment a pre-commit allocation point
(`Spill copied(other.spill_)`), but the tests inject allocation failure only during the ninth
`push_back`. Add a test with an inline destination and spilled source, set the failing allocator
before `destination = source`, assert `std::bad_alloc`, then assert the destination's prior
size/order/values are unchanged and retry successfully. This directly verifies the promised
assignment guarantee instead of inferring it from the code.

The isolated files also still need the planned full compilation of every migrated consumer and
project test integration before M4 can pass its gate.
