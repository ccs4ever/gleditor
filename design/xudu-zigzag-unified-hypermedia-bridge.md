# Xudu-Zigzag Unified Hypermedia Bridge: Universal Links, Transclusions, and Formatting

**Document Version:** 1.0\
**Related Documents:**

- [`store-slice-convergence.md`](store-slice-convergence.md) — Unified storage, microversion DAG,
  and `CompactOpNode`
- [`links-vs-transclusions-computation-and-optimization.md`](links-vs-transclusions-computation-and-optimization.md)
  — Link topology and transclusion discovery
- [`arena-to-manifold-promotion.md`](arena-to-manifold-promotion.md) — Ephemeral boundaries and
  persistent DAG promotion
- [`enfilade/spanfilade-transclusion-index.md`](enfilade/spanfilade-transclusion-index.md) —
  Sublinear interval B-enfilade range stabbing
- [`ui_workflow_xudu_intertwingle.md`](ui_workflow_xudu_intertwingle.md) — 3D beam ribbons and
  intertwingle layout
- [`ui_workflow_zigzag_multiview.md`](ui_workflow_zigzag_multiview.md) — Multidimensional cell
  projections and view modes

______________________________________________________________________

## 1. Executive Summary & Context

Following the convergence of storage and the OSMIC-backed microversion DAG, both **Xudu** (paginated
linear Xanadocs) and **Zigzag** (multidimensional cell lattices) share the same underlying
`xanadu::Store` and append-only operations spool (`CompactOpNode`).

However, architectural friction points and seams persist across the higher-level subsystems:

1. **Link Layout & Endpoints**: [`link_layout.hpp`](../apps/common/xanadu/link_layout.hpp) hardcodes
   1D linear document offsets (`doc`, `start`, `end`), unable to address discrete 3D manifold cells.
1. **Transclusion Discovery**: [`spanfilade.cpp`](../apps/common/xanadu/enfilade/spanfilade.cpp)
   indexes cells into its leaf nodes, but its transclusion discovery loops explicitly skip cells
   (`if (pI.isCell()) continue;`), preventing cross-domain (Doc $\leftrightarrow$ Cell) or
   cell-to-cell (Cell $\leftrightarrow$ Cell) transclusion discovery.
1. **Formatting Links**: [`LinkType::Format`](../apps/common/xanadu/ops.hpp) presentation attributes
   are parsed in Xudu (`session.cpp`) into `DecoratedRange` and `BlockStyleRange`, but are
   completely ignored during cell text layout in Zigzag's `UnifiedTransclusionEngine` and
   `ZigzagVisualizer`.
1. **Cross-Domain Linking & Navigation**: `LinkForgeWidget` has no mechanism to clasp Zigzag cells,
   and `LinkBeams` only anchors to 2D paged document coordinates rather than 3D cell positions.

This document formalizes the architectural blueprint to bridge these seams into a single, seamless,
content-addressed continuum operating at 120 FPS.

______________________________________________________________________

## 2. Invariants & Guarantees

The bridge design adheres strictly to the foundational architectural rulings of the repository:

### 2.1 Storage Immutability (Zero Format Bumps)

In Nelsonian hypermedia, all links (`OpKind::Link`) attach to immutable `PrimediaSpan` addresses in
the author's permascroll. Because both document pieces and Zigzag cell contents are runs of
`PrimediaSpan`s (`contentOf(cell)`), a cross-domain link is an ordinary Xanadu butterfly link whose
left spans land in a Xanadoc and right spans land in a cell. **No new disk opcodes or spool format
bumps are required.**

### 2.2 System V AMD64 ABI Register-Passing Target

`UniversalLinkEnd` is designed at exactly 16 bytes (`alignas(4)`). Under the System V AMD64 ABI, an
aggregate of $\le 16$ bytes consisting of integer fields is classified as `INTEGER, INTEGER`,
enabling endpoints to be passed across hot loops in two 64-bit general-purpose registers
(`%rsi, %rdx`) with **zero stack spills**.

