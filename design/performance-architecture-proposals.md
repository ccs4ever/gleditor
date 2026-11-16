# Performance architecture proposals (v8)

This document records medium-to-large structural performance proposals derived from `-O3 -g -pg`
(`gprof`) profiling of the full test suite (`gleditor_test`, `zigzag_test`, `xuzz_test`) in
`/data/git/gleditor-restore`, following the low-hanging fixes in commit `5d604feb` (eliminating
full-map copies of `VortexVM::cursorPC_` and duplicate `structureBirths_` lookups in
`Manifold::applyStructure`).

**Owner decision:** P1-P5, including the release ThinLTO+PGO pipeline, are accepted. Execution
follows the [implementation plan](performance-architecture-implementation-plan.md); correctness and
measurement gates remain part of that acceptance.

Foundational invariants referenced below live in
[store-slice-convergence.md](store-slice-convergence.md) (R2, R7, R8, R9, R12),
[vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md),
[vql-performance-spike.md](vql-performance-spike.md), and
[vagent-metaprogramming.md](vagent-metaprogramming.md).

## 1. Observed `gprof` baseline (pre-`5d604feb`)

The three test binaries were compiled with `DEBUG_OPTS="-O3 -g -pg"` and run headlessly.

These instrumented results identify candidates, not release speedups. `-pg` adds function-entry
profiling work and can alter optimization decisions. Self time includes useful function work; call
counts do not establish allocations or removable call overhead. All P1-P5 benefits below remain
unmeasured on the ordinary release build after `5d604feb`.

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
     spark metadata and, for `ShadowDeltaPrefix`, an arena prefix snapshot; the victim pops
     `choiceStack_.front()` and advances its root watermark (matching
     `ArenaManifold::evictOldestMark()` at `arena_manifold.cpp:3453-3460`). Before returning from
     the steal, destroy owning payloads below the minimum watermark of **all** surviving choices and
     local savepoints (or the trail end if none survive). Mark those entries inactive without
     shifting indices, so live watermarks remain valid. Track active savepoint watermarks
     explicitly; depth alone cannot establish this minimum.
   - **Untrailed control state:** `resumeValues_` and `resumeTokenIds_` (`vortex_vm.hpp:836-837`)
     remain explicitly untrailed VM control state; only token references in `cellReferences_`
     backtrack.
1. **Open-addressed flat hash maps for `VortexVM` per-cell tables:**
   - Replace `std::unordered_map<CellRef, ...>` (`cellReferences_`, `streams_`, `iterators_`,
     `opcodeMap_`, `cursorPC_`) with open-addressed linear-probing tables with explicit occupancy
     and optional generation tags. Generation-only reset is permitted only for trivially
     destructible values (`opcodeMap_`, `cursorPC_`, scalar references). Owning tables (`streams_`,
     `iterators_`) destroy every live payload before reset: $O(N)$ destruction, retaining reusable
     bucket capacity only. Generation wrap requires a full occupancy sweep before reuse; no stale
     generation may become live again. Raw `CellRef`-indexed dense vectors are **prohibited**
     because R8 `ephemeralBit` (`0x8000'0000U`, `arena_manifold.hpp:20-29`) and sparse synthetic
     unit-test `CellRef` values would cause gigabyte allocations.

**Undo ownership and failure contract:**

- Presence is separate from value: a missing map key, an existing unbound `optional<CellRef>`, and a
  bound reference are distinct undo states. Tags describe the container, inverse operation, and
  presence; no reserved `CellRef` represents absence.
- Scalar maps and sets record old values or membership. Shared-handle maps retain exact typed
  handles (`vortex_vm.hpp:382-432, 470-472`), with the first-write rule below for mutable stream and
  iterator payloads. Saving a pointer after mutating its payload cannot restore the old value;
  saving one before every write would force repeated COW at `vortex_streams.cpp:243-250,349-353`.

**Mutable payload snapshot boundaries:**

- Each choice and local savepoint owns a private touched-key set keyed by `(containerTag, CellRef)`
  for `streams_` and `iterators_`. The active boundary is the innermost live choice or local
  savepoint in mark order. Before its first mutation of a key, append an undo record retaining the
  old handle and presence, then mark the key touched, **before** checking payload uniqueness or
  mutating it. Reserve the trail and touched-set capacity before publishing either. On failure,
  unwind the enclosing savepoint; no touched key may suppress an unrecorded snapshot.
