# Round 1 critique: Xanadu / ZigZag Purist (performance-architecture-proposals.md)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 2 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§2, lines 140-150) LMDB Manifold checkpoints violate R9 and hypertime branching integrity

Full title: Serializing Manifold checkpoints into LMDB violates R9 materialised view semantics, the "nothing stores versions" principle, and OSMIC hypertime branching integrity

**Problem:** Proposal 3 advocates storing serialized `Manifold` snapshots in an LMDB database (`$XDG_CACHE_HOME`) keyed by `{StoreId, opIndex}` to skip cold fold replay from the ops spool. This directly contradicts the core architecture of `Store`–Slice convergence (R9, `design/store-slice-convergence.md:395-409`) and the foundational Nelsonian invariant that "nothing stores versions" (`apps/common/xanadu/store.hpp`).

In OSMIC hypertime, history is a branching DAG of immutable operations (`CompactOpNode`), not a linear sequence of version frames. Keying checkpoints by `{StoreId, opIndex}` falsely assumes history is a linear chain: two diverging branches can share or fork from the same op index, rendering a cached ancestor manifold snapshot invalid or poisoned for alternative branches.

Furthermore, introducing an external side-cache of mutable manifold state creates a dual-truth problem. In `LmdbCache` (`apps/common/xanadu/lmdb_cache.hpp`), cached entries are verified, immutable BitTorrent v2 content-addressed piece payloads. A serialized `Manifold` blob in LMDB has no cryptographic Merkle proof, cannot be swarm-verified, and bypasses R9's honesty mechanism (`verifyAgainstFullRebuild`). If an LMDB checkpoint goes stale across internal layout edits, or becomes desynchronized after segment adoption or branch scrubbing, it will resurrect severed links or deleted cells out-of-band without any operation recorded in the ops spool.

**Evidence:**
- `design/store-slice-convergence.md:395-409` (R9): "`Manifold` is an explicitly materialised view with a stated rebuild policy... It is not a pretend-pure replay product. Folding `Structure` over `ancestralPath` is $O(\text{history})$... and `store.hpp` guarantees that nothing caches. The policy is therefore explicit: Full fold once, on load, from `ancestralPath`... A test hook `bool verifyAgainstFullRebuild(const Store &) const` asserting that the incremental state equals a cold fold. That hook is the honesty mechanism, and it is what stops the view drifting".
- `design/performance-architecture-proposals.md:146-150`: "Add an optional `MDB_dbi` (using `MDB_INTEGERKEY` / `{StoreId, opIndex}` keys) storing serialized `Manifold` checkpoints... Cold folds load the nearest ancestor checkpoint from LMDB and replay only the tail delta ops."
- `apps/common/xanadu/store.cpp:529-542`: `Store::rebuildManifoldFromIndex` rebuilds via `OsmicWalker::walkAncestral`, ensuring every cell and link is derived strictly from operations on `ancestralPath`.
- House Style Rule 2: "Cells and Dimensions Over Side Tables or Bit-Packing".

**Fix:** Reject external LMDB checkpoint serialization for `Manifold`. Cold folds must remain pure replays of `Structure` operations over `ancestralPath`. Rebuild acceleration must be achieved through zero-copy in-memory spool access (Proposal 4) and dense/paged R7 resolution in `Manifold::byRef` (Proposal 2), preserving the spool as the single cryptographically verifiable source of truth.

### 2. (§2, lines 154-156) Direct indexing via cached `dimOrdinal` resurrects privileged dimensions

Full title: Caching dense `dimOrdinal` and introducing direct dimension indexing violates R12's prohibition on privileged dimensions

**Problem:** Proposal 3 suggests: "(or cache a small dense `dimOrdinal` per dimension) so cells with many dimensions use binary search or direct indexing instead of linear scan." This explicitly reintroduces privileged dimensions into the storage layer, directly violating R12 of `design/store-slice-convergence.md:502-521`.

Under R2 and R12, dimensions are cells (`CellRef`) minted on the `d.dims` rank hanging off the home cell. R12 deliberately removed the fixed inline dimension array and demoted `DimOrdinal` to a local lookup table of twelve well-known names precisely because direct indexing by ordinal privileges compiled-in dimensions over user-minted or query-generated dimensions (e.g. dimensions minted dynamically by VQL). A user-minted dimension cannot have a fixed small ordinal, so direct indexing inherently creates a two-tier storage hierarchy where user dimensions are second-class citizens.

