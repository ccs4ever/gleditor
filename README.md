# gleditor

GPU text editor experiment, with OpenGL, OpenGL ES and Vulkan backends

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)

Still a work in progress.

## Overview

`gleditor` is an experimental text editor rendered on the GPU. It uses SDL (3 or 2) for windowing/input and Pango/Cairo for text shaping and rasterization. The goal is to explore fast, flexible text rendering in a 2D/3D scene.

- Entry point: `src/main.cpp`
- Rendering pipeline and glyph cache live under `src/` (see `src/glyphcache/*`, `src/renderer.cpp`).

## Rendering backends

The graphics API is chosen at run time with `--backend`:

| Backend | Requires | Notes |
| ------- | -------- | ----- |
| `opengl` (default) | OpenGL 3.3 core | |
| `opengles` | OpenGL ES 3.0 | |
| `vulkan` | Vulkan 1.0 | Only when built with `GLEDITOR_ENABLE_VULKAN=1` |

Everything above the backend -- documents, pages, the glyph cache, the buffer
allocator -- is written against `render::RenderDevice` (`include/gleditor/render/`)
and names no graphics API. Adding a backend means implementing that interface;
it does not mean touching the document code.

### How one pipeline serves all three

- **Glyphs are instanced quads.** One instance per glyph, with the four corners
  derived from the vertex index. Expanding points into quads in a geometry
  shader would rule out OpenGL ES 3.0, which has no geometry stage.
- **The draw offset is a buffer binding offset**, not a base-instance draw:
  OpenGL ES has no `glDrawArraysInstancedBaseInstance`.
- **The glyph atlas is single-channel coverage**, narrowed from Cairo's ARGB32
  on the CPU. Uploading BGRA and letting the driver keep one channel is an
  OpenGL convenience with no Vulkan equivalent.
- **The shaders have one source.** `assets/shaders/*.glsl` are written in the
  common subset of GLSL 3.30, GLSL ES 3.00 and Vulkan GLSL; the version
  directive, precision qualifiers, varying locations and uniform declarations
  come from a preamble generated per backend in `src/render/shader_source.cpp`.
  The SPIR-V the Vulkan backend loads is produced at build time from those same
  bodies through that same generator, so the two forms cannot drift apart.
- **Clip space differs and the backend absorbs it.** Vulkan's +Y points down;
  `DeviceVK` negates the row of the transform that produces clip-space Y, so
  callers hand every backend the same conventional matrix. Framebuffer rows
  likewise: picking coordinates are window coordinates, top-down, and the
  OpenGL backend flips them internally.
- **One transform per draw, not a per-frame camera.** `DrawUniforms` carries
  `projection * view * model` outright. A per-frame camera would live in
  storage every draw of the frame shares -- a descriptor-backed block on
  Vulkan, which a recorded command buffer cannot rewrite between draws -- and
  the notification overlay could then not use a different projection from the
  documents behind it.

### Threads: what Vulkan can do that the GL family cannot

`RenderDevice::capabilities()` reports a `DeviceCapabilities`, and the field
that actually differs between backends is `parallelCommandRecording`. Vulkan
sets it; OpenGL and OpenGL ES do not, and not because their drivers are slower.
A GL context is current on one thread at a time and every call that records work
goes through it, so a second thread cannot build part of a frame -- making the
context current elsewhere would first have to release it here, which serialises
rather than overlaps. Vulkan separates recording from submission: each thread
records into its own command buffer out of its own command pool, and one thread
submits the result.

Callers do not branch on the flag. `drawGlyphBatches()` takes the frame's page
draws as one list on every backend; a device that cannot split it records it in
order, and the default implementation on `RenderDevice` is exactly that loop.
The flag is there for deciding whether producing the list is worth the trouble,
and for reporting what a build can do.

