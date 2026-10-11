# Restart: finish link-beam validation, then audit real-work journeys

Continue work in `/data/git/gleditor-restore`. Read this prompt, the actual `AGENTS.md`, relevant
README sections, and the skills listed below before changing code. This handoff was written on 11
October 2026. Evidence directories keep their original `20261010` names.

## Current checkpoint — 11 October 2026, after resumed audit

The snapshot under “Exact immediate state” below is historical. Its panel failure, retained-caret
check, performance measurements and backend checks have since been resolved and rerun. The latest 30
panel/overlay/cache tests, 81 library tests, 40 navigation tests and 24 settings tests pass.
Dense100 selection preparation is now1.371ms; all100 owners cycle and wrap. KJV companion and 24
placed-page/backend component captures have bounded independent acceptance.

Fresh J16 authoring started first, followed by parallel journey runners. Their usage allocation
ended before all reports were written; the lead recovered the evidence and finished the terminal
handoffs, J14/J15 and additional graphical runs. The current results and prioritized product
findings are in [`../audit-2026-10-10-real-work.md`](../audit-2026-10-10-real-work.md), especially
“Journey results — frozen interface runs”. Read that before scheduling new runs. Do not repeat the
historical panel/performance work or claim full J1–J16 acceptance.

The VQuery implicit permascroll-guessing defect is fixed and its all-program build passes. Fresh
prompt-only J6 succeeds and Xuzz displays the saved alpha result. J13 now consumes 11 UI-authored
facts, exports Prolog flags1/1/0, and calculates VPL counts2/1/3. Report composition still fails.
Actual J16 primary-store navigation enters cells8/92, retains2×3 endsets, saves two futures and
restores those cell futures after reopen. Navigation source hashes are unchanged. Secondary-store
candidate selection and resumed reverse document context still fail. Walks controls are absent.
Native menu Actions work, but editable fields expose no Text/EditableText interface. No native edit
pass is claimed.

All native processes are stopped. No commit or push was made. Source profiles, launch plans,
captures, transcripts, navigation hashes and closed-store dumps are under `build/ux-full-20261010/`;
`closed-store-dump-manifest.json` indexes the final dumps. Continue any new fix as a separate
implementation phase, then rerun its affected interface steps. The complete set of unvalidated
substeps is stated in the audit; no unit-test result fills those gaps.

## User objective and decisions

The active request is:

> Complete the remaining work in the report, then run the full user journey suite in parallel,
> starting with J16.

The report is `design/ux_link_colour_offscreen_routes_2026-10-09.md`. The full real-work contract is
J1–J16 in `design/ux_workflow_real_work.md`. The user reminded us that no format is fixed and GLSL
is as malleable as everything else. Prefer correct architecture and behavior over preserving
prototype interfaces or shader layouts. Preserve the append-only and addressing invariants in
AGENTS. Authorial editions remain the author's decision, reviewable/editable at publication.

The user then requested this restart prompt. Do not merely summarize or propose the next work:
resume implementation, verification and the audit. Do not commit or push without a fresh request.
The original publication swarm journeys are earlier work; do not silently count them as additional
freshly verified journeys in this J1–J16 audit.

## Preserve the workspace

Branch: `main`. There are approximately 103 modified/untracked paths, including a large preexisting
NURBS/UI prototype and new work from this continuation. Preserve everything; do not reset, clean,
stash away or overwrite unrelated changes. No commit was made during this continuation.

Use skills:

- `.claude/skills/xuzz-ui-design/SKILL.md`
- `.claude/skills/ux-validation/SKILL.md`
- `.claude/skills/system-architect/SKILL.md` for the measured performance optimization

UI/UX skills were announced and read. The system-architect skill was applied to the measured media
classification bottleneck with independent quality, architecture and profiling reviews. At most
three subagents plus the lead may be active. The user explicitly authorized parallel journeys. All
validation runs change no product code; fixes and reruns are separate phases.

Every graphical/device run is headless with isolated data/config/cache. Outside make:

```sh
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 \
  XDG_DATA_HOME=<run>/data XDG_CONFIG_HOME=<run>/config XDG_CACHE_HOME=<run>/cache <command>
```

Never edit compiling source/headers during a build. Never rebuild/relink while native application
processes are active. Earlier concurrent edits caused compiler crashes. All known native processes
and builds had stopped when this handoff was written. Verify before restarting.

