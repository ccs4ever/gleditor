# UX Validation 2026-10-03: Remaining Journey Gaps

Follow-up to the unvalidated cases in
[`audit-2026-10-02-ux-real-work-main.md`](audit-2026-10-02-ux-real-work-main.md) and
[`audit-2026-10-03-ux-j17.md`](audit-2026-10-03-ux-j17.md), on `feature/xuzz-link-context` at
`d296c84`. The initial validation changed no product code; the fix follow-up below records
subsequent implementation and revalidation. Every graphical run uses the offscreen SDL driver, dummy
audio and isolated XDG directories. The build uses SDL3 and has no AccessKit; accessibility evidence
comes from the harness tree.

The remaining checks expose three failures: formatting does not follow a passage into another store,
in-session reopening resets the caret, and repeating overline leaves it enabled. The File submenu
has no Close action. Camera restoration, locally cross-store link navigation, and the additional
backend checks pass within the scope below.

## Fix follow-up, 2026-10-04

The three confirmed failures are fixed in this worktree. File now also offers Close document through
the existing sovereign close command. The table below records the original findings; this section
records the current acceptance results.

| Check                              | Current outcome                                                                                                                                     | Evidence under `/tmp/ux-gap-fix-ui/`                                                            |
| ---------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------- |
| Close/Open reading place           | Pass: caret 30, anchor 10 and camera restored; the reopened and relaunched frames match the original exactly                                        | `resume/close-reopen.log`, `before.ppm`, `reopened.ppm`, `relaunched.ppm`                       |
| File > Close document              | Pass: the submenu exposes the action and clicking it closes the active document                                                                     | `menu/menu.log`, `file-menu.ppm`, `close-click.log`, `closed-via-menu.ppm`                      |
| Existing-document quote formatting | Pass: the shared `alpha` renders bold, italic and underlined after reopening                                                                        | `formats/existing-drop.log`, `existing-result.ppm`                                              |
| Pouch quote formatting             | Pass: the shared `alpha` inherits source formatting across stores, including later source edits without reopening                                   | `formats/pouch-insert.log`, `pouch-result.ppm`, `pouch-late.ppm`, `pouch-live-no-underline.ppm` |
| Overline on/off                    | Pass: the on frame adds only the overline; repeated invocation removes it while underline and strike remain, including after independent reopenings | `decor/on-reopened.ppm`, `off-reopened.ppm`, `settled-off.ppm`, `comparison.png`                |

`FormatResolver` now translates format endsets into each destination's coordinates across loaded
authorities. Local spans require the same permascroll object; external spans require the same
canonical scroll key, regardless of numeric ScrollId. Rendering registers no scrolls and appends no
operations. Keyboard and radial formatting refresh the open document sources. This is local
authority resolution; it does not fetch an unopened or remote source's links.

UI decoration commands now toggle. Turning an attribute off splices the selected addresses out of
every applicable format endset using append-only Structure operations. Duplicate format links and
partial selections are handled, other attributes survive, and previous operation folds remain
unchanged. The additive typed-decoration callback retains its existing behavior. Link-table replay
now updates an existing link's endpoint projection rather than retaining its first cached value.

Close retains a `ReadingPlace` for the document in memory, including its version, caret, selection
and camera. Open reuses the loaded authority and restores that place after view creation. The close
and reopen regression asserts that the visited store contains only its original typing operation.
This check covers closing and reopening during one session, plus normal quit/relaunch with an open
document. Selected-link context after Close/Open and closed-document checkpoints across process exit
remain outside this acceptance.

The 225-test format/link/structure/reading-place/binary-orchestration regression run passes. All 11
final targeted checks pass, including new UI command regressions, partial removal, save/reload,
historical fold preservation, and both local and external identity collisions. Logs are retained as
`/tmp/ux-gap-fix-expanded-tests.log` and `/tmp/ux-gap-fix-final-tests.log`; UI command manifests and
logs are beside their captures.