**Where it is used.** The frame's document draws, which is the only part of the
frame whose size grows with the document: one draw per page, so 1152 of them for
the 4.6 MB sample. `Doc::collect()` appends them to one list across every open
document -- a per-document list would cap the work available to split at one
document's page count -- and `DeviceVK::drawGlyphBatches()` cuts that list into
one chunk per thread. Each chunk is recorded into its own secondary command
buffer and the primary executes them in chunk order, so which thread finished
first changes nothing about what is drawn. Vulkan 1.0 cannot mix inline commands
with executed ones in a subpass, so the sequential path records into a secondary
buffer too, and the two paths cannot drift apart.

**Whether it is used is measured, not assumed.** Splitting costs waking the
workers; recording a draw costs a few hundred nanoseconds. Which wins depends on
whether the machine has a core free for a worker to wake onto, which is a
property of the load and not of the code -- so `DeviceVK` times both strategies
over a few frames, keeps the cheaper one, and re-checks periodically. It says
which it chose through the diagnostic sink. `GLEDITOR_RECORD_THREADS` takes the
decision away from it, which is what lets both paths be compared: recording the
4.6 MB sample's 1152 draws on four threads and on one produces byte-identical
frames, and `tools/compare-backends.sh` checks that.

**What the measurements said.** `--benchmark N` draws N settled frames and
reports the median, with collecting the draws timed apart from recording them --
only the second can be split, and a figure covering both would hide it. On the
4.6 MB sample (1152 page draws), headless on four cores:

```
$ xvfb-run -a build/gleditor --backend vulkan --benchmark 40 tests/samples/kjv.txt
driver info: recording 1152 draws on 1 thread(s): 495 ns/draw split against 247 ns/draw in one piece
benchmark: 40 frames, 1152 page draws, median frame 731.315 ms, median collect 0.044 ms, median record 0.285 ms, ...
```

| | median frame | collect | record |
| --- | --- | --- | --- |
| Vulkan (lavapipe) | 731 ms | 0.044 ms | 0.285 ms |
| OpenGL (llvmpipe) | 738 ms | 0.044 ms | 729 ms |

Three things worth reading off that. Recording a frame costs about 0.3 ms, or
247 ns per draw -- material against a 16.7 ms budget, invisible against this
frame. The device measured the split at 495 ns per draw and declined it, and did
so on every repeat: this machine runs a software rasteriser that keeps all four
cores busy drawing the previous frame, so a worker waits for a core longer than
recording in one piece takes. `GLEDITOR_RECORD_THREADS=1` reproduces the
sequential side on its own. And the OpenGL `record` figure is not a recording
cost at all -- the GL driver blocks inside the draw calls, which is exactly what
separating the two columns exposes.

**Where the split does start to pay.** Several documents can be open at once,
and the draws add up: `gleditor a.txt b.txt c.txt` puts every page of all three
in the one list. Running the 4.6 MB sample two and three times over, still under
lavapipe on four cores:

| documents | page draws | median frame | collect | record |
| --- | --- | --- | --- | --- |
| 1 | 1152 | 731 ms | 0.044 ms | 0.285 ms |
| 2 | 2304 | 1649 ms | 0.077 ms | 0.695 ms |
| 3 | 3456 | 2474 ms | 0.115 ms | 1.279 ms |

The gap between the two strategies closes as the fixed cost of a split is spread
over more draws: the device measured them 2.0x apart at 1152 draws, 1.36x at
2304, and at 3456 the split finally won. Forced both ways with
`GLEDITOR_RECORD_THREADS`, 3456 draws record in 1.279 ms on one thread and
1.045 ms on four -- 18% off the largest CPU cost in the frame. So the crossover
on this machine is somewhere between two and three copies of the sample; on a
machine whose cores are not all busy rasterising, it would be far lower.

The chooser finds that when the two are far apart and is only mostly right when
they are close. At 1152 draws it declined on every run. At 3456, where the true
difference is 18%, it took the split in two runs of three (recording in 0.95 and
1.03 ms) and declined in the third (1.30 ms): the split's measured cost moved
between 277 and 400 ns per draw from one run to the next while the sequential
figure held between 371 and 394, so the spread in what it is measuring is about
as wide as the difference it is trying to resolve. This only happens where the
two strategies are close, which is also what bounds the cost of picking the
wrong one -- 0.25 ms out of a 2.5 s frame here. Where it would cost more, it has
been right every time. `GLEDITOR_RECORD_THREADS` settles it by hand when that is
not good enough.

