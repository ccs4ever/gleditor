# GPU-Native Text Shaping, Height-Budgeted Layout, and Typography Architecture

An architectural specification and design document for the zero-Cairo /
zero-Pango typography pipeline, HarfBuzz complex script shaping, $O(1)$
height-budgeted pagination, dynamic 2D texture array glyph caching, and
persistent GPU stream ring buffers across `gleditor`, `xudu`, and `zigzag`.

---

## 1. Architectural Philosophy: Zero Cairo, Zero Pango

Modern Linux desktop text stacks typically rely on heavyweight abstraction
layers (Pango, Cairo, PangoFT2, Pangomm) that incur massive heap allocation
overheads, hidden synchronization locks, and CPU-side software rasterization
bottlenecks.

`gleditor` eliminates these legacy layers entirely:
1. **Direct Hardware Foundations**: Built directly on **FreeType 2**,
   **HarfBuzz**, **libunibreak (UAX #14)**, **FriBidi**, and **Fontconfig**.
2. **Strict Thread Decoupling**: Unicode line-breaking and HarfBuzz complex text
   shaping execute asynchronously on worker threads. The render thread handles
   strictly GPU instance buffer uploads and draw calls.
3. **$O(1)$ Height-Budgeted Pagination**: Text slicing eliminates $O(N^2)$
   full-document reflow during keystroke updates.
4. **Dynamic 2D Array Atlas**: Single-channel 8-bit coverage texture array
   scaling up to $16384 \times 16384 \times 64$ layers without texel coordinate
   invalidation.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          TextLayout Pipeline                            │
│                                                                         │
│  [UTF-8 Primedia] ──► [libunibreak] ──► [HarfBuzz Shaping]              │
│                             │                  │                        │
│                             ▼                  ▼                        │
│                     [Line Breaks]       [Cluster Mapping]               │
│                             │                  │                        │
│                             └────────┬─────────┘                        │
│                                      ▼                                  │
│                       [FriBidi BiDirectional Reorder]                   │
│                                      │                                  │
│                                      ▼                                  │
│                  [PageShaping / ClusterBox / GlyphEntry]                │
└──────────────────────────────────────┬──────────────────────────────────┘
                                       │
                                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                       GPU Instance Stream Buffer                        │
│                                                                         │
│  StreamBufferGL / StreamBufferVK (Persistent Mapped Ring Buffer)        │
│  ├── Dynamic VBO Instance Array (Quad bounds, UV rects, anim/decor)     │
│  ├── Multi-Layer Texture Array Atlas (512x512 -> 16384x16384 x 64)      │
│  └── 2 MB UBO (Lock-free highlight ranges & selection overlays)         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Text Shaping and Pagination Pipeline (`TextLayout`)

Text layout is implemented in [`TextLayout::layoutPage()`](src/text/layout.cpp).

### 1. Unicode Line Breaking (`libunibreak`)
`set_linebreaks_utf8()` evaluates the UTF-8 byte stream, marking valid
line-break opportunities (`LINEBREAK_ALLOWBREAK`, `LINEBREAK_MUSTBREAK`) in
conformance with Unicode Standard Annex #14.

### 2. Complex Script Shaping & Font Fallback (`HarfBuzz`)
- Runs of text are shaped with HarfBuzz (`hb_shape()`), resolving glyph indices,
  advances, and ligature substitutions.
- **Dynamic Fallback Resolution**: When HarfBuzz returns `codepoint == 0`
  (missing glyph), [`FontManager::getFallbackFont()`](src/text/font.cpp) queries
  Fontconfig to locate a matching system face (e.g. CJK, Math, Emoji) and
  re-shapes the run seamlessly.

### 3. $O(1)$ Height-Budgeted Slicing (Eliminating $O(N^2)$ Reflow)
When formatting a single page in a 100+ MB document, shaping the entire document
produces catastrophic $O(N^2)$ latency. `TextLayout::layoutPage()` calculates an
analytical upper bound on text required to fill `maxHeightPx`:

$$\text{maxLinesEst} = \left\lceil \frac{\text{maxHeightPx}}{\text{lineHeight}} \right\rceil + 8$$
$$\text{sliceBudget} = \max(32768, \text{maxLinesEst} \times 1024)$$

The input text is sliced to `sliceBudget` bytes on a UTF-8 character boundary.
Keystroke layout latency is strictly $O(1)$ and executes in $< 0.5\text{ ms}$.

---

## 3. Dynamic 2D Texture Array Glyph Atlas (`GlyphCache`)

Glyphs and shaped clusters are cached in a GPU texture array managed by
[`GlyphCache`](include/gleditor/glyphcache/cache.hpp):

### 1. Power-of-Two Area-First Growth
- Starts at $512 \times 512 \times 1$ layer.
- Growth quadruples area ($2\times$ width, $2\times$ height) until hitting GPU
  limits (e.g. $4096 \times 4096$), then doubles layer count up to **64 layers**
  (the 6-bit hardware limit in `Doc::VBORow`).
- **Immutable Placements**: Reallocation replays stored `Placement` records,
  preserving exact relative UV coordinates so existing VBO quad buffers do not
  need rebuilding.

### 2. Mipmap Bleed Prevention
To support sharp rendering during 3D camera zooms without texture bleeding:
- 4 mipmap levels are generated (`atlasMipLevels = 4`).
- An **8-texel zeroed gutter** (`glyphPadding = 8`) surrounds every glyph box,
  preventing color bleeding across adjacent glyphs at low mip levels.

### 3. Synthetic Outlines & In-Atlas Decorations
- **Real vs. Synthetic Styles**: If native bold/italic faces are missing,
  FreeType applies `FT_GlyphSlot_Embolden` and `FT_GlyphSlot_Oblique` directly
  to vector outlines before rasterization.
- **Underlines and Strikethroughs**: Composited directly into 8-bit coverage
  bitmaps during rasterization using font typographic metrics, eliminating
  secondary draw calls.

---

## 4. GPU Instancing & Packed VBO Architecture

Every glyph quad on a page is rendered as a single packed 24-byte instance
([`Doc::VBORow`](include/gleditor/doc.hpp)):

```cpp
struct VBORow {
  std::array<float, 2> pos;   ///< Quad center in layout space (8 bytes)
  unsigned int foreground;    ///< Ink RGBA colour + flags (4 bytes)
  unsigned int atlas;         ///< Atlas texel origin (x:16, y:16) (4 bytes)
  unsigned int quad;          ///< Dimensions (width:12, height:12, layer:6, kind:2) (4 bytes)
  unsigned int paper;         ///< Background colour (RGB565:16) + cluster:16 (4 bytes)
};
static_assert(sizeof(VBORow) == 24);
```

### Shaders (`glyph.vert.glsl`, `glyph.frag.glsl`)
- **Vertex Stage**: Generates oriented triangle strips procedural from
  `gl_VertexID` with zero per-vertex attributes.
- **Fragment Stage & Vertical Multi-Banding**:
  - Direct solid bypass (`vSolid == 1`) skips texture sampling for page paper
    backgrounds.
  - Coverage blending blends foreground ink against background paper.
  - **Multi-Banding Highlights**: When up to 4 highlight ranges overlap a single
    character (e.g. transclusion gold and critique cyan), the fragment shader
    partitions the quad vertically into distinct color bands.

---

## 5. Lock-Free GPU Stream Ring Buffers

To eliminate driver stalls and buffer synchronization fences:
- [`StreamBufferGL`](include/gleditor/render/gl/stream_buffer.hpp): Persistent
  mapped buffer with `GL_MAP_UNSYNCHRONIZED_BIT` and explicit range flushing
  (`glFlushMappedBufferRange`). Space is reclaimed non-blockingly via
  `glClientWaitSync` with zero timeout.
- [`StreamBufferVK`](include/gleditor/render/vulkan/stream_buffer_vk.hpp):
  Host-visible, host-coherent persistent staging ring guarded by `VkFence`
  queues.

---

## 6. Implementation File Map

| Component | Source Files | Description |
| :--- | :--- | :--- |
| **Font Manager** | [`src/text/font.cpp`](src/text/font.cpp), [`include/gleditor/text/font.hpp`](include/gleditor/text/font.hpp) | FreeType2 `FT_Face` and HarfBuzz `hb_font_t` management and Fontconfig fallback |
| **Text Layout** | [`src/text/layout.cpp`](src/text/layout.cpp), [`include/gleditor/text/layout.hpp`](include/gleditor/text/layout.hpp) | Line breaking, HarfBuzz shaping, and $O(1)$ height-budgeted pagination |
| **Glyph Atlas** | [`src/glyphcache/cache.cpp`](src/glyphcache/cache.cpp), [`include/gleditor/glyphcache/cache.hpp`](include/gleditor/glyphcache/cache.hpp) | 2D texture array allocation, padding gutters, and coverage rasterization |
| **Document Geometry** | [`src/doc.cpp`](src/doc.cpp), [`include/gleditor/doc.hpp`](include/gleditor/doc.hpp) | `PageShaping`, `Doc::VBORow` instance assembly, and selection geometry |
| **GL Stream Buffer** | [`include/gleditor/render/gl/stream_buffer.hpp`](include/gleditor/render/gl/stream_buffer.hpp) | Persistent mapped OpenGL ring buffer with explicit range flushing |
| **Vulkan Stream Buffer** | [`include/gleditor/render/vulkan/stream_buffer_vk.hpp`](include/gleditor/render/vulkan/stream_buffer_vk.hpp) | Host-coherent Vulkan staging ring buffer with fence synchronization |
| **Shaders** | [`assets/shaders/glyph.vert.glsl`](assets/shaders/glyph.vert.glsl), [`glyph.frag.glsl`](assets/shaders/glyph.frag.glsl) | Instanced glyph quad vertex and fragment multi-band shaders |
| **Unit Tests** | [`tests/lib/font.cpp`](tests/lib/font.cpp), [`tests/lib/layout.cpp`](tests/lib/layout.cpp), [`tests/lib/cache.cpp`](tests/lib/cache.cpp) | Typographic metrics, shaping, fallback, and atlas growth tests |