Default shell sandbox execution has previously failed because of the `.agents` symlink. Shell calls
used `sandbox_permissions: require_escalated`; automatic review accepted them. There was no approval
rejection to report or permission question to repeat.

## Exact immediate state

The full native-AccessKit-enabled build succeeds after the latest media cache implementation:

```sh
make -j$(nproc) all render-link-stress \
  ACCESSKIT_DIR=/tmp/ux-accesskit-linux GLEDITOR_ENABLE_A11Y=1
```

Log: `build/ux-full-20261010/build-media-cache.log`.

A subsequent test-only rebuild also succeeds: `build/ux-full-20261010/build-panel-test-fix.log`.

**One newly exposed panel-layout test still fails and needs investigation/fixing.** Latest test run:
`build/ux-full-20261010/media-cache-tests-final.log`. Of 27 tests, 26 pass: seven Session
media-cache tests, eight LinkPanelOverlay tests and eleven of twelve LinkPanelPresentation tests.
Failure:

`LinkPanelPresentationTest.ExactRangePrefixesSurviveLongContextAndSmallLineLimit`

- At width640, UI scale2, all eleven action labels have fitted0×0/truncated controls.
- At width1280, scale2, the panel crosses a protected reading band:
  `leaf.bounds.bottom + leaf.bounds.height == 712`, expected `<= reading->bottom == 450`.
- This appeared after updating the test's stale nine-action ID fixture to eleven IDs to account for
  the newly added Previous link and Next link controls. The preceding run had eleven identity count
  mismatch exceptions. Those mismatches are fixed; this remaining failure is real evidence that the
  expanded panel's constrained layout/paging needs attention, not a reason to weaken the readability
  or exclusion assertions.
- Test and fixture are in `tests/zigzag/link_panel_overlay_test.cpp`, `actionIds()` near line53,
  failing test near line214. Inspect `apps/common/ui/link_panel_presentation.cpp` and overlay
  paging.

**The corrected retained-caret RTL test has compiled but has not been rerun.** Run it after the
panel fix. The first focused library run passed74/75 tests, including all three palette backends.
The failed precondition used Latin-first mixed text to demand reversed Hebrew placement. Actual
native layout guesses one direction for the page. The test now separately uses a Hebrew-first DejaVu
Sans page with a genuine reversed-X assertion, while retaining the original native ligature fixture
and exact cached-coordinate checks. Mixed-script run resolution is a preexisting separate
limitation; do not claim it was fixed.

**Native AccessKit crash fix is compiled but unverified.** Empty TextRun values were previously
omitted by the adapter, causing an AccessKit consumer Rust panic during publication:
`text.rs:696:45 node.data().value().unwrap()`. The adapter now explicitly supplies empty values for
TextRun, TextInput and MultilineTextInput. Native Action/EditableText and modifier-drag gates still
need fresh runs. No successful native gate or full journey has been claimed.

**Media classification cache is compiled and its seven behavior regressions pass, but fresh dense
performance measurements have not run.** This is the next performance verification after fixing
panel layout and freezing/rebuilding code.

**J16 and the full fresh J1–J16 audit have not started.** Do not carry forward older all-pass
claims.

## Changes implemented during this continuation

1. `apps/common/xanadu/link_panel.cpp`: Previous link/Next link visible controls use the existing
   `nav::StepLink` command boundary. Eleven actions total. Tests verify command parity. This is
   sequential access to obscured owners, not independent picking of every saturated ribbon.
1. `apps/common/ui/xanadoc/beams.cpp`: page-to-cell explicit link anchors preserve the link hue;
   removed cyan cell overrides. Geometry is retained while settled, with invalidation on document
   basis/opacity/edit/page readiness/visible range, projection, context, configuration and reach
   motion. Only locked transclusions bypass retention for traveling pulses. No link/endset cuts.
1. Placed document basis: `Doc::modelMatrix()` retains `model` basis and replaces translation;
   `animateMoveTo` changes translation without replacing the basis. Rotated edges order in page
   coordinates, derive extents from edge vectors and use outward attachment vectors. Tilted terminal
   accents are conservatively omitted; tilted transclusion looms use original strands.
