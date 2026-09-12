# The True Spanfilade: Interval B-Enfilade for Permascroll & Transclusion Indexing

This document specifies the architecture, interval monoid algebra, and runtime mechanics of the
Spanfilade in `apps/common/xanadu/enfilade/spanfilade.hpp` and `.cpp`.

The Spanfilade provides logarithmic-time interval stabbing, transclusion pairing, and multi-document
quotation discovery over immutable permascroll addresses, replacing (O(N)) linear scans while
strictly upholding Nelsonian transclusion semantics and architectural rulings R8, R9, R12, U3, and
V3.

______________________________________________________________________

## 1. Nelsonian Grounding & Theoretical Context

In Theodor Holm Nelson's Project Xanadu architecture, documents are not containers of raw
characters, but virtual windows—Edit Decision Lists (EDLs)—pointing into a universal, write-only
permascroll. When multiple documents quote or transclude content from the permascroll, the central
operation of the docuverse is **transclusion discovery**: finding every occurrence of a given
primedia address across all open documents and Zigzag cells.

Historically, this was the domain of the **Spanfilade**: a 1D interval enfilade whose keys are
permascroll spans and whose values point to document offsets or cell identifiers. While U1
specifications introduced B-enfilades for 1D rank navigation in Zigzag dimensions, transclusion
detection remained an (O(N)) linear scan across piece vectors.

The True Spanfilade realizes this foundational primitive as an in-memory, cache-conscious
B-enfilade.

______________________________________________________________________

## 2. Interval Monoid Algebra

The Spanfilade structures 1D interval spans into a hierarchy of bounding boxes using the interval
monoid ((Dsp, Wid)).

### 2.1 The Span Monoid: `SpanDsp` and `SpanWid`

- **`SpanDsp`**: The relative displacement along the permascroll coordinate axis.
- **`SpanWid`**: The span width encompassing all subordinate intervals under a given subtree.

For two intervals or bounding boxes (A = (s_A, l_A)) and (B = (s_B, l_B)), their monoidal
combination is the minimal bounding interval containing both:

```text
min_start = min(s_A, s_B)
max_end   = max(s_A + l_A, s_B + l_B)
combined  = (min_start, max_end - min_start)
```

The monoid identity element is the empty span:

```text
Identity = SpanWid { minStart = UINT64_MAX, maxEnd = 0 }
```

Satisfying the monoid axioms:

1. **Identity**: (\\text{combine}(e, A) = \\text{combine}(A, e) = A)
1. **Associativity**: (\\text{combine}(A, \\text{combine}(B, C)) =
   \\text{combine}(\\text{combine}(A, B), C))

### 2.2 Relative Coordinate Action

Crums store relative displacements (`SpanDsp`) from their parent node. A descending tree walk
accumulates displacements:

```text
absolute_coord = parent_coord + dsp.offset
```

This ensures coordinate relativity: subtrees can be shifted, spliced, or rebalanced without updating
the absolute coordinates of all child nodes.

______________________________________________________________________

## 3. Data Structure & Memory Layout

Modern CPU architectures penalize traditional 2-3 pointer trees with severe L1/L2 cache misses. The
Spanfilade adopts a flat B-enfilade architecture optimized for cache line locality:

```text
                    +------------------------------------+
                    |  Spanfilade (Multi-Scroll Table)   |
                    |  std::unordered_map<ScrollId, ...> |
                    +-----------------+------------------+
                                      |
                                      v
                     +----------------------------------+
                     |        ScrollSpanfilade          |
                     |  std::vector<SpanCrum> crums_   |
                     |  std::vector<SpanEntry> entries_ |
                     +----------------------------------+
```

### 3.1 Crum Node Layout (`SpanCrum`)

Internal routing nodes are organized with a branching factor (B = 16):

