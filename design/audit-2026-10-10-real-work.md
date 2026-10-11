# Real work validation — 10 October 2026

Status: fresh interface runs and findings are consolidated. The full J1–J16 contract does not pass.
Native editing, secondary-store links, visibility, resumed context and report composition remain
blocked by the product findings below.

## Baseline findings and fixes

The remaining beam-report checks first exercised native prepared fixtures. These are rendering and
navigation diagnostics, not UI authoring passes. Source hashes remained unchanged.

| Case                           | Observed result                                                                              | Evidence                                                             |
| ------------------------------ | -------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| Five overlapping owners        | Keyboard reaches all five; pointer hits the exposed owner, without an obscured-owner control | `build/ux-full-20261010/stress-dense/overlap5/`                      |
| Ten reversed routes            | All ten owner identities cycle and wrap; native frame is visually saturated                  | `build/ux-full-20261010/stress-dense/crisscross10/`                  |
| 100 links                      | 320 resolved strands; median steady software OpenGL frame 5627.910 ms                        | `build/ux-full-20261010/stress-dense/dense100/benchmark/`            |
| 200 links                      | 640 resolved strands; median steady software OpenGL frame 11455.342 ms                       | `build/ux-full-20261010/stress-dense/dense200/benchmark/`            |
| Native accessibility readiness | Current binary explicitly omits accessibility; internal dumps are not native actions         | `build/ux-full-20261010/harness-inventory/native-smoke-correct-cwd/` |

The initial dense100 cycle was stopped after 149 seconds. It does not establish traversal of all 100
owners. These software-driver measurements say nothing about hardware frame rates.

Implementation adds visible Previous link and Next link controls through the existing navigation
boundary, keeps the explicit link hue on page-to-cell anchors, retains a document's placed basis
during translation, and supplies a live per-draw document paper/ink palette to the shared shader.
Native modifier holds are represented by `--hold-mods`, using the ordinary pointer gesture path.
These changes still require final-build tests and native acceptance captures.

Retaining prepared beam geometry reduced median steady software frames to 8.329 ms for ten links,
12.244 ms for 100 and 20.385 ms for 200. These runs preserve all resolved relations; selection still
triggered a 5535.118 ms preparation in the 100-link fixture. A separate page geometry fix is in
progress because anchor queries repeatedly request text shaping. The cached steady-frame result does
not establish responsive selection.

The native-enabled build now loads AccessKit, but its first smoke aborted while publishing an empty
TextRun. The native consumer requires an explicit empty value, which the adapter omitted. The
adapter mapping has been corrected for text runs and editable text; native verification is pending.
No native action, editable-text or modifier-drag pass is claimed from the aborted runs. Evidence:
`build/ux-full-20261010/harness-inventory/native-enabled-readiness.md`.

The retained-caret build passed 74 of 75 focused library tests, all three document-palette backend
checks, 17 navigation/anchor/gutter tests, eight LinkPanelOverlay tests and 24 system-document
configuration tests. The remaining test failure was an invalid RTL fixture precondition: native page
layout guesses one direction for the page, so Latin-first mixed content does not establish reversed
Hebrew placement. A separate Hebrew-first fixture preserves that assertion. Mixed-script run
resolution remains a preexisting limitation, separate from the caret cache.

The next dense100 diagnostic still measured 5684.365 ms for selected-owner preparation, with 14.989
ms median steady frames and zero steady shaping. A child-owned debugger sample traced the render
thread through libmagic, `classifyRun`, `Session::mediaSpansFor` and the endpoint media resolver.
The resolver repeatedly classified the same 11000-byte plain-text run. Evidence is
`build/ux-full-20261010/stress-dense/dense100/gdb-final/run.log`; this is one sampled stack, not a
statistical profile.

The cache fix retains Session-owned classifications only for complete, unsegmented local pieces.
Hits recheck store identity, operation count, session generation, physical local bounds and segment
metadata. Imported, external, incomplete and segment-backed content is deliberately recomputed;
remote arrival must not be hidden by a cached negative classification. Owning vector returns avoid
references dangling across invalidation. This change is awaiting rebuild, regression checks and
fresh performance measurements.

## Fresh resumption checks — 11 October 2026

