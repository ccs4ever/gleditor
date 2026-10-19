# Native revised-beam prototype validation

The full-KJV prototype now runs in Xuzz with real stored links and an isolated author permascroll.
The first, near-middle and last passages are navigable at reading size with the note companion. A
second example demonstrates one continuous member across a page break. This is partial native
acceptance of the revised proposal; dense recovery and inspection leaders remain required work. The
native scrolling obstruction is resolved for this journey.

## Run it

See [native KJV instructions](ui/prototypes/beam-candidates/NATIVE-KJV.md) for preparation, launch,
ordinary navigation controls and evidence reproduction. The fixture source is the full local
`tests/samples/kjv.txt`, 4,606,954 bytes after removing its three-byte BOM. Fixture preparation uses
the CLI; it does not count as a UI authoring journey.

## Verified native behavior

Current captures and logs are under `build/kjv-fragments-{opengl,opengles,vulkan}-05/evidence/`.
Each contains eight PNG/PPM frames, `run.log` with complete accessibility descriptions, and
`source-integrity.json` with both source stores' before/after file hashes. Runs use SDL offscreen,
dummy audio, isolated XDG directories and a 1800-by-1050 viewport. GL/GLES use
`LIBGL_ALWAYS_SOFTWARE=1`; the Vulkan run uses the available RADV Vega adapter named in its log.

| Scenario                                    | Current evidence and limit                                                                                                                                                                                                                                                       |
| ------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Sparse link, three independent left members | Link 20 retains Genesis 1:1 `[87,153)`, Psalm 63:5 `[2303359,2303477)` and Revelation 22:21 `[4606877,4606953)`. Native default pagination places them on pages 1, 623 and 1261 of 1261. Psalm is near the midpoint, not the exact middle page.                                  |
| Enter and Origin                            | Exact native focus requests match the complete expected sequence, including repeated Genesis after resizing and Origin. Origin does not replace the independently selected Revelation cursor.                                                                                    |
| Continuous member                           | Link 42 retains one left member `[2302771,2303273)`, Psalm 62:12 through Psalm 63:3, across pages 622–623. The panel names one chosen document occurrence and both pages.                                                                                                        |
| Exact page fragments                        | The first page has a true-start cap and open continuation end; the second has an open start and true-end cap. The physical gap, preceding Psalm 62:11 and following Psalm 63:4 remain unmarked. The original Psalm 63:5 member stays separately cyan.                            |
| Preview and scrolling                       | The checker rejects any extra native focus request from selection or wheel scrolling. Earlier independent image comparisons also show unchanged reading and note-prose crops during preview. Private activity-operation counts during preview are not yet independently audited. |
| Start outside viewport                      | The final-page bracket survives after the start fragment leaves the viewport. A separate library regression verifies a built interior page while the start page is actually unbuilt. A native three-page interior-only scene is still missing.                                   |
| Source integrity                            | KJV and note `ops.nodes` and `store.tables` are byte-identical before/after navigation on all three backends. Completed visits use the private activity store. This does not prove restart/branching or platform AT usability.                                                   |
| Bracket material                            | Independent aesthetics review accepts slim same-hue satin brackets and open continuation caps across GL, GLES and Vulkan at the captured scale. This is not exact pixel equality or a cross-driver/hardware matrix.                                                              |

The fresh companion notes distinguish the original three-member sparse link from the separate
one-member continuous link. Both remain real authored links; fragment geometry mints no operations.

## Implementation and tests

Built pages retain their small line table. `Page::textBandFor()` intersects a nonempty document
range with that page and returns its measured vertical band without shaping or allocation. Authored
bracket staging visits only the conservative viewport page range, skips unbuilt pages, and does not
require the occurrence's global start anchor. Media-widget anchors retain their separate rectangle
path. The continuous case uses the same picking identity for its fragments.

- 32 library page-gap/automation tests pass, including page-boundary clipping, an unbuilt-start
  regression and zero shaping calls during repeated band reads.
- All 10 panel/control tests pass, including the font/scale matrix and a compact-spacing regression
  ensuring that additional viewport height does not inflate labels. The compact panel has eight
  native captures on each backend. Independent OpenGL review accepts its spacing and hierarchy; the
  entry state's visible left-range label originally truncated its ending byte number; the
  exact-range follow-up below resolves that finding.
- Grouped navigation and all 22 `LinkLayout` cases pass. The earlier aggregate run had one
  readable-text measurement failure, diagnosed below.
- Formatting and lint gates pass for the page-fragment and compact-panel source changes.

