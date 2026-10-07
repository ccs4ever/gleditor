# UI text fitting: batch 6

Batch 6 migrates the pouch drawer, store object manager and embedded clasp forge to retained,
scaled, fitted UI presentation. Rendering and input adapters remain C++ because they bridge the
shared GPU widgets and raw pointer events; store, pouch and hyperlink operations remain in the
application and xanalogical engine.

## Panels and interaction

The store manager uses bounded right-hand dock geometry, full object names and synchronized,
virtualized open/close lists. Its actions identify structure births rather than current row indices.
Pointer and accessibility requests queue domain actions for the render thread, where the current
object and open state are checked again. Drawing, focus, picking and accessibility share widget
rectangles. Wheel and page keys reach objects beyond the initial viewport.

The pouch drawer retains its header, visible partitions and item actions. Previews use fitted
Unicode text with complete accessibility labels. Scrolling and zone paging keep overflow actions
reachable; hidden partitions have no drop target. Pouch actions retain full item identities and zone
identities. The forge retains its two span benches and relation controls while preserving
many-to-many link creation, type and prominence. Delayed actions validate the selected bench state
and opening epoch. Sliding, drag guides and forge bursts remain animated.

The pouch drawer also supports horizontal resizing at the inner divider. Its preferred logical width
is a live UI setting, bounded by the safe area. Completed resize gestures persist the width;
interrupted gestures cancel their preview. Pointer capture follows motion and release outside the
divider. Focused Left and Right keys resize the drawer, and activation widens it by one step.

The drawer reserves complete preview rows. When large configured fonts exceed the smallest
viewport's vertical budget, the header uses one row and the forge combines each slot heading and
source kind into a caption. Complete source metadata remains accessible. Padding contracts before
text does; resizing respects the measured minimum width for the controls. Clipped virtual-list edge
rows with no room for a text line have no focus, picking or accessibility target when the viewport
can fit a complete line. Tiny choice controls retain selection bounds.

The UI system xanadoc now seeds pouchPanel.widthPx, pouchPanel.maxWidthShare,
pouchPanel.maxHeightShare, storePanel.widthPx, storePanel.maxWidthShare and
storePanel.maxHeightShare. Preferred widths default to 320 and 340 logical pixels, width shares to
0.9 and height shares to 1. Native schema and setting notes describe units and valid ranges. Xuzz
applies these settings at launch and on live UI changes; empty font overrides follow the shared
typography roles.

## Verification

The new panel tests cross three viewport sizes, four font scales and four font families, including
wide fallback text. They cover containment, full accessibility labels, retained shaping and uploads,
scrolling, stable asynchronous actions, modal isolation, and pointer resizing. Actual-application
scripts create store objects and forge a clasp by the labels of the drawn controls.

The shared automation option --click-label resolves a unique visible actionable accessibility label
and uses its drawn bounds for an ordinary asynchronous GPU pick. Missing and ambiguous labels fail
the run. Parser coverage checks repeatable ordering and full label preservation.

The final full build, format-check and lint passed. The main test suites passed 733 library, 1,255
engine, 59 Xuzz and 166 ZigZag tests. All 12 network swarm tests passed. The final publication
network journey initially failed its downloaded seed file check; the isolated publication test
passed on retry (one test, 58.5 seconds). The affected publication implementation is unchanged in
this batch; the first failure remains an intermittent validation result, rather than a proven UI
regression.

Focused panel checks passed 24 tests, including actual glyph containment at 48 viewport/font-family/
font-scale combinations and four minimum-width cases. Sixteen shared form/widget checks passed.
Real-application journeys verify creation, forging, modal isolation, saved drawer width and
unchanged visited document content. Strict backend comparison passed its rendering, picking and
atlas checks. Optional application E2E inside that comparison script was disabled; application E2E
ran in the engine suite above. The comparison sample did not contain enough draws to split threaded
recording.

A temporary no-op-device probe measured 1,000 warm frames per scenario. The migrated forge, store
manager and pouch drawer performed zero C++ allocations, text layouts, HarfBuzz calls or buffer
uploads in those frames. The forge CPU p50/p95 fell from 51.4/58.3 microseconds to 0.25/0.25
microseconds. Its previous implementation allocated 164 objects and uploaded 5,064 bytes per frame.
This comparison uses equivalent domain content and geometry, not pixel-identical layouts. The store
manager measured 0.19/0.19 microseconds. Pouch p50/p95 ranged from 1.05/1.06 to 1.47/1.48
microseconds with one through four populated zones; only the zones fitting the viewport were drawn.
Software GPU timings include synchronization and are recorded separately below.

Software rendering p50/p95 milliseconds (100 frames after 10 warm-up frames):

| Panel         | OpenGL       | OpenGL ES   | Vulkan      |
| ------------- | ------------ | ----------- | ----------- |
| Pouch drawer  | 8.87 / 10.96 | 4.63 / 5.06 | 8.50 / 9.11 |
| Store manager | 9.40 / 11.24 | 5.03 / 5.43 | 8.92 / 9.46 |
| Clasp forge   | 9.37 / 11.63 | 4.70 / 5.33 | 8.42 / 9.13 |

All measured warm GPU scenarios had zero layout and HarfBuzz calls. Actual panel captures were
pixel-identical across OpenGL, OpenGL ES and Vulkan. Default and widened pouch captures were
inspected along with store and forge captures: widening from 320 to 700 logical pixels gives
previews and bench labels more room. Glyphs remain sharp with flat baselines and fitted ellipses.
The captures and raw logs are temporary verification artifacts under
`/tmp/ui-text-fit-batch6-captures` and `/tmp/ui-text-fit-batch6-*.log`.

The initial combined Vulkan probe exhausted its fixed 64-pipeline diagnostic pool after constructing
all earlier batches' scenarios. Running these three panel scenarios in an isolated process passed;
no rendering implementation was changed to accommodate the temporary probe.
