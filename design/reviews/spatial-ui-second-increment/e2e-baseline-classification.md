# Broader native E2E baseline classification

The four OpenGL failures recorded for the first increment also reproduce against the repaired pinned
baseline, `e9726a6b`. They are not regressions introduced by `4f2d3fac`. This establishes baseline
classification only; it does not accept a failing gate or any full journey.

The same compiled test driver ran only these four tests against each application binary. Each
variant had its own working directory, `build` fixtures, screenshots and profile data. The
application received the tests' exact prepared stores and real pointer/keyboard input. Assets and
sample fixtures were symlinked from the repository. The native Docker runner supplied fresh data,
config and cache roots, SDL offscreen video, dummy audio and software Mesa. Both binaries use the
behavior build with `-O0 -g0`; no timing/performance conclusions follow. The baseline includes the
previously recorded portability repairs needed to run it on this environment.

The implementation binary was frozen before subsequent drawer repairs. Binary, library, test driver
and test-source hashes are in
[comparison-manifest.json](evidence/e2e-baseline/comparison-manifest.json). This report compares the
first increment, not the later drawer correction.

| Check                                               | Pinned baseline                                                          | First increment             | Finding                                                                                                                                                                                                                                                                                         |
| --------------------------------------------------- | ------------------------------------------------------------------------ | --------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `textSurvivesAtWholePageDistance`                   | 89 ink pixels; requires 100                                              | 89 ink pixels; requires 100 | The entire captured RGB frame is identical. Text is present, but this frame fails the unchanged threshold.                                                                                                                                                                                      |
| `storePanelCreatesObjectsThroughNamedDrawnControls` | `New Xanadoc` named click finds zero visible actionable controls         | Same failure                | The accessibility tree names the button, but the actual drawer does not draw it. This is an existing interface defect, not a successful creation journey.                                                                                                                                       |
| `aDraggedSelectionLandsWhereItIsDropped`            | Quoted `beta` plus a space inserted at position 14; assertion expects 16 | Same failure                | The actual drag preserves the selected source-byte addresses. The test estimates a destination using selection width, then assumes that coordinate means the end of the line. Native picking selects position 14 in both versions. Later empty-space cases are not reached after the assertion. |
| `ForeignRunDragCreatesAReaderOwnedStore`            | Drop at `770,300` hits page background, kind 2; no spawned reader store  | Same failure                | The test calls this empty space, but the native frame shows publication paper through that point. No empty-space drop occurred, so this case does not establish that an actual empty-space transclusion fails.                                                                                  |

The whole-page [baseline](evidence/e2e-baseline/pinned-whole-page.png) and
[implementation](evidence/e2e-baseline/current-whole-page.png) images are pixel-identical. Their
small text is visible on both pages; lowering the threshold is not an acceptance conclusion here.

The drawer [baseline](evidence/e2e-baseline/pinned-drawer.png) and
[implementation](evidence/e2e-baseline/current-drawer.png) images show `New Slice` and the truncated
open/close row, but omit `New Xanadoc` and `Close Drawer`. The drawer capture uses the E2E-prepared
store with fresh profiles and the actual `store-manager-toggle` interface. Launch records and
accessibility dumps accompany the frames. A separate owning repair must demonstrate visible,
actionable creation and closing controls.

The foreign-run [baseline](evidence/e2e-baseline/pinned-foreign-dropped.png) and
[implementation](evidence/e2e-baseline/current-foreign-dropped.png) images show the paper covering
`770,300`. Both transcripts report the selected foreign run and the same page-background hit. The
varying publication tab names are generated timestamps, so these frames are not claimed to be
pixel-identical. Correcting this check requires an actual empty-space destination established from
the drawn interface, followed by reader-owned-store and exact global-address assertions.

These findings narrow the remaining gate work: repair and verify the drawer; replace unjustified
drag target assumptions with actual drawn-interface evidence; investigate the whole-page ink
expectation independently. The failures remain visible in validation records until their owning
checks pass. No J1–J16 preservation or J17–J32 acceptance claim follows from this diagnostic run.
