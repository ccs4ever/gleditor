# Round 1 critique: System Architect (performance-architecture-proposals.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§2, line 50) Missing Numbered Proposal Register (P1..Pn) for Core Engine Files

Full title: Failure to Isolate Invasive Changes to Load-Bearing Engine Files in a Formal Proposal
Register

**Problem:** The document proposes structural, pervasive changes across the core runtime and engine
files (`apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`, `vortex_closures.cpp`,
`apps/common/xanadu/zigzag/manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`,
`apps/common/xanadu/segmented_ops_spool.{hpp,cpp}`, `store.{hpp,cpp}`, `lmdb_cache.hpp`,
`Makefile`), but fails to isolate them in a numbered Proposal Register (`P1..Pn`). House Style Rule
6 explicitly mandates: "Any change to existing C++ engine, core, stdlib, or runtime files must be
isolated in a numbered Proposal Register (`P1`, `P2`, ...) stating the exact files touched, why the
change cannot live in the caller/compiler, the cost, and the fallback if not approved." The document
uses loose prose headings ("Proposal 1", "Proposal 2", etc.) that omit all required accountability
fields.

**Evidence:**

- `design/performance-architecture-proposals.md:50-210`: Sections labeled "Proposal 1" through
  "Proposal 5" contain no numbered registry entries (`P1..Pn`), no enumerations of exact target
  files, no rationale for why logic cannot reside in callers or compilers, no priced memory/cycle
  costs, and no fallbacks.
- `brief-common.md:30-32` & `.agents/skills/vortex-design-critique/SKILL.md:67-70`: Rule 6 requires
  an isolated Proposal Register (`P1`, `P2`, ...).

**Fix:** Add a dedicated "Proposal Register" section enumerating each proposed modification as a
formal entry (`P1`, `P2`, etc.) with:

1. Target files touched (exact paths).
1. Justification explaining why the optimization cannot live in the caller or bytecode compiler.
1. Quantified cost (heap bytes, cache lines, cycle budget, structural links).
1. Fallback plan if the proposal is refused or deferred.

______________________________________________________________________

### 2. (§2.1, line 66) Unsound Ring-Buffer Undo Log & Ignoring Existing `ArenaManifold::trail_`

Full title: Architecturally Unsound Ring-Buffer Undo Log and Failure to Consolidate with In-Tree
`ArenaManifold::trail_`

**Problem:** Proposal 1 suggests implementing a "WAM-style mutation trail" using a "fixed-capacity
ring buffer or contiguous arena-backed trail" to log VM side-table mutations. First, using a ring
buffer for an undo log is structurally unsound: when ring capacity is exceeded, new writes overwrite
the oldest entries. When execution encounters backtracking or exception unwinding, the VM will
replay corrupted records or fail to restore early choices, leading to silent, catastrophic runtime
corruption. Second, Proposal 1 completely ignores the existing WAM watermark trail already
implemented in `ArenaManifold` (`trail_`, `Mark::trailSize`, `ArenaManifold::trail()`,
`ArenaManifold::release()`), which is already referenced by `ChoicePoint::mark`! Proposing a
secondary, parallel undo trail without evaluating or consolidating with `ArenaManifold`'s trail
violates DRY consolidation and engine invariants.

**Evidence:**

- `design/performance-architecture-proposals.md:69-71`: "...append
  `{ContainerTag, CellRef key, OldValue}` entries to a contiguous undo trail (`std::vector` or
  bounded ring-buffer slab). On `backtrack()` or exception rollback, unwind entries back to
  `trailMark` in reverse order."
- `design/performance-architecture-proposals.md:80-82`: "...a fixed-capacity ring buffer or
  contiguous arena-backed trail avoids per-node heap allocations during execution and resets in
  $O(1)$ on `cut()` / `clearChoicePoints()`."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:208`: `std::uint32_t trailSize{0};` inside
  `struct Mark`.
- `apps/common/xanadu/zigzag/arena_manifold.hpp:965`: `std::vector<TrailEntry> trail_;`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:2361-2384`:
  `void ArenaManifold::trail(const std::uint32_t dense)`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:2592-2598`: `ArenaManifold::release` unwinds
  `trail_` in reverse order back to `m.trailSize`.
- `apps/common/xanadu/vortex/vortex_vm.hpp:523`: `struct ChoicePoint` already embeds
  `zigzag::Mark mark{};`.