The frame time is dominated by none of them: it is 4.6 million quads being
rasterised, every page of the document, every frame, whether or not the page is
on screen. Culling pages outside the view would cut both columns by about two
orders of magnitude, and is the optimisation this measurement most clearly
points at -- see TODO.

### Driver diagnostics

Both APIs report problems through a C function pointer the driver calls on its
own stack. An exception must not leave such a callback: unwinding through a
frame with no C++ exception tables is undefined, and it happens midway through a
driver call. The callbacks therefore only record, into a shared
`render::DiagnosticSink`; the device raises what was recorded from `endFrame()`,
its own code, where throwing is defined and the render loop's handler can report
it.

- **OpenGL and OpenGL ES** use `KHR_debug`, which is core only in OpenGL 4.3 and
  OpenGL ES 3.2 -- above what these backends require -- so the entry points are
  resolved optionally and a context without them simply reports nothing.
  Delivery is synchronous, so a recorded error belongs to the call that caused
  it rather than to some later frame.
- **Vulkan** records through its debug messenger when the validation layers are
  installed.

Everything recorded is logged once, deduplicated so a driver complaining every
draw call does not bury the log, and everything above a notice is also shown in
the window as a notification. The editor keeps running: a driver objecting to
something it can survive should not close the document you are editing.

`--strict-diagnostics` puts that back to fatal, and the render thread that stops
this way sets the process exit status. Automated runs want it, and
`tools/compare-backends.sh` passes it: a frame rendered while the driver was
reporting errors has proved nothing, however plausible it looks.

### Notifications

`ToastOverlay` draws a stack of expiring panels in the bottom-left corner,
through the same glyph pipeline the documents use. Two things make that
possible: the transform is per draw, so the overlay can pass an orthographic
projection in window pixels while the documents pass a perspective camera; and
its pipeline is created with `depthTest` off, so being submitted last is what
puts it on top.

The panel behind the text is one more glyph instance with its foreground and
background set to the same colour, which makes the fragment stage's blend
between them independent of whatever atlas coverage it samples. That gives a
solid panel without a blend state, a second shader or a reserved blank texel.

### Picking

The glyph pipeline writes a second colour attachment holding the identity of
whatever produced each fragment, so asking what is under the cursor is a
one-pixel read of that attachment.

The read is asynchronous on every backend, and the interface says so:
`requestPickingTag(x, y)` queues a read and `takePickingTag()` collects one that
has finished, returning the pixel it came from because by then the cursor has
usually moved. Results are collected at the top of the next frame, so a request
is never answered within the frame that made it.

- **OpenGL and OpenGL ES** read into a ring of pixel buffer objects. With a
  pixel pack buffer bound, `glReadPixels` queues the transfer and returns; a
  fence says when the bytes have landed, and it is polled with a zero timeout so
  the loop never blocks on it. Reading straight to client memory would instead
  stall the pipeline until the GPU caught up.
- **Vulkan** copies the pixel into a per-frame host-visible buffer just after
  the render pass, while the attachment is already in transfer-source layout.
  The frame's own fence says when the copy completed, so picking needs no
  synchronisation object of its own.

`--pick X,Y` prints the tag at one pixel and exits, which is how the backends
are compared.

### Editing: caret, clusters and reflow

Clicking on text places the caret. The caret is a byte offset into the
document, not a page and a coordinate: pages are a presentation that reflows,
and an offset survives that.

`tests/samples/ligatures.txt` holds `ffi ffl fi fl ff`. Rendered in a font with
those ligatures -- FreeSerif has all five; DejaVu Serif lacks `ffi`; Liberation
Serif has none -- each is drawn as a single joined glyph, and clicking across
one steps the caret through the characters inside it.

