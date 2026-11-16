# Round 2 critique: Language Design & Ergonomics (performance-architecture-proposals.md)

Paths below are relative to the repo root.

Tally: 0 BLOCKING, 1 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Ring-buffer trail and O(1) cut reset): RESOLVED — Struck ring-buffer trail; replaced with
  contiguous LIFO `std::vector` trail and watermark `trailMark`, resetting only when
  `choiceStack_.empty() && 0 == vmSavepointDepth_`.
- R1.2 (Global manifold mutation epoch invalidation): RESOLVED — Struck monolithic `DecodedOp` cache
  and global epoch invalidation; moved to Refused Alternatives §4.4; replaced with hyperstructural
  dispatch in P5.
- R1.3 (Omitting `CellRef opcode` in `DecodedOp`): RESOLVED — Struck `DecodedOp`; P5 explicitly
  preserves pure hyperstructural execution and `ExecutionResult.failingClause`.
- R1.4 (Unconditional trailing in deterministic languages): RESOLVED — Adopted two-tier conditional
  trailing with RAII `VMSavepoint`, bypassing trailing when
  `0 == choiceDepth() && 0 == vmSavepointDepth_`.
- R1.5 (Direct-indexed flat VM tables breaking synthetic `CellRef`s): RESOLVED — Mandated
  open-addressed flat hash maps with generation tags, prohibiting raw `CellRef`-indexed arrays due
  to R8 `ephemeralBit` and sparse test IDs.
- R1.6 (Checkpoint codec omission corrupting dynamic stores): RESOLVED — Refused LMDB `Manifold`
  checkpoints due to R9 materialised-view honesty and missing `byRef`; moved to Refused Alternatives
  §4.2.
- R1.7 (Omission of numbered Proposal Register): RESOLVED — Added formal Proposal Register table
  (§3) covering P1 through P5 with files, justifications, costs, and fallbacks.
- R1.8 (Omission of Refused Alternatives section): RESOLVED — Added dedicated Refused Alternatives
  section (§4) evaluating and rejecting 5 classic alternatives with technical justifications.

## BLOCKING

None.

## MAJOR

### 1. (§2.5, line 215) Review finding

Full title: `inplace_vector<CellRef, 8>` in P5 and U-P3 cannot fall back to heap, imposing an
arbitrary 8-operand ceiling on surface language predicates and calls (violating Rule 4)

**Problem:** Proposal 5 specifies refactoring `VortexCore::wingOf` to "return a
`beman::inplace_vector<CellRef, 8>` (via `<gleditor/cpp26.hpp>`) or lazy range / callback
(`function_ref<void(CellRef)>`) for common opcodes ($\le 8$ operands), falling back to heap only if
exceeded, eliminating `7.4M+` heap allocations." Open Question `U-P3` then sets the default to:
"`inplace_vector<CellRef, 8>` for drop-in compatibility with existing callers that index `inputs[i]`
/ `outputs[i]`."

This design has an internal type contradiction that directly breaks surface language semantics:

1. `beman::inplace_vector<T, N>` / `std::inplace_vector<T, N>`
   (`include/gleditor/cpp26_inplace_vector.hpp:14-24`) is a fixed-capacity, purely stack-allocated
   container. It has no heap allocation fallback. Attempting to push more than 8 elements throws
   `std::bad_alloc`. A function with return type `inplace_vector<CellRef, 8>` cannot "fall back to
   heap only if exceeded".
1. If `wingOf` and `inputsOf` / `outputsOf` return `inplace_vector<CellRef, 8>`, any surface
   language construct with $> 8$ operands will throw `std::bad_alloc` at runtime:
   - In `VProlog`: facts, relations, and compound terms frequently have arity $> 8$ (e.g. database
     records, multi-argument predicates).
   - In `VLace`: data constructors and curried function calls with $> 8$ arguments.
   - In `VPL`: array literals and multi-argument verbs.
   - In `vagent`: action schemas, perception frames, and tool invocations with multiple attributes.
   - In `vortex_stdlib.cpp:2368`: `inputsOf(fnOp)` binds arguments for arbitrary stdlib function
     calls.
