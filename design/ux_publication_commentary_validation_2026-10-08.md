# Commentary quotation prerequisites: validation (2026-10-08)

This batch repairs the insertion and cross-store address failures encountered while beginning
[P4/P5](ux_workflow_publication.md). It verifies a cached-publication quotation and link-authoring
prerequisite. It does not certify two independently published commentaries, Alice's response,
notifications, offline reopening, or the full P1–P7 journey.

## Baseline and repairs

Before the repair, the visible Notes card's Insert action did nothing: Xuzz had never registered the
drawer's use handler. `build/publication-commentary/baseline3.log` and the before/after captures
show the commentary remaining `Bob response:` after activation.

The drawer now inserts at the active commentary caret. Keyboard staging keeps every contiguous
member of a selection, as separate existing pouch cards and complete ordered forge endsets. Each
span is translated from its source store into the pouch's deployment coordinates, then into the
active destination's coordinates. Numeric scroll slots are never treated as cross-store identities
in these repaired paths. The drawn forge uses the active document rather than document zero.
Type/tier selectors remain usable without an active document; actual forging requires one.

`carrySpans` reuses `carrySpan` and preflights the whole run before registering destination scrolls.
An unreachable author-local member refuses the run before operations or primedia are written.
Session transclusion, the two-stage document link command, focused-cell quotation and cell link
endpoints now supply their source store explicitly. Closing a document cancels pending link state.
Cell drag staging names the actual slice store rather than store zero.

These are native scroll-table translation and raw UI wiring changes: their C++ boundary handles
store deployment coordinates and existing render callbacks. They introduce no new view, command, key
binding, wire format or stored operation layout. Author edition choices remain unchanged.

## Cached-publication regression

`E2EBinaryOrchestrationTest.cachedForeignRunsAreQuotedAndLinkedIntoTheActiveCommentary` prepares
Alice's signed immutable fixture with temporary BEP 46 keys and explicit mock provenance. It has
three consecutive carrier runs: `ALPHA`, an external research-scroll `RESEARCH`, and `OMEGA`. This
setup supplies an already cached publication; it is not DHT discovery, Oracle verification, or the
three nonadjacent passages and slice-cell authorship required by P4.

The actual application opens the signed manifest through Ctrl+O's Custom path field, stages Notes
with Ctrl+Shift+3 and the left bench with Ctrl+Alt+\[, creates Bob's sovereign store with Ctrl+N,
types `Bob:`, and stages that text on the right bench with Ctrl+Alt+\]. F2 opens the drawer; its
Zones and Notes pagination controls expose all three Insert actions. The visible Forge Clasp control
authors the link. These automation steps use default bindings and accessible controls.

The regression verifies:

- The accessibility text is `Bob: ALPHARESEARCHOMEGA`.
- Saved quotation and link members have Alice's original global keys, offsets and lengths.
- The commentary link keeps all three left members and Bob's right member.
- Alice's installed native store has the original operation count and no commentary link.
- Alice's primedia is not copied into Bob's permascroll.

The saved-address check explicitly supplies piece-verified carriers from the fixture. It does not
prove standalone offline reopening of the commentary. A separate fresh-profile application run
opened the saved commentary without Alice: `offline.log` reports `Bob:` followed by eighteen blank
positions, and `offline.png` shows the missing quotation content. This confirms a restart failure,
not merely a suspected limitation. Engine regressions separately use conflicting source/destination
scroll-slot orders and a refused run containing a foreign-author local member. The overlay
regression checks that the drawn forge writes into the active commentary and leaves the source store
untouched.

## Captures and independent review

Evidence lives under `build/publication-commentary/`:

| Artifact                              | Evidence                                                                |
| ------------------------------------- | ----------------------------------------------------------------------- |
| `manual.log`, `open.png`              | Opening the signed cached source; readable Alice text                   |
| `manual.log`, `quoted.png`            | All three Insert actions and Forge; cleared benches afterward           |
| `visible.log`, `staged.png`           | Populated benches before forging                                        |
| `visible.log`, `visible.png`          | Readable Bob commentary after Escape, Ctrl+Alt+1 and Ctrl+3             |
| `offline.log`, `offline.png`          | Fresh-profile reopening loses quotation content without source carriers |
| `commentary.ops.txt`                  | Saved native commentary operations and scroll references                |
| `focused-test.log`, `zigzag-test.log` | Focused regression and full slice/overlay suite                         |

Independent UX and aesthetics reviewers inspected the actual captures. The first post-forge frame
cropped the commentary behind/off the drawer. Closing it and selecting document mode produced
readable visual proof, confirmed by both reviewers. Presentation findings remain: header controls
truncate/run together, populated bench summaries collide, the byte count reports the first member
rather than the whole run, and filename/untitled tabs provide weak ambient provenance.

## Checks

The focused cached-publication and span-translation regressions passed, as did all 193 slice/overlay
cases. Format-check, lint, the UI text-policy check and `git diff --check` passed.

The first broad run passed 751 library cases and 1,302 engine cases, with one environment video
skip, but failed the unrelated Chronofilade speed assertion (6.23 µs versus 5.25 µs). Its isolated
retry passed (3.66 µs versus 4.40 µs). Both logs are retained; the first run is not a passing
`make test`. The complete retry passed the benchmark but failed the existing drag calibration when
its captured selection could not be detected. That drag case passed in the first broad run and then
in isolation. Both complete runs therefore ended nonzero before the remaining suites. The drag retry
and full Xuzz suite passed separately (61 cases). The new cached-publication case passed on OpenGL,
OpenGL ES and Vulkan. No assertion or screenshot tolerance was weakened. Namespace transport passed
all 12 cases and all four publication integrations passed through `make -j$(nproc) test/swarm`,
recorded in `swarm-test.log`.

## Remaining work

1. Retain or resolve the verified immutable carrier closure for quotations in the commentary and
   private pouch across restart without reopening the source publication. Descriptor translation
   alone does not provide those files.
1. Complete pointer document-drop run translation and whole-selection preservation. Direct pointer
   drops still use the old first-member/raw-coordinate path. Repair pouch origin navigation and F9's
   startup-store assumptions, and distinguish authoring a commentary from Ctrl+T's source branching
   behavior. The current result does not establish input parity for those paths.
1. Repair populated-bench presentation, total byte counts and provenance labels.
1. Run Bob and Carl's nonadjacent document/research/cell quotations, commentary publication,
   backlinks, Alice's reply and notifications through P4/P5, then rerun full P1–P7 acceptance.
