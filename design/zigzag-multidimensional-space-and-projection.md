# Zigzag Multidimensional Information Space, 2-Rank Manifolds, and Projection Architecture

An architectural specification and design document for Project Xanadu's
Zigzag multidimensional information model, 2-rank manifold validation,
64-byte compact cell memory layouts, clone cell master headcells, and
bidirectional Xanadoc $\longleftrightarrow$ Zigzag projection across `apps/zigzag`
and `apps/xudu`.

---

## 1. Philosophical Foundations: The Orthogonal Hyper-Grid

Traditional user interfaces trap information in rigid hierarchies (trees,
directories, tables, or linear text streams). In 1997, Theodor Holm Nelson
invented **ZigZag**: a universal, non-hierarchical, multidimensional data
structure.

In Zigzag:
1. **Cells and Dimensions**: All informational atoms are **Cells**. Cells are
   connected along named, orthogonal **Dimensions** (e.g. `d.1`, `d.2`, `d.doc`,
   `d.transclude`, `d.version`).
2. **The 2-Rank Manifold Invariant**: Along any dimension $d$, every cell $C$ has
   **at most one positive neighbor (+1) and at most one negative neighbor (-1)**:
   $$\text{deg}^+(C, d) \le 1, \quad \text{deg}^-(C, d) \le 1$$
3. **Ranks**: Traversing along a single dimension forward and backward traces
   an unambiguous, non-intersecting sequence of cells called a **Rank**.
4. **Clone Cells & Master Headcells**: Data is never duplicated. When the same
   item exists in multiple views or categories, it is cloned along `d.clone`.
   Walking negward along `d.clone` reaches the single canonical **Master
   Headcell**.

```
                                  ▲ +d.2 (Parent Topic)
                                  │
                                  │
        -d.1 (Prev Step) ────────[Cell]──────── +d.1 (Next Step)
                                  │
                                  │
                                  ▼ -d.2 (Sub-topic)
                                 ╱ 
                                ╱ +d.clone (Clone Family)
                               ▼
```

---

## 2. High-Density Memory Architecture: `CompactZZCell`

In `apps/zigzag`, the multidimensional manifold is staged in
[`CompactZZCell`](apps/zigzag/core/compact_zzcell.hpp), designed for cache
efficiency and zero-copy string views:

```cpp
struct CompactZZCell {
  CellID id{0};                  ///< Unique 64-bit cell identifier
  std::uint32_t spoolOpIndex{0}; ///< Offset in xudu::SegmentedOpsSpool
  xudu::PrimediaSpan span{};     ///< Canonical address in primedia spool

  /// Fast inline fixed-size link table for standard dimensions (12 standard dims)
  std::array<LinkPairs, StandardDimensionCount> standardDimensions{};

  /// Dynamic overflow for user-defined dimensions
  std::vector<DynamicDimensionLink> dynamicDimensions{};

  std::optional<Preflet> preflet{};
  std::string type{"cell"};
  std::string ephemeralText{};
  xudu::ResolutionStatus resolutionStatus{xudu::ResolutionStatus::VerifiedBytes};
  std::optional<xudu::TranscopyrightDescriptor> transcopyrightInfo{};
  std::optional<xudu::PublishedHoleRecord> holeRecord{};

  /// Zero-copy text resolution into memory-mapped primedia spool
  [[nodiscard]] std::string_view resolveLocalView(
      const xudu::SegmentedPrimediaSpool &spool) const noexcept;
};
```

### Inlined Standard Dimensions (`DimOrdinal`)
Twelve core dimensions are inlined directly into a flat fixed-size array,
avoiding dynamic heap allocations during navigation:
- `D1`, `D2`, `D3`, `D4`, `D5`: Spatial and logical user dimensions.
- `Doc`: Linear reading sequence of the document.
- `Transclude`: Identity connections between identical primedia spans.
- `OpsTime`: Monotonic append order in the author's primedia spool.
- `OpsDag`: Ancestral branching graph in the operation DAG.
- `Version`: Microversion evolution.
- `Link`: Explicit xanalink connections.
- `Clone`: Clone family rank linking instances to the master headcell.

---

## 3. 2-Rank Manifold Validation: $O(C \times D)$ Verification

To guarantee that the multidimensional manifold never degenerates into a
corrupted graph, [`UnifiedTransclusionEngine::validate2RankManifold()`](apps/zigzag/core/unified_transclusion_engine.cpp)
validates topological symmetry across all active cells $\mathcal{C}$ and
dimensions $\mathcal{D}$:

$$\forall c \in \mathcal{C}, \forall d \in \mathcal{D}: \quad c.\text{links}[d].\text{pos} = t \iff t.\text{links}[d].\text{neg} = c$$

```cpp
bool UnifiedTransclusionEngine::validate2RankManifold(
    const std::vector<CompactZZCell> &cells,
    std::string *outError) {
  // Verifies that:
  // 1. No cell has dangling target pointers.
  // 2. Positive links have exact corresponding negative backlinks.
  // 3. In-degree and out-degree per dimension are <= 1.
}
```

---

## 4. Clone Cells and Master Headcell Resolution

