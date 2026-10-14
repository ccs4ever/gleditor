# First post-fix replay: failed landing diagnostic

This is an incomplete, failed post-fix replay of the stable binary `0.0.1.911.gc1ab2314`
(`build/xuzz` SHA-256 `05cb2e51d98ef7b55963c6618b18d2d112c224462f6bd57bbf009fb045de5eb3`). It is
retained under `failed-first/` so it cannot be mistaken for final passing evidence.

At 800×600, the OpenGL and OpenGL ES runs completed the exact eight-line, 654-byte source / 23-byte
target authoring flow, Enter, Origin, Back, and fold/unfold checks. Both `01-enter.png` frames show
the source canvas shifted/cropped at the left edge with the lower selected text covered by the link
panel. The complete selected passage is not visually readable. The OpenGL and OpenGL ES
`01-enter.ppm` frames have identical SHA-256; raw PPMs are outside Git at
`/workspace/spatial-build/fourth-raw/wide/failed-first/` and their hashes are in `manifest.sha256`.

The click itself is valid: the native script clicked the visible `Enter` button at `(320,465)`; Xuzz
tagged the hit as overlay pick scope (`kind 1`, `doc 0`, `page 6`, `cluster 15`), then awaited
`Doc 2=selected`. The a11y snapshot reports the entire source text in eight runs, `Doc 2` selected,
`reading: left member 1`, left bytes `0–654`, and right bytes `0–23`. It reports a local caret at
`77 from 0`; this record is retained as observed and is not evidence that the selected range
changed. Origin and Back checkpoints returned to the target reading state. Source-operation dumps
are byte-identical before/after in both completed runs; their hashes match baseline. The private
activity log records two visits and the post-Back current visit. See `800,600-opengl/session.log`,
`800,600-opengles/session.log`, `argv.txt`, and source/activity snapshots.

The Vulkan attempt produced no screenshot. It failed at startup because SDL's offscreen video driver
does not provide Vulkan: The complete startup error is retained in
[session.log](800,600-vulkan/session.log). The replay harness has been corrected to launch Vulkan
under Xvfb with `SDL_VIDEODRIVER=x11`; that corrected path has not yet run.

No post-fix 640×480 or 1440×1000 captures were made. The replay stopped after this first matrix
attempt. The next stable build should run the full matrix through `run-wide-final-matrix.sh`, which
writes passing-candidate captures under `evidence/wide/final/` and archives raw frames/profiles
outside Git.
