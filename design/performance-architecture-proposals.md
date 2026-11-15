# Performance architecture proposals (medium-to-large structural changes)

This document captures medium-to-large performance proposals derived from `-O3 -g -pg` (`gprof`)
profiling of the full test suite (`gleditor_test`, `zigzag_test`, `xuzz_test`) in
`/data/git/gleditor-restore`, after the low-hanging fixes in commit `5d604feb` (eliminating full-map
copies of `VortexVM::cursorPC_` and duplicate `structureBirths_` lookups in
`Manifold::applyStructure`).

## 1. Observed `gprof` baseline (pre-`5d604feb`)

The three test binaries were compiled with `DEBUG_OPTS="-O3 -g -pg"` and run headlessly.

### `xuzz_test` (`gmon.out`, ~26s total sampled time)

| % time                | Self (s) | Calls         | Symbol                                                                                              | Status after `5d604feb`                       |
| --------------------- | -------- | ------------- | --------------------------------------------------------------------------------------------------- | --------------------------------------------- |
| 35.06%                | 9.20     | 774,792       | `std::_Hashtable<uint, pair<const uint, optional<uint>>>::_M_assign` (`VortexVM::cursorPC_` copies) | Fixed (`cursorPC_` single-entry backup)       |
| 6.25%                 | 1.64     | 59,250,397    | `zigzag::Manifold::applyStructure(uint32_t, const CompactOpNode&)`                                  | Partially reduced (`try_emplace`)             |
| 6.02%                 | 1.58     | 243,788,510   | `zigzag::ArenaManifold::linked(CellRef, DimRef, DimVector) const`                                   | Open (Proposal 3)                             |
| 3.35% + 2.21%         | 1.46     | 2,120,764,009 | `xanadu::SegmentedOpsSpool::get(uint32_t)` (mutable + const)                                        | Open (Proposal 4)                             |
| 2.13%                 | 0.56     | 67,479,244    | `zigzag::Manifold::linkFor(uint32_t, uint32_t)`                                                     | Open (Proposal 2 / 3)                         |
| 2.02%                 | 0.53     | 728,075       | `~unordered_map<uint, optional<uint>>()` (`cursorPC_` teardown)                                     | Fixed                                         |
| 1.98%                 | 0.52     | 11,160,354    | `zigzag::vortex::VortexVM::getOpcodeKind(uint32_t) const`                                           | Open (Proposal 5)                             |
| 1.56% + 1.56% + 1.45% | 1.20     | 139,455,559   | `std::_Hashtable` ops on `Manifold::byRef` and `Manifold::structureBirths_`                         | Partially reduced (`try_emplace`; Proposal 2) |
| 0.69% + 0.53%         | 0.32     | 675,006       | `_M_allocate_node` / `_M_deallocate_node` on `activationFrames_`                                    | Open (Proposal 1)                             |
| 0.57%                 | 0.15     | 2,535,901     | `zigzag::vortex::VortexVM::inputValue(uint32_t) const`                                              | Open (Proposal 5)                             |
| 0.50%                 | 0.13     | 7,402,292     | `zigzag::vortex::VortexCore::wingOf(uint32_t, DimVector) const`                                     | Open (Proposal 5)                             |

### `zigzag_test` (`gmon.zigzag`, ~1.50s total sampled time)

| % time        | Self (s) | Calls      | Symbol                                                             | Status after `5d604feb`           |
| ------------- | -------- | ---------- | ------------------------------------------------------------------ | --------------------------------- |
| 31.93%        | 0.53     | 19,461,525 | `zigzag::Manifold::applyStructure(uint32_t, const CompactOpNode&)` | Partially reduced                 |
| 7.23%         | 0.12     | 22,307,705 | `zigzag::Manifold::linkFor(uint32_t, uint32_t)`                    | Open (Proposal 2 / 3)             |
| 7.23%         | 0.12     | 161,980    | `std::_Hashtable<uint, pair<const uint, uint>>::_M_rehash`         | Open (Proposal 2)                 |
| 6.63% + 6.63% | 0.22     | 25,973,736 | `structureBirths_.contains` + `_M_emplace_uniq`                    | Partially reduced (`try_emplace`) |
| 5.42%         | 0.09     | 78,449,357 | `xanadu::SegmentedOpsSpool::get(uint32_t) const`                   | Open (Proposal 4)                 |
| 5.42%         | 0.09     | 19,332,855 | `byRef._M_emplace_uniq`                                            | Open (Proposal 2)                 |