The readable-text detector previously required white pixels beside every glyph, discarding text on
orange highlights while admitting HUD/caret pixels. It reported a paragraph gap of 45 pixels as body
pitch; independent pixel review measured 15–16 pixels. The detector now requires nearby paper
ownership, local contrast and several ink pixels. The same readable-size assertion passes on GL,
GLES and Vulkan without increasing its bound. The aggregate integration suite passes all 36 tests on
each backend. Its image comparison initially failed five lifecycle scenes on GLES: shared XDG
configuration contaminated subsequent runs. Repeating those five scenes with fresh per-backend data,
configuration and cache produces exact GL/GLES equality and Vulkan differences of 0.15–0.93 percent.
The comparison harness now uses this isolation; shell formatting and lint pass. The first full rerun
passes 35/36 GL scenarios but fails to find the selected text in the drag calibration capture. The
exact drag scenario subsequently passes both drop paths; the cause of the initial missing selection
remains unproven. The subsequent full rerun with all builds finished passes all 36 integration
scenarios on GL, GLES and Vulkan and every image comparison within the existing, unchanged
thresholds. Final evidence is under `build/prototype-backend-parity-regions-02/`; the earlier failed
run is retained under `build/prototype-backend-parity-regions/`. The basic threaded comparison uses
too few page draws to exercise workers; the separate 257-draw region test explicitly verifies four
workers. Earlier isolation diagnostics remain under `build/prototype-backend-parity-fragments/` and
`build/parity-lifecycle-isolated-{backend}-01/`.

## Chrome exclusion verified

The generic per-frame `RenderRegion` API separates viewport, writable clip and depth slice. Recorded
batches retain region IDs for worker recording; immediate draws restore their previous region.
GL/GLES use scissor and their profile's depth-range API; Vulkan converts the bottom-up rectangle and
neutral depth slice to its framebuffer and reversed-Z conventions. World pages, caret, images,
beams, slice content and world-anchored feedback use the content region. Screen controls retain the
whole target. The viewport and camera projection remain unchanged.

The complete top band through the floating tab strip is reserved, including whitespace above it.
Chrome changes keep a frame pending until its content clip settles. The native checker uses
`--continuous --chrome` to capture the clipped scroll stop and then an ordinary reverse wheel.
`continuous-readable.png` shows the complete selected “according to his work” and neighboring
context below the strip. No cap is added at the clip edge. Independent reader and aesthetics reviews
accept this scoped fix; the latter inspects all 24 frames. Reading/note crops during preview are
unchanged on all three backends. Direct frame comparisons measure GL/GLES differences of
0.0096–0.0809 percent and Vulkan differences of 0.7051–1.2057 percent against GL (one-level channel
tolerance for Vulkan). These native frames are visually reviewed; they are not a claim of exact
pixel equality.

Twelve region/capability/batch tests pass, including real GL/GLES/Vulkan color and hidden-picking
checks, restoring whole-target drawing, and 257 mixed-region draws. The latter explicitly records on
four Vulkan workers and verifies that a near inset stays in front of later distant draws.
Thirty-eight existing picking, Canvas and Vulkan resource-lifetime regressions pass. Formatting and
lint pass. Native scale variations, minimization and platform accessibility actions remain outside
this evidence.

## Selected-link colour consistency

The selected-link panel no longer contributes a fixed gold background. It contributes a separate
edge emphasis using the same presentation identity, link type and prominence as its bracket and
beam. Uniform RGB darkening keeps the link hue and gives the edge contrast against the authored
fill. A soft inward falloff leaves glyph ink and geometry unchanged. The active chosen occurrence
has the strongest edge; the opposite chosen occurrence and other manifestations are quieter.
`system://ui` exposes `linkPanel.chosenEdgeWidthPx` and `linkPanel.memberEdgeWidthPx` (defaults 2.5
and 1 device pixels). Cell range decorations retain their exact spans and their supplementary card
border now uses the link hue.

The generic shader distinguishes background fills from edge emphasis. Edges consume no overlap
colour band and are prioritized after the editing selection, before semantic fills, when packing the
fixed range table. The normal blue editing selection and genuine shared-address transclusion gold
retain their existing meanings. Text is not lifted: independent UI and UX reviews defer actual Z
displacement until glyphs, caret and picking can move coherently.

The build, shader compilation, formatting, lint and UI-text policy checks pass. Eleven panel tests,
twenty generic picking/region tests and twenty-three system-document tests pass. The new panel test
checks exact byte ranges, the semantic hue, changing active side and clearing emphasis on Dismiss.
All three native `-06` runs pass with source hashes unchanged. Independent UX review accepts all 24
captured frames; selected gold is replaced with the same-hue edge. This evidence is scoped to the
sparse and continuous KJV journeys; dense table exhaustion, mixed typography, partial cell spans and
a real transclusion/selected-link overlap still need separate visual validation.

