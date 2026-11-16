# Round 1 critique: Technical Writer (performance-architecture-proposals.md)

Paths below are relative to the repo root.

Tally: 2 BLOCKING, 6 MAJOR, 5 MINOR.

## BLOCKING

### 1. (§2, line 50) Missing numbered Proposal Register (P1..Pn) for C++ changes

Full title: Omission of the mandatory Proposal Register isolating modifications to existing C++
engine, core, and runtime files

**Problem:** Standing House Style Rule 6 (`brief-common.md:30-32`, `SKILL.md:67-71`) mandates: "Any
change to existing C++ engine, core, stdlib, or runtime files must be isolated in a numbered
Proposal Register (`P1`, `P2`, ...) stating exact files touched, why it cannot live in
caller/compiler, cost, and fallback." The document proposes invasive structural changes to existing
C++ files across `apps/common/xanadu/vortex/` (`vortex_vm.{hpp,cpp}`, `vortex_frames.cpp`,
`vortex_closures.cpp`, `vortex_core.{hpp,cpp}`), `apps/common/xanadu/zigzag/` (`manifold.{hpp,cpp}`,
`arena_manifold.{hpp,cpp}`), and `apps/common/xanadu/` (`store.{hpp,cpp}`, `lmdb_cache.hpp`,
`segmented_ops_spool.{hpp,cpp}`), but groups them as narrative discussion sections without a
numbered Proposal Register table (`P1..Pn`) specifying files touched, why not in caller/compiler /
why not Vortex, cost, and fallback.

**Evidence:** In `design/performance-architecture-proposals.md:50-210`, the proposals are labeled
`### Proposal 1...`, `### Proposal 2...`, but there is no Proposal Register table. Contrast with
`design/vql-lens-bridge.md:346-358` and `design/vlace-lens-bridge.md:229-270` which provide the
required table format.

**Fix:** Add a dedicated section `## Proposal Register` containing a markdown table with columns for
ID (`P1..Pn`), touched files, change summary, why not caller/Vortex, cost (allocations, bytes,
cycles), and fallback. Assign identifiers `P1` through `P6` to each concrete C++ file modification.

### 2. (§2, line 129) Proposal 3 conflates systems and specifies invalid codec

Full title: Proposal 3 combines two distinct architectural proposals and cites
`ArenaManifold::PrefixSnapshot` for persistent `Manifold` checkpoints where it cannot restore
`byRef` or `structureBirths_`

**Problem:** First, Proposal 3 couples two completely unrelated systems under one header: (1)
LMDB-backed persistent `Manifold` checkpoint caching during cold fold replay (`store.cpp`,
`lmdb_cache.hpp`), and (2) in-memory fast-path edge lookup in ephemeral `ArenaManifold::linked` via
bitflags and sorted link runs (`arena_manifold.cpp`). The document's own recommended execution order
in §3 splits them into Step 2 and Step 4, proving they are separate architectural proposals that
must be decoupled and registered separately (`P3` and `P4`). Second, Proposal 3 asserts that LMDB
checkpoints can store serialized `Manifold` snapshots "mirroring how `encodeArenaPrefix` /
`ArenaManifold::PrefixSnapshot` (`arena_manifold.cpp:3541`) already serializes
`slots`/`links_`/`content` snapshots" and that "cold folds load the nearest ancestor checkpoint from
LMDB and replay only the tail delta ops." However, `ArenaManifold::PrefixSnapshot` only records
`slots_`, `links_`, `content_`, and mark counts (`arena_manifold.cpp:3541-3560`); it has zero
representation for `Manifold::byRef` (`manifold.hpp:903`) or `Manifold::structureBirths_`
(`manifold.hpp:914`). In `Manifold`, resolving a cell's operation chain (R7) requires `byRef`. If a
cold fold restores an LMDB checkpoint that only contains slots and links, subsequent operations in
the ancestral replay tail will fail or produce an invalid manifold state because `byRef` is
unpopulated. The serialization scheme cannot work as specified without explicitly serializing
`byRef` and `structureBirths_` (or rebuilding them).