### `gleditor_test` (`gmon.gleditor`, 0.19s total sampled time)

`gleditor_test` had minimal total CPU time (`0.19s`); its top entry was
`std::vector<PageShaping::GlyphEntry>::_M_realloc_append` (`52.63%`, `0.10s` across 8 calls). No
structural bottleneck was observed in `src/` compared to `apps/common/xanadu/`.

______________________________________________________________________

## 2. Structural proposals

### Proposal 1: Trail-based undo & dense per-cell state in `VortexVM` (Large)

**Current structure (`apps/common/xanadu/vortex/vortex_vm.hpp`, `vortex_vm.cpp`,
`vortex_frames.cpp`, `vortex_closures.cpp`):**

- `ChoicePoint` (`vortex_vm.hpp:520-543`) and `VortexVM::pushChoicePointImpl`
  (`vortex_vm.cpp:1403-1425`) snapshot 10 separate `std::unordered_map` / `std::unordered_set`
  containers by value (`cellReferences_`, `streams_`, `iterators_`, `streamSlots_`,
  `iteratorSlots_`, `activationFrames_`, `guardStacks_`, `pendingGuardChecks_`, `guardScopes_`,
  `mutationGroups_`).
- `invokeFrame` (`vortex_frames.cpp:401-408`), `returnFrame` (`vortex_frames.cpp:620-625`), and
  closure opcodes (`vortex_closures.cpp`) also copy several of these full maps on entry/exit for
  exception rollback.

**Proposed change:**

1. **WAM-style mutation trail (ring-buffered or chunked undo log):** Instead of copying entire maps
   at `pushChoicePointImpl` / `invokeFrame` / `returnFrame`, store only a stack/trail watermark
   (`std::size_t trailMark`) in `ChoicePoint` and frame savepoints. Route VM state mutations through
   small helpers that append `{ContainerTag, CellRef key, OldValue}` entries to a contiguous undo
   trail (`std::vector` or bounded ring-buffer slab). On `backtrack()` or exception rollback, unwind
   entries back to `trailMark` in reverse order.
1. **Dense flat indexed tables for VM per-cell state:** Replace `std::unordered_map<CellRef, ...>`
   for `cellReferences_`, `streams_`, `iterators_`, `opcodeMap_`, and `cursorPC_` with flat
   generation-stamped tables or open-addressed flat maps keyed by `CellRef`.

**Leveraging existing codebase patterns:**

- **Ring-buffer / slab trail:** Similar to `NetStreamRing`
  (`apps/common/xanadu/zigzag/net_lens.hpp:75-92`) and `StreamBufferGL`
  (`include/gleditor/render/gl/stream_buffer.hpp`), a fixed-capacity ring buffer or contiguous
  arena-backed trail avoids per-node heap allocations during execution and resets in $O(1)$ on
  `cut()` / `clearChoicePoints()`.

**Risks & trade-offs:**

- `stealNearRootChoicePoint()` (`vortex_vm.cpp:1604`) for OR-parallelism (`or_parallel.cpp:224`)
  moves a near-root `ChoicePoint` to a worker VM. A trail-based design must either materialize a
  delta snapshot only when stealing (rare) or keep OR-parallel stealing reconstructing state from
  the root-to-choice trail slice.
- Every direct write to `cellReferences_`, `streams_`, etc. across `vortex/` must go through
  trail-recording mutators.

______________________________________________________________________

### Proposal 2: Flat open-addressed / paged indexed tables in `zigzag::Manifold` (Medium–Large)

**Current structure (`apps/common/xanadu/zigzag/manifold.hpp`, `manifold.cpp`):**