**Fix:** Reject ring-buffered overwrite semantics for undo logging. The undo trail must be an
append-only contiguous vector backed by a watermark, with strict resource bounds enforced via
execution grant limits (`vagent.max_trail_entries` / `ArenaManifold::setExecutionLimits`) that trap
on fuel/scratch exhaustion rather than overwriting history. Consolidate VM trail recording with
`ArenaManifold::trail_` (or factor out a reusable `Trail<T>` utility under `apps/common/xanadu/`),
driving unwinding through a unified watermark mechanism.

______________________________________________________________________

### 3. (§2.3, line 140) Unbounded LMDB Manifold Checkpointing & Content Cache Conflation

Full title: Storing Volatile Derived Checkpoints in `LMDBContentCache` with Unbounded Retention and
Unpriced I/O

**Problem:** Proposal 3 advocates storing serialized `Manifold` snapshots every $K = 4096$
operations in `LMDBContentCache`. This is architecturally flawed on four levels:

1. *Layer and Role Conflation*: `LMDBContentCache` (`apps/common/xanadu/lmdb_cache.hpp:93-100`) is
   explicitly designated for immutable, verified BitTorrent pieces, cryptographic content-encryption
   keys (CEKs) paid with real micropayments, and primedia scroll coordinates, held under restricted
   filesystem permissions (0700/0600). Polluting this secure content store with volatile, derived
   engine replay snapshots conflates immutable cryptographic storage with transient execution
   caching.
1. *Resource Exhaustion (`MDB_MAP_FULL`)*: `LMDBContentCache` is configured with a default map size
   of 1 GiB (`lmdb_cache.hpp:96`). Serialized `Manifold` checkpoints (which can span tens of
   megabytes for stores with $10^5$ cells and links) written every 4096 ops with no retention
   bounds, LRU eviction, or compaction policy will rapidly exhaust the 1 GiB memory map, triggering
   fatal LMDB errors.
1. *Arbitrary Ceilings (House Style Rule 4)*: $K = 4096$ is an arbitrary, unconfigurable magic
   number.
1. *Unpriced Deserialization vs Memory Replay (House Style Rule 1)*: Proposal 3 asserts that LMDB
   checkpoints will accelerate cold folds without pricing B+tree lookup, transaction
   synchronization, and binary deserialization overhead against sequential in-memory `CompactOpNode`
   replay from the mmap'd `SegmentedOpsSpool` (which already runs at memory bus speeds, ~27.7 ns/op
   in `xuzz_test`).

**Evidence:**

- `design/performance-architecture-proposals.md:144-150`: "Add an optional `MDB_dbi` (using
  `MDB_INTEGERKEY` / `{StoreId, opIndex}` keys) storing serialized `Manifold` checkpoints (e.g.
  every $K = 4096$ structure ops or at named branch heads)..."
- `apps/common/xanadu/lmdb_cache.hpp:93-100`: `LMDBContentCache` constructor with default
  `map_size_bytes = 1024ULL * 1024 * 1024` and explicit security comments governing CEKs and
  verified text.
- `apps/common/xanadu/segmented_ops_spool.hpp:163`: `SegmentedOpsSpool` mmap operations already
  reside in virtual memory via `VirtualMemoryArena`.
- `brief-common.md:26-27`: Rule 4 ("No Arbitrary Ceilings").

**Fix:** Remove `Manifold` checkpoint caching from `LMDBContentCache`. If manifold checkpoints are
empirically justified by benchmarks against large stores ($>10^6$ ops), they must live in a
dedicated, decoupled cache store with an explicit retention/LRU budget and configurable stride, or
be integrated into the store's native segment/snapshot model. The serialization and deserialization
cycle costs must be benchmarked against sequential mmap spool replay before introducing disk-backed
checkpointing.

______________________________________________________________________

## MAJOR

### 1. (§2.1, line 78) Layer Boundary Bleed: Citing OpenGL Buffer in Headless Engine Proposal

Full title: Citing OpenGL Rendering Abstraction (`StreamBufferGL`) as Precedent for Headless Engine
VM Trail

**Problem:** Proposal 1 cites `StreamBufferGL` (`include/gleditor/render/gl/stream_buffer.hpp`), an
OpenGL streaming vertex/uniform buffer abstraction, as architectural justification for an undo trail
in `apps/common/xanadu/vortex/`. Headless engine code under `apps/common/xanadu/` must remain 100%
free of UI and GPU rendering dependencies. Drawing architectural patterns from OpenGL streaming
buffers for core engine execution violates clean conceptual separation between rendering
(`include/gleditor/render/gl/`, `src/render/gl/`) and the headless Xanadu engine
(`apps/common/xanadu/`), while overlooking the exact domain-appropriate pattern already present in
`ArenaManifold::trail_`.