1. Live generic DocumentPalette paper/ink path from settings through AppState/DrawBudget and
   DrawUniforms to shared GL/GLES/Vulkan glyph shaders. Packed RGBA offsets72/76/80; uniform size84
   bytes. Monochrome document rows change; colored rows and ordinary UI retain their colors.
   Highlight ink uses linear-sRGB contrast handling. Settings keys: `page.paperColour` and
   `page.inkColour`, with defaults and packed-integer validation. Real pixel/picking tests on all
   three backends passed in the earlier focused run.
1. `--hold-mods` synthetic input in `src/app.cpp` and ordered script steps allow ordinary Alt-cell
   dragging. Modifier automation unit test passes, native gesture proof still pending.
1. LinkPanelOverlay exposes last-frame visible document companions only for the current link owner.
   Explicit Enter frames the chosen endpoint with a visible companion without selecting that
   companion or recording a visit. Do not claim aggregate framing of arbitrarily many documents; the
   current composition is pairwise.
1. `apps/vquery/main.cpp`: normal default store loading uses PermascrollRegistry's default user when
   no explicit permascroll directory was supplied, matching other programs. Fresh J6 PTY handoff
   still required to prove it with UI-authored data.
1. `include/gleditor/doc.hpp`, `src/doc.cpp`: Page retains optional first-glyph caret positions and
   placed box metadata at construction, before atlas upload can fail. Caret/box queries reuse
   retained lines and built line height, without FontManager/layoutFrom. Tests cover ligatures,
   actual RTL native placement, blank lines, media boxes, byte renumbering, reflow and repeated
   zero-shaping queries. No full glyph strings/shaping are retained for this fix.
1. `src/a11y/platform_accesskit.cpp`: explicit empty text values, as described above.
1. `apps/common/ui/xanadoc/session.hpp/.cpp`: Session-owned media classification cache. Keeps
   existing owning-vector API. Key is loaded store slot/version; entry also checks document identity
   and operation count. Clears on Session epoch changes. Admission is conservative: own local
   binding, complete physical permascroll bounds, unsegmented local pieces only. Hits recheck cached
   piece bounds and segment metadata, so adding MIME metadata without an operation forces
   recomputation. External/imported/bootstrap-missing/segmented data bypasses caching. This avoids
   stale negative results when remote bytes arrive at the same version. TRACE category `xudu.media`
   reports reuse. The two existing consumers are serialized renderer paths; this does not make
   Session globally thread-safe.

New meaningful cache tests: `tests/zigzag/session_media_cache_test.cpp` (seven passing cases): plain
repeated queries, media offsets across edits, owning result snapshots, identical version names in
separate stores, missing external→live SVG at unchanged epoch/opcount, same-version segment metadata
changing plain→media, and bootstrap missing→available with physical permascroll still empty.

## Performance evidence and next acceptance measurement

Evidence root: `build/ux-full-20261010/stress-dense/`. Prepared stores are component/navigation
diagnostics, never UI authoring passes. Source ops/tables and real permascroll hashes remained
unchanged.

- Original ten links: median steady frame348.702ms.
- Original100 links:320 resolved strands, median5627.910ms.
- Original200 links:640 strands, median11455.342ms.
- Retained settled beam geometry: ten8.329ms,10012.244ms,20020.385ms.
- After retained caret fix,100 still had first crossing6839.67ms and preparation8834/6933ms, with
  median13.631ms steady frames.
- Explicit100-owner selection still prepared for5684.365ms, median14.989ms steady with zero steady
  shaping. This is not responsive selection acceptance.
- Valid child-owned GDB sample found render thread in libmagic→MagicMimeDetector→classifyRun→
  Session::mediaSpansFor→Xuzz media resolver→LinkBeams::visitDocumentEdges. Every endpoint
  reclassified the same11000-byte plain run. Evidence: `dense100/gdb-final/run.log` around1096–1119.
  One sampled stack, not statistical profiling.
- The new cache targets exactly that measured path. Proposed acceptance is selected preparation
  below100ms in the same software OpenGL diagnostic, cold crossing strongly improved, all100
  identities reachable and wrapping without cuts, steady zero shaping preserved. Measure, do not
  assume these targets pass.

Diagnostic scripts:

- `stress-dense/final-plan.json` and `run-final-plan.py`, with exact recorded arguments.
- `crisscross10`, `dense100`, `dense200`, `selection100`, `cycle100`, `pointer-overlap5` and eight
  placed-page/dark/scale component cases.
- The runner refuses existing evidence directories. Profiler prepared fresh output paths named
  `benchmark-media`, `selection-media`, `cycle-media`, `pointer-media`; inspect plans before
  running.
