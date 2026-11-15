# Performance architecture proposals (v3)

This document records medium-to-large structural performance proposals derived from `-O3 -g -pg`
(`gprof`) profiling of the full test suite (`gleditor_test`, `zigzag_test`, `xuzz_test`) in
`/data/git/gleditor-restore`, following the low-hanging fixes in commit `5d604feb` (eliminating
full-map copies of `VortexVM::cursorPC_` and duplicate `structureBirths_` lookups in
`Manifold::applyStructure`).

Foundational invariants referenced below live in
[store-slice-convergence.md](store-slice-convergence.md) (R2, R7, R8, R9, R12),
[vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md),
[vql-performance-spike.md](vql-performance-spike.md), and
[vagent-metaprogramming.md](vagent-metaprogramming.md).

## 1. Observed `gprof` baseline (pre-`5d604feb`)

The three test binaries were compiled with `DEBUG_OPTS="-O3 -g -pg"` and run headlessly.

### 1.1 `xuzz_test` (`gmon.out`, ~26s total sampled time)

| % time                | Self (s) | Calls         | Symbol                                                                                              | Status after `5d604feb`                 |
| --------------------- | -------- | ------------- | --------------------------------------------------------------------------------------------------- | --------------------------------------- |
| 35.06%                | 9.20     | 774,792       | `std::_Hashtable<uint, pair<const uint, optional<uint>>>::_M_assign` (`VortexVM::cursorPC_` copies) | Fixed (`cursorPC_` single-entry backup) |
| 6.25%                 | 1.64     | 59,250,397    | `zigzag::Manifold::applyStructure(uint32_t, const CompactOpNode&)`                                  | Partially reduced (`try_emplace`)       |
| 6.02%                 | 1.58     | 243,788,510   | `zigzag::ArenaManifold::linked(CellRef, DimRef, DimVector) const`                                   | Open (§2.3 / P3)                        |
| 3.35% + 2.21%         | 1.46     | 2,120,764,009 | `xanadu::SegmentedOpsSpool::get(uint32_t)` (mutable + const)                                        | Open (§2.4 / P4)                        |
| 2.13%                 | 0.56     | 67,479,244    | `zigzag::Manifold::linkFor(uint32_t, uint32_t)`                                                     | Open (§2.2 / P2)                        |
| 2.02%                 | 0.53     | 728,075       | `~unordered_map<uint, optional<uint>>()` (`cursorPC_` teardown)                                     | Fixed                                   |
| 1.98%                 | 0.52     | 11,160,354    | `zigzag::vortex::VortexVM::getOpcodeKind(uint32_t) const`                                           | Open (§2.5 / P5)                        |
| 1.56% + 1.56% + 1.45% | 1.20     | 139,455,559   | `std::_Hashtable` ops on `Manifold::byRef` and `Manifold::structureBirths_`                         | Partially reduced (§2.2 / P2)           |
| 0.69% + 0.53%         | 0.32     | 675,006       | `_M_allocate_node` / `_M_deallocate_node` on `activationFrames_`                                    | Open (§2.1 / P1)                        |
| 0.57%                 | 0.15     | 2,535,901     | `zigzag::vortex::VortexVM::inputValue(uint32_t) const`                                              | Open (§2.5 / P5)                        |
| 0.50%                 | 0.13     | 7,402,292     | `zigzag::vortex::VortexCore::wingOf(uint32_t, DimVector) const`                                     | Open (§2.5 / P5)                        |

### 1.2 `zigzag_test` (`gmon.zigzag`, ~1.50s total sampled time)

| % time        | Self (s) | Calls      | Symbol                                                             | Status after `5d604feb`       |
| ------------- | -------- | ---------- | ------------------------------------------------------------------ | ----------------------------- |
| 31.93%        | 0.53     | 19,461,525 | `zigzag::Manifold::applyStructure(uint32_t, const CompactOpNode&)` | Partially reduced             |
| 7.23%         | 0.12     | 22,307,705 | `zigzag::Manifold::linkFor(uint32_t, uint32_t)`                    | Open (§2.2 / P2)              |
| 7.23%         | 0.12     | 161,980    | `std::_Hashtable<uint, pair<const uint, uint>>::_M_rehash`         | Open (§2.2 / P2)              |
| 6.63% + 6.63% | 0.22     | 25,973,736 | `structureBirths_.contains` + `_M_emplace_uniq`                    | Partially reduced (§2.2 / P2) |
| 5.42%         | 0.09     | 78,449,357 | `xanadu::SegmentedOpsSpool::get(uint32_t) const`                   | Open (§2.4 / P4)              |
| 5.42%         | 0.09     | 19,332,855 | `byRef._M_emplace_uniq`                                            | Open (§2.2 / P2)              |