Furthermore, ZigZag cells in real slices participate in a modest number of dimensions (typically 2 to 6). Linearly scanning a contiguous 12-byte `DimLink` run (`dim`, `pos`, `neg`) is cache-line friendly and faster than branch mispredictions from binary search or indirection through an ordinal translation table.

**Evidence:**
- `design/store-slice-convergence.md:502-521` (R12): "No privileged dimensions. Links are per-cell runs, keyed by `CellRef`. R2 makes a dimension a cell so that a user can mint one. A fixed inline array of well-known dimensions puts the privilege straight back at the storage layer, one level down where it is harder to see: a user-minted dimension could never be fast, however hot it actually is... Every dimension is then reached the same way and costs the same, whether it is `d.1` or something VQL minted forty milliseconds ago. `DimOrdinal` survives only as R2 already demoted it: a lookup table of twelve well-known names, never a storage layout."
- `design/performance-architecture-proposals.md:154-156`: "(or cache a small dense `dimOrdinal` per dimension) so cells with many dimensions use binary search or direct indexing instead of linear scan."

**Fix:** Strike all references to caching dense `dimOrdinal` and direct dimension indexing from Proposal 3. Retain contiguous `DimLink` runs keyed by `DimRef` as mandated by R12. Address traversal efficiency in `ArenaManifold::linked` by eliminating redundant hash probes on `occurrenceToCanonical_`, not by compromising dimension uniformity.

### 3. (§2, lines 190-210) Pre-lowered `DecodedOp` instruction cache abandons hyperstructural execution

Full title: Pre-lowered linear `DecodedOp` instruction cache in `VortexVM` violates House Style Rule 2, abandons hyperstructural execution, and creates cache-invalidation thrashing on manifold edits

**Problem:** Proposal 5 proposes lowering the `VortexCore` opcode graph into a flat C++ vector of `DecodedOp` structs (`OpcodeKind`, continuation `CellRef`, pre-resolved input/output `CellRef` spans, cached `FrameDescriptor` pointer), bypassing ZigZag topological link traversal during execution. This violates House Style Rule 2 ("Cells and Dimensions Over Side Tables"), the Purist checklist (§2.6: "No Foreign Serialization Inside Cells: Structured records, lists, ASTs, and activation frames must be represented as ZigZag cells and links... never hidden in C++ structs when the design claims manifold grounding"), and the core philosophy of the Vortex runtime (`design/vortex-hyperstructural-runtime.md`).

In Vortex, code *is* hyperstructural data in ZigZag space: instructions are cells, sequentially chained along `d.spin`, with inputs and outputs connected via `d.grab` and chained along `d.step`. Compiling this topology into a linear C++ side-vector of decoded structs reduces Vortex to a conventional linear bytecode interpreter that merely uses ZigZag as an initial serial file format.

Crucially, Proposal 5 claims this cache can be "keyed by a `Manifold` mutation epoch so any structural edit invalidates the decoded cache." In a hyperstructural runtime, running programs actively weave data cells, relink results, and instantiate structures in `ArenaManifold`. If every manifold edit bumps the mutation epoch, the instruction cache will be invalidated and re-lowered on every single instruction that modifies the manifold (causing catastrophic compile storms and cache thrashing). Conversely, if code cells are partitioned or exempt from epoch checks, self-modifying code or dynamically linked wings on `d.grab` will execute stale instructions.

Profiling shows that `wingOf` and `getOpcodeKind` are slow not because ZigZag traversal is flawed, but because `wingOf` (`vortex_core.cpp:396-398`) allocates a heap `std::vector<CellRef>` via `std::ranges::to<std::vector>()` on every call, and `getOpcodeKind` (`vortex_vm.cpp:576-591`) queries four separate `std::unordered_map` containers before checking the cell.