- Subsequent mutations of that key under the same boundary do not retain another old handle.
  Existing uniqueness checks still run: aliases such as stream iterator chunks must continue to
  trigger COW. With one live boundary and no additional aliases, N appends take one boundary
  snapshot and at most one snapshot-induced payload copy, followed by ordinary vector growth; rank
  iteration similarly avoids a snapshot-induced copy of its growing `seen` set each step. This is a
  design requirement to verify, not a measured speedup.
- A nested choice/savepoint starts with an empty touched set and its own watermark; even a key
  touched by its parent needs a child snapshot. Child rollback restores its entry state and destroys
  that child's touched set and records. Child commit/cut discards its touched set but retains its
  records whenever an outer boundary can still roll back; it does **not** merge or mutate the
  parent's touched set, so commit needs no allocation. A parent's existing snapshot remains
  sufficient; if the parent never touched that key, its first later write records the then-current
  handle. Reverse replay of all retained records still reaches the parent's entry state. Thus a
  committed child can leave an extra retained version, priced per actual boundary, rather than per
  subsequent parent write.
- Whole-map clear and erasure use the same first-write rule for each affected key, including
  presence. Root stealing drops only the removed boundary's touched set; surviving sets stay
  associated with their original marks. Prefix reclamation uses all live boundary watermarks. When
  no boundary remains, discard all tracking and dead snapshots. Cursor/scalar and immutable
  shared-handle undo may continue to record per mutation; the coalescing rule concerns mutable
  payload identity and contents, not a replacement of the global LIFO order.

**Frame and restoration ownership:**

- `ActivationStack` is a vector, not a single frame handle (`vortex_vm.hpp:518`). Trail individual
  push/pop inverses: undo a push by popping; undo a pop with the retained
  `shared_ptr<const ActivationFrame>`. Keep empty stack entries and their capacity while any
  enclosing mark can restore them. A whole-stack clear logs its removed frames in reverse order.
  This covers `vortex_frames.cpp:477-478, 675-686` without copying a vector into each entry.
- During active marks, erased map entries retain a restorable bucket with explicit absent state.
  Rehash must preserve that state by key; undo never stores pointers into movable buckets. Reserve
  trail records, buckets, and stack capacity before publishing a mutation. If preparation fails,
  leave VM and arena state unchanged; multi-write operations use the enclosing savepoint. Undo
  performs only non-allocating lookup, scalar restoration, handle moves, and stack operations within
  retained capacity. Both savepoint destruction and scoped abort must remain `noexcept`
  (`vortex_vm.cpp:468-509, 1548-1579`). Reclaim retained absent entries after the last mark closes.
- Local savepoints also guard `cursorPC_` and `streamCursor_`. Ordinary choice rollback preserves
  existing cursor routing: select `altOp` after restoring registers rather than indiscriminately
  rolling back all cursors. `opcodeMap_` is not a choice snapshot and stays outside the undo trail.

**Pricing and limits:**

- Target `VMTrailEntry`: `24 bytes` on the supported 64-bit ABI: 8-byte header (container,
  inverse-operation/presence tags, padding, `CellRef key`) and a 16-byte aligned union of scalar
  values or the exact typed shared handles above. Manual construction/destruction uses the tags;
  move and unwind must be `noexcept`. Verify size/alignment during implementation; a larger ABI must
  use its actual `sizeof`, never pack pointers to force this estimate.
- Cost is `sizeof(VMTrailEntry) * retained record capacity`, plus retained map buckets, frame-stack
  capacity, boundary tracking and shared payload lifetimes. A touched-set slot targets 16B (8B key
  plus explicit occupancy/padding); account actual `sizeof(slot) * capacity` and boundary headers.
  At a maximum 0.7 occupancy, capacity is at least `ceil(touchedKeys / 0.7)`; charge growth slack
  too. These are ephemeral host ownership records, not persisted application metadata. Retained
  payload versions are counted once per distinct allocation; first-write coalescing does not make
  their bytes free. It removes whole-map snapshot copies, not allocations for actual frame objects,
  table growth, or trail growth. With no choices **and** no local savepoints, trail recording costs
  zero allocations; deterministic frame calls still open local savepoints and may grow the trail.
