# Native drag regression repair

The original pinned/current comparison established two stale target coordinates, not regressions in
native drag behavior. `aDraggedSelectionLandsWhereItIsDropped` estimated a line end from the width
of selected `beta` and its trailing space; native picking landed at byte 14. The foreign-run test
called `770,300` empty space, but its captured frame and native kind-2 pick identify publication
paper at that position.

The repaired selection test uses the recorded Home and Shift+End keys to capture the entire native
line, then drops past its measured final glyph. It retains the strict byte-16 insertion assertion
and verifies the quote starts six bytes into the original primedia span. Both cases derive an empty
canvas margin from a solid 9-by-9 black patch in the native calibration capture, away from
top/bottom UI. No application code changed. Selection calibration and drop sessions now retain
separate profiles, transcripts and captures rather than deleting preceding evidence.

The foreign-run case retains all three exact global source-span comparisons, a distinct reader store
identity, the unchanged published author's operation count, pouch origin provenance, and cancelled
scaled/unscaled drag checks. The selection case also retains the offscreen no-hit drop check. These
are prepared-fixture native regression checks, not complete journey acceptance or AT-SPI results.

Final comparison runs use isolated scratch roots outside Git, the six recorded environment
variables, OpenGL software rendering, actual profile keyboard and pointer input, and native
captures. PNGs are lossless conversions of those captures; original PPM files and full stores remain
in scratch. See the manifest and the pinned/current original-failure excerpts for provenance. Final
results are recorded in the paired native run logs.

Both repaired cases pass on the pinned application and on `c1ab2314`: **2/2 in each isolated run**.
The changed C++ passes clang-format checking and `git diff --check`. The retained native application
hash did not change during the comparison. These timings reflect O0 software rendering with
concurrent validation; they do not establish a performance budget.
