# Animated SVG and Animated GIF Primedia via ThorVG, giflib, and MediaWidget

## Motivation & Architecture

While static raster images and static SVGs render as flat inline or floating figures via `ImageOverlay` and `SvgCache`, animated graphics (animated GIFs, animated SVGs with SMIL or vector animation) require a dynamic temporal dimension: play, pause, stop, seeking, playback rate control, continuous frame progression, and temporal fragment transclusion.

Rather than building redundant, separate UI widgets for each animation format, the existing `MediaWidget` and `MediaPlayer` architecture was extended to treat animated GIFs and animated SVGs as native visual streams alongside LibVLC video. Users can transclude full animations or temporal sub-ranges (quoting specific time spans with 3D link ribbons and identity prisms), interact with transport controls, adjust playback rates, and navigate through the accessibility tree.

## Build Flags and Dependency Precedent

Following the precedent set by TIFF (`HAVE_DECODE_INDEX_TIFF`) and LibAV (`HAVE_DECODE_INDEX_LIBAV`):

1. **Makefile Probing**:
   `HAVE_DECODE_INDEX_GIF` is probed via `pkg-config --exists giflib` with a fallback check for `(test -f /usr/include/gif_lib.h && echo 1)`.
2. **Defines**:
   When detected, `-DGLEDITOR_HAVE_DECODE_INDEX_GIF=1` is injected into `CXXFLAGS`, and `-lgif` is appended to `LIBS`.
3. **Decode Index**:
   `DecodeIndexFormat::Gif` is defined in `include/gleditor/decode_index.hpp`, backed by `peekGifSize(span)`, `isAnimatedGif(span)`, and `buildGifIndex(span)`.

## ClickableRegistry & Multi-Kind Picking Tag Auto-Registration

Previously, interactive widgets relied on manual sequential sub-tag constants and repetitive `if (offset == tagPlay) ... else if (offset == tagPause) ...` chains. To make interactive elements auto-registering without hardcoding picking ladders across all visual layers:

- **Unified Multi-Kind Coverage**: `ClickableRegistry` handles all picking tag usages: `tagKindOverlay` (HUD/widget controls, dialog buttons), `tagKindPage` (page background clicks, margin navigation, page flipping), and `tagKindGlyph` (clickable words, hyperlinks, mentions, and multi-cluster text spans).
- **Compile-time Static Tags**: `StaticTag<FixedString, Kind>` uses compile-time FNV-1a 32-bit hashing, providing deterministic, collision-free sub-tags for static controls across kinds:
  - `StaticOverlayTag<Id>` / `StaticSubTag<Id>`: defaults to `tagKindOverlay`.
  - `StaticPageTag<Id>`: compile-time static page control tag.
  - `StaticGlyphTag<Id>`: compile-time static glyph/link control tag.
- **Control Descriptors (`ClickableControl`)**: Stores `tagKind`, optional document/page constraints (`docIndex`, `pageIndex`), cluster/offset bounds (`tagOffset`, `tagEndOffset` for single clusters or span ranges `[start, end)`), callbacks (`onClick`, `onPickTag`, `onPick`), layout dimensions, and accessibility roles (`a11y::Role`).
- **Unified Dispatching**:
  - `dispatch(const render::PickingResult &pick)` / `dispatch(const render::PickingTag &tag)`: inspects `tag.kind`, verifies document/page filters, applies optional `tagBase_` offset subtraction for overlays, matches cluster spans, and executes callbacks with full coordinate and fraction data.
  - `dispatch(tagOffset)`: backward-compatible fast path for overlay sub-tags.
  - `picked(pick, state)`: drop-in compatibility with `PickObserver`.

## Animated GIF Decoding (`GifDecoder`)

Multi-frame GIF decoding is implemented in `gleditor::GifDecoder` using `giflib`:

- **Slurped Stream**: Uses `DGifOpen` with a custom memory reader callback and `DGifSlurp` to extract global/local color tables, screen descriptors, and graphics control blocks.
- **Disposal and Transparency**: Handles GIF disposal modes (`DISPOSAL_UNSPECIFIED`, `DISPOSAL_NONE`, `DISPOSAL_BACKGROUND`, `DISPOSAL_PREVIOUS`) and transparent color indexing across frames.
- **Scanline Decoding**: De-interlaces or sequentially copies scanline pixel bytes, mapping palette indexes into little-endian RGBA32 pixels (`0xAABBGGRR`).
- **Timestamp Seeking**: Stores frame delay centiseconds, mapping seconds elapsed to discrete animation frames via `frameAt(seconds)`.

## Animated SVG via ThorVG (`SvgAnimator`)

Vector animation support is implemented in `gleditor::SvgAnimator` using ThorVG (`libthorvg-1`):

- **Detection Strategy**: To detect whether a loaded file contains an active animation in ThorVG without manually parsing the JSON or markup, `SvgAnimator::isAnimated(span)` instantiates a `tvg::Animation` context, retrieves its linked `tvg::Picture`, loads the vector asset, and evaluates `duration() > 0.0f || totalFrame() > 1`. Static assets report 0.0 duration and $\le 1$ frames. For SVG files with SMIL animations that ThorVG's static vector picture loader does not animate at runtime, a lightweight fallback checks for XML SMIL animation elements (`<animate>`, `<animateTransform>`, `<animateMotion>`, `<set>`).
- **SMIL & Attribute Interpolation**: Parses target attributes (e.g., `x`, `y`, `width`, `height`, `transform`), start/end values, and duration parameters (`dur`, `repeatCount`).
- **Rasterization**: Dynamically computes animated state at target timestamp $t$, applies transformation matrices, and rasterizes frames into RGBA32 buffers via `tvg::SwCanvas`.

## MediaPlayer Continuous Animation Pipeline

`MediaPlayer::Impl` integrates both `Backend::Gif` and `Backend::Thorvg` alongside LibVLC:

- **Clock Decoupling**: Since animations lack audio hardware sinks, time advances via external system $\Delta t$:
  $$\Delta t_{\text{effective}} = \Delta t \times \text{playbackRate}$$
- **Looping & Range Clamping**: Respects configured time ranges (`timeRangeStart_` to `timeRangeEnd_`) for temporal transclusion, looping continuously within sub-ranges.
- **Thread Safety**: Generates immutable `std::shared_ptr<VideoFrame>` instances uploaded to the GPU texture array atlas upon `drawFrame()`.

## Playback Rate & Speed Control

`MediaWidget` features an interactive speed button (`tagSpeed`):

- **Cycling Range**: Cycles through `1.0x -> 1.5x -> 2.0x -> 0.25x -> 0.5x -> 1.0x`.
- **Dynamic Badge**: Formats the button label dynamically (e.g. `1.0×`, `1.5×`, `2.0×`).
- **Instantaneous Fragment Application**: `loadFragment` immediately applies pending temporal slices (`applyPendingFragment()`) if duration is available synchronously.

## XanaDoc Xanadulogical Integration & Animation Transclusion

Within `xudu`:

1. **Classification**: `Session::mediaSpansFor` checks `isAnimatedGif(containerSpan)` and `SvgAnimator::isAnimated(containerSpan)`. Matching spans are classified with `isAnimation = true` and `isImage = false`.
2. **Layout Sizing**: `mediaFitFor` uses `videoFitSize` rather than `imageFitSize`, reserving vertical space for transport chrome (`MediaWidget::chromeHeightPx`) and enforcing `Block` placement with centered alignment.
3. **Hypertext Beams & Ribbons**: Link ribbons and transclusion prisms query `views.widgetRectFor(doc, mSpan.docOffset)`, flush with the media card margins.
