# Sixth increment validation

The sixth increment repairs UI-authored multipage Link entry through the generic document reflow
index. `Doc::reflowFrom()` used to rebuild GPU-backed pages without refreshing `pageEntries`, the
metadata that indexes every shaped page. Text appended beyond the old built frontier could leave the
exact endpoint unindexed, so the existing pending Link entry could never build its endpoint anchor.
The repair recomputes changed pagination until a matching unchanged boundary, splices the suffix,
keeps unbuilt slots sparse, shifts retained offsets and page poses, and invalidates the cached index
for same-count length changes. Stale queued shaping is discarded. The existing priority builder
continues to allocate wanted pages within its frame budget.

The patch changes `src/doc.cpp` and owning `tests/lib/doc_gap_test.cpp`; no public API, camera, Link
command, serialization, source operation or reader-activity implementation changes. Metadata
preparation is outside drawing. Changed pages are shaped for metadata and again for GPU rebuild; no
optimized performance-budget acceptance follows from this O0 build.

## Frozen candidate and regression proof

Parent commit: `78f692e5cf1d5005381120459e6b09c63add6105`. Product/test diff SHA-256:
`c68b1a28f5c828eddddd163594811e39e9a2292ee754310ca1c4118cdd17618e`. The frozen native application is
`build/xuzz`, SHA-256 `404c21bc477c8dd5324bbba7957545f9de50ba52b0f96f38b94dfb3bd68a6cbd`, paired
with `build/libgleditor.so.0`, SHA-256
`6964780c202365c8da5ada84fad51f0d21fc621a0c2db72ad0ebc792b75ffcf6`. The library test executable is
separately hashed as `e550b8a10e3015a725458f6012e2b8365c3d69cf6240332eb9141bf0f3f1594a`. Exact
sources, binaries and compressed logs are indexed in
[`evidence/tests/manifest.json`](evidence/tests/manifest.json).

The new append-after-break regression failed against the old library: one indexed page instead of
two. The candidate passes 25/25 DocGap/DocPageBudget tests, including append/grow/shrink and sparse
page-slot remapping. The first candidate compilation failed because derived TEST_F bodies did not
inherit the fixture's private friendship; edits were moved into fixture helpers, without exposing a
production API. Both diagnostic and successful build logs are retained.

## Headless checks and open capture gate

- The sequential full library suite passes 787/787, UI 233/233 and Xuzz 99/99. The earlier owning
  25/25 pass overlaps those full-suite tests and is not added to their count. The first concurrent
  library run passes 786/787; it fails the assertion that a distant priority page builds in the same
  time-budgeted call as the viewport page. The sequential full rerun passes that assertion without
  changing code, test or budget. Both results are preserved.
- Three strict `tools/compare-backends.sh` runs fail, so the complete core renderer gate is open.
  Ordinary GL/GLES frames are identical and Vulkan differs by 1,186/480,000 pixels (0.2471%, maximum
  channel delta 4). The first and final runs fail overlay equality in document glyph pixels. The
  sequential run passes overlays, picking, culling, atlas growth and minified text, then fails
  configured Vulkan 4-versus-1 recording capture equality. That small sample does not reach the
  actual split-recording threshold. No equality criterion or tolerance was changed.
- A control using the same `build/gleditor` executable with frozen old library
  `5257e33b1de539b487bce0082e287ea7d5539932f915da50e8182738afeb3054` also fails GL/GLES overlay
  equality in glyph bounds `(173,309)–(554,424)`. The configured Vulkan control frames match. The
  failed candidate Vulkan capture differs within the same glyph bounds, by a maximum 23 levels. This
  reproduces an existing overlay capture finding; it does not establish the exact cause or certify
  threaded recording. All failed logs and lossless images remain under
  [`evidence/tests/`](evidence/tests/), with raw PPM paths and hashes in provenance manifests.
- The broad E2E extension was explicitly excluded using a nonexistent `XUDU_TEST_BIN`. Its
  historical whole-page ink threshold remains open. O0 behavior tests do not accept optimized
  performance budgets.

