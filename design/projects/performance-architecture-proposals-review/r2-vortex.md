# Round 2 critique: Vortex & Spool-Mesh Expert (performance-architecture-proposals.md)

Paths below are relative to the repo root (`/data/git/gleditor-restore`). Tally: 0 BLOCKING, 2
MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Ring-buffer trail violates backtracking LIFO invariants and cut semantics): RESOLVED —
  Struck all ring-buffer references; replaced with contiguous LIFO `std::vector<VMTrailEntry>` trail
  and preserved outer trail records on `cut(cutBarrier > 0)`.
- R1.2 (Mischaracterization of `stealNearRootChoicePoint()` and OR-parallel workers): RESOLVED —
  Corrected to reflect that workers construct fresh VM/Arena instances from scalar spark metadata,
  and victim pops the root choice point while advancing root watermark.
- R1.3 (Global Manifold mutation epoch invalidation causes cache thrashing): RESOLVED — Monolithic
  `DecodedOp` flat vector and manifold mutation epoch invalidation were refused; replaced with P5
  zero-allocation `wingOf` and $O(1)$ `OpcodeKind` dispatch.
- R1.4 (Conflation of backtracking trail vs deterministic execution and local savepoints): RESOLVED
  — Two-tier conditional trailing model adopted with RAII `VMSavepoint`, bypassing trail when
  `0 == choiceDepth() && 0 == vmSavepointDepth_`.
- R1.5 (Sparse/synthetic `CellRef` key hazard for dense flat VM tables): RESOLVED — Prohibited raw
  `CellRef`-indexed direct arrays; mandated open-addressed flat hash maps with generation tags.
- R1.6 (Missing Proposal Register entries for core runtime modifications): RESOLVED — Added formal
  `## 3. Proposal Register` table with numbered entries `P1`–`P5` stating files touched, C++
  justification, priced budgets, and fallbacks.

______________________________________________________________________

## BLOCKING

*None.*

______________________________________________________________________

## MAJOR

### 1. (§2.5, line 215 & §5, line 272) Review finding

Full title: Returning `inplace_vector<CellRef, 8>` directly lacks heap fallback, imposing an
unbudgeted hard ceiling of 8 operands on opcode wings

**Problem:** Proposal 5 states (§2.5, line 215): "Refactor `VortexCore::wingOf` to return a
`beman::inplace_vector<CellRef, 8>` (via `<gleditor/cpp26.hpp>`) or lazy range / callback
(`function_ref<void(CellRef)>`) for common opcodes ($\le 8$ operands), falling back to heap only if
exceeded, eliminating `7.4M+` heap allocations." However, Open Question `U-P3` (§5, line 272)
establishes: "Default: `inplace_vector<CellRef, 8>` for drop-in compatibility with existing callers
that index `inputs[i]` / `outputs[i]`."

This creates a critical contradiction and runtime hazard:

1. `inplace_vector<T, N>` is a fixed-capacity container with strictly inline storage; in C++ it
   throws `std::bad_alloc` when `push_back` is called beyond $N$. A function whose return type is
   statically declared as `inplace_vector<CellRef, 8>` **cannot** fall back to the heap when
   capacity is exceeded.
1. In ZigZag/Vortex, operand wings along `+d.grab` / `-d.grab` chained via `d.step` have no
   topological bound. User-defined functions, VQL multi-column relations, `#Call` sites with $\ge 9$
   parameters, and record constructors with $\ge 9$ fields will attempt to push a 9th operand and
   crash the runtime with `std::bad_alloc`. This introduces an arbitrary small ceiling directly
   violating House Style Rule 4 ("No Arbitrary Ceilings").

**Evidence:**

- `design/performance-architecture-proposals.md:215-218, 272`.
- `include/gleditor/cpp26_inplace_vector.hpp:1-30`.
- `apps/common/xanadu/vortex/vortex_core.cpp:382-398`: `wingOf` walks
  `zigzag::rank(arena_, *first, dims_.step)` which yields an unbounded sequence of operand slots.

**Fix:** Specify a concrete return type that provides drop-in index compatibility (`inputs[i]`)
while genuinely falling back to the heap when operands exceed 8: Define an `OperandList` hybrid
small-vector struct (e.g. 64 bytes total: `gleditor::cpp26::inplace_vector<CellRef, 8>` inline
buffer [40B] + `std::vector<CellRef>` heap fallback [24B], providing `operator[]`, `size()`, and
standard iterator accessors). If `size() <= 8`, zero heap allocations occur; if `size() > 8`,
operands spill into `heapOperands`. Update `U-P3` to name this hybrid container rather than bare
`inplace_vector<CellRef, 8>`.