- Run benchmarks alone: other software GPU/native programs or builds contaminate timings.
- Full100-cycle was previously deferred because selection remained seconds. Rerun after cache
  measurement. It proves owner access through default controls, not every endpoint Entry.
- Pointer overlap case uses actual label-bound pointer clicks on place/Cross/Next/Previous, not
  native AT actions. Check each owner change resets both independent member/occurrence cursors.

New optional native component harness: `tools/render-link-stress.cpp`, built by
`make -j$(nproc) render-link-stress` with the same AccessKit variables. It uses real Session,
LinkContext, LinkBeams, Doc and Renderer over cloned fixtures. Cases include coplanar, rotated,
roll120°, depth, foreground occlusion, one-visible, dark paper and enlarged UI/font. Arguments and
source-integrity output are in the plans. It explicitly isolates XDG inside each output fixture. Run
GL, GLES and Vulkan as appropriate; inspect actual frames and matrices. Diagnostic placement is not
a user-facing rotation affordance or a full journey pass.

Remaining report also requires mixed cell endpoints and the no-sworph KJV Psalm entry that
previously sent the notes companion offscreen. Existing `/tmp/run-link-colour-route.py` and KJV
tools are prepared-fixture diagnostics; adapt to the frozen binary and preserve exact chosen
ranges/source hashes. Shallower-angle arrangement is still deferred/unwired despite prototype code
being present.

## Native harness and tooling

- `tools/ux-atspi-client.py`: isolated DBus/native AT-SPI Action, Focus, EditableText, Text,
  snapshots. Includes exact text equality assertions. `--cwd` must point at repository assets.
- `tools/ux-pty-client.py`: real PTY REPL input, prompt-anchored synchronization, full transcripts.
- Harness inventory: `build/ux-full-20261010/harness-inventory/inventory.md`.
- Native crash/backtrace: `native-enabled-readiness.md`, `rootnative-enabled-backtrace/`.
- Post-fix plans: `harness-inventory/post-fix-plans/{empty-document,trailing-newline}.json`. They
  exercise native Document Action, Custom path Focus/Edit/Text, empty→text→empty→text readbacks, and
  captured frames. Run in new evidence directories, inspect frames/native tree.
- Native Alt-cell-drag smoke: UI-create a cell, inspect fresh frame/bounds, then use actual held Alt
  pointer press/move/release. Earlier creation aborted before capture because of AccessKit; drag has
  not yet been attempted successfully.
- `--click-label` is a pointer action resolved from internal label bounds, not native accessibility.
- Native AccessKit headers/library exist locally: `/tmp/ux-accesskit-linux/include/accesskit.h` and
  `/tmp/ux-accesskit-linux/lib/linux/x86_64/shared/libaccesskit.so`. They were copied from an
  existing Unix build, not downloaded. No installation is needed.
- Three REPL :help/:quit harness smokes passed earlier; these are not journey passes.

Formatting tools and gates:

```sh
/tmp/gleditor-clang-format-19/clang_format/data/bin/clang-format -i --style=file <files>
/home/ccs4ever/.local/bin/mdformat --wrap 100 --extensions gfm \
  --extensions dollarmath --extensions frontmatter <markdown>
GEM_HOME=$(ruby -e 'print Gem.user_dir') make -j$(nproc) format-check lint \
  CLANG_FORMAT=/tmp/gleditor-clang-format-19/clang_format/data/bin/clang-format
```

Formatting/lint passed before the latest eleven-ID fixture correction. Logs: `gates-resume.log`,
`gates-media-cache.log`. Recheck final changes and this prompt. Always use named Make targets
(`gleditor_test`, not explicit `build/gleditor_test`), because OBJDIR's extra slash can otherwise
make the requested build/path target falsely do nothing.

## Independent reviewers and agent handoff

Prior sessions' agents may not survive a new session. Recreate roles from these briefs, not assumed
liveness. All were stopped/idle or failed on usage limits, with no active native children at
handoff.

- Design/quality agent implemented caret cache and test fixtures; files are frozen. Notes:
  `/tmp/media-spans-cache-audit.md`. Seven media tests compiled and pass.
- Pathology/profiler agent owns dense evidence and final plans. It identified the libmagic path,
  preserved source hashes, and was waiting for cache-build release. Its latest turn failed on a
  usage limit before executing new cache benchmarks. No performance pass was claimed.