**Evidence:**

- `design/performance-architecture-proposals.md:79-80`: "Similar to `NetStreamRing`
  (`apps/common/xanadu/zigzag/net_lens.hpp:75-92`) and `StreamBufferGL`
  (`include/gleditor/render/gl/stream_buffer.hpp`)..."
- `AGENTS.md:148-154`: "Everything xanalogical goes in `apps/common/`: `apps/common/xanadu/` if it
  needs no graphics device... `src/` and `include/gleditor/` are generic only".
- `.agents/skills/system-architect/SKILL.md:148-150`: "Does headless engine code
  (`apps/common/xanadu/`) remain 100% free of UI/graphics dependencies?"

**Fix:** Remove all references to `StreamBufferGL`. Confine architectural analogies and shared
abstractions to engine-level structures under `apps/common/xanadu/`.

______________________________________________________________________

### 2. (§2.3, line 152) Bit-Packing in `CellSlot` Violates House Style Rule 2

Full title: Bit-Packing `isQuoteOccurrence` and `isProxy` Flags into `CellSlot::flags` Without
Justification

**Problem:** Proposal 3 suggests: "Encode `isQuoteOccurrence` and `isProxy` as bitflags in
`CellSlot::flags` (already 1 byte in `CellSlot`), eliminating separate hash/set checks on non-proxy,
non-quote cells." House Style Rule 2 strictly dictates: "State, structure, and metadata belong on
orthogonal ZigZag ranks (`d.*`) unless justified; no bit-packed fields outside `CompactOpNode`."
Proposal 3 bit-packs flags into `CellSlot` without providing the required architectural
justification. Furthermore, Proposal 3 overlooks that `isProxy` can already be determined by
inspecting `slots_[*dense].valueKind == ValueKind::ExternRef` (which is already an existing field in
`CellSlot`), eliminating the hash lookup without introducing ad-hoc bitfields.

**Evidence:**

- `design/performance-architecture-proposals.md:152`: "Encode `isQuoteOccurrence` and `isProxy` as
  bitflags in `CellSlot::flags` (already 1 byte in `CellSlot`)..."