**Evidence:** `design/performance-architecture-proposals.md:148-150`: "mirroring how
`encodeArenaPrefix` / `ArenaManifold::PrefixSnapshot` (`arena_manifold.cpp:3541`) already serializes
`slots`/`links_`/`content` snapshots. Cold folds load the nearest ancestor checkpoint from LMDB and
replay only the tail delta ops." Compare with
`apps/common/xanadu/zigzag/arena_manifold.cpp:3541-3560`, which serializes only ephemeral arena
marks, and `apps/common/xanadu/zigzag/manifold.hpp:903, 914`, where `byRef` and `structureBirths_`
are essential to persistent structure replay.

**Fix:** Split Proposal 3 into two distinct proposals. For the LMDB checkpoint proposal, explicitly
design and specify a dedicated `ManifoldSnapshot` codec that either serializes `byRef` and
`structureBirths_` or specifies an $O(\text{checkpoint ops})$ index reconstruction phase before tail
replay begins.

## MAJOR

### 1. (§2, line 48) Complete omission of Refused Alternatives section

Full title: Missing mandatory Refused Alternatives section recording rejected designs with technical
reasons

**Problem:** Standing House Style Rule 1 (`brief-common.md:18-20`, `SKILL.md:54-56`) mandates that
designs "justify every mechanism by citing exact repository files and line numbers (`file:line`),
pricing the cost (bytes, cycles, links, messages, segments), and recording refused alternatives with
the technical reason they were rejected." The document provides narrative "Risks & trade-offs"
paragraphs under each proposal, but contains no dedicated Refused Alternatives section or table
documenting what competing designs were evaluated and why they were rejected (e.g. persistent
functional data structures like HAMT/Immer for `ChoicePoint` state; fork-based memory snapshots vs.
LMDB checkpoints; custom arena bump-allocator for activation frames; JIT/copy-and-patch vs. linear
decoded instruction cache).

**Evidence:** `design/performance-architecture-proposals.md` has sections `## 1`, `## 2`, and
`## 3`, with no Refused Alternatives section.

**Fix:** Add a dedicated `## Refused Alternatives` section recording evaluated and rejected
alternatives for each proposal along with the technical reasons for refusal.

### 2. (§2, line 48) Complete omission of Open Questions table

Full title: Absence of an Open Questions table capturing unresolved design decisions with concrete
defaults and scope boundaries

**Problem:** The document raises several non-trivial open questions in prose (e.g. how
OR-parallelism's `stealNearRootChoicePoint()` will reconstruct state from a trail slice; how
synthetic unit tests with sparse `CellRef`s affect two-level paged arrays vs. open-addressed hash
tables; cache eviction policy and invalidation triggers for LMDB checkpoints across
`CompactOpNode`/`CellSlot` layout changes; invalidation mechanics for self-modifying code in the
instruction cache). None of these are recorded in a structured Open Questions table with assigned
identifiers (`U-X<k>`), concrete defaults, and scope boundaries, violating house style.

**Evidence:** `design/performance-architecture-proposals.md:84-91, 121-125, 158-163, 206-209`
contain unresolved trade-offs in prose with no tracking table.

**Fix:** Add an `## Open Questions` section containing a table assigning identifiers (e.g., `U-P1`
for OR-parallel trail stealing, `U-P2` for sparse ID paging vs. flat table, `U-P3` for LMDB
checkpoint cache invalidation, `U-P4` for instruction cache mutation invalidation) with concrete
defaults and scope boundaries.

### 3. (§2, line 48) Absence of priced rulings across all proposals

Full title: Proposals fail to price costs in bytes, cycles, allocations, and cache footprints

**Problem:** Standing House Style Rule 1 requires that designs "price the cost (bytes, cycles,
links, allocations)". While §1 provides profiling call counts and percentages from `gprof`, §2 fails
to provide concrete priced costs for the proposed data structures: Proposal 1 does not price the
byte size of trail entries `{ContainerTag, CellRef key, OldValue}`, the maximum capacity or memory
footprint of the trail slab/ring buffer, or the cycle cost of trail unwinding; Proposal 2 suggests a
two-level paged array or flat open-addressed hash table with generation tags, but does not price
page size (e.g. 1024 entries $\times$ 4 bytes = 4 KiB), root directory overhead, or slot size for
the flat hash table; Proposal 3 does not price LMDB checkpoint disk footprint per snapshot,
checkpoint frequency overhead, or the memory layout cost of sorting `DimLink` runs; Proposal 5 does
not define the memory size of `DecodedOp` or the total cache footprint per compiled program cell.

