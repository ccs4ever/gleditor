# Real work journey acceptance — 12 October 2026

The acceptance contract is `design/ux_workflow_real_work.md`, with the link navigation contract in
`design/ui_workflow_xuzz_navigation.md`. This record supersedes the outcomes in the 10 October
audit; failed attempts remain in their evidence directories. All J1–J16 journeys pass on the
validated build.

## Method and scope

The lead evaluated results and implemented fixes. Three Luna runners exercised documents, terminal
programs and structure journeys. No additional runners were started. Implementation/build phases and
interface runs were separated. Runs used offscreen rendering, software GL, dummy audio and isolated
data, configuration and cache directories. Native accessibility used AccessKit and AT-SPI.

Documents, slices, links and input data were authored through ordinary menus, bindings and pointer
operations. Terminal work used real PTYs and visible REPL commands. Complete closed profiles were
carried between programs; the author permascroll is under `data/xuzz/permascroll/default`. Launching
an existing store path did not count as no-argument session resumption. Store dumps, hashes and unit
tests corroborate interface behavior; they do not replace it.

AT-SPI supplies Text, Component focus and Action on this build. Editing used native focus followed
by ordinary keyboard input and Text readback. EditableText is unavailable and is not claimed.

Evidence root: `build/ux-green-20261011/`. Paths below are relative to that root. Plans, application
logs, native events/snapshots, frames, PTY transcripts and closed dumps are retained there.

## Journey results

| Journey | Result and evidence                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| J1      | Menu/native Action and key creation retain distinct document text. `harness-j1-j2`, `harness-j1-native-r2`.                                                                                                                                                                                                                                                                                                                                                  |
| J2      | Menu/native Action and key creation produce connected alpha/beta ranks. `harness-j1-j2`, `harness-j2-native`.                                                                                                                                                                                                                                                                                                                                                |
| J3      | Real pointer drag quotes Harbor with a gold tether; Meadow survives pouch close/reopen and insertion. Closed spans prove shared primedia. `harness-j3`.                                                                                                                                                                                                                                                                                                      |
| J4      | Keyboard Home/F6 preserves the selected link and origin; native cell Action owns keyboard editing, and F6 restores document caret 3. `harness-j4-j5`, `postfix-checks/j4-native-keyboard-r2`.                                                                                                                                                                                                                                                                |
| J5      | Menu Save and quit exits cleanly; same-profile no-argument reopen restores two documents, caret 3, focused cell 4, link 30 and Meadow. Saved and both resumed frames are pixel-identical. One resumed insertion at byte 3 survives another clean close and reopen, with caret 10. `postfix-checks/j5-camera-r6`; earlier menu/key close checks remain in `postfix-checks`.                                                                                   |
| J6      | Query, refinement, save and native GUI result readback pass on the fresh J2 slice. `terminal-prepared/j6-query`, `terminal-prepared/j6-result-native`.                                                                                                                                                                                                                                                                                                       |
| J7      | Inspect 12/8/5, total 25, revise to 12/10/5, total 27, inspect contributors and recover from an expression error. `terminal-prepared/j7-vpl`, `terminal-prepared/j7-original-native`.                                                                                                                                                                                                                                                                        |
| J8      | Independent typed report 12/10/5/27 retains original source identities; reopening and summing its first three values gives 27. Pointer/native comparison reaches both original and result. `terminal-prepared/j8-reopen`, `terminal-prepared/j8-compare-r3`, `terminal-prepared/j8-result-native`.                                                                                                                                                           |
| J9      | Facts, rules, negative answers, distinct solutions and seven saved clauses survive consult/reopen. `terminal-prepared/j9-prolog`, `terminal-prepared/j9-reopen`.                                                                                                                                                                                                                                                                                             |
| J10     | All 18 native presence assertions pass over three Xanadocs/Slices/Both cycles; document/slice store hashes remain unchanged. `postfix-checks/j10-native-r2`.                                                                                                                                                                                                                                                                                                 |
| J11     | One authored link has one left and two right members. Separate member/occurrence choices reach cells 8 and 52; both Forward futures survive no-argument reopen and native Action. Actual author-store hashes remain unchanged. `j11-author-r1`, `j11-final-r3`, `j11-native-reopen/acceptance.md`.                                                                                                                                                           |
| J12     | Three shared occurrences retain source addresses through surrounding edits, exact historical source return and reopening. Native Action restores source cell 6. `j12-complete-r4`, `j12-native-r4`.                                                                                                                                                                                                                                                          |
| J13     | Eleven queried facts retain exact original CellRefs; Prolog flags are 1/1/0; VPL report is 2/1/3 and recomputes 3 after reopen. Pointer/native source controls reach flags, facts and original cells. Original hashes remain unchanged. `terminal-prepared/j13-source-cell-results.md`.                                                                                                                                                                      |
| J14     | Ordered None/Int64/Double/Bool/OpHandle/ExternRef rank survives reopen. Handle 38 names Bool birth operation 25. ExternRef 46 names the foreign MakeCell 6 even after a later foreign SetValue. Main semantic operations remain unchanged by that foreign edit. `prepared-fix-checks-r1/j14`.                                                                                                                                                                |
| J15     | Base and both branch text changes, revised cell 17, added cells 24/37, membership and links agree before/after reopening. All versions are selected through map controls; comparison navigation preserves operations. Native snapshots corroborate text and focused cells. `j15-final-r5`, `prepared-fix-checks-r1/j15`.                                                                                                                                     |
| J16     | Pointer and native selection retain one overlapping identity; both authorities, every member/occurrence, distant rank/projection, reverse document entry and two reopened futures pass. Walks reference and Unicode/multiline note survive; native previews add no visit. Unavailable saved-target entry exposes Cancel; native cancellation retains the link and adds no visit. Actual visited store hashes are unchanged. See the detailed evidence below. |