______________________________________________________________________

### 2. (§2.1, lines 83-84) Review finding

Full title: Premature trail truncation on inner `VMSavepoint::commit()` when `choiceStack_.empty()`
corrupts enclosing exception rollback in nested frames

**Problem:** Proposal 1 specifies (§2.1, lines 83-84): "Introduce an RAII `VMSavepoint` (symmetric
to `zigzag::ArenaSavepoint`) for `invokeFrame`, `returnFrame`, `CheckClosureCall`, and
`executeRestoreBindings`. On `VMSavepoint::commit()` when `choiceStack_.empty()`, truncate
`vmTrail_` back to the entry watermark immediately."

This rule is incorrect when savepoints are nested during deterministic execution
(`choiceDepth() == 0`): If an outer savepoint (e.g. `invokeFrame`, `vmSavepointDepth_ == 1`, entry
watermark 0) executes a routine that invokes an inner savepoint (e.g. `CheckClosureCall`,
`vmSavepointDepth_ == 2`, entry watermark $W > 0$):

1. When the inner savepoint succeeds, it calls `commit()`.
1. Because `choiceStack_.empty()` is true, a literal execution of "truncate `vmTrail_` back to the
   entry watermark immediately" truncates `vmTrail_` back to $W$, discarding the undo entries
   recorded by the inner routine.
1. If the outer savepoint subsequently encounters an error (such as a failed argument validation,
   postcondition assertion, or allocation failure) and triggers rollback to watermark 0, the undo
   records between $W$ and the top are missing. The inner mutations cannot be unwound, silently
   corrupting register and frame state.

**Evidence:**

- `design/performance-architecture-proposals.md:83-84`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:2484-2498`: In
  `ArenaManifold::discard(const Mark &m)`, `trail_` is cleared **only** when
  `0 == outstandingMarks_`. While any outer mark remains active (`outstandingMarks_ > 0`), the trail
  entries are retained so that an enclosing rollback can unwind all nested modifications.

**Fix:** Harmonize `VMSavepoint::commit()` exactly with `ArenaManifold::discard`: On
`VMSavepoint::commit()`, decrement `vmSavepointDepth_`. Clear `vmTrail_` **only** when
`choiceStack_.empty() && 0 == vmSavepointDepth_`. When outer savepoints or choice points remain
active (`vmSavepointDepth_ > 0 || !choiceStack_.empty()`), committed undo entries must remain in
`vmTrail_` so that an enclosing rollback can unwind them.

______________________________________________________________________

## MINOR

- (§2.5, line 215) **Header and namespace citation for `inplace_vector`:** Line 215 cites
  `beman::inplace_vector<CellRef, 8> (via <gleditor/cpp26.hpp>)`. In this repo, `inplace_vector` is
  isolated in `<gleditor/cpp26_inplace_vector.hpp>` (not `cpp26.hpp`), and per `AGENTS.md` must be
  referenced via the repo wrapper namespace `gleditor::cpp26::inplace_vector`.
- (§3, line 237) **P5 Proposal Register files touched:** The table omits
  `apps/common/xanadu/vortex/vortex_frames.cpp` (where `VortexVM::inputsOf` and
  `VortexVM::outputsOf` are defined at lines 326–336) and `apps/common/xanadu/vortex/vortex_vm.hpp`
  (where they are declared at lines 792–793). When `VortexCore::wingOf` return type changes, these
  wrapper signatures must adapt.
- (§2.1, line 109) **Non-trivial union destruction in `VMTrailEntry`:** Line 109 proposes a
  `uint64_t/CellRef/shared_ptr` union. Because `std::shared_ptr<const ActivationFrame>` has
  non-trivial destruction and copy/move semantics, `VMTrailEntry` should be explicitly documented as
  using a tagged `std::variant` or an aligned union with manual destructor invocation keyed on
  `containerTag`.
- (§2.1, lines 61–63) **Complete container list for `ChoicePoint`:** Line 61 lists 10 map/set
  containers, but `ChoicePoint` (`vortex_vm.hpp:520–543`) also contains
  `std::optional<CellRef> streamCursor` and `std::optional<std::uint64_t> mutationCheckpointId`,
  which should be noted alongside `streams_` and `mutationGroups_`.

______________________________________________________________________

VERDICT: NOT CONVERGED
