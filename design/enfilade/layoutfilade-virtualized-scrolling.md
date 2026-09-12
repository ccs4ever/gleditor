# The True Layoutfilade: 2D Coordinate & Height B-Enfilade for Virtualized Scrolling

This document specifies the architecture, 2D monoid algebra, and runtime mechanics of the
**Layoutfilade** in `apps/common/xanadu/enfilade/layoutfilade.hpp` and `.cpp`.

The Layoutfilade provides true (O(\\log N)) virtualized scrolling, screen coordinate (Y
\\leftrightarrow (\\text{line}, \\text{byte})) mapping, native non-text rich-media layout, and
localized incremental reflow without touching downstream nodes.

______________________________________________________________________

## 1. The Virtualized Scrolling Problem

In text editors and xanadoc visualizers, handling massive documents (e.g. 50 MB, 1,000,000 lines, or
the Gutenberg Bible) poses severe performance challenges:

1. **Quadratic Full-Document Shaping**: Shifting or paginating an entire million-line document
   front-to-back triggers catastrophic (O(N^2)) rendering latency.
1. **Heuristic Byte Windows**: Naive approaches slice fixed-byte chunks
   (`maxHeightPx / lineHeight * 1024`), failing on non-uniform text with long paragraphs, multi-byte
   UTF-8 scripts, or embedded media widgets.
1. **Downstream Invalidation**: Editing 5 bytes on line 10 shifts the absolute byte offsets of every
   line that follows.
1. **Rich Media Decoupling**: Non-text media (images, videos, audio widgets) have visual dimensions
   that must not dictate document byte offsets.

The **Layoutfilade** solves all four problems using a 2D B-enfilade over flow entities.

______________________________________________________________________

## 2. 2D Coordinate & Flow Monoids

Flowing text lines and media cards are modeled using a 2D progression and extent monoid.

### 2.1 The Displacement Monoid: `LayoutDsp`

Stores the progression between frames:

- `deltaBytes`: Cumulative UTF-8 bytes progression (\\Delta\\text{bytes}).
- `deltaYPx`: Cumulative vertical coordinate progression (\\Delta Y\_{\\text{px}}).
- `deltaLines`: Cumulative flow entity count (\\Delta\\text{lines}).

Composition is associative vector addition:

```text
d_1 ∘ d_2 = (d_1.bytes + d_2.bytes, d_1.Y + d_2.Y, d_1.lines + d_2.lines)
Identity  = (0, 0.0, 0)
```

### 2.2 The Width / Summary Monoid: `LayoutWid`

Summarizes the aggregate metrics of all flow entities in a subtree:

- `totalBytes`: Sum of UTF-8 bytes (text + 3B per media anchor).
- `totalHeightPx`: Sum of visual heights (\\sum (h_i + 2 m_i)).
- `lineCount`: Total visual lines and media blocks.
- `maxLineWidthPx`: Maximum visual width (\\max (w_i + 2 m_i)).
- `mediaBoxCount`: Count of non-text rich-media boxes in the subtree.

Combination evaluates the associative bounding envelope:

```text
w_1 ⊕ w_2 = (w_1.bytes + w_2.bytes,
             w_1.height + w_2.height,
             w_1.lines + w_2.lines,
             max(w_1.maxWidth, w_2.maxWidth),
             w_1.mediaCount + w_2.mediaCount)
```

### 2.3 Coordinate Action

Because metric extents (heights, line widths, byte lengths) are translation-invariant under
coordinate shifts:

```text
d.act(w) = w
```

Satisfies `EnfiladeAction<LayoutDsp, LayoutWid>`.

______________________________________________________________________

## 3. Cache-Conscious Memory Layout

To eliminate pointer-chasing overhead and fit neatly into modern CPU cache lines:

```text
                      +----------------------------------+
                      |         Layoutfilade             |
                      |  rootIndex_: std::size_t         |
                      +----------------+-----------------+
                                       |
                   +-------------------+-------------------+
                   |                                       |
                   v                                       v
   +-------------------------------+       +-------------------------------+
   |   std::vector<LayoutCrum>     |       |   std::vector<LayoutEntry>    |
   |   alignas(64), B = 16         |       |   sizeof == 32B (2 per 64B)   |
   +-------------------------------+       +-------------------------------+
```

