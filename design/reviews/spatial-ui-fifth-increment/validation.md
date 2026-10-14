# Fifth increment validation

The fifth increment adds a governed `Ctrl+Alt+Shift+F` action for `ui.fontScale`. It opens the
existing native form with the current value, accepts a positive finite number representable by the
UI, writes a typed double to the existing `system://ui` setting, and repoints that system document
so the live watcher applies the value. Malformed input, overflow, underflow and Escape leave the
setting unchanged. The system-document notes distinguish chrome typography from document text size.
This reuses the native form, command dispatcher and typed settings adapter; it adds no new domain
service, camera, view host or navigation path.

In C++, `Views` opens the existing native `Form` and handles the answer callback through the render
thread state adapter. The callback calls the existing typed system-setting update API and repoints
`Session`; the action uses the existing sovereign keymap and Vortex command-dispatch path.

The final app binary is `build/xuzz`, SHA-256
`13e0efc05161179adac8ebc5f1f71c7f5d9279bf865cf6a8e957219cb0bc0a08`. The corrected owning-test binary
is `build/xudu_test`, SHA-256 `e53564fb6da78a65749d945c757b1ae54ac473547a22d1988c08e2ea21d31101`.
The increment starts from parent commit `975d8b1b0fb2b08659aa3db04ed81071d0526a43`. Hashes for the
exact product files, test files and binaries are listed in
[`evidence/tests/manifest.txt`](evidence/tests/manifest.txt).

## Results

- The UI suite passed 233/233, the Xuzz suite passed 99/99, and the focused system-document suite
  passed 24/24. The complete test logs are compressed under
  [`evidence/tests/unit/`](evidence/tests/unit/).
- `E2EBinaryOrchestrationTest.uiFontScaleUsesGovernedNativeForm` passed 1/1 on OpenGL, OpenGL ES and
  Vulkan, run sequentially with isolated profiles. It verifies malformed, overflow and underflow
  input and Escape as no-ops, stores `1.5` as a typed value, checks the setting after reopening the
  same profile, and validates the before/after/reopen captures. The PNGs show the test-owned setting
  flow at 800×600; the independent visible-control journey and its captures are indexed in
  [`evidence/font-native/report.md`](evidence/font-native/report.md).
- The independent native font-scale journey passed at 800×600 on all three backends and at 1440×1000
  on OpenGL. It entered an eight-line, 654-byte Link source at scale 1.5, navigated Enter, Origin
  and Back, and restarted the same profile. The visited source operation dumps match before and
  after navigation; the UI system-document setting records the expected typed update. The font edit
  itself adds no reader visit. This is bounded single-page evidence, not a multipage acceptance. The
  existing short-Link E2E was not rerun: this isolated profile journey already exercises Enter,
  Origin and Back at scale 1.5 on all three backends.
- `tools/compare-backends.sh` passed under Xvfb using the core `build/gleditor` binary
  (`9d25646af8e667b684000eae1c0c0491a78e1957a4298dc5ffff8b5b385773c4`). OpenGL/OpenGL ES are
  pixel-identical (0/480,000 differences); Vulkan differs by 1,186/480,000 pixels (0.2471%, max
  channel delta 4). Overlay, picking, culling, atlas-growth and minified-text checks also passed.
  The broad E2E extension was explicitly excluded with a nonexistent `XUDU_TEST_BIN`; the whole-page
  ink threshold remains open. Lossless PNGs and the compressed comparison log are in
  [`evidence/tests/backend-compare/`](evidence/tests/backend-compare/); raw PPMs remain outside Git
  in `/workspace/spatial-build/fifth-compare-raw/`.
- The 15 checked-in PNGs match their raw PPM captures pixel-for-pixel. `evidence/tests/SHA256SUMS`
  verifies 59 evidence and raw-capture files; raw font E2E PPMs are under
  `/workspace/spatial-build/fifth-test-raw/font-scale/`.
- The previous 785/785 core library result is reused only after confirming its `build/gleditor_test`
  SHA is unchanged (`3d23a7f6f09ea7d5161e5bd9e9a4c23dd3a34b59e2475141550cf2a0d3488615`). It was not
  rerun as part of the fifth increment. The inherited 1117-test total is not presented as a fifth
  increment result.

The font-scale E2E first failed because its fixture wrote the seed system document under
`config/xudu/system/ui` while the running app reads `config/xuzz/system/ui`. The fixture path was
corrected; the application binary did not change, and the corrected test binary passed all three
backends. Both the first failure log and the incomplete accidental unfiltered `xudu_test` run are
preserved under [`evidence/tests/diagnostics/`](evidence/tests/diagnostics/) as diagnostics, not
acceptance results. The unfiltered run is not the 233-test UI suite and is not counted as one.

## Bounded journey findings

Repository format and UI text policy checks pass. `make lint` exits 2 with the same 37 Markdown
diagnostics as the fourth checkpoint, all in unchanged files; no touched file has a diagnostic. The
final build, formatting and lint logs are archived under [`evidence/gates/`](evidence/gates/), with
exact commands, exit codes and uncompressed hashes in `manifest.json`.

The earlier native scalar-setting probe could not reach a usable value editor from the specific
settings-document focus state it tested. That bounded probe did not establish that all native routes
were unavailable; the new governed action supplies a direct path. The native form journey shows that
the action is discoverable, labelled, live and persistent. At 800×600 and 1.5 scale, the folded Link
investigation status is visibly clipped, and the investigation cannot fully expand at that size; see
the independent visual review in [`independent-final.md`](independent-final.md). Invalid submission
closes the form, so the reader must reopen it to correct the value. The native text journey covers
valid entry; invalid and cancel no-op checks are covered by the owning E2E. Native AT-SPI is
unavailable in this build.

The occurrence fixture has one available occurrence for each authored member; repeated occurrences
were not exercised. Its bounded preview adds no visit and leaves source operations unchanged. The
cancel chord was sent at the origin with no pending target, so it does not validate cancellation of
a pending or unavailable endpoint. See
[`evidence/occurrence/validation.md`](evidence/occurrence/validation.md).

The two 80-line multipage preview runs at 800×600 did not record a completed target-document entry.
One fixture includes an authored page break; both previews report page 1/1. No cross-page
continuation or return behavior is validated. Source operation snapshots match before and after, and
neither run creates a completed visit. See
[`evidence/multipage/report.md`](evidence/multipage/report.md).

## Open acceptance limits and next task

The next task is to investigate the UI-authored multiline Link pending-anchor and page-build
lifecycle. Both selected range endpoints must be ready before the existing bounded preprojection
fit. A normal `Ctrl+Return` page break must preserve its authored operation and must not be treated
as an Enter visit or trigger an invented reader write. Then validate actual enlarged document text
through existing layout tunables; `ui.fontScale` changes chrome only. Continue with cell/history,
pending, cancellation and branch-reopen cases. Audit existing publication services before advancing
the separate D2 history prerequisites.

Repeated occurrences, pending/unavailable cancellation, branch reopen, cell/history endpoints,
actual enlarged document text, publication-service audit/D2, native AT-SPI, optimized performance
acceptance, the historical whole-page ink threshold and complete 16+16 journey coverage remain open.
The bounded fifth increment does not establish full J1–J16 preservation or complete J17–J32
coverage.
