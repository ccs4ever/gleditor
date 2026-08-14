# gleditor

A GPU-rendered document library, with OpenGL, OpenGL ES and Vulkan backends, and
two programs built on it.

[![C/C++ CI](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/ccs4ever/gleditor/actions/workflows/c-cpp.yml)

Still a work in progress.

## Overview

This tree builds three things.

**`libgleditor`** is the library: SDL for windowing and input, Pango and Cairo
for shaping and rasterisation, a device abstraction with three backends, a glyph
atlas, a buffer allocator, a paginated document model and a render loop. It
names no document format and no application. Everything under `src/` is part of
it.

**`gleditor`** is the plain editor -- open files, look at them, type into them.
It is `apps/gleditor/main.cpp`, and it is 119 lines: a command line, a key map,
and the library doing the rest.

**`xudu`** is a second program that keeps a versioned hypertext instead of a
file, after Ted Nelson's OSMIC and Project Xanadu. It is `apps/xudu/`. It shares
the library with the editor and shares no code with it.

The split is the point. See [Building on the library](#building-on-the-library)
for what a program gets to hook into, and [xudu](#xudu-a-xanadoc-editor) for
what one program did with it.

## Rendering backends

The graphics API is chosen at run time with `--backend`:

| Backend            | Requires        | Notes                                           |
| ------------------ | --------------- | ----------------------------------------------- |
| `opengl` (default) | OpenGL 3.3 core |                                                 |
| `opengles`         | OpenGL ES 3.0   |                                                 |
| `vulkan`           | Vulkan 1.0      | Only when built with `GLEDITOR_ENABLE_VULKAN=1` |

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

|                   | median frame | collect  | record   |
| ----------------- | ------------ | -------- | -------- |
| Vulkan (lavapipe) | 731 ms       | 0.044 ms | 0.285 ms |
| OpenGL (llvmpipe) | 738 ms       | 0.044 ms | 729 ms   |

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

| documents | page draws | median frame | collect  | record   |
| --------- | ---------- | ------------ | -------- | -------- |
| 1         | 1152       | 731 ms       | 0.044 ms | 0.285 ms |
| 2         | 2304       | 1649 ms      | 0.077 ms | 0.695 ms |
| 3         | 3456       | 2474 ms      | 0.115 ms | 1.279 ms |

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

There is one other thread boundary worth naming, because it is easy to cross by
accident. Documents are paginated on a loader thread, but each finished layout
is handed to the render thread through the render queue, and the `RefPtr` means
both threads then hold the same `Pango::Layout`. A layout computes its lines
lazily, so *asking it a question mutates it* -- `get_line_count()` shapes the
text. Anything the loader thread wants to know about a layout has to be asked
before the layout is handed over, not after. Doing it in the other order crashed
about one run in five, always somewhere inside Pango or glib's allocator, and
also corrupted the shaping badly enough to produce clusters 237 texels wide.

The frame time is dominated by none of them: it is 4.6 million quads being
rasterised, every page of the document, every frame, whether or not the page is
on screen. Culling pages outside the view would cut both columns by about two
orders of magnitude, and is the optimisation this measurement most clearly
points at -- see TODO.

### Drawing less: culling, and text too small to read

Two decisions are made per page before anything is submitted, both in
`Page::collect()`, because both need to know what the page is rather than what
the draw is.

**A page outside the view is not drawn.** Its box is transformed by the page's
own MVP and rejected when all eight corners fall outside the same clip plane.
The comparisons are made in clip space, before the perspective divide, so a page
behind the camera needs no special case -- dividing by a negative `w` is exactly
what would flip a sign and cull something visible. The test is conservative: a
page straddling the edge of the screen is kept, costing a draw rather than a
frame with a page missing from it. Only the four side planes are tested; the
depth range is the one thing the backends disagree on (OpenGL clips to `[-w, w]`,
Vulkan to `[0, w]`) and a document is spread sideways and downwards rather than
in depth.

**A page too small on screen is drawn as one solid bar per line.** The decision
is made from how big the page lands, not from how far away it is: distance says
nothing without the field of view and the size of the drawable, so
`screenScaleAt()` reads the projected size of one layout pixel straight off the
transform. Below `--coarse-below` screen pixels per layout pixel -- 0.15 by
default, about three pixels of glyph -- the page draws its coarse rows instead.

Those rows live in the same allocation, straight after the detailed ones, so
choosing between them is a byte offset and a count and there is no second buffer
to keep in step. They also share the glyph pipeline: a bar is a quad whose
foreground and background are the same colour, which makes the atlas sample the
fragment stage takes irrelevant.

How dark a bar should be is the only interesting part, and it is derived rather
than tuned. Two earlier attempts had a constant in them -- first a flat shade,
then a flat assumption about how much of its box a glyph inks -- and each was
right on one sample and 10 to 40 levels of brightness out on the other, because
their lines are not equally full. What the code does now is take the glyph boxes
the detailed path already places and the mean coverage the glyph cache measured
when it rasterised each cluster, and darken white paper in proportion. Nothing
is assumed about the typeface, and a font change carries through on its own.

**What it costs and what it saves.** On the 4.6 MB sample under lavapipe:

|                             | page draws | median frame |
| --------------------------- | ---------- | ------------ |
| one document, every page    | 1152       | 962 ms       |
| one document, culled        | 1          | 14 ms        |
| three documents, every page | 3456       | 2657 ms      |
| three documents, culled     | 2          | 15 ms        |

The last row is the one that matters: with culling, what a frame costs stops
tracking how much is open and starts tracking how much is on screen, which is
the only quantity bounded by the window.

Widening the view until several pages are on screen at once is what exercises
the coarse path; at `--fov 60` the same document culls to 7 pages, and those 7
cost 18.0 ms drawn as glyphs against 4.6 ms drawn as bars.

**The atlas is mipmapped, which is what stops minified text crawling.** A page
drawn smaller than its glyphs samples the atlas at less than one texel per
pixel, and without a mip chain each pixel takes whichever texel it happens to
land on. Nudge the camera and those samples jump between stroke and paper, which
is the shimmer you see when panning. Measured by zooming 0.33% -- a change a
filtered render should barely notice -- and asking how much of the page moved:

|        | pixels changed | mean change | changed by >32 levels |
| ------ | -------------- | ----------- | --------------------- |
| before | 6.5%           | 5.41        | 4.3%                  |
| after  | 10.9%          | 0.78        | 0.1%                  |

More pixels move and each moves less, which is exactly the shape of the fix: a
sub-pixel zoom should nudge everything slightly rather than flip a few pixels
between black and white. The violent changes -- the ones that read as crawling
-- fall by a factor of forty.

Mipmapping an atlas is not free of consequences, because a mip texel at level L
averages a 2^L block of level zero aligned to level zero's grid, and so reaches
up to 2^L-1 texels outside whatever it covers. Glyphs were packed edge to edge,
so level one alone would have averaged each glyph with its neighbour. Each glyph
now sits inside a zeroed border sized for the deepest level the atlas carries,
and the texture coordinates handed to the shader are narrowed back to the glyph,
so nothing downstream knows the border is there. Four levels reach one eighth
scale, a little past where the coarse path takes over entirely, so between them
the two cover every size a page is drawn at.

OpenGL has `glGenerateMipmap`; Vulkan has nothing equivalent and `DeviceVK`
blits each level from the one above with the layout transitions to go with it.
Either way it is `RenderDevice::generateMipmaps()`, called once a frame when a
glyph has been added rather than once per glyph -- loading a document adds
thousands of clusters between two frames, and the chain only has to be right by
the time something samples it. The two backends produce identical frames, which
is the useful check on a hand-written blit chain.

**The atlas is allocated small and grown on demand.** It used to be sized for
the worst case at startup, which is a poor trade in both directions: too large
for the hundred or so distinct clusters a document of plain English actually
uses, and still a hard ceiling for one that uses more. Now it opens at 512x512
in a single layer and grows when a glyph will not fit.

It grows sideways first, up to whatever `RenderDevice::textureLimits()` reports,
and only then adds layers. A layer twice as wide holds four times as much where
a second layer holds twice as much, and layers are the scarcer resource anyway:
the vertex packing names one in six bits, so 64 is the ceiling regardless of the
2048 lavapipe offers. Growing means a new texture object either way -- an array
texture cannot gain layers and a 2D texture cannot gain pixels -- so every glyph
already packed is written into the new one, at the texel it was already on.

That last part is why glyph coordinates are texels rather than a fraction of the
texture, with the shader dividing by `textureSize()` when it samples. A fraction
would move every glyph named by a page's vertex buffer the moment the atlas
doubled, and those buffers are already on the device. Texels do not move,
because growth only ever adds room above and to the right of what is packed.

Provoking it takes deliberate effort: after the fix below, the whole King James
Bible packs into one 512x512 layer and never grows at all. `tests/glyph_cache.cpp`
uses a mock device reporting whatever limits a test asks for, and checks the
order (size before layers), that no allocation exceeds what the device allowed,
that a glyph's coordinates survive a growth unchanged, and that a glyph too
large for any layer is refused rather than chased with reallocations that cannot
help. End to end, `GLEDITOR_ATLAS_SIZE=64` makes ordinary text overflow the
atlas twice over, and `tools/compare-backends.sh` renders that against the
ungrown frame on every backend.

That last comparison is not a byte comparison, and the reason is worth stating
because it looks like a tolerance chosen for convenience. A smaller atlas packs
glyphs at different texels, and a mip texel averages a block aligned to level
zero's grid rather than to the glyph, so the same glyph at an odd offset
genuinely has a different mip chain from one at an even offset. An atlas opened
at 256, which never grows at all, differs from one opened at 512 by as much as
one opened at 64, which grows twice -- so the difference is packing, not growth.
What a failed re-upload looks like is glyphs missing outright, and the limits
were set by breaking it on purpose:

|                   | mean change | >32 levels | >64   | >100   |
| ----------------- | ----------- | ---------- | ----- | ------ |
| working           | 0.64        | 0.38%      | 0.02% | 0.000% |
| re-upload dropped | 2.24        | 1.92%      | 1.19% | 0.739% |

The phase shift nudges pixels; a missing glyph flips them from ink to paper. The
limit that carries the check is the one a working atlas does not reach at all.

Fitting a Bible into one small layer was not the plan; it fell out of a bug the
work uncovered. A page bounds its layout by height and Pango implements that by
ellipsizing, so the last line is cut short while still reporting its full logical
length. Taking that length made each page claim bytes it never drew, and the
cluster walk then handed the cache single bitmaps up to 848 texels wide -- one
per page, hundreds of them. Measuring what the layout actually consumed removed
them, and with them a factor of 64 in atlas area.

**Occlusion queries would add nothing here, and were not used.** They answer
"is this hidden behind something already drawn", and after frustum culling there
is nothing left to hide behind anything: the 7 surviving pages are stacked with
gaps and overlap neither each other nor the pages of a document alongside. A
query would also cost a round trip -- the answer arrives a frame or two later,
the same latency the picking readback has -- for pages that are already down to
a handful. Frustum culling is what the geometry here rewards.

All three are checked rather than asserted, by `tools/compare-backends.sh`.
Culling is checked by rendering the same document with `--no-cull` and requiring
the two frames to be identical to the byte, on every backend. The coarse path is
checked by comparing the average brightness of the pages between the two paths,
which is what would give the switch away as the camera crosses it. The filtering
is checked by the 0.33% zoom above, with limits an order of magnitude under what
the unfiltered atlas measured.

One of those checks corroborated another, which is the sort of thing worth
noticing: mipmapping brought the coarse path's brightness *closer* to the
detailed path's, from 1.85 levels apart to 0.81. The two were derived
independently -- one from the glyph coverage the cache measured, the other from
whatever the rasteriser produced -- so them converging says both are now
right rather than agreeably wrong.

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

| selection       | highlighted           |
| --------------- | --------------------- |
| first `f`       | 10 px                 |
| `ff`            | 19 px                 |
| whole `ffi`     | 29 px                 |
| middle `f` only | 9 px, offset 10 px in |

`--click X,Y` and `--type TEXT` drive both without a mouse or a keyboard, which
is how caret placement is compared between backends. Every click reports the
pixel it answered, since picking is asynchronous and a reply that named only
the offset could not be lined up with the click that caused it.

The automation options are a script, carried out **in the order they are
written**, each waiting for the one before it: a click waits for its own
picking answer, and typing waits for the reflow it causes. So

```
gleditor --click 200,150 --type "one " --click 400,300 --type "two " \
         --pick 200,150 --screenshot out.ppm doc.txt
```

types in two places and then reports what ended up at the first of them --
rather than clicking twice and inserting everything at the second caret, which
is what a run that took each option in turn would do. `--select START,END`
takes its turn among them; a click replaces a selection rather than extending
one, so where it falls in the sequence decides whether it survives.

## Building on the library

Everything the plain editor does, it does through the same surface any other
program gets. There are six hooks, and each exists because something concrete
could not be written without it. None of them names a document format, a file,
or an application.

### Where text comes from -- `gleditor/text_source.hpp`

A document's constructor used to open a file, strip its byte order mark and
validate it. Only the validation is about documents; the rest is one answer to
"what text?". `TextSource` asks the question. `FileTextSource` is the old
answer and `MemoryTextSource` is the other obvious one; a program whose text is
computed rather than stored supplies its own.

```cpp
class TextSource {
  virtual std::string text() const = 0;   // UTF-8; need not be valid
  virtual std::string name() const = 0;
};
```

### What changed -- `gleditor/document_observer.hpp`

A document applied an edit and remembered only the result, so nothing could
journal, replay, undo or account for one afterwards. `DocumentObserver` is told
about insertions and removals, and carries the removed text so a recipient can
reverse an edit without having kept its own copy of the document.

Adding it meant giving `Doc` an `erase()` at all -- it could only insert -- and
making the reflow take a signed delta. That is in 64-bit arithmetic, because
the test that decides where pagination re-syncs compares unsigned offsets and
would otherwise wrap rather than go below zero.

### Colouring a range -- `gleditor/span_decorator.hpp`

The renderer could already paint a background behind a byte range, quantise its
edges to character boundaries inside a ligature, split it across pages and pack
it for the fragment stage. All of that was reachable only by making a
selection. A `SpanDecorator` returns byte ranges and colours, and the library
does not ask what they mean -- search hits, a diff, unsaved regions, whichever
parts of a document came from somewhere else.

The selection is built into the table first, and both reasons are properties of
what consumes it: the fragment stage returns on the first span covering it, so
an earlier entry wins an overlap, and the device keeps the first
`render::maxHighlightRanges` and drops the rest, so an earlier entry survives a
full table.

### Drawing -- `gleditor/frame_contributor.hpp` and `gleditor/canvas.hpp`

A `FrameContributor` is called once a frame with the camera and the render
state, after the documents and before the notifications. `deviceReady()` hands
it the device and the document pipeline description the first time both exist,
which is on the render thread and therefore after the program registered
itself.

`Canvas` is what it draws with: rectangles, lines and text through the glyph
pipeline, in pixels, with the transform supplied at draw time so the same
canvas can be a screen overlay or an object standing in the world. It is built
on the observation that made the notification panel possible -- a quad whose
foreground and background are the same colour ignores the atlas entirely, so a
solid rectangle needs no second pipeline, no blend state and no reserved blank
texel.

### Being a program at all -- `gleditor/app.hpp`

`Application` owns SDL, the window, the render thread and the event loop.
`CommandTable` maps a key and its modifiers to something to run, and can print
itself. `addCommonArguments()` registers the options every program here accepts
-- the backend, the font, and the whole set for driving a run without a person.
A program supplies its own options and its own keys and gets the rest.

### Reaching the render thread -- `runWithState()` and `editCaret()`

Editing a document takes a `RenderState`, which is created inside the render
loop and exists nowhere else, so a key bound to "delete the selection" had no
way to reach it. `AbstractRenderer::runWithState()` is that way, and
`editCaret()` is how a command asks what is selected.

## xudu: a xanadoc editor

[OSMIC](https://xanadu.com.au/ted/OSMIC/) is Ted Nelson's 1996 proposal, a
byproduct of Project Xanadu. Its argument is that the undo everyone ships is
destructive: go back five states, type one character, and the five you came
through are gone forever. Nelson's objection is that "the problems of versioning
and backtrack are not simple and need to be appreciated -- and solved -- in
their full complexity, rather than simplified for programmer convenience."

`xudu` implements it. The engine is `apps/xudu/core/` and needs no graphics
device, which is why it has its own test binary that does not link the library.

### The two spools

"In OSMIC, data is logically saved in the server as two cumulative spools --
that is, Append-and-Read-Only files."

Text that is typed goes into the **primedia spool** at an address it keeps
forever. Each edit goes into the **operations spool**, filed under the state it
produced. Nothing is ever removed from either.

An address says *which* content as well as where in it. The local spool is one
**scroll**; somebody else's append-only sequence is another, and two addresses
into different scrolls never overlap however close their numbers are. A scroll
only grows, so an offset into it is settled when the bytes are written and no
later event moves it -- in particular not a change in which torrent carries
that stretch. See [stable references](#stable-references-quoting-a-torrent) and
[scrolls](#scrolls-addresses-that-survive-being-repackaged).

### Nothing stores a version

"The server does not store versions. Nothing stores versions. Versions
themselves are not saved, but regenerated as needed from these two files."

A state's name is enough to rebuild it. In Nelson's numbering, "change 2 creates
state 2. A branch is given a letter, after which new integers begin with 1
again; thus change 2a4 creates state 2a4" -- so `2a4` is reached by replaying
`1`, `2`, `2a1`, `2a2`, `2a3`, `2a4` and nothing else. `MicroversionId::path()`
is that, and it is why there is no cache of documents to keep in step.

### Time branches

Editing a state that already has a successor does not overwrite it; it starts a
branch. That is one `if` in `Store::apply()`, and it is the whole of what OSMIC
is arguing for.

```
$ xudu --import notes.txt xanadoc     # imported as state 1
   ... type something ...             # state 2
$ xudu --version-id 1 xanadoc         # go back to 1
   ... type something else ...        # state 1a1; state 2 is untouched
$ xudu --map xanadoc                  # see all three
```

### Deleting keeps the content

Deletion is what OSMIC calls "rearrange to limbo": the version stops pointing at
the content, and the content stays in the spool. Every earlier state still
resolves, so going back to one still shows what it showed.

### Quoting costs a pointer

A version is "a list of pieces", each naming a run of primedia -- the same shape
as the edit decision list a film edit produces. Transcluding a passage inserts a
piece aimed at the address the original already uses, so there is one copy and
two documents showing it: Nelson's "conceptually there is only one copy of
anything".

Two consequences the implementation gets for free. Whether two documents show
the same content is a question about addresses, so it survives editing around
the quotation, where a text comparison would not -- and two documents that
merely happen to read the same are correctly *not* reported as sharing anything.
And a link's ends are primedia addresses rather than positions in a document, so
a link shows up on everything quoting that content, which is Nelson's criterion
that a "link to any portion is present on all manifestations".

Opening two branches side by side shades everything they have in common and
leaves only the words that differ plain:

```
$ xudu --version-id 2 --alongside 1a1 xanadoc
```

![Two branches of one document side by side, everything they share shaded, with
the hypertime map showing state 1 forking into 1a1 and
2](assets/xudu-intercomparison.png)

Both documents were state `1` until one was edited into state `2` and the other,
from `1` again, into `1a1`. The shading is not a text diff: it is the passages
whose primedia addresses the two versions have in common, which is why only
" EDITED" and " BRANCHED" -- the bytes that were typed separately -- come out
plain.

### Stable references: quoting a torrent

A local address is not a Xanadu address. `primedia.spool` offset 218 means
nothing on another machine and nothing on this one either once the machine is
gone, and a reference that stops resolving is exactly the rot Xanadu was meant
to avoid.

A torrent's info hash is the kind of name Xanadu asks for, and it already
exists. It is the SHA-1 of the bencoded `info` dictionary, so it is derived
from the content: nobody assigns it and nobody can reassign it, the same
content always produces it, and resolving it does not need one particular
server to still be answering. Every piece carries its own hash, so what arrives
can be checked against what was named.

That last property is the one that matters most here. Transclusion claims there
is only one copy of anything; without verification a reader has no way to tell
that copy from a substitution, and the claim is a hope rather than a fact.

```
$ xudu --torrent fox.torrent --quote 0,4,5 xanadoc
xudu: fox.torrent is magnet:?xt=urn:btih:41270f22...&dn=fox.txt (1 file(s), 218 bytes)
xudu: 1 quotes 41270f22... file 0 [4,9)

$ cat xanadoc/scrolls.spool
scroll 1 - -
segment 1 0 218 41270f227583fd10ef9c3e3d9aa71fea4117c24e 0 0 fox.txt
$ wc -c < xanadoc/primedia.spool
0
```

The local spool is empty. The document holds no content of its own at all --
only a reference into content addressed by its own hash, which anyone with the
reference can resolve and verify.

Alter one byte of the referenced file and the quotation stops resolving:

```
$ xudu --profile --screenshot before.ppm --torrent fox.torrent xanadoc   # 907 pixels of text
$ printf 'X' | dd of=fox.txt bs=1 seek=5 conv=notrunc 2>/dev/null
$ xudu --profile --screenshot after.ppm  --torrent fox.torrent xanadoc   # 0
```

Nothing is shown rather than something plausible, and nothing partial is shown
either: a piece hash covers a whole piece, so whole pieces are fetched and
checked, and if any of them fails the whole read returns empty. A partial
answer would be a substitution with extra steps, since nothing downstream can
tell verified bytes from unverified ones.

A document whose references cannot be reached still opens -- the quotation is
blank and everything else is intact -- because being unable to resolve a
reference right now is not the same as the document being corrupt.

### Magnet links

A magnet link is how one of these references is written down and passed around,
and `xudu` reads them as well as emitting them:

```
$ xudu --torrent fox.torrent --torrent 'magnet:?xt=urn:btih:41270f22...' xanadoc
```

Both spellings of the hash are accepted -- forty hex digits and the
thirty-two character base-32 form older links use -- along with `dn`, `tr`, and
BEP 53's `so` for naming particular files. A link carrying only a v2 `btmh`
hash is refused rather than half-understood: that names content by a SHA-256
merkle root, and accepting it would mean claiming to identify content not one
byte of which could be verified. A hybrid link carrying both is fine, and the
v1 hash is the one used.

What a magnet **cannot** do on its own is resolve. It carries the name and
nothing else that matters: the piece hashes and the file list live in the info
dictionary, which the link only refers to. A real client obtains that from the
swarm (BEP 9) and then checks that what it was given hashes back to the name it
started from. Since fetching from a swarm is the part behind `ContentSource`
that is not implemented, a magnet resolves here exactly when its metadata has
arrived some other way -- a `.torrent` given alongside it. A magnet with no
metadata is refused with an error saying so, rather than being recorded as a
reference that would silently read as empty forever.

### Fetching from the swarm

`--swarm` fetches quoted content from BitTorrent peers instead of only from
this disk. Without it a reference resolves only when this machine already holds
the bytes -- which quietly reintroduces the dependency on one particular
machine that addressing content by its hash exists to remove.

```
$ xudu --swarm --torrent fox.torrent --quote 0,4,9 xanadoc
xudu: swarm listening on port 42343
xudu: fox.torrent is magnet:?xt=urn:btih:dc308895...&dn=sample.txt (1 file(s), 218 bytes)
xudu: 1 quotes dc308895... file 0 [4,13)
```

Only the pieces a quotation needs are requested, not the whole file: a
quotation is usually a sentence out of something long, and fetching the rest
would make a reference cost what a copy costs. Whole *pieces* though, because a
piece hash covers a piece and says nothing about a fragment of one.

With a swarm, a bare magnet link resolves as well -- that is the case it was
designed for. The metadata it lacks is fetched from a peer (BEP 9), and
libtorrent accepts an info dictionary only if it hashes back to the name the
link carried.

`--peer HOST:PORT` introduces a peer directly, for when there is no tracker or
DHT to find one through.

Nothing above the fetching changed to make this work. `Resolver` still gets
whole pieces and hashes them against the torrent before returning anything, and
it does not know whether they came off a local disk or from a stranger. That is
the point of verifying: a peer is not trusted, so it does not have to be.

libtorrent is required to build xudu. It is not only the swarm: the ed25519 a
publisher's name is signed with comes from it too, so a build without it would
be a xanadoc editor whose documents cannot leave the machine that wrote them.
The build says so at configure time rather than producing one.

#### Testing it

Two peers that share a loopback are not really two peers. `make test/swarm`
puts each in a network namespace of its own, joined by a veth pair and nothing
else:

```
$ sudo make test/swarm
==> making a torrent of tests/samples/quick_brown_fox.txt
  218 bytes, 4 pieces, info hash dc308895c32545a2fb09f050d0be66164b234219
==> two network namespaces, joined by a veth pair
  seeder 10.77.0.1, leecher 10.77.0.2
==> starting the seeder in xudu-seed
  listening on 10.77.0.1:34941
==> running the swarm tests in xudu-leech
[  PASSED  ] 11 tests.
```

So the two have separate addresses, separate routing, and separate loopbacks:
`127.0.0.1` means something different to each of them and neither can reach the
other by accident. There is no DHT, no local discovery and no tracker, so the
swarm contains exactly the two peers that were introduced -- a transfer that
succeeds is this code talking BitTorrent over a network device and nothing
else.

The tests cover a byte range arriving, a range spanning a piece boundary, the
whole file, a magnet getting its metadata from the peer, a document quoting
content this machine never had, and a read of content nobody is seeding giving
up rather than hanging. One of them checks that the bytes really came over the
wire: it starts with an empty download directory and asserts afterwards both
that libtorrent's received-byte counter moved and that the file materialised.
Counting connected peers does not work for that -- a peer with nothing left to
give disconnects, so by the time a completed read can be asked about, the
connection it used is gone.

Four more cover BEP 46 over the same wire, with the seeder publishing a name it
mints at startup: the name resolving to the right info hash, content fetched
from nothing but a name, an unpublished name giving up rather than hanging, and
the salt being part of the name rather than decoration.

The seeder sets `allowManyConnectionsPerAddress`, which is worth explaining
because libtorrent recommends against it. These tests are several sessions in
one namespace, so they share an address while being separate peers in every
other sense; the default has the seeder refuse the newcomer for as long as the
previous connection is still being torn down, and the newcomer then waits for
content nobody will send. That was a real intermittent failure -- about one run
in five, always the test following the heaviest one.

It needs root, because creating a network namespace does, which is why it is
not part of `make test`. It runs as its own CI job.

BTFS -- the FUSE filesystem that mounts a torrent as a directory -- was
investigated as an alternative to this and as a basis for a shared permascroll
server. It fits the `ContentSource` seam with no new code and is still the wrong
choice, because a filesystem read of unavailable content blocks where this has
to fail quickly. The permascroll question has a better answer that BitTorrent
does support. See [design/btfs-and-permascrolls.md](design/btfs-and-permascrolls.md).

### Scrolls: addresses that survive being repackaged

Addressing a span by torrent works for content that is finished, and is wrong
for content still being written -- for a reason that only shows up later. An
info hash fixes the file list, the piece length and every piece hash, so
appending produces a *different* torrent. Anything that grows is therefore
carried by a succession of torrents, each sealed when it stopped growing. If a
span names the torrent, sealing changes what an address means, and it does so
silently: the old reference still resolves, to content that is no longer the
same passage.

So a span names a **scroll** and an offset within it. A scroll is one
publisher's append-only sequence, which only ever grows, so an offset is settled
the moment the bytes are written. Which torrent carries a given stretch is a
separate, replaceable fact, kept as a list of segments:

```
$ cat xanadoc/scrolls.spool
scroll 1 4b617dae...9d76 -
segment 1 0 4096 dc308895c32545a2fb09f050d0be66164b234219 0 0 part-0
segment 1 4096 1731 8f2a11bd7c04e6539ab8102ff6cd41e0b7a5d382 0 0 part-1
```

Two things follow, and they are the point:

- **Sealing moves no address.** A test quotes a passage, repackages the content
  into a torrent with a different piece length and therefore a different info
  hash, and the quotation still reads back the same bytes.
- **A quotation crossing a seal is one span.** It is fetched from both torrents,
  verified against both sets of piece hashes, and joined -- and nothing above
  the resolver can tell where one segment ended. It shades as one passage
  because it *is* one span.

That second point replaces a worse fix. The BTFS note originally proposed
teaching the extent merge that segment *n* ends where *n+1* begins; that treats
a symptom, and would leave every other piece of address arithmetic to be taught
the same lesson separately.

A scroll with a publisher's key is identified by that key, so learning it has
been re-sealed folds the new segment into the scroll already known rather than
creating a second one -- otherwise every existing reference would quietly stop
being recognised as the same content, which is transclusion coming apart. A
scroll with no key is content that exists only as a fixed torrent file, and is
identified by that file. Two packagings of the same bytes with no publisher are
*not* the same scroll, which is honest: nothing binds them together.

One behaviour changed. A range running past the end of a scroll used to be
clamped to what existed; it now reads as nothing. Nothing downstream can tell a
clamped answer from a complete one, and it matters more once a scroll grows: a
quotation reaching past the last sealed segment quotes content nobody has
published yet, and showing the part that exists would misrepresent it.

Stores written before scrolls are read from `origins.spool` and rewritten in the
new shape. A one-segment scroll's offsets are its file's offsets, so every span
already on disk keeps meaning what it meant.

### Names that outlive what they point at

An info hash names bytes, which is its strength and also its one limit: it
cannot name something still being written. Appending to a torrent produces a
different torrent, because the hash fixes the file list, the piece length and
every piece hash. A permascroll is exactly the thing that keeps growing, so its
address cannot be the address of its content.

BEP 46 supplies the missing indirection, and it is already deployed. A
publisher holds an ed25519 key pair; the public key is the permanent name, and
the DHT holds a signed, sequence-numbered pointer under it saying which info
hash is current. The key never changes and what it points at does. It is
written `magnet:?xs=urn:btpk:KEY` -- `xs`, "exact source", rather than `xt`.

```
$ xudu --swarm --dht-node 10.77.0.1:33493 --peer 10.77.0.1:33493 \
       --torrent 'magnet:?xs=urn:btpk:f12c8faa...' --quote 0,4,9 doc
xudu: swarm listening on port 35815
xudu: joining the DHT through 10.77.0.1:33493
xudu: magnet:?xs=urn:btpk:f12c8faa... is dc308895... (awaiting metadata)
xudu: 1 quotes dc308895... file 0 [4,13)
```

That reader was never told `dc308895...`. It was given a public key, asked the
DHT what the key currently means, and got an info hash back -- then fetched and
verified the content as usual, ending with a document whose `primedia.spool` is
zero bytes.

The reason a stranger's answer can be believed is that it is signed. A DHT node
is somebody asked to hold a value, and a stranger holding your address is
normally where rot and substitution come from. Here the answer carries a
signature over the sequence number and the payload, so the only party that can
move a name is the one holding the private key -- and resolving is untrusted in
exactly the way fetching a piece is untrusted. An unsigned, wrongly signed, or
non-BEP 46 answer is treated as no answer, never as a partial one.

The signing buffer is assembled here rather than taken from a library, and it
is checked against BEP 44's published vectors: given that specification's key
pair, this code reproduces its published signatures byte for byte. That check
matters more than most, because a buffer assembled slightly wrongly still signs
and still verifies against itself -- producing a name that resolves for its
author and for nobody else alive.

`--dht-node HOST:PORT` introduces a node, since no public router is contacted
unless named. `--private-dht` drops the public rule limiting the routing table
to one node per /8, which on a single private network would otherwise leave a
DHT of one.

This is the first step of the permascroll design in
[design/btfs-and-permascrolls.md](design/btfs-and-permascrolls.md), and it went
where it was meant to: a name resolves to an info hash and nothing below that
changed. The next step is not the append protocol but the smaller thing it
rests on -- moving a span's address off the torrent that happens to carry it
and into scroll coordinates, so that sealing a segment cannot disturb a
reference already handed out.

**What is implemented and what is not.** The addressing, the verification and
the fetching are all real. What is not here is any notion of who may read what:
Nelson's model has a payment and permission story around "feed and sale... from
original", and this has none -- content in a swarm is content anyone can take.

`--quote` names a range on the command line because picking a byte range of a
remote document is a user-interface problem this has not solved yet, not
because the model needs one.

### Publishing, and reading somebody else's document

Everything above is addressed for one machine. A span names a scroll by a small
integer that means something only in the store that handed it out, and a
microversion is called "2a4", which is a name among the versions of one
document in one store. None of it travels.

Publishing rewrites a document into names that do. Every piece becomes a
`btpk:KEY:salt` scroll key and an offset, the links whose ends are all
addressable come along, and the whole manifest is signed by the ed25519 key
this machine publishes under. A manifest that does not verify does not decode:
an unsigned or wrongly signed document is not a weaker document to show with a
warning, it is somebody's claim to have published what they did not.

```
$ xudu store --author-name 'Ada Lovelace' --author-email ada@example.org \
             --import essay.txt --publish essay
xudu: publishing as Ada Lovelace <ada@example.org> (kept in ~/.config/xudu/config.yaml)
xudu: minted this machine's name e8a417cb...
xudu: published 1 as store/published/essay.xanadoc
```

Reading one in is the other direction, and it is what makes a published
document a thing rather than a file. Its scrolls become addresses this store
can resolve, its pieces become a version here, and its links become links here.
Nothing is copied: the pieces point at the publisher's scrolls, so this store
and theirs point at one copy of the content, which is what makes a link about a
passage of it a link about the same passage.

```
$ xudu mine --torrent theirs/published/primedia.torrent \
            --torrent-data theirs/published \
            --read theirs/published/essay.xanadoc
xudu: read "essay" by e8a417cb… seq 1786617153, 205 bytes in 1 pieces as a1
```

From there a document written here -- never published, with no global name of
any kind -- can be linked to a passage of theirs with `ctrl-l`. That asymmetry
is the point: what a link relates is content, and only the end that has to
travel needs a name that travels. Requiring both ends to be published would
make linking a privilege of publishers.

### Provenance: who wrote this, in a form somebody can check

The ed25519 key a publication is signed with answers "the same publisher as
last time" perfectly well and answers "who is this" not at all. It was minted
by this program, means nothing outside it, and is bound to no person.

So before anything is sealed, publishing writes a YAML record naming the author
and has GnuPG sign it:

```yaml
# Authorship of a xanadoc, signed with OpenPGP before the content was
# sealed into a torrent, and sealed into it alongside the content. The
# signature is in AUTHORSHIP.yaml.asc; check it with:
#   gpg --verify AUTHORSHIP.yaml.asc AUTHORSHIP.yaml
author: "Ada Lovelace"
email: "ada@example.org"
title: "essay"
salt: "essay"
publisher: "e8a417cbfd100d73ea604c176aafebf78591070eb32f1415cfd953bc358398b2"
version: "1"
published: 1786617153
content_length: 205
content_sha256: "1ff7117d4101e3b14d5d3e8db1c251ddf4375772691d8917cb8455cb0b124031"
```

Three decisions, each earning its keep:

*YAML, not the bencode everything else uses.* The audience is a person deciding
whether to believe it, and `gpg --verify AUTHORSHIP.yaml.asc AUTHORSHIP.yaml`
is the whole procedure -- no part of it needs this program.

*Signed before the seal, and sealed in.* The torrent carries three files: the
content, the record, and the signature. The info hash therefore covers the
content and the claim about who wrote it together, and neither can be swapped
for the other afterwards without producing a different address. The content is
file zero at offset zero, so every address already handed out still points
where it did.

*It says what it covers.* The length and SHA-256 of the sealed content are in
the record, so a reader holding both can tell the record is about what arrived
with it.

```
$ xudu --check-authorship theirs/published/primedia
...
xudu: signed by Ada Lovelace <ada@example.org>
      key 80BCBB97490F21119BCC58FE324FBE808844BE88
      which is a key this keyring trusts
      and it is about the content sealed with it
```

Whether the signature is good and whether the key is anybody you know are
reported as the two separate questions they are. A valid signature by a key
your keyring has never heard of is a real signature by somebody you have no
reason to believe, and reporting the two as one answer is how a signature
becomes a rubber stamp.

GnuPG is what signs, rather than something here: the whole value of an OpenPGP
signature is the key management and the web of trust around it, none of which
this program has any business duplicating -- and a reader will reach for gpg to
check it, so gpg is what should produce it.

gpg is run with an argument vector rather than a command line, on every
platform: an author's name and a passphrase are somebody else's text, and
handing text to a shell is how text becomes commands. What that means differs
by platform. POSIX has one call that both builds the argument list and takes
it -- `posix_spawn` -- so there is nothing to assemble. Windows has no such
call; `CreateProcess` takes a single string, so the argument vector is joined
into one by hand, quoted the way `CommandLineToArgvW` will read it back apart
again (`apps/xudu/core/windows_quoting.cpp`, checked by
`tests/xudu/windows_quoting.cpp` on every platform this project tests on, since
the algorithm itself needs no Windows API to run). Either way, the passphrase
goes down a pipe to gpg's stdin rather than onto the command line, which is
readable by every other process on the machine.

#### Who you are, kept where your other settings are

Identity belongs to a person, not to a document. Being asked to state it again
per store is how it ends up spelled three ways, or omitted -- and an omitted
author is an unsigned document. So it lives in the per-user configuration
directory, in the same small YAML the authorship record is written in:

```yaml
# ~/.config/xudu/config.yaml  ($XDG_CONFIG_HOME is honoured; $XUDU_CONFIG
# names a file outright, which is what lets somebody keep two identities)
author: "Ada Lovelace"
email: "ada@example.org"
gpg_key: "ada@example.org"       # which key in the keyring, when it holds several
gpg_home: "/media/key/.gnupg"    # optional: a keyring other than the usual one
```

`--author-name`, `--author-email` and `--gpg-key` write it; `--show-config`
prints where it is and what it says. Values are merged rather than replaced, so
setting only a key does not blank the name set last week.

The key is named rather than located: gpg signs with keys in a keyring, so what
the configuration says is *which* of them -- a fingerprint, a key id, an email
address, anything gpg accepts -- and `gpg_home` says which keyring when it is
not the usual one. Saying nothing at all is the ordinary case, and means the
key gpg would reach for on its own: whatever `default-key` names, or failing
that the first key that can sign.

Three places can say who publishes, and the nearest wins: this file is who
somebody is; `author.yaml` in a store, written by `--author-here`, is who they
are for that store -- a pen name, a work identity; and the publish dialog is
who they are for one publication.

#### The publish dialog

Publishing is the irreversible act in this program. A document goes out under
somebody's name, signed, and cannot be recalled from whoever has it -- so
ctrl-shift-s does not publish, it asks:

Everything the record will say is filled in from the configuration and the
store and is editable before anything is signed. Required fields are marked and
an attempt to go ahead without one says which is missing rather than publishing
something half-filled; tab moves between fields, enter goes ahead, escape
leaves it. While it is up it has the keyboard: the text being typed into a
title cannot land in the document behind it, and clicks are held.

The signing key is a drop-down of what the keyring actually holds, because a
fingerprint is not something anybody remembers and a key that turns out not to
be there is a failure at the last step of publishing. It starts on the key the
configuration names, or on the one gpg would use if it names none. Space opens
the list, up and down move through it, enter takes one; left and right step
through the options without opening it at all.

Underneath it is a passphrase, for the case where the key has one and no agent
is holding it -- shown as asterisks, with a button beside it that reveals it.
What is typed there is used for that one signature and kept nowhere: a
passphrase in a settings file is a passphrase somebody else can read. It
reaches gpg down a pipe rather than as an argument, because a command line is
readable by every process on the machine.

`--do publish` opens it from a script, `--type` fills in the field the keyboard
is on, and `--key tab|enter|escape|...` presses what is not text -- which is how
the dialog is tested, and how any other command can be driven without a person
at the keyboard.

When publishing goes wrong -- an unknown key, a refused passphrase, a keyring
with nothing in it that can sign -- what comes back is a native message box,
put up by SDL. See "What SDL provides for dialogs" below for why that half is
the platform's and the form itself is not.

### Commands

Control is used throughout, because a bare letter is text: the whole point of
this program is that typing is an edit, so it has to reach the document.

| Key                       | Does                                                      |
| ------------------------- | --------------------------------------------------------- |
| `ctrl-b` / `ctrl-n`       | go back or forward in hypertime, losing nothing           |
| `ctrl-t`                  | quote the selection into a second document                |
| `--torrent`, `--quote`    | quote a range of a torrent-backed file; see above         |
| `ctrl-l`                  | mark one end of a link, then join it to another selection |
| `ctrl-shift-l`            | forget a link that was begun and not finished             |
| `ctrl-k` / `ctrl-shift-k` | show or hide the beams; stop them moving documents        |
| `ctrl-m`                  | show or hide the hypertime map                            |
| `ctrl-p`                  | print every state to the terminal                         |
| `ctrl-s` / `ctrl-shift-s` | write the spools out; ask what to publish, then publish   |
| `ctrl-q`                  | save and quit                                             |
| `backspace`               | stop pointing at the selection                            |

### What the library did not learn

Nothing in `src/` or `include/` mentions a version, a transclusion, a link or a
spool. Xudu reaches the library as a `TextSource` that rebuilds a version on
demand, a `DocumentObserver` that turns each keystroke into a hyperop, a
`SpanDecorator` that shades shared passages, and a `FrameContributor` that draws
the map. Deep intercomparison -- the shading in the picture above -- took no
code in the library at all.

## Accessibility

Everything this program puts in its window is quads: a document, a caret, a
notification, a form field, a beam between two passages. A quad is a picture of
a thing rather than the thing, and nothing can read one back. So there is a
second description, kept beside the drawing and built from the same state, and
it is handed to the platform's assistive technologies through
[AccessKit](https://accesskit.dev) -- UI Automation on Windows, AT-SPI on X11
and Wayland, NSAccessibility on macOS.

### What is described

| On screen                      | As a node                                                            |
| ------------------------------ | -------------------------------------------------------------------- |
| the window                     | `Window`, named by its title, sized to the drawing area              |
| each open document             | `MultilineTextInput`, holding one `TextRun` per line                 |
| the caret and the selection    | a text selection on the document node, in characters                 |
| the notification overlay       | a `Log` whose entries are announced; an error interrupts             |
| the publish dialog             | a modal `Dialog` of labelled fields                                  |
| its drop-down of keys          | a `ComboBox` with a `ListItem` per key                               |
| its passphrase field           | a `PasswordInput`, whose value is asterisks and never the passphrase |
| its reveal button              | a `Switch`                                                           |
| Xudu's links between documents | a `List` of `Link`s, each followable                                 |
| Xudu's hypertime map           | a `List` of states, with the current one marked                      |

A document is described as text runs rather than as one long value because
that is the unit an assistive technology navigates in: it asks for the
character at an offset, the word around one, the line above. Each run carries
its text, the byte length of every character in it -- which is what makes an
offset in characters mean something in a UTF-8 string -- and the character
index each of its words begins at. A run is a line. A line with no break in it
for two hundred characters is cut anyway, at a space where there is one,
because word starts are carried a byte apiece and a word beginning past the
two hundred and fifty-fifth character of a run cannot be described at all.

Actions come back the other way. Clicking a document or one of its lines puts
the caret there; clicking a beam follows the link; clicking a state in the map
goes to it; a form field can be focused, a switch flipped, a drop-down entry
chosen, a text field set. They arrive on AccessKit's own threads, are queued,
and are carried out by whichever thread owns the thing being acted on --
which for a document means the render thread.

### How it is put together

```
include/gleditor/a11y/tree.hpp        the vocabulary: nodes, roles, actions
include/gleditor/a11y/publisher.hpp   collecting what everything has to say
include/gleditor/a11y/platform.hpp    where it goes
include/gleditor/a11y/documents.hpp   the documents, the runs and the caret
src/a11y/platform_accesskit.cpp       AccessKit
src/a11y/platform_none.cpp            no AccessKit
```

The vocabulary is its own rather than AccessKit's, for three reasons and the
third is the one that matters. It is plain data, so building a tree is testable
without a device, a window, a D-Bus session or a screen reader. AccessKit's own
node vocabulary is the intersection of several platforms' ideas of what a user
interface is, and naming the small part of it a text editor needs is what keeps
the mapping readable. And a build without AccessKit should not be a build where
half the UI code is conditional: `platform.hpp` is where the whole of that
difference lives, the tree is assembled either way, and only its destination
changes. A description that is only built in some builds is one that is only
correct in some builds.

The two threads are the same split as everywhere else here. The documents and
the caret belong to the render thread, so that is where the tree is built --
once a frame, and only when one of its sources says something has changed.
AccessKit's Windows and macOS adapters must each be talked to from the thread
that created the window, so that is where it is sent. The tree is a value and
crosses under a lock.

Nothing calls back into the program from AccessKit's threads. The activation
handler -- which fires whenever an assistive technology starts, possibly an
hour into the session -- is answered from the last tree, kept for the purpose;
action requests are queued and polled for by the event loop.

### Windows, X11, Wayland and macOS

X11 and Wayland share one adapter, and that is not a simplification: AT-SPI is
a D-Bus protocol, so an assistive technology on Linux talks to the
accessibility bus and never to the display server. The one place the display
server shows through is where the window is on the desktop, which every node's
rectangle is offset by. A Wayland client is not told its own position; SDL
answers with zeroes there, which means bounds are reported relative to the
window, which is what a Wayland assistive technology expects.

Windows uses the subclassing adapter, which installs itself on the window
procedure to answer `WM_GETOBJECT` and can only do so before the window has
ever been shown. That is why the window is created hidden on Windows and shown
a few lines later, and why `Application::run()` opens accessibility between the
two.

macOS speaks a third API, NSAccessibility, which is neither a bus nor a
message: an assistive technology asks the window's content view directly, so
AccessKit's macOS adapter subclasses that view -- dynamically, at run time,
since this program does not own the class SDL created for it. Unlike Windows
this needs no particular ordering against the window being shown, because
nothing has to be installed before the first message a window procedure could
receive; NSAccessibility just asks the view whenever it asks.

One thing macOS does need that neither of the others does: SDL's own `NSWindow`
subclass answers `accessibilityFocusedUIElement` itself rather than deferring
to its content view, because SDL puts the keyboard focus on the window and not
the view. Left alone, whatever AccessKit put the focus on would never be
reached. `openPlatform()` patches SDL's window class once, before opening the
adapter, to forward that one query down to the content view --
`accesskit_macos_add_focus_forwarder_to_window_class_with_length()`, the
function AccessKit ships for exactly this: "windowing libraries such as SDL
that place the keyboard focus directly on the window rather than the content
view."

### Building it

AccessKit is reached through
[accesskit-c](https://github.com/AccessKit/accesskit-c), its C bindings: one
header and one library. Releases are published as archives holding an
`include/` and a `lib/<os>/<arch>/`, and distributions that package it install
a pkg-config file. The build looks in three places, in the order somebody is
likely to have arranged one:

- `ACCESSKIT_DIR`, naming an unpacked release or a source tree that has been
  built. Spelled the same as accesskit-c's own CMake option, so a directory
  that works there works here.
- pkg-config, for `accesskit`.
- the compiler's own include path.

`GLEDITOR_ENABLE_A11Y=1` makes it required and its absence an error;
`GLEDITOR_ENABLE_A11Y=0` builds `platform_none.cpp` and never looks. Unset asks
for it and settles for none, because not everybody has it installed and a build
that stopped would be a worse answer than an editor that draws.

```
make ACCESSKIT_DIR=/opt/accesskit-c
```

AccessKit is written in Rust, so building accesskit-c from source needs a Rust
toolchain -- but this project does not: it links a library, the same as it
links libtorrent. There is no cargo in this build and no Rust in this tree.

### Seeing what a screen reader would be told

`--dump-a11y` prints the tree once the frame has settled, indented, one node
per line. It is the one way to check the description without an accessibility
bus and somebody listening to it, and it is what the tests read.

```
./build/xudu store --import essay.txt --click 400,300 --dump-a11y --profile
```

```
window "Xudu"
  multiline text input "1" [caret 14]
    text run = "hello world\n"
    text run = "this is a second line\n"
    text run = "and a third with words in it\n"
  list "hypertime: every state of this document"
    list item "1" = "current"
focus: 1
```

The description has also been read back off a real accessibility bus -- an
`at-spi-bus-launcher`, a registry and `org.a11y.Status.IsEnabled` set, with the
tree walked from the registry root by D-Bus. The application registers, the
window and its children are reachable from outside the process, and the
document's whole text comes back through the AT-SPI `Text` interface. The
Windows and macOS paths are written against the same C API and have not been
run against a real screen reader here -- CI proves each builds, links against
AccessKit and starts (Windows also runs `--dump-a11y` against a software
renderer; see the workflow), but neither has been checked against VoiceOver or
Narrator themselves.

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

### What SDL provides for dialogs

Worth stating plainly, because it decides what is drawn here and what is not.
SDL is a windowing, input and audio library, not a widget toolkit. For asking a
person something it offers exactly two things:

- **Message boxes.** `SDL_ShowMessageBox` and `SDL_ShowSimpleMessageBox`: a
  title, a body, and up to a handful of buttons, drawn by the platform. In both
  majors, on every platform SDL supports.
- **File dialogs**, in SDL3 only (`SDL_ShowOpenFileDialog` and its siblings).
  Nothing in this program opens a file that way yet.

There is no text field, no list, no drop-down, no password entry and no widget
of any kind -- `SDL_ShowInputBox` does not exist in either major. So the
publish dialog cannot be an SDL dialog: it asks nine questions, one of them a
drop-down of keys and one a masked passphrase, and none of those have a
platform control behind them. It is `gleditor::Form`, drawn with the same
`Canvas` as the hypertime map and the notification overlay, in the window.

What SDL does have, this program uses:

- **`sdl::showMessageBox()`** wraps both majors' message box, and everything
  that has one thing to say and nothing to fill in goes through it. It blocks,
  and it must be called from the thread that created the window -- while the
  things worth saying happen on the render thread, where the store and the
  documents are. So `AppState::showDialog()` queues one from any thread and the
  event loop shows it, which is the same shape as every other cross-thread hand-
  off here.
- **`SDL_StartTextInput`**, so a modal can be typed into even in a program that
  has text input off. A question nobody can answer is not a question.
- **`sdl::setTextInputArea()`** -- `SDL_SetTextInputArea` in SDL3,
  `SDL_SetTextInputRect` in SDL2 -- so the platform knows where the answer will
  appear. That is what puts an input method's candidate window beside the field
  being typed into rather than over it, and what stops a phone's on-screen
  keyboard covering it. `ModalInput::textArea()` is the seam; `Form` works the
  rectangle out while drawing, since that is where the panel's geometry is
  decided, and the event loop tells SDL only when it moves.

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
  - accesskit-c (optional; what reports the user interface to screen readers.
    See "Accessibility" above)
- Testing: GoogleTest + GoogleMock
- Vendored/third-party: `thirdparty/argparse`, `thirdparty/Choreograph`,
  `thirdparty/cosmopolitan` toolchain support (optional)

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
  libgtest-dev libgmock-dev \
  libtorrent-rasterbar-dev gnupg
```

`libtorrent-rasterbar-dev` is required, not optional: it is where the ed25519
that signs a publisher's name comes from as well as the swarm, so a build
without it cannot publish at all. `gnupg` is wanted at run time rather than at
build time -- publishing signs an authorship record with it, and reading one
checks the signature with it.

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
- For coverage (`make profile`), install `llvm-profdata` and `llvm-cov`
  (e.g., `llvm-14-tools` or similar on Ubuntu).
- Headless testing works with Mesa's software drivers: `llvmpipe` for OpenGL and
  OpenGL ES, `lavapipe` for Vulkan. Both come from `mesa-vulkan-drivers` and the
  usual Mesa packages.

## Build

Common targets (see `Makefile`):

- Build everything (library, both programs, tests, compile commands):
  - `make` → builds `build/libgleditor.so.0`, `build/gleditor`, `build/xudu`,
    `build/gleditor_test`, `build/xudu_test` and `build/compile_commands.json`
- Build the library only:
  - `make lib` → `build/libgleditor.so.0`, plus the `build/libgleditor.so` linker name
- Build the xanadoc editor only:
  - `make xudu` → `build/xudu`
- Build with the Vulkan backend:
  - `make GLEDITOR_ENABLE_VULKAN=1` → also compiles `assets/shaders/vulkan/*.spv`
- Build against a particular SDL:
  - `make GLEDITOR_SDL=2` or `make GLEDITOR_SDL=3`
- Compile the SPIR-V modules only:
  - `make shaders` (needs `glslangValidator`)
- Build the app only:
  - `make gleditor` → `build/gleditor`
- Build tests only:
  - `make gleditor_test` → `build/gleditor_test`
- Clean:
  - `make clean` → removes `build/` artifacts

Compile commands database (for clangd, etc.):

- Generated automatically by `make` at `build/compile_commands.json`.
- You can also run: `make build/compile_commands.json`.

Optional Make variables:

- `DEBUG=1` enables debug flags and sanitizer flag sets.
- `GLEDITOR_ENABLE_VULKAN=1` compiles the Vulkan backend.
- `GLEDITOR_SDL=2` or `GLEDITOR_SDL=3` picks the SDL major version; unset means
  SDL3 when pkg-config finds it, SDL2 otherwise.
- `GLEDITOR_ENABLE_A11Y=1` requires AccessKit and fails the build without it;
  `=0` builds without reporting anything to assistive technologies; unset uses
  it when it can be found. `ACCESSKIT_DIR` names an unpacked accesskit-c
  release. See "Accessibility" above.
- `STATIC=--static` attempts static linking for libs resolved via pkg-config.

Objects are rebuilt when the compile flags change, so toggling either of the
first two does not leave a binary that disagrees with what was asked for.

`clang++` is the default compiler, but only when nothing else asked for one:
`CXX=g++ make` and `make CXX=g++` are both honoured. The `-std` flag is chosen
by asking the compiler what it takes, because `-std=c++2c` is clang's and
gcc 14's spelling and gcc 13 rejects it.

## Installing and packaging

```
make install DESTDIR=/tmp/stage prefix=/usr
```

which lays out the binary, the shaders and SPIR-V, the icon, a desktop entry,
AppStream metainfo and a man page. `make dist` produces the release tarball the
distribution packages build from, with both submodules unpacked into it --
they are vendored dependencies, and `git archive` alone produces a tree that
will not compile.

**The data files are found rather than assumed.** `gleditor::assetDir()` takes
the first of these that exists: `$GLEDITOR_ASSET_DIR`; the executable's own
directory, then the `share/gleditor` beside it; a compiled-in
`GLEDITOR_DATADIR`; and finally `./assets`. The executable's path comes from
SDL rather than `/proc/self/exe`, which is what makes the same search work on
Windows. `GLEDITOR_DATADIR` is compiled in by `make install` and by nothing
else, so a build tree cannot quietly read the assets of an older installed
copy, and `./assets` last is what keeps `make run` working from the source
tree.

Definitions live under `packaging/`, one directory per format, all of them
building the same tarball with the same `install` target:

| Target          | Definition                                | SDL | Compiler     |
| --------------- | ----------------------------------------- | --- | ------------ |
| Debian / Ubuntu | `packaging/debian/`                       | 2   | g++          |
| Fedora          | `packaging/fedora/gleditor.spec`          | 3   | g++          |
| Arch            | `packaging/arch/PKGBUILD`                 | 3   | gcc          |
| Nix             | `flake.nix`, `packaging/nix/gleditor.nix` | 3   | stdenv       |
| Windows         | `packaging/windows/build-msys2.sh`        | 3   | MSYS2 UCRT64 |
| macOS           | `packaging/macos/gleditor.rb`             | 3   | clang        |

Android is not in this table: an APK is not a tarball an `install` target can
produce, and none of pango, cairo, glib or SDL are things a distribution
installs there for the Makefile to find with pkg-config -- vcpkg cross-builds
that whole stack from source instead, against the Android NDK. It is `gleditor`
only for now, not `xudu`, and lives under `packaging/android/` with its own
[README](packaging/android/README.md) rather than in the table above.

The Vulkan backend is enabled on all six targets, so `glslang` is a build
dependency everywhere and the Vulkan loader is a weak runtime one -- it is one
backend of three, and the other two work without it. On macOS the driver
behind the loader is MoltenVK, a Vulkan implementation on top of Metal rather
than a native one, and Homebrew's `molten-vk` and `vulkan-loader` formulae are
what `packaging/macos/gleditor.rb` builds against. A driver reached that way
is what Vulkan calls a "portability" driver, and its loader excludes one from
`vkEnumeratePhysicalDevices` unless the instance opts in and, separately,
requires `VK_KHR_portability_subset` be enabled on the device once the driver
reports it; `DeviceVK` does both, checked per loader and per device rather
than per platform, so the same code path runs unchanged on the five platforms
whose loader never advertises either. What packaging cannot yet do is prove
this against a live MoltenVK device: `packaging.yml`'s macOS job builds,
installs and runs `--version` against the result, the same as it has always
done for OpenGL, because whether a GitHub-hosted runner's WindowServer hands
SDL a real GPU context at all -- for either backend -- is a standing unknown
this project's CI has never resolved, unlike the software rasterisers
(`lavapipe`/`llvmpipe`) the other five platforms' CI jobs render through.
AccessKit, by contrast, is wired in on macOS the same as on Windows:
`packaging/macos/gleditor.rb` fetches accesskit-c's macOS build as a resource
and builds with `GLEDITOR_ENABLE_A11Y=1`, and the macOS CI job proves the link
the same way the Windows one does -- that reporting does not depend on a live
GPU context the way rendering does. macOS also builds against a vendored copy
of `GL/glcorearb.h` (`thirdparty/opengl-registry/`) rather than a system one:
it has no `gl.pc`, and Apple's own OpenGL.framework headers were never going
to grow the Khronos registry layout the other four platforms get from their
GL loader's dev package or, on Windows, from MinGW itself.

Debian gets SDL2 because SDL3 is not in the archive and a package cannot depend
on whichever version the build happened to find; everything else gets SDL3,
stated rather than probed for the same reason.

Windows is an MSYS2/MinGW build producing a zip rather than an MSVC installer.
The dependencies are the GTK stack -- pangomm, cairomm, glibmm -- which MSYS2
packages and vcpkg largely does not. The zip carries every non-system DLL,
found by walking `ldd` until it settles, and puts the assets *beside* the
executable, which is the first place the search looks; unzip it anywhere and it
finds its own data.

`gleditor --print-asset-dir` reports where that search landed and exits before
opening a window, which is how the answer can be checked on a machine whose
driver cannot give the program a context -- the Nix job does exactly that,
since a store binary links nixpkgs' libglvnd and cannot load a non-NixOS
runner's Mesa drivers.

`.github/workflows/packaging.yml` builds each of them, and then installs the
result and runs it from a directory with no relation to the source tree.
That second half is the point: until the data files were found at run time,
every one of these packages would have built, installed, and then failed to
start, with nothing in a build log to say so. `packaging/smoke-test.sh` is
what each job runs, and it is runnable by hand against any installed copy.

## Run

- After building, run either:
  - `make run` (runs `build/gleditor`), or
  - `./build/gleditor [options] [files...]`, or
  - `./build/xudu [options] [store]` -- see [xudu](#xudu-a-xanadoc-editor)

Both programs take the options below; `xudu` adds a few of its own, which
`xudu --help` lists. Neither needs `LD_LIBRARY_PATH` to find the library out of
the build tree: the run path records the binary's own directory, and the
library is built beside it. An installed copy relies on the dynamic linker,
which finds a library under `/usr` or `/usr/local` on its own; the install
directory is added to the run path only when it is somewhere the linker would
not look, since a runpath into a standard directory is at best noise and
Fedora's packaging checks reject one outright.

Command-line options (registered by `gleditor::addCommonArguments()` in
`src/app.cpp`):

- `--font <name>` default: `"Monospace 16"`

- `--backend <name>` `opengl` (default), `opengles` or `vulkan`

- `--profile` open any provided files and then exit (useful for profiling)

- `--screenshot <path>` write the first settled frame to `<path>` as a binary PPM

- `--pick X,Y` print the picking tag at that pixel once the document has
  settled, then exit

- `--toast [severity:]text` show a notification over the document; severity is
  `info`, `warning` (the default) or `error`. Driver diagnostics raise these on
  their own; this is how the overlay is exercised on demand and compared
  between backends

- `--click X,Y` click there, moving the caret, and print where it
  landed; repeatable

- `--type TEXT` insert TEXT at the caret; repeatable

- `--select START,END` select that document byte range, as a drag would;
  repeatable

  These four -- `--pick`, `--click`, `--type` and `--select` -- are carried out
  in the order they appear on the command line, each waiting for the one before
  it to finish.

- `--fov DEGREES` initial vertical field of view; widening it is how a
  headless run sees many pages at once, and small ones

- `--no-cull` draw every page of every document, including the ones
  entirely outside the view. The frame must come out identical

- `--coarse-below N` draw a page as one solid bar per line once one layout
  pixel of it covers fewer than N screen pixels; `0` always draws glyphs

- `--benchmark N` draw N frames once the document has settled, report how
  long they took, and exit

- `--strict-diagnostics` treat a driver error as fatal instead of showing it
  as a notification

- `--no-present` draw frames without showing them, for capturing one on a
  machine that can give a context but cannot put it on a screen. The capture is
  unaffected -- drawing goes to an offscreen target either way, and presenting
  only copies it to the window. OpenGL and OpenGL ES accept it; Vulkan refuses,
  because every frame acquires a swapchain image and presenting is what hands
  it back, so skipping it would block rather than capture

- `files...` one or more input files to open at startup

Most of these exist to drive the editor without a person at the keyboard, so
`--help` lists only the everyday ones -- `--font`, `--fov`, `--backend` and
`--coarse-below`. `--help-all` lists everything, at length and in its own
section. Hiding is only about the listing: every switch is accepted either way,
so a script written against one build still runs on another whose help does not
mention what it passes.

Help:

- `./build/gleditor --help` the everyday switches
- `./build/gleditor --help-all` every switch, described in full

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

Note: the `/run` targets set recommended `ASAN_OPTIONS`, `TSAN_OPTIONS`, or
`MSAN_OPTIONS` and then execute the binary.

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

| Key    | Action                                                         |
| ------ | -------------------------------------------------------------- |
| n      | Create a new page                                              |
| w      | Close the most recently opened document                        |
| ctrl-s | Save the most recently opened document back to disk            |
| r      | Reset view back to start                                       |
| q      | Quit the application                                           |
| g      | Increment fov by 1 (max 360); use Shift+g to decrement (min 1) |

### Animation

Documents do not appear and disappear between one frame and the next.
Choreograph drives a single timeline, stepped once per frame in real time so a
fade lasts as long on a software rasteriser as on a GPU. Opening a document
eases it back into its place in the row while it becomes opaque; closing one
reverses that, and the documents after it slide up into the space. Retargeting
an animation that is already running continues from wherever it had got to,
which is what stops a second close from making the survivors jump.

The alpha is one number per draw rather than one per glyph, so fading a
document does not mean rewriting its vertex buffer every frame. It reaches the
shader as a push constant on Vulkan and a plain uniform on OpenGL and OpenGL
ES, and only the colour attachment blends -- the picking attachment is an
integer target, which is both meaningless to blend and forbidden to.

An animation counts as unfinished work, so `--screenshot` and `--profile` still
wait for the settled frame: a capture shows the finished document, never the
middle of a fade. Notifications are the exception. They fade at both ends too,
but from their own clock rather than from the timeline, because a toast lives
for eight seconds and no capture should wait that long.

## Build speed

`ccache` is used when it is installed, unless `GLEDITOR_NO_CCACHE=1` or an
explicit `CXX` says otherwise. It matters most for one thing: turning
`GLEDITOR_ENABLE_VULKAN` on or off changes the flags every object is compiled
with, so a flip rebuilds all seventy of them. On four cores that is around a
hundred seconds each way; with ccache the second flip is a second and a half,
because the objects for the configuration being returned to are still there.

A real code change still costs what it costs -- around nine seconds for a
change to `doc.cpp` and the relinking that follows. What ccache removes is
recompiling the sixty-nine files that did not change.

## Tests

- Build and run tests:

  - `make test` → builds and runs `build/gleditor_test` and `build/xudu_test`,
    leaving out the suites that need a peer on another network stack. Those
    skip without one, and a skip in the middle of a run reads as a pass.
  - `make test/all` → the same two binaries with nothing left out. This is
    what the pull request checks run.
  - `make test TEST_FILTER='*'` overrides the exclusion for a one-off, and
    names a single suite the same way: `make test TEST_FILTER='SwarmTest.*'`.

  There are two binaries because there are two things to test. `gleditor_test`
  covers the library and links the shared library the programs link, so a
  symbol that failed to be exported fails the test build rather than going
  unnoticed until something outside this tree tried to link it. `xudu_test`
  covers the xanalogical engine and links no graphics library at all, which is
  the boundary being checked rather than merely asserted: if a rule about
  versions, spans or links ever needed a renderer, that binary would stop
  linking.

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

  - `make profile` → generates `gleditor_test.prof` and `coverage.lcov`
    - Requires `llvm-profdata` and `llvm-cov` on PATH.

## Documentation

- Generate API docs with Doxygen:
  - `make doc`
- Output is written to `docs/` (configured in `Doxyfile`).

## Project structure

- `src/a11y/` the accessibility tree, and the one file that talks to AccessKit
- `src/` the library: document model, glyph cache, SDL wrappers, render loop
- `src/render/` the device abstraction and its backends (`gl/`, `vulkan/`)
- `include/` the library's public headers, under `gleditor/`
- `apps/gleditor/` the plain editor
- `apps/xudu/` the xanadoc editor; `apps/xudu/core/` is its engine, which needs no graphics device
- `assets/shaders/` portable GLSL bodies, plus generated SPIR-V under `vulkan/`
- `tools/` build-time and verification helpers
- `design/` design notes: investigations and the reasoning behind decisions
- `tests/lib/` the library's unit tests (GoogleTest/GoogleMock)
- `tests/xudu/` the xanalogical engine's unit tests
- `thirdparty/` vendored dependencies (argparse, Choreograph, cosmopolitan, etc.)
- `assets/` assets like `logo.png`
- `docs/` Doxygen output directory
- `Makefile` build orchestration
- `Doxyfile` Doxygen configuration
- `LICENSE` project license

## Environment and Make variables

- `DEBUG=1` enable debug flags and sanitizer options in builds.
- `GLEDITOR_ENABLE_VULKAN=1` compile the Vulkan backend and its SPIR-V.
- `GLEDITOR_SDL=2` / `GLEDITOR_SDL=3` build against that SDL major version.
- `GLEDITOR_RECORD_THREADS=N` threads the Vulkan backend records a frame with,
  and an instruction rather than a ceiling: setting it takes the choice away
  from the device, so `1` records in one piece and anything more always splits.
  That is what lets the same binary be run both ways, which is how the two
  paths get timed and compared at all. Ignored by the other backends, which
  cannot split.
- `GLEDITOR_ATLAS_SIZE=N` side length the glyph atlas opens at, before it grows
  to fit. Exists because nothing else provokes growth -- a whole Bible packs
  into the default 512 -- so a small value is how the grown atlas gets rendered
  and compared against the ungrown one. `tools/compare-backends.sh` sets it.
- `STATIC=--static` attempt static linking (where supported by your system/libs).
- `ASAN_OPTIONS`, `TSAN_OPTIONS`, `MSAN_OPTIONS` fine-tune sanitizer behavior
  (the `/run` targets set sensible defaults).
- LandlockMake: if `LANDLOCKMAKE_VERSION` is set, the Makefile enables a
  sandbox for builds (optional/developer setup).

## License

This project is licensed under the GPL-3.0. See `LICENSE` for details.\
TODO: Confirm whether the intent is GPL-3.0-only or GPL-3.0-or-later.

## TODOs / Notes

- macOS's Vulkan backend has not been proven against a live MoltenVK device --
  CI only checks that the package builds, installs and runs `--version`; see
  the packaging table above.
- The app resolves `assets/glsl` and `logo.png` relative to the working
  directory, so it must be started from the repository root (`make run` does).