### 2.3 Sublinear $O(\log_8 N + K)$ Transclusion Discovery

The legacy pairwise $O(N^2)$ nested iteration over open views is replaced with sublinear range
stabbing over the interval B-enfilade (`ScrollSpanfilade`), eliminating quadratic degradation across
large collections of documents and cells.

### 2.4 Lock-Free, Zero-Allocation Format Resolution

Format link attributes are resolved from compile-time `vocabularyScroll` addresses and compressed
into a 16-bit format bitmask (`uint16_t`). This bitmask is stored directly in `CellSlot`'s existing
2 unused padding bytes, incurring **zero cache line bloat** (`sizeof(CellSlot) == 32`) and zero
allocation overhead during 120 FPS text shaping.

______________________________________________________________________

## 3. Architecture Specification

### 3.1 Universal Link Endpoints (`UniversalLinkEnd`)

To represent both linear document concatext and multidimensional cell addresses without breaking
legacy code, `UniversalLinkEnd` unifies both concepts in a 16-byte packed structure:

```cpp
enum class LinkTargetKind : std::uint8_t {
  Document   = 0, ///< 2D Doc concatext byte range
  ZigzagCell = 1, ///< 3D Zigzag CellRef + intra-cell character range
};

struct alignas(4) UniversalLinkEnd {
  std::uint32_t targetId{0};   ///< docIndex if Document, or CellRef if ZigzagCell
  std::uint32_t start{0};      ///< byte offset in doc concatext or cell text
  std::uint32_t length{0};     ///< span byte length
  std::uint16_t spanIndex{0};  ///< index in cell's content run (0 for doc)
  std::uint8_t  kind{0};       ///< LinkTargetKind
  std::uint8_t  flags{0};      ///< bit 0: isWithheld, bit 1: isEphemeral

  [[nodiscard]] constexpr bool isDocument() const noexcept { return kind == 0; }
  [[nodiscard]] constexpr bool isCell() const noexcept { return kind == 1; }
  [[nodiscard]] constexpr std::uint32_t end() const noexcept { return start + length; }

  // Backward-compatibility accessors matching legacy LinkEnd
  [[nodiscard]] constexpr std::uint32_t doc() const noexcept { return targetId; }
  [[nodiscard]] constexpr zigzag::CellRef cell() const noexcept {
    return isCell() ? targetId : zigzag::noCell;
  }
};
static_assert(sizeof(UniversalLinkEnd) == 16);
static_assert(alignof(UniversalLinkEnd) == 4);
```

#### Compact Transclusion & Link Pairs:

- `UniversalTransclusionPair` (24 bytes, 8-byte aligned): `fromId`, `fromOffset`, `toId`,
  `toOffset`, `length`, `scrollId`. Exactly 8 pairs pack cleanly into three 64-byte cache lines (192
  bytes), cutting memory bandwidth by 50% over legacy 48-byte structs.
- `UniversalLinkedPair`: connects `from` and `to` `UniversalLinkEnd`s with `linkId`, `type`, and
  `tier`.
- Legacy aliases (`LinkEnd = UniversalLinkEnd`, `TransclusionPair = UniversalTransclusionPair`)
  ensure that existing call sites in `link_layout.hpp` and `beams.cpp` compile without
  modifications.

______________________________________________________________________

### 3.2 Universal Transclusion Engine

Transclusion discovery in `Spanfilade` is expanded to operate across a unified context:

```cpp
struct UniversalViewContext {
  std::vector<const Version *> docViews;
  std::vector<const zigzag::Manifold *> manifoldViews;
};
```

#### The Universal Stabbing Algorithm:

1. `Spanfilade::indexManifold()` indexes all cell spans into the `ScrollSpanfilade` interval B-tree
   with `.flags = 1U` (`isCell()`) and stores `(cellDense, spanIndex)`.