J5's accepted comparison uses `postfix-checks/j5-camera-r6/savequit-baseline.png` against
`noarg-initial.png` and `noarg-settled.png`, decoded as PPM despite their extensions. Both
differences are empty. Native snapshots show two open documents, caret 3 and focused home cell 4.
Earlier files named `resume-*` show an already blank pre-navigation camera and are not the accepted
baseline. `reopened-after-probe-native.json` reads `HarJ5PROBEbor source passage.` and caret 10
after a clean keyboard quit; the companion text is unchanged. Native pouch closure uses its named
Close action. The modal drawer consumes its opener chord while open, so that chord was not counted
as dismissal.

J14 native cell Actions and names expose the complete typed content and foreign target. Its stores
contain slice cells, so their xanadoc Text entries are empty; failed attempts to read cell content
through those page entries are preserved and are not claimed as passes.

J3's initial helper overwrote its earlier authoring log on a later stage. The original frames,
closed shared-span proof and subsequent pouch insertion evidence remain; the lost log is not
presented as retained evidence. J15's native API exposes cells and focus but no separately named
link-endpoint relation. Link differences are corroborated by the ordinary comparison frames and
semantic operations rather than an invented native relation.

## Crowded-link evidence

`j16-author-r4` authors link 111 with two left and three right members, plus overlapping link 126.
The distant right members are cells 8, 52 and 92. Repeated spans appear in document and
registry-cell occurrences and remain separately selectable. `j16-final-r9` records all four/three
left occurrences and both occurrences of each right member, both directions, exact cell range 0–16,
rank movement, projection change, reverse document entry and source origin 0–19. The focused
neighborhood is no longer covered by endpoint preview cards. The link panel retains its full endset
counts and cursors.

`j16-primary-final-r11b` selects the actual overlapping beam with the pointer as the primary store,
then enters distant cell 92 and returns to the exact source. The earlier `--click-label` attempt
failed because a rendered link is not a bounded actionable control; the corrected pointer run uses
its visible beam geometry. `j16-complete-r7` retains the native link-selection Action and ordinary
Walks reference/annotation authoring. `j16-native-final-r10` reopens both previews with native
choices/focus and reads the retained reference and note: “Quoted future — café” followed by “Keep
both branches.” `j16-futures-final-r12` restores both saved futures through Walks and Activity
Forward with their identity, side, member, occurrence, document selection and cell focus intact.