The eleven-action constrained panel now uses measured label widths to add columns only when vertical
space requires them. The 1280-pixel enlarged-text case fits below its protected reading band. At
640×480 and font scale 2, mandatory content needs 408 pixels against a 370-pixel floating budget;
the regression follows the ordinary dock/paging fallback and checks every action, command identity,
exact byte range, pinned cursor and contained bound across its pages. It does not render the
rejected provisional floating layout as though it were the overlay's final presentation.

Fresh native-enabled build: `build/ux-full-20261010/build-resume-final.log`. Fresh focused checks
all pass:

- 27 panel presentation, overlay and media cache tests:
  `build/ux-full-20261010/panel-paging-tests.log`.
- 81 library tests, including retained caret/RTL, document palette backends, automation,
  accessibility and render regions: `build/ux-full-20261010/resume-lib-tests.log`.
- 40 link navigation, panel, gutter and anchor tests:
  `build/ux-full-20261010/resume-xuzz-tests.log`.
- 24 system settings tests: `build/ux-full-20261010/resume-xudu-tests.log`.
- Formatting and lint: `build/ux-full-20261010/gates-panel-resume.log`.

These checks supersede the earlier failing unit-test status only. Backend comparison, fresh dense
measurements, native readiness and J16/J1–J15 results remain pending until their runs finish.

## Frozen-build diagnostic results — 11 October 2026

The final caption correction accounts for font-role padding: a caption button can have a larger line
height than the panel's configured label font. Horizontal decoration now fits around the measured
complete captions. Thirty panel/overlay/media-cache checks pass in
`build/ux-full-20261010/caption-role-padding-tests.log`; formatting and lint pass in
`gates-caption-role-padding.log`. Build: `build-caption-role-padding.log`.

The isolated software OpenGL measurements preserve all source store and permascroll hashes:

| Case                            | Median steady frame | Other evidence                                          |
| ------------------------------- | ------------------- | ------------------------------------------------------- |
| Ten reversed routes             | 6.949 ms            | Zero steady shaping                                     |
| 100 links, 320 authored strands | 12.258 ms           | First crossing 869.10 ms                                |
| 200 links, 640 authored strands | 15.033 ms           | First crossing 908.76 ms                                |
| Selected 100-link owner         | 13.098 ms           | Selected preparation 1.371 ms; first crossing 847.45 ms |

The selected preparation meets the proposed below-100-ms diagnostic threshold, versus the earlier
5684.365 ms. These are software-driver measurements, not hardware frame-rate promises. Every steady
sample reports zero render-thread layout, HarfBuzz, fallback and input bytes. Exact commands, hashes
and logs: `stress-dense/resume-isolated-plan.json`, `resume-isolated-results.json`, and each
`*-media-r2-isolated/` directory below `build/ux-full-20261010/stress-dense/`.

The final 100-owner default-key cycle completes in 12.698 seconds, retains 100 distinct identities
and wraps; the pointer overlap case exercises independent occurrence choices, Cross and both owner
controls. Complete captions pass independent frame review. Evidence: `dense100/cycle-media-r3/` and
`overlap5/pointer-media-r3/`. These prepared fixtures establish navigation diagnostics, never UI
authoring passes.

Consecutive settled `*-media-record-r4/` frames have identical pixel hashes within each case and
contain readable body text and full controls. An earlier `selection-media-r3/selected.png` genuinely
has no body glyph pixels in its text crop, while subsequent fresh frames contain them. This remains
an intermittent rendering finding. Apparent entirely black frames were image-display artifacts: the
saved files contain nonblack pixels; that finding is withdrawn.

Eight placed-page cases pass bounded geometry review on OpenGL, GLES and Vulkan: rotated bases,
depth, foreground occlusion, one-visible routes, dark paper and enlarged type. All 24 processes exit
successfully, keep fixture hashes unchanged and report zero steady shaping. Evidence is
`stress-dense/component-final/` and `resume-component-backends-results.json`. Limitations remain:
120-degree framing clips content, curves can appear faceted, selected contours include blank margin,
and the enlarged dark case shows a collapsed Controls dock. These do not establish UI rotation
controls or complete mixed-endpoint journeys.