- Native reader/harness agent owns post-fix plans and will run readiness gates, then J16 first.
  Architecture review: `harness-inventory/media-cache-architecture.md`.
- Independent aesthetics agent prepared criteria but reviewed no final new frames. Latest turn
  failed on a usage limit. Use an independent nonwriter to inspect the final native captures.
- Independent UX reviewer prepared criteria and graphical authoring plan; no journeys started:
  `graphical-review/diagnostic-criteria.md`, `authoring-plan.md`, `source-review.md`.

## Continue in this order

1. Inspect/fix the remaining eleven-control constrained panel layout failure. Preserve readable
   labels and protected content; use actual paging/docking behavior where needed. Rebuild only after
   source freezes and native processes stop.
1. Rerun retained caret, palette/automation/a11y, media-cache, full panel presentation/overlay,
   navigation/gutter/anchor and system settings checks. Broaden generic tests for the changes as
   warranted. Known earlier results:74/75 focused lib;17 navigation;24 settings;8 overlay. Do not
   present the earlier failed test as passed. Run real backend comparison, inspect its outputs.
1. Freeze product code. Measure fresh dense100 cold/selection first, then all100/wrap and200
   scaling. If cache does not resolve the measured hot path, diagnose and fix before moving on.
1. Finish dark/rotated/depth/offscreen/scaled/mixed-cell and KJV companion diagnostics. Have
   independent UI aesthetics and UX reviewers inspect actual frames and accessibility evidence.
   Update the remaining report with bounded acceptance and exact unresolved findings.
1. Prove native AccessKit empty/newline/edit gates and modifier-cell-drag harness. Fix harness gaps
   first, using ordinary input paths. No fabricated native Action success from pointer scripts.
1. **Start J16 first**, through real UI authoring. Then run the remaining J1–J15 audit in parallel,
   at most three subagents. No product code edits during validation. Fix new findings separately and
   rerun affected journeys; a No affordance is a finding, not a Store API workaround.
1. Merge fresh reports into `design/audit-2026-10-10-real-work.md` or a clearly dated successor. The
   current draft still says audit pending and contains historical implementation/test status; update
   it with the final frozen evidence, never assume an old report is current acceptance.

Suggested journey ownership:

- Reader runner: J16 first, plus separately evidenced J11 if the authored context covers it.
- Graphical runner: J1–5/J10/J12, persistence, and closed UI-authored handoff data for terminal
  consumers. Existing detailed plan is in `graphical-review/authoring-plan.md`.
- Terminal runner: J6–9/J13. J9 is independent; other journeys wait for closed UI-authored stores.
- Lead: J14–15, keyboard/native accessibility repetitions, persistence and merged reporting.

Every run gets its own XDG profile. Consumer handoffs copy the complete closed author registry/data
profile with matching permascroll, not a fabricated adjacent directory or --permascroll escape.
Retain frames, native trees, PTY transcripts, `xudu-dump` after close/reopen, identities/cell refs,
source hashes/provenance and private activity growth. Prepared fixtures, unit tests, direct APIs and
command-line-only authoring never count as a journey pass.

Important contract details:

- J3 requires actual pointer selection/drag/pouch reuse; --select alone is not proof.
- J7 uses real UI-authored12/8/5 budget and later changes artwork to10, yielding25→27. J8 reuses
  saved results without retyping. J13 consumes preceding program outputs, never manually recopied
  values. J9 asserts rules at the actual Prolog prompt and saves/reconsults them.
- J12 source return for saved-cell quotation is currently explicitly unavailable; attempt the real
  control and report the exact fresh failure, not a whole-journey pass based on shared bytes.
- J14 requires all value kinds, an explicitly selected operation annotation and actual foreign cell
  reference. J15 requires three branch versions with text and slice comparison/navigation.
- J16 UI-authors two left/three right members, repeated quotation, distant cell and overlapping
  second link. It checks independent cursors, both full endsets, actual document/cell Entry,
  companion, two private activity futures, Walks preview/reference/annotation/reopen, and cancelled
  unavailable target. Navigation must not append to visited document/slice stores.
- Activity Forward default is Alt+Shift+Y; document-to-cell link finish is Alt+Shift+F.

Keep the user informed with meaningful updates. Finish authorized work; do not stop at a plan or
claim completion while these checks and journeys remain unrun.
