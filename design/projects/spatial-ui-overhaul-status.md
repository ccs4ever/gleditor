# Spatial UI overhaul implementation status

Baseline: `e9726a6b3825e794ab86db6cd54f18d9258e75c0` on `design/spatial-ui-overhaul-2026-10-13`.
Implementation branch: `feat/spatial-ui-overhaul`. Started 2026-10-13. This file is the continuation
checkpoint; update it in every implementation commit. Do not merge or deploy from this project
session.

## Current checkpoint

Sixth checkpoint (2026-10-14): UI-authored text reflow now updates the shaped-page index beyond the
GPU-built page frontier. The fifth checkpoint could leave Link Enter pending because its exact
endpoint page was missing from that index. `Doc::reflowFrom()` recomputes changed metadata until an
unchanged pagination boundary, retains suffix metadata and sparse GPU slots, shifts retained page
offsets/poses, and invalidates same-count byte-length changes. Stale queued shaping is discarded;
the existing priority/budgeted builder obtains the endpoint. No Link command, reader activity,
serialization, camera, public API or application-specific code changes.

The append-after-break regression fails against the old library; the candidate passes all 25 owning
DocGap/DocPageBudget checks. A sequential full library run passes 787/787, the UI suite 233/233 and
Xuzz suite 99/99. The earlier concurrent library run passes 786/787 with one single-call budget
assertion failure; both logs are retained. Three Luna validators demonstrate actual authored normal
`[0,7439)` and forced `[0,7438)` multipage Enter and selected later-page continuation on GL, GLES
and Vulkan. GL also covers 1440×1000; enlarged chrome uses the governed native font-scale form.
Origin and direct Back restoration are distinguished from Back-at-root no-ops. Sources remain
unchanged across observed navigation snapshots. Some snapshots named `before` were taken after
Enter; true pre-entry activity deltas are established separately and must not be inferred from those
late snapshots. A supplemental GLES forced run captures root visit 1 before Enter, visit 2 after
Enter, and the same two records after wheel/Back/Origin, with both source dumps byte-identical
throughout.

The core renderer gate remains open: three strict comparison runs fail exact glyph-pixel equality in
an overlay or configured Vulkan recording comparison. The same overlay glyph difference is
reproduced using the frozen old library with the same plain-editor executable; no tolerance was
weakened. The sequential run passes ordinary frames, overlays, picking, culling, atlas-growth and
minified-text checks before its later configured-thread capture mismatch. The small sample does not
exercise an actual split recording path. This is an unresolved capture finding, not a fully passed
backend gate. Exact logs, lossless PNGs and binary hashes are in
`design/reviews/spatial-ui-sixth-increment/validation.md`.

Repository format, configuration harmony and UI text policy checks pass. Lint has exactly the same
37 baseline Markdown diagnostics and none in touched files. The strict core capture comparison is an
open finding; it is not included in those passed gates. `origin/main` remains at the recorded base;
only the implementation branch is pushed.

Metadata preparation remains outside drawing, but shapes changed pages separately from rebuilding
GPU pages. No optimized performance-budget acceptance follows from this O0-g0 build. Pending and
stale-target cancellation, repeated occurrences, cell/history, branch reopen, actual enlarged
document text, native AT-SPI, publication-service audit/D2, whole-page ink and the full 16+16
journey set remain open. Do not claim complete J24/J25, J17–J32 or full J1–J16 preservation.

Fifth checkpoint (2026-10-14), validation complete: the native UI now has a governed
`Ctrl+Alt+Shift+F` action for editing `ui.fontScale`. It opens the existing native form, validates a
positive finite value representable by the UI, writes the typed setting through the existing system
document adapter and repoints the UI system document so the live watcher applies it. The UI system
document notes distinguish chrome typography from document text size and point to the action. This
is an existing-form input path and command-dispatch action, not a new domain service, camera or view
host. In C++, `Views` opens the existing `Form` and handles its callback on the render thread; the
callback uses the typed system-setting update API and repoints `Session`. The action uses the
existing sovereign keymap and Vortex command-dispatch path. The form also rejects malformed,
out-of-range and underflow values without changing the setting; cancel leaves it unchanged. The UI
suite passes 233/233, the Xuzz suite 99/99 and the SystemDocs suite 24/24. The new form E2E passes
on OpenGL, OpenGL ES and Vulkan. The independent font-scale Link journey passes at 800×600 on all
three backends and 1440×1000 on OpenGL. The core renderer comparison has exact OpenGL/OpenGL ES
equality and 0.2471% Vulkan pixel difference; its broad E2E extension was excluded. The prior
785/785 core library pass is reused after verifying the binary is unchanged, not counted as a
fifth-increment rerun. Do not treat the inherited 1117 test count as a fifth increment run. Full
details and hashes are in `design/reviews/spatial-ui-fifth-increment/validation.md`.