**Evidence:** `design/performance-architecture-proposals.md:64-75, 107-120, 140-157, 199-205`
contain qualitative assertions ("small helpers", "tiny page-directory indirection", "high payoff")
without quantitative byte or cycle pricing.

**Fix:** Provide explicit data structure definitions with `sizeof` calculations, memory footprint
bounds, and cycle/allocation cost models for each proposed mechanism.

### 4. (§1–§2, line 100) Missing citations to sibling specifications

Full title: Uncited references to architectural concepts and missing cross-references to
foundational sibling design documents

**Problem:** The document uses architectural terminology and constraints defined in sibling design
documents without citing them: Line 100 mentions "a cell's R7 chain" without citing
`design/store-slice-convergence.md` where R7 (Structure operation index as micro-history chain) is
defined; Proposal 1 discusses WAM choice points, Dewey paths, and trail backtracking without citing
`design/vortex-hyperstructural-runtime.md`; Proposal 3 discusses cold fold replay and compaction
without citing `design/store-slice-convergence.md` R12; the document discusses performance
bottlenecks in VQL queries and zigzag traversals without citing `design/vql-performance-spike.md`.

**Evidence:** `design/performance-architecture-proposals.md:1-222` contains zero cross-references to
`design/store-slice-convergence.md`, `design/vortex-hyperstructural-runtime.md`, or
`design/vql-performance-spike.md`.

**Fix:** Add explicit markdown links and section citations to sibling design specifications in the
introduction and relevant proposal sections.

### 5. (§1–§3, line 15, 48, 212) Non-hierarchical section numbering

Full title: Inconsistent section numbering schema (`Proposal N` vs `§X.Y`) and fractured proposal
citations across tables and execution order

**Problem:** Under `## 2. Structural proposals`, subsections are labeled `### Proposal 1: ...`,
`### Proposal 2: ...` without hierarchical section numbers (`§2.1`, `§2.2`, etc.). In §1's baseline
tables (lines 19, 21, 34), cross-references cite `Open (Proposal 3)` or `Open (Proposal 2 / 3)`
rather than standard `§X.Y` section markers. In §3 (lines 217-222), "Recommended execution order"
refers to "Proposal 2 & Proposal 3 (`ArenaManifold::linked` fast path)" in item 2, and "Proposal 3
(LMDB Manifold checkpoints)" in item 4, dividing Proposal 3 into separate steps because it combines
two distinct proposals under one label.

**Evidence:**
`design/performance-architecture-proposals.md:19, 21, 34, 50, 95, 129, 166, 190, 217, 221`.

**Fix:** Renumber proposals as `§2.1 Proposal 1`, `§2.2 Proposal 2`,
`§2.3 Proposal 3 (LMDB Checkpoints)`, `§2.4 Proposal 4 (Fast-path linked)`,
`§2.5 Proposal 5 (Inline Spool)`, `§2.6 Proposal 6 (Decoded Op Cache)`. Update all table entries in
§1 and §3 to cite explicit `§2.X` references.

### 6. (§2, line 78) Trail cites OpenGL buffer instead of native trail

Full title: Proposal 1 cites render library buffer and net lens ring buffer while omitting
`ArenaManifold`'s native WAM conditional trail

**Problem:** In §2.1 (Proposal 1, lines 78-83), the document seeks architectural precedent for an
execution undo trail and cites `NetStreamRing` (`apps/common/xanadu/zigzag/net_lens.hpp:75-92`) and
`StreamBufferGL` (`include/gleditor/render/gl/stream_buffer.hpp`). Citing `StreamBufferGL` violates
layer separation (generic OpenGL renderer in `gleditor` library cited for a core VM execution
model). More critically, the text completely ignores the fact that `ArenaManifold` already
implements an in-tree WAM conditional undo trail in the same subsystem
(`apps/common/xanadu/zigzag/arena_manifold.hpp:939-965`, `trail_`, `trailSize`, `trail(dense)`).
`ArenaManifold`'s conditional trailing mechanism is the exact architectural pattern that `VortexVM`
should mirror or integrate with.