The broad glyph/backend checks pass. All 36 binary orchestration scenarios pass on each backend, and
all 17 captured integration scenes compare within the existing unchanged thresholds. GLES has
0–0.0158 percent differing pixels against GL; Vulkan has 0.1473–1.8627 percent with the existing
one-level channel tolerance. GL evidence is in `build/prototype-backend-parity-edges-01`; final
GLES/Vulkan evidence is in `build/edge-e2e-{opengles,vulkan}-04/evidence`.

The combined comparison run ended with signal 15 during GLES. An attempted parallel continuation was
invalid because the suite uses fixed working-directory paths despite isolated XDG roots; an initial
isolated-directory attempt lacked relative shader assets. Neither attempt counts as a product
result. Final runs use independent working directories with the required asset and sample links, and
both finish with all 36 tests passing.

## Continuation anchors in page margins

A member that continues across a real page boundary now extends its margin rail to that paper edge.
The preceding page's rail reaches the bottom edge and the next page's rail begins at the top edge,
with open ends and the same link hue and stroke. Actual authored terminals retain their caps at the
text. The physical page gap stays blank. A viewport or chrome cut clips the rail without inventing
an endpoint.

Separate optional paper-edge points drive the rail, its visibility test and overlap-lane extent. The
text band and connector attachment keep their exact original geometry. Paper edges come from the
built page, so there is no duplicated margin size or new spacing constant.

The full build, formatting/lint checks and generic GL/GLES/Vulkan backend comparison pass. All three
native `-07` checkers produce eight frames with six exact focus requests and unchanged source
hashes. Independent UX and aesthetics reviews accept all 24 frames. The focused visual evidence
covers the continuous Psalm passage, true endpoints, ordinary scroll recovery, chrome clipping and
separate sparse members. Dense extended-rail collision and pointer-pick ambiguity remain outside
this small fixture's acceptance.

## Exact endpoint ranges: compact-panel follow-up

The panel now presents each chosen side as a required block: side and member count, place count,
then exact byte boundaries and any partial-coverage flag. The location description follows as
optional context. Range numbers come from the typed occurrence, rather than a display-name string.
When truncation would remove a boundary digit, or put an ellipsis immediately after the complete
boundary, the visible label keeps only the required block. Its complete location description remains
in the accessibility name.

Wrapped required blocks receive height before optional context and decoration. At a height shortage,
reading and origin status share one row; if necessary, metadata and actions use the live theme's
Caption role while exact ranges retain Label. Free-area candidate admission measures wrapped ranges
and the caption fallback, including a conservative padding allowance. This is a finite typography
step, not continuous shrinking.

The full build and formatting, lint and UI-text policy gates pass. All 37 focused tests pass: 12
panel presentation/control tests, 11 engine panel tests and 14 generic widget tests. The
presentation matrix covers 640×480, 1280×800 and 1800×1050 at font scales 1, 1.5 and 2 with
`maxLines=1`, long context, 32-bit-sized boundaries and complete accessible descriptions. The
1280×800 / scale-2 case also checks that a protected reading rectangle remains clear when a suitable
free area exists.

The final native `-09` checkers retain eight captures and six exact focus requests on each of
OpenGL, OpenGL ES and Vulkan, with unchanged source-store hashes. The continuous entry visibly shows
`2302771–2303273` and `0–34` without a trailing ellipsis. Independent UI and UX reviews accept this
exact-range/control scope while retaining the narrow placement finding below. These captures
exercise ordinary keymap navigation and scroll input; they do not prove the complete
pointer/accessibility/restart journey.

The separate `build/kjv-fragments-narrow-08/evidence/entry.png` capture shows all nine controls and
complete byte boundaries at 640×480 with normal type, but the panel obscures reading content when no
sufficient free rectangle exists. The exploratory run was terminated after capture because it
omitted the automation exit flag; its partial accessibility log is not a completed journey
transcript. Native enlarged-type captures, arbitrary live Caption sizes and impossible
minimum-height handling remain unverified. The prepared-scene scale matrix is component evidence,
not a native UX pass.

## Substantial ribbons and visible neighboring links

The neighboring cyan relation was already staged in the continuous-entry scene. Its appearance was
nevertheless inadequate: one filament at `0.14` of a line height, multiplied by an inactive opacity
factor of `0.35`, left a nearly invisible route. New defaults use `beams.connectorWidthOfLine=0.40`
and `beams.inactiveLinkAlpha=0.70`. Both are still live settings in `system://layout`; existing
stored values remain authoritative. The fresh validation configuration exercises the new defaults.
Exact bracket width, endpoint geometry, NURBS routing, hue and stationary satin shading are
unchanged.