Repository format and UI text policy checks pass. Lint retains exactly the 37 unchanged Markdown
diagnostics from the fourth checkpoint and reports none in touched files. `origin/main` remains at
`c7f70cc757034c006a6ce18d2a11e9c962aecce4`; no subsequent main change required integration.

The earlier native `ui.fontScale` probe did not reach a usable scalar-value editor from the tested
settings-document focus state; it did not establish that all native routes were unavailable. The new
governed action supplies a direct path. At 800×600 and scale 1.5, the folded Link investigation
status is clipped and the investigation cannot fully expand. The native occurrence fixture had one
available occurrence for each authored member; repeated occurrences were not exercised. The 80-line
multipage previews at 800×600 did not produce evidence of a completed document entry; no cross-page
continuation or return behavior is validated. See
`design/reviews/spatial-ui-fifth-increment/evidence/occurrence/validation.md` and
`design/reviews/spatial-ui-fifth-increment/evidence/multipage/report.md`.

At the fifth checkpoint, the next task was UI-authored multiline Link entry through the existing
page-readiness and pending-anchor lifecycle: both selected range endpoints must be ready before the
bounded preprojection fit, while an ordinary `Ctrl+Return` page break must not trigger an invented
Enter visit or visited-source write. Then validate actual enlarged document text using existing
layout tunables, followed by cell/history, pending/cancel and branch reopen. Audit the publication
services before advancing the separate D2 history prerequisites. Do not claim full J1–J16
preservation or complete J17–J32 coverage. Repeated occurrences, unavailable/pending cancellation,
branch reopen, cell/history, actual enlarged document text, native AT-SPI, optimized performance,
whole-page ink and complete 16+16 journey coverage remain open.

Fourth checkpoint (2026-10-14), validation complete: multiline Link landing uses retained page-range
bounds and the existing camera placement seam. The fit accounts for live and settled chrome on all
edges and the visible overview with a shared safe margin. After existing LinkBeams and timeline
motion settle, it queues one final fit before projection; generation, weak document, view index and
exact selection guards cancel stale work. Complete fitting is bounded to a built single page with
supported glyph geometry; oversized, multipage or unsupported ranges prioritize the exact beginning
with its full line height. No new page poses, camera, view host, source operations or visits were
introduced. The existing source page pose, exact selection and activity command boundary remain
unchanged.

Independent native review accepts five cases: 800×600 on OpenGL, OpenGL ES and Vulkan shows all
eight selected lines with a clear 30 px first-glyph inset; 1440×1000 OpenGL shows the full source
and target; 640×480 OpenGL shows a readable prefix of roughly two lines without claiming the whole
selection fits. The five before/after source-operation snapshots are byte-identical. The final
785-test library suite passed, including the three new retained-geometry checks for wide
intermediate lines, clipping to an interior page and partial ligature fractions; no tests were
skipped. The 233-test UI suite and 99-test Xuzz suite passed. Link travel passed on all three
backends and both repaired drag E2Es passed on OpenGL. The core renderer comparison has exact
OpenGL/OpenGL ES equality and 0.2471% Vulkan pixel difference. The full evidence index is
`design/reviews/spatial-ui-fourth-increment/validation.md`; raw native captures remain outside Git
in `/workspace/spatial-build/fourth-raw/`.

The stale broad E2E drag target assumptions were corrected from native calibration frames. The final
safe-margin application passes both repaired strict cases on OpenGL (2/2), preserving exact byte
insertion, primedia provenance, reader ownership and unchanged author operations. These two tests
are closed findings, not future work. The historical whole-page ink threshold remains open, and
these bounded validations do not establish full J1–J16 preservation.