- `FrameLimits` (`vortex_vm.hpp:281-286`) and cycle budgets do not independently bound retained
  trail bytes across repeated runs. Add an optional caller-supplied `maxUndoBytes` to P1's VM
  limits; absence means allocator-limited, not an invented fixed ceiling. Charge record/bucket/
  stack/touched-set capacities and boundary headers retained for rollback before growth; shared
  payload bytes remain charged to their existing stream/frame/arena budgets. Exhaustion returns a
  failed `ExecutionResult` with the executing opcode as `failingClause`, after rollback, before
  publishing the refused mutation. Before the next trail growth or budget refusal, compact inactive
  prefixes when no local savepoint is active, rebasing every surviving choice watermark. When local
  savepoints prevent compaction, cleared record capacity remains charged until the outermost local
  savepoint closes; compact there before the next growth. Retained live undo state must never be
  evicted to satisfy a budget. Root eviction promptly destroys dead payloads even when capacity
  cannot yet be reused.
- Prefix compaction costs O(live records + surviving choices); `vector::erase`/moving the suffix
  makes slots reusable but does not shrink charged allocation capacity or return it to the OS.
  Repeated small-prefix compactions may repeat that work; no amortized constant-time claim is made.
  Measure root-steal/growth frequency and bytes moved. A threshold or chunked trail requires its own
  measured case; prompt dead-handle destruction and valid watermarks remain mandatory.

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
     `std::vector<std::uint32_t>` pre-sized via a capacity hint from `Store` for dense folds.
     Address valid operation `opIndex` at zero-based offset `opIndex - 1`; index zero is not a valid
     operation and is refused before subtraction. Use the U-P2 default consistently:
     `opIndex < 2 * std::max<std::size_t>(opCount, 4096)` selects dense storage; indices at or above
     that threshold use the sparse overflow map. Grow dense capacity on demand within that
     threshold.
   - Dense entries have a separate occupancy bitmap: the index is valid only when its presence bit
     is set. `denseOf` still returns `std::optional<std::uint32_t>`; dense slot zero remains valid
     (`manifold.cpp:126-134, 477-492`). Out-of-range or unset entries return `nullopt`; lookup must
     also consult the sparse overflow map if the mixed representation is used. Clearing/rebuilding
     resets both mappings and occupancy; no reserved dense index encodes absence.
1. **`structureBirths_` (`opIndex -> StructureBirthInfo`):**
   - Only `Make` ops (`MakeCell`, `MakeSlice`, `MakeXanadoc`) insert into `structureBirths_`,
     whereas `SetLink`, `SetValue`, and `Splice` do not. Because `StructureBirthInfo`
     (`manifold.hpp:213-220`) is 40 bytes and sparse relative to total ops, use a **flat
     open-addressed hash table** (preserving `std::optional` / occupancy tag, zero sentinel keys)
     rather than a dense array.

**Pricing:**

- `byRef` dense table: `4 * capacity` bytes plus `ceil(capacity / 8)` occupancy bytes (`capacity`
  initially `opCount`, with no index-zero slot), committed on demand and rounded to mapping page
  boundaries (4 KiB of indices plus 128B logical presence bits per 1,024 ops), 0 heap node
  allocations, $O(1)$ direct load instead of hash + pointer chase.
- `structureBirths_` open-addressed slot: `48 bytes` (`StructureBirthInfo` [40B] + key [4B] +
  state/hash [4B]), capacity 1.5x–2x live entries (occupancy 0.5–0.67), zero per-insert heap
  allocations until table growth. Report growth allocations and peak old-plus-new table bytes too.
- Dense storage is a candidate for folds with enough useful mappings, not a conclusion from
  sequential op IDs. `store.cpp:529-537` folds an ancestral path; text-heavy and branch-heavy stores
  can have large op counts with few useful mappings. Compare those against structure-heavy folds,
  recording mapped entries/capacity, initialization time, page faults, committed/RSS bytes and
  reserved virtual bytes separately. A vector initializes all entries; a reserved virtual arena
  still pays for committed/initialized pages. Keep the sparse fallback until those results justify
  the dense policy and U-P2 threshold.

______________________________________________________________________

### §2.3 P3: Proxy-first fast path in `ArenaManifold::linked`

