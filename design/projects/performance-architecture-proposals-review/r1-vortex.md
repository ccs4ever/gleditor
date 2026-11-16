# Round 1 critique: Vortex & Spool-Mesh Expert (performance-architecture-proposals.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§2, line 78) Ring-buffer trail violates backtracking LIFO invariants and cut semantics

Full title: Ring-buffer trail violates backtracking LIFO invariants, risks silent state corruption
or arbitrary depth ceilings, and misdefines cut reset semantics

**Problem:** Proposal 1 suggests using a "fixed-capacity ring buffer or contiguous arena-backed
trail" that "resets in $O(1)$ on `cut()` / `clearChoicePoints()`" to avoid per-node heap
allocations. This proposal violates fundamental trail invariants in two ways:

1. An undo trail is strictly a LIFO stack. A ring buffer is a FIFO structure that evicts/overwrites
   the oldest entries when capacity is exceeded. If a search exceeds the ring buffer's capacity,
   older choice points will have their trail records overwritten. A subsequent `backtrack()` to an
   earlier choice point will read corrupted/overwritten undo entries or underflow watermarks,
   silently desynchronizing VM state (leaking register bindings, corrupting activation stacks, or
   leaving stale stream registers). Alternatively, if the ring buffer throws on overflow, it
   introduces an unbudgeted hard ceiling on search depth/turn size, directly violating House Style
   Rule 4 ("No Arbitrary Ceilings").
1. The assertion that the trail "resets in $O(1)$ on `cut()`" is dangerously invalid for
   `cut(barrier)` with `barrier > 0`. In `VortexVM::cut(std::size_t cutBarrier)`
   (`apps/common/xanadu/vortex/vortex_vm.cpp:1493-1546`), cutting inner choice points leaves outer
   choice points intact on `choiceStack_`. If the trail were reset on an inner `cut()`, the undo
   records required by surviving outer choice points would be destroyed, permanently breaking
   backtracking for all enclosing predicates.

**Evidence:**

- `design/performance-architecture-proposals.md:78-83`: "Similar to `NetStreamRing` ... a
  fixed-capacity ring buffer or contiguous arena-backed trail avoids per-node heap allocations
  during execution and resets in $O(1)$ on `cut()` / `clearChoicePoints()`."
- `apps/common/xanadu/vortex/vortex_vm.cpp:1493-1546`: `cut(cutBarrier)` removes choices above
  `cutBarrier`; choice points `[0, cutBarrier)` remain active on `choiceStack_`.
- `apps/common/xanadu/zigzag/arena_manifold.hpp:186-202` and
  `apps/common/xanadu/zigzag/arena_manifold.cpp:2484-2498`: `ArenaManifold::trail_` uses a
  contiguous `std::vector<TrailEntry>` LIFO stack and clears trail storage *only* when
  `0 == outstandingMarks_` (`arena_manifold.cpp:2493-2496`).

**Fix:** Strike all references to "ring buffer". The VM undo trail must be a contiguous LIFO stack
(e.g., `std::vector<VMTrailEntry>` or chunked slab stack). The trail watermark `trailMark` recorded
in `ChoicePoint` can only be cleared when `choiceStack_.empty()` and `0 == activeSavepoints_`. On
`cut(cutBarrier)`, choice points above `cutBarrier` are discarded, but the trail is never reset
while `cutBarrier > 0`.

______________________________________________________________________

### 2. (§2, line 85) Mischaracterization of `stealNearRootChoicePoint()` and OR-parallel workers

Full title: Mischaracterization of `stealNearRootChoicePoint()` state transfer and nonexistent
worker trail slice reconstruction

**Problem:** Proposal 1 lists under "Risks & trade-offs": "`stealNearRootChoicePoint()`
(`vortex_vm.cpp:1604`) for OR-parallelism (`or_parallel.cpp:224`) moves a near-root `ChoicePoint` to
a worker VM. A trail-based design must either materialize a delta snapshot only when stealing (rare)
or keep OR-parallel stealing reconstructing state from the root-to-choice trail slice." This claim
is factually false with respect to the repository's OR-parallel implementation:

1. A worker VM never receives, unpacks, or reconstructs the victim's `ChoicePoint` register maps
   (`cellReferences`, `streams`, `iterators`, `activationFrames`, etc.). In
   `or_parallel.cpp:202-247` (`exportSparkFromVictim`), `stolen` is consumed solely to populate
   `OrParallelSpark` with scalar metadata: `deweyPath`, `deweyKey`, `cyclesSinceRoot`, `cursor`,
   `stackFrame`, and `altOp`. The rest of `stolen`'s snapshotted maps are discarded.