**One quad is one cluster, and a cluster is not a character.** Pango shapes
"ffi" into a single ligature, and a letter with its combining marks into a
single cluster; each is drawn as one quad covering several characters. The
character boundaries inside it have no geometry to click on. So the fragment
stage writes out how far across its quad each fragment sits, and the cluster's
character count says how many boundaries to divide that among -- subdividing a
cluster's width evenly, which is what Pango's own `x_to_index` does. Caret
geometry comes back from Pango's `get_cursor_pos`, which already knows about
right-to-left runs.

**Reflow stops where pagination re-syncs.** Typing splices the text
immediately and schedules the layout onto the render thread. A page that still
ends where it did, shifted by the bytes inserted, means every later page holds
byte-identical text: unchanged shaping, unchanged glyphs, unchanged vertex
rows, and only the offset it reports moves. An insertion that does not spill
its page rebuilds one page however long the document is -- 1 of 1152 pages on
the 4.6 MB sample.

The reflow reports its scope so the fast path is observable rather than merely
claimed: `line` when no line break moved, `page` when they moved but the page
still ends where it did, `document` when it did not. Line and page both confine
the work to one page; the distinction is diagnostic, since Pango cannot lay out
one line of a page again in isolation. A genuinely line-local relayout would
want a layout object per line.

The caret and the notifications write the picking attachment like everything
else, and are drawn last, so they cover the tag of whatever is beneath them.
They carry an identity of their own rather than zero: tagged zero a click on
the caret read as empty space and cleared it. Being transparent to picking
instead would mean masking writes to the second colour attachment alone, which
OpenGL ES 3.0 cannot do -- per-attachment colour masks arrive in ES 3.2 -- so
clicking the caret leaves it where it is.

### Selection

Pressing and dragging selects; the caret is one end of the span and the anchor
is where the drag began, so dragging backwards over the anchor needs no special
case. Clicking on empty space -- a pixel where nothing was drawn -- clears the
caret and the selection together and returns the editor to navigating.

**Highlighting respects character boundaries inside a quad.** A selection
ending between the "f" and the "i" of an "fi" ligature has to cover part of one
quad: the two characters share it and have no geometry to tell them apart.
Highlighting whole clusters would show a selection the user did not make. So
the decision is made per fragment rather than per instance -- a vertex only
knows its corner -- using the same across-quad fraction that resolves a click.
A span names clusters plus a fraction at each end, and the fractions are
quantised on the host to `k / charCount`, so an edge always lands on a
character boundary. Single-character quads are the same rule with nothing to
divide.

Selecting inside the three-character `ffi` ligature of
`tests/samples/ligatures.txt`, whose quad is 29 pixels wide on screen:

| selection | highlighted |
| --- | --- |
| first `f` | 10 px |
| `ff` | 19 px |
| whole `ffi` | 29 px |
| middle `f` only | 9 px, offset 10 px in |

`--select START,END` applies a span as a drag would, after any `--click` has
been answered -- a click replaces a selection rather than extending one.

`--click X,Y` and `--type TEXT` drive both without a mouse or a keyboard, which
is how caret placement is compared between backends. Every click reports the
pixel it answered, since picking is asynchronous and a reply that named only
the offset could not be lined up with the click that caused it.

## SDL2 and SDL3

Either major version works, chosen at build time with `GLEDITOR_SDL=2` or
`GLEDITOR_SDL=3`. Left unset, the Makefile uses SDL3 when pkg-config finds it
and SDL2 otherwise -- SDL3 is what the code is written against, SDL2 is what
most distributions still ship. CI builds and tests both, as parallel jobs.

`include/gleditor/sdl_compat.hpp` is where the difference lives. Everything
else uses SDL3 spellings, which that header supplies for SDL2; the only other
place with a version test is the optional `SDL_image` include in
`src/sdl_wrap.cpp`, whose header path differs and which most builds do not have
at all. The differences the header covers are of four kinds:

- Renamed symbols, which a macro settles.
- Calls that returned 0 for success in SDL2 and `true` in SDL3
  (`SDL_InitSubSystem`, `SDL_GL_MakeCurrent`). These are functions rather than
  macros: getting the sense backwards turns a failure into a success.
- Changed signatures: window creation, and both Vulkan entry points -- the
  latter in `render/vulkan/sdl_vulkan_compat.hpp`, so a build without Vulkan
  never includes `SDL_vulkan.h`.
- Window resize, which SDL2 reports as one event type carrying a sub-type and
  SDL3 reports as several event types. That is a difference in shape rather
  than in naming, so it is exposed as a predicate, `sdl::windowSizeChanged()`,
  instead of a constant.

SDL2 builds require 2.0.22 or newer, for `SDL_GetWindowSizeInPixels`. The
startup banner names the version the binary was built against, since nothing on
the command line can change it.

## Tech stack

- Language: C++23
- Build system: GNU Make (no CMake)
- Compiler: clang++ by default (gcc should work)
- Package discovery: pkg-config
- Libraries (via pkg-config):
  - pangomm-2.48 (Pango) and cairomm
  - SDL3 or SDL2 (see below)
  - SDL_image (optional; supplies the window icon and nothing else, and is
    skipped when pkg-config cannot find it)
  - Vulkan (only with `GLEDITOR_ENABLE_VULKAN=1`)
  - GLM (headers)
  - The OpenGL and OpenGL ES entry points are resolved at run time through
    `SDL_GL_GetProcAddress`, so no GL library is linked. `GL/glcorearb.h` is
    still needed for its typedefs and enum values.
- Testing: GoogleTest + GoogleMock
- Vendored/third-party: `thirdparty/argparse`, `thirdparty/Choreograph`, `thirdparty/cosmopolitan` toolchain support (optional)

## Requirements

You’ll need a C++23 toolchain, `make`, and the libraries above.

The vendored dependencies are git submodules, so clone with them:

```
git clone --recurse-submodules https://github.com/ccs4ever/gleditor
# or, in an existing clone:
git submodule update --init --recursive
```

On Ubuntu/Debian, for example:

```
sudo apt-get update && sudo apt-get install \
  clang libclang-rt-dev make pkg-config doxygen \
  libglm-dev libpangomm-2.48-dev \
  libsdl3-dev \
  libgl-dev libgl1-mesa-dev libglu1-mesa-dev \
  libgtest-dev libgmock-dev
```

Substitute `libsdl2-dev` for `libsdl3-dev` to build against SDL2; see
"SDL2 and SDL3" above.

For the Vulkan backend, additionally:

```
sudo apt-get install libvulkan-dev glslang-tools
# a driver, plus a software one for headless testing:
sudo apt-get install mesa-vulkan-drivers vulkan-validationlayers
```

Notes:
- SDL3 is not yet in the Ubuntu archive; the CI workflow pulls it from the
  `ppa:hrzhu/sdl3-backport` PPA for its SDL3 job. Building SDL3 from source
  works equally well, and `libsdl2-dev` from the archive avoids the question
  entirely -- see "SDL2 and SDL3" above.
- That PPA has no SDL3_image, which is why the dependency is optional: without
  it the window simply has no icon. Install `libsdl3-image-dev` or
  `libsdl2-image-dev`, or build it from source, to get one.
- The default `LDFLAGS` include `-rtlib=compiler-rt`, which needs the LLVM
  runtime package (`libclang-rt-dev` on Debian/Ubuntu).
- spdlog is not used at present (it was removed due to libc++ linking issues).
- For coverage (`make profile`), install `llvm-profdata` and `llvm-cov` (e.g., `llvm-14-tools` or similar on Ubuntu).  
- Headless testing works with Mesa's software drivers: `llvmpipe` for OpenGL and
  OpenGL ES, `lavapipe` for Vulkan. Both come from `mesa-vulkan-drivers` and the
  usual Mesa packages.

## Build

Common targets (see `Makefile`):