Third checkpoint (2026-10-14): short cross-document Link landings now keep the complete selected
passage readable and synchronize the selected tab with document focus. Exact-range focus reuses
`Views::activateDocument()` and cancels its ordinary page-zero frame; after existing companion
framing, the existing caret-follow helper brings the selected range's endpoints into view with its
beginning taking priority. No new camera, view host, source operation or visit path is added.

The independent Luna post-fix run now has same-profile source operation snapshots before navigation
and after Back. Both source hashes are unchanged. Enter → Origin → Enter → Back produces exactly two
sibling completed visits (2 and 3 under root 1); Origin and Back restore without adding visits. The
focused passage and selected tab match through the sequence. Native evidence is in
`design/reviews/spatial-ui-third-increment/evidence/landing/`.

The new UI-authored E2E check waits for Doc 2 selected after Enter, Doc 3 after Origin and default
after Back; it verifies exact selections and the retained root plus two sibling visits. It passes on
OpenGL/OpenGL ES/Vulkan, alongside 233/233 native UI and 99/99 Xuzz tests. Independent review
accepts the bounded short single-line 1440×1000 fixture. Long/multi-line ranges, narrow/enlarged
text, motion/AT and cell/historical/unavailable/reopen variants remain open. Sequential endpoint
following does not guarantee that a range larger than the viewport fits in its entirety.

Second checkpoint (2026-10-14): the pinned-baseline comparison classifies all four original broader
E2E failures as pre-existing. The drawer's missing visible New Xanadoc and Close Drawer controls are
repaired by reserving whole-pixel measured padding through nested containers and flooring the header
width. No settings, object creation semantics, stored identities or stale-action checks change. The
full native UI suite now passes **233/233**, the four owning drawer checks pass their 48
size/font-scale/family combinations, and the named-control creation E2E check passes.

The scoped Luna follow-up UI-authors a two-document Link and verifies logical Enter/Origin/Back, two
distinct sibling visits (2 and 3 under parent 1), current visit 3 after the second Enter and current
visit 1 after Back. Source dumps match an independently UI-authored control fixture, not a
same-profile before/after snapshot. That second-checkpoint left landing clipped the passage
beginning beyond the viewport; the third checkpoint resolves the short fixture. Wider
readable-landing variants, cell/historical targets, unavailable/cancel and branch reopen remain
unaccepted. The report preserves those limits explicitly.

Actual Store Object Manager → New Xanadoc → New Slice → Close Drawer → reopen sequences pass at
640×480 and 1440×1000 on OpenGL, OpenGL ES and Vulkan, using visible controls. Independent review
accepts the bounded repair; dense and enlarged-text captures, keyboard-only creation and AT remain
open. Evidence and the baseline classification are in `design/reviews/spatial-ui-second-increment/`.
The unchanged whole-page frames are pixel-identical; the two original drag checks use stale
destination assumptions. Those three E2E checks still fail and are not accepted by reclassifying
them.

Inventory and the first runnable native prerequisite increment are implemented. The default build
and 100 owning checks pass. Native UI authoring, complete 2-by-3 member controls and a readable
same-document quiet/expanded/refolded sequence are evidenced. No complete J17–J32 journey or full
J1–J16 preservation gate is accepted. The 86 tasks, 49 alternate routes, 370 transitions and 435
states in the prototype remain representation evidence only.

The increment adds ephemeral exact byte references; freezes a separate selection-start Return
checkpoint without recording selection/preview visits; persists that checkpoint in versioned reader
activity; and adds **Inspect members** / `Ctrl+Alt+I` to the existing retained Link panel. It lists
both complete ordered endsets, individually named preview actions, chosen occurrences and exact
authority/state/frame/source ranges. Explicit Enter remains travel. Expansion uses unoccupied canvas
and refuses to cover the reading surface when it cannot fit. Physical wings and the new
view-host/camera architecture remain deferred. Existing `Alt+Shift+I` Controls remains separate.

Old `visit1` / `selection1` activity payloads are preserved and refused as unreadable; they are not
silently migrated or rewritten. The new records are `visit2` / `selection2`. The operation-node
format and ReadingPlace schema do not change. Session activity is validated before a writer is
adopted. Exact byte references explicitly reject partial occurrences lacking offset mapping, package
synthetic presentation spans, mismatched source registries and ambiguous document frames. They do
not implement media clocks, durable D5 cards or a latest-state fallback.

