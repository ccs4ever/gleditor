# Optimized spatial UI performance acceptance plan

## Current evidence and status

Optimized performance remains open. Existing timings are useful controls and regression clues, but
none is an optimized acceptance result.

The seventh-increment comparison explicitly built with `O0 -g0` at source base
`67435ffdfbddbd536a7d99ea43c465ccbcdd432a`. Its unchanged
`LayoutfiladeBenchmarkTest.IncrementalResizeLatency` uses 10,000 synthetic entries and 10,000
`updateEntry()` calls, then compares the mean to a fixed 1,000 ns/update test assertion. It measured
3,392.3 ns/update in the full run, which failed; isolated candidate and frozen old-library runs
recorded 684.2 and 727.3 ns/update. The fourth-increment record preserves two older O0 results:
4,625 ns/update in an earlier run and 854.6 ns/update later. The historical measurements vary
substantially, and the test measures a small in-memory Layoutfilade update rather than
`Doc::reflowFrom()`. Keep the 1,000 ns assertion as its existing test contract, but do not reuse it
as an optimized UI budget.

The default GNU Make flags in the current build are `-O3 -g`; the README says the logger mode is not
an optimized/debug distinction. The `O0 -g0` logs are from the explicit previous performance
investigation. The current `engine-baseline-comparison.json` is a GoogleTest count/time comparison,
not a timing run for layout or atlas allocation. A 799-case O3 library-suite pass would establish
correctness/build coverage only; it cannot close this gate.

For atlas correctness, the old control is the pre-alignment implementation at
`67435ffdfbddbd536a7d99ea43c465ccbcdd432a`. The implementation commit `fbd4169c` changes
`src/glyphcache/cache.cpp` and adds its regression test, without changing public headers. The frozen
control library hash recorded in the seventh reproduction is
`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`. At O0, the controlled
`GlyphCacheMipOrderTest` produced 80 differing pixels with max channel delta 45 on baseline across
the actual backends and exact equality on candidate; this is a correctness result, not a speed or
memory measurement. The current fixture holds the glyph, font, color, geometry, transform, and draw
constant while preceding `A` with an unused `m` request. Preserve this pixel assertion unchanged in
every optimized run.

The seventh backend comparison's `GLEDITOR_ATLAS_SIZE=64` path already records atlas growth and
placed texels, and its grown-versus-ungrown captures match. Those checks establish rendering parity
under growth but do not quantify the additional padded allocation or its runtime cost.
`glyphPadding` is eight pixels; the changed allocator rounds each padded glyph width and height up
to an eight-texel boundary. Its added coverage area is therefore the difference between the aligned
rectangle and the old `(inkWidth + 16) × (inkHeight + 16)` rectangle; each axis adds 0–7 trailing
texels.

## Comparison protocol

Build baseline and candidate in separate clean worktrees and build directories. Use the same
compiler binary, dependency versions, backend features, Make variables, linker flags, font files,
environment, and benchmark fixture. Save source and binary hashes, `compile_commands.json` or full
compile/link commands, CPU model/governor, OS, graphics driver/ICD, and display driver. Run serially
after the active full test load ends, with fresh isolated XDG roots. For renderer or device
measurements set the repository's six headless/XDG variables and keep the same software GPU/ICD; do
not compare X11/Xvfb with offscreen captures or GL with Vulkan timings as if they were one
condition.

The glyph-cache control should build the exact pre-fix source at `67435ff...` and the candidate
source containing the mip-grid alignment fix. The changed cache implementation does not alter public
types or function signatures, so each test binary can be built from the same test source and headers
against its own clean library. Do not reuse objects from the other build tree, and do not use a
library with headers from a different revision. First run the unchanged mip-order pixel test to
establish the expected baseline failure and candidate pass. Compare old and new source only for the
performance counters below; keep correctness assertions as separate gates.

For the edit-layout comparison, use a source control only if it implements the same workload and its
correctness output agrees with the candidate. The immediately preceding revision `975d8b1b` predates
the sparse page-index repair at `67435ff`; if that repair affects the test's page entries or
rendered text, it is not a valid performance control. In that case record the optimized current
implementation as a new measured baseline and do not claim a before/ after improvement. The
synthetic Layoutfilade test can be reported separately with the frozen baseline library, but it
cannot stand in for edit-triggered reflow.

