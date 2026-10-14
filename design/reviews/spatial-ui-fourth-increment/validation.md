# Fourth increment validation

This document indexes the final safe-margin validation for the multiline Link landing. It records a
bounded set of native checks, not acceptance of all J24/J25 behavior or the full J1–J16 preservation
gate.

## Final build

The canonical native build passed with exit 0 in
[`fourth-build-margin.log.gz`](evidence/tests/logs/fourth-build-margin.log.gz). The tested source
was revision `c1ab2314e7bf31db67545aefd8a59f795a0d4756` with the product diff SHA-256 at build time
`17cfb9755b64a65a0829a38e813565303073da3a1b535161345e56950fdb5e06`. Final executable hashes:

| Binary                      | SHA-256                                                            |
| --------------------------- | ------------------------------------------------------------------ |
| `build/xuzz` / `build/xudu` | `52dadabe6cfee1e5994a5e2e297ea35b6e9e6eef150f7f68012e9ff3a949cdf7` |
| `build/xudu_test`           | `6009f8ddb005e59c8ad89ad1aca847a89166580bb30b043ec81fd5771d0f44ed` |
| `build/zigzag_test`         | `e1a3e5a1f4ba9dfab62f84509241df39fb89881cb75d6a8217f868ad5ebee20d` |
| `build/gleditor`            | `4b435e37947f72f8ea3433d0ed1ba67d74f13436fa4bb0500bd308675c5bcf52` |

The library and Xuzz test binaries and the core renderer binary were unchanged for this application
build; their earlier successful logs remain applicable. The final relinked UI binary was rerun.

## Automated results

| Scope                                                   | Result                                          | Log                                                                                              |
| ------------------------------------------------------- | ----------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Native UI suite                                         | 233/233 passed                                  | [`fourth-zigzag-margin-final.log.gz`](evidence/tests/logs/fourth-zigzag-margin-final.log.gz)     |
| Native library suite                                    | 785/785 passed, no skips                        | [`fourth-gleditor-xvfb.log.gz`](evidence/tests/logs/fourth-gleditor-xvfb.log.gz)                 |
| Native Xuzz suite                                       | 99/99 passed                                    | [`fourth-xuzz-full.log.gz`](evidence/tests/logs/fourth-xuzz-full.log.gz)                         |
| Link travel retaining exact document and sibling visits | 1/1 passed on OpenGL                            | [`fourth-margin-link-opengl.log.gz`](evidence/tests/logs/fourth-margin-link-opengl.log.gz)       |
| Same link travel check                                  | 1/1 passed on OpenGL ES                         | [`fourth-margin-link-opengles.log.gz`](evidence/tests/logs/fourth-margin-link-opengles.log.gz)   |
| Same link travel check                                  | 1/1 passed on Vulkan                            | [`fourth-margin-link-vulkan.log.gz`](evidence/tests/logs/fourth-margin-link-vulkan.log.gz)       |
| Selection drag lands where dropped                      | 1/1 passed on OpenGL                            | [`fourth-margin-drag-selection.log.gz`](evidence/tests/logs/fourth-margin-drag-selection.log.gz) |
| Foreign-run drag creates reader-owned store             | 1/1 passed on OpenGL                            | [`fourth-margin-foreign-drag.log.gz`](evidence/tests/logs/fourth-margin-foreign-drag.log.gz)     |
| Core renderer comparison                                | OpenGL/OpenGL ES exact; Vulkan within tolerance | [`fourth-core-compare.log.gz`](evidence/tests/logs/fourth-core-compare.log.gz)                   |

The three geometry tests added for this work pass in the library suite:
`DocGapTest.TextBoundsUnionWideIntermediateLinesWithoutReshaping`,
`DocGapTest.TextBoundsClipToAnInteriorBuiltPage`, and
`DocGapTest.TextBoundsKeepPartialLigatureEndpointFractions`. The full suite also exercises the
retained caret and reflow checks. Its incremental resize measurement was 854.6 ns/update against a
1000 ns test threshold in this O0 build. This does not constitute optimized-build performance
acceptance; the earlier 4625 ns measurement remains part of the historical record.