**Evidence:**
- `design/performance-architecture-proposals.md:201-205`: "Compile/lower a `VortexCore` opcode graph lazily on first execution into a flat vector of resolved `DecodedOp` structs... keyed by a `Manifold` mutation epoch so any structural edit invalidates the decoded cache."
- `vortex-design-critique/SKILL.md:199-202`: "No Foreign Serialization Inside Cells: Structured records, lists, ASTs, and activation frames must be represented as ZigZag cells and links (`d.vars`, `d.values`, `d.elts`, `d.stack`), never serialized into strings or hidden in C++ structs when the design claims manifold grounding."
- `apps/common/xanadu/vortex/vortex_core.cpp:382-398`: `wingOf` performs dynamic heap vector allocation on every opcode step instead of returning an ephemeral rank view or stack-allocated span.
- `apps/common/xanadu/vortex/vortex_vm.cpp:576-591`: `getOpcodeKind` probes `cellReferences_`, `streamSlots_`, `iteratorSlots_`, and `opcodeMap_` before reading the cell's own value bits.

**Fix:** Reject the flat C++ `DecodedOp` vector cache. Maintain pure hyperstructural execution over ZigZag ranks (`d.spin`, `d.grab`, `d.step`). Eliminate the execution bottlenecks by:
1. Refactoring `VortexCore::wingOf` to return a lazy range or an `inplace_vector<CellRef, 8>` stack buffer, eliminating heap allocations during operand fetching.
2. Storing the `OpcodeKind` tag directly in `CellSlot::valueBits` / `valueKind` at opcode creation time, allowing `getOpcodeKind` to inspect the cell slot in $O(1)$ without probing side maps.

## MAJOR

### 4. (§2, lines 72-75) Flat arrays indexed directly by raw `CellRef` break under the R8 `ephemeralBit` partition

Full title: Flat tables indexed directly by raw `CellRef` fail under R8's `ephemeralBit` partition between persistent and arena cells

**Problem:** Proposal 1 proposes replacing `std::unordered_map<CellRef, ...>` for VM per-cell state with "flat generation-stamped tables or open-addressed flat maps keyed by `CellRef`." Under R8 (`design/store-slice-convergence.md:541-553` and `apps/common/xanadu/zigzag/arena_manifold.hpp:20-30`), cell references are strictly partitioned: ephemeral `ArenaManifold` refs have `ephemeralBit = 0x8000'0000U` set.

If a flat table attempts to use raw `CellRef` values directly as dense array indices, an arena ref such as `0x8000'0001` would index past 2 billion entries, triggering an immediate out-of-bounds crash or requiring a multi-gigabyte allocation. Furthermore, `VortexVM` routinely handles both persistent bytecode cells (small op indices from a base `Store`) and ephemeral arena cells (with `ephemeralBit` set). A single flat array indexed by raw `CellRef` cannot span both domains without either mapping arithmetic or partitioning.

**Evidence:**
- `design/performance-architecture-proposals.md:72-75`: "Dense flat indexed tables for VM per-cell state: Replace `std::unordered_map<CellRef, ...>` for `cellReferences_`, `streams_`, `iterators_`, `opcodeMap_`, and `cursorPC_` with flat generation-stamped tables... keyed by `CellRef`."
- `design/store-slice-convergence.md:545-547`: `inline constexpr CellRef ephemeralBit = 0x8000'0000U;`
- `apps/common/xanadu/zigzag/arena_manifold.hpp:20-29`: "Arena refs carry `ephemeralBit`. Not as a convention: it makes every existing R8 refusal catch a leak with no new code."

**Fix:** Amend Proposal 1 to specify that VM tables cannot use raw `CellRef` as a direct array offset. Any flat indexed structure must either:
1. Use an open-addressed hash table that safely handles the high-bit tag without allocating dense address space, or
2. Maintain a two-partition flat structure where persistent refs index `[0, opCount)` and arena refs index `ref & ~ephemeralBit` within `[0, arenaSlots)`.

### 5. (§2, lines 66-71) Standalone WAM-style VM trail duplicates and desynchronizes from `ArenaManifold::mark()`/`release()`

Full title: An independent WAM-style VM mutation trail duplicates and risks desynchronizing from `ArenaManifold`'s authoritative trail

**Problem:** Proposal 1 introduces a standalone WAM-style mutation trail in `VortexVM` storing `{ContainerTag, CellRef key, OldValue}` to roll back VM state on choice-point backtracking. However, `ArenaManifold` already contains an authoritative, battle-tested trail and watermark system (`ArenaManifold::mark()` and `release()`, `apps/common/xanadu/zigzag/arena_manifold.cpp:2428-2560`), which is already invoked at every choice point in `VortexVM::pushChoicePointImpl` (`vortex_vm.cpp:1428`) and `VortexVM::backtrack` (`vortex_vm.cpp:1464`).