Build prerequisites repaired in this increment: Beman optional-reference use in the native panel,
Poppler API capability detection for the installed 25.03 library, and the optional no-ThorVG test
helper guard. An existing dock test incorrectly required paging even when a compact dock was fully
readable. An isolated pinned/current presenter comparison reproduces that assumption; the test now
checks exact prefixes and every command in either complete layout. The new equal-text identity test
uses nonadjacent source addresses to avoid automatic adjacent-span merging.

The clone matches the requested commit; submodules are initialized recursively. `origin/main` is
still `c7f70cc757034c006a6ce18d2a11e9c962aecce4`, the design branch's recorded base, so there are no
subsequent main changes to integrate. No previous implementation worktree existed in this workspace.
`.agents` is a symlink to `.claude`; the recorded rules and skills are present there.

## Code inventory and discrepancies

| Boundary                  | Existing native code                                                                                                                                                                                       | Reconciliation and missing prerequisites                                                                                                                                                                                                                                                                        |
| ------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Generic rendering         | `include/gleditor/spatial.hpp`, render-region types/device implementations, `src/renderer.cpp`, `Canvas`, `WorldPanel`, widgets and focus manager                                                          | Render regions and depth slices are implemented with backend-neutral projection. `PlaneSet`, page flow/pose separation, pane tree and view-host framework are absent. Do not create a private replacement camera or plane architecture.                                                                         |
| Exact Link navigation     | `link_occurrences.*`, `link_navigation.*`, `link_panel.*`, `link_views.hpp` in `apps/common/xanadu/`                                                                                                       | One Link identity, ordered complete endsets, separate side cursors and exact document/cell occurrences already exist. D1 still needs a reusable disjoint reference descriptor; partial matches lack a source-offset mapping.                                                                                    |
| Native UI integration     | `LinkContext`, `LinkPanelOverlay`, `LinkBeams`, `BridgeCoordinator`, `Views` under `apps/common/ui/xanadoc/`; `link_panel_presentation.*`                                                                  | Retained panel, protected prose placement, dock and paged controls already exist. Extend their command boundary. Baseline `execute()` calls `noteOrigin()` on new selection and can append a visit after caret movement: fix before acceptance.                                                                 |
| Activity and restoration  | `StoreActivityLog`, `reading_place.*`, `Session`, `walks.*`                                                                                                                                                | Persistent reader-owned visits, parents/children, references, notes and session checkpoints are implemented despite older proposal labels. Exact selection-start Return needs a checkpoint distinct from a completed visit. Media clock restoration remains unbuilt. Unreadable activity must remain preserved. |
| Publication and packages  | `publication_inbox.*`, discovery/subscriptions/outbox, `ReaderLinkPackages`, download/package forms                                                                                                        | Immutable publication and reader-layer services are live. Audit pin ownership, verification domains, retain/enable/emphasis and disabled-but-pinned selection before J17/J23 acceptance. Do not substitute fixture success.                                                                                     |
| Search and comparison     | Telescope search, VQL, hypertime graph/comparison, Spanfilade and Chronofilade                                                                                                                             | Existing search and comparison do not supply D4 cross-media/all-version occurrence coverage or D3 parent-state effect footprints. Historical text removed later cannot be found by raw primedia search alone.                                                                                                   |
| Collections and authoring | `pouch_zone.*`, pouch drawer, quotation builder, clasp forge, batch orchestrator                                                                                                                           | Existing durable content cards and ordered Link authoring are usable seams. Typed op/query/walk/draft dispatch, durable drafts, explicit pinned receiving base and attempt-token reconciliation need services; no atomicity claim is established.                                                               |
| History                   | Operation spool, Chronofilade, cell subject chains, hypertime graph                                                                                                                                        | D2 own/contained/all-branch enumeration and D3 affected topology remain prerequisites. A subject chain is not all history affecting a cell; diagram order is not ancestry.                                                                                                                                      |
| App wiring and isolation  | `apps/xuzz/xuzz_app.cpp`, existing `ViewCoordinator` and slice presentation surface                                                                                                                        | Legacy page/slice/navigation remain available. `apps/gleditor` stays isolated. No `apps/xudu` or `apps/zigzag` sources remain. M0 A1 relocation exists; A2 test rename is still outstanding (`zigzag_test`, `tests/zigzag`).                                                                                    |
| Automation and tests      | Ordered synthetic input in `src/app.cpp`/`src/renderer.cpp`, `--chord`, click-label, mouse gestures, wheel, resize, await-a11y, capture and a11y dumps; `tools/ux-journey.py` and 51 curated J1-J16 stages | Graphical journeys use offscreen SDL, not batch `--headless`. Prepared scripts are replay aids, not authoring passes. Native captures and actual controls are required. Tests remain with their owning code.                                                                                                    |