The core comparison reports 0 of 480,000 differing pixels between OpenGL and OpenGL ES; Vulkan
differs from OpenGL at 1,186 of 480,000 pixels (0.2471%, maximum channel delta 4). Picking, overlay,
culling, atlas growth, coarse text, minified text, and single-versus-four-thread frame checks pass.
The sample has too few page draws to exercise threaded splitting. The comparison used an explicit
nonexistent `XUDU_TEST_BIN` to exclude the broad orchestration extension; it does not claim that
extension passed. Vulkan tests ran with SDL's X11 driver under Xvfb.

## Native framing and persistence

Independent review accepted five native cases in [`independent-final.md`](independent-final.md):

| Size and backend  | Result                                                                     |
| ----------------- | -------------------------------------------------------------------------- |
| 800×600 OpenGL    | All eight selected lines fit above the chrome; first glyph begins at x=30. |
| 800×600 OpenGL ES | All eight lines fit with the same inset.                                   |
| 800×600 Vulkan    | All eight lines fit with the same inset.                                   |
| 1440×1000 OpenGL  | Full selected source and destination passage are visible together.         |
| 640×480 OpenGL    | Readable prefix of about two lines; full selection is not claimed to fit.  |

The UI-authored range is 654 bytes across eight lines; the destination is 23 bytes. Before/after
source operation snapshots are byte-identical in all five runs. The Link, caret selection, visit
state and source/target stores remain governed by existing activity and editor seams.

Raw PPM captures and private XDG profiles are outside Git under
`/workspace/spatial-build/fourth-raw/`. Lossless PNGs and their SHA-256 values are indexed in
[`evidence/tests/current-unit-runs.md`](evidence/tests/current-unit-runs.md) and the wide-frame
inventory is
[`evidence/wide/raw-frame-and-profile-manifest.md`](evidence/wide/raw-frame-and-profile-manifest.md).
The 800×600 frames are [`OpenGL`](evidence/wide/final/800,600-opengl/01-enter.png),
[`OpenGL ES`](evidence/wide/final/800,600-opengles/01-enter.png), and
[`Vulkan`](evidence/wide/final/800,600-vulkan/01-enter.png).

## Implementation boundary

The landing fits retained page-range bounds inside the shared chrome safe area. It waits for the
existing LinkBeams and timeline motion to settle, then queues the final camera fit before
projection. Generation, weak-document, view-index and exact-selection guards reject stale work. The
fit changes no page poses and adds no camera, view host, source operations or visits. Full fitting
is restricted to built single-page ranges with supported glyph geometry; oversized, multipage and
unsupported ranges prioritize the exact beginning and its full line height.

## Historical results and open gates

Earlier checkpoint results remain historical and are not rewritten as green. The prior broad
orchestration run measured 32/36 on OpenGL, and the old O0 resize benchmark measured 4625 ns against
the 1000 ns threshold. The two stale drag destination assumptions were subsequently corrected and
both repaired cases pass on the final safe-margin build; do not list them as remaining work. The
unchanged whole-page ink threshold still fails its historical 89-versus-100 comparison. The broad
orchestration extension was excluded from the final core renderer run.

Still open are native AT-SPI, discovery of a UI text-scale control and enlarged-text captures,
multipage, cell and historical targets, J24 repeated-occurrence/unavailable/cancel variants, J25
branching Back/Return/reopen, the whole-page ink threshold, legacy J3/J11/J15 findings, optimized
performance acceptance, and the complete 16+16 journey set. The 640×480 prefix and the three-backend
800×600 case do not establish those variants or full J1–J16 preservation.

## Repository checks

Repository formatting and UI text policy checks pass. Lint retains the 37 unchanged baseline
Markdown diagnostics, with none in touched documents; other lint checks pass. Exact compressed
build, test and check logs are preserved in [the log archive](evidence/tests/logs/manifest.json),
with SHA-256 hashes of their original bytes.