### 1.3 `gleditor_test` (`gmon.gleditor`, 0.19s total sampled time)

`gleditor_test` reflects unit-test harness overhead only (`0.19s` total; top symbol
`std::vector<PageShaping::GlyphEntry>::_M_realloc_append` at `52.63%` / `0.10s` across 8 calls).
Layout and HarfBuzz/FreeType shaping performance in `src/` must be measured with dedicated probes
(`tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`) rather than unit tests.

______________________________________________________________________

## 2. Structural proposals

### §2.1 P1: Conditional LIFO undo trail & open-addressed VM state in `VortexVM`

**Current structure:** `ChoicePoint` (`apps/common/xanadu/vortex/vortex_vm.hpp:520-543`) and
`VortexVM::pushChoicePointImpl` (`vortex_vm.cpp:1403-1425`) snapshot 10 separate
`std::unordered_map` / `std::unordered_set` containers by value (`cellReferences_`, `streams_`,
`iterators_`, `streamSlots_`, `iteratorSlots_`, `activationFrames_`, `guardStacks_`,
`pendingGuardChecks_`, `guardScopes_`, `mutationGroups_`, plus scalar `streamCursor` and
`mutationCheckpointId`). Additionally, local error-recovery guards in `invokeFrame`
(`vortex_frames.cpp:401-408`), `returnFrame` (`vortex_frames.cpp:625-632`), `CheckClosureCall`
(`vortex_closures.cpp:864-883`), and `executeRestoreBindings` (`vortex_bindings.cpp:238-258`) clone
full maps solely for exception rollback (whereas `CreateClosure` at `vortex_closures.cpp:529-541`
already uses `ArenaSavepoint`).

**Why C++ side tables remain separate from `ArenaManifold`:** VM control structures
(`activationFrames_` `shared_ptr` stacks, `streams_` vector handles, `guardStacks_`,
`pendingGuardChecks_`) manage host C++ lifetimes and non-reentrant execution state that cannot be
serialized into `CellSlot` without violating host memory safety. However, their rollback must stay
synchronized with `ArenaManifold::trail_` (`arena_manifold.hpp:965`,
`arena_manifold.cpp:2361-2384, 2484-2498`).

**Proposed design:**

1. **Two-tier conditional LIFO trail (`std::vector<VMTrailEntry>`):**
   - Mirror `ArenaManifold::trail_` (`if (0 == outstandingMarks_) return;`). When
     `0 == choiceDepth() && 0 == vmSavepointDepth_`, VM mutations bypass trailing completely,
     preventing unbounded trail growth in deterministic workloads (`VLace`, `VPL`, `vagent`).
   - Introduce an RAII `VMSavepoint` (symmetric to `zigzag::ArenaSavepoint`) for `invokeFrame`,
     `returnFrame`, `CheckClosureCall`, and `executeRestoreBindings`. On `VMSavepoint::commit()`,
     decrement `vmSavepointDepth_`, and clear `vmTrail_` **only** when
     `0 == choiceDepth() && 0 == vmSavepointDepth_` (matching `ArenaManifold::discard()` at
     `arena_manifold.cpp:2484-2498`, so nested savepoints never discard enclosing rollback records).
   - Store `std::size_t vmTrailMark` in `ChoicePoint` alongside `zigzag::Mark mark`
     (`vortex_vm.hpp:523`). On `backtrack()`, `abortScopedChoices` (`vortex_vm.cpp:1548-1579`), and
     `abortOwnedMutationGroups` (`vortex_vm.cpp:468-509`), unwind `vmTrail_` in reverse order back
     to `cp.vmTrailMark` strictly **before** calling `core_.arena().release(cp.mark)` so teardown
     never observes reclaimed arena slots.
   - On `cut(cutBarrier)` (`vortex_vm.cpp:1493-1546`), inner choice points `[cutBarrier, end)` are
     popped, **but `vmTrail_` is never reset while `cutBarrier > 0`**, preserving outer undo records
     for `VProlog` `!` and `Vicon` bounded evaluation. `vmTrail_` clears only when
     `0 == choiceDepth() && 0 == vmSavepointDepth_`.
   - On `stealNearRootChoicePoint()` (`vortex_vm.cpp:1604-1618`), workers in
     `or_parallel.cpp:392-444` construct fresh `ArenaManifold` and `VortexVM` instances using scalar
     spark metadata only; the victim simply pops `choiceStack_.front()` and advances its root
     watermark, leaving dead prefix trail entries inert until turn end (matching
     `ArenaManifold::evictOldestMark()` at `arena_manifold.cpp:3453-3460`).
   - **Untrailed control state:** `resumeValues_` and `resumeTokenIds_` (`vortex_vm.hpp:836-837`)
     remain explicitly untrailed VM control state; only token references in `cellReferences_`
     backtrack.
