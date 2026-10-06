# Canvas boxed text and clipping: batch 2

Batches 0 and 1 were committed as `91e23ff` before starting this batch in the same worktree.

## API and geometry

`Canvas::addText(state, ui::Rect, text, colour, background, TextFit, cache, decorations)` returns
`BoxedText`, containing the fitted text and its effective draw box. Boxes use logical Canvas pixels,
Y upwards, and text starts at their top left. Positive fitting dimensions can narrow the supplied
box while retaining its top edge. Zero fitting dimensions use the box dimensions; an empty box draws
nothing rather than becoming an unbounded fit.

The overload accepting `FittedText` draws retained geometry without layout. Cache hits also perform
no layout. Unseen clusters may still need atlas rasterization. Precomputed fitting must use the
Canvas font, and the returned effective box should be reused with its fitted result.

```cpp
auto label = canvas.addText(state, gleditor::ui::Rect{20, 30, 240, 32},
                           title, foreground, background, {}, &shapingCache);
// A later rebuild can reuse this pair without shaping.
canvas.addText(state, label.box, label.fitted, foreground, background);
```

`pushClip(Rect)` intersects with the enclosing clip. `popClip()` restores the parent; underflow is a
logic error. `clear()` resets the stack. Invalid or negative rectangles are refused, while zero
extent represents an empty clip. Clips affect rectangles, the bounding rectangles of lines, text and
images in local Canvas space before applying the draw transform. A boxed text draw restores the
parent clip on exit.

Glyph and solid quads keep the existing packed document instance format and shaders. Glyph crops
advance integer atlas origins along with their cropped geometry. Fractional clip boundaries round
inward to whole glyph texels, potentially leaving less than one logical pixel of extra inset. Image
crops retain floating geometry and interpolate all four UV edges. Picking identities survive both
crop paths. No backend or shader change is required.

Clip fitting now retains the intersecting edge cluster for Canvas to crop. `visibleBytes` counts the
fully retained logical prefix; the partial cluster still carries its original complete source span.
RTL prefixes position the partial cluster on the visual left. Legacy styled/media ellipsis selects
only the fully retained prefix before adding its marker, preserving its earlier behavior.

`setTextWidthLimit()` remains as a documented compatibility API for callers awaiting later widget
migrations. Its single-line ellipsis path now also clips physical glyph ink to that width. Existing
form, switcher, toast and ZigZag label callers retain their single-line layout behavior; this batch
does not redesign those widgets or their responsive sizing.

## Verification

The full build and test target pass: 595 library, 1,249 Xanadu, 57 Xuzz, 119 ZigZag tests and both
network suites (12 and 1 tests). The final library run passes 596 tests after adding the narrower
fit-constraint physical clipping regression; a final focused run passes 49 fitting/cache/Canvas
tests after pairing retained results with their effective boxes.

New checks cover nested intersection, stack restoration and reset, invalid and empty clips,
four-edge image UV interpolation, fractional glyph atlas crops, picking identity, partial-cluster
Clip drawing, narrower fitting constraints, single-line legacy width limits, and zero shaping for
warm cached and retained draws. Lint and changed-file clang-format 19 checks pass. The preexisting
repository-wide formatting failures remain outside this batch.

The final `compare-backends.sh` run passes document/overlay pixels, picking, culling, atlas growth,
coarse text, zoom and Vulkan recording comparisons. An earlier run during application tests failed
the Vulkan culling comparison in document glyph pixels; the repeat after those tests completed
passes. The cause of that transient capture difference has not been established. The script's
additional per-backend application E2E section remains disabled because of the unresolved batch 0
Vulkan drag timeout; application E2E runs in the full test target.

The workload tool adds boxed ellipsis rebuild/retained scenes and a boxed Clip scene. Every changed
pixel is checked against its label's allocated box, independently of logical fit metrics. All three
backends pass these physical containment checks; retained boxed frames perform zero shaping. All six
workload captures match exactly between GL, GLES and Vulkan, and the boxed captures were inspected.
Raw captures and TSV are in `build/ui-text-fit-batch2-results/`, with document parity captures in
`build/ui-text-fit-batch2-parity/`. These 30-frame runs are verification smokes rather than a new
performance baseline.
