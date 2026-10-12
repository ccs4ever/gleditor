# Restart: finish the real-work journeys until J1–J16 are green

Completed on 12 October 2026: all J1–J16 journeys are green. The current acceptance evidence and
final checks are recorded in [the completed audit](../audit-2026-10-12-real-work-green.md). The
restart state below is retained as historical context.

Continue in `/data/git/gleditor-restore` on `main`. Read this prompt, actual `AGENTS.md`, the
relevant README sections, `.claude/skills/xuzz-ui-design/SKILL.md`, and
`.claude/skills/ux-validation/SKILL.md`. The authoritative acceptance contract is
`design/ux_workflow_real_work.md`; navigation also uses `design/ui_workflow_xuzz_navigation.md` and
R8 in `design/store-slice-convergence.md`.

## User objective and authorization

The latest work instruction is:

> commit and push what we have, then keep trying the journeys until they're all green

The existing checkpoint has been committed and pushed. Continue implementation and actual interface
reruns; do not stop at an audit, a plan, or bounded smoke passes. The user then requested this
restart prompt. Prior authorization includes parallel journey runners under the UX-validation skill,
with at most three subagents plus the lead. Preserve unrelated work and append-only data.

## Verified Git and execution state

- `0e19b92e`: `Refine link beams and panels and record real-work UX audit`, committed the
  accumulated 134-file workspace, excluding generated `tools/__pycache__/`.
- The first push was rejected because the remote had advanced. Fetched four remote commits through
  `6e9eb090`, including modular identity/crypto, migration and container hardening. Merged them,
  preserving the tested local behavior in four conflict hunks and remote changes elsewhere.
- `ccdf16db`: `Merge remote-tracking branch 'origin/main'`, successfully pushed to `origin/main`.
  The server reported bypassed branch rules but accepted the push. Do not force-push or rewrite it.
- New journey fixes below are **uncommitted**. `git status --short` currently shows approximately 43
  modified paths plus four new source/test files and generated `tools/__pycache__/`.
- No native application or build process was running at the final inspection. Recheck before work.
- Final `git diff --check` passed. Current source is NOT fully built or validated.

Later tool calls and agent turns hit a usage limit. Automatic approval review could not complete;
this was not an unsafe-action determination. No bypass was attempted. Read-only calls and writing
this prompt subsequently succeeded, but do not assume all agents completed their assigned work. Use
the actual source, not their last stated intentions.

## Build, isolation and tools

Never edit compiling source or relink while native programs are active. Separate implementation and
validation windows. All device runs are headless, with separate XDG profiles. Always build with:

```sh
make -j$(nproc) ACCESSKIT_DIR=/tmp/ux-accesskit-linux GLEDITOR_ENABLE_A11Y=1
```

Outside make, set all six environment variables from AGENTS: offscreen video, dummy audio, software
GL, and isolated XDG data/config/cache. Prefer Xuzz's normal graphical path under
`SDL_VIDEODRIVER=offscreen` for interface validation; windowless batch output is not an interface
pass.

Shell execution has required `sandbox_permissions: require_escalated` because of the `.agents`
symlink; auto-review normally accepted it. A review failure must not be bypassed.

Local tools:

```sh
/tmp/gleditor-clang-format-19/clang_format/data/bin/clang-format -i --style=file <files>
/home/ccs4ever/.local/bin/mdformat --wrap 100 --extensions gfm \
  --extensions dollarmath --extensions frontmatter <markdown>
GEM_HOME=$(ruby -e 'print Gem.user_dir') make -j$(nproc) format-check lint \
  CLANG_FORMAT=/tmp/gleditor-clang-format-19/clang_format/data/bin/clang-format
```

The fetched remote renamed the dump tool to `build/store-dump` / `tools/store-dump.cpp`; inspect
current Makefile/help instead of assuming the older `xudu-dump` spelling still exists.

## Evidence and prior verified checkpoint

The previous complete audit is `design/audit-2026-10-10-real-work.md`, especially “Journey results —
frozen interface runs” and its prioritized findings. It remains historical, not current all-pass
acceptance. The older restart file `design/projects/resume-link-beams-and-real-work-audit.md`
contains useful detailed fixture/harness information, but its “no commit” statement is superseded.

Evidence roots:

- `build/ux-full-20261010/`: previous audit, native harness, authored stores, PTY stages and
  captures.
- `build/ux-green-20261011/`: new fixes and reruns, described below.

