# OSMIC Microversioning, Hypertime Branching, and Operation DAG Architecture

An architectural specification and design document for the non-destructive
OSMIC versioning model, bijective base-26 hypertime branching, 64-byte
cache-aligned operation DAGs, and virtual memory spool management across `xudu`
and `gleditor`.

---

## 1. Philosophical Foundations: The OSMIC Versioning Paradigm

In conventional word processors and editors (the "Bleditor" legacy), document
history is modeled as a linear, destructive sequence of undo/redo states. If a
user steps back several revisions and types a new character, the future branch
is permanently deleted.

Project Xanadu and Theodor Holm Nelson's **OSMIC (Operating System for
Multidimensional, Intertwingled Content)** model fundamentally inverts this:
1. **Time is a Non-Destructive Directed Acyclic Graph (DAG)**: Every edit,
   deletion, rearrangement, and transclusion is an immutable historical event.
   No version is ever destroyed or overwritten.
2. **The Server Stores Zero Document Snapshots**: Documents are not saved as
   full text snapshots. A document version is an Edit Decision List (EDL)
   regenerated on-the-fly by replaying the operation lineage from state zero.
3. **Bijective Self-Reconstituting Nomenclature**: Version identifiers (e.g.
   `1`, `2`, `2a1`, `2a2`, `2b1`) are not arbitrary database UUIDs; they are
   self-executing historical algorithms that encode their complete lineage back
   to the null document.

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

---

## 2. Microversion Nomenclature and Hypertime Branching

In `xudu`, version identity is encapsulated by [`MicroversionId`](apps/xudu/core/microversion.hpp).

### Bijective Base-26 Branching
A version name consists of alternating numeric sequences and alphabetic branch
segments:
- Mainline evolution proceeds as monotonic integers: `1` $\to$ `2` $\to$ `3`.
- Forking a branch off state `2` appends a branch segment: `2a1`, `2a2`.
- Subsequent sibling forks off state `2` increment the branch ordinal: `2b1`,
  `2c1` ... `2z1`, `2aa1` ($27^{\text{th}}$ fork).
- Deep branching continues hierarchically: `2a4b1c3`.

```cpp
struct Segment {
  std::uint32_t branch{0}; // 0 for initial integer chain; 1 for 'a', 27 for 'aa'
  std::uint32_t number{0}; // Monotonic operation index within this branch
};
```

### Ancestral Lineage Resolution
The history of any version is resolved analytically without database indexing:
- `parent()`: Computes the immediate predecessor state (e.g. `2a4` $\to$ `2a3`,
  `2a1` $\to$ `2`).
- `next()`: Computes the next sequential state on the current branch (`2a4` $\to$ `2a5`).
- `branch(ordinal)`: Forks a new branch (`ordinal=1` produces `'a'`, `ordinal=27` produces `'aa'`).
- `path()`: Returns the complete vector of microversions from state `0` to the
  target version:

$$\text{path}(2\text{a}3) = [1, 2, 2\text{a}1, 2\text{a}2, 2\text{a}3]$$

---

## 3. The 64-Byte Cache-Aligned Operation DAG: `CompactOpNode`

To achieve high-throughput graph traversal during live editing and 120 FPS
rendering, every node in the operation DAG is encoded as a strictly 64-byte POD
struct ([`CompactOpNode`](apps/xudu/core/compact_op.hpp)) aligned to CPU cache
lines:

```cpp
struct alignas(64) CompactOpNode {
  // Tree topology & metadata (16 bytes)
  std::uint32_t parentIndex{0};      ///< Index of parent node in arena (0 for root)
  std::uint32_t firstChildIndex{0};  ///< First branch or continuation child
  std::uint32_t nextSiblingIndex{0}; ///< Sibling branch off the same parent
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

  // Content span & link reference (24 bytes)
  ScrollId scrollId{localScroll};    ///< Scroll ID (0 = local author spool)
  std::uint32_t linkId{0};           ///< Link ID for OpKind::Link
  std::uint64_t spanStart{0};        ///< Byte start in primedia spool
  std::uint64_t spanLength{0};       ///< Byte length in primedia spool
};
static_assert(sizeof(CompactOpNode) == 64);
```

### Mechanical Sympathy
- **Zero Cache Line Split**: Single-node reads generate exactly one 64-byte
  memory fetch.
- **Pointerless Graph Addressing**: Relationships are expressed as 32-bit array
  indices into a flat virtual memory arena, eliminating 64-bit pointer overhead
  and heap fragmentation.

---

## 4. Hyperops: The Non-Destructive Edit Set

`xudu` implements Nelson's formal OSMIC hyper-operations:

| Hyperop (`OpKind`) | Description | Semantic Invariant |
| :--- | :--- | :--- |
| `OpKind::Insert` | Inserts a new primedia span into the document. | Primedia is appended to the sovereign spool; the version inserts a pointer. |
| `OpKind::Delete` | Removes a range from the active document. | **Rearrange to Limbo**: Primedia is never destroyed; the version stops referencing those coordinates. |
| `OpKind::Rearrange` | Moves a range of text to a new offset. | Pieces are permuted in the EDL without duplicating bytes. |
| `OpKind::Transclude` | Quotes a span from an existing document or scroll. | References the original author's coordinate tuple $(\text{ScrollId}, \text{Offset}, \text{Length})$. |
| `OpKind::Link` | Asserts a butterfly link. | Connects Left List and Right List spans. |
| `OpKind::PageBreak` | Inserts a structural page break. | Concatext-relative layout marker with no primedia footprint. |