Full backend comparison, including binary orchestration, passes in
`build/ux-full-20261010/backend-resume/run.log`. The short render sample still cannot exercise
threaded page splitting. Native AT-SPI finds the exact application process without the previous
empty-text panic. Initial client discovery saw no children, but direct settled D-Bus queries confirm
publication of the window and controls. The initial plan incorrectly waited for an editable
custom-path field before selecting the Custom path option. Corrected native Document and Custom-path
option Actions succeed; the chosen field still exposes no Text/EditableText interface. Its source
supplies value/SetValue but no text-run children. This is a product accessibility finding, separate
from the corrected discovery/plan gaps. Evidence:
`native-j16-resume-20261011-rildrjme/empty-corrected-direct/`. No native editable-text pass is
claimed.

## Audit rules and evidence

Runs use isolated data, configuration and cache directories, headless software rendering, and real
pointer/keymap/native accessibility or terminal prompt input. Prepared structure scripts, unit tests
and accessibility-labelled pointer clicks do not establish UI authoring or native accessibility
passes. A missing affordance is reported at the exact attempted step.

The harness inventory is `build/ux-full-20261010/harness-inventory/inventory.md`. Reusable clients
are `tools/ux-atspi-client.py` and `tools/ux-pty-client.py`. The local Unix AccessKit build is
available for the frozen native-enabled build. J16 starts first, then independent runners cover
remaining journeys in parallel, with closed UI-authored stores handed to terminal consumers.

## Journey results — frozen interface runs

Every journey was investigated, with J16 authoring started first and independent graphical and
terminal runners. The runners exhausted their usage allocation before finishing their reports; the
lead recovered their artifacts and performed the affected handoffs and remaining J14/J15 runs. There
is no all-pass result. The table distinguishes an executed step from a whole-journey verdict.
Evidence paths below are relative to `build/ux-full-20261010/`.