The merged checkpoint passed the full native-enabled build in
`build/ux-full-20261010/build-remote-integration.log`, 50 navigation/layout tests in
`tests-remote-integration.log`, and format/lint in `gates-remote-integration-final.log`. Remote
formatter differences were normalized with clang-format19 before the merge push.

The first new fix batch built successfully in `build-journey-fixes-1-retry.log` under the old root.
Its engine run passed 55 tests: `build/ux-green-20261011/tests-engine.log`. Its library run passed
60 of 61 tests: `tests-lib.log`. The one failure was a genuine trailing-newline text-run defect,
subsequently fixed in source, but that fix has NOT been rebuilt or rerun.

## Current implementation and unfinished integration

### VPL report composition and provenance

`apps/common/xanadu/vpl/` and `apps/vpl/main.cpp` now implement comma catenate/ravel in the direct
REPL engine, with independent result cells, preserved aliases/types, expression/output-row source
tracking and stale-result invalidation after expression errors. Tests cover revised budget
12/10/5/27 and counts2/1/3 without pretending a literal has input provenance. Compiler-only VM
catenate still explicitly rejects unsupported compilation; default interactive evaluation is direct.
The first 55-test run passed these changes. Actual current-profile J8/J13 report handoffs are
pending.

### Native text accessibility

`include/gleditor/a11y/tree.hpp`, `src/a11y/tree.cpp`, `src/form.cpp`, `src/ui/overlay.cpp` and
tests expose TextRun children and Unicode caret/selection metadata for Form/Overlay inputs. Password
runs remain masked. Stable run IDs have bounded disjoint namespaces. AccessKit Unix0.22.1 supplies
Text but **does not implement EditableText**. Prove native Component.grab_focus plus ordinary
keyboard editing and native Text readback; do not claim EditableText.SetTextContents exists. The
pending newline fix retains an empty final run after a trailing newline. Rerun:
`A11yRunTest.*:FormTest.accessibilityTextRunsTrackUnicodeCaretAndEmptyNewline`, plus related Form
and Overlay tests. The first failure expected two runs and got one; do not weaken that assertion.

### Menu creation, view modes and automation synchronization

Xuzz radial actions beginning `run:` now call `app.commands().run(...)`; the action-handler block
was moved after `gleditor::Application app(...)` to permit that capture. This built successfully.
ViewCoordinator now sets generic `RenderState.documentsVisible` and reports mode changes so slice
keyboard focus/camera transitions agree with Xanadocs/Slices/Both filtering.

Fresh J1 found a separate harness timing defect: the pointer pick creates the new document but the
next scripted typing can run before its queued focus change drains, using a stale settled flag.
`Renderer::advanceScript` currently adds a blanket `hasPendingWork()` guard. **Review this before
building:** a blanket guard can defeat `CaptureLive` during animation. Preserve live-capture
behavior while refusing advancement past queued document/focus work. The attempted refinement did
not execute. Add meaningful regression coverage and rerun native menu creation without inserted
delay workarounds.

### Multi-store native link candidates and presentation

LinkContext now resolves native links from every loaded authority, not only the primary store.
PackageOccurrenceIndex has a read-only native-store overload using canonical scroll identities; it
avoids offset aliasing across independent author scrolls and avoids deployment-slot mutation.
`presentationLinks()` combines native and reader-package scaffolds, retains full endsets and avoids
Cartesian strand expansion. Native tiers remain intact. The native-scroll identity regression
passed.

Final source inspection confirms beams now use raw primary link IDs and presentation IDs only for
secondary/package owners. This correction was applied by an agent before its usage failure; it is
unbuilt. Rerun actual secondary-store J16 selection, exact occurrences and reverse entry.

### Saved cell quotation source return

Views has `cellSourceEntry`; source return validates the saved source authority/version, cell
membership and quoted addresses before binding the slice and restoring focus. Xuzz wires that
callback. A bounded native J12 rerun returns to cell#6 through the actual “Open source” control.
Full three-occurrence transclusion, surrounding edits, exact range behavior and reopen remain
pending.

### J14 map annotation parent

HypertimeGraph has an opened callback. Xuzz captures the bound slice's editing parent and focus
before map navigation previews history. Annotation uses that captured parent/focus and the
explicitly selected target operation, inserts the handle after the captured rank focus and preserves
its next neighbor. This is unbuilt; verify the complete six-kind rank and foreign target after
reopen.

### J15 paired text/slice authored commits

