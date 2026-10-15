# OpenGL text scale and link navigation

This archive records the successful OpenGL run that combined the native text scale change with
cross-document link navigation. The runner is preserved at `run-document-enlarged.sh`; it records
the original `/tmp` paths and serves as provenance, not as a drop-in rerunner. The app used the
candidate binaries `xuzz` SHA-256 `81cd8bde678910f98f81cb67369cff611dfbb1977a0b8d6f281f5f9fd38dace8`
and `libgleditor.so.0` SHA-256 `ea4d46e75ce858119626f470045c3cc555956ad2bd4ca9425f3aeffe3e831ce9`.
The before and after guards match. The journey exited 0.

The UI route opened `system://layout`, selected `readableTextPx` through the semantic query,
committed `24.0`, and returned to the document. The captured configuration store is under
`evidence/config/layout`; the session log records one `system://layout` update callback. This
combined GL run proves the native 24 px setting. It did not capture a true 16 px baseline in the
same run; the separate GLES run is the true-16 baseline proof.

At 800×600, the run created an 80-line source passage of 7,438 bytes and used Ctrl+Return at byte
3,719 to split it across two pages. It created a local comment link (link 21, author `local_author`)
from source range `[0,7438)` to the 23-byte target `[0,23)`. The session log shows the source
endpoint across pages 1–2 and the target endpoint on page 1. Opening the layout document also
introduced system link 71; the second Alt+Shift+N selected the authored local comment link.

The captured journey shows source selection, link preview, link selection, expanded controls, Enter,
six wheel pans, continuation wheel, Back, and Origin. All 15 captures are lossless PNGs in
`evidence/png/`. The original raw PPM hashes are retained in `evidence/ppm-SHA256SUMS`; no PPM files
are duplicated. `evidence/SHA256SUMS` preserves the source evidence checksums, and
`evidence/png-SHA256SUMS` checks the archived PNGs.

The activity-store checkpoints before navigation, after Controls, after Enter, and at exit record
visit counts 1, 1, 2, and 2. The source operation dumps before and after navigation are unchanged:
hashes `9e785c3ed9d6fe9b9f45aaa68c940b5dd49fef742ece29de3ae272a39fd8ebad` and
`b6d483a77522fd1b63f44cd78f00a587bbb956dd9188cee06c55b0b1135d4b89`. The activity store records the
completed reader transition without adding operations to either visited source.

`diagnostics/attempts/` keeps compact evidence from earlier enlarged GL runs, including their logs
and text snapshots. Those attempts are not counted as proof; only `enlarged-controls-enter` is the
successful combined journey.