`j16-cancel-final-r13` closes the quoted document, requests its saved visit, exposes explicit
unavailability and invokes native Cancel. Both snapshots retain link 111. The six semantic visit
operations are identical before/after cancellation (`visit-comparison.json`). Hashes in
`visited-after.json` match the original actual author-store paths in
`j16-final-r8/visited-before.json`; checking only copied profile stores would have missed where this
session actually saved documents.

## Fixes validated

- Renderer scripts wait for queued focus/document work while preserving live captures.
- Forms and overlays expose Unicode Text runs, caret/selection and a trailing empty newline.
- Document tabs and native cell activation bind the same exact view version and keyboard owner.
- Reader candidates and beams use loaded reader authorities and one stable link identity; hidden
  system configuration links and Format links do not enter the reader's candidate cycle.
- Saved reading places retain independent pinned link cursors and origin. Unavailable targets are
  refused before navigation dispatch, with explicit status and cancellation.
- Walks provides ordinary Preview, Enter, Reference and Annotate controls in the private activity
  store; completed visits preserve branching histories.
- Exact historical cell quotation return validates authority, version, cell and shared span.
- Paired document/slice commits advance together. Map annotation preserves its captured editing
  parent while the companion view follows the result; foreign insertion advances it too.
- Dimension bundles remain view-only and do not mint absent dimensions or steal document shortcuts.
- Endpoint preview cards hide while the actual slice owns keyboard focus, keeping its rank visible.
- VPL report composition preserves scalar types and source provenance. VQuery resolves authored base
  cells as well as federation proxies, retaining exact original CellRefs in saved facts.
- SIGPIPE is ignored so a closed native IPC connection returns an error instead of terminating the
  application before persistence. The exact former failing IPC write was not established.
- The native harness honors Text-content selectors and waits for graceful application exit after
  native Save and quit. Ambiguous titles, early snapshots and premature helper cleanup are treated
  as harness failures, not product passes.

## Build and checks

The native-enabled full build passes in `build-source-preview-paired-retry.log`. Its preceding
attempt records a test API typo; the test was corrected to use `Manifold::contentOf`.

Affected regressions pass: 25 arena-federation/reading-place tests in `tests-source-identity.log`,
seven world-presentation/reader UI tests in `tests-preview-paired.log`, and the preceding 119
navigation, metadata, federation, reading-place and visualizer checks in `tests-final-batch-*.log`.
The new presentation test verifies that hiding cards clears queued native actions and picking, and
that restoring them does not execute the stale action. The new provenance test distinguishes
base/proxy/quotation identities from constructed scratch content.

The first full test run passed all 787 library tests but stopped at two engine integration failures.
The grouped-link fixture used the retired `data/xudu/permascroll/default` path and was corrected to
`data/xuzz/permascroll/default`, retaining its assertions. A drag calibration capture was blank on
that run; two isolated reruns passed with both private and shared cache settings. No cache cause or
renderer fix is claimed. The original capture is retained as `drag-first-failure.png`.

The full rerun passed 787 library tests, 1,362 engine tests and 85 Xuzz tests in
`tests-all-retry.log`; the video-card screenshot test was skipped by the test itself. Both previous
integration failures passed. The slice suite passed 228 tests but found an outdated assumption in
`QuotationBuilderOverlayTest`: it required new accessibility-only TextRun children to have widget
layout boxes. The assertion now verifies their owner and retains every widget geometry/glyph check.
The complete slice rerun passed all 229 tests in `tests-zigzag-final.log`. All 12 rootless
swarm/name-resolution tests passed in `tests-swarm-final.log`. Three publication-network tests
passed in `tests-outbox-final.log`. The independent-package test exercised both readers successfully
but expected an obsolete label containing an internal document index. Its assertion now checks the
visible place/byte-range label, the native chosen member, selection 6–11 and the same focused
target; both recorded reader runs satisfy those checks. The corrected full network rerun passed in
`tests-package-final.log`, with exit code 0 in `tests-package-final-result.json`. Its interrupted
attempt is retained in `tests-package-interrupted.log`, and the first failed assertion's UI evidence
is in `package-first-failure-ui`.

All required suites passed across these reruns: 2,479 tests passed, with one deliberately skipped
video test. The final full build passed in `build-final-package-test.log`. Formatting and lint
passed in `gates-final-record.log`; the completed audit also passes the focused Markdown checks in
`gates-final-audit.log`.
