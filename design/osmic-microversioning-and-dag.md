# OSMIC Microversioning, Hypertime Branching, and Operation DAG Architecture

An architectural specification and design document for the non-destructive OSMIC versioning model,
bijective base-26 hypertime branching, 64-byte cache-aligned operation DAGs, and virtual memory
spool management across `xudu` and `gleditor`.

______________________________________________________________________

## 1. Philosophical Foundations: The OSMIC Versioning Paradigm

In conventional word processors and editors (the "Bleditor" legacy), document history is modeled as
a linear, destructive sequence of undo/redo states. If a user steps back several revisions and types
a new character, the future branch is permanently deleted.

Project Xanadu and Theodor Holm Nelson's **OSMIC (Operating System for Multidimensional,
Intertwingled Content)** model fundamentally inverts this:

1. **Time is a Non-Destructive Directed Acyclic Graph (DAG)**: Every edit, deletion, rearrangement,
   and transclusion is an immutable historical event. No version is ever destroyed or overwritten.
1. **The Server Stores Zero Document Snapshots**: Documents are not saved as full text snapshots. A
   document version is an Edit Decision List (EDL) regenerated on-the-fly by replaying the operation
   lineage from state zero.
1. **Bijective Self-Reconstituting Nomenclature**: Version identifiers (e.g. `1`, `2`, `2a1`, `2a2`,
   `2b1`) are not arbitrary database UUIDs; they are self-executing historical algorithms that
   encode their complete lineage back to the null document.

```
                       State 0 (The Null Document)
                                    │
                                 State 1 (Insert Span A)
                                    │
                                 State 2 (Insert Span B)
                                ┌───┴────────────────┐
                     State 2a1 (Rearrange)       State 2b1 (Insert Span C)
                                │                    │
                     State 2a2 (Delete/Limbo)    State 2b2 (Transclude)
                                │
                     State 2a3 (Link Comment)
                                │
                     State 2a4 (Final Edition)
```

______________________________________________________________________

## 2. Microversion Nomenclature and Hypertime Branching

In `xudu`, version identity is encapsulated by
[`MicroversionId`](apps/common/xanadu/microversion.hpp).

### Bijective Base-26 Branching

A version name consists of alternating numeric sequences and alphabetic branch segments:

- Mainline evolution proceeds as monotonic integers: `1` $\to$ `2` $\to$ `3`.
- Forking a branch off state `2` appends a branch segment: `2a1`, `2a2`.
- Subsequent sibling forks off state `2` increment the branch ordinal: `2b1`, `2c1` ... `2z1`,
  `2aa1` ($27^{\text{th}}$ fork).
- Deep branching continues hierarchically: `2a4b1c3`.

```cpp
struct Segment {
  std::uint32_t branch{0}; // 0 for initial integer chain; 1 for 'a', 27 for 'aa'
  std::uint32_t number{0}; // Monotonic operation index within this branch
};
```

### Ancestral Lineage Resolution

The history of any version is resolved analytically without database indexing:

- `parent()`: Computes the immediate predecessor state (e.g. `2a4` $\to$ `2a3`, `2a1` $\to$ `2`).
- `next()`: Computes the next sequential state on the current branch (`2a4` $\to$ `2a5`).
- `branch(ordinal)`: Forks a new branch (`ordinal=1` produces `'a'`, `ordinal=27` produces `'aa'`).
- `path()`: Returns the complete vector of microversions from state `0` to the target version:

$$
\text{path}(2\text{a}3) = [1, 2, 2\text{a}1, 2\text{a}2, 2\text{a}3]
$$

______________________________________________________________________

## 3. The 64-Byte Cache-Aligned Operation DAG: `CompactOpNode`

To achieve high-throughput graph traversal during live editing and 120 FPS rendering, every node in
the operation DAG is encoded as a strictly 64-byte POD struct
([`CompactOpNode`](apps/common/xanadu/compact_op.hpp)) aligned to CPU cache lines:

```cpp
struct alignas(64) CompactOpNode {
  // Tree topology & metadata (8 bytes)
  std::uint32_t parentIndex{0};      ///< Index of parent node in arena (0 for root)
  OpKind kind{OpKind::Insert};       ///< Operation type enum
  std::uint8_t flags{0};             ///< Reserved bit flags
  std::uint16_t branchOrdinal{0};    ///< Bijective base-26 branch ordinal

  // Position & geometry coordinates (24 bytes)
  std::uint32_t at{0};               ///< Concatext offset in target version
  std::uint32_t length{0};           ///< Delete/Rearrange span length
  std::uint32_t to{0};               ///< Rearrange destination offset
  std::uint32_t sourceAt{0};         ///< Transclude source offset
  std::uint32_t sourceLength{0};     ///< Transclude source length
  std::uint32_t sourceOpIndex{0};    ///< Transclude source version index

  // Content span, link reference & typed value (32 bytes)
  ScrollId scrollId{localScroll};    ///< Scroll ID (0 = local author spool)
  std::uint32_t linkId{0};           ///< Link ID for OpKind::Link
  std::uint64_t spanStart{0};        ///< Byte start in primedia spool
  std::uint64_t spanLength{0};       ///< Byte length in primedia spool
  std::uint64_t value{0};            ///< Canonical scalar bits; zero until R6 lands
};
static_assert(sizeof(CompactOpNode) == 64);
static_assert(alignof(CompactOpNode) == 64);
static_assert(offsetof(CompactOpNode, value) == 56);
```

### Mechanical Sympathy

- **Zero Cache Line Split**: Single-node reads generate exactly one 64-byte memory fetch.
- **Pointerless Graph Addressing**: Relationships are expressed as 32-bit array indices into a flat
  virtual memory arena, eliminating 64-bit pointer overhead and heap fragmentation.
- **Immutable once stored**: only the edge pointing *up* lives in the node. `firstChildIndex` and
  `nextSiblingIndex` used to sit beside `parentIndex`, which made filing a new operation under an
  existing parent a write into that parent — and `adoptSegmentNodes()` maps a page-aligned sealed
  segment `PROT_READ`, so the write was a SIGSEGV waiting for the first segment that happened to
  land on a page boundary. Both are derivable from `parentIndex`, so they moved to
  `SegmentedOpsSpool::tree`, a side array index-aligned with the nodes and rebuilt on adopt in
  ascending index order — which reproduces exactly the sibling order `append()` produced, because a
  parent always sits at a lower index than its children. Eight bytes per operation either way.
- **The offset assertion is not redundant**: `alignas(64)` pads a shrunken struct back up to 64 on
  its own, so `sizeof == 64` cannot notice a field going missing. Pinning `value` to offset 56 can.

The eight bytes the tree edges vacated are `value`, an 8-aligned slot for a cell's canonical scalar
bits. It is written zero today; see [`store-slice-convergence.md`](store-slice-convergence.md) §5.2
for what fills it, and R11 there for why the slots were spent rather than reserved.

______________________________________________________________________

## 4. Hyperops: The Non-Destructive Edit Set

`xudu` implements Nelson's formal OSMIC hyper-operations:

| Hyperop (`OpKind`)   | Description                                        | Semantic Invariant                                                                                    |
| :------------------- | :------------------------------------------------- | :---------------------------------------------------------------------------------------------------- |
| `OpKind::Insert`     | Inserts a new primedia span into the document.     | Primedia is appended to the sovereign spool; the version inserts a pointer.                           |
| `OpKind::Delete`     | Removes a range from the active document.          | **Rearrange to Limbo**: Primedia is never destroyed; the version stops referencing those coordinates. |
| `OpKind::Rearrange`  | Moves a range of text to a new offset.             | Pieces are permuted in the EDL without duplicating bytes.                                             |
| `OpKind::Transclude` | Quotes a span from an existing document or scroll. | References the original author's coordinate tuple $(\text{ScrollId}, \text{Offset}, \text{Length})$.  |
| `OpKind::Link`       | Asserts a butterfly link.                          | Connects Left List and Right List spans.                                                              |
| `OpKind::PageBreak`  | Inserts a structural page break.                   | Concatext-relative layout marker with no primedia footprint.                                          |