## Measure actual changed-region reflow

`Doc::reflowFrom()` rebuilds shaped-page metadata from the edited page until an old page boundary is
found again, then updates built `Page` objects. Existing `ui-text-baseline` covers label rebuilding
and retained geometry; it does not edit a multipage document. `layout-latency-probe` compares
first-page loading against a bounded slice; it also is not edit reflow. Existing `DocGapTest` cases
are correctness tests, not timing harnesses.

Use an opt-in, non-gating harness on an actual `Doc` and renderer path, reusing the existing editing
and page construction code. Time from an edit request through the renderer-thread reflow completion
rather than timing only the asynchronous `insert()` call. Separately capture CPU time in metadata
shaping, page object rebuild, and wait/device work so scheduling does not masquerade as layout cost.
Record:

- edit byte offset and edit kind/size;
- total document bytes, prior page count, pages whose metadata was reshaped, built page objects
  replaced, and the old boundary at which pagination resynchronized;
- elapsed wall time and thread CPU time, plus p50/p95 over repeated runs;
- font, shaping-cache warm/cold state, and whether glyph cache misses or atlas growth occurred.

Use the same deterministic multipage text in baseline and candidate. Include insertion and deletion
at an early, middle, and late page; a same-length replacement whose page boundary re-aligns quickly;
and a length-changing edit that shifts page boundaries farther into the document. Include ordinary
prose and a Unicode/mixed-script case, with fixed font packages. Validate the final page text, byte
ranges, caret geometry, and page-boundary index before using any timing. Report changed work as a
function of actual touched pages; do not compare a one-page edit to full-document layout without
counting the work each performed. Keep cold-start and warm-cache results separate.

No threshold should be invented from one run. Collect enough independent serial samples to show the
distribution and host noise, and compare only correctness-equivalent workloads. Any eventual
acceptance limit needs an owner-approved product latency target and a pinned environment; this plan
sets neither.

## Measure glyph padding and atlas cost

Use the same renderer workload on the baseline and aligned candidate, with fresh caches and
identical glyph request sequences. Include the current single-glyph mip-order control, the existing
`quick_brown_fox.txt` backend scenario, the multilingual `tests/samples/ui/long-labels.tsv`
workload, and a large real text sample that exercises atlas growth (`tools/compare-backends.sh`
already has a small-atlas growth path). Run each workload at ordinary and minified scales; preserve
exact image comparison for the mip-order test and the existing grown/ungrown parity check.

Count unique raster allocations, sum old padded area, sum aligned area, and report padding
bytes/texels both per glyph and total. Reconcile this with the existing `GlyphCache::reallocate()`
diagnostics (placed rectangle count, texels, largest rectangle, palette count, old/new dimensions
and layers). Record each growth event and time spent waiting idle, allocating the new texture, and
replaying placements. Estimate R8 texture storage from every mip level for the actual dimensions and
layers, and distinguish that reserved texture capacity from occupied padded rectangles. Repeat one
cold-cache load and warm-cache draw; report p50/p95 and GPU/CPU times separately. If existing
diagnostics cannot expose per-allocation aligned versus prior area without altering production
behavior, add measurement-only instrumentation to the opt-in harness or a benchmark build, not to
the normal log stream.

Run OpenGL, OpenGL ES, and Vulkan as separate controls when compiled. Keep software renderer/driver
and atlas-size override fixed between sources; the existing `GLEDITOR_ATLAS_SIZE=64` growth
comparison is a stress profile, not a representative default workload. Report atlas growth count,
final allocated bytes, cumulative copied placement bytes, and frame results. Do not infer a memory
or latency budget from the small `A` glyph fixture alone.

## Completion criteria and open work

The O0 historical control is reproduced or explicitly classified as non-reproducible, without
changing its test threshold. The O3 comparison uses clean, hash-pinned builds and reports actual
`Doc::reflowFrom()` work plus aligned-atlas area/growth costs. The old/candidate layout workload
must produce the same text, ranges, caret geometry, and page indices before timing can be compared.
The atlas candidate must retain exact mip-order output and existing backend/growth parity.

This plan does not set numerical performance targets. Acceptance remains open until the product
owner supplies a latency/memory objective or adopts a repeatable baseline-derived regression rule,
and the measured runs meet that rule. No performance run was launched while the full test suite was
active.