1. In `or_parallel.cpp:392-444` (`executeSparkOnWorker`), the worker creates a brand new
   `ArenaManifold workerArena{base}` (hydrating `prefixSnapshot` if running `ShadowDeltaPrefix`) and
   a fresh `VortexVM workerVm{workerCore}`. The worker's registers start empty and its opcodes are
   registered fresh via `bindProgramOpcodes`. There is no "trail slicing" or "delta snapshot" on the
   worker.
1. On the victim VM, `stealNearRootChoicePoint()` (`vortex_vm.cpp:1604-1618`) merely does
   `choiceStack_.erase(choiceStack_.begin())` and calls `core_.arena().evictOldestMark()`. In a
   trail-based VM, popping the root choice point simply advances the victim's root watermark to
   `choiceStack_.front().trailMark`. The trail records preceding that watermark become dead prefix
   entries and can be retained until `choiceStack_.empty()`, exactly matching
   `ArenaManifold::evictOldestMark()` (`arena_manifold.cpp:3453-3460`).

**Evidence:**

- `design/performance-architecture-proposals.md:85-89`: "A trail-based design must either
  materialize a delta snapshot only when stealing (rare) or keep OR-parallel stealing reconstructing
  state from the root-to-choice trail slice."
- `apps/common/xanadu/vortex/or_parallel.cpp:229-246`: `OrParallelSpark` captures only `deweyPath`,
  `cyclesSinceRoot`, `cursor`, `stackFrame`, `altOp`, and `prefixSnapshot`. It never reads or
  serializes `stolen->cellReferences` or other VM maps.
- `apps/common/xanadu/vortex/or_parallel.cpp:392-420`: Workers construct independent `ArenaManifold`
  and `VortexVM` instances from `base`.
- `apps/common/xanadu/vortex/vortex_vm.cpp:1604-1618`: `stealNearRootChoicePoint()` enforces
  `streams_.empty() && iterators_.empty() && !hasActiveGuards() && !hasScopedChoices()`, erases
  `choiceStack_.front()`, and calls `core_.arena().evictOldestMark()`.

**Fix:** Rewrite the risk section to accurately reflect the real codebase: strike the requirement
for delta snapshot materialization and trail slicing for workers. Clarify that for the victim VM,
stealing the root choice point simply advances the root watermark; dead trail prefix entries remain
inert until the turn terminates or `clearChoicePoints()` clears the trail.

______________________________________________________________________

### 3. (§2, line 201) Global "Manifold mutation epoch" invalidation causes severe cache thrashing

Full title: Global Manifold mutation epoch invalidation causes catastrophic instruction cache
thrashing and relies on nonexistent manifold counter

**Problem:** Proposal 5 specifies: "Compile/lower a `VortexCore` opcode graph lazily on first
execution into a flat vector of resolved `DecodedOp` structs ... keyed by a `Manifold` mutation
epoch so any structural edit invalidates the decoded cache." This design suffers from two critical
flaws:

1. Neither `zigzag::Manifold` (`apps/common/xanadu/zigzag/manifold.hpp`) nor `ArenaManifold`
   (`apps/common/xanadu/zigzag/arena_manifold.hpp`) possesses a mutation epoch counter. Introducing
   one touches core engine headers without isolating it in a numbered Proposal Register entry
   (violating House Style Rule 6).
1. During normal execution, standard Vortex opcodes continuously mutate the manifold: `New`, `Link`,
   `Break`, `MakeVar`, `unify`, `writeValue`, and stream operations all perform structural edits on
   runtime data cells (`vortex_vm.cpp:4147-4196`). If *any* structural edit in the manifold
   invalidates the decoded instruction cache, the cache will be invalidated on virtually every cycle
   in loops that construct or link data! The resulting recompilation/allocation churn would cause
   severe performance degradation worse than the existing interpretation loop.

**Evidence:**

- `design/performance-architecture-proposals.md:201-209`: "keyed by a `Manifold` mutation epoch so
  any structural edit invalidates the decoded cache."
- `apps/common/xanadu/vortex/vortex_vm.cpp:4147-4196`: `stepImpl` performs source mutations (`Link`,
  `Value`, `New`, `Break`, `Splice`, `Clone`) as everyday execution primitives.
- `apps/common/xanadu/vortex/vortex_core.cpp:208-226` and
  `apps/common/xanadu/vortex/vortex_vm.hpp:583-596`: P15 (`WriteFault::OpcodeKind`, `writeGuard_`)
  *already* prevents program-value writes to registered opcode cells.

**Fix:** Decouple instruction cache invalidation from global data-manifold mutations. Cache decoded
instructions keyed by `CellRef opcode` in a dense table or flat vector:

1. Leverage P15's existing kind immutability: registered opcode cells are already protected against
   value overwrites.
1. Invalidate decoded opcode entries only when an opcode's structural code edges (`d.spin`,
   `d.grab`, `d.step`, `d.contract`) are mutated, or when `pruneAssemblyState()` / `setOpcodeKind()`
   is invoked. Data-cell mutations must never invalidate the instruction cache.

______________________________________________________________________

## MAJOR

### 4. (§2, line 60) Review finding

Full title: Conflation of nondeterministic backtracking trail with deterministic function calls and
transient exception rollback

**Problem:** Proposal 1 notes that `invokeFrame` (`vortex_frames.cpp:401-408`), `returnFrame`
(`vortex_frames.cpp:620-625`), and closure opcodes (`vortex_closures.cpp:864-883`) copy full maps
for exception rollback, and proposes routing all VM mutations through an undo trail with watermarks.
However, it fails to separate two distinct execution modes:

1. **Nondeterministic Search (`choiceDepth() > 0`)**: Backtracking is an expected control-flow
   event. Register mutations must be trailed so `backtrack()` can restore previous values.
1. **Deterministic Execution (`choiceDepth() == 0`)**: Normal function calls
   (`invokeFrame`/`returnFrame`) and closure calls execute deterministically without choice points.
   If register writes unconditionally append to an undo trail when `choiceDepth() == 0`, the trail
   will grow unboundedly in long-running deterministic loops, accumulating memory until the turn
   ends. Furthermore, the full map copies in `invokeFrame`, `returnFrame`, and `CheckClosureCall`
   are strictly *local error-recovery guards* (e.g. restoring state if `arena.mark()` fails or
   argument binding throws). For instance, `CheckClosureCall` (`vortex_closures.cpp:864-883`) copies
   the entire `cellReferences_` map solely to guard the write of a single output slot and a cursor
   PC update! The proposal must explicitly specify how local error recovery interacts with the
   trail: a scoped RAII `VMSavepoint` (mirroring `zigzag::ArenaSavepoint`) should be used. When
   `choiceDepth() == 0` and no `VMSavepoint` is active, register writes should bypass the trail
   entirely. When a local `VMSavepoint` commits successfully at `choiceDepth() == 0`, its trail
   entries must be immediately discarded.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_frames.cpp:401-440`: copies 6 maps into stack locals
  (`previousReferences`, `previousStreams`, etc.) and restores them only in
  `catch (const std::exception &error)`.
- `apps/common/xanadu/vortex/vortex_closures.cpp:864-883`: `CheckClosureCall` copies
  `cellReferences_` solely to undo one output write if `savepoint.commit()` throws.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:2361-2364`: `if (0 == outstandingMarks_) return;` —
  `ArenaManifold` bypasses trailing entirely when no marks/savepoints are active.

**Fix:** Specify a two-tier trailing model:

- Introduce a RAII `VMSavepoint` helper (wrapping `vmSavepointDepth_++` and recording entry
  `trailMark`), symmetric to `zigzag::ArenaSavepoint`.
- In VM mutator helpers, if `0 == choiceDepth() && 0 == vmSavepointDepth_`, skip appending to
  `vmTrail_`.
- When `VMSavepoint::commit()` runs at `choiceDepth() == 0`, truncate `vmTrail_` back to entry
  watermark, reclaiming trail storage immediately.

______________________________________________________________________

### 5. (§2, line 72) Sparse/synthetic `CellRef` key hazard for dense flat VM tables

Full title: Dense flat VM tables risk gigabyte allocations or segfaults on sparse synthetic
`CellRef` keys

**Problem:** Proposal 1 proposes replacing `std::unordered_map<CellRef, ...>` for `cellReferences_`,
`streams_`, `iterators_`, `opcodeMap_`, and `cursorPC_` with "flat generation-stamped tables ...
keyed by `CellRef`". While production stores assign dense monotonic `uint32_t` operation indices,
synthetic unit tests across the test suite frequently construct sparse, mock `CellRef` values (e.g.,
`CellRef{100'000}` or `0x7FFF'FFFE`). Directly indexing a flat array (`std::vector<T>[cellRef]`)
would either attempt multi-gigabyte allocations or segfault on sparse synthetic keys. Proposal 2
(§2, lines 108-117) acknowledged this exact hazard for `Manifold::byRef` and proposed a two-level
paged array or flat open-addressed hash table, but Proposal 1 omits this safeguard for VM tables.

**Evidence:**

- `design/performance-architecture-proposals.md:72-74`: "Replace `std::unordered_map<CellRef, ...>`
  for `cellReferences_`, `streams_`, `iterators_`, `opcodeMap_`, and `cursorPC_` with flat
  generation-stamped tables or open-addressed flat maps keyed by `CellRef`."