| Journey | Executed step                                                                 | Affordance                                                    | Outcome                                                                     | Evidence                                                                                                               |
| ------- | ----------------------------------------------------------------------------- | ------------------------------------------------------------- | --------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| J1      | Start empty, create by key and type/save                                      | No-argument launch, Ctrl+N, typing, save                      | Pass                                                                        | `graphical-run/start.log`, `j1-key-save.log`, `j1-key-save.ppm`                                                        |
| J1      | Create through the radial submenu                                             | New and Open → New xanadoc                                    | Fail: typing remains in default document                                    | `graphical-run/j1-form.log`, `new-menu.ppm`                                                                            |
| J2      | Create slice, name and connect alpha/beta                                     | Ctrl+Alt+N, N, E, Return                                      | Pass                                                                        | `graphical-run/j2-author.log`, `j2-alpha-beta.ppm`                                                                     |
| J2      | Create through the radial submenu                                             | New and Open → New slice                                      | Fail: no slice; typed names become document text                            | `j2-menu/application.log`, `cells.ppm`                                                                                 |
| J3      | Select and drag into empty space                                              | Native pointer press/move/release                             | Pass: new Harbor quotation and gold tether                                  | `graphical-run/j3-gestures.log`, `j3-void-drop.ppm`                                                                    |
| J3      | Store, close/reopen and reuse pouch quotation                                 | Pointer pickup before drawer opens; zone drop; zone ›; Insert | Pass for reuse through the visible Insert action                            | `j3-pouch-rerun/settled-plan.json`, `reopened-settled.ppm`, `reuse-button-plan.json`, `reused.ppm`                     |
| J4      | Return from home with caret mid-document                                      | Alt+Home, F6                                                  | Pass: caret 7 remains 7                                                     | `j4-keyboard/application.log`, `before.ppm`, `home.ppm`, `returned.ppm`                                                |
| J5      | Relaunch saved document/cells, close and reach Open                           | No arguments, Ctrl+W, Ctrl+O                                  | Pass for these bounded steps                                                | `graphical-run/j5-resume.log`, `j5-relaunch.ppm`, `j5-key-close.ppm`, `j5-open-form.ppm`                               |
| J6      | Inspect, refine and save authored text                                        | VQuery prompts: rank, predicate, :save                        | Pass after registry fix                                                     | `terminal-resume/j6-save/transcript.txt`, `query-result/`                                                              |
| J6      | Open and inspect the saved alpha cell                                         | Ctrl+O, custom-path form, actual tab pointer, F6              | Pass                                                                        | `j6-result-ui/open-chord-plan.json`, `cells-pointer-plan.json`, `result-cells.ppm`                                     |
| J7      | Author typed 12/8/5 rank                                                      | N/E/Return in Xuzz                                            | Pass                                                                        | `graphical-run/j7-author.log`, `j7-budget.ppm`                                                                         |
| J7      | Inspect, total 25, revise artwork and total 27; recover from error            | VPL :open/:rank, array expression, reduction, :grid           | Pass                                                                        | `terminal-resume/j7/j7-transcript.txt`, `j7revise-transcript.txt`                                                      |
| J8      | Save/reopen revised amounts and recompute 27                                  | VPL :save/:open, reduction                                    | Pass                                                                        | `terminal-resume/j7/j8revise-transcript.txt`, `revised-data/`                                                          |
| J8      | Inspect saved typed amounts alongside original                                | Ctrl+O, result tab, F6                                        | Pass: 12/10/5; original remains 12/8/5                                      | `j8-result-ui/result-cells.ppm`, `cells-pointer-plan.json`                                                             |
| J8      | Compose amounts and total into one report                                     | `report ← data , total` at VPL prompt                         | Fail: comma rejected; :save would retain previous scalar                    | `terminal-resume/j7/j8combined-transcript.txt`                                                                         |
| J9      | Assert facts/rule, negative answer, add approval and enumerate both solutions | Actual Prolog prompts and semicolon continuation              | Pass                                                                        | `terminal-resume/j9/transcript.txt`                                                                                    |
| J9      | Save, quit, consult/list and decide again                                     | :save, halt, consult, listing, query                          | Pass                                                                        | `terminal-resume/j9/reopened.txt`, `issue_rules.pl`                                                                    |
| J10     | Repeated Pages/Slices/Both visibility changes                                 | Ctrl+Alt+1/2/3                                                | Fail: Slices-only still draws page and omits visible cells                  | `graphical-run/j10-j12.log`, `j10-*-body-crop.png`                                                                     |
| J11     | Author one passage → two distant cell members                                 | Ctrl+L, Alt+Shift+A/F                                         | Pass: one identity 105, members 7 and 86                                    | `native-j16-resume-20261011-rildrjme/j11-authoring/`                                                                   |
| J11     | Choose exact A and B occurrences, enter/return and see both futures           | Member/place/Cross/Enter/Origin; Activity Forward             | Pass for live branching navigation; visited files unchanged                 | `j11-explicit-choices/chosen-A.ppm`, `entered-A.ppm`, `entered-B.ppm`, `forward-branches.ppm`, `source-integrity.json` |
| J11     | Repeat both choices after reopening                                           | Store-path and subsequent no-argument launches; Back/Forward  | Fail to establish the reopened B choice; closed-version origin appears      | `j11-explicit-choices/reopened-chosen-B.ppm`, `resume-futures.ppm`, `resume-B.ppm`                                     |
| J12     | Quote distinctive cell into document and another cell                         | Ctrl+Alt+T, Alt+Shift+T                                       | Pass for shared content                                                     | `graphical-run/j12-fresh.log`, `j12-fresh-shared.ppm`                                                                  |
| J12     | Return through saved cell provenance                                          | Pouch Open source action                                      | Fail: explicit unavailable message                                          | `j12-return/application.log`, `source-return.ppm`                                                                      |
| J13     | Author issue/gate clauses; query/save 11 rows                                 | Native cell editing; VQuery rank predicates and :save         | Pass after registry fix and proper fact authoring                           | `j13-facts-author/plan.json`, `j13-facts-query/transcript.txt`, `facts/`                                               |
| J13     | Consume dataset, decide and export 1/1/0                                      | Prolog :open, rule assertion, query continuations, :export    | Pass: alpha/beta ready, gamma false; three exported solutions               | `j13-facts-prolog/transcript.txt`, `flags/`                                                                            |
| J13     | Consume flags and calculate ready/unready/total                               | VPL :open, reduction and tally                                | Pass: 2, 1, 3 derived from exported data                                    | `j13-facts-vpl/transcript.txt`                                                                                         |
| J13     | Assemble and save final report                                                | `report ← ready , unready , total`                            | Fail: comma rejected                                                        | Same transcript                                                                                                        |
| J14     | Create None/Int64/Double/Bool cells                                           | N/E/Return with ordinary text, 12, 3.5, true                  | Pass before map navigation                                                  | `lead-j14/application.log`, `ops-before.txt`, four captures                                                            |
| J14     | Select birth #25 and annotate that operation                                  | Map node, Annotate operation, form                            | Pass for target identity; Fail for retaining full rank state                | `j14-handle-target25/handle.ppm`; handle #38 names #25, but Bool edit #29 is outside selected version                  |
| J14     | Reference foreign birth and inspect after its content changes                 | Ctrl+Alt+R, choice, edit foreign cell, return via tabs        | Pass: ExternRef #47 retains operation 6 and resolves later text             | `j14-handle-target25/foreign-reference.ppm`, `foreign-edited.ppm`, `reference-after-change.ppm`                        |
| J15     | Interleave text and slice edits on intended lineage                           | F6 and native editing                                         | Fail: document and slice retain different parents, making sibling histories | `lead-j15/ops-A.txt`, `ancestor.ppm`, `branch-A.ppm`                                                                   |
| J15     | Select three versions, inspect ancestry and open comparison                   | Map controls, modified node selection, Comparison, Open in 3D | Pass for 0/a1a2/a15 comparison; not full branch/change-site acceptance      | `j15-comparison/three-plan.json`, `three-comparison.ppm`, `three-views.ppm`                                            |
| J16     | Author two left/three right spans and long rank                               | Bench keys, genuine Alt-cell pickup/drop, Forge               | Pass for authored shape; cells 8/52/92                                      | `lead-j16-author-r3/plan.json`, `drop-C.ppm`, `ops.txt`                                                                |
| J16     | Select authored link in normal multi-store session                            | Default Next-link key after activating source tab             | Fail: secondary-store link absent from candidates                           | `j16-navigation/settled-plan.json`, `identity.ppm`                                                                     |
| J16     | Select owning store on restart, add overlapping link, explore both cursors    | Store-path launch, Ctrl+L, member/place/Cross keys            | Pass for bounded primary-store workflow: identities 111 and 127             | `j16-primary-store/author-plan.json`, `authored-overlap.ppm`, `left-first.ppm`, `right-A.ppm`, `right-C.ppm`           |
| J16     | Enter A and distant C, create two futures                                     | Enter/Origin, rank/projection controls, Activity Forward      | Pass for cell entry and two futures; visited files unchanged                | `j16-primary-store/entered-A.ppm`, `entered-C-projection.ppm`, `two-futures.ppm`, `navigation-integrity.json`          |
| J16     | Reopen and restore both cell futures                                          | No-argument launch, Forward, Back, choose other child         | Pass for A/C restoration                                                    | `j16-primary-store/reopened-futures.ppm`, `reopened-A.ppm`, `reopened-C.ppm`                                           |
| J16     | Reverse into chosen document, reselect second identity after restoration      | Origin/Cross/member/Enter, owner keys                         | Fail in this resumed sequence: focus remains C; link context absent         | `j16-primary-store/document-entry.ppm`, `second-owner.ppm`                                                             |
| J16     | Walks preview/reference/annotation                                            | No visible control or default binding found                   | No affordance                                                               | Native command inventory and source search; earlier `walks-attempt.ppm`                                                |
| J16     | Author unavailable target, request it and cancel                              | No ordinary unavailable-target authoring route found          | No affordance for this audit setup                                          | `j16-primary-store/cancel-without-target.ppm` is cancellation without a pending target, not a pass                     |