**Evidence:** `design/performance-architecture-proposals.md:78-83` vs
`apps/common/xanadu/zigzag/arena_manifold.hpp:939-965`.

**Fix:** Remove the reference to `StreamBufferGL`. Cite `ArenaManifold::trail_`
(`apps/common/xanadu/zigzag/arena_manifold.hpp:939-965`) as the primary architectural model for
conditional WAM trailing in `VortexVM`.

## MINOR

### 1. (§2, line 60) Inaccurate line range citation for vortex_frames.cpp

Full title: Citation `vortex_frames.cpp:620-625` misidentifies returnFrame container copies

**Problem:** Line 60 cites `vortex_frames.cpp:620-625` for copying full maps on frame return. Lines
620-623 actually perform result cell validation (`if (port.kind == FramePortKind::Cell ...)`). The
full map copies occur at `vortex_frames.cpp:625-632`.

**Evidence:** `apps/common/xanadu/vortex/vortex_frames.cpp:620-635`.

**Fix:** Update citation to `vortex_frames.cpp:625-632`.

### 2. (§2, line 177) Factually incorrect claim that Spool size() is out-of-line

Full title: Proposal 4 claims `SegmentedOpsSpool::size()` needs moving to header when it is already
inline

**Problem:** Line 177 states: "Move `SegmentedOpsSpool::get(std::uint32_t)` (and
`SegmentedOpsSpool::size()` / `ArenaManifold::slot()`) into their headers as `inline` methods".
`SegmentedOpsSpool::size()` is already defined inline in `segmented_ops_spool.hpp:290`
(`[[nodiscard]] std::size_t size() const { return opCount; }`).

**Evidence:** `apps/common/xanadu/segmented_ops_spool.hpp:290`.

**Fix:** Remove `SegmentedOpsSpool::size()` from the list of methods to be moved into headers.

### 3. (§2, line 178) Misnamed arena class and unmentioned rawOps()

Full title: Citation of non-existent `MmapArena` instead of `VirtualMemoryArena` and omission of
existing `rawOps()`

**Problem:** Line 178 suggests "exposing `base_` on `MmapArena` or caching
`const CompactOpNode *nodesBase_` in `SegmentedOpsSpool`". `SegmentedOpsSpool` does not use
`MmapArena`; it uses `VirtualMemoryArena arena;` (`segmented_ops_spool.hpp:431`). Furthermore,
`SegmentedOpsSpool` already provides an inline public accessor `rawOps()`
(`segmented_ops_spool.hpp:392-394`) returning
`reinterpret_cast<const CompactOpNode *>(arena.base())`.

**Evidence:** `apps/common/xanadu/segmented_ops_spool.hpp:392-394, 431`.

**Fix:** Revise wording to reference `VirtualMemoryArena` and note that `rawOps()` already exposes
`arena.base()` directly.

### 4. (§2, line 192) Misattributed source file for frameDescriptor

Full title: Proposal 5 cites `vortex_vm.cpp` for `frameDescriptor`, which is defined in
`vortex_frames.cpp`

**Problem:** Lines 192-195 cite `vortex_vm.cpp` and `vortex_core.cpp` for
`VortexVM::frameDescriptor`. While declared in `vortex_vm.hpp:688`, `VortexVM::frameDescriptor` is
defined in `apps/common/xanadu/vortex/vortex_frames.cpp:194`.

**Evidence:** `apps/common/xanadu/vortex/vortex_frames.cpp:194`.

**Fix:** Add `vortex_frames.cpp` to the cited source files for Proposal 5.

### 5. (§2, line 152) Missing definition citation for CellSlot

Full title: Proposal 3 references `CellSlot::flags` without citing its definition in `manifold.hpp`

**Problem:** Line 152 references `CellSlot::flags` ("already 1 byte in `CellSlot`"), but §2.3 only
cites `store.cpp` and `arena_manifold.cpp`. `struct CellSlot` is declared in
`apps/common/xanadu/zigzag/manifold.hpp:321-352`.

**Evidence:** `apps/common/xanadu/zigzag/manifold.hpp:321-352`.

**Fix:** Add `apps/common/xanadu/zigzag/manifold.hpp:321-352` to the cited headers in Proposal 3.

VERDICT: NOT CONVERGED