1. The legacy `if (pI.isCell()) continue;` filter is removed.
1. For each active view, overlapping intervals are stabbed in $O(\log_8 N + K)$ time.
1. Transclusion pairs are emitted across all permutations:
   - **Xanadoc $\leftrightarrow$ Xanadoc** (Linear $\leftrightarrow$ Linear)
   - **Xanadoc $\leftrightarrow$ Zigzag Cell** (Linear $\leftrightarrow$ Multidimensional)
   - **Zigzag Cell $\leftrightarrow$ Zigzag Cell** (Multidimensional $\leftrightarrow$
     Multidimensional)
1. Contiguous adjacent runs along identical scrolls are merged into unbroken transclusion bands.

______________________________________________________________________

### 3.3 Universal Content-Addressed Formatting Engine (`FormatResolver`)

Nelsonian formatting attaches to content via `LinkType::Format` links naming words in
`vocabularyScroll`. `FormatResolver` provides a shared, zero-copy formatting extraction service for
both documents and cells:

```cpp
class FormatResolver {
public:
  struct FormattingResult {
    std::vector<gleditor::DecoratedRange> decoratedRanges;
    std::vector<gleditor::BlockStyleRange> blockStyles;
  };

  explicit FormatResolver(const Store &store) noexcept;

  [[nodiscard]] FormattingResult resolveSpans(
      std::span<const PrimediaSpan> spans) const;
  [[nodiscard]] FormattingResult resolveVersion(
      const Version &version) const;
  [[nodiscard]] FormattingResult resolveCell(
      const zigzag::Manifold &manifold, zigzag::CellRef cell) const;
};
```

#### Fast-Path Cell Layout Integration:

- In `CellSlot`, the 2 unused padding bytes store `std::uint16_t formatFlags{0}` (mapping the 11
  `FormatAttribute`s: Bold, Italic, Underline, AlignCentre, etc.).
- In `UnifiedTransclusionEngine::layoutCellShaped()`, formatting is extracted via `FormatResolver`
  and passed into `gleditor::text::LayoutOptions::decoratedRanges`.
- `stageVisibleCells()` renders cell quads with bold/italic font variants and underline decorations,
  honoring native format links across both application frontends.

______________________________________________________________________

### 3.4 Cross-Domain Link Forging, Optical Beams, & Navigation

```
+---------------------------------------------------------------------------------------+
|                                    Xudu Viewport                                      |
|                                                                                       |
|   [Text Passage]  ---(Click Link Hotspot)--------------------+                        |
|         ^                                                    |                        |
|         |                                                    v                        |
+---------+----------------------------------------------------+------------------------+
          |                                                    |
          | (Golden Transclusion Beams)                        | (Focus Camera on Cell)
          | (3D Optical Link Ribbons)                          |
          v                                                    v
+---------------------------------------------------------------------------------------+
|                                   Zigzag Viewport                                     |
|                                                                                       |
|   [Target Cell]   <------------------------------------------+                        |
|         |                                                                             |
|         +----------(Click Outgoing Link Badge) -> Scroll Xanadoc Viewport             |
+---------------------------------------------------------------------------------------+
```

#### 1. Drag-and-Drop Clasp Assembly (`PouchItem` & `LinkForgeWidget`):

- `PouchItem` is extended with `PouchOriginKind { XanadocDocument, ZigzagCell }`, `originCell`, and
  `originSliceIndex`.
- Users can drag a Xanadoc text selection or a Zigzag cell into Homestead (Left) and Toward (Right)
  clasp slots.
- Forging extracts `item.span` from both sides and writes an ordinary `OpKind::Link` to the store.

#### 2. Two-Tier 3D Optical Beam Anchoring (`LinkBeams`):

- **Tier 1 (Layout Cache)**: When geometry reflows, `LinkBeams::resolveAnchors` computes:
  - Document anchors via `Doc::anchorFor(offset)` projected into 3D world space.
  - Cell anchors via `ZigzagVisualizer::findRenderCell(cell)` extracting cell center coordinates.
  - Caches local anchors keyed by monotonic `doc.layoutGeneration()`.