A full keyboard/native-accessibility repeat is not established. Native menu Actions succeed, but the
Custom path field lacks Text/EditableText, blocking the first native editing gate. Pointer scripts
and internal trees are not substitutes for native actions. Keyboard J4 and terminal runs are
separately evidenced; the entire passing graphical subset is not labelled accessibility-complete.

Further steps not validated: J4's pointer and selected-link round trip; J5's complete caret/camera,
pouch and selected-link restoration; J12's surrounding edits and three-location reopen; J14's six
kinds on one intact rank after reopen; J15's two branches each containing both edit kinds and every
reopened change-site target; J16's pointer candidate choice, every repeated document occurrence,
Walks notes/references and a genuinely pending unavailable endpoint. Failures and absent affordances
above prevent full acceptance; these omissions are not passes inferred from unit tests.

## Cross-program handoffs and source identity

The final VQuery fix removes implicit nearby-permascroll guessing. A native path under
`xudu/xanadocs/` previously selected the bare `xudu/permascroll` registry container. Without an
explicit override, it now uses `PermascrollRegistry::defaultUser()`, matching Xuzz and VPL. The
native-enabled all-program build passes in `build-vquery-registry-fix.log`. Fresh PTY J6 and J13
runs use ordinary store paths and copied author registries, without a permascroll CLI escape.

