# Eighth spatial UI checkpoint

This increment fixes live application of native system-setting edits, corrects two investigation
findings and repairs the seventh-checkpoint archive's quality gates. The baseline is `3afcc8b8`;
`candidate-provenance.json` records the exact product patch, source and binary hashes. The initial
pointer proof and historical Back audit have separate provenance in `source-provenance.json`.

## Live system-setting commits

Native Omnibar navigation reaches the active layout scalar through
`##/d.vars[. = "readableTextPx"]/d.values`. E opens the existing cell editor; entering `24.0` keeps
the value a double. The earlier axis-only investigation missed this existing route. Its initial
claim of no affordance is superseded by the native semantic-path evidence.

The baseline edit appended a correct SetValue but left the consumed system current head at 16 and
sent no watcher notification. `Session::advanceViewAfterCommit()` refreshed only the paired view.
Accepted authored slice commits now publish and save the known system-store head, then notify the
existing system-document watcher. This C++ change connects the native editor's commit boundary to
Session's existing lifecycle; no new command, setting, camera or view architecture is added.
Historical sibling views keep their own exact heads. No-op and rejected commits append no
notification operations and send no callback.

The owning regression fails on the baseline with no callback and stale in-memory and saved values.
It passes after the fix, including active double 24, reload, historical view preservation and
no-op/rejected controls. Full UI coverage passes 234/234. Persistence failures retain the existing
system-edit behavior: a save exception can leave the in-memory state advanced, so this change makes
no transactional rollback claim.

## Native document text and whole-page ink

The native GLES journey opens `system://layout` with Ctrl+O, Tab, its URI and Enter; F6 enters the
slice controls, and F2 opens the existing Omnibar. The semantic query
`##/d.vars[. = "readableTextPx"]/d.values` selects the active scalar. Escape closes the Omnibar, E
opens its editor, `24.0` and Enter commit, and Ctrl+1 returns to the source. The decimal spelling
preserves the setting's double type. This changes document reading size; the independent
`ui.fontScale` setting changes chrome.

`text-gles/` retains a true pre-edit scalar value of 16, the committed and reopened 24.0, one live
layout-update notification, both successful exits and byte-identical source operation dumps across
all five snapshots. The observer is bracketed by frame-file creation and a bounded pause; earlier
late snapshots are retained as invalid diagnostics. Visible glyph bands grow from about 9–11 pixels
to 18 pixels, with row spacing growing from about 15 to 24 pixels and persisting after reopen.
Returning to the source also reframes the page, so this is native visual behavior, not a
camera-locked performance measurement. The lead independently inspected the before, live and
reopened frames. The mini-map occludes some enlarged lines; earlier long-line clipping at 800 pixels
is retained separately. No complete accessibility or responsive-layout acceptance is inferred from
the legible short-line sample.

The independent offscreen Vulkan journey in `text-vulkan/` also brackets the active 16 value before
editing, consumes 24.0 with one layout notification, and retains it after same-profile reopen.
Source operations are byte-identical across all five snapshots, both processes exit 0, and the lead
directly inspected the enlarged frame. Its baseline red artifacts are retained.
`text-row-audit.json` records a common visible-row predicate on both backend frame sets; enlarged
main glyph bands and row spacing agree in that sampled region. The same reframe and mini-map
limitations apply. Vulkan here is the local RADV device; no software-Vulkan coverage is inferred.

The existing whole-page ink test also passes unchanged on local GLES and Vulkan, complementing the
prior GL pass. `ink/` retains logs, exit results and actual lossless frames. Its 100-pixel threshold
is unchanged. This is one existing test on these local backends, not broad E2E coverage.

## Enter through the visible controls

The earlier direct Enter click failed because the responsive dock was collapsed. Its visible
Controls button expands the actions. The native pointer route is Controls, then Enter; both drawn
controls were selected through their unique accessible labels. At 800 by 600 and UI font scale 1.5,
all actions fit on one expanded controls page. No paging control is needed in this case.

The independent native GL run retains four snapshots: before Controls, after Controls, after Enter
and after continuation/Back/Origin. Activity visit totals are 1, 1, 2 and 2. Both source operation
dumps are byte-identical at every snapshot. Thirteen raw captures were converted losslessly to PNG.
Exact argument, source, activity and image artifacts are in `pointer/`.

The saved UI store confirms the active double value was changed from 1 to 1.5, while its schema
default remains 1. The transcript records the live UI system-document update. The full diagnostic
ops dump and a short proof are retained with the pointer evidence; the scale is not inferred only
from the scripted input.

