# Round 1 critique: Codebase Expert (performance-architecture-proposals.md)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 3 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§2, lines 48-210) Missing Proposal Register for C++ engine changes (House Style Rule 6)

Full title: Complete absence of a numbered Proposal Register (`P1..Pn`) for core C++ modifications

**Problem:** The design document proposes medium-to-large structural modifications to existing C++ core engine and runtime files (`apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `segmented_ops_spool.{hpp,cpp}`, `store.cpp`, `lmdb_cache.hpp`, `vortex/vortex_vm.{hpp,cpp}`) without isolating them in a numbered Proposal Register. Standing House Style Rule 6 explicitly mandates:
> "Any change to existing C++ engine, core, stdlib, or runtime files must be isolated in a numbered Proposal Register (`P1`, `P2`, ...) stating exact files touched, why it cannot live in caller/compiler, cost, and fallback."

While sections are informally titled "Proposal 1" through "Proposal 5", none of them provide the required register structure: exact files touched, architectural justification for why the logic cannot reside in callers or compilers, concrete priced costs (bytes, cycles, allocations), and actionable fallbacks if rejected.

**Evidence:**
- Standing House Style Rule 6 in `design/projects/performance-architecture-proposals-review/brief-common.md:30-32` and `design/store-slice-convergence.md`.
- `design/performance-architecture-proposals.md:48-210` introduces engine-level C++ changes across 5 proposals without a unified Proposal Register table or individual proposal entries adhering to Rule 6.

**Fix:** Add a formal Proposal Register section (`P1..Pn`) before §3 containing:
1. Register ID (`P1`, `P2`, etc.).
2. Exact files touched (`apps/common/xanadu/...`).
3. Why the change cannot live in the caller, compiler, or Vortex stdlib.
4. Concrete priced cost (memory footprint, cache lines, cycle overhead, migration cost).
5. Explicit fallback mechanism if the proposal is refused.

---

### 2. (§2, Proposal 3, lines 147-150) Manifold LMDB checkpointing ignores `byRef` micro-history chains

Full title: Serializing only `slots`/`links_`/`content` breaks delta folding because `byRef` is required for R7 micro-history resolution

**Problem:** Proposal 3 asserts that LMDB-cached `Manifold` checkpoints can be implemented by:
> "...mirroring how `encodeArenaPrefix` / `ArenaManifold::PrefixSnapshot` (`arena_manifold.cpp:3541`) already serializes `slots`/`links_`/`content` snapshots. Cold folds load the nearest ancestor checkpoint from LMDB and replay only the tail delta ops."

This premise is fatally flawed for `Manifold`. Unlike `ArenaManifold` (which is an ephemeral scratchpad that does not process `OpKind::Structure` operations), `Manifold` is a persistent replay product. In `Manifold::applyStructure` (`apps/common/xanadu/zigzag/manifold.cpp:420-630`), every subsequent `SetLink`, `SetValue`, and `Splice` operation references past operations on the same cell via `CompactOpNode::sourceOpIndex` (R7 micro-history chains). `Manifold::applyStructure` resolves these subject cells and target cells using `Manifold::byRef` (`std::unordered_map<CellRef, std::uint32_t>`).

If an LMDB checkpoint only serializes `slots`, `links_`, and `content` (as `PrefixSnapshot` does in `arena_manifold.cpp:3541-3580`), `byRef` will be completely empty upon restoring the checkpoint. When `Store::rebuildManifoldFromIndex` replays delta operations from the checkpoint to the target index, the very first operation referencing a cell created prior to the checkpoint will immediately fail with `FoldRefusal::UnknownSubject` (`manifold.cpp:443, 463`) or `FoldRefusal::UnknownTarget` (`manifold.cpp:525`).

**Evidence:**
- `apps/common/xanadu/zigzag/manifold.cpp:516-525`:
  ```cpp
  case xanadu::StructureVerb::SetLink: {
    // The subject is named by the micro-history chain rather than by a field of
    // its own: sourceOpIndex is the previous operation on this same cell, and
    // the chain's first link is the MakeCell whose index *is* the CellRef. See
    // R7, and byRef, which holds every operation in a chain for exactly this.
  ```
- `apps/common/xanadu/zigzag/manifold.hpp:901-903`:
  ```cpp
  /// Operation index -> dense id, for every operation in a cell's chain and
  /// not only for its birth op -- see slot().
  std::unordered_map<CellRef, std::uint32_t> byRef;
  ```
- `apps/common/xanadu/zigzag/arena_manifold.cpp:3541-3580` (`encodeArenaPrefix` only writes `slots`, `links`, `content`, `scratch`, `trail`, `shadowOrder`, etc., having no concept of `byRef` or `sourceOpIndex` history chains).

**Fix:** Specify that a `Manifold` checkpoint format cannot simply mirror `ArenaManifold::PrefixSnapshot`. It MUST serialize `byRef` (or record the complete R7 `lastOp` -> `dense` mapping along with a mechanism to traverse ancestral ops), together with `structureBirths_`, `externalCells_`, `unresolvedExternals_`, `home_`, `dimsDim_`, and `foldedThrough_`. Alternatively, document the exact reconstruction algorithm if `byRef` is to be rebuilt on checkpoint load.

---

### 3. (§2, Proposal 3, line 146) `MDB_INTEGERKEY` specified for composite `{StoreId, opIndex}`, and `StoreId` is hallucinated

Full title: LMDB `MDB_INTEGERKEY` flag violates LMDB ABI when used with composite keys, and `StoreId` does not exist

**Problem:** Proposal 3 proposes:
> "Add an optional `MDB_dbi` (using `MDB_INTEGERKEY` / `{StoreId, opIndex}` keys) storing serialized `Manifold` checkpoints..."

This contains two critical contradictions:
1. `StoreId` does not exist anywhere in the codebase. The stable persisted identity of a store is `DocumentId` (`apps/common/xanadu/document_id.hpp:25`), which is a 16-byte random byte array (`std::array<std::uint8_t, 16> bytes_`).
2. In LMDB (`lmdb.h`), `MDB_INTEGERKEY` mandates that keys are native unsigned integers of size `sizeof(size_t)` or `sizeof(unsigned int)` (4 or 8 bytes). A composite key containing a 16-byte `DocumentId` and a 4-byte `opIndex` is 20 bytes (or 24 bytes with alignment padding). Passing a 20-byte composite struct to an `MDB_INTEGERKEY` database violates LMDB's ABI, causing LMDB to compare only the first 4 or 8 bytes (or fail with `EINVAL`), leading to corrupted B+tree ordering and key collisions across stores.

**Evidence:**
- `apps/common/xanadu/document_id.hpp:25-31`:
  ```cpp
  class DocumentId {
  public:
    DocumentId() { generate(); }
    [[nodiscard]] const std::array<std::uint8_t, 16> &bytes() const noexcept {
      return bytes_;
    }
  ```
- `apps/common/xanadu/store.hpp:1446`: `DocumentId documentId_;`
- `apps/common/xanadu/lmdb_cache.hpp:39-65` shows existing composite keys (`PieceKey` is 24 bytes, `VSpanKey` is 24 bytes, `ExtSpanKey` is 36 bytes), none of which use `MDB_INTEGERKEY`.

**Fix:** Replace `StoreId` with `DocumentId`. Drop `MDB_INTEGERKEY` and define a fixed-layout binary key struct:
```cpp
struct CheckpointKey {
  std::array<std::uint8_t, 16> documentId{};
  std::uint32_t opIndex{0};
  bool operator==(const CheckpointKey &) const = default;
};
```
following the established key pattern in `LMDBContentCache` (`lmdb_cache.hpp`).

## MAJOR

### 4. (§2, Proposal 4, lines 177-180) Proposal 4 cites non-existent `MmapArena`, ignores existing inline `base()`, and falsely claims `size()` needs inlining

Full title: Inaccurate class name `MmapArena`, unnecessary call to expose `base_`, and redundant proposal to inline `size()`

**Problem:** Proposal 4 states:
> "Move `SegmentedOpsSpool::get(std::uint32_t)` (and `SegmentedOpsSpool::size()` / `ArenaManifold::slot()`) into their headers as `inline` methods (exposing `base_` on `MmapArena` or caching `const CompactOpNode *nodesBase_` in `SegmentedOpsSpool`)."

This contains three codebase inaccuracies:
1. `MmapArena` does not exist in the codebase. The virtual address manager used by `SegmentedOpsSpool` is `VirtualMemoryArena` (`apps/common/xanadu/virtual_memory_arena.hpp:24`).
2. `VirtualMemoryArena` already exposes its base pointer as a public inline method in the header:
   `[[nodiscard]] std::uint8_t *base() const { return baseAddress; }` (`apps/common/xanadu/virtual_memory_arena.hpp:97`). `SegmentedOpsSpool` already has direct access to `arena.base()`.
3. `SegmentedOpsSpool::size()` is ALREADY an inline method in `apps/common/xanadu/segmented_ops_spool.hpp:290`:
   `[[nodiscard]] std::size_t size() const { return opCount; }`. It does not need to be moved to the header.

**Evidence:**
- `apps/common/xanadu/virtual_memory_arena.hpp:24`: `class VirtualMemoryArena`
- `apps/common/xanadu/virtual_memory_arena.hpp:97`: `[[nodiscard]] std::uint8_t *base() const { return baseAddress; }`
- `apps/common/xanadu/segmented_ops_spool.hpp:290`: `[[nodiscard]] std::size_t size() const { return opCount; }`
- `apps/common/xanadu/segmented_ops_spool.hpp:431`: `VirtualMemoryArena arena;`

**Fix:** Revise Proposal 4 to cite `VirtualMemoryArena` and note that `arena.base()` is already public inline. Remove `SegmentedOpsSpool::size()` from the list of functions needing to be moved into the header. Keep the focus cleanly on moving `SegmentedOpsSpool::get(std::uint32_t)` and `ArenaManifold::slot(CellRef)` to inline header definitions.

---

### 5. (§2, Proposal 3, lines 135-138) Misstated `ArenaManifold::linked` behavior regarding `denseOf` and `isProxy` hash lookups

Full title: `denseOf(from)` is direct bitwise masking for ephemeral cells, not a hash lookup; `isProxy` has no independent hash lookup

**Problem:** Proposal 3 describes the current bottleneck in `ArenaManifold::linked` as:
> "On every call it performs a `denseOf(from)` hash lookup, checks `isQuoteOccurrence(from)` and `isProxy(from)` (each doing hash lookups), and linearly scans `cell.linkCount` edges in `links_`."

This mischaracterizes how `ArenaManifold` resolves cells:
1. `denseOf(from)` is NOT a hash lookup for ephemeral cells (`isEphemeral(from)` is true, i.e., bit `0x8000'0000U` set). For ephemeral cells, `denseOf` performs a bitwise mask `from & ~ephemeralBit` and a bounds check against `slots_.size()` (`arena_manifold.cpp:289-291`). It only performs a hash lookup in `overlay_` when `from` is a non-ephemeral base cell.
2. `isProxy(from)` (`arena_manifold.cpp:544-557`) does NOT perform an independent hash lookup. It checks `isQuoteOccurrence(ref)`, and if false, checks `slots_[*dense].valueKind == ValueKind::ExternRef`.

The only actual hash lookup on the ephemeral fast path of `ArenaManifold::linked` is `isQuoteOccurrence(from)` querying `occurrenceToCanonical_`.

**Evidence:**
- `apps/common/xanadu/zigzag/arena_manifold.cpp:280-292`:
  ```cpp
  OptionalDense ArenaManifold::denseOf(const CellRef ref) const noexcept {
    if (!isEphemeral(ref)) {
      const auto found = overlay_.find(ref);
      return found == overlay_.end() ? std::nullopt : std::optional<std::uint32_t>{found->second};
    }
    const auto dense = ref & ~ephemeralBit;
    return dense < slots_.size() ? std::optional<std::uint32_t>{dense} : std::nullopt;
  }
  ```
- `apps/common/xanadu/zigzag/arena_manifold.cpp:544-557`:
  ```cpp
  bool ArenaManifold::isProxy(const CellRef ref) const noexcept {
    if (!isEphemeral(ref)) return false;
    if (isQuoteOccurrence(ref)) return false;
    const auto dense = denseOf(ref);
    if (!dense.has_value() || *dense >= slots_.size()) return false;
    return slots_[*dense].valueKind == static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef);
  }
  ```

**Fix:** Correct the analysis in Proposal 3. State clearly that `denseOf(from)` is direct bitwise masking for ephemeral cells, and that the single hash lookup on the ephemeral hot path is `isQuoteOccurrence(from)` (`occurrenceToCanonical_.contains(from)`). This clarifies exactly why bitflags in `CellSlot::flags` eliminate that specific lookup.

---

### 6. (§2, Proposal 3, lines 142-145, 152-154) Hallucinated class name `LmdbCache` and unargued bit-packing under House Style Rule 2

Full title: Class name is `LMDBContentCache`, and packing flags into `CellSlot::flags` requires explicit Rule 2 justification

**Problem:**
1. Proposal 3 refers to `LmdbCache` (`apps/common/xanadu/lmdb_cache.hpp`). The actual class defined at `lmdb_cache.hpp:93` is `LMDBContentCache`.
2. Proposal 3 suggests packing `isQuoteOccurrence` and `isProxy` into `CellSlot::flags`. House Style Rule 2 states:
   > "State, structure, and metadata belong on orthogonal ZigZag ranks (`d.*`) unless justified; no bit-packed fields outside `CompactOpNode`."
   While `CellSlot::flags` exists as an unclaimed 1-byte field (`manifold.hpp:348`), using it to store runtime bitflags must be explicitly argued under Rule 2, documenting why rank walking `d.quote-origin` or maintaining an external side table cannot meet the throughput demands of `linked()` (243.8 million calls).

**Evidence:**
- `apps/common/xanadu/lmdb_cache.hpp:93`: `class LMDBContentCache`
- `apps/common/xanadu/zigzag/manifold.hpp:348`: `std::uint8_t flags{0}; ///< 1: unclaimed`
- House Style Rule 2 in `brief-common.md:21-23`.

**Fix:** Correct `LmdbCache` to `LMDBContentCache`. In Proposal 3 and its Proposal Register entry, explicitly argue the Rule 2 exemption: traversing orthogonal ranks for presentation/proxy status on every hop during evaluation is prohibitive (accounting for 6.02% of total runtime in `xuzz_test`), and `CellSlot::flags` already exists as an allocated byte in the 32-byte slot layout, so utilizing it introduces zero new memory overhead and zero struct padding changes.

## MINOR

### 7. (§2, Proposal 3, line 152) `CellSlot` size assertion is 32 bytes, not 48 bytes

Full title: Stale 48-byte assertion comment in `manifold.hpp:890` contradicts real `sizeof(CellSlot) == 32` static assert

**Problem:** Proposal 3 discusses `CellSlot::flags` and cache entry invalidation across `CellSlot` layout changes. `apps/common/xanadu/zigzag/manifold.hpp:890` contains a stale comment stating `CellSlot is 48 bytes by assertion`. However, line 357 explicitly asserts `static_assert(sizeof(CellSlot) == 32);` (shrunk when inline `PrimediaSpan` was replaced with an arena run offset).

**Evidence:**
- `apps/common/xanadu/zigzag/manifold.hpp:353-357`:
  ```cpp
  // Thirty-two, down from forty-eight: an inline 24-byte PrimediaSpan became a
  // six-byte reference into an arena...
  static_assert(sizeof(CellSlot) == 32);
  ```
- `apps/common/xanadu/zigzag/manifold.hpp:890-893` (stale comment).

**Fix:** Clarify in the proposal that `CellSlot` is strictly 32 bytes (`static_assert(sizeof(CellSlot) == 32)`), and that utilizing `flags` preserves this 32-byte layout without altering alignment or size.

---

### 8. (§2, Proposal 3, line 135) Incomplete line citation for `ArenaManifold::linked`

Full title: Citing lines 1625-1665 cuts off the second half of `ArenaManifold::linked`

**Problem:** Proposal 3 cites `ArenaManifold::linked` as `arena_manifold.cpp:1625-1665`. In reality, the function continues through line 1682. Line 1665 is in the middle of proxy foreign space resolution; the main edge scan loop over `links_` sits at lines 1675-1681.

**Evidence:**
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1625-1682`.

**Fix:** Change citation from `arena_manifold.cpp:1625-1665` to `arena_manifold.cpp:1625-1682`.

---

### 9. (§2, Proposal 2, lines 108-117) Two-level paged array is unpriced for sparse `structureBirths_`

Full title: Paged array optimization applies well to `byRef`, but causes memory waste for sparse `structureBirths_`

**Problem:** Proposal 2 proposes a two-level paged array for both `byRef` and `structureBirths_`. `byRef` contains an entry for every operation in a cell's history chain (dense across `[1, opCount]`). However, `structureBirths_` only records birth operations (`MakeCell`, `MakeSlice`, `MakeXanadoc`). `StructureBirthInfo` (`manifold.hpp:213-220`) is ~36-40 bytes. Allocating 1024-entry pages of `StructureBirthInfo` indexed by `opIndex` will result in significant internal fragmentation and memory waste, as the vast majority of operations in a store are `SetLink`, `SetValue`, and `Splice`.

**Evidence:**
- `apps/common/xanadu/zigzag/manifold.hpp:213-220`:
  ```cpp
  struct StructureBirthInfo {
    std::uint32_t opIndex{0};
    xanadu::StructureKind kind{xanadu::StructureKind::Cell};
    std::uint32_t containerOp{0};
    xanadu::PrimediaSpan nameSpan{};
  ...
  ```
- `apps/common/xanadu/zigzag/manifold.hpp:914`: `std::unordered_map<std::uint32_t, StructureBirthInfo> structureBirths_;`

**Fix:** Refine Proposal 2 to recommend the two-level paged array specifically for `byRef` (where values are dense 4-byte `uint32_t` slot indices), while recommending a flat open-addressed hash table for `structureBirths_` to avoid sparse table waste.

VERDICT: NOT CONVERGED