1. **Open-addressed flat hash maps for `VortexVM` per-cell tables:**
   - Replace `std::unordered_map<CellRef, ...>` (`cellReferences_`, `streams_`, `iterators_`,
     `opcodeMap_`, `cursorPC_`) with open-addressed linear-probing tables with generation tags for
     $O(1)$ reset. Raw `CellRef`-indexed dense vectors are **prohibited** because R8 `ephemeralBit`
     (`0x8000'0000U`, `arena_manifold.hpp:20-29`) and sparse synthetic unit-test `CellRef` values
     would cause gigabyte allocations.

**Pricing:**

- `VMTrailEntry`: `24 bytes` using a tagged union with explicit `containerTag` discriminator
  (`uint8_t containerTag`, `uint8_t opKind`, 2-byte pad, `CellRef key` [4B], and an aligned 16-byte
  union holding `uint64_t` / `CellRef` / `std::shared_ptr` with manual destructor dispatch keyed on
  `containerTag`), bounded by existing `FrameLimits` / `VortexVM` cycle budgets.
- Eliminates 10 `std::_Hashtable::_M_assign` heap-node copies per `ChoicePoint` and per
  `invokeFrame`/`returnFrame` call.

______________________________________________________________________

### §2.2 P2: `VirtualMemoryArena`-backed `byRef` & open-addressed `structureBirths_` in `Manifold`

**Current structure:** `Manifold::byRef` (`apps/common/xanadu/zigzag/manifold.hpp:903`,
`std::unordered_map<CellRef, std::uint32_t>`) and `Manifold::structureBirths_` (`manifold.hpp:914`,
`std::unordered_map<std::uint32_t, StructureBirthInfo>`) key on `uint32_t` operation indices.
`zigzag_test` spends ~27% of total CPU time in `_M_rehash`, `_M_emplace_uniq`, and `contains` on
these two maps.

**Proposed design:**

1. **`byRef` (`opIndex -> dense`):**
   - For sequential `Store` spools (`Store::rebuildManifoldFromIndex`, `store.cpp:529-542`),
     `opIndex` is dense in `[1, opCount]`. Use `VirtualMemoryArena`
     (`apps/common/xanadu/virtual_memory_arena.hpp:24-71`, already used by `SegmentedOpsSpool`) or a
     `std::vector<std::uint32_t>` pre-sized via a capacity hint from `Store` for dense folds,
     falling back to an open-addressed flat hash map only if an arbitrary sparse `opIndex` (>
     `2 * opCount` or non-contiguous test fixture) is passed to `applyStructure()`.
1. **`structureBirths_` (`opIndex -> StructureBirthInfo`):**
   - Only `Make` ops (`MakeCell`, `MakeSlice`, `MakeXanadoc`) insert into `structureBirths_`,
     whereas `SetLink`, `SetValue`, and `Splice` do not. Because `StructureBirthInfo`
     (`manifold.hpp:213-220`) is 40 bytes and sparse relative to total ops, use a **flat
     open-addressed hash table** (preserving `std::optional` / occupancy tag, zero sentinel keys)
     rather than a dense array.