**Current structure:** `ArenaManifold::linked`
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1625-1682`) was called `243.8M` times (`6.02%`) in
`xuzz_test`. Note that `ArenaManifold::denseOf(from)` (`arena_manifold.cpp:280-292`,
`arena_manifold.hpp:22-23`) is already $O(1)$ bitwise arithmetic (`ref & ~ephemeralBit`) for
ephemeral cells, not a hash lookup. However, on every ephemeral cell lookup, line 1632 calls
`isQuoteOccurrence(from)` (a hash lookup in `occurrenceToCanonical_`), and then runs a `DimLink`
scan (`lines 1634-1641`) that is **byte-for-byte identical** to the default scan at
`lines 1674-1681`. Quote occurrences start with `ValueKind::None` (`arena_manifold.cpp:570`), but
`setValueBits` can change their kind (`arena_manifold.cpp:2135-2145`). `isProxy` explicitly excludes
occurrences (`arena_manifold.cpp:544-556`); construction does not make their value immutable.

**Proposed design:**

- Keep the existing missing-dense/base delegation path unchanged. For an arena slot, inspect
  `slots_[d].valueKind == static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef)` first.
- Non-`ExternRef` slots go directly to the shared local `DimLink` scan. Inside the `ExternRef`
  branch, call `isQuoteOccurrence(from)` **before** any proxy shadow/foreign delegation. An
  occurrence still goes to the local scan, including when its local edge is absent. Only an
  `ExternRef` slot that is not an occurrence follows existing proxy shadow/foreign resolution. This
  preserves the exclusion in `isProxy`; do not call it again and duplicate the checks.
- Preserve local-only occurrence semantics even though `foreignOf` can resolve an occurrence's
  canonical proxy (`arena_manifold.cpp:601-609`). The regression case is an ExternRef-valued
  occurrence whose canonical proxy has a foreign edge: verify both an absent local edge and a
  different local edge, then value changes and mark rollback. Neither may inherit that foreign edge
  through this shortcut.
- **No `CellSlot::flags` bit-packing** (preserves House Style Rule 2 and `sizeof(CellSlot) == 32` at
  `manifold.hpp:357`), and **no `dimOrdinal` caching** (preserves R12 dimension uniformity; typical
  degree $N \le 6$ favors linear scan over binary search).

**Pricing:**

- No new per-slot storage. Removes the occurrence lookup only for slots whose current kind is not
  `ExternRef`; the qualifying fraction of calls is unmeasured. Record that distribution and release
  timings before claiming a net benefit; proxy/ExternRef occurrence calls retain the guard.

______________________________________________________________________

### §2.4 P4: Inline accessors and release ThinLTO + PGO

**Current structure:** `SegmentedOpsSpool::get(std::uint32_t index)`
(`apps/common/xanadu/segmented_ops_spool.cpp:397-409`) was called `2.12B` times (`5.56%` / `1.46s`)
in the instrumented `xuzz_test`. That self time includes checks and address work and does not
isolate cross-TU call overhead or predict the release benefit of inlining.
`VirtualMemoryArena::base()` (`virtual_memory_arena.hpp:97`) and `SegmentedOpsSpool::size()`
(`segmented_ops_spool.hpp:290`) and `rawOps()` (`segmented_ops_spool.hpp:392-394`) are already
public and inline in their headers.

**Proposed design:**

1. Move `SegmentedOpsSpool::get(std::uint32_t)` (mutable and const), `Manifold::slot()`
   (`manifold.hpp:369`, `manifold.cpp:136-139`), and `ArenaManifold::slot()` into their headers as
   `inline` methods. `SegmentedOpsSpool::get(const MicroversionId &)` remains out-of-line in
   `segmented_ops_spool.cpp` to keep hash/bencode dependencies out of the header.
1. Make **ThinLTO and profile-guided optimization (PGO) part of the release-build pipeline**. The
   Clang release configuration combines `-O3`, ThinLTO and a trained optimization profile in the
   final compile/link. Probe the selected compiler/linker and matching profile tools through the
   existing Makefile conventions; do not hardcode a compiler or a language-standard flag. Preserve
   GCC support with its own LTO/PGO pipeline, identifying that as a compiler-specific alternative
   rather than calling GCC LTO ThinLTO. Unsupported configurations must explain which optimization
   is unavailable; an explicit development/portability fallback must not silently masquerade as the
   fully optimized release.
1. Add separate **profile generation, training/merge, and profile-use build stages**. Generate
   instrumentation with the selected compiler's PGO facility, run representative headless workloads,
   merge the resulting profiles with matching tools, then rebuild the release with profile use and
   ThinLTO. Keep generation and use outputs separate, or make all relevant flags/profile identity
   part of dependency tracking so instrumented or stale objects cannot enter the final release.
   Training binaries contain instrumentation; the final optimized release does not collect profiles.
1. Version the training recipe and record source/build identity, compiler/tool versions, target,
   flags, corpus and profile digest. Train store loading/editing, document layout/rendering, ZigZag
   traversal and Vortex/surface-language execution; unit-test frequency alone must not determine
   production hotness. Keep performance-validation inputs separate from training inputs. Reject
   missing, incompatible or stale profiles in the profile-use stage and document retraining after
   relevant source, toolchain, target or workload changes. PGO optimization profiles are a separate
   workflow from the existing coverage-oriented `make profile` targets (`Makefile:1472-1515`);
   `gprof` data is not a PGO input.

**Pricing:**

- No new dynamic data structures; executable-text size can grow with header inlining and affect
  I-cache behavior. Header visibility permits, rather than guarantees, elimination of call sites.
  ThinLTO changes compile/link time and peak build memory. PGO adds an instrumented build, training,
  profile merge/storage and a profile-use rebuild. Price CI/training time, profile bytes and
  retraining alongside final binary size and runtime; none of those deltas is measured here.
- Compare ordinary `-O3 -g` release builds after `5d604feb`: existing out-of-line, header-inline,
  ThinLTO-only, PGO-only, ThinLTO+PGO, and header-inline combinations on identical held-out headless
  workloads. Record compiler/linker versions, flags, build identity, cold/warm conditions, repeated
  timings and spread. Inspect optimization remarks/disassembly for actual inlining; report
  executable-text size, build/link resources, and cycles/instructions when available. Retain the
  fallback unless release benefit justifies these costs; the instrumented 1.46s is not a promised
  saving. ThinLTO+PGO is the requested release configuration; control/fallback builds remain
  available for comparison and portability, and measured regressions require investigation before
  shipping.

______________________________________________________________________

### §2.5 P5: Inline operand buffers & classification-preserving opcode lookup

**Current structure:** `VortexVM::getOpcodeKind` (`1.98%`, `11.16M` calls) and `VortexCore::wingOf`
(`0.50%`, `7.40M` calls) slow down hyperstructural dispatch:

- Nonempty `VortexCore::wingOf` results (`vortex_core.cpp:382-398`) materialize an owning
  `std::vector<CellRef>` via `std::ranges::to<std::vector>()`. Empty wings return `{}` at
  `vortex_core.cpp:394-395` without allocating an operand buffer. The 7.40M call count is not an
  allocation count.
- `VortexVM::getOpcodeKind` (`vortex_vm.cpp:576-591`) probes 4 separate hash containers
  (`cellReferences_`, `streamSlots_`, `iteratorSlots_`, `opcodeMap_`; two maps and two sets) before
  checking the cell.

**Proposed design:** Preserve pure hyperstructural execution over ZigZag ranks (`d.spin`, `d.grab`,
`d.step`) and preserve `ExecutionResult.failingClause` (`vortex_vm.cpp:761-765`):

1. Refactor `VortexCore::wingOf` (and `VortexVM::inputsOf`/`outputsOf` in
   `vortex_frames.cpp:326-335`) to return an `OperandList` hybrid small-vector
   (`gleditor::cpp26::inplace_vector<CellRef, 8>` via `<gleditor/cpp26_inplace_vector.hpp>` inline
   buffer [40B] + `std::vector<CellRef>` heap spill [24B], exposing `operator[]`, `size()`,
   `begin()`, `end()`, `empty()`, `front()`, `back()`, mutable iteration, and a contiguous
   `std::span` view). Common opcodes ($\le 8$ operands) incur 0 heap allocations, while variadic
   `VProlog`/`VLace`/`VPL`/`vagent` calls ($> 8$ operands) spill seamlessly to `std::vector` with no
   hard ceiling (Rule 4).
1. Migrate explicit vector consumers in `vortex_vm.cpp:1745,1807,1892`,
   `vortex_vm.cpp:2938-2987,3315-3421,3701-3776` and `vortex_stdlib.cpp:918, 927` to
   `auto`/`OperandList`. Keep `VortexCore::inputsOf`/`outputsOf` declarations
   (`vortex_core.hpp:286-287`) and VM wrappers consistent. Do not provide implicit conversion to
   `std::vector`: consumers needing owned vectors explicitly construct one from iterators and
   account for that allocation. Audit all callers, including tests and language compilers, when
   implementing; range/span consumers should use the contiguous view. Preserve wing order,
   reference/master resolution, and runtime-slot mapping (`vortex_core.cpp:382-398`,
   `vortex_frames.cpp:326-335`). On the ninth operand, move the inline prefix into the spill vector
   in order; use the spill exclusively thereafter.
   - Cross-module explicit-vector consumers also switch to `auto`/`OperandList`:
     `apps/common/xanadu/vql/compiler.cpp:1392,1410`,
     `apps/common/xanadu/vpl/compiler.cpp:1341,1361`, and `tests/xuzz/vortex_core_test.cpp:353,358`.
     These sites index or iterate operands and do not need an owned-vector conversion. Their
     migration is part of P5, not a later compiler change.
1. Preserve `getOpcodeKind`'s general classification precedence (`vortex_vm.cpp:576-591`):
   reference/stream/iterator registers return `nullopt` before registered opcodes and the numeric
   arena-value fallback. P1 may change map storage but must preserve those semantics. A separate
   direct lookup is deferred under `U-P4` until a maintained instruction-cell precondition is
   demonstrated; reaching a cell through a cursor alone does not prove it is not a register.

**Pricing:**

- `OperandList`: `64 bytes` on stack (`40B` inline + `24B` spill header), 0 heap allocations for
  $\le 8$ operands, normal heap allocation only when $> 8$. This targets operand-buffer allocations,
  not allocations in operand resolution or downstream explicit-vector boundaries. Eight is a
  provisional spill threshold, not a proven optimum. Collect wing arities, empty-wing fraction,
  allocation counts and spills; compare smaller inline buffers and a visitor/span path, including
  return-object copies/moves and stack use. Keep the vector fallback if those release measurements
  do not justify the larger return object.

______________________________________________________________________

## 3. Proposal Register

| ID   | Files Touched                                                                                                                                                                                                                       | Summary                                                                                                    | Why Not Caller / Vortex                                                                                              | Cost                                                                                                                                       | Fallback                                                                         |
| ---- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------- |
| `P1` | `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_closures.cpp`, `vortex_bindings.cpp`, `vortex_streams.cpp`, `vortex_contracts.cpp`                                                                    | Conditional LIFO `vmTrail_` + `VMSavepoint` + open-addressed VM maps                                       | Host C++ lifetimes (`shared_ptr`, `vector`) cannot live in `CellSlot`; rollback must sync with `ArenaManifold::Mark` | `24B` target/record + boundary touched sets + retained payload/capacity costs; no trail when both depths are zero; optional `maxUndoBytes` | Keep maps with upfront `reserve()` + `VMSavepoint` single-entry guards           |
| `P2` | `apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `store.cpp`                                                                                                                                                                         | `VirtualMemoryArena` dense `byRef` + flat open-addressed `structureBirths_`                                | `Manifold` is the C++ R7 replay fold over `CompactOpNode`                                                            | `4B`/op + 1 presence bit/op (page-rounded) for `byRef`; `48B`/slot for `structureBirths_`                                                  | Pre-`reserve()` existing `unordered_map`s in `rebuildManifoldFromIndex`          |
| `P3` | `apps/common/xanadu/zigzag/arena_manifold.cpp`                                                                                                                                                                                      | Check `ExternRef` before `isQuoteOccurrence` in `linked()`                                                 | Low-level CSR edge traversal in C++ `ArenaManifold`                                                                  | `0B` slot storage; skips occurrence lookup only for non-ExternRef values; guard retained otherwise                                         | Keep existing order                                                              |
| `P4` | `apps/common/xanadu/segmented_ops_spool.{hpp,cpp}`, `manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `Makefile`                                                                                                                    | Inline spool/slot accessors + release ThinLTO and trained PGO pipeline                                     | Header/build visibility of C++ spool/slot accessors                                                                  | Text/I-cache, build/link resources, instrumented training, profile storage/merge and rebuild costs                                         | Explicit unprofiled/portable controls; preserve release optimization diagnostics |
| `P5` | `apps/common/xanadu/vortex/vortex_core.{hpp,cpp}`, `vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_stdlib.cpp`, `apps/common/xanadu/vql/compiler.cpp`, `apps/common/xanadu/vpl/compiler.cpp`, `tests/xuzz/vortex_core_test.cpp` | `OperandList` (`inplace_vector<CellRef, 8>` + heap spill) in `wingOf` + classification-preserving dispatch | Operand fetching & opcode dispatch in C++ host VM                                                                    | `64B` stack per `wingOf`; 0 operand-buffer allocs for $\le 8$; spill and explicit vector boundaries priced separately                      | Keep `std::vector` return with `reserve(4)`                                      |

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
| `U-P4` | Can instruction-only dispatch bypass register classification?                                                | Deferred: retain general `getOpcodeKind` semantics. Require measured benefit and an explicit invariant maintained by registration, register writes, and rollback before adding a separate fast path.                |

______________________________________________________________________

## 6. Implementation acceptance gates

These are proposed checks for implementation, not results of this design review.

- P1: compare snapshot and trail behavior for nested commit/rollback, inner cut with an outer
  choice, scoped/mutation-group abort, root stealing, frame push/pop, missing versus unbound
  references, and allocation/budget failure before each mutation. Rollback must allocate nothing.
  Repeated owning-table resets must release payloads; exercise generation wrap and retained buckets.
- P5: compile all programs and tests to catch container-type boundaries; compare operand order and
  values at arities 0, 8, 9, and large variadic wings, including mutable runtime-slot mapping. Check
  register cells whose numeric values overlap opcode ordinals and the numeric fallback.
- P1 payloads: one long-lived choice with N appends/rank advances, then nested commit/rollback, cut
  and root-steal cases. Check contents and compare allocations, copied elements, touched-set growth,
  unique retained payload bytes and peak RSS with existing map snapshots. Without extra
  aliases/boundaries, payload copies induced by snapshots must not grow once per mutation. Exercise
  allocation refusal while preparing the first-write record and touched-set growth.
- P1 compaction: repeated root steals interleaved with trail growth; measure live records moved,
  watermark-rebase work and reusable versus still-allocated capacity.
- P2: compare missing operation refs, valid dense slot zero, sparse overflow, and rebuild/reset
  against the current optional lookup behavior. Measure text-heavy, branch-heavy and structure-heavy
  folds with the density, initialization and virtual/physical memory counters specified in §2.2.
- P3: run the ExternRef-valued occurrence regression in §2.3 and compare the existing and proposed
  `linked()` answers for local, quote, proxy and base cells; measure actual fast-path eligibility.
- P4: verify generation, training/merge and profile-use stages, including missing/stale-profile
  refusal and isolation of instrumented outputs. Confirm ThinLTO and profile use in the final
  compile/link commands. Run the test suite and backend rendering comparisons headlessly on that
  final binary; compare held-out workloads with the release controls in §2.4. Retain raw results,
  profile provenance, inlining/text-size evidence and build/training resource costs.
- P5: measure arity/spills/allocations and compare buffer capacities as specified in §2.5.
- Reprofile after `5d604feb` before accepting performance claims. Report allocation counts, peak
  retained bytes, and headless workload timings; the pre-fix gprof table is not an additive speedup
  forecast. P1/P5 fallbacks remain available if measured benefit does not justify their complexity.

## 7. Recommended execution order

1. **Release baseline and narrow P3/P4 experiments:** establish post-`5d604feb` release timings,
   then add P4's ThinLTO+PGO release pipeline and test its controls, header-inline variants and the
   guarded P3 path. Candidate priority reflects small change size, not a forecast that all
   instrumented calls or self time disappear.
1. **P2/P5 representation experiments:** compare useful-mapping density and operand arity/spill
   distributions before selecting dense storage or an inline capacity. Their fallbacks remain the
   baseline until release measurements support the change.
1. **P1 trail prototype:** the largest ownership/rollback change. First verify boundary-aware COW,
   no-throw unwind, retained-memory accounting and compaction costs against the current snapshots;
   proceed only if those correctness and measured-cost gates pass.