- **Tier 2 (Transform Pipeline @ 120 FPS)**: On camera motion, SIMD matrix-vector multiplication
  transforms cached anchors into world points ($\approx 15\,\mu\text{s}$ for 300 active links),
  eliminating the legacy $1.8\,\text{ms}$ line-search bottleneck.

#### 3. Bidirectional Navigation Flow:

- Clicking a link hotspot on a Xanadoc passage targeting a Zigzag cell dispatches a focus command to
  the `ZigzagVisualizer`, animating the 3D camera to center on the cell and triggering a visual
  dimensional pulse.
- Clicking an outgoing link badge on a Zigzag cell smoothly scrolls the active Xanadoc viewport to
  the target passage and highlights the span with a golden margin bracket.

______________________________________________________________________

## 4. Phased Implementation Stages

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  STAGE 1: Universal Link Endpoints & Layout Structures                      │
│  - Define UniversalLinkEnd (16 bytes, SysV ABI register passing)            │
│  - Define UniversalTransclusionPair (24 bytes) & UniversalLinkedPair        │
│  - Maintain full backward compatibility for existing LinkEnd call sites     │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│  STAGE 2: Universal Transclusion Discovery Across Docs & Cells              │
│  - Uncap Spanfilade transclusion discovery (Doc-Doc, Doc-Cell, Cell-Cell)   │
│  - Replace O(N^2) pairwise scans with sublinear O(log8 N + K) B-enfilade    │
│  - Generalize link_layout.hpp (placeLinks, placeTransclusions)              │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│  STAGE 3: Universal Content-Addressed Formatting Engine                     │
│  - Create FormatResolver (PrimediaSpan -> FormatAttribute -> DecoratedRange)│
│  - Cache 16-bit formatFlags in CellSlot padding (zero cache line bloat)     │
│  - Integrate into UnifiedTransclusionEngine::layoutCellShaped()             │
│  - Render rich typography (bold, italic, alignment) in ZigzagVisualizer     │
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│  STAGE 4: Cross-Domain Link Forging, Optical Beams, & Navigation            │
│  - Extend PouchItem & LinkForgeWidget for drag-and-drop cell clasping       │
│  - Two-tier anchor resolution in LinkBeams (2D Doc page <-> 3D cell center) │
│  - Bidirectional navigation: clicking doc link focuses 3D cell and vice-versa│
└──────────────────────────────────────┬──────────────────────────────────────┘
                                       │
┌──────────────────────────────────────▼──────────────────────────────────────┐
│  STAGE 5: DRY Consolidation & Code Quality Cleanup                          │
│  - Unify 3D Beams rendering pipeline across Xudu and Zigzag                 │
│  - Consolidate redundant rank-walk implementations (5 places -> 1 helper)   │
│  - Consolidate text resolution routines and reconcile visualizer units      │
└─────────────────────────────────────────────────────────────────────────────┘
```

______________________________________________________________________

## 5. Verification Matrix

| Area                      | Verification Method                             | Pass Criteria                                                                               |
| :------------------------ | :---------------------------------------------- | :------------------------------------------------------------------------------------------ |
| **Endpoint ABI**          | `static_assert(sizeof(UniversalLinkEnd) == 16)` | Exactly 16 bytes, passed in `%rsi, %rdx`                                                    |
| **Transclusion Stabbing** | Unit test with mixed `Version` and `Manifold`   | Discovers Doc-Doc, Doc-Cell, and Cell-Cell transclusions in $O(\log_8 N + K)$               |
| **Cell Formatting**       | Unit test on `FormatResolver` + Visualizer test | `LinkType::Format` renders bold/italic text quads in Zigzag cells                           |
| **Cross-Domain Linking**  | End-to-end clasp forging test                   | Xanadoc $\leftrightarrow$ Cell links write valid `OpKind::Link` without disk format changes |
| **120 FPS Framerate**     | Latency probe on dual-view render loop          | Anchor transform + ribbon vertex staging $\le 0.5\,\text{ms}$ CPU budget                    |
| **Headless Build & Lint** | `make test && make format-check && make lint`   | Zero test regressions; clean exit code 0                                                    |