The full `make -j$(nproc)` build passes. The complete Xuzz and ZigZag engine suites also pass all 56
and 120 tests respectively, with logs in `/tmp/ux-gap-fix-xuzz-tests.log` and
`/tmp/ux-gap-fix-zigzag-tests.log`. All these runs use isolated XDG paths and headless settings.

## Steps and outcomes

| Journey | Step                                                                             | Affordance used                                        | Outcome       | Evidence                                                                                                                      |
| ------- | -------------------------------------------------------------------------------- | ------------------------------------------------------ | ------------- | ----------------------------------------------------------------------------------------------------------------------------- |
| J1/J17  | Type a document and apply italic to a selected passage                           | keyboard, Ctrl+Alt+I                                   | Pass          | `journey-*/source.png`, `session.log`, `default.ops.txt`                                                                      |
| J3/J17  | Transclude the selected passage into a new page                                  | Ctrl+T                                                 | Pass          | `journey-*/transclusion.png`, `default.ops.txt`                                                                               |
| J2      | Create a slice, add and edit a cell                                              | Ctrl+Alt+N, N, Right, E, keyboard, Return              | Pass          | `journey-*/slice.png`, `session.log`, `untitled-*.ops.txt`                                                                    |
| J5      | Restore a panned camera on relaunch before selecting a tab                       | wheel, Ctrl+Q, relaunch without a store argument       | Pass          | `00_initial.png`, `02_before_quit.png`, `03_relaunch.png`, `relaunch.log`                                                     |
| J5      | Close a document view through a visible control and through the keyboard         | tab's x, Ctrl+W                                        | Pass          | `same-session.log`, `15_closed_before_open.png`, `18_closed_by_key.png`                                                       |
| J5      | Reopen and read a closed document from the running application                   | Ctrl+O, Tab to Custom path, type the saved path, Enter | Pass          | `same-session.log`, `16_path.png`, `17_reopened_same_session.png`                                                             |
| J5      | Resume the caret after that in-session close and reopen                          | caret at byte 30, Ctrl+W, Ctrl+O                       | Fail          | `caret-reopen.log`, `19_caret30.png`, `21_reopen_caret.png`                                                                   |
| J5      | Close through the radial File menu                                               | right-click, New and Open wedge                        | No affordance | `22_file_menu.png`, `file-menu.log`                                                                                           |
| J4/J17  | Author one 2×3 link                                                              | bench bindings, Ctrl+Alt+L                             | Pass          | `author.log`, `01_2x3_forged.png`, `before-navigation-*.ops.txt`                                                              |
| J4/J17  | Select overlapping links and a third forged link                                 | Alt+Shift+N                                            | Pass          | `navigate.log`, `04_selected.png`, `09_second_selected.png`, `10_third_selected.png`                                          |
| J4      | Choose each side's member independently                                          | Alt+Shift+J, Alt+Shift+X                               | Pass          | `exact-navigation.log`, `11_left_member2.png`, `12_right_member3_doc.png`                                                     |
| J4      | Enter the chosen passage in the other store and return                           | Alt+Shift+L, Alt+Shift+Return, Alt+Shift+O             | Pass          | `exact-navigation.log`, `13_foreign_entered.png`, `14_origin_restored.png`                                                    |
| J17     | Insert the passage into an existing document                                     | select, drag into its page                             | Pass          | `existing-retry-and-strike.log`, `19_existing_drop_retry.png`, `untitled-*.ops.txt`                                           |
| J17     | Insert the stored passage from a pouch                                           | Ctrl+D, Ctrl+N, F2, card's +                           | Pass          | `pouch-and-frame.log`, `06_pouch_inserted.png`, `untitled-*.ops.txt`                                                          |
| J17     | Drop the passage into empty space                                                | select, drag                                           | Pass          | `drops-and-reformat.log`, `13_empty_drop.png`, `default.ops.txt`                                                              |
| J17     | Reread source formatting in the empty-space occurrence, live and after reopening | source Ctrl+Alt+O and Ctrl+Alt+X, Ctrl+4               | Pass          | `23_empty_struck.png`, `27_reopened_empty.png`, `format-comparison.png`                                                       |
| J17     | Reread source formatting in the existing-document and pouch occurrences          | source format bindings, Ctrl+2/3, relaunch             | Fail          | `21_existing_struck.png`, `22_pouch_struck.png`, `25_reopened_existing.png`, `26_reopened_pouch.png`, `format-comparison.png` |
| J17     | Remove overline while preserving other formatting                                | select the same passage, Ctrl+Alt+O again              | Fail          | `/tmp/ux-gap-decor-only/comparison.png`, `settled-toggle-comparison.png`, `*.args.json`, `*.log`                              |

