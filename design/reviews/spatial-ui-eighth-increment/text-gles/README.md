# Native GLES text-scale validation

This directory records the successful native OpenGL ES enlarged-text journey and compact diagnostics
from earlier attempts. The valid run used the candidate `build/xuzz` and `build/libgleditor.so.0`
from the spatial UI overhaul worktree. The exact pre-format runner is preserved as
`run-native-enlargement.sh.txt`. The current `run-native-enlargement.sh` is formatted and
ShellCheck-clean; its paths point to the original `/tmp/spatial-local-validation.W4mSRr` run root,
so it is a provenance record rather than a drop-in rerunner from this archive. The original and
current runner SHA-256 values are recorded below.

The run used OpenGL ES 3.2 on Mesa 26.2.4 with software rendering through X11 under Xvfb, at
800×600. It drove the ordinary UI route: open `system://layout`, focus `readableTextPx` through the
semantic query, commit `24.0`, return to the source, close, and reopen the app in the same isolated
profile. All six SDL/XDG environment variables and the profile locations are recorded in
`evidence/run-metadata.txt` and the runner. No raw PPM files are duplicated; the six captured frames
are lossless PNGs under `evidence/png/`, with their SHA-256 values in `evidence/png-SHA256SUMS`.
`evidence/ppm-SHA256SUMS` records the original raw-capture hashes for provenance.

The true pre-edit observer snapshot was bracketed after the semantic-value capture and before the
5-second pause ended and the `Escape`, `E`, `24.0`, `Enter` edit sequence began. The semantic frame
was written at `2026-10-14 20:46:12.603222960 -0400`; snapshotting began at
`2026-10-15T00:46:13.159611099Z` and completed at `2026-10-15T00:46:13.187109793Z`. The marker is
`yes`. The pre-edit layout dump still has cell 160 at `16`, matching the pre-run ops hash
`28df389dd1a5fa2c3568fe67363a347a62aa8d178406b98d9ae60c2023262b8d`.

The accepted edit added operation 1284, `SetValue double ctx=160`, text `24.0`, with canonical bits
`4627448617123184640`. The after-run and same-profile reopen layout dumps both hash to
`97f826a1e57031f755125d2c0a2df9d6c584246abfd6c15e3d56daeb05fe9be7`. The app log records one
`system doc updated (system://layout)` callback, and A11y reports cell 160 as `24.0` after commit
and after reopening. Both app runs exited 0. The selected source store is explicitly `default`; its
ops dump is byte-identical in all five before, pre-edit, after, after-exit, and reopen snapshots,
SHA-256 `944eb775e6a7ac9536f3be55745efcbde0a3c2b81afe4c02600d7996e4be5bd9`. The full operation
dumps, profile inventories, callback logs, exit statuses, source fixture, and pre/post binary hashes
are retained in `evidence/`.

The before frame shows the source at its default scale. The live and reopened frames show visibly
larger text; in the clean body region, occupied glyph rows are about 9–11 px high before and 18 px
after, and line rows are about 15 px apart before versus 24 px after and on reopen. The return to
the source also reframes the page, so the pixel-height comparison is an observed visual measurement
rather than a controlled camera-locked benchmark. The enlarged view's mini-map overlays lines 9–12;
those lines are not unobscured in the capture. `diagnostics/clipped-stress/` retains a prior
long-line capture that shows horizontal clipping at this 800 px viewport. The short-line proof
establishes legibility for the displayed lines without concealing that narrow-window limitation.

`diagnostics/attempts/` records two non-authoritative harness outcomes. The uppercase-key attempt is
incomplete: it exited 0 but did not provide a valid pre-edit snapshot or establish the requested
value transition. Retry 4 did commit `24.0`, but its purported pre-edit snapshot was late: its
snapshot dump already contains the new operation. Neither is counted as validation. The valid
evidence is retry 5 only.

Runner provenance: original SHA-256
`c024e220b09a8e6f016c436cdb05ac110f7acd40ea48197d2e2c380e7511677e`; formatted runner SHA-256
`b51eeec1837cf6907a2abfe82db74b213435765cd7852c0fac9c440a8fc2dbed`. Formatting and lint-only edits
do not change the captured evidence or the recorded run.
