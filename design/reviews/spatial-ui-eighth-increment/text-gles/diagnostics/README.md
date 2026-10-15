# GLES diagnostics (not validation)

`clipped-stress/` retains retry 2's before and enlarged live frames, session log, layout dump, and
run metadata. The enlarged long-line frame demonstrates horizontal clipping at 800 px; it is a
limitation capture, not the legibility proof. Retry 2 reached and committed `24.0`, but did not take
a verified pre-edit snapshot.

`attempts/` preserves selected logs and state from an invalid uppercase-key attempt and a
late-snapshot retry. The uppercase-key run exited successfully, but its UI action sequence did not
establish a valid value transition. Retry 4 reached `24.0`; however, snapshotting began after the
semantic capture and its “pre-edit” operation dump already contains the `SetValue 24.0` operation.
These attempts are retained for audit and are excluded from pass claims. Retry 5 in the parent
directory is the valid run.