When a cell is cloned across multiple views or ranks:
1. All instances are chained along the `d.clone` dimension.
2. [`findCloneMaster()`](apps/zigzag/core/zzcore.hpp) walks negward along
   `d.clone` until reaching the root cell with no negative clone neighbor:

```cpp
CellID findCloneMaster(const ZZSpace &space, CellID cellId) {
  CellID current = cellId;
  while (true) {
    const auto prev = space.getNeg(current, "d.clone");
    if (prev == 0 || prev == current) break;
    current = prev;
  }
  return current;
}
```

3. Edits made to any clone instance update the master headcell's primedia span,
   instantly propagating changes across every view without data divergence.

---

## 5. Bidirectional Projection Architecture (Xudu $\longleftrightarrow$ Zigzag)

`apps/zigzag` and `apps/xudu` are fully isomorphic: any Xanadoc can be
projected into an N-dimensional Zigzag space, and any Zigzag manifold can be
linearized into readable Xanadoc text.

```mermaid
graph LR
    Xanadoc["Xanadoc Store / EDL<br/>(Primedia + Microversions)"]
    Projector["zz_xudu_projector<br/>(Bidirectional Projector)"]
    Zigzag["Zigzag Manifold<br/>(CompactZZCells on Ranks)"]
    Package["LinkPackage<br/>(LinkType::Dimension)"]

    Xanadoc -->|projectStoreToZigzag()| Zigzag
    Zigzag -->|rasterizeZzStructure()| Xanadoc
    Zigzag -->|zzStructureToLinkPackage()| Package
    Package -->|Ingest into Store| Xanadoc
```

### 1. Xanadoc $\to$ Zigzag (`projectStoreToZigzag`)
- Paragraphs and spans become `CompactZZCell` nodes.
- Sequential reading order maps to `d.doc`.
- Shared primedia spans map to `d.transclude`.
- Operation DAG branches map to `d.version` and `d.ops_dag`.
- Unchanged spans across document revisions become **Clone Cells** linked on
  `d.clone`.

### 2. Zigzag $\to$ Xanadoc (`rasterizeZzStructure`)
- A 2D projection plane (selected by primary axis $X$ and secondary axis $Y$) is
  traversed row-by-row.
- Cell texts are concatenated into an Edit Decision List, preserving original
  primedia addresses.

### 3. Portable Link Packages (`zzStructureToLinkPackage`)
- Multidimensional Zigzag structures are serialized into signed `LinkPackage`
  bundles where dimensional links are preserved as `LinkType::Dimension`
  xanalinks.

---

## 6. High-Throughput 120 FPS GPU Staging Pipeline

To render large multidimensional cell meshes at 120 FPS ($8.33\text{ms}$):
1. **Radial Neighborhood Extraction**: [`stageVisibleCells()`](apps/zigzag/core/unified_transclusion_engine.cpp)
   extracts visible cells within a bounded radius $(R_x, R_y, R_z)$ around the
   focus cell.
2. **Text Layout & Glyph Caching**: Cell text is shaped via `TextLayout` and
   rasterized into the dynamic glyph atlas.
3. **Instance Quad Assembly**: Assembles packed 24-byte `Doc::VBORow` instances.
4. **Persistent Ring Staging**: [`stageIntoStreamBuffer()`](apps/zigzag/core/unified_transclusion_engine.cpp)
   copies instance buffers directly into persistent mapped [`StreamBufferGL`](include/gleditor/render/gl/stream_buffer.hpp)
   memory, issuing single-call instanced GPU draws.

---

## 7. Implementation File Map

| Component | Source Files | Description |
| :--- | :--- | :--- |
| **Compact Cell Layout** | [`apps/zigzag/core/compact_zzcell.hpp`](apps/zigzag/core/compact_zzcell.hpp) | 64-byte aligned multidimensional cell with inlined standard dimensions |
| **Transclusion Engine** | [`apps/zigzag/core/unified_transclusion_engine.hpp/.cpp`](apps/zigzag/core/unified_transclusion_engine.hpp) | 2-rank manifold validator, neighborhood extractor, and GPU uploader |
| **Bidirectional Projector** | [`apps/zigzag/core/zz_xudu_projector.hpp/.cpp`](apps/zigzag/core/zz_xudu_projector.hpp) | Xanadoc $\longleftrightarrow$ Zigzag mapping, clone deduplication, and rasterization |
| **Zigzag Core Data Model** | [`apps/zigzag/core/zzcore.hpp/.cpp`](apps/zigzag/core/zzcore.hpp) | Dimensional navigation, clone master resolution, and rank iterators |
| **YAML Slice Loader** | [`apps/zigzag/core/zzstructure_loader.cpp`](apps/zigzag/core/zzstructure_loader.cpp) | RapidYAML parser for `.zz` multidimensional slice files |
| **Visualizer & A11y** | [`apps/zigzag/core/zigzag_visualizer.cpp`](apps/zigzag/core/zigzag_visualizer.cpp) | 3D navigation, mouse picking, and AccessKit accessibility tree |
| **Unit Tests** | [`tests/zigzag/test_unified_transclusion_engine.cpp`](tests/zigzag/test_unified_transclusion_engine.cpp), [`tests/zigzag/test_xudu_convergence.cpp`](tests/zigzag/test_xudu_convergence.cpp) | Manifold validation, clone syncing, and round-trip tests |