- Build everything (app, tests, compile commands):
  - `make`  → builds `build/gleditor`, `build/gleditor_test`, and `build/compile_commands.json`
- Build with the Vulkan backend:
  - `make GLEDITOR_ENABLE_VULKAN=1`  → also compiles `assets/shaders/vulkan/*.spv`
- Build against a particular SDL:
  - `make GLEDITOR_SDL=2` or `make GLEDITOR_SDL=3`
- Compile the SPIR-V modules only:
  - `make shaders`  (needs `glslangValidator`)
- Build the app only:
  - `make gleditor`  → `build/gleditor`
- Build tests only:
  - `make gleditor_test`  → `build/gleditor_test`
- Clean:
  - `make clean`  → removes `build/` artifacts

Compile commands database (for clangd, etc.):
- Generated automatically by `make` at `build/compile_commands.json`.
- You can also run: `make build/compile_commands.json`.

Optional Make variables:
- `DEBUG=1` enables debug flags and sanitizer flag sets.
- `GLEDITOR_ENABLE_VULKAN=1` compiles the Vulkan backend.
- `GLEDITOR_SDL=2` or `GLEDITOR_SDL=3` picks the SDL major version; unset means
  SDL3 when pkg-config finds it, SDL2 otherwise.
- `STATIC=--static` attempts static linking for libs resolved via pkg-config.

Objects are rebuilt when the compile flags change, so toggling either of the
first two does not leave a binary that disagrees with what was asked for.

## Run

- After building, run either:
  - `make run`  (runs `build/gleditor`), or
  - `./build/gleditor [options] [files...]`

Command-line options (from `argparse` in `src/main.cpp`):
- `--font <name>`     default: `"Monospace 16"`
- `--backend <name>`  `opengl` (default), `opengles` or `vulkan`
- `--profile`         open any provided files and then exit (useful for profiling)
- `--screenshot <path>` write the first settled frame to `<path>` as a binary PPM
- `--pick X,Y`        print the picking tag at that pixel once the document has
  settled, then exit
- `--toast [severity:]text`  show a notification over the document; severity is
  `info`, `warning` (the default) or `error`. Driver diagnostics raise these on
  their own; this is how the overlay is exercised on demand and compared
  between backends
- `--click X,Y`       click there once the document has settled, moving the
  caret, and print where it landed; may be given more than once
- `--type TEXT`       insert TEXT at the caret once it has been placed
- `--select START,END`  select that document byte range once the document has
  settled, as a drag would
- `--benchmark N`     draw N frames once the document has settled, report how
  long they took, and exit
- `--strict-diagnostics`  treat a driver error as fatal instead of showing it
  as a notification
- `files...`          one or more input files to open at startup

Help:
- `./build/gleditor --help`

## Running with sanitizers

Sanitizers are wired via Make targets. Set `DEBUG=1` to activate sanitizer flags:

- AddressSanitizer:
  - `make clean && make DEBUG=1 sanitize/address`
  - Run with env defaults: `./build/gleditor`
  - Or use the helper: `make DEBUG=1 sanitize/address/run`
- ThreadSanitizer:
  - `make clean && make DEBUG=1 sanitize/thread`
  - Or: `make DEBUG=1 sanitize/thread/run`
- MemorySanitizer: (requires the entire dependency chain to be MSAN-instrumented)
  - `make clean && make DEBUG=1 sanitize/memory`
  - Or: `make DEBUG=1 sanitize/memory/run`

Note: the `/run` targets set recommended `ASAN_OPTIONS`, `TSAN_OPTIONS`, or `MSAN_OPTIONS` and then execute the binary.

Known sanitizer noise: the sanitizer flag sets include `-fsanitize=integer`,
whose `implicit-integer-sign-change` check fires inside libstdc++'s `<format>`
whenever a negative integer is formatted (`format:3510` and `format:1016` in
GCC 13). Those reports come from the standard library's own modular arithmetic,
not from `gleditor`.

## Usage: Movement and Commands

Movement keys:

```
            Keys        How they move
             e               up
          s d/D f   left  zoomout/in  right
             c              down
```