### The sixth hyperop, and what `PageBreak` turns out to be

OSMIC names six. The five above are the five `xudu` implements; the sixth — **MAKE/CHANGE STRUCTURE
MAP** — has never been implemented anywhere, including in Udanax. It is the operation that says
where a piece of content *sits* in a structure that is not the document's own reading order: a rank,
an axis, a dimension.

`OpKind::PageBreak` is that operation, restricted to one dimension. A break says "the text divides
here" without naming any primedia — it is a fact about arrangement rather than about content, which
is exactly why it has to be concatext-relative and address-less, and exactly why a transcluded
passage does not carry the source's breaks with it. Give the same operation a dimension to name and
a cell to name it about, and it stops being a special case: pagination becomes one rank among many.

That is the argument [`store-slice-convergence.md`](store-slice-convergence.md) makes, and the
reason it does not retire `PageBreak` afterwards: a break's position must be *rebased* by ordinary
edits to the text around it, and nothing else in the model needs that, so deleting the special case
would mean writing a position-rebasing subsystem to replace it.

______________________________________________________________________

## 5. Virtual Memory Arena and Segment Management

The operations spool ([`SegmentedOpsSpool`](apps/common/xanadu/segmented_ops_spool.hpp)) and
primedia spool ([`SegmentedPrimediaSpool`](apps/common/xanadu/segmented_primedia_spool.hpp)) are
managed via a multi-tiered virtual address layout
([`VirtualMemoryArena`](apps/common/xanadu/virtual_memory_arena.hpp)):

```
┌────────────────────────────────────────────────────────────────────────┐
│               512 MB Reserved Virtual Address Space                   │
├──────────────────┬──────────────────┬──────────────────┬───────────────┤
│ Segment 0 (mmap) │ Segment 1 (mmap) │ Active (mprotect)│ Uncommitted   │
│ Sealed Torrent 0 │ Sealed Torrent 1 │ Read/Write Spool │ PROT_NONE     │
│ [0 .. 64 KiB]    │ [64 .. 128 KiB]  │ [128 .. 192 KiB] │ [192 .. 512M] │
└──────────────────┴──────────────────┴──────────────────┴───────────────┘
```

1. **512 MB Virtual Reservation**: On startup, `VirtualMemoryArena::reserve()` allocates a 512 MB
   virtual address window using `mmap(PROT_NONE, MAP_ANONYMOUS)`. Physical RAM is allocated only as
   pages are committed.
1. **Zero-Copy Multi-Segment Slicing (`MAP_FIXED`)**: When historical torrent segments are ingested
   from the network, sealed node files are mapped directly into target page-aligned offsets of the
   contiguous virtual arena using
   `mmap(targetAddr, len, PROT_READ, MAP_SHARED | MAP_FIXED, fd, offset)`.
1. **Open-Addressing Hash Index**: To locate microversions in $O(1)$ time with zero heap
   allocations, `SegmentedOpsSpool` maintains an open-addressing linear probing hash table
   (`idHashSlots`) indexed by 64-bit FNV-1a hashes of `MicroversionId::Segment` records.

### Segment files, and the header they are getting

A segment file today is a bare run of `CompactOpNode`s: no header, no state-zero slot, and no
microversion names anywhere in it. The names come back out of the tree, each node saying which index
produced it and by which branch ordinal, so a segment is written and read back in the same order and
the indices inside it are the ones they had when it was sealed.

That works, and it has one failure mode that is worse than not working: **a file written under an
older node layout does not fail to load, it loads and means something else.** With nothing at the
front of the file to identify it, `st.st_size % sizeof(CompactOpNode) == 0` is the only check
available, and it stays true across any layout change that keeps the node 64 bytes.