Repository format checks pass. Lint exits 2 with exactly the same 37 Markdown diagnostics as the
fifth checkpoint and none in touched files; configuration harmony, UI text policy and all seven
policy tool tests pass. Gate logs, exact exit codes and uncompressed hashes are included in the test
manifest. The working tree contains no generated source fixture or third-party changes.

## Native journeys and independent review

Three Luna validators use the frozen candidate through native controls and default chords, with
fresh XDG data/config/cache roots, dummy audio and software rendering under X11/Xvfb. They author 80
source lines and a target, create one Link, click visible Enter, and inspect actual physical-page
continuation through ordinary wheel movement. The normal exact range is `[0,7439)`; forced input
uses `Ctrl+Return` at byte 3719 and totals `[0,7438)` because splitting the typed chunks omits a
separator newline. Occurrence counts remain distinct from physical page counts. The governed native
UI font-scale form changes chrome, not document text.

- [GL report](evidence/gl/report.md): normal and forced runs pass at 800×600 and 1440×1000, all at
  UI scale 1.5. Captures visibly show selected later-page lines. The 800 forced run has a reliable
  pre-entry activity snapshot: one root record before Enter, two records immediately after, and the
  same two after wheel/Origin/Back. The other three snapshots named `before` are already after Enter
  and cannot establish its delta. The 1440 normal variant invokes Back directly from the entered
  source and selects the profile root; Origin then selects Doc 3.
- [GLES evidence](evidence/gles/README.md): normal and forced runs pass at 800×600, UI scale 1.5,
  with incremental pans showing selected physical-page continuation. The original snapshots named
  `before` are already after Enter. The
  [supplemental forced run](evidence/gles/supplement-preenter/visit-delta.txt) uses line-buffered
  output and a 20-second native preview pause. It captures one root visit/current index 1 before
  Enter, two visits/current index 2 immediately after, then the same two/index 2 after wheel, direct
  Back and Origin. Both source operation dumps are byte-identical across all three stages. Its 12
  PNGs match the raw captures pixel-for-pixel.
- [Independent Vulkan and visual review](independent-final.md): normal and forced sources pass at
  800×600; a further forced run applies UI scale 1.5 and invokes Back before Origin. The default
  forced run isolates the pre-entry activity delta from one root to root plus one child. Normal and
  scaled forced snapshots named `before` are late and do not isolate that delta. The reviewer
  compares actual GL/GLES/Vulkan captures, accepts the bounded entry/continuation repair, and
  records the dense 800-pixel dock and scope limits.

The source operation snapshots compare byte-for-byte through observed navigation; reliable true
pre-entry runs also cover Enter's no-source-write contract. Completed-visit records stay unchanged
through wheel, Origin and Back. The order Origin then Back produces a Back-at-root no-op; it is not
Back restoration evidence. Direct Back variants are identified separately. Preliminary wrong-tab
assertions, oversized pans, bounded timeouts and an incorrectly labelled default-scale Vulkan run
are retained as diagnostics, never acceptance evidence. Lossless PNGs retain raw-frame hashes;
private profiles, primedia and raw PPMs remain outside Git. The lead independently verifies all 210
checked-in PNGs against 216 retained sixth-increment raw PPMs by exact RGB equality; every PNG has a
matching original. The mappings and RGB hashes are in
[`roundtrip-audit.json`](evidence/tests/roundtrip-audit.json).

## Repository gates

Repository `make format-check` passes. `make lint` exits 2 with the exact same 37 Markdown
diagnostics as the fifth checkpoint and none in touched files; configuration harmony, UI text policy
and its owning tool tests pass. Final format/lint logs are archived under
[`evidence/gates/`](evidence/gates/). Frozen source and binary hashes are unchanged after review.

## Acceptance limits

This is a bounded prerequisite for multipage J24 and reader restoration J25. It does not establish
complete J24/J25, full J1–J16 preservation or J17–J32 acceptance. Repeated occurrences,
pending/unavailable cancellation, stale target replacement, cell/history endpoints, branch reopen,
actual enlarged document text, native AT-SPI, optimized performance, publication-service audit/D2,
the historical whole-page ink threshold and complete 16+16 journey coverage remain open. The native
internal accessibility tree is exercised independently of absent platform AccessKit/AT-SPI support.
