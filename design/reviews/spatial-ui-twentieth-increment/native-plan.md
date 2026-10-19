# Twentieth-increment native cleanup journey plan

This plan is read-only preparation. Do not launch a twentieth-increment app until the parent
supplies the frozen binary path and complete hash tuple. Do not edit product/build files or mutate
the nineteenth-increment continuation profile.

## What the native UI can establish

The continuation request is a real, UI-authored pinned request from the eighteenth increment and was
reopened under the frozen nineteenth build. Its persisted identity is documented in
`design/reviews/spatial-ui-nineteenth-increment/native-restart-validation.md`. The nineteenth
profile is `build/validation-nineteenth/native/restart-smoke/`. Read-only inspection shows the Inbox
LMDB files but no `<request-id>.attempt-*` staging directory, so the existing attempt `1` has zero
staged bytes. This run can prove the user's **Discard partial data** control is reachable and safe
when there are zero staged bytes, and that the same request survives restart. It cannot prove
nonzero partial bytes were deleted.

Nonzero cleanup must be established by the engine-owned focused test and the actual-workspace
filesystem evidence prepared by the parent. Do not seed a native profile with fabricated staging
files, label a test fixture as UI-authored, enable test transport in the ordinary UI, or substitute
a local `.xanadoc` import.

## Native route after freeze

Create a fresh native run root. Copy only `data/` and `config/` from
`build/validation-nineteenth/native/restart-smoke/`; this continues the previously UI-authored
request without altering its original profile. Start with fresh empty cache and GNUPG home. Run the
frozen app with six isolated SDL/OpenGL/XDG variables, private Xvfb/X11 and software rendering, plus
private D-Bus. No key generation is needed because this is not an authoring or signing journey.

Use the ordinary Ctrl+O route to select the saved request and open its status. First capture the
actual status and the expanded Action choices. Confirm from the visible/accessibility choices that
**Discard partial data** is an available user action for this `Failed` attempt. If it is absent,
record that finding and stop; do not invoke an internal API or keymap command as a substitute.
Select and submit Discard through the form, after recording the exact precondition and chosen
option. Do not select Retry. Capture the immediate result and allow the worker/UI state to settle
using its ordinary Refresh control if needed. Record the actual stable phase, verification, error
and attempt; do not infer them from the button label.

After graceful exit, use read-only `mdb_dump` and strict XPI1 decoding. The request ID and every pin
field must still match the nineteenth baseline, attempt must remain `1`, and there must be no staged
attempt directory or staged bytes. Compare the actual stable journal status with the before-action
record. Relaunch the same copied profile once, open the request again through Ctrl+O, and capture
its settled status. Confirm no stale transient cleanup result appears after restart, the same
request and pin remain available, the attempt is still `1`, and no retry occurred. If observed
behavior differs, report the exact state rather than forcing the expected state.

Record source document and package-visibility hashes and `xudu-dump` before and after. Decode
activity before and after for completed visit count and current visit; shutdown may append activity
metadata, so compare visit identity/count rather than claiming activity byte identity. Capture each
actual frame, internal accessibility snapshot, argv, selected isolated environment, process log and
exit. Archive exact PPM bytes as deterministic gzip plus PNGs verified at zero RGB difference. Keep
the full profile, private keys, LMDB data files and binaries out of evidence.

## Separate nonzero-byte evidence

The engine-owned test must create nonzero staged bytes through its actual Inbox worker, exercise
Discard/cleanup, and inspect the real workspace after the worker settles. Pair its test result with
filesystem tracing that captures the actual writes/removals and synchronization calls. Report the
fixture/test boundary plainly: this demonstrates engine cleanup of nonzero attempt data, not an
ordinary UI-created partial download and not physical power-loss recovery. The native UI run remains
a zero-byte user-affordance/restart check.
