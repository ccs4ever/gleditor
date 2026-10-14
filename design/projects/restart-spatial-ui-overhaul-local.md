# Local restart prompt: native Xuzz spatial UI overhaul

Paste the following into the local coding session. Fetch the implementation branch first so the
checkpoint, source changes and native evidence are available together.

```text
Continue the native Xuzz spatial UI overhaul in https://github.com/ccs4ever/gleditor.git.
Clone or fetch it, inspect any existing checkout/unrelated changes, and continue branch
feat/spatial-ui-overhaul. The original design baseline is
e9726a6b3825e794ab86db6cd54f18d9258e75c0 on design/spatial-ui-overhaul-2026-10-13.
The sixth checkpoint is 67435ffdfbddbd536a7d99ea43c465ccbcdd432a; the seventh restart
checkpoint includes this prompt and the glyph-atlas alignment fix. Do not reset unrelated work.

First read AGENTS.md, the architectural/headless rules, .agents/skills/xuzz-ui-design/SKILL.md,
.agents/skills/ux-validation/SKILL.md, design/projects/start-spatial-ui-overhaul.md,
design/projects/spatial-ui-overhaul-status.md, and the seventh-increment README, diagnosis,
independent reviews and evidence under design/reviews/spatial-ui-seventh-increment/.
Follow the recorded contracts, dependency order, existing view/camera seams and delivery gates.
Use Luna subagents to save credits: three journey validators and independent visual review,
with at most three concurrent children. Keep validators from changing product code during runs.

Current fix: src/glyphcache/cache.cpp rounds padded glyph allocation widths/heights to the
existing deepest mip block (8 texels), with zero trailing padding. This keeps mip-grid phase
independent of insertion order; glyph dimensions, positions, colors and APIs are unchanged.
The new tests/lib/glyph_cache_mip_test.cpp draws the same minified A on actual GL/GLES/Vulkan
with/without an unused m request. It failed on all three baseline backends (80 differing pixels,
max delta 45, origins (8,8) versus (55,8)) and passes exactly after the fix. The earlier i-prefix control
passed because its shift was already aligned. tools/compare-backends.sh changes comments only;
comparison criteria remain intact. Historical startup request order was not captured, so do not
claim it was measured directly.

Completed cloud checks: owning 53/53; two independent strict core backend comparisons pass,
GL/GLES ordinary + overlay 0 differing pixels, Vulkan 0.2471%, configured 1/4 frames match.
The small sample did not exercise actual split recording. Broad E2E was explicitly excluded.
Full library first run 789/790, with only unchanged O0 incremental-resize benchmark failing
at 3392.3 ns versus 1000 ns. Isolated candidate 684.2 ns and old-library 727.3 ns both pass.
No threshold was relaxed and optimized performance is not accepted. GL and Vulkan native captures completed with exit 0 and source/visit proofs; GLES and independent
final visual review remain pending locally. Review and finish the three-validator evidence for the 800×600 forced 7438-byte selection, scale 1.5 chrome, Ctrl+Return at 3719,
Enter, physical-page continuation, direct Back/Origin, source purity and visit 1→2→2 evidence.

First local task: initialize submodules, build with GNU Make, and finish the seventh checkpoint's
validation gates. Rebuild on the local environment; cloud binaries and /workspace/spatial-build
helpers/raw PPM paths are provenance, not locally available dependencies. Run the full library
suite (expected 790 cases), UI build/zigzag_test (233), Xuzz build/xuzz_test (99), owning mip/cache
checks and strict backend comparison. Classify unavailable backends honestly. Investigate any
repeat latency failure without weakening the test or confusing O0 results with optimized budgets.
Check format/lint; known baseline is 37 Markdown diagnostics, zero new touched-file findings.
All display/audio work must remain headless, with fresh XDG data/config/cache roots and all six
required variables; use X11/Xvfb-noreset where Vulkan needs it. Retain actual frames and hashes.

Then investigate the existing whole-page ink E2E without lowering its 100-pixel threshold, and
validate actual enlarged document text through existing layout tunables. ui.fontScale already
has a native governed form and enlarges chrome only. Continue J24 repeated occurrences,
pending/unavailable cancellation and stale targets, cell/history endpoints, and J25 branching
Back/Return and reopen. Publication/pin-lifecycle audit precedes D2 history enumeration.
Do not create a second camera/view architecture or simulated missing services.

Preserve J1–J16 and validate J17–J32 through actual controls/default chords/internal accessibility.
One explicit Enter adds one completed reader visit; Back/Origin restore without adding visits;
wheel/inspect/fold add no operations to visited sources. Some sixth snapshots named before were
late; use the corrected pre-Enter proofs rather than infer a delta. Keep native AT-SPI,
optimized performance, whole-page ink and complete 16+16 journey acceptance open where unverified.
Commit and push tested increments to feat/spatial-ui-overhaul, updating status and exact next task.
Do not merge main or deploy. Main's recorded base is c7f70cc757034c006a6ce18d2a11e9c962aecce4.
```