Session and ZigzagVisualizer expose authored-commit callbacks carrying exact parent/new head, and
exact-parent advancement APIs. Historical sibling views must stay unchanged; navigation emits no
commit. New `tests/zigzag/paired_edit_heads_test.cpp` checks interleaved edits and sibling refusal.

Final inspection confirms Xuzz callback wiring **has been applied** near `pairedDocument` around
line1104: document commit advances the paired slice; slice commit advances the paired Session view;
both update sliceHeads/map/LinkContext. This was not built. Inspect completeness and callback/thread
behavior. Flushing buffered document edits before entering slice editing was requested but not
confirmed; no flush call was found in the last targeted search. Do not use global Store.latest to
advance a historical comparison view or reset focus/axes/camera through unnecessary rebinding.

### Walks: components exist, application wiring absent

New `apps/common/ui/walks.hpp/.cpp` and `tests/zigzag/walks_form_test.cpp` implement a reusable
Form. StoreActivityLog exposes `walkEntries()`, `reference(VisitId)`, and
`annotate(VisitId,string_view)`. Separate strict metadata records preserve references and
Unicode/multiline annotations only in the private activity store. References are idempotent; empty
annotations and unknown targets refuse. `nav::EnterVisit` was appended to the command variant and
restores an arbitrary known visit. Tests are in `tests/xuzz/store_activity_log_test.cpp` and
`link_navigation_test.cpp`.

**Walks is NOT wired into Xuzz.** No Walks command/default binding/radial entry or LinkContext
wrappers were present at final inspection. Complete this integration:

1. Expose LinkContext walk entries, current visit and reference/annotate wrappers.
1. Add an ordinary visible Walks control and free default Ctrl+Alt+W through system_docs specs.
1. Use `openWalksForm` and its Preview/Enter/Reference/Annotate actions. Preview must read the saved
   version's document/cell excerpt without changing navigator selection, current visit or focus.
1. Add exact-store/version cell-entry handling through Views.cellSourceEntry, so restored cell
   visits need not match the currently bound slice head.
1. **Preflight EnterVisit and ordinary Enter/EnterAt before navigator dispatch.** A
   closed/unavailable document or absent cell must not select a visit or invent a completed
   transition before focus fails. Preserve the chosen owner/member, expose explicit unavailable
   status and a Cancel control in `apps/common/ui/xanadoc/link_panel_overlay.cpp`. Reader-package
   pending state remains separate.
1. Unit tests plus real native Walks reference/annotation/preview/reopen are required.

Suggested tests: xuzz_test `ActivityMetadataTest.*:StoreActivityLogTest.*:LinkNavigationTest.*`;
zigzag_test `WalksFormTest.*`, paired-edit tests; add integration refusal tests as appropriate.

## Current rerun results and prepared plans

Under `build/ux-green-20261011/`:

- `j1-menu-key`: both documents were created, but menu typing landed in the previous default
  document because of the harness timing finding. Not a pass. Key-created document typed correctly.
- `j2-menu-key`: menu and key creation both show named connected cells. Bounded pass, not full
  persistence/accessibility acceptance.
- `j10-modes`: fresh two documents plus same-store slice, edits, all three modes and repeated cycles
  captured. Slices hides pages and names the focused cell. Full caret/camera persistence comparisons
  and keyboard/native repetition remain.
- `j12-return`: actual saved cell quotation “Open source”, then Close, restores visible cell#6.
  Bounded source-return proof; not whole J12.
- `j13-report`, `j8-report` and their `*-rank` attempts are **invalid input-profile runs**. See next
  section. Prompt success is not a values pass.
- Prepared, not run: `j16-author-r2/plan.json`, `j1-menu-key-r2/plan.json`,
  `j2-menu-key-r2/plan.json`, `j13-fresh-author/plan.json`, `j7-fresh-author/plan.json`. The J16
  fresh author plan is 242 argument tokens and authors two passages, repeated quotation, three
  distant rank cells and a2×3 link through ordinary controls. Still needs the second overlapping
  identity and every navigation substep; inspect actual saved IDs rather than assuming old cell
  numbers8/52/92 survive changed commit ordering.

## Crucial permascroll migration and terminal handoffs

