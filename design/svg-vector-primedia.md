# Static SVG primedia via ThorVG

## Why

xudu already routes `image/*` spans to `gleditor::ImageCache`, a shared
2048x2048x16-layer RGBA8 atlas fed by SDL_image-decoded raster bytes (PNG,
JPEG, WebP, GIF). SVG is vector, not raster: there is nothing for SDL_image
to decode until *something* rasterizes it first. ThorVG (`thorvg-1.pc`,
confirmed present at v1.1.1 on this system) is that something -- a small,
dependency-light 2D vector rendering library with both a software
rasterizer (`SwCanvas`, renders into a caller-owned RGBA buffer) and a GPU
one (`GlCanvas`, renders straight into an OpenGL framebuffer). The request
was specifically to use `GlCanvas` for the OpenGL/OpenGL ES backends where
possible, with a fallback for Vulkan (which `GlCanvas` cannot target at
all).

## Design: a dedicated texture per SVG, not the shared atlas

`ImageCache`'s atlas is shelf-packed: every raster image lands at some
`(x, y)` offset inside one of up to 16 shared 2048x2048 layers.
`GlCanvas::target()` needs an FBO with a *whole* layer attached
(`glFramebufferTextureLayer` attaches an entire array layer, not a
sub-rectangle of one), and there is no way to tell ThorVG's internal
`glViewport(0, 0, w, h)` to render only into some other tenant's already-
packed corner of a bigger layer without either trusting undocumented
internals or fighting them with scissor state across a callback boundary
neither side is written to cooperate on. Given that, each SVG gets its own
small array texture instead -- one layer, sized to the SVG's own intrinsic
pixel dimensions -- exactly the pattern `MediaWidget` already uses for
video frames (`src/media_widget.cpp`: `texSize = std::max(width, height)`,
`device->createTextureArray(texSize, 1, RGBA8)`, a partial
`updateTextureLayer()` upload, and `u1`/`v1` set to `width/texSize` and
`height/texSize` rather than `1.0`). `gleditor::ImageResource` already
models exactly this shape (`texture`, `layer`, `u0/v0/u1/v1`) with nothing
that assumes the texture is the shared atlas, and `Canvas::addImage()`
(`src/canvas.cpp`) rebinds `imageAtlas = image.texture` on every call --
confirmed by reading `ImageOverlay::drawFrame()`, which already issues one
`addImage()`/`commit()`/`draw()` per placement rather than batching several
into one shared-texture draw call. So a distinct texture per SVG resource
needed zero changes to `Canvas` or `ImageOverlay`'s drawing code -- the new
`gleditor::SvgCache` (`include/gleditor/svg_cache.hpp`,
`src/svg_cache.cpp`) just produces `ImageResource` values the existing path
already knows how to draw.

## The GL escape hatch

`render::RenderDevice`'s own file doc is explicit that "application code
never sees an API handle" -- `Explore` confirmed there is no existing
precedent anywhere in this codebase for backend-specific code reaching past
that abstraction. `GlCanvas` genuinely needs one: a real FBO bound to a real
GL texture, and the GL context to render with. Rather than widen the
abstract `RenderDevice` interface (which would leak GL concepts into the
Vulkan backend too), `DeviceGL` alone (`include/gleditor/render/gl/device_gl.hpp`)
grew one new, narrowly-scoped public method:

```cpp
bool renderIntoTextureLayer(TextureHandle texture, int layer,
                            const std::function<void(unsigned fbo,
                                                      void *glContext)> &fn);
```

It binds a scratch FBO whose `GL_COLOR_ATTACHMENT0` is the given texture
array's layer, checks completeness, calls `fn` with the FBO name and this
device's native GL context handle, then restores whatever was bound before
and destroys the scratch FBO -- reusing `DeviceGL`'s own already-resolved
`GLApi` function table and its existing `TextureHandle -> GLuint` lookup, no
new GL loading. One new entry point had to be added to that table,
`glFramebufferTextureLayer` (`PFNGLFRAMEBUFFERTEXTURELAYERPROC`), which
nothing in this codebase had needed before.

`SvgCache::loadBuffer()` calls this only when `device->backend()` is
`OpenGL` or `OpenGLES`; on Vulkan, or if the GL attempt fails for any
reason (unknown texture, incomplete FBO, `GlCanvas::gen()`/`target()`/
`add()` failing), it falls back to rasterizing via `SwCanvas` into a host
buffer and uploading through the same `createTextureArray`/
`updateTextureLayer` pair every backend already supports -- "a working
picture beats a missing one," the same fallback philosophy the zstd/FLAC/
TIFF decode-index work already used for their own coarse-vs-fine cases.

## Empirical findings, not assumed

Several things below were proven with small standalone programs before
being written into `svg_cache.cpp`, the same discipline the TIFF and FLAC
phases used for strip independence and SEEKTABLE round-tripping:

- **`tvg::Paint` lifetime.** A freshly `tvg::Picture::gen()`'d object has
  `refCnt() == 0`, and a plain `picture->unref(true)` (the default) frees it
  cleanly under ASan with no leak, when it was never added to any canvas.
  `Canvas::add()` transfers ownership on success (confirmed the same way:
  `delete canvas` after a successful `add()` does not leak or double-free).
  This is why `renderSw()`/`tryRenderGl()` each parse their own fresh
  `Picture` from the same bytes rather than trying to share one across the
  GL-then-CPU-fallback attempt -- once `add()` succeeds the picture belongs
  to that specific canvas, and untangling "did the GL attempt already
  consume this" bookkeeping costs more than a second cheap XML parse of a
  small SVG document.
- **`ColorSpace::ABGR8888S`'s in-memory byte order.** Rendering a
  `fill="blue"` rect and reading the buffer back byte-for-byte confirmed the
  first byte is R, matching `DecodedImage::rgba`'s and
  `updateTextureLayer()`'s own expected order exactly -- no channel
  shuffling needed when handing ThorVG's output straight to the same upload
  path raster images use.
- **`GlCanvas::target()` with null display/surface, a real context, and a
  texture-array-layer FBO.** Built a standalone SDL3 offscreen-GL program
  that creates a `GL_TEXTURE_2D_ARRAY`, attaches layer 0 to a scratch FBO
  via `glFramebufferTextureLayer`, and renders an SVG into it through
  `GlCanvas` with only the native `SDL_GLContext` passed (no EGL display or
  surface) -- read back via `glReadPixels`, the rendered rect's colour
  matched the source SVG exactly. This is the exact shape
  `DeviceGL::renderIntoTextureLayer()` + `SvgCache::tryRenderGl()` use in
  production, proven end to end before being wired in rather than assumed
  from the header doc comment alone.
- **libmagic's SVG detection.** `MagicMimeDetector` uses `MAGIC_MIME_TYPE`
  (not plain `MAGIC_MIME`), so it never appends a `; charset=...` parameter.
  Checked via `file --mime-type` against both a full-XML-prolog SVG and a
  bare `<svg>`-root SVG: both report `image/svg+xml` cleanly, so
  `gleditor::MimeType{mime} == gleditor::MimeType::ImageSvg` (already
  defined in `mimetype.hpp`) is a safe, exact comparison with no parameter-
  suffix edge case to guard against.
- **`sample_image.svg`'s pixel-exact rendering**, both through a standalone
  `SwCanvas` program and through `tests/lib/svg_cache_test.cpp`'s mocked-
  device test: row 0 is the SVG's own top row (top-down, the same
  convention every raster decoder here already uses), and the four
  quadrant rects plus the centre circle land exactly where their `x`/`y`
  attributes say, at the exact colours given in the source markup.

## What's tested where, and why

`tests/lib/svg_cache_test.cpp` proves `peekSize()` (no device needed at
all, safe off the render thread) and the CPU/`SwCanvas` path, using
`MockRenderDevice` reporting `Backend::Vulkan` so the GL attempt is never
even tried -- the same mocked-device pattern
`tests/lib/canvas_image.cpp`'s `RecordingDevice` already established for
`Canvas`'s own buffers. Real pixel assertions (not just "it returned
something") come from the same fixture whose colours were confirmed by the
standalone scratch program above. The GL/`GlCanvas` fast path itself is
*not* unit-tested: it needs a real, current GL context and a real bound
texture, which no harness in `tests/lib/` provides today (not even
`canvas_image.cpp`'s own "framebuffer readback" test, which is mock-based
too) -- that path is proven by running the actual `xudu` binary against a
new sample document (`tests/samples/xudu/multimedia/10_svg_static_image`,
mirroring `03_mixed_text_image`'s raster-PNG layout with
`tests/samples/sample_image.svg` instead) and reading back a real
screenshot, which showed all four quadrant colours and the centre circle
rendered correctly.

## A pre-existing bug this work ran into, not caused

Running the same sample under `--backend vulkan` to prove the SwCanvas
fallback end to end at the application level failed immediately with
`DeviceVK::createPipeline: 8 is all this device's descriptor pool was sized
for` -- but the *identical* failure reproduces on the pre-existing
`03_mixed_text_image` sample and even on a plain single-document
`04_audio_doc` sample with no SVG or transclusion involved at all. This is
a real, separate bug in `DeviceVK`'s fixed-size descriptor pool sizing, not
anything introduced by `SvgCache` -- confirmed before assuming so, not
after. Filed separately rather than fixed here; the CPU fallback path
itself is still proven correct via `svg_cache_test.cpp`'s mocked-device
test above, independent of whatever is wrong with `DeviceVK`'s pool
capacity.

## Explicitly out of scope

- `tvg::Animation` (SMIL-animated SVG, Lottie) -- a deliberate follow-up
  phase. It will likely need `MediaWidget`'s own-texture-per-instance,
  update-every-frame pattern rather than `SvgCache`'s cache-once-by-id
  pattern, since independent document instances of the same animated asset
  need independent playback position.
- Re-rasterizing an already-cached SVG at a different pixel size (e.g. on
  zoom): this rasterizes once at the SVG's own intrinsic size, the same "1
  unit = 1 pixel" convention every raster format here already uses. A
  small-viewBox SVG shown larger will look as blurry as a small PNG would --
  a named limitation, not a silent one.
- `WgCanvas` (WebGPU) -- ThorVG's own header marks it "not fully supported
  yet."