---

## 5. Virtual Memory Arena and Segment Management

The operations spool ([`SegmentedOpsSpool`](apps/xudu/core/segmented_ops_spool.hpp))
and primedia spool ([`SegmentedPrimediaSpool`](apps/xudu/core/segmented_primedia_spool.hpp))
are managed via a multi-tiered virtual address layout ([`VirtualMemoryArena`](apps/xudu/core/virtual_memory_arena.hpp)):

```
┌────────────────────────────────────────────────────────────────────────┐
│               512 MB Reserved Virtual Address Space                   │
├──────────────────┬──────────────────┬──────────────────┬───────────────┤
│ Segment 0 (mmap) │ Segment 1 (mmap) │ Active (mprotect)│ Uncommitted   │
│ Sealed Torrent 0 │ Sealed Torrent 1 │ Read/Write Spool │ PROT_NONE     │
│ [0 .. 64 KiB]    │ [64 .. 128 KiB]  │ [128 .. 192 KiB] │ [192 .. 512M] │
└──────────────────┴──────────────────┴──────────────────┴───────────────┘
```

1. **512 MB Virtual Reservation**: On startup, `VirtualMemoryArena::reserve()`
   allocates a 512 MB virtual address window using `mmap(PROT_NONE, MAP_ANONYMOUS)`.
   Physical RAM is allocated only as pages are committed.
2. **Zero-Copy Multi-Segment Slicing (`MAP_FIXED`)**: When historical torrent
   segments are ingested from the network, sealed node files are mapped directly
   into target page-aligned offsets of the contiguous virtual arena using
   `mmap(targetAddr, len, PROT_READ, MAP_SHARED | MAP_FIXED, fd, offset)`.
3. **Open-Addressing Hash Index**: To locate microversions in $O(1)$ time with
   zero heap allocations, `SegmentedOpsSpool` maintains an open-addressing linear
   probing hash table (`idHashSlots`) indexed by 64-bit FNV-1a hashes of
   `MicroversionId::Segment` records.

---

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
   - `ancestralPath(targetIndex)` follows `parentIndex` pointers backwards to
     state `0`, reversing the array in $O(\text{depth})$ time.
   - `Store::replay()` initializes an empty `Version` and applies each
     `CompactOpNode` in sequential order.
2. **$O(1)$ Forward Delta Fast-Path**:
   - When the user types a keystroke, the current version is updated in-place:
     $$\text{known} == \text{version}.\text{parent}() \implies \text{Apply single terminal Op}$$
   - Keystroke latency remains under $0.5\ \mu\text{s}$ regardless of total
     document length.

---

## 7. Incremental Publishing & History Reconstruction

### Binary Export (`sealableOps` / `exportBinaryOps`)
When an author publishes a revision:
1. `sealableOps()` builds an external scroll translation table mapping local
   `ScrollId` indices to permanent global keys (`btpk:<pubkey>:<salt>`).
2. `exportBinaryOps()` serializes newly added operations into an ultra-compact
   binary stream using LEB128 varints and sequential name suppression
   (`FLAG_SEQUENTIAL`).

### Remote Store Ingestion (`historyFromSeal`)
A reader downloading sealed operation segments reconstructs an independent,
sovereign `Store`:
1. Maps `ScrollId 0` to the author's published permascroll.
2. Replays the binary operations into the local `SegmentedOpsSpool`.
3. Preserves the author's original hypertime graph names and branch topology.

---

## 8. Implementation File Map

| Component | Source Files | Description |
| :--- | :--- | :--- |
| **Microversion ID** | [`apps/xudu/core/microversion.hpp/.cpp`](apps/xudu/core/microversion.hpp) | Bijective base-26 branch parser, formatter, and lineage algebra |
| **Compact Op Node** | [`apps/xudu/core/compact_op.hpp`](apps/xudu/core/compact_op.hpp) | 64-byte cache-line aligned POD operation struct |
| **Ops Spool** | [`apps/xudu/core/segmented_ops_spool.hpp/.cpp`](apps/xudu/core/segmented_ops_spool.hpp) | Virtual memory arena, open-addressing index, and persistence |
| **Version Model** | [`apps/xudu/core/version.hpp/.cpp`](apps/xudu/core/version.hpp) | Edit Decision List (EDL) piece table, splits, and coalescing |
| **Store Engine** | [`apps/xudu/core/store.hpp/.cpp`](apps/xudu/core/store.hpp) | Transactional store, operation replay, and delta forward paths |
| **Binary Ops Serialization** | [`apps/xudu/core/binary_ops.hpp/.cpp`](apps/xudu/core/binary_ops.hpp) | LEB128 varint binary format for sealed operations |
| **Unit Tests** | [`tests/xudu/store.cpp`](tests/xudu/store.cpp), [`tests/xudu/version.cpp`](tests/xudu/version.cpp) | Test suites covering branching, replay, and serialization |