Introducing a separate, uncoordinated VM undo trail creates two parallel undo mechanisms. In complex control-flow scenarios—such as OR-parallel choice-point stealing (`stealNearRootChoicePoint`), cut pruning, or exception unwinding—divergence between the VM trail watermark and `ArenaManifold`'s `lastMarkSerial_` can lead to trail corruption or ordering violations (`markOrderRefusal`).

**Evidence:**
- `design/performance-architecture-proposals.md:66-71`: "Instead of copying entire maps at `pushChoicePointImpl` / `invokeFrame` / `returnFrame`, store only a stack/trail watermark (`std::size_t trailMark`) in `ChoicePoint` and frame savepoints. Route VM state mutations through small helpers that append `{ContainerTag, CellRef key, OldValue}` entries to a contiguous undo trail..."
- `apps/common/xanadu/vortex/vortex_vm.cpp:1428`: `choiceStack_.back().mark = core_.arena().mark();`
- `apps/common/xanadu/vortex/vortex_vm.cpp:1464`: `zigzag::expectMarkOrder(core_.arena().release(cp.mark));`
- `apps/common/xanadu/zigzag/arena_manifold.cpp:2428-2560`: `ArenaManifold::mark()` tracks serials, cell counts, link sizes, proxy counts, and dimension counts.

**Fix:** Harmonize the VM mutation trail with `ArenaManifold`. Rather than inventing an isolated VM trail mechanism, bind the VM mutation trail watermark directly to `ArenaManifold::Mark::serial`, or extend `ArenaManifold`'s trail callback hooks so all hyperstructural and VM mutations unwind in lockstep through a single coordinated rollback boundary.

## MINOR

### 6. (§2, lines 136-137) Proposal mischaracterizes `ArenaManifold::denseOf(from)` as a hash lookup

Full title: Section 2 incorrectly describes `ArenaManifold::denseOf(from)` as performing a hash lookup

**Problem:** Proposal 3 states that on every call to `ArenaManifold::linked`, "it performs a `denseOf(from)` hash lookup". In `ArenaManifold`, `denseOf(from)` is pure bitwise arithmetic and a bounds check ($O(1)$, zero hashing), as documented in `apps/common/xanadu/zigzag/arena_manifold.hpp:20-23` and lines 257-264 (`from & ~ephemeralBit`). Only the persistent `Manifold` uses a hash table (`byRef`) for `denseOf` to resolve R7 micro-history chains.

**Evidence:**
- `design/performance-architecture-proposals.md:136-137`: "On every call it performs a `denseOf(from)` hash lookup..."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:22-23`: "An arena cell has no history, so `denseOf()` here is arithmetic and there is no map."

**Fix:** Correct the prose in Proposal 3 to clarify that `denseOf` is already $O(1)$ arithmetic in `ArenaManifold`, isolating the actual hash lookup bottleneck to the `isQuoteOccurrence` check on `occurrenceToCanonical_`.

### 7. (§2, lines 152-154) `isQuoteOccurrence` guard in `linked()` is redundant with the general traversal path

Full title: Dedicated `isQuoteOccurrence` check in `ArenaManifold::linked` duplicates the general link scan and can be eliminated

**Problem:** Proposal 3 recommends encoding `isQuoteOccurrence` into `CellSlot::flags` to avoid map lookups. However, comparing lines 1634-1641 with lines 1674-1681 in `apps/common/xanadu/zigzag/arena_manifold.cpp` reveals that the loop executed for a quote occurrence is literally byte-for-byte identical to the default link scan. The check exists solely to bypass foreign-space proxy delegation. Since proxy cells already carry `slots_[d].valueKind == ValueKind::ExternRef` (`arena_manifold.cpp:555-556`), checking for proxy status first makes the `isQuoteOccurrence` guard entirely redundant.

**Evidence:**
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1634-1641` vs `1674-1681`: Both execute identical linear scans over `cell.linkCount` entries in `links_`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:555-556`: `isProxy` is defined by `slots_[*dense].valueKind == static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef)`.

**Fix:** Reorder `ArenaManifold::linked` to check `slots_[d].valueKind == ValueKind::ExternRef` (proxy check) and allow non-proxy cells (both regular cells and quote occurrences) to fall straight into the link scan. This eliminates the `occurrenceToCanonical_` hash check completely without modifying `CellSlot` bitflags.

VERDICT: NOT CONVERGED