R14 of [`store-slice-convergence.md`](store-slice-convergence.md) fixes this with a header: a
twelve-byte PNG-style signature that survives being probed and fails loudly when a transport mangles
it, a format version, and — the field that would have caught the actual incident — the `nodeSize`
the writer believed in. The header is exactly one 64 KiB Merkle piece, which is forced rather than
chosen: `mmap` needs a page-aligned file offset for the nodes that follow it, and 64 KiB is the
smallest size that is a whole number of pages on 4 KiB, 16 KiB and 64 KiB systems alike. It also
keeps node boundaries on piece boundaries, so the header is piece 0 and the nodes are pieces 1..N.

______________________________________________________________________

## 6. Historical Delta Materialization Pipeline

```mermaid
graph TD
    Target["Target Microversion (e.g. 2a4)"]
    Spool["SegmentedOpsSpool::ancestralPath()"]
    Replay["Store::replay() Engine"]
    EDL["Materialized Version (runs of PrimediaSpan)"]

    Target -->|O(depth) Index Walk| Spool
    Spool -->|Ancestral CompactOpNodes| Replay
    Replay -->|Incremental Splits & Joins| EDL
```

1. **Full Ancestral Reconstitution**:
   - `ancestralPath(targetIndex)` follows `parentIndex` pointers backwards to state `0`, reversing
     the array in $O(\text{depth})$ time.
   - `Store::replay()` initializes an empty `Version` and applies each `CompactOpNode` in sequential
     order.
1. **$O(1)$ Forward Delta Fast-Path**:
   - When the user types a keystroke, the current version is updated in-place:

     $$
     \text{known} == \text{version}.\text{parent}() \implies \text{Apply single terminal Op}
     $$

   - Keystroke latency remains under $0.5\ \mu\text{s}$ regardless of total document length.

______________________________________________________________________

## 7. Incremental Publishing & History Reconstruction

### Binary Export (`sealableOps` / `exportBinaryOps`)

When an author publishes a revision:

1. `sealableOps()` builds an external scroll translation table mapping local `ScrollId` indices to
   permanent global keys (`btpk:<pubkey>:<salt>`).
1. `exportBinaryOps()` serializes newly added operations into an ultra-compact binary stream using
   LEB128 varints and sequential name suppression (`FLAG_SEQUENTIAL`).

### Remote Store Ingestion (`historyFromSeal`)

A reader downloading sealed operation segments reconstructs an independent, sovereign `Store`:

1. Maps `ScrollId 0` to the author's published permascroll.
1. Replays the binary operations into the local `SegmentedOpsSpool`.
1. Preserves the author's original hypertime graph names and branch topology.

______________________________________________________________________

## 8. Implementation File Map

| Component                    | Source Files                                                                                       | Description                                                     |
| :--------------------------- | :------------------------------------------------------------------------------------------------- | :-------------------------------------------------------------- |
| **Microversion ID**          | [`apps/common/xanadu/microversion.hpp/.cpp`](apps/common/xanadu/microversion.hpp)                  | Bijective base-26 branch parser, formatter, and lineage algebra |
| **Compact Op Node**          | [`apps/common/xanadu/compact_op.hpp`](apps/common/xanadu/compact_op.hpp)                           | 64-byte cache-line aligned POD operation struct                 |
| **Ops Spool**                | [`apps/common/xanadu/segmented_ops_spool.hpp/.cpp`](apps/common/xanadu/segmented_ops_spool.hpp)    | Virtual memory arena, open-addressing index, and persistence    |
| **Version Model**            | [`apps/common/xanadu/version.hpp/.cpp`](apps/common/xanadu/version.hpp)                            | Edit Decision List (EDL) piece table, splits, and coalescing    |
| **Store Engine**             | [`apps/common/xanadu/store.hpp/.cpp`](apps/common/xanadu/store.hpp)                                | Transactional store, operation replay, and delta forward paths  |
| **Binary Ops Serialization** | [`apps/common/xanadu/binary_ops.hpp/.cpp`](apps/common/xanadu/binary_ops.hpp)                      | LEB128 varint binary format for sealed operations               |
| **Unit Tests**               | [`tests/xudu/store.cpp`](tests/xudu/store.cpp), [`tests/xudu/version.cpp`](tests/xudu/version.cpp) | Test suites covering branching, replay, and serialization       |