The view specification and plan still call some completed seams unbuilt and cite removed paths.
Current code is authoritative for implementation status. Conversely, the completed HTML review does
not establish native backend, persistence, network, cryptographic or authoring behavior.

Sixth-checkpoint inventory correction: `src/doc.cpp` owns `pageEntries`/`pageIndexFilade` for all
shaped pages, distinct from sparse GPU-built `pages`. `reflowFrom()` previously updated only built
pages and left appended pagination absent from the index. The repaired splice retains unchanged
suffix metadata, invalidates same-count byte-length changes, preserves null GPU slots and makes new
exact endpoints available to the existing priority builder. The fix and its regressions stay in the
generic library; no Xuzz model or plain-editor dependency changes.

## Dependency map and package order

1. D1/H0 exact byte references and independent selection return checkpoints. Reuse existing Link
   navigation and activity. Land an opt-in native quiet/expanded investigation surface with member,
   occurrence and exact identity lists. No visited-source operations on select, inspect or fold.
1. Audit existing publication services and pin lifecycle, then telescope acquisition and separate
   retain/enable/emphasis controls. J17/J18/J23 precede acquisition-dependent search acceptance.
1. D2/H0 branch/frame enumeration; D3/H4 asynchronous parent-state effects; D4/H5 versioned
   occurrence search with declared rules, units, coverage, stale ownership and cancellation. Connect
   comparison and query lists only when those services exist. J19-J21/J30.
1. D5/H6 typed durable references, exact duplicate policy and inventory access. Explicit type
   dispatch precedes reusable forge endpoints and recipes/result snapshots. J26/J32.
1. Destination/base/author preflight, private staging, validation, receipt publication and stable
   attempt status. Establish backend guarantees before forge/recovery; unknown outcomes block blind
   retry. J27/J28 and D3/H7 for J31.
1. Concrete media locators/clocks and supported seek capabilities precede J29 cue controls. Saved
   viewing cues stay separate from authored Links.
1. S1/H1/H2 coiled retained layout and equivalent lists over verified D2 records; S2 query
   ownership, bounded visible preparation and measurements; S3 camera continuity and
   picking/material spikes. Physical tablet wings/occurrence stacks use L6/L7/U4a/U3; scenes/panes
   use U4b at M8.

M0-M6 first release remains unchanged: relocation, spikes, spine, packs, wheel, pages, selector.
Track H remains a follow-on with H0-H7 service and density gates. This native overlay increment is a
prerequisite-backed extension of the existing presentation, not completion of M2 or a competing view
registry/host. No cutover until equivalent live legacy journeys, performance and backend frame
reviews pass.

## First increment journey-to-command map

| Task subset                                       | Native command boundary                                                                                               | Evidence required                                                                                                                                                |
| ------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| J22 reading context and fold/expand subset        | Investigation presentation toggle; existing `std:xuzz/link_controls` and paging; no navigation dispatch for folding   | Quiet, expanded and refolded frames; same caret, page bounds, state, visit and both cursors. Search/comparison/layer/pouch integration is later work.            |
| J24 complete Link and independent browsing subset | `SelectLink`, `SelectMember`, `SelectOccurrence`, `Cross`; existing default chords and matching visible list controls | Authored 2-by-3 Link, gaps and repeated occurrences; both full counts; exact authority/state; no preview visits.                                                 |
| J24 exact Enter/Return and unavailable subset     | `Enter`, `ReturnToOrigin`, `CancelEntry`, existing document/cell entry handlers                                       | Actual target span and distant neighborhood; one completed visit on successful Enter, none on unavailable/cancel; preserved identity and ordinal.                |
| J25 branching restoration subset                  | `ActivityBack`, `ActivityForward`, `EnterVisit`; Walks preview/reference/annotation controls                          | Separate current/preview, actual child choices and reopened activity/source dumps. Media position and remote availability require later prerequisites.           |
| J1-J16 preservation                               | Existing editor, slice, REPL, publication, media and navigation interfaces                                            | Owning tests, full headless gate, relevant native replays and three independent Luna reports. A green replay does not establish an unaffordable or missing step. |