## Backend coverage

All three compiled backends run through the offscreen driver. Xvfb is not installed and was not
needed. Evidence is in `/tmp/ux-gap-backends/`.

The document-formatting, new-page transclusion and slice-editing rows were each repeated on OpenGL,
OpenGL ES and Vulkan.

The source and quoted passage address the same primedia; the quote is a Transclude operation, not a
second Insert. The slice's cell content is persisted as Structure operations. Frames were inspected,
including the gold transclusion beam and the focused cell's rank neighborhood. Commands and
arguments are retained in `journeys.py` and each run's `args.json`.

Separately, all 26 `E2EBinaryOrchestrationTest` cases passed on OpenGL ES and Vulkan, including
dragging a selection to its destination, many-to-many links and multi-page rendering. These are
supporting integration checks; tests that construct fixtures do not count as journey authoring. Logs
are `e2e-opengles.log` and `e2e-vulkan.log`, with captures in the matching directories.

The renderer comparison passed text, notifications, picking, culling, atlas growth and minified text
on all three backends. OpenGL ES matched the OpenGL reference exactly; Vulkan's differing pixels
stayed below the comparison's one-percent limit. See `offscreen-parity.log`, `recheck.log` and their
capture directories.

One first-run check failed: `vk.threads1.ppm` and `vk.threads4.ppm` differed at 5,510 pixels. Four
fresh captures, two at each thread setting, matched exactly; a complete renderer recheck also
passed. Retain the original mismatch as an unresolved intermittent capture result. The one-page
sample did not trigger parallel recording, so this run does not validate parallel recording itself.

## Resume and in-session reopening

Evidence is in `/tmp/ux-gap-resume/`.

The camera run moves the page mostly out of the viewport. Its saved view is reapplied on relaunch:
the before-quit and first resumed frames are identical at every pixel. The activity-store dump,
`activity-after-close.ops.txt`, retains the camera coordinates. No tab click precedes the resumed
capture.

The in-session reopen preserves the text but resets the caret: byte 30 before closing, byte 0 after
opening the same saved path. This is different from quitting and resuming the open session. Start a
fix in `Views::closeActive()` and `Views::openDocumentPalette()` in `apps/xudu/views.cpp`: retain
the closed document's reading place, keyed by store authority and version, and reuse
`restorePlace()` when reopening it. Do not refit the camera or overwrite its saved caret while
activating the reopened view. Selection and selected-link restoration on this path require a
follow-up check after that fix.

The File menu provides New xanadoc, New slice and Open; it has no Close document or Close store
entry. The visible tab x and Ctrl+W close a document view. Add a named File-menu action wired to the
existing close command if the contract's explicit menu route is retained; closing an entire store
with several open objects has not been established by the document-view check.

The harness takes lowercase `--key tab` and `--key enter`. An earlier attempt with `Tab` and
`Return` was refused before those events were dispatched and is retained as diagnostic evidence, not
a product failure.

## Many-member, overlapping and cross-store links

Evidence is in `/tmp/ux-gap-links-evidence/`; the driver is `/tmp/ux-gap-links-check.py`. Two
documents were typed through the interface, one in the default store and one in a Ctrl+N store.
Their passages were collected on the clasp's two benches with Ctrl+Alt+[ and Ctrl+Alt+], then forged
with Ctrl+Alt+L. One link has two discontinuous left members and three right members. Two more links
connect the stores; one overlaps the first link's `alpha` passage.