**Pricing:**

- `byRef` dense table: `4 bytes * opCount` committed on demand (4 KiB per 1,024 ops), 0 heap node
  allocations, $O(1)$ direct load instead of hash + pointer chase.
- `structureBirths_` open-addressed slot: `48 bytes` (`StructureBirthInfo` [40B] + key [4B] +
  state/hash [4B]), ~1.5x–2x load factor, zero per-insert heap allocations.

______________________________________________________________________

### §2.3 P3: Proxy-first fast path in `ArenaManifold::linked`

**Current structure:** `ArenaManifold::linked`
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1625-1682`) was called `243.8M` times (`6.02%`) in
`xuzz_test`. Note that `ArenaManifold::denseOf(from)` (`arena_manifold.cpp:280-292`,
`arena_manifold.hpp:22-23`) is already $O(1)$ bitwise arithmetic (`ref & ~ephemeralBit`) for
ephemeral cells, not a hash lookup. However, on every ephemeral cell lookup, line 1632 calls
`isQuoteOccurrence(from)` (a hash lookup in `occurrenceToCanonical_`), and then runs a `DimLink`
scan (`lines 1634-1641`) that is **byte-for-byte identical** to the default scan at
`lines 1674-1681`. Because quote occurrences are minted with `ValueKind::None`
(`arena_manifold.cpp:570`), they never carry `ValueKind::ExternRef`.

**Proposed design:**

- Reorder `ArenaManifold::linked` (`arena_manifold.cpp:1632-1673`) to inspect
  `slots_[d].valueKind == static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef)` first (which is
  already how `isProxy` identifies proxies at `arena_manifold.cpp:555-556`).
- Only when `valueKind == ExternRef` does `linked()` check `proxyShadowedEdges_` /
  `foreignOf(from)`. All non-proxy cells (regular cells and quote occurrences) fall straight into
  the contiguous `DimLink` loop (`lines 1674-1681`) without touching `occurrenceToCanonical_`.
- **No `CellSlot::flags` bit-packing** (preserves House Style Rule 2 and `sizeof(CellSlot) == 32` at
  `manifold.hpp:357`), and **no `dimOrdinal` caching** (preserves R12 dimension uniformity; typical
  degree $N \le 6$ favors linear scan over binary search).

**Pricing:**

- Zero extra bytes; eliminates 1 hash lookup (`occurrenceToCanonical_`) on >99% of `243.8M`
  `linked()` calls.

______________________________________________________________________

### §2.4 P4: Inline `SegmentedOpsSpool::get(uint32_t)` & optional `LTO=1`

**Current structure:** `SegmentedOpsSpool::get(std::uint32_t index)`
(`apps/common/xanadu/segmented_ops_spool.cpp:397-409`) was called `2.12B` times (`5.56%` / `1.46s`)
in `xuzz_test` due to cross-TU call overhead. `VirtualMemoryArena::base()`
(`virtual_memory_arena.hpp:97`) and `SegmentedOpsSpool::size()` (`segmented_ops_spool.hpp:290`) and
`rawOps()` (`segmented_ops_spool.hpp:392-394`) are already public and inline in their headers.

**Proposed design:**

1. Move `SegmentedOpsSpool::get(std::uint32_t)` (mutable and const), `Manifold::slot()`
   (`manifold.hpp:369`, `manifold.cpp:136-139`), and `ArenaManifold::slot()` into their headers as
   `inline` methods. `SegmentedOpsSpool::get(const MicroversionId &)` remains out-of-line in
   `segmented_ops_spool.cpp` to keep hash/bencode dependencies out of the header.
1. Add an optional `LTO=1` (`-flto=thin` under Clang, `-flto=auto` under GCC) build flag in
   `Makefile` for release builds.

**Pricing:**

- Zero runtime memory cost; eliminates `2.12B` cross-TU calls in `xuzz_test` (~1.4s saved).

______________________________________________________________________

### §2.5 P5: Zero-allocation `VortexCore::wingOf` & direct `OpcodeKind` lookup

**Current structure:** `VortexVM::getOpcodeKind` (`1.98%`, `11.16M` calls) and `VortexCore::wingOf`
(`0.50%`, `7.40M` calls) slow down hyperstructural dispatch:

- `VortexCore::wingOf` (`apps/common/xanadu/vortex/vortex_core.cpp:382-398`) allocates a heap
  `std::vector<CellRef>` via `std::ranges::to<std::vector>()` on every `inputsOf`/`outputsOf` call.
- `VortexVM::getOpcodeKind` (`vortex_vm.cpp:576-591`) probes 4 separate `std::unordered_map`
  containers (`cellReferences_`, `streamSlots_`, `iteratorSlots_`, `opcodeMap_`) before checking the
  cell.

**Proposed design:** Preserve pure hyperstructural execution over ZigZag ranks (`d.spin`, `d.grab`,
`d.step`) and preserve `ExecutionResult.failingClause` (`vortex_vm.cpp:761-765`):

1. Refactor `VortexCore::wingOf` (and `VortexVM::inputsOf`/`outputsOf` in
   `vortex_frames.cpp:326-335`) to return an `OperandList` hybrid small-vector
   (`gleditor::cpp26::inplace_vector<CellRef, 8>` via `<gleditor/cpp26_inplace_vector.hpp>` inline
   buffer [40B] + `std::vector<CellRef>` heap spill [24B], exposing `operator[]`, `size()`,
   `begin()`, `end()`). Common opcodes ($\le 8$ operands) incur 0 heap allocations, while variadic
   `VProlog`/`VLace`/`VPL`/`vagent` calls ($> 8$ operands) spill seamlessly to `std::vector` with no
   hard ceiling (Rule 4).
1. Maintain `opcodeMap_` in an open-addressed table (via P1) and check `arena_.contains(cell)` +
   `opcodeMap_` directly when `cell` is known to be an instruction cursor rather than probing 4 hash
   maps sequentially.

**Pricing:**

- `OperandList`: `64 bytes` on stack (`40B` inline + `24B` spill header), 0 heap allocations for
  $\le 8$ operands, normal heap allocation only when $> 8$.

______________________________________________________________________

## 3. Proposal Register

| ID   | Files Touched                                                                                                      | Summary                                                                                                   | Why Not Caller / Vortex                                                                                              | Cost                                                                                 | Fallback                                                                |
| ---- | ------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ | ----------------------------------------------------------------------- |
| `P1` | `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_closures.cpp`, `vortex_bindings.cpp` | Conditional LIFO `vmTrail_` + `VMSavepoint` + open-addressed VM maps                                      | Host C++ lifetimes (`shared_ptr`, `vector`) cannot live in `CellSlot`; rollback must sync with `ArenaManifold::Mark` | `24B`/trail entry; 0 allocs on deterministic path (`choiceDepth() == 0`)             | Keep maps with upfront `reserve()` + `VMSavepoint` single-entry guards  |
| `P2` | `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `store.cpp`                                                        | `VirtualMemoryArena` dense `byRef` + flat open-addressed `structureBirths_`                               | `Manifold` is the C++ R7 replay fold over `CompactOpNode`                                                            | `4B`/op committed on demand for `byRef`; `48B`/slot for `structureBirths_`           | Pre-`reserve()` existing `unordered_map`s in `rebuildManifoldFromIndex` |
| `P3` | `apps/common/xanadu/zigzag/arena_manifold.cpp`                                                                     | Check `ExternRef` before `isQuoteOccurrence` in `linked()`                                                | Low-level CSR edge traversal in C++ `ArenaManifold`                                                                  | `0B` extra; removes 1 hash lookup on non-proxy `linked()` calls                      | Keep existing order                                                     |
| `P4` | `apps/common/xanadu/segmented_ops_spool.{hpp,cpp}`, `manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `Makefile`   | Inline `SegmentedOpsSpool::get(uint32_t)`, `Manifold::slot()`, `ArenaManifold::slot()` + optional `LTO=1` | Header/build visibility of C++ spool/slot accessors                                                                  | `0B` RAM; slight link-time increase when `LTO=1`                                     | `LTO=1` only without header changes                                     |
| `P5` | `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`, `vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`                      | `OperandList` (`inplace_vector<CellRef, 8>` + heap spill) in `wingOf` + streamlined `getOpcodeKind`       | Operand fetching & opcode dispatch in C++ host VM                                                                    | `64B` stack per `wingOf`; 0 heap allocs for $\le 8$ operands; normal spill for $> 8$ | Keep `std::vector` return with `reserve(4)`                             |

______________________________________________________________________

## 4. Refused Alternatives

1. **Ring-buffer undo trail for `VortexVM`:** Rejected because FIFO eviction corrupts outer
   choice-point undo state or imposes an arbitrary depth ceiling (violating Rule 4), and $O(1)$
   reset on inner `cut(cutBarrier > 0)` destroys enclosing `VProlog` / `Vicon` backtracking state.
1. **LMDB-cached `Manifold` checkpoints in `LMDBContentCache`:** Rejected because (a) R9
   ([store-slice-convergence.md](store-slice-convergence.md)) requires cold folds to replay from
   `ancestralPath` with `verifyAgainstFullRebuild` honesty; (b) OSMIC history is a branching DAG,
   not a linear chain; (c) `LMDBContentCache` (`lmdb_cache.hpp:93`) holds security-isolated,
   Merkle-verified BitTorrent content/CEKs (`1 GiB` map) rather than volatile derived views; and (d)
   `ArenaManifold::PrefixSnapshot` (`arena_manifold.cpp:3541`) lacks `byRef` / `structureBirths_`.
1. **Cached `dimOrdinal` / direct dimension array in `ArenaManifold`:** Rejected because R12
   ([store-slice-convergence.md](store-slice-convergence.md)) forbids privileged dimensions, and
   cells typically have $\le 6$ dimensions where linear scan beats binary search.
1. **Monolithic pre-lowered `DecodedOp` linear bytecode cache with global epoch invalidation:**
   Rejected because (a) it abandons hyperstructural ZigZag execution (Rule 2); (b) any data-cell
   edit (`New`, `Link`, `Splice`) would thrash the global epoch; (c) `vagent` metaprogramming
   mutates code graphs dynamically; and (d) omitting `CellRef opcode` breaks `failingClause`
   diagnostics.
1. **Persistent functional maps (HAMT/Immer) for `ChoicePoint`:** Rejected because pointer-chasing
   tree nodes add allocation and cache-miss overhead on every deterministic step compared to an
   in-place open-addressed map with a conditional LIFO trail.

______________________________________________________________________

## 5. Open Questions

| ID     | Question                                                                                                     | Default / Scope Boundary                                                                                                                                                                                            |
| ------ | ------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `U-P1` | Should `P1`'s `vmTrail_` share a unified `Trail<T>` template with `ArenaManifold::trail_`?                   | Default: keep `vmTrail_` inside `VortexVM` keyed to `ArenaManifold::Mark::serial` first; extract a shared `Trail` helper only if a third trailed subsystem appears.                                                 |
| `U-P2` | What threshold triggers `P2`'s fallback from dense `byRef` vector to open-addressed map for synthetic tests? | Default: use dense `VirtualMemoryArena` when `opIndex < 2 * std::max<std::size_t>(opCount, 4096)`, else overflow into the open-addressed fallback.                                                                  |
| `U-P3` | Should `P5`'s `wingOf` return `OperandList` or accept a `function_ref<void(CellRef)>` visitor?               | Default: return `OperandList` (`gleditor::cpp26::inplace_vector<CellRef, 8>` inline + `std::vector<CellRef>` heap spill for $> 8$ operands) for drop-in `inputs[i]` / `outputs[i]` compatibility with zero ceiling. |

______________________________________________________________________

## 6. Recommended execution order

1. **`P4` & `P3`** (`SegmentedOpsSpool::get` inline + `ArenaManifold::linked` proxy-first check) —
   minimal diff, zero memory overhead, removes `2.12B` calls and `243M` redundant hash probes.
1. **`P2` & `P5`** (`Manifold` dense `byRef` / open-addressed `structureBirths_`, `wingOf`
   `OperandList`) — eliminates `Manifold` rehash storms and `wingOf` heap allocations.
1. **`P1`** (conditional LIFO `vmTrail_` + `VMSavepoint` + open-addressed VM maps) — largest
   remaining CPU ceiling in `xuzz_test`.