### 3.1 Leaf Entry: `LayoutEntry` (32 bytes)

Leaves are packed into 32-byte records, placing exactly 2 entries per 64-byte cache line:

- `byteLength`: UTF-8 bytes (3 for `U+FFFC`, or line length).
- `heightPx`: Line or media height.
- `widthPx`: Line advance width or media width.
- `marginPx`: Vertical and horizontal margin.
- `baselineOffsetPx`: Distance below baseline for inline boxes.
- `boxId`: Matches `LayoutBox::id` (opaque handle to `MediaWidget` / texture).
- `kind`: `LayoutEntryKind` (`TextLine`, `BlockBox`, `InlineBox`, `FloatBox`, `ForcedBreak`).
- `placement`: `BoxPlacement` (`Block`, `FloatLeft`, `FloatRight`, `Inline`).
- `flags`: Bitflags (`HasNewline`, `IsForcedBreak`).

### 3.2 Routing Crum: `LayoutCrum` (64 bytes)

Internal nodes are aligned to 64 bytes (`alignas(64)`) with branching factor (B = 16), keeping the
tree depth (\\le 4) even for 100,000 lines.

______________________________________________________________________

## 4. Rich-Media Integration

Rich media integrates natively via the **`LayoutBox`** model and the **`U+FFFC`** anchor:

1. **Decoupled Bytes & Dimensions**: A 4K image (3840 (\\times) 2160) occupies exactly 3 bytes
   (`\xEF\xBF\xBC`) in the text stream, but contributes 2160.0 px to `totalHeightPx` and 3840.0 px
   to `maxLineWidthPx`.
1. **Zero-Overhead Off-Screen Media**: When `visibleRange(topY, bottomY)` is queried, only the media
   boxes that intersect the viewport window are returned in `visibleMediaBoxIndices`. Off-screen
   media items require no GPU texture allocations, decoding threads, or HarfBuzz shaping passes.
1. **Dynamic Resizing in (O(\\log N))**: Resizing a media card updates its single `LayoutEntry` and
   bubbles up the parent chain to the root, immediately updating document height without touching
   surrounding text.

______________________________________________________________________

## 5. Architectural Ruling Compliance

| Ruling       | Constraint                     | Layoutfilade Compliance                                                                                                                                                      |
| ------------ | ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **R8**       | Navigation must never mint ops | Layoutfilade is an ephemeral, in-memory layout product. Viewport queries and scrolling never touch disk spools.                                                              |
| **R9**       | Equivalence with linear scan   | `verifyAgainstLinearScan()` validates that `findEntryAtY()`, `findEntryAtByte()`, `findEntryByIndex()`, and `visibleRange()` match linear scans to the exact byte and pixel. |
| **R12 & U3** | `sizeof(CellSlot) == 32`       | Layoutfilade operates on layout streams and external indices without modifying or enlarging Zigzag `CellSlot`.                                                               |
| **V3**       | Transient structure lifecycle  | Rebuilt on demand or discarded upon store compaction / document reload.                                                                                                      |

______________________________________________________________________

## 6. Performance Benchmarks

Benchmarks executed on Linux x86_64 (`tests/xudu/layoutfilade_benchmark_test.cpp`):

### 6.1 Coordinate Descent vs Linear Scan ((N = 10,000) lines, 10,000 queries)

- **Linear Scan (Y) Descent**: (8,410\\ \\mu\\text{s})
- **Layoutfilade (O(\\log N)) Descent**: (432\\ \\mu\\text{s})
- **Speedup Factor**: **(19.5\\times) faster**

### 6.2 Incremental Line & Media Resizing

- **Average Update Latency**: (< 45\\text{ ns}) per update across 10,000 lines.
- Updating an entry touches at most 4 cache lines from leaf to root.