## Validation gates

- Build: `make -j$(nproc)` with all submodules initialized. Host lacks development dependencies;
  disposable Debian trixie Docker builder and runner are in `/workspace/spatial-build`. Keep proxy
  defaults and read-only CA trust; use the managed local daemon. All native runs set the six
  headless/XDG variables, with independent run directories.
- First capture the pinned baseline before product edits. Run owning engine/UI tests for identity,
  selection, return and persistence, then the full `make test` gate. Record any rootless swarm/veth
  restriction separately from gtest regressions.
- UI: actual controls/default chords, named accessible state, and the real headless input boundary.
  Retain before/after visited stores, permascroll and activity, frames and transcripts. Capture
  dense, narrow/enlarged text and reduced motion. Fix harness gaps before judging a task.
- Three Luna validators, maximum three concurrent child agents per skill, independent directories
  and rotated J17-J32 assignments. They edit no product code and distinguish native Pass, Fail,
  Missing, No affordance and Harness gap. Preserve J1-J16 with independent interface checks.
- Independent UX and visual review before and after implementation; initial notes are in
  `design/reviews/spatial-ui-first-increment/`. Reviewers inspect actual native/backend captures.
- After drawing changes: headless `xvfb-run ./tools/compare-backends.sh`; inspect frames, not exit
  codes alone. Run clang-format 19, `make format-check`, `make lint` and UI text policy checks.
- Commit only tested increments, push the feature branch, update this status with exact evidence,
  implemented steps, checks and remaining gaps. Do not report prototype success as a native pass.

## Fourth-checkpoint validation (2026-10-14)

- Final safe-margin build: `/workspace/spatial-build/fourth-build-margin.log`, exit 0. Source
  revision `c1ab2314e7bf31db67545aefd8a59f795a0d4756`, product diff SHA-256 at build time
  `17cfb9755b64a65a0829a38e813565303073da3a1b535161345e56950fdb5e06`, `build/xuzz` SHA-256
  `52dadabe6cfee1e5994a5e2e297ea35b6e9e6eef150f7f68012e9ff3a949cdf7`.
- Full UI suite: 233/233. Full library suite: 785/785, no skips; the retained-geometry tests for
  wide intermediate lines, interior-page clipping, and partial ligature fractions all pass. Full
  Xuzz suite: 99/99. Native runs used fresh XDG roots and Xvfb/x11 where Vulkan required it.
- Five independent safe-margin native cases pass: 800×600 on OpenGL/OpenGL ES/Vulkan, 1440×1000
  OpenGL, and 640×480 OpenGL. The first three show all eight source lines from x=30 above the
  chrome; the 1440 frame also shows the complete target; 640 shows a prefix, not the complete
  selection. Source operations remain byte-identical before and after each run.
- Final link-travel E2E passes on OpenGL, OpenGL ES and Vulkan. The two repaired drag E2Es pass on
  OpenGL (2/2). Their full logs and exact binary hashes are in
  [`fourth-increment validation`](../reviews/spatial-ui-fourth-increment/validation.md).
- Core backend renderer comparison passes: OpenGL/OpenGL ES have 0 differing pixels; Vulkan differs
  by 0.2471% (0.010 mean channel delta). It reused the unchanged core binary. Raw PPMs and private
  XDG profiles are outside Git under `/workspace/spatial-build/fourth-raw/`; lossless PNGs and their
  hashes are indexed in the validation document.
- Current incremental resize measurement is 854.6 ns/update in the O0 test build, below the 1000 ns
  test threshold. This is a test-build result, not optimized-build performance acceptance. Preserve
  the earlier 4625 ns O0 measurement as historical evidence; no performance budget is accepted.
- Open gates remain: native AT-SPI, an accessible enlarged-text control and enlarged-text captures,
  multipage/cell/history endpoints, unavailable/cancel/reopen variants, whole-page ink threshold and
  the complete 16+16 journey set. Do not promote bounded cases to full J24/J25 or J1–J16 acceptance.

