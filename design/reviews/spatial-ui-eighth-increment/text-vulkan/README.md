# Native Vulkan enlarged document text

This archive compares the prior red UI-edit run with a fresh native Vulkan candidate run. The
candidate uses the existing Xuzz UI route to set `system://layout` → `readableTextPx` from 16 to
24.0, returns to the source text, closes the app, and reopens it in the same isolated profile.

## Result

The baseline dump contains the authored `SetValue double ctx=160` operation with text `24.0`, but
the run log has no `system doc updated (system://layout)` notification. The old session therefore
did not establish that the live source view consumed the new head. Its captured active-scalar frame
showed the old 16-pixel text size. The original artifacts, including raw captures, are retained
under `baseline/`.

The candidate began from a true pre-edit snapshot: after the semantic query captured cell 160 at 16,
the observer waited for that frame file, recorded a start time, dumped the layout store, recorded
completion, and left a `yes` marker. The snapshot completed within 23 ms. The after dump contains
operation 1284, `SetValue double ctx=160`, text `24.0`; the app log emits one
`system doc updated (system://layout)` callback and accessibility reports cell 160 at `24.0`. The
reopened session uses the same XDG profile and the saved layout dump retains operation 1284.

The source document operation dump is byte-identical before editing, at the pre-edit snapshot, after
editing, after exit, and after reopening. The first and reopen app processes both exited 0. These
are persistence and interaction evidence; the exit codes alone are not treated as a visual pass.

The lead independently inspected the native Vulkan before, live and reopened frames. A common
row-occupancy measurement over the same unobscured body region on GLES and Vulkan finds main glyph
bands about 9–11 pixels tall before and 18 pixels after; the live and reopened frames have 24-pixel
row spacing versus about 15 before. The exact predicate and bands are in the parent
`text-row-audit.json`. Returning to the source also reframes the camera, so this is observed native
visual behavior, not a camera-locked benchmark. The mini-map overlays part of the lower text in the
800×600 viewport, so this proves readable short lines in the visible region, not unobscured display
of all twelve lines. Long-line clipping and other viewport sizes were not retested here.

## Run details

The renderer reports `AMD Radeon RX Vega (RADV VEGA10)`, Vulkan API 1.4.354. SDL uses the
`offscreen` video driver and `dummy` audio driver; `LIBGL_ALWAYS_SOFTWARE=1` is set, with new data,
config, and cache directories under `/tmp/spatial-vulkan-text-enlarged-candidate-20261015/profile/`.
The exact argv, route, environment paths, frame times, logs, accessibility dumps, operation dumps,
exit codes and binary hashes are in `evidence/`. Candidate `build/xuzz` SHA-256 is
`81cd8bde678910f98f81cb67369cff611dfbb1977a0b8d6f281f5f9fd38dace8`; the shared library hash is
`ea4d46e75ce858119626f470045c3cc555956ad2bd4ca9425f3aeffe3e831ce9`. Both match the assigned build
before and after the run. No product source or build artifact was edited or built for this
validation.

`run-native-enlargement.sh` records the reproducible native Vulkan route. It uses a fresh isolated
profile, waits for the actual semantic frame before taking the pre-edit snapshot, reopens the same
profile, records binary hashes, and stores PNG frames with PPM source hashes. The historical
baseline is preserved as captured; PNG copies are in `baseline/png/`.
