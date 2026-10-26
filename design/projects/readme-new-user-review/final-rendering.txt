# Library and rendering

[Project overview](../README.md) · [Build setup](../README.md#quick-start) ·
[Tests and diagnostics](testing.md)

Commands and source paths below are relative to the repository or worktree root.

`libgleditor` owns text layout, pagination, the glyph cache, rendering, SDL integration, input,
accessibility data, and the application loop. It names no xanadoc, cell, link, or document format.
The plain editor keeps its own TSV configuration and shares no Xanadu code.

## First saved file

The plain editor opens existing file paths. For a disposable first file, create an empty file, then
open it:

```sh
scratch=$(mktemp -d /tmp/gleditor-first.XXXXXX)
touch "$scratch/hello.txt"
./build/gleditor "$scratch/hello.txt"
```

Type text and press **Ctrl+S** to write it back to that path; exit and run the same command to
reopen it in the same shell. An existing empty file is accepted, but a path that does not exist is
reported as missing. The plain editor's **Ctrl+N** makes an unnamed document, and there is no Save
As command for it yet, so start with a file path when you want a saved file.

## One pipeline, three backends

The graphics backend is selected at run time with `--backend opengl|opengles|vulkan`. OpenGL 3.3
core is the default, OpenGL ES 3.0 is supported, and Vulkan 1.0 is compiled when its development
tools are found. All use `render::RenderDevice`, the same glyph instances, and shader bodies from
`assets/shaders/`. Backend-specific shader preambles and build-time SPIR-V generation keep those
bodies aligned. GL entry points come from `SDL_GL_GetProcAddress`; the build does not link `-lGL`.

## Text layout and fitting

The glyph atlas holds FreeType's 8-bit coverage. HarfBuzz shapes runs, FriBidi resolves text
direction, libunibreak chooses line breaks, and Fontconfig resolves fonts.
`TextLayout::layoutPage()` limits work to lines that fit a page; immutable `PageShaping` values pass
from page building to the render thread. Editing rebuilds the affected page and continues only until
pagination resynchronizes. Caret and selection offsets are document byte positions, while picking
and highlighting account for character boundaries inside shaped clusters such as ligatures.

UI labels use `text::fit()` with explicit width, height, line count and overflow policies. End,
middle and start ellipsis retain complete graphemes and shaping clusters, with source byte offsets
preserved. `LayoutOptions::ellipsize` now truncates the last visible line; document pages disable it
to continue normally. Caller-owned `text::ShapingCache` retains page layouts and fitted labels in
one bounded LRU, keyed by the font and every layout constraint. References last until their entry is
evicted or the cache clears; use each cache on one thread.

Canvas can fit text directly into a Y-up `ui::Rect`, returning a `BoxedText` with the fitted label
and its effective draw box. Drawing the returned `FittedText` again uses no layout. `pushClip()` and
`popClip()` intersect nested boxes on the CPU for rectangles, lines, glyphs and images; `clear()`
resets the clip stack. Glyph crops preserve integer atlas texels, rounding fractional clip edges
inward. Image crops interpolate their UV rectangle. The compatibility width-limit API keeps
single-line ellipsis and clips ink to that width; new callers should use boxed text.

## Render regions, culling and picking

Pages outside the view are culled conservatively in clip space. Distant pages switch to per-line
coarse bars according to projected size. `--no-cull` and `--coarse-below` expose both paths for
comparison. `Doc::collect()` submits page draws across open documents as one list. Vulkan can record
chunks on worker threads; GL and GLES record in order on their context thread. Vulkan measures the
choice at run time; `GLEDITOR_RECORD_THREADS` overrides it for comparison.

Per-frame render regions keep viewport mapping separate from the writable clip rectangle and depth
range. Batches carry their region into worker recording; immediate draws use a restoring scope.
World content and its picking are clipped outside settled edge chrome while screen controls retain
the whole target. The camera projection is unchanged.

Picking writes an identity attachment beside color. `requestPickingTag()` queues an asynchronous
read and `takePickingTag()` returns a completed result with its pixel coordinates. GL/GLES use a PBO
ring and fences; Vulkan uses per-frame host-visible buffers. The renderer also collects driver
messages in `render::DiagnosticSink`, logs them by category, and shows significant messages as
notifications. `--strict-diagnostics` makes them fatal for automated checks.

## Extension interfaces and diagnostics

Programs extend the library through `TextSource`, `DocumentObserver`, `SpanDecorator`,
`FrameContributor`, `PickObserver`, and the application/command interfaces in `include/gleditor/`.
`Canvas` draws text and geometry in world or screen coordinates through the same glyph pipeline.
`CurveRibbons` draws tapered NURBS strokes with continuous normals, soft edges and optional
stationary filament shading through a portable shader; a circular exclusion can preserve a pointer
picking target beneath transient feedback. `runWithState()` moves commands onto the render thread.
Xuzz uses these hooks for versioned text, shared-span shading, and links, and contributes ZigZag
cells and picking without putting its model in `libgleditor`.

For UI text overflow, `SPDLOG_LEVEL=ui.layout=debug` reports declared Canvas parent bounds;
`ui.layout=trace` also reports render-thread shaping per frame. `--benchmark` includes p95 frame
time and shaping counts. The optional `make -j$(nproc) ui-text-baseline` tool measures long-label
rebuilds and retained geometry across backends. See the
[text-fitting baseline](../design/ui-text-fit-baseline.md) for headless commands and coverage.

## C++26 compatibility

The public C++26 compatibility facades select native library facilities or pinned fallbacks for
`function_ref`, optional references and ranges, and `inplace_vector`. Scoped adapters provide
checked span access and a two-span `views::concat`. Vortex `map`/`filter`/`fold` and
`WorkerPool::run` use `function_ref`; VQL `!both` uses the concat view.
`GLEDITOR_CPP26_FORCE_FALLBACK=1` tests the fallback paths. Installed headers record the library's
native/fallback choices so clients use the same type layout. See the
[C++26 compatibility note](../design/cpp26-compatibility.md) for the exact boundary.

## Backend regression checks

For a rendering regression, build and run `./tools/compare-backends.sh` under `xvfb-run`. It
compares software-rendered PNGs, picking tags, and Vulkan's single- and multi-thread recordings. A
process exiting zero while drawing nothing is not a rendering test.

## Accessibility and SDL

The GPU draws quads, which do not describe themselves to a screen reader. A separate accessibility
tree is built from the same document and UI state. It describes documents as line-sized text runs,
caret and selection offsets in characters, notifications, forms, links, hypertime, and ZigZag cells.
Actions return through a queue to the thread that owns the target state. `include/gleditor/a11y/`
defines platform-independent nodes; `src/a11y/` sends them through AccessKit to AT-SPI on Linux, UI
Automation on Windows, and NSAccessibility on macOS. A build without AccessKit still builds the
internal tree and uses a no-op platform adapter. `GLEDITOR_ENABLE_A11Y=1` requires the binding;
`ACCESSKIT_DIR` can point to a local accesskit-c installation. `--dump-a11y` prints the settled tree
for headless inspection, at its place among the other automation options. `--wait-ms` pauses the
automation script for up to 60 seconds while rendering, input and background work continue.
`--click-label "Full control label"` resolves a unique visible actionable accessibility label and
clicks the centre of its drawn bounds through normal asynchronous picking. Missing or ambiguous
labels fail the run; responsive panel tests use this instead of fixed coordinates.

### SDL selection and text input

The Makefile selects SDL3 when pkg-config finds it, otherwise SDL2; use `GLEDITOR_SDL=2|3` to pin a
major. `include/gleditor/sdl_compat.hpp` normalizes their input, window, and text-input APIs. SDL2
needs version 2.0.22 or newer. SDL supplies windows and message boxes, not a widget toolkit:
editable forms such as xuzz's publishing dialog are rendered with `gleditor::Form`. Text input area
updates position input-method candidate windows beside the focused field.

### Focus and modal input

`ui::FocusManager` owns modal precedence and keyboard focus. Scopes publish immutable
`ui::LayoutResult` snapshots so traversal, input-method placement, picking and accessibility use the
same node identities and bounds. Tab follows layout order and wraps; arrows move within a focus
group after the control has handled editing. The accessibility publisher follows the manager's
active scope, reports one modal root and blocks background actions while that scope is modal.
Applications hold `FocusManager::ScopeHandle` registrations for their `FocusScope` objects and
release the handles before destroying those objects. Composed text uses `std::string_view`;
render-thread GPU picks are delivered to the same focused scope. Boxed `Canvas::addText` uses
`TextFit` constraints rather than a mutable canvas-wide width limit. `make lint` rejects byte-based
ellipsis truncation and fixed Sans size literals in UI components.