- `Manifold::byRef` (`std::unordered_map<CellRef, std::uint32_t>`, `manifold.hpp:903`) maps every
  operation index in a cell's R7 chain to its dense `slots` index.
- `Manifold::structureBirths_` (`std::unordered_map<std::uint32_t, StructureBirthInfo>`,
  `manifold.hpp:914`) maps every `Make` operation index to its birth metadata.
- Every `MakeCell`, `SetLink`, `SetValue`, and `Splice` inserts into `std::unordered_map`, causing
  millions of heap node allocations and `_M_rehash` storms (~27% of `zigzag_test`).

**Proposed change:**

1. **Flat open-addressed hash table with generation-epoch clear (or two-level paged flat array):**
   - Keys (`CellRef` / `opIndex`) are `uint32_t` operation indices, typically dense in
     `[1, opCount]` for real stores, though synthetic unit tests can pass sparse `uint32_t` values.
   - A **two-level paged flat array** (e.g., 1024-entry pages matching `SegmentedOpsSpool`'s
     1024-node piece cadence, lazily allocated per high-bit page) gives $O(1)$ direct array lookup
     with zero hashing and zero rehash for dense stores, while staying memory-safe for sparse
     synthetic IDs.
   - Alternatively, a **flat open-addressed power-of-two hash table** (`std::vector<Slot>` with
     Robin Hood or linear probing and a `uint32_t generation_` tag per slot so `clear()` is $O(1)$
     without zeroing) works uniformly for both dense and sparse `uint32_t` keys.
1. **Capacity hints from `Store`:** Pass `index` / estimated cell count from
   `Store::rebuildManifoldFromIndex` (`store.cpp:529`) to pre-size `byRef` / `structureBirths_`.

**Risks & trade-offs:**

- A two-level paged array trades a tiny page-directory indirection for zero hash collisions; an
  open-addressed flat table is simpler to drop in as a `std::unordered_map` replacement if key
  ranges in tests are completely arbitrary (e.g. `0x7FFF'FFFE`).

______________________________________________________________________

### Proposal 3: LMDB-backed Manifold checkpoints & fast-path `ArenaManifold::linked` (Medium–Large)

**Current structure (`apps/common/xanadu/store.cpp`, `arena_manifold.cpp`):**

- `Store::rebuildManifoldFromIndex` (`store.cpp:529-542`) walks from the root on every cold fold
  (`OsmicWalker::walkAncestral`) and calls `folded.compact()` at the end.
- `ArenaManifold::linked` (`arena_manifold.cpp:1625-1665`) was called `243.8M` times (`6.02%`) in
  `xuzz_test`. On every call it performs a `denseOf(from)` hash lookup, checks
  `isQuoteOccurrence(from)` and `isProxy(from)` (each doing hash lookups), and linearly scans
  `cell.linkCount` edges in `links_`.

**Proposed change:**

1. **LMDB-cached `Manifold` checkpoints (reusing `LmdbCache` pattern):**
   - `LmdbCache` (`apps/common/xanadu/lmdb_cache.hpp`) already maintains zero-copy `MDB_env` + named
     `MDB_dbi` B+trees (`dbi_pieces_`, `dbi_vspans_`, `dbi_ext_spans_`, `dbi_ceks_`) in
     `$XDG_CACHE_HOME` with fixed-layout binary keys (`PieceKey`, `VSpanKey`, `ExtSpanKey`).
   - Add an optional `MDB_dbi` (using `MDB_INTEGERKEY` / `{StoreId, opIndex}` keys) storing
     serialized `Manifold` checkpoints (e.g. every $K = 4096$ structure ops or at named branch
     heads), mirroring how `encodeArenaPrefix` / `ArenaManifold::PrefixSnapshot`
     (`arena_manifold.cpp:3541`) already serializes `slots`/`links_`/`content` snapshots. Cold folds
     load the nearest ancestor checkpoint from LMDB and replay only the tail delta ops.