1. This introduces a hard ceiling of 8 operands across the entire language runtime, directly
   violating Standing House Style Rule 4 ("No Arbitrary Ceilings: Do not invent fixed small caps
   that force user workarounds; when a bound is required for resource safety, price it and expose it
   as a documented setting or grant limit.").

**Evidence:**

- `design/performance-architecture-proposals.md:215-218`: "Refactor `VortexCore::wingOf` to return a
  `beman::inplace_vector<CellRef, 8>` (via `<gleditor/cpp26.hpp>`) or lazy range / callback
  (`function_ref<void(CellRef)>`) for common opcodes ($\le 8$ operands), falling back to heap only
  if exceeded"
- `design/performance-architecture-proposals.md:272`: "Default: `inplace_vector<CellRef, 8>` for
  drop-in compatibility with existing callers that index `inputs[i]` / `outputs[i]`."
- `include/gleditor/cpp26_inplace_vector.hpp:14-24`: wraps `inplace_vector`, which throws on
  overflow.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2368`: `inCells = core_.inputsOf(fnOp)` binds
  arbitrary-arity arguments.

**Fix:** Correct the return type specification in §2.5, §3, and `U-P3` to support unbounded arities
without throwing `std::bad_alloc`:

1. Either specify a hybrid container `OperandList` / `SmallCellVector<8>` (e.g.
   `std::variant<cpp26::inplace_vector<CellRef, 8>, std::vector<CellRef>>` with an adapter providing
   `operator[]`, `size()`, `begin()`, and `end()`) that stores $\le 8$ elements inline and spills to
   heap if $> 8$; OR
1. Specify that `wingOf` returns a lazy range over `rank(arena, first, dims_.step)` (or provide a
   visitor `forEachOperand(opcode, side, function_ref<void(CellRef)>)`), while providing an inline
   helper `inlineWingOf<8>()` only for opcodes with statically known bounds;
1. Update `U-P3`'s default to explicitly document that arities $> 8$ dynamically spill to heap
   without imposing an arbitrary ceiling.

## MINOR

- (§2.1, line 109) `sizeof(VMTrailEntry)` pricing: Line 109 states `VMTrailEntry` is 24 bytes
  assuming a 16-byte `std::variant<uint64_t, CellRef, shared_ptr>`. In standard C++ library
  implementations (libstdc++ / libc++), a `std::variant` containing a 16-byte member and alignment
  padding occupies 24 bytes by itself, resulting in a 32-byte struct (`8B header + 24B variant`)
  unless packed as a custom tagged union sharing discriminator bytes.
- (§2.1, line 80) Inconsistent usage of `choiceStack_.size() == 0` vs `choiceDepth() == 0`: Line 80
  checks `0 == choiceStack_.size() && 0 == vmSavepointDepth_`, while elsewhere the doc and runtime
  use `0 == choiceDepth()`. Using the canonical `choiceDepth()` method ensures consistency with
  `VortexVM` accessors.
- (§2.5, line 210) Orphaned reference to `vortex_frames.cpp:194`: Line 210 states
  `VortexVM::frameDescriptor is defined in apps/common/xanadu/vortex/vortex_frames.cpp:194`. This
  was an artifact from the v1 `DecodedOp` design; in v2 it has no purpose or referent in P5.
- (§3, line 237) P5 cost column pricing: The Proposal Register table lists cost as
  `40B stack per wingOf; eliminates 7.4M+ heap allocs`. This accounts only for
  `sizeof(inplace_vector<CellRef, 8>)`, and should note the hybrid/small-vector representation cost
  when heap fallback occurs for large arities.

VERDICT: NOT CONVERGED