- `brief-common.md:21-23`: Rule 2 ("no bit-packed fields outside `CompactOpNode`").
- `apps/common/xanadu/zigzag/manifold.hpp:348`: `std::uint8_t flags{0}; ///< 1: unclaimed`.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:555-556`:
  `slots_[*dense].valueKind == static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef);`.

**Fix:** Either:

1. Provide a rigorous micro-architectural justification under Rule 2 demonstrating why caching
   transient classification flags in `CellSlot` is required for $O(1)$ fast paths without altering
   persistent semantics; or
1. Refactor `ArenaManifold::isProxy` to check `CellSlot::valueKind == ValueKind::ExternRef`
   directly, avoiding bitfield packing.

______________________________________________________________________

### 3. (§2.5, line 201) Mutation Epoch Invalidation Thrashing & Unbounded Instruction Cache

Full title: Coarse Global Mutation Epoch Induces Invalidation Thrashing in Hyperstructural Execution

**Problem:** Proposal 5 proposes lowering `VortexCore` opcode graphs into a flat vector of
`DecodedOp` structs, invalidated by a global "Manifold mutation epoch". In Vortex, execution is
hyperstructural: code, activation frames, variables, and output streams are represented as cells and
links in the manifold. If any manifold mutation increments the epoch, any routine that updates a
variable cell, writes an output cell, or moves a cursor will invalidate the entire instruction cache
on every step, resulting in continuous invalidation and recompilation thrashing. Additionally, the
cache lacks retention bounds, memory limits, and eviction policies, risking unbounded heap
consumption in long-running processes.

**Evidence:**

- `design/performance-architecture-proposals.md:201-205`: "...compiled/lowered lazily on first
  execution into a flat vector of resolved `DecodedOp` structs... keyed by a `Manifold` mutation
  epoch so any structural edit invalidates the decoded cache."
- `design/performance-architecture-proposals.md:208-210`: Identifies complexity on self-modifying
  code but ignores invalidation from ordinary data writes.
- `brief-common.md:26-27`: Rule 4 ("Bounds for resource safety must be priced and exposed as
  settings or grant limits").

**Fix:** Scope cache invalidation to specific code spans or routine entry points (e.g. tracking
mutation epochs on the `d.code` rank) rather than the global manifold epoch. Bound the instruction
cache with a strict capacity ceiling and LRU eviction policy exposed as a grant setting
(`vagent.max_decoded_ops`).

______________________________________________________________________

### 4. (§2.2, line 108) Bespoke Paged Directory Instead of Leveraging `VirtualMemoryArena`

Full title: Designing Bespoke Two-Level Paged Directory Instead of Consolidating with
`VirtualMemoryArena`

**Problem:** Proposal 2 proposes replacing `Manifold::byRef` and `structureBirths_` with either a
custom "two-level paged flat array (e.g., 1024-entry pages matching `SegmentedOpsSpool`'s 1024-node
piece cadence, lazily allocated per high-bit page)" or an open-addressed flat table. The engine
already contains `VirtualMemoryArena` (`apps/common/xanadu/virtual_memory_arena.hpp`), which
reserves contiguous virtual address space (`reserve()`) and commits anonymous memory on demand
(`commitAnonymous()`). Both `SegmentedOpsSpool` and `SegmentedPrimediaSpool` rely on
`VirtualMemoryArena` to handle hundreds of millions of operations with zero upfront RAM allocation
(`PROT_NONE`). Inventing a bespoke two-level paging directory with pointer chasing and secondary
allocations duplicates virtual memory paging logic and adds unpriced complexity.

**Evidence:**

- `design/performance-architecture-proposals.md:108-117`: Proposal 2.
- `apps/common/xanadu/virtual_memory_arena.hpp:24-71`: `VirtualMemoryArena` API (`reserve`,
  `commitAnonymous`, `pageSize`).
- `apps/common/xanadu/segmented_ops_spool.hpp:163, 431`: `VirtualMemoryArena arena;`.
- `.agents/skills/system-architect/SKILL.md:57-58`: Mandates identifying duplicated utility or
  allocation routines.

**Fix:** Evaluate using `VirtualMemoryArena` directly (committing pages on demand as operations are
appended) or flat contiguous arrays pre-sized via `Store` capacity hints, rather than engineering a
bespoke two-level directory. Price the exact memory footprint per entry and document refused
alternatives with technical justifications.

______________________________________________________________________

### 5. (§2.1, line 72) Missing C++ vs Vortex Architectural Justification for VM Side Tables

Full title: Failure to Justify C++ Side Tables Over Manifold Ranks for `VortexVM` State

**Problem:** Proposal 1 focuses on optimizing 10 separate C++ container maps (`cellReferences_`,
`streams_`, `iterators_`, `streamSlots_`, `iteratorSlots_`, `activationFrames_`, `guardStacks_`,
`pendingGuardChecks_`, `guardScopes_`, `mutationGroups_`) into flat open-addressed C++ tables.
However, it fails to address the core mandate of GEMINI.md and House Style Rules 2 & 5: *Why are
these 10 state categories maintained in C++ side tables instead of being modeled as cells and links
on ZigZag ranks (`d.*`) in `ArenaManifold`?* If these execution structures were represented on the
manifold, `ArenaManifold::release(mark)` would roll them back naturally during backtracking with
zero custom C++ map copying or trailing code.

**Evidence:**

- `design/performance-architecture-proposals.md:55-59`: Enumerates 10 separate C++ containers
  snapshotted by value in `ChoicePoint`.
- `GEMINI.md`: "New C++ code must justify why it isn't being written in Vortex."
- `brief-common.md:21-23, 28-29`: Rules 2 & 5.

**Fix:** Provide an explicit architectural justification for why transient execution state must
reside in C++ structures (e.g., non-reentrant host dispatch, raw cycle budgets) rather than on
manifold ranks, or migrate suitable state structures into the manifold to leverage existing
topological rollback.

______________________________________________________________________

## MINOR

### 1. (§2.4, line 178) Hallucinated `MmapArena` and Redundant `size()` Inlining

Full title: Citation of Non-Existent `MmapArena` and Redundant Proposal to Inline
`SegmentedOpsSpool::size()`

**Problem:** Proposal 4 states: "Move `SegmentedOpsSpool::get(std::uint32_t)` (and
`SegmentedOpsSpool::size()` / `ArenaManifold::slot()`) into their headers as inline methods
(exposing `base_` on `MmapArena` or caching `const CompactOpNode *nodesBase_` in
`SegmentedOpsSpool`)." `MmapArena` does not exist in the codebase; the arena member in
`SegmentedOpsSpool` is `VirtualMemoryArena arena;`, and its `base()` accessor is already public and
inline at `virtual_memory_arena.hpp:97`. Furthermore, `SegmentedOpsSpool::size()` is ALREADY defined
inline in `segmented_ops_spool.hpp:290`.

**Evidence:**

- `design/performance-architecture-proposals.md:177-179`.
- `apps/common/xanadu/segmented_ops_spool.hpp:290`:
  `[[nodiscard]] std::size_t size() const { return opCount; }`.
- `apps/common/xanadu/segmented_ops_spool.hpp:431`: `VirtualMemoryArena arena;`.
- `apps/common/xanadu/virtual_memory_arena.hpp:97`:
  `[[nodiscard]] std::uint8_t *base() const { return baseAddress; }`.

**Fix:** Correct the proposal to cite `VirtualMemoryArena::base()`, drop the redundant proposal to
inline `size()`, and restrict the proposal to inlining `SegmentedOpsSpool::get(uint32_t)` and
`ArenaManifold::slot(CellRef)`.

______________________________________________________________________

### 2. (§2.3, line 143) Inaccurate Class Identifier `LmdbCache`

Full title: Use of Inaccurate Class Identifier `LmdbCache` Instead of `LMDBContentCache`

**Problem:** Proposal 3 refers repeatedly to `LmdbCache`. The actual class name defined in
`apps/common/xanadu/lmdb_cache.hpp` is `LMDBContentCache`.

**Evidence:**

- `design/performance-architecture-proposals.md:143`: "reusing `LmdbCache` pattern".
- `apps/common/xanadu/lmdb_cache.hpp:93`: `class LMDBContentCache`.

**Fix:** Update the class identifier to `LMDBContentCache` throughout the document.

______________________________________________________________________

### 3. (§2.3, line 154) Small-Degree Link Runs Make Binary Search Slower Than Linear Scan

Full title: Unqualified Recommendation for Binary Search Across Contiguous Small-Degree Link Runs

**Problem:** Proposal 3 recommends sorting `DimLink` runs by `DimRef` to replace linear scans with
binary search in `ArenaManifold::linked`. In ZigZag manifolds, the vast majority of cells
participate in few dimensions ($N \le 6$). For small contiguous arrays, linear scan incurs fewer
branch mispredictions and lower instruction overhead than binary search. Sorting on link insertion
would penalize link authoring without improving read latency for typical degree cells.

**Evidence:**

- `design/performance-architecture-proposals.md:154-156`: "...so cells with many dimensions use
  binary search or direct indexing instead of linear scan."

**Fix:** Qualify the recommendation: retain linear scan for small link runs ($N \le 8$) and employ
binary search or index hashing only for high-degree hub cells.

______________________________________________________________________

### 4. (§3, line 213) Execution Order Disconnected from Formal Proposal Register

Full title: Execution Order Plan References Informal Proposal Numbers Instead of Registered Changes

**Problem:** Section 3 provides a recommended execution order using informal proposal numbers
("Proposal 4", "Proposal 2 & Proposal 3", etc.) that do not correlate to formal Proposal Register
entries (`P1..Pn`), obscuring concrete dependency ordering (e.g. refactoring `ArenaManifold::trail_`
prior to `VortexVM` trail undo).

**Evidence:**

- `design/performance-architecture-proposals.md:213-223`.

**Fix:** Re-anchor the execution roadmap to the formal Proposal Register entries once established.

______________________________________________________________________

### 5. (§1.3, line 40) `gleditor_test` Profiling Does Not Reflect Real Layout/Render Paths

Full title: Premature Conclusion on `src/` Bottlenecks Based Solely on Minimal Unit Test Profile

**Problem:** Section 1.3 concludes that no structural bottlenecks exist in `src/` based on a
0.19-second run of `gleditor_test`. `gleditor_test` is a minimal headless unit test suite, not a
representative layout or rendering workload under HarfBuzz/FreeType shaping, complex bidirectional
scripts, or heavy scrolling.

**Evidence:**

- `design/performance-architecture-proposals.md:40-44`: "`gleditor_test` had minimal total CPU time
  (`0.19s`)... No structural bottleneck was observed in `src/` compared to `apps/common/xanadu/`."
- `.agents/skills/system-architect/SKILL.md:83-85`: Recommends dedicated probes
  (`tools/layout-latency-probe.cpp`, `tools/benchmark-kjv-load.py`) for measuring layout latency.

**Fix:** Clarify that the `gleditor_test` profile reflects unit test harness overhead only, and that
layout/shaping performance in `src/` must be evaluated using dedicated probes rather than the unit
test suite.

______________________________________________________________________

VERDICT: NOT CONVERGED