1. **`ArenaManifold::linked` fast path:**
   - Encode `isQuoteOccurrence` and `isProxy` as bitflags in `CellSlot::flags` (already 1 byte in
     `CellSlot`), eliminating separate hash/set checks on non-proxy, non-quote cells.
   - Keep each cell's `DimLink` run sorted by `DimRef` (or cache a small dense `dimOrdinal` per
     dimension) so cells with many dimensions use binary search or direct indexing instead of linear
     scan.

**Risks & trade-offs:**

- LMDB checkpoints must be keyed by a content-derived `Store`/spool fingerprint (or invalidated on
  segment/format changes) so stale cache entries are never loaded across `CompactOpNode` or
  `CellSlot` layout changes.

______________________________________________________________________

### Proposal 4: Inline `SegmentedOpsSpool::get(uint32_t)` & optional `ThinLTO` (Medium)

**Current structure (`apps/common/xanadu/segmented_ops_spool.hpp`, `segmented_ops_spool.cpp`):**

- `SegmentedOpsSpool::get(std::uint32_t index)` (`segmented_ops_spool.cpp:397-409`) is a 4-line
  bounds check + pointer index into `arena.base()`, but it is defined out-of-line in `.cpp`.
- In `xuzz_test`, it was called **2.12 billion times** (`5.56%` / `1.46s` self time) solely in
  cross-TU call/return overhead.

**Proposed change:**

1. Move `SegmentedOpsSpool::get(std::uint32_t)` (and `SegmentedOpsSpool::size()` /
   `ArenaManifold::slot()`) into their headers as `inline` methods (exposing `base_` on `MmapArena`
   or caching `const CompactOpNode *nodesBase_` in `SegmentedOpsSpool`).
1. Add an optional `LTO=1` (`-flto=thin` when `CXX` is Clang) knob in `Makefile` for release builds
   so hot cross-TU helpers across `xanadu/`, `zigzag/`, and `vortex/` inline automatically.

**Risks & trade-offs:**

- Minimal code risk; `LTO=1` increases release link time and memory usage during linking, so keeping
  it opt-in (or release-only, off in `DEBUG=1`) preserves fast incremental dev builds.

______________________________________________________________________

### Proposal 5: Pre-lowered linear instruction cache in `VortexVM` / `VortexCore` (Medium)

**Current structure (`apps/common/xanadu/vortex/vortex_vm.cpp`, `vortex_core.cpp`):**

- `VortexVM::getOpcodeKind` (`1.98%`, `11.16M` calls), `VortexVM::inputValue` (`0.57%`),
  `VortexCore::wingOf` (`0.50%`, `7.40M` calls), and `VortexVM::frameDescriptor` (`0.38%`)
  re-traverse ZigZag links (`inputsOf`, `outputsOf`, `wingOf`) and query `opcodeMap_` on every VM
  step.

**Proposed change:**

- Compile/lower a `VortexCore` opcode graph lazily on first execution into a flat vector of resolved
  `DecodedOp` structs (`OpcodeKind`, continuation `CellRef` / dense index, pre-resolved input/output
  `CellRef` spans, cached `FrameDescriptor` pointer), keyed by a `Manifold` mutation epoch so any
  structural edit invalidates the decoded cache.

**Risks & trade-offs:**

- Highest implementation complexity among the VM optimizations; must invalidate cleanly if
  self-modifying or dynamic code-cell tests mutate program cells after execution starts.

______________________________________________________________________

## 3. Recommended execution order

1. **Proposal 4** (inline `SegmentedOpsSpool::get` + optional `LTO=1`) — smallest diff, immediately
   eliminates 2.1B+ cross-TU calls.
1. **Proposal 2 & Proposal 3 (`ArenaManifold::linked` fast path)** — localized to `zigzag/`, high
   payoff for both `zigzag_test` and `xuzz_test`.
1. **Proposal 1** (trail-based `VortexVM` undo + flat VM maps) — largest remaining CPU win in
   `xuzz_test` after `cursorPC_`.
1. **Proposal 3 (LMDB Manifold checkpoints)** & **Proposal 5** (`VortexVM` pre-lowered instruction
   cache) — larger architectural investments once flat tables and trail undo are in place.
