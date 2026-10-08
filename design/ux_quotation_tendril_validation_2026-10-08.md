# Quotation tendril and Vulkan ownership — 8 October 2026

This batch follows the quotation report. It fixes event-thread destruction of the pouch divider's
GPU buffers and replaces the stepped drag tether with a tapered quadratic curve. It does not change
publication formats, authorial editions, quotation addresses or persistent transclusion beams.

## Design and implementation

The quotation feedback is view-only. Its source stays anchored; the pointer endpoint follows actual
input, while existing spring return remains available on cancellation. A narrow gold stem tapers
from 4.5 to 1 logical pixel. Stationary paired fibres and restrained optical banding recall the
transclusion beam's texture, but the curved taper, small stem and hollow pointer marker distinguish
an unfinished quotation from the broad persistent prism. No travelling highlight is added.

Independent UX and aesthetics reviewers approved this direction before implementation. They required
scaled dimensions, quiet texture, readable card instructions and a clear actual pointer pixel, even
while spring-smoothed state lags. The entire ribbon excludes a circle around that pointer; the stem
also ends beyond the marker. Short drags omit the stem when it cannot fit. The floating card keeps
the existing collection, spawning and cancellation instructions.

The generic `CurveRibbons` renderer lives in the library. Procedural quadratic-strip vertices share
positions and analytic normals at subdivision boundaries. Its fragment shader softens the
silhouette, shades the fibres and excludes the pointer circle. Tessellation and retained storage
bound a single curve to 256 instances; stationary geometry does not rebuild. The glyph-compatible
device interface and portable GLSL pipeline serve OpenGL, OpenGL ES and Vulkan. This native C++ and
shader code handles GPU geometry and resource ownership; it is not an application command or stored
computation suited to Vortex. Quotation semantics and presentation policy stay in the shared
xanalogical UI.

The live `system://ui` `tether` settings expose root/tip widths, sag cap/fraction, pointer
marker/gap, texture period/strength, colour and reduced motion. Defaults, units and bounds are in
the system document's schema/purpose page and setting specifications. Configuration changes
invalidate retained geometry; reduced motion follows input directly and completes spring
cancellation without a tween.

Closing the pouch previously reset its device-backed divider on the event thread. Buffer destruction
called the Vulkan idle path concurrently with render-thread queue work. The divider now stays alive;
its visibility changes immediately and its identity/model updates on the next render frame. This
removes the ownership violation rather than serializing driver calls while leaving cross-thread
resource mutation in place. A regression checks that close/reopen performs no buffer creation,
updates, destruction or device idle call.

## Evidence

All captures and transcripts are retained under `build/tendril-evidence/`, with native stores,
permascrolls and saved operation dumps alongside them. Independent reviewers inspected the new
Vulkan direction/scale matrix and the OpenGL/OpenGL ES equivalents. They found no tendril-specific
or bounded collection/drop blocker: the curve, taper, fibres and hollow endpoint are readable.

| Journey                   | Step                                                 | Affordance used                                           | Outcome                                                                                           | Evidence                                                                                |
| ------------------------- | ---------------------------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------- |
| J3 subset                 | Detach while preserving source operations            | Pointer press, movement, release in empty space           | Pass; new reader store retains all three global span identities, source operation count unchanged | `<backend>/detached.png`, `spawned.png`, `spawn/transcript.log`                         |
| J3 subset                 | Close/reopen pouch during a quotation and collect it | F2, Zones, Escape, source press, F2, pointer release      | Pass; three saved cards with exact source ranges and no Vulkan driver error                       | `<backend>/pouch-hover.png`, `pouch-dropped.png`, `pouch/transcript.log`                |
| J3 subset                 | Follow short, long and reversed drags                | Pointer movement                                          | Pass for feedback; stem stays continuous and tapers, pointer marker remains hollow                | `<backend>/tendril-left.png`, `tendril-short.png`, `tendril-up.png`, `tendril-down.png` |
| J3 subset                 | Cancel at default scale                              | Escape, then pointer release                              | Pass; no new store and no remaining ghost                                                         | `<backend>/cancelled.png`, `cancel/transcript.log`                                      |
| Reduced-motion diagnostic | Drag and cancel at UI scale 1.5                      | Prepared native UI configuration, then pointer and Escape | Tendril and cancellation pass; card heading placement finding remains                             | `<backend>/scaled-tendril.png`, `scaled-cancelled.png`, `scale/transcript.log`          |

The foreign-run journey now uses `--strict-diagnostics`. It passed once on OpenGL and OpenGL ES and
three fresh runs on Vulkan, including the previously failing pouch toggle. The existing page-drop
regression passed on each backend, twice on Vulkan. No driver-error text appears in the final Vulkan
transcripts. The mock regression separately proves that an event-thread toggle performs no buffer
creation, update, destruction or idle call; this directly covers the removed reset.

The library suite passed all 753 cases, Xuzz passed 61 and the slice/overlay suite passed 194. The
engine suite excluding interface orchestration ran 1,303 cases: 1,285 passed, 17 skipped (16
namespace-dependent network cases and one video capture case), and the unchanged Chronofilade speed
benchmark failed during concurrent renderer checks. Its isolated retry passed: raw replay 4.44
microseconds versus Chronofilade 3.34, compared with the initial 4.24 versus 6.35. No benchmark
assertion was weakened. Both logs remain; this is not a full `make test` pass.

`make -j$(nproc) shaders`, format-check, lint, UI text policy and `git diff --check` passed. The
library renderer comparison passed on its isolated retry across all compiled backends, using SDL
offscreen because `xvfb-run` is unavailable. The first run failed exact Vulkan culled/unculled image
equality: 5,904 pixels differed in the glyph region, with maximum channel difference 39. Captures
and both logs are retained; the cause of that intermittent difference remains unconfirmed. The
comparison's full orchestration extension was excluded with `XUDU_TEST_BIN`; the targeted backend
journeys above validate the new shader and input flow. The small plain-editor sample did not
exercise actual multi-thread command-recording splitting.

The interface fixture prepares Alice's three-member publication with temporary keys and mock
provenance. That preparation is not publication/verification acceptance. The scale and
reduced-motion matrix uses prepared native UI settings as diagnostic inputs; it does not certify
editing those settings through a user control. Actual quotation pickup, movement, collection,
release and Escape use native pointer events and keymap actions.

## Remaining scope

This batch validates the quotation feedback and the observed pouch resource race. It does not claim
that all Vulkan callers are thread-safe, certify the complete publication swarm, or redesign the
oversized quotation card and dense pouch controls. Saved cell source-return and the existing
activity history's missing scoped-document birth remain separate work.

Independent capture review found remaining card/notification placement limits: the Spawn xanadoc
heading is hidden at scale 1.5 although preview and release instruction remain; the floating card
covers part of the terminal path in downward drags; a ready notification overlaps the left-drag
release instruction; pouch controls remain crowded. These findings need the shared world-card and
notification layout work. They do not certify full UI polish at either scale.
