# Independent source review: durable publication installation

I reviewed the frozen increment-19 diff from `7200fd17` in `publication_inbox.cpp` and
`tests/xudu/publication_outbox_test.cpp`, including the follow-up portability corrections for
configured inbox paths and Windows headers/file handles. This was a read-only source review. I did
not build or run the tests; the owning test and filesystem-trace results are root-owned and should
be recorded separately.

I found no blocking ordering or preservation defect in the reviewed POSIX path. The download path
closes the transport, completes store installation, removes the transfer cache, writes and closes
the manifest, then syncs every regular file and directory in the staged tree. It promotes the
manifest and syncs the attempt directory before entering the guarded final transition. Under the
inbox guard it checks cancellation, renames the attempt directory to the immutable request root,
syncs the inbox parent, sets Ready/Complete, and commits the LMDB record. The retained-only
revalidation path also syncs its already-installed tree and parent before publishing Ready.

The sync walker rejects symlinks and special files before opening them, does not follow directory
symlinks, and opens POSIX regular files with `O_NONBLOCK` and `O_NOFOLLOW` where available before
confirming regular-file type with `fstat`. Directory handles are similarly checked. The owning FIFO
test exercises the failure-before-Ready behavior and confirms the staged FIFO remains in place.
Sync, rename, and LMDB failures do not trigger deletion or replacement of an installed root; a root
moved before a later failure remains available for the existing full-pin recovery path.

The final rename uses sibling paths under the same inbox directory, so the source and destination
share a parent and the operation cannot silently degrade to a cross-filesystem copy. Destination
existence is checked before rename; POSIX rename would replace an empty destination directory in
some cases, but the inbox guard and prior existence check protect the normal single-writer path.
This assumes the private inbox tree is not concurrently modified by an external process.

The Windows implementation commits regular-file handles and uses
`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` for file and directory moves, while directory-sync is
intentionally a no-op. That is explicitly narrower than the POSIX parent-directory `fsync` contract
and should not be described as verified power-loss durability. The final Windows diff uses `_O_RDWR`
for `_commit` and defines `NOMINMAX` before including `windows.h`. No Windows run was part of this
review.

The configured inbox leaf is rejected if it is itself a symlink before creation/canonicalization.
The implementation then canonicalizes the resulting directory once, so relative paths and legitimate
symlinked ancestors are resolved before the sync-chain traversal applies `O_NOFOLLOW` to each real
directory. The new test covers both a relative inbox path and a symlinked ancestor. This resolves
the earlier portability blocker for normal aliased paths such as `/tmp` or `/var` while keeping the
inbox leaf check and internal retained-tree checks intact.

Remaining evidence limits are deliberate and should remain visible in the final claim: the FIFO test
covers special-file refusal, but no true power-cut or child-kill-at-syscall-boundary test was run
here; a syscall trace can establish the observed `fsync`/rename ordering, not survival through power
loss. The reviewed test diff does not inject a sync failure or an LMDB commit failure, nor does it
test an existing destination collision with sentinel bytes. The added path-canonicalization and FIFO
tests were source-reviewed but not run by me. No test result is claimed by this report. These are
validation gaps rather than source blockers found in this pass.
