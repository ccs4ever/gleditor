# Independent initial review: sixth spatial UI increment

Reviewed `HEAD 78f692e5` with the `xuzz-ui-design` and `ux-validation` contracts, the
fifth-increment source evidence, and the current document/link entry path. This is an independent UX
and visual-direction review before the sixth candidate is frozen. The fifth multipage evidence is a
bounded failure, not proof of a deadlock: the UI authored and selected the 7,439-byte range,
including a real forced page break. In its accessibility snapshot, the source and target occurrence
rows each say `page 1/1` even though the selected source spans 80 lines; the visible `place 1/1`
labels separately and correctly describe a single occurrence. A visible Enter click was logged, yet
no post-entry frame, changed focus, or new visit was captured before the bounded run ended. No
continuation or return result follows from it.

## Current seams and risk

`Doc::layoutFrom()` respects authored forced breaks by stopping shaping at the next break after the
page start. The page index filade retains shaped page byte ranges and indices independently of
GPU-built `Page` objects. Link entry resolves the exact selected document occurrence and puts both
range endpoints into `Doc::setPriorityOffsets()`, which moves their pages ahead of unrelated
backlog. This gives the candidate an existing route to prioritize the exact beginning and end, but
an indexed page is not yet usable geometry: `anchorFor()` remains the readiness gate for a built
page.

`LinkContext::execute()` currently makes Enter pending when either endpoint anchor is missing, and
`advanceFrame()` retries while the exact view generation and document remain present. A new
navigation command clears the previous pending request before processing; `CancelEntry` clears its
unavailable status and returns without dispatching a visit. On readiness it re-enters through
`execute(command)`, so the selected link/member/occurrence must still resolve to the requested exact
site before movement. The sixth run must demonstrate that cancellation or stale/replaced selection
cannot later complete the old trip. Waiting for anchors must be visible, cancellable, bounded by
ordinary frame progress, and must not fake a completed visit.

For a document entry, `xuzz_app.cpp` first aligns the source and companion pages and places the
camera, then calls `Views::keepRangeInView()`. That helper computes precise selection bounds only
when the range is on one built page and `Page::textBoundsFor()` supports it; a cross-page range
deliberately falls back to the first caret. A later camera callback waits for link/camera motion to
settle and rechecks the entry generation, document identity, caret document, and exact selection
before applying the same fit again. This protects against stale camera work, but a successful
multipage journey still needs explicit ordinary wheel continuation. That movement must leave the
byte selection, caret, Link identity and occurrence unchanged and must not repeatedly refit to page
one.

The essential bounded outcome is a real `Ctrl+Return`-authored cross-page source range of
`[0,7439)`: visible Enter waits for both endpoint anchors and enters only the chosen source
occurrence; the beginning is readable on landing; an ordinary wheel continues to the later selected
page without a new visit; explicit Enter creates exactly one visit. Source document operation
snapshots must remain byte-identical throughout. Canceling a pending Enter, or replacing its
selection/link before readiness, must leave the reader at the origin and never produce a later trip.
This is a focused regression scope, not a complete J24 or J1–J32 journey.

The presentation should keep the selected passage as the focal point, retain the existing
Link/context hierarchy and hues, and show pending status without competing with text or implying
entry. At each point where the selection is visible, glyphs should remain readable above chrome,
with later-page continuation clearly distinguishable from an accidental camera snap. Compare the
actual default viewport and a tighter viewport if the frozen candidate provides both; inspect the
initial landing, a partial wheel move, the later-page view, cancellation and final state.

As a visual baseline, I inspected the archived 800×600 OpenGL entry capture from the fourth
increment (`evidence/wide/final/800,600-opengl/01-enter.png`). The cyan selection gives a clear
focal point, the tab underline and link panel share a restrained cyan accent, and the gold focus
marker is legible without taking over. The selected text reads clearly above the bottom panel. The
rest of the canvas is mostly black negative space, so the sixth landing should retain the text scale
and hierarchy while giving the reader an obvious later-page continuation; do not infer multipage
quality from this single-page reference. The fifth forced-break preview screenshot shows a one-line
selected target and a large Link panel, while the accessibility snapshot reports both endpoint
occurrence rows as page 1/1; neither gives evidence that the chosen 80-line source is readable
through later pages.

## Boundaries for this increment

Keep the fix within existing `Doc` layout/index/readiness, `LinkContext` pending entry and
`advanceFrame()`, and current camera callbacks. Reacquire page anchors from the exact byte offsets
after layout/reflow, keep the existing readable first-caret fallback for ranges whose full bounds
cannot fit, and let normal camera movement expose later pages. Do not fabricate page geometry or
block the frame while shaping. Preserve camera orientation and established companion placement;
there is no sixth-increment justification for a new camera service.

Keep tablet hardware behavior, a new view host, and camera architecture work deferred with the
separate L6/L7/U4a work. This review does not authorize range-wide auto-framing across many pages.
It also does not claim native AT-SPI coverage: the available tree is not the platform's internal
accessibility tree. Optimized performance acceptance (O0) is outside this bounded UX regression;
record the build mode and avoid any performance claim.

## Evidence needed for the independent final review

After the root candidate is frozen, I will replay only through a fresh X11/Xvfb profile with the
exact default keymap and visible Enter control, then verify the running binary hash/caps, actual
interface captures, exact selection and current visit, and source operation snapshots. No `--do`,
model/path injection, or direct operation writing can stand in for user input. Retain lossless PNGs,
session/action log, accessibility dump, metadata, and hashes in this review's Vulkan evidence
directory; raw PPM captures and private profile stores stay outside Git. Compare PNGs against the
actual raw PPM frames, inspect the other captured GL/GLES frames too, and record any backend
evidence separately from the Vulkan journey. If the frozen candidate exposes a different exact user
route, document and assess that route before claiming it.

The final report will state the bounded scope and evidence gaps plainly. It will not claim
completion of J24, J1–J32, native AT-SPI, or O0 performance validation.

## Review follow-up and fixture correction

The initial goal above used the normal fixture length for both modes. The executed forced fixture is
7,438 bytes: the 80 typed lines are split at byte 3,719 around `Ctrl+Return`, and the shell command
substitution removes the newline at the split. Its selected range is `[0,7438)`; the normal fixture
is 7,439 bytes with range `[0,7439)`. Both are valid multipage journeys, but the forced run does not
demonstrate a 7,439-byte selected range. This correction preserves the initial review chronology
while fixing the evidence contract.

Likewise, the fifth-increment occurrence label `place 1/1` is a valid count of one occurrence. The
separate preview accessibility row `page 1/1` was the layout-page label under review. The current
UI's `--dump-a11y` output is the product's internal accessibility tree; it does not provide a
platform AT-SPI tree, which is unavailable in this environment.