The remote changed `PermascrollRegistry::defaultUser()` from
`$XDG_DATA_HOME/xudu/permascroll/default` to **`xuzz/permascroll/default`**. Session store/config
paths may still say xudu. Old audit profiles only contain the old xudu author scroll, so opening
them with this binary can retain canonical scalar bits while all dimension names/text are empty. The
failed new report attempts used those old profiles; do not diagnose their zero values as a VPL
regression and do not relabel them green. Re-author inputs through the current Xuzz interface and
carry the complete closed data/config profile through each consumer. No `--permascroll` escape or
manual numeric copying counts as a journey pass.

Fresh J7 plan authors numeric12/8/5 on d.1. In an actual VPL PTY, interactively `:open` that store,
`:rank d.1`, inspect `data`, total25, revise `data ← data + 0 2 0`, total27, recover from an error,
compose `report ← data , total`, inspect `:grid`, save, reopen and total the first three saved cells
(`amounts ← 3 ↑ data`) to27. Inspect report12/10/5/27 alongside original12/8/5 in Xuzz.

Fresh J13 plan authors issue_alpha and beta with editing/rights/approval, gamma with
editing/approval only. VQuery `return 6/d.1::rank[. != "home"]/d.2::rank` previously extracted11
facts; determine current head IDs from the newly authored store. Save facts through the prompt.
VProlog opens those facts, asserts ready(I) requiring three gates, proves gamma false and alpha/beta
separate solutions, derives status1/1/0 and exports flags. VPL opens actual flags, selects d.result,
calculates ready2/unready1/total3, catenates and saves report, then Xuzz inspects count cells and
their source route through flags/facts/original. Reopen and recompute, with no manual input
substitution.

Old reusable terminal plans: `build/ux-full-20261010/j13-facts-query/plan.json` and
`j13-facts-prolog/plan.json`; rewrite paths and value assertions. `contains` in
`tools/ux-pty-client.py` must be an ARRAY of complete strings; a string is iterated characterwise
and makes weak assertions. Check exact output, not only returned prompts.

## Harness details

- `/tmp/ux-resume-ui.py <evidence-dir> <plan.json>` launches Xuzz with optional store-path
  launch_args, resize1440×1000 and ordered input steps, isolated XDG, timeout120, retained
  launch/log.
- `tools/ux-pty-client.py --binary build/<program> --plan <json> --evidence <dir>` drives a real
  PTY.
- Form interaction uses `--chord Return`, not dialog-only `--key`. Custom path: Ctrl+O, Return, five
  Down choices, Return, Tab, type path, Return. Inspect current choices rather than assuming.
- `--click-label` uses accessible bounds for a real pointer click; it is NOT a native AT Action.
- Actual Alt-cell pickup requires `--hold-mods Alt`, mouse-down/move/up, then None.
- Native AT tools/plans live under `build/ux-full-20261010/harness-inventory/` and
  `tools/ux-atspi-client.py`. Use native Text/Component capabilities actually exposed by AccessKit.
- Retain frames, a11y dumps, native readbacks, full PTY transcripts, source identities, closed-store
  dumps and visited-store integrity hashes. Unit tests do not substitute for journey acceptance.

## Continue in order

1. Verify dirty state/processes, finish the incomplete integrations above and the CaptureLive guard.
   Check current source because agent turns ended after some partial edits. Freeze all source.
1. Build all programs, run meaningful affected tests including the previously failing newline case,
   then formatting/lint. Resolve real failures without weakening invariants.
1. Freeze product code and run fresh J16 authoring first. Test primary and secondary authorities,
   every member/occurrence, both directions, document and distant-cell entry, two activity futures,
   Walks preview/reference/annotation/reopen and unavailable-target cancellation. Navigation must
   append nothing to visited stores.
1. Run J1–J15 in parallel ownership groups with isolated profiles. Fresh-author terminal inputs.
   Cover all previously unvalidated steps: J4 menu/key roundtrip with selected link; J5 complete
   caret/camera/pouch/link/cell resume and menu/key close; J12 three shared occurrences plus
   surrounding edits/reopen; J14 complete ordered six-kind rank and stable foreign reference; J15
   both edit kinds on both branches, all change-site targets and reopened comparison.
1. Every new finding starts a separate fix phase, then an affected rerun. Keyboard/native
   accessibility repetition is part of acceptance. Known dense blank-body rendering remains a
   finding until reproduced/resolved or bounded with actual evidence.
1. Update the audit with fresh outcomes and evidence. Keep historical failures distinguishable. Do
   not claim all green until every required step passes. Finish authorized work autonomously.

Do not repeat the already accepted historical beam performance/backend project unless new changes
require it. The priority is complete real-work behavior and demonstrable interface acceptance.