The OpenGL continuous-entry cross-section at y=660 in the gutter contains one faint cyan pixel in
`-09`, versus seven cyan pixels in `-10`. The active blue connection remains stronger; the
neighboring cyan connection can now be traced to its own passage. All 45 system-document and
link-layout tests, the full build and formatting/lint/UI-text policy checks pass. The native `-10`
checkers pass all three backends with eight captures, six exact focus requests and unchanged source
hashes each.

In `build/kjv-beam-neighbor-pick/evidence/`, clicking the inactive beam at (900,660) selects link
20, changes its prominence and opens its context while retaining the continuous passage as the
reading position. The panel correctly asks for a member/occurrence rather than inventing a pairing.
The log contains only the initial Genesis and continuous-Psalm focus requests: the click and
subsequent `Alt+Shift+D` dismissal add neither. Dismiss leaves both connections visible. Source
hashes remain unchanged. Independent UI review accepts the ribbon weight and active/inactive
hierarchy. The same pointer/dismiss journey passes OpenGL ES and Vulkan in
`build/kjv-beam-neighbor-pick-{opengles,vulkan}/evidence/`; independent UX review confirms selection
without an added focus transition.

This proves the reported adjacent-link case, rather than every possible route. Offscreen endpoint
culling, dense crossings, public-tier contrast and enlarged/zoomed presentations remain broader
validation work. Native frames retain the panel placement limits described above.

## Docked controls when no floating area fits

The panel now reports failed protected-area placement and required-content fit explicitly. A bottom
dock reserves a real world-content clip instead of painting controls over prose. The default Read
state retains link identity, both member/place summaries and partial-occurrence markers. Controls
exposes exact byte ranges and all nine navigation commands; Read returns to the smaller band. Both
the visible buttons and `Alt+Shift+I` change only presentation. Dismiss remains separate. Full
original labels, including reading/origin context, remain in the accessibility tree.

The dock persists through ordinary scrolling for the same selected link. An actual window resize
reassesses placement. This fixes a native review finding in `-02`: scrolling could previously find
free canvas, silently replace the dock with a floating panel and remove its Read control. The new
bottom reservation delays drawing and pick registration until the prior-frame clip has caught up.
Collapsing releases the band without reframing the camera or adding a completed reader visit.

The scoped native evidence is `build/kjv-panel-dock-{opengl,opengles,vulkan}-04/evidence/`: nine
captures per backend, visible Controls/Read pointer clicks, a keyboard toggle round trip, ordinary
source scrolling, wide/narrow resizing and dismissal. Reading and Controls include accessibility
snapshots. Each run records exactly the two deliberate Genesis focus requests and unchanged hashes
for all four source store files in `source-integrity.json`. Pointer captures wait 200 ms after mode
changes; `-03` retained safe transitional blank/old-clip frames and is not settled composition
evidence. Fourteen panel tests and 23 system-document tests pass, including complete ordinary dock
actions, exact accessible ranges, partial markers, clip/pick settling and retaining the toggle when
a floating location becomes available. Full build, formatting, lint and UI-text policy pass.
`tools/compare-backends.sh` also passes headless with its configured pixel tolerances, including all
36 binary orchestration scenarios on each backend; evidence is in
`build/beam-dock-backend-compare/`. The default small sample does not exercise split threaded
recording.

This is an ordinary 640×480 no-free-area fix, not a pass for arbitrary type enlargement. A
three-times-font device-free case explicitly reports insufficient space and retains Dismiss/Cancel
and full accessible context; it does not provide all nine commands visibly. Native enlarged type,
minimum-size warning usability, platform assistive-technology interaction and mixed slice/HUD
composition remain findings. Expanded Controls can clip the currently selected prose out of the
remaining viewport; Read and ordinary scrolling remain available without changing selection or text
scale. These states do not establish full J16.

The aesthetics expert supports a future weak preference for shallower routes across visible
occurrences, constrained by exact anchors and fixed reading focus. The deferred contract is in
[the alignment note](ui_nurbs_beam_shallow_alignment_deferred.md); the subsequent delegated work
implements an unwired solver, described below.

## Enlarged-type inspector and shallower-angle prerequisite

When expanded dock content cannot fit, explicit Controls now opens a paged inspector in the
available canvas. It uses the configured text size and panel surface. Every page pins the link type,
number and owner, both independent member/place summaries, and Previous/Next, Read and Dismiss.
Pending entry keeps its reason and Cancel visible. Cursor summaries wrap at their measured height.
Exact side byte ranges are atomic blocks; optional location context can be retained in the
accessible description when it cannot fit beside the range. The nine navigation actions remain
reachable across pages.