Other actions:

| Key | Action |
| --- | ------ |
| n   | Create a new page |
| r   | Reset view back to start |
| q   | Quit the application |
| g   | Increment fov by 1 (max 360); use Shift+g to decrement (min 1) |

## Tests

- Build and run tests:
  - `make test`  → builds and runs `build/gleditor_test`
- Compare the backends against each other:
  - `./tools/compare-backends.sh [file]`

  Renders the same document through every compiled-in backend and diffs the
  captured frames, then compares the picking tag each one reports at a set of
  pixels. Exiting zero proves very little about a renderer -- a backend that
  draws nothing still exits zero -- so this comparison is what actually
  demonstrates a backend works. OpenGL and OpenGL ES are required to match
  exactly; Vulkan is allowed a small tolerance for edge rounding, since it
  rasterises through a different pipeline. Picking must agree exactly on all
  three, and at least one queried pixel must report something.

  It then renders the same document through Vulkan twice more, recording the
  frame on four threads and on one, and requires the two to be byte-identical.
  Chunks are recorded out of order and executed in order, so an ordering
  mistake shows up here and in no other check. A document with too few page
  draws to split says so instead of reporting a pass -- pass a larger sample,
  such as `tests/samples/kjv.txt`, to exercise it.

  Headless, with software drivers:

  ```
  xvfb-run -s "-screen 0 1024x768x24" ./tools/compare-backends.sh
  ```
- Coverage from tests:
  - `make profile`  → generates `gleditor_test.prof` and `coverage.lcov`
    - Requires `llvm-profdata` and `llvm-cov` on PATH.

## Documentation

- Generate API docs with Doxygen:
  - `make doc`
- Output is written to `docs/` (configured in `Doxyfile`).

## Project structure

- `src/`        application sources (document model, glyph cache, SDL wrappers, etc.)
- `src/render/` the device abstraction and its backends (`gl/`, `vulkan/`)
- `include/`    public headers under `gleditor/`
- `assets/shaders/` portable GLSL bodies, plus generated SPIR-V under `vulkan/`
- `tools/`      build-time and verification helpers
- `tests/`      unit tests (GoogleTest/GoogleMock)
- `thirdparty/` vendored dependencies (argparse, Choreograph, cosmopolitan, etc.)
- `assets/`     assets like `logo.png`
- `docs/`       Doxygen output directory
- `Makefile`    build orchestration
- `Doxyfile`    Doxygen configuration
- `LICENSE`     project license

## Environment and Make variables

- `DEBUG=1`  enable debug flags and sanitizer options in builds.
- `GLEDITOR_ENABLE_VULKAN=1`  compile the Vulkan backend and its SPIR-V.
- `GLEDITOR_SDL=2` / `GLEDITOR_SDL=3`  build against that SDL major version.
- `GLEDITOR_RECORD_THREADS=N`  threads the Vulkan backend records a frame with,
  and an instruction rather than a ceiling: setting it takes the choice away
  from the device, so `1` records in one piece and anything more always splits.
  That is what lets the same binary be run both ways, which is how the two
  paths get timed and compared at all. Ignored by the other backends, which
  cannot split.
- `STATIC=--static`  attempt static linking (where supported by your system/libs).
- `ASAN_OPTIONS`, `TSAN_OPTIONS`, `MSAN_OPTIONS`  fine-tune sanitizer behavior (the `/run` targets set sensible defaults).
- LandlockMake: if `LANDLOCKMAKE_VERSION` is set, the Makefile enables a sandbox for builds (optional/developer setup).

## License

This project is licensed under the GPL-3.0. See `LICENSE` for details.  
TODO: Confirm whether the intent is GPL-3.0-only or GPL-3.0-or-later.

## TODOs / Notes

- Cross-platform support (Windows/macOS) has not been documented or tested yet.
- Packaging/distribution not defined.
- The app resolves `assets/glsl` and `logo.png` relative to the working
  directory, so it must be started from the repository root (`make run` does).

