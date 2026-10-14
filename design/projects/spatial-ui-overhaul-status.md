# Spatial UI overhaul implementation status

Baseline: `e9726a6b3825e794ab86db6cd54f18d9258e75c0` on `design/spatial-ui-overhaul-2026-10-13`.
Implementation branch: `feat/spatial-ui-overhaul`. Started 2026-10-13. This file is the continuation
checkpoint; update it in every implementation commit. Do not merge or deploy from this project
session.

## Current checkpoint

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

## Validation checkpoint (2026-10-14)

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

- Full native Xuzz suite: 99/99. Full native UI suite: 232/233, with an isolated object-manager
  layout failure (`FittedRowsShareSafeGeometryAndFullLabels`) outside modified panel code; its
  pinned-baseline classification remains unresolved. Full library suite: 781/782 under Xvfb;
  unchanged O0 incremental-resize benchmark exceeds its 1000 ns threshold (4625 ns measured). These
  are open gates, not a full-suite pass. Initial offscreen suite cannot create Vulkan windows; use
  Xvfb with `-noreset` to retain the server across repeated SDL initialization. Do not treat the
  offscreen driver limitation as a product failure.

- AccessKit is absent from this build and `dbus-run-session` is absent from the runner. Internal
  accessible-tree/actions and actual keyboard/pointer inputs are exercised; native AT-SPI parity
  remains blocked and no assistive-technology session is claimed. O0 behavior validation provides no
  performance-budget acceptance.

- Store suite: 1325/1327 initially outside E2E/network suites; unchanged O0 memoization throughput
  benchmark fails. Generated sample permascroll changes were restored and its purity check passes.
  Broad backend E2E extension: 32/36 OpenGL checks pass; whole-page text, object creation and two
  drag cases fail, preventing the other backend E2E runs. Baseline classification is unresolved.
  Renderer core comparisons and separate new inspector frames pass on OpenGL/OpenGL ES/Vulkan.

- Network namespace tests skip in this runner. Format check passes. Lint has the same 37 pinned
  Markdown diagnostics and no new touched-document failures; other lint checks pass.

- Full evidence/gate details: [validation](../reviews/spatial-ui-first-increment/validation.md). Do
  not describe the complete test gate or J1–J16 preservation as passed.

## Exact next task

After this tested checkpoint, establish full J24 source/activity invariants for distant cell and
historical document endpoints, unavailable/cancel and repeated occurrences, then J25 branching
Back/Return/reopen. Keep native anchor captures at narrow/enlarged text and all backends. Resolve
legacy J3 pouch insertion, J11 stale drag authoring and J15 stale version-selection replay findings
against the pinned baseline before calling them regressions. Preserve the three Luna native reports
and open AT/performance gates.

The next spatial service package is the publication/pin-lifecycle audit followed by D2 branch/frame
enumeration, per the dependency map above. Do not substitute raw primedia search for historical
occurrence coverage, or add physical tablets/another camera before L6/L7/U4a and view-host gates.
