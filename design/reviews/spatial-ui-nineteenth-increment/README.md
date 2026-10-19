# Nineteenth spatial UI checkpoint

Based on `7200fd17`, this increment orders filesystem installation before a publication request can
be journaled Ready. It continues the existing inbox worker, exact-pin journal and immutable-root
layout. The spatial overhaul remains incomplete.

## Behavior

On POSIX, the worker closes completed staging writes, syncs every regular file and then directories
bottom-up, promotes the signed manifest, and syncs the attempt directory. Under the same guarded
transition as cancellation, it renames the attempt into its immutable request root, syncs the inbox
directory, and only then commits Ready/Complete to LMDB. Retained-root revalidation applies the same
content and directory barrier before Ready. Failures preserve staged or installed bytes for
inspection and the existing full-pin recovery path; they do not remove an earlier snapshot.

The configured inbox leaf must not be a symlink. Its canonical absolute path supports relative
configuration and legitimate aliased ancestors before directory syncing. Internal staging and
retained trees reject symlinks and special files before opening entries. The new FIFO test confirms
refusal without blocking or publishing Ready. The XPI1 journal, publication and store formats are
unchanged. The filesystem protocol belongs in the existing C++ worker owner; no alternate runtime,
view or network service is introduced.

Windows uses read/write file handles with `_commit` and write-through moves. Its directory-sync
helper has no POSIX-equivalent barrier. The Windows branch was not compiled or run here, and Windows
directory power-loss durability is not accepted.

## Validation

Canonical GNU Make passes. Fresh headless full UI 246/246, Xuzz 107/107 and owning
publication/package 72/72 pass. The focused completed-install/offline-reopen test also passes under
actual syscall tracing on workspace ZFS. The retained trace shows staged-file/directory sync,
manifest rename and attempt-directory sync, root rename and inbox-directory sync, followed by LMDB
journal sync. This is observed protocol ordering and process-restart recovery evidence, not a
physical power-cut test.

Default library and library-test binaries remain byte-identical to checkpoint 17, permitting reuse
of 799/799 and mip/cache 53/53 evidence. The plain editor relinked, so strict backend comparison is
rerun and passes unchanged criteria: GL/GLES ordinary and overlay are exact; Vulkan differs by
0.5104% / 0.5096% under the existing 1% limit and channel tolerance 1. The small sample does not
exercise actual split recording. Format, lint and UI-text policy pass. The
[native restart smoke](native-restart-validation.md) preserves the saved full pin, failure and
attempt 1, source/package bytes and one completed visit/current 1. Shutdown appends activity
metadata; activity-store byte identity is not claimed. No successful native install is claimed. The
independent review found no remaining source blocker in scope.

## Remaining work

Physical power-cut and kill-at-boundary recovery, injected sync/LMDB failures, Windows/macOS native
coverage and multi-process interference remain unverified. Private inbox ownership is assumed. No
partial-download cleanup control or aggregate disk policy is added here. Complete that lifecycle
before enabling governed normal publication transport. Completed and referenced roots must remain
protected until the pin-ownership audit supports removal. Real enrollment/BEP46 binding, remote
availability variants, complete journeys, native AT-SPI parity and optimized performance remain
open.
