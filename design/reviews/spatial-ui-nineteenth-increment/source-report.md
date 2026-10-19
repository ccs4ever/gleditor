# Increment 19 source report

The inbox now places a POSIX durability barrier between completed staging and the journal's `Ready`
state. It syncs every regular file in the staged tree, then its directories bottom-up; promotes the
signed manifest and syncs the attempt directory; renames the attempt root to the immutable request
root and syncs the inbox directory before persisting `Ready` in LMDB. The same content and directory
barrier runs when locally re-verifying a retained root, so that path cannot publish `Ready` without
syncing an older snapshot first.

The configured inbox leaf is still rejected if it is a symlink. After creating it, the owner
canonicalizes the path once and syncs its actual ancestor chain, which supports relative roots and
configured ancestor aliases such as macOS `/var`. Retained and staged trees reject symlinks and
special files before opening entries; the FIFO test confirms that a staged special file fails before
`Ready` and remains available for inspection. The XPI1 journal schema is unchanged.

On Windows, staged regular files are opened read/write and passed to `_commit`; renames use
`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`. There is no portable directory `fsync` equivalent in
this implementation, and Windows directory power-loss durability was not tested or claimed. The
Windows branch was not compiled in this Linux environment. The `NOMINMAX` include guard was added
for the Windows header interaction with `std::min` and `std::numeric_limits`.

Validation results:

- `make -j$(nproc) xudu_test`: exit 0.
- Headless `PublicationInboxTest.*:PublicationSubscriptionsTest.*`: 35 tests passed, exit 0. This
  includes staged-before-root and renamed-orphan recovery, retained snapshot verification, relative
  and symlinked-ancestor inbox paths, and the FIFO refusal case.
- `PublicationInboxTest.CompleteStoreRetainsSignedSnapshotAndReopensOffline` under `strace -ff -yy`
  on the workspace ZFS filesystem: 1 test passed, exit 0. The trace shows staged-file and directory
  `fsync` calls, manifest rename followed by attempt-directory `fsync`, attempt-root rename followed
  by inbox directory `fsync`, and then LMDB `fdatasync` on `data.mdb`.
- `clang-format` 19 ran on both changed source/test files; `git diff --check` passed before the
  final Windows-only `NOMINMAX` guard.

This verifies the syscall ordering and process-restart recovery behavior. It is not a power-cut
test; no process kill at syscall boundaries or filesystem image recovery was performed. No real
transport, cleanup control, staging budget, UI, or journal-format change is included.