`run-pointer-journey.sh` is the reviewed reproduction aid. It takes a binary directory, evidence
directory, raw-frame directory, width, height and mode, using the caller's isolated headless/XDG
environment and Xvfb. It records actual app/library hashes before and after and refuses changes. The
tested case is 800, 600, forced. Other layouts may expose a different visible control route. This
native evidence resolves the missing-Enter report for this case; it does not justify adding another
Enter action or changing responsive product layout.

Independent UX/visual review confirms legible Controls and Enter actions and unchanged source and
visit invariants. The mini-map can still cover selected text, and the expanded dock reduces visible
document space. This bounded route is not complete J24/J25 or 16+16 journey acceptance.

The candidate rebuild repeats the default reading-size pointer proof in `candidate-pointer/`; the
lead's checkpoint audit again finds visits 1, 1, 2, 2 and identical source dumps. The additional
`text-gl-link/` run combines actual document reading size 24, chrome scale 1.5 and the forced
7438-byte selection at 800 by 600. Opening the layout store adds real system Links, so the reader
uses Next link again to select the authored `comment` relation with `local_author` authority. The
chosen left endset is bytes 0–7438 across the physical page break at 3719; the right endset is bytes
0–23 in the target. Controls and Enter remain visible and clickable. Its four snapshots again show
visits 1, 1, 2, 2 and unchanged source operations through continuation, Back and Origin. This
combined run proves the committed 24 setting and native Link route; the separate GLES run provides
the bracketed 16-to-24 transition and same-profile reopen proof. The 15 combined frames retain the
clipped long-line and reduced viewport limitations.

## Correction to the Back review

The previous local report said Vulkan omitted the Link panel after Back. That observation was wrong.
Directly reopening all three archived Back frames shows the same docked summary, Controls and close
actions. Their normalized accessibility trees match exactly; normalization replaces only
run-generated source/target names with stable role names. All show default focus, the same Link,
ordered endsets and chosen occurrences. Activity records show restoration to visit 1 with no
additional completed visit.

`back-audit.json` records the exact comparisons, and the normalized trees are alongside it. GL and
GLES pixels match exactly. GL and Vulkan differ in 10,091 of 480,000 RGB pixels, including 2,447 in
the panel's y=340–550 band. This is semantic panel-state parity, not a claim of exact image parity
or a newly passed native-image tolerance. No Back product fix is justified by these frames. The old
report remains intact as a historical record; this correction supersedes its Back-panel claim.

Independent review also distinguishes Origin from Back. In the default reading-size
`pointer/png/12-origin.png` and candidate equivalent, focus returns to `home` and no Link panel is
visible. The pre-fix frame already has that state. In the enlarged combined
`text-gl-link/evidence/png/12-origin.png`, the authored Link panel and both endsets remain visible.
These runs have other state differences, including opening the layout store; no causal claim about
text size follows. The source/visit invariants hold, but complete Origin context-restoration parity
remains open. The combined run's Back frame also has a blank-looking page with no panel, as recorded
in the independent review; it is not promoted to a full visual restoration pass. The next native
investigation must isolate that difference against the pinned baseline rather than treating all
restoration states as accepted.

## Archive quality and provenance

Seven seventh-checkpoint shell scripts are now formatted and lint-clean. Unused variables were
removed and intentional comma tuples quoted. The GLES script now propagates the actual app exit
status instead of always returning success. The long reproduction command is fenced to satisfy
Markdown line length. Neither comparison criteria nor product services changed.

The exact pre-repair scripts, report and root manifest are preserved as text under
`../spatial-ui-seventh-increment/archived-originals/`, with original/copy and old/new SHA256
ledgers. Nested historical run manifests are unchanged. The seventh-checkpoint root manifest now
identifies current files and preserved originals explicitly; it no longer presents changed scripts
as if they matched old hashes.

The archive-only full format and lint runs pass. Final checks after adding this evidence are
recorded separately. The full UI suite now passes 234/234, including the new regression. The full
library 799/799, Xuzz 99/99 and cache/mip owning 53/53 results are reused after verifying their test
binaries and shared library are byte-identical to the previous passing checkpoint. They are not
counted as new suite runs. The existing strict core backend result remains applicable to unchanged
generic rendering; this increment changes no renderer or comparison criterion.

Final repository gates pass: full `make format-check` with clang-format 19 and full `make lint`,
both exit 0. Compressed logs and `final-checks.json` record these gates. The lead also verified
`git diff --check`, the seventh root inventory and preserved originals. The eighth root `SHA256SUMS`
identifies the final evidence files; nested historical manifests keep their original scope and
paths. The next task is the isolated Origin/Back context-restoration investigation, followed by the
remaining J24/J25 variants listed in the project status. Full native AT-SPI, optimized performance
and 16+16 journey acceptance remain open.