- `SpanDsp dsp`: Relative offset to parent frame.
- `SpanWid wid`: Monoidal bounding interval `[minStart, maxEnd)`.
- `std::uint32_t firstChild`: Index of the first child crum in `crums_`.
- `std::uint16_t childCount`: Number of child crums ([0, 16]).
- `std::uint32_t firstEntry`: Index of first leaf entry in `entries_`.
- `std::uint16_t entryCount`: Number of leaf entries ([0, 16]).

### 3.2 Leaf Entry Layout (`SpanEntry`)

Leaf nodes store compact interval targets:

- `std::uint64_t start`: Start offset in permascroll.
- `std::uint64_t length`: Length of primedia span.
- `std::uint32_t docId`: Document or view identifier.
- `std::uint32_t docOffset`: Offset within the document where the span appears.
- `std::uint32_t cellDense`: Dense index of Zigzag cell (if indexing a Manifold).
- `std::uint16_t spanIndex`: Span index within cell `contentArena` (supporting U3 multi-span cells).

______________________________________________________________________

## 4. Range Stabbing & Transclusion Pairing

Transclusion discovery uses range stabbing across the bounding interval tree.

### 4.1 Pruning Condition

During a walk, if the query interval (\[Q\_{\\text{start}}, Q\_{\\text{end}})) does not overlap the
accumulated bounding interval of a crum:

```text
overlap = max(Q_start, node.minStart) < min(Q_end, node.maxEnd)
```

If `overlap` is false, the entire subtree of up to (16^k) entries is pruned immediately.

### 4.2 Transclusion Pairing

When computing transclusion bridges across multiple open documents (such as between two columns in
Xudu or across connected Zigzag cells):

1. Construct a unified `Spanfilade` across all active views in (O(N \\log N)).
1. For each view, query its spans against the Spanfilade.
1. Overlapping spans from different documents form a `TransclusionPair`.
1. Transclusions are found in (O(K \\log N + M)), where (K) is the number of spans and (M) is the
   number of transclusion intersections, down from (O(N^2)).

______________________________________________________________________

## 5. Architectural Ruling Compliance

| Ruling       | Constraint                                  | Spanfilade Compliance                                                                                                                                             |
| ------------ | ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **R8**       | Navigation & indexing must never mint ops   | Spanfilade is strictly an ephemeral replay product. It never modifies the `SegmentedOpsSpool` or mints microversions.                                             |
| **R9**       | Equivalence with linear scan                | `verifyAgainstLinearScan()` validates that `Spanfilade::occurrencesOf()` matches `Version::occurrencesOf()` and `xanadu::placeTransclusions()` to the exact byte. |
| **R12 & U3** | `sizeof(CellSlot) == 32` & multi-span cells | Spanfilade references cells via dense index and records `(cellDense, spanIndex)` without enlarging `CellSlot`.                                                    |
| **V3**       | Transient structure lifecycle               | Spanfilade does not persist to disk. It is constructed on demand and discarded or rebuilt during store compaction.                                                |

______________________________________________________________________

## 6. Performance Benchmarks

Microbenchmarks executed on Linux x86_64 (`tests/xudu/spanfilade_benchmark_test.cpp`) demonstrate
decisive improvements over legacy linear scans:

### 6.1 Query Stabbing Speedup ((N = 500) pieces, 5,000 queries)

- **Legacy Linear Scan (`Version::occurrencesOf`)**: (4,891\\ \\mu\\text{s})
- **True Spanfilade Stabbing (`Spanfilade::occurrencesOf`)**: (542\\ \\mu\\text{s})
- **Performance Factor**: **(9.02\\times) speedup**

### 6.2 Scaling Characteristics

As document size grows from (10^2) to (10^5) pieces, legacy scans scale at (O(N)), requiring full
linear sweeps for every cursor step and transclusion beam layout. The Spanfilade scales at
(O(\\log\_{16} N + K)), ensuring fluid 120 FPS rendering even in heavily edited,
multi-thousand-piece xanadocs.