- `apps/common/xanadu/vortex/vortex_vm.hpp:871-885`: containers map `CellRef` to registers, opcode
  kinds, and activation stacks.

**Fix:** Explicitly mandate that flat VM per-cell tables must use either:

1. Open-addressed flat hash maps (`std::vector<Slot>` with linear/Robin Hood probing and generation
   tags for $O(1)$ clearing), safe for arbitrary sparse 32-bit keys, or
1. A two-level paged array (e.g., 1024-entry leaf pages allocated on demand matching
   `SegmentedOpsSpool` pieces) if direct non-hashing array indexing is required. Unpaged direct
   array indexing by raw `CellRef` must be explicitly prohibited.

______________________________________________________________________

### 6. (§2, line 50) Missing Proposal Register entries for core runtime modifications

Full title: Proposals 1 and 5 lack numbered Proposal Register entries with file lists, costs, and
fallbacks

**Problem:** House Style Rule 6 (`brief-common.md:30-32`) requires: "Any change to existing C++
engine, core, stdlib, or runtime files must be isolated in a numbered Proposal Register (`P1`, `P2`,
...) stating exact files touched, why it cannot live in caller/compiler, cost, and fallback."
Proposals 1 and 5 propose structural rewires of `vortex_vm.hpp`, `vortex_vm.cpp`,
`vortex_frames.cpp`, `vortex_closures.cpp`, `vortex_bindings.cpp`, `vortex_core.hpp`, and
`vortex_core.cpp`, but present them as informal narrative sections rather than numbered register
entries with priced costs (bytes per trail entry, allocations per step, cache memory footprints) and
fallbacks.

**Evidence:**

- `brief-common.md:30-32` and `SKILL.md:67-70`.
- `design/performance-architecture-proposals.md:50-92, 190-210`: missing formal register metadata
  blocks.

**Fix:** Restructure Proposals 1 and 5 into numbered Proposal Register entries (`P1`, `P5`)
specifying:

- Exact files touched (including `vortex_bindings.cpp`, `vortex_streams.cpp`,
  `vortex_contracts.cpp`, which also access `cellReferences_`).
- Justification why the change cannot live in the VQL/Vortex compiler.
- Concrete memory and cycle budgets (e.g., 24-byte `VMTrailEntry` union/variant, paged table memory
  limits).
- Fallback paths if not approved (e.g., retaining sparse maps with upfront `reserve()`, or caching
  only `OpcodeKind` and arity).

______________________________________________________________________

## MINOR

### 7. (§2, line 60) Incomplete call site citations for map-cloning exception rollback

Full title: Incomplete call site citations for map-cloning exception rollback in closure and binding
opcodes

**Problem:** Proposal 1 cites `vortex_closures.cpp` generally for closure map copies, but the map
copies specifically occur in `CheckClosureCall` (`vortex_closures.cpp:864-883`), while
`CreateClosure` uses `ArenaSavepoint` (`vortex_closures.cpp:529-541`). It also omits
`executeRestoreBindings` (`apps/common/xanadu/vortex/vortex_bindings.cpp:238-258`), which similarly
clones `cellReferences_` on entry.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_closures.cpp:529`, `864`.
- `apps/common/xanadu/vortex/vortex_bindings.cpp:238`: `auto oldCellReferences = cellReferences_;`.

**Fix:** Cite the specific call sites: `CheckClosureCall` (`vortex_closures.cpp:864-883`) and
`executeRestoreBindings` (`vortex_bindings.cpp:238-258`), and note that `CreateClosure` already uses
`ArenaSavepoint`.

______________________________________________________________________

### 8. (§2, line 58) Clarify exclusion of `resumeValues_` / `resumeTokenIds_` from the trail

Full title: Explicitly confirm that `resumeValues_` and `resumeTokenIds_` remain non-trailed VM
control state

**Problem:** Proposal 1 lists 10 containers snapshotted by `ChoicePoint`, but does not clarify the
status of `resumeValues_` and `resumeTokenIds_`. `vortex_vm.hpp:836-837` explicitly documents:

```cpp
// Resume values are VM control state, deliberately not part of a
// ChoicePoint snapshot. Only token references in cellReferences_ backtrack.
```

When wrapping register writes into a unified trail mutator, the design must explicitly reaffirm that
`resumeValues_` mutations do not enter the undo trail.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.hpp:836-837`.

**Fix:** Add an explicit sentence stating that `resumeValues_` and `resumeTokenIds_` remain
untrailed VM control state, and only token references placed in `cellReferences_` are trailed.

______________________________________________________________________

VERDICT: NOT CONVERGED