## Earlier validation checkpoint (2026-10-14)

- Owning checks: 13 PDF/SVG library checks, 4 ReadingPlace checks, 49 exact-reference/navigation/
  panel/activity checks and 34 native panel/package UI checks pass (100 total).

- Native evidence and independent reviews live in `design/reviews/spatial-ui-first-increment/`.
  Three Luna reports distinguish actual interface subsets from missing services, unexercised
  variants and stale replay targets. No exit-only replay is promoted to a journey pass.

- Final same-document anchor: `evidence/luna-1/anchor-final/`. A UI-authored two-left/three-right
  Link includes a quoted repeated occurrence. Quiet/expanded/folded preserve exact state `a34`,
  range `0–20`, caret/selection and visit 1; independent visual review confirms unchanged source
  pixels. The paired `evidence/luna-1/no-write/` run has identical source dumps and no added
  completed visit. A second reopened check in `evidence/inspection-invariants/` preserves both
  source spool hashes and the two existing visit records through member/occurrence preview and fold.
  Activity metadata checkpoints are expected writes to the private reader store. Earlier blank
  source frames and clipped preview headers are retained as diagnostic limitations.

- Full native Xuzz suite: 99/99. First-checkpoint native UI suite: 232/233; its object-manager
  layout failure is now repaired and the second-checkpoint suite passes 233/233. Full library suite:
  781/782 under Xvfb; unchanged O0 incremental-resize benchmark exceeds its 1000 ns threshold (4625
  ns measured). These are open gates, not a full-suite pass. Initial offscreen suite cannot create
  Vulkan windows; use Xvfb with `-noreset` to retain the server across repeated SDL initialization.
  Do not treat the offscreen driver limitation as a product failure.

- AccessKit is absent from this build and `dbus-run-session` is absent from the runner. Internal
  accessible-tree/actions and actual keyboard/pointer inputs are exercised; native AT-SPI parity
  remains blocked and no assistive-technology session is claimed. O0 behavior validation provides no
  performance-budget acceptance.

- Store suite: 1325/1327 initially outside E2E/network suites; unchanged O0 memoization throughput
  benchmark fails. Generated sample permascroll changes were restored and its purity check passes.
  Broad backend E2E extension: 32/36 OpenGL checks pass; whole-page text, object creation and two
  drag cases fail, preventing the other backend E2E runs. All four now reproduce on the pinned
  baseline; the drawer creation case is repaired. Renderer core comparisons and separate new
  inspector frames pass on OpenGL/OpenGL ES/Vulkan.

- Network namespace tests skip in this runner. Format check passes. Lint has the same 37 pinned
  Markdown diagnostics and no new touched-document failures; other lint checks pass.

- Full evidence/gate details: [validation](../reviews/spatial-ui-first-increment/validation.md). Do
  not describe the complete test gate or J1–J16 preservation as passed.

## Exact next task

Resolve the strict core glyph-capture equality finding against the frozen-library control without
weakening its criteria. Then validate actual enlarged document text through existing layout
tunables; `ui.fontScale` has a governed native form and changes chrome only. The sixth checkpoint
repairs authored multipage readiness and validates visible later-page continuation. Extend J24 to
repeated occurrences, unavailable/pending cancellation and stale target replacement, then
cell/history endpoints. Extend J25 to branching Back/Return and same-profile reopen with exact
selected state/ranges and activity snapshots. Preserve per-backend native captures and exact
source-operation evidence. Bounded entry, Origin and direct Back do not complete J24/J25 or
establish full J1–J16 preservation.

Investigate the whole-page ink threshold without weakening it, and resolve legacy J3, J11 and J15
findings against the pinned baseline. The two stale drag tests are repaired; the native UI
font-scale control and enlarged-chrome captures are implemented. Native AT-SPI, actual enlarged
document text, optimized performance acceptance and the full 16+16 journey set remain open. Audit
changed-region metadata shaping and reflow costs in an optimized build; the sixth repair keeps
sparse GPU pages but shapes changed pages separately for metadata and GPU rebuild.

The next spatial service package is the publication/pin-lifecycle audit followed by D2 branch/frame
enumeration, per the dependency map above. Do not substitute raw primedia search for historical
occurrence coverage, or add physical tablets/another camera before L6/L7/U4a and view-host gates.
