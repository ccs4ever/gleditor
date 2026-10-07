# UI text fitting: batch 5

Batch 4 was committed as `4b63aef`. Batch 5 migrates the existing small Xanadu overlays to shared
metrics, fitted text, safe-area layout and retained geometry. Link navigation, document splitting
and payment operations remain in the application and engine layers.

The current tree differs from the original plan. There is no Walks overlay to migrate; the activity
model and its tests remain intact. Link panel and overview are registered in Xuzz. Page-break and
transcopyright components have no application construction or registration sites, so their migration
is tested without introducing a new activation path.

## Presentation and interaction

The link panel consumes the existing complete `PanelLine` and `PanelButton` values. Its bounded
panel keeps left and right member descriptions and navigation commands separate. Wrapped labels and
a button flow use shared widget boxes for drawing, picking and accessibility. The live UI store
provides `linkPanel.maxLines`, `linkPanel.maxWidthShare` and `linkPanel.maxHeightShare`, including
schema defaults, native schema prose and user notes. Empty `linkPanel.font` follows the label role;
explicit legacy overrides follow display and font scales.

Page-break controls retain the paragraph-discovery semantics, cache newline boundaries and present a
fitted button near the projected gap. Delayed actions carry the original view/version/offset and are
validated on the render thread. Hovering anywhere inside the scaled button retains its original
paragraph target. Payment badges retain domain snapshots, cached anchors and fitted labels, with a
separate canvas for unlock blooms. Captured IDs preserve the original span through reordering and
reject removed or unlocked badges.

Overview dimensions and offsets scale in logical units and clamp to the safe area. Its retained
stamp includes chrome, metrics, document and page transforms and page extents. Drawing, camera-click
mapping and accessibility share the same panel box. A delayed camera click is rejected after its
geometry changes. Xuzz registers the link panel and overview accessibility sources.

`ui::withFontOverride` centralizes legacy font-description parsing for all consumers. No application
model was promoted into the core library. Graphics-level overlay tests run in the existing ZigZag
application test binary; engine-only Xudu and Xuzz tests keep their graphics-independent boundary.

## Verification

The 23 new overlay regressions cover viewport edges, three standard sizes, four font scales,
Sans/Serif/Monospace/wide fallback families, visible fitted glyphs, accessibility bounds, retained
uploads and shaping, exact navigation commands and stale pointer/accessibility actions. The default
link panel can use the full safe height: at large fonts, all context and action rows need it.
Integer nested layout budgets prevent the final button row from losing its glyphs through rounding.
Intrinsic page-break button sizes also reserve fractional text advances before rounding.

The full build and strict backend comparison passed. OpenGL and GLES app captures are identical. The
link panel and overview also match Vulkan exactly; Vulkan's page-break and payment captures differ
by more than one channel level in 0.0498% and 0.0396% of pixels, below the 1% comparison tolerance.
Captured PNGs were inspected: both link endsets and all actions are visible, the normal page-break
label is complete, badges stay bounded and the overview has visible page/camera geometry.

The temporary app harness extends `tools/ui-text-baseline.cpp`'s synchronized software-rendering
workload. It takes 100 measured frames after 10 warm frames, including device completion. The p95
values below are milliseconds; they describe these fixtures, without claiming a historical
before/after speedup.

| Presentation  | OpenGL |  GLES | Vulkan |
| ------------- | -----: | ----: | -----: |
| Link panel    |  5.731 | 5.680 | 10.444 |
| Page break    |  4.242 | 4.156 |  7.504 |
| Payment badge |  4.055 | 3.912 |  7.712 |
| Overview      |  3.931 | 4.014 |  7.610 |

A separate no-op render-device probe measured 1,000 warm frames per presentation. Every case had
zero C++ allocations/bytes, buffer or texture uploads, pipeline creation, HarfBuzz/layout/fallback
calls and shaped input bytes. CPU p95 was 191 ns for payment, 201 ns for page break, 181 ns for its
long-family override, 191 ns for the retained link presentation and 141 ns for a real-document
overview. This excludes GPU/driver work, third-party malloc-only allocations, live payment-domain
resolution, link-model preparation, paragraph discovery and Session validation, document attachment
and active unlock animation.

Logs and temporary probe sources are under `/tmp/ui-text-fit-batch5-*`, `/tmp/ui-app-profile.cpp`
and `/tmp/ui-app-text-baseline.cpp`; captures are in `/tmp/ui-text-fit-batch5-captures/`.

The full test run passed 731 library, 1,252 engine, 58 Xuzz, 141 ZigZag, 12 swarm and one
publication test. The engine suite retained its existing skipped video screenshot test and disabled
test. After the final scaled-button hover regression, the full build and all 142 ZigZag tests passed
again. Repository formatting and lint checks passed.