Original failed J6/J13 transcripts remain under `terminal-resume/j6/` and `j13/`. J13's original
bare gate labels are also unsuitable Prolog clauses. Its successful retry authors complete facts
through Xuzz, queries and saves all 11 rows, and consumes that artifact directly in Prolog. No
intermediate rows are retyped. Prolog exports numeric flags 1/1/0; VPL consumes those flags and
computes 2/1/3. Final report composition is still rejected. Saving after an expression error must
not be described as saving the failed expression.

VQuery accepts literal cell ID `6`; `#6` silently evaluates its `#` home prefix and ignores the
remaining token. The original runner's query therefore compounded the scroll defect. Corrected
queries return alpha/beta and three issue heads with their actual gate ranks. Reject trailing tokens
at the parser boundary and show accepted anchor syntax in REPL help.

J11 and J16 navigation hash comparisons cover their exact visited stores. Authoring new links and
quotations necessarily appends operations; that phase is excluded from the navigation comparison. No
claim is made that every UI-authoring profile stayed byte-identical. Saved activity futures are
private activity-store records, not operations added to the visited store.

## Findings in priority order

1. **Links in secondary stores disappear from reader candidates.** A newly authored 2×3 clasp in the
   normal multi-store session cannot be selected; opening its owning store as primary restores it.
   `LinkBeams` and `LinkContext` still consult `session.store().links()`. Compose candidates from
   all loaded stores, carry authoritative LinkKey throughout presentation, and rerun J16 from
   ordinary no-argument startup.
1. **Visibility and lineage diverge between document and slice.** Slices-only draws a page and omits
   its visible cell neighborhood. F6 editing resumes a stale document parent while slice edits
   resume their own head. Fix the document drawing gate in ViewCoordinator/render integration and
   reconcile the presented version through the existing view/bridge boundaries; do not mint
   navigation operations. Rerun J10/J15 with text and cell edits on explicitly selected parents.
1. **Native editable fields have no text interface.** Form metadata has value/SetValue but no child
   text runs or selection metadata. Share the document adapter's text metadata path with Form and
   overlay inputs, preserving password masking. Repeat the actual AT-SPI empty/newline/readback
   gates before claiming native editing.
1. **Radial New entries do not perform their advertised actions.** Native GPU picks reach New
   xanadoc/New slice, but typing continues in the default document. Trace the radial action dispatch
   into the Xuzz command table and rerun both menu paths; keyboard slice creation already works.
1. **Resumed activity does not preserve all link context.** Live A/B and A/C futures exist; A/C
   restore after reopen, but reverse document entry and owner selection subsequently remain at C.
   J11's reopened B choice is also unproven. Fix restoration of link key, side, member, occurrence
   and origin together at the activity/LinkContext boundary, then rerun both branch choices and
   reversal from reopened state.
1. **Annotation selection changes the editing parent.** Selecting birth #25 moves to version25;
   Bool's later edit #29 is absent when the handle is placed. Keep operation inspection separate
   from the current editing version, and place the selected target's handle on the current rank in
   HypertimeGraph/Xuzz's annotation handler. The ExternRef identity remains stable across a foreign
   cell edit, so that separate behavior passes.
1. **Result composition is incomplete.** VPL calculations and persistence work, but catenation is
   rejected for both budget and decision reports. Implement the documented array composition path in
   the VPL parser/evaluator and retain source identities for derived report cells. Prolog exported
   rows currently cite the aggregate input provenance, which is weaker than a per-solution
   contributing-fact trail; complete route-back inspection remains unvalidated.
1. **Cell provenance return and Walks operations are unavailable.** The pouch source action
   explicitly refuses saved cell return in `Views::swingBackToSpan`. Wire it through the cell
   navigation boundary. Walks preview/reference/annotation needs ordinary named controls; the
   activity Back/Forward interface alone does not satisfy J16.
1. **Reading polish remains limited.** Pouch/bench captions are heavily ellipsized at1440×1000; zone
   actions are paged through a small › label. A prior dense frame has absent body glyphs despite
   subsequent stable captures. Keep that rendering failure open and improve the drawer's measured
   captions/action discoverability. Do not reclassify the earlier disk-verified blank body as an
   image-display artifact.

No commit or push was made. All native runs have stopped before the final gates. Older baseline
pending statements above describe their original measurement stage; the fresh results and findings
in this section are the current audit outcome.