Previous/Next and default `Alt+Shift+PageUp`/`Alt+Shift+PageDown` change presentation only. They
retain command identities and selection, and append no source operations or reader visits.
`Alt+Shift+I` returns to Read. No panel focus scope is registered: an attempted registration
consumed explicit Enter as button activation and dismissed the link. That integration was removed.
Native Tab/F6 panel access remains a finding; accessibility snapshots and a device-free activation
test do not establish platform assistive-technology activation.

Eighteen panel tests pass, including wrapped large cursor counts, pending status, unchanged
navigation during paging, stable command identities and physical-minimum refusal. Twenty-one
engine/solver tests and 23 system-document tests pass. A full build passes.

Native double-size evidence is in `build/beam-overflow-{opengl,opengles,vulkan}-05/evidence/`:
twelve captures per backend, five Controls pages reached through actual Previous/Next clicks, a
keyboard page round trip, explicit Enter, Read and Dismiss. Each run contains exactly two deliberate
Genesis `[87,153)` focus requests and unchanged hashes for all four source store files. Full
accessible byte ranges, source/page names and all nine commands occur across the snapshots.
Independent UX and aesthetics reviews accept this scope. Optional location context is sometimes
available only in the accessible label. Full-height Controls hides world content through the
reserved clip; Read restores the compact band and existing reading position. These captures use an
isolated prepared font setting, not proof of a user-facing settings-edit journey.

The `-04` paging attempt used the wrong accessible button name and stopped on page one. It is failed
harness evidence, superseded by `-05`. The earlier focus-registration probe also remains failed
evidence. Neither is counted as a native pass.

At a physical minimum, the warning remains a refusal rather than complete endpoint access. It
retains the correct Controls/Read toggle and Dismiss/Cancel paths. Enlarging the window reassesses
placement and returns to Read; explicit Controls can then recover an inspector without changing
selection. Mixed slice/HUD composition is still unverified.

Native minimum-space evidence is in `build/beam-overflow-minimum-opengl-07/evidence/`: five captures
at triple text size, an untruncated **Enlarge window** cue with readable Read/Dismiss, resize from
640×480 to 640×800, and an actual Controls click opening the recovered inspector. The run records
one deliberate Genesis entry and unchanged source hashes. Only the first recovered page is captured;
this is warning/recovery proof, not complete triple-size endpoint access. Independent UI and UX
review accept this minimum-space warning/recovery scope. Earlier `-04`/`-05`/`-06` minimum
diagnostics omitted the automation exit flag and were terminated after capture; they are not
completed-run evidence.

Formatting, lint and UI text policy checks pass. The rendering/picking/overlay portions of
`tools/compare-backends.sh` pass headless offscreen in `build/beam-inspector-backend-compare/`. The
108 broad binary-orchestration scenarios passed in the preceding dock batch and were not repeated
for this inspector-only batch. The small default comparison sample does not exercise split threaded
recording.

The delegated shallower-angle work adds a bounded, deterministic device-free solver with ten passing
tests, including an 18-pixel gutter, shared-counterpart compromise, occurrence fairness, conflicting
endsets, overflow and work-budget refusal. Independent aesthetics and UX review accept this as an
unwired prerequisite. It does not change live beam placement: the renderer needs a matching settled
scaffold snapshot, fixed reader/camera bounds and explicit Arrange/Restore actions before native
acceptance. See [the alignment note](ui_nurbs_beam_shallow_alignment_deferred.md).

## Required next work

1. Extend the inspector validation to mixed slice/HUD chrome, native pending cancellation and
   partial occurrences; retain explicit minimum-space refusal and verify platform accessibility
   activation and native Tab/F6 access.
1. Implement exact conditional wide-page inspection leaders with true picking pass-through and a
   named no-safe-route fallback; retain caret, media and control input.
1. Add named identity recovery at ambiguous crossings and honest margin overflow beyond four lanes.
   Preserve full ordered member/occurrence access when ornament is reduced.
1. Bound and measure total dense work, including discovery, lane assignment, staging and retained
   caches. Grouped descriptor count alone is not a performance result.
1. Complete the backend/scale, failure/cancellation, pointer/keymap/accessibility parity and private
   branching/restart evidence appropriate to the revised prototype.

The two independent reviewers accept the sparse long-jump and continuous-fragment demonstrations.
Their scrolling-chrome finding is resolved for the captured journey. This report does not certify
full J16 or the complete revised proposal.
