---
name: rendering-and-text
description: >-
  Generic libgleditor rendering, FreeType/HarfBuzz shaping, glyph atlas, GL/GLES/Vulkan, hot loops,
  and apps/gleditor TSV config. Use when working on text shaping, shaders, AccessKit accessibility,
  generic widgets/SDL, or Layoutfilade.
---

# Generic library, rendering, and text architecture

## Library boundaries and invariants (`src/`, `include/gleditor/`)

- **`libgleditor` is generic only:** `src/` and `include/gleditor/` (`text/`, `render/`,
  `enfilade/`) hold generic components only — document model, glyph cache, SDL wrappers, render
  loop, `src/text/`, `src/render/` (`gl/`, `vulkan/`), `src/a11y/` (AccessKit), and `src/enfilade/`
  (`Layoutfilade`). No library type, file, or test may be named for a Xanadu/ZigZag concept (cell,
  link, xanadoc, slice, view kind), and `libgleditor` must **never** include or depend on `apps/`.
- **Plain editor (`apps/gleditor`):** consumes `libgleditor` only, shares no code or dependency with
  Xanadu, ZigZag, or Xuzz, and loads its own parameters from a **plain TSV configuration file**
  (`apps/gleditor/editor_config.cpp`) with a `user_notes.notes` entry.
- **Library invariants and defaults:** Core library fallback defaults and immutable physical
  invariants (e.g., cache-line size `64`, HarfBuzz 26.6 scale factor `64.0F`, BitTorrent piece size
  `65536`) must be explicitly typed `constexpr` within dedicated domain constants headers with
  rationale comments. Fallback defaults for configuration structs must document their rationale; the
  library must never hardcode application-level assumptions or UI policies.
- **Shaders (`assets/shaders/`):** GLSL shader bodies live in `assets/shaders/`; `vulkan/` holds
  generated SPIR-V (validated and compiled via `make shaders`).

## Text architecture

Zero Cairo, zero Pango: FreeType 2, HarfBuzz, libunibreak (UAX #14), FriBidi, Fontconfig.

- `text::FontManager` / `FontFace` (`src/text/font.cpp`) resolve `"Sans Bold 12"`-style descriptions
  via Fontconfig into thread-safe `FT_Face`/`hb_font_t` pairs and compute `ascent`, `descent`,
  `lineHeight`, `spaceWidth`.
- `text::TextLayout` (`src/text/layout.cpp`): `layoutPage()` breaks with libunibreak, shapes with
  HarfBuzz, wraps at `maxWidthPx`, paginates by `maxHeightPx`. **Height-budgeted slicing** — input
  is cut to the lines that could fit — keeps page generation $O(1)$ instead of $O(N^2)$ over
  thousands of pages. `layoutSingleLine()` is the toast/measurement fast path. Output is
  `PageShaping`, consumed by `Doc` and `Page`.
- `GlyphCache` (`src/glyphcache/cache.cpp`) rasterises 8-bit coverage with FreeType into a 2D
  texture array atlas (512² growing to 16384² over 64 layers). Quads anchor to `line.top` at
  `lineHeight`; cluster textures put the baseline at $Y = \text{ascent}$.

## Systems realism and performance

- Hot rendering loops must have **zero dynamic memory allocations**.
- Critical hot structs in rendering and layout paths must be cache-friendly (64-byte aligned where
  appropriate).
- Back performance claims with empirical data from probe tools (`tools/layout-latency-probe.cpp`,
  `tools/benchmark-kjv-load.py`, `make ui-text-baseline` / `tools/ui-text-baseline.cpp`).

## Visual verification, platform docs, and rendering plans

- After any shaping, layout, or backend change, run `./tools/compare-backends.sh` under `xvfb-run`
  (`xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh`) and inspect the PNGs it writes
  (flat baselines, correct cluster height, sharp glyphs). Reading a captured frame is inspection;
  opening a live window is not.
- For SDL2/SDL3 differences, AccessKit accessibility, `RenderDevice`, clipping/culling, and the
  three-backend pipeline, see [docs/rendering.md](../../../docs/rendering.md).
- [`design/world-space-rendering-plan.md`](../../../design/world-space-rendering-plan.md) — plan,
  unbuilt, library only: unprojection matching the projection code, render regions (device scissor
  and depth range), placed planes and a page arrangement seam. Records the backends' clip-space
  conventions, Vulkan's reversed Z included.