The accessibility tree keeps one selected identity and both full endsets. Choosing left member 2 and
right member 3 preserves the independent cursors. Explicitly choosing the document occurrence enters
document 1 at bytes 8–13 (`three`), with the same link selected, then restores the origin at
document 0 byte 2. Merely choosing a member leaves the occurrence undecided when both its document
and its backing cell contain the same span; Enter does not silently choose one.

This closes the limited earlier gap for links between the locally open stores. It does not prove
remote publication resolution, pointer disambiguation at an overlap, or every J16 activity branch
and unavailable-target case.

The operation dumps of both visited stores are identical after authoring ended and after the
navigation checks. See `before-navigation-*.ops.txt` and `after-exact-navigation-*.ops.txt`. Live
movement and entering a saved passage wrote no operations to those stores.

## Formatting and transclusion occurrences

Evidence is in `/tmp/ux-gap-format/` and `/tmp/ux-gap-decor-only/`.

All three insertion routes address the original `alpha` at local primedia `[43041,43046)`. The
existing-document and pouch stores use Insert operations over that existing span; no newly typed
`alpha` bytes were appended. The empty-space page is a Transclude microversion in the source store.
Their text and addresses survive a relaunch.

The source is bold, italic and underlined before quoting; later overline and strikethrough are
applied to the same range. The empty-space page has the source's appearance. The existing-document
and pouch occurrences render in ordinary type without those decorations, both live and after
reopening. `format-comparison.png` enlarges the four passages side by side. The earlier audit's
passing new-page case was within one store; these failing destinations are separate Ctrl+N stores.

`Session::sourceFor()` in `apps/xudu/session.cpp` constructs `FormatResolver(st)` from the
destination store alone. The source's Format links are in the other open store. Proposed fix:
resolve applicable Format links across the relevant open authorities using canonical primedia
identity, and invalidate all affected document sources when a shared span's formatting changes.
Retain the shared addresses rather than copying text or manufacturing independent format links for
each occurrence. This inference from the lookup path explains the observed store boundary; it is not
a tested implementation fix.

The separate decoration run applies overline to `alpha`, then repeats the command on the same range.
The line remains. Adding strikethrough and underline provides visible positive controls. The
overline-on/off pair is repeated using final `--screenshot` frames and independent reopenings: the
text region still does not change. This is not only a stale intermediate `--capture`.
`Session::markDecorated()` always appends a Format link; the UI's actions described as toggles call
that additive method. Implement actual toggling at the UI command boundary while preserving other
attributes and the append-only operation model. Do not assume that the earlier bold/italic toggle
acceptance proves other attributes can be removed.

The first existing-page drop used coordinates from the preceding process and missed the source: it
changed no content. The retry frames and checks the source in the same session, confirms a glyph hit
in document 0, then inserts five shared bytes into document 1. Only the retry counts as a pass.

## Scope limits

This follow-up does not repeat every J1–J6 gesture on every backend or renew the acceptance of
J7–J16 from their separate audit. Native platform accessibility remains unvalidated because
AccessKit is absent. Evidence under `/tmp` must be retained to review the run later.

## Repository checks

At initial validation, the relevant binaries were up to date. The two additional backend integration
runs pass all 26 cases each. `make -j$(nproc) lint` passes, and the three changed Markdown files
pass their targeted formatting and lint checks. `git diff --check` passes.

The initial full `make -j$(nproc) format-check` fails on pre-existing formatting in untouched C++
files, including `link_navigation.cpp`, `reading_place.cpp`, `system_docs.cpp`, `link_context.cpp`,
`views.cpp`, `xuzz_app.cpp` and `link_in_page_store_test.cpp`. The complete diagnostics are retained
in `/tmp/ux-gap-backends/format-check.log`.

After the fixes, all changed C++ files pass clang-format 19's targeted check. The full gate with
clang-format 19 still reports existing formatting in unrelated files, starting with
`apps/common/ui/hypertime_graph.cpp` and `apps/common/xanadu/enfilade/edl_transform.cpp`.
Diagnostics are in `/tmp/ux-gap-fix-format-check.log`. The lint gate passes after the fixes and
audit updates; its output is in `/tmp/ux-gap-fix-final-lint.log`.
