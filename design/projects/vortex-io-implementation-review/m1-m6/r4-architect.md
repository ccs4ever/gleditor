# Round 4 critique: System Architect (Vortex I/O M1–M6 code)

Target 720fef61. Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR.

## Status of earlier findings

- R3-1 (written vs fsynced): RESOLVED. `SnapshotChannel` has `syncedOps` and `syncedTablesSequence`
  (`store.cpp:4189-4190`). A Fsync write that finds its operations already written still fsyncs
  `ops.nodes` (`store.cpp:4407-4416`) and a newer-but-buffered `store.tables` (`:4445-4449`) before
  the directory. Buffered Replace and `load()` reset `syncedOps` (`:4399-4402`, `:4740`). Tests
  `aBufferedSaveDoesNotLetALaterFsyncWriteSkipTheFsync` and the Replace case read the
  `SnapshotSyncs` seam.
- r3 MINOR 1 (O(store) first snapshot): RESOLVED in the plan M6 notes and `p3-implementation.md`.
- r3 caveat (owner compaction may wait on a worker): RESOLVED in the plan and the `compactWhere`
  comment (`capture_journal.hpp:299-301`).

## Lane checks (clean)

- Directory channel registry (`store.cpp:4256-4291`): bounded and reaped. Entries are `weak_ptr`;
  each `channelFor` call erases expired ones, and a `Store` holds only its last directory's channel,
  so the registry size is the number of directories with a live `Store` or in-flight snapshot. No
  hidden ceiling. Between calls an expired entry stays until the next call (a few dozen bytes of
  control block); not growth. A process-global is the right layer: a directory is one process-wide
  OS resource, exactly like a file lock, and the engine has no per-process context object to hang it
  on. Inode reuse (directory deleted and recreated while an old `Store` holds the channel) is caught
  by the existing `file_size` check (`:4357-4366`) and `nextSequence` is shared and monotonic, so no
  stale-skip.
- One writer of `ops.nodes`: still true. The `.tmp` and rename live inside `writeSegmentRange`
  (`segmented_ops_spool.cpp:765-837`); `writeSegmentFile` and the snapshot writer both go through
  it. `store.tables` has one serializer (`writeStoreTablesAtomically`). The temp is removed on every
  failure path. Directory fsync after the rename is the caller's, and the snapshot writer does it
  (`store.cpp:4450`); the comment says so.
- `CaptureJournal::compactWhere`: shape is sound (`function_ref`, non-owning, `select` outlives the
  call; scan, choice and rewrite under one `mutex_` hold). Constraints are documented: runs under
  the mutex, must not call the journal, may wait on a worker's fsync, refuses on poisoned or
  unreadable. The coordinator's callback reads only owner-thread state (`pending_`, `pinned_`,
  `unpublished_`), which workers never touch, and calls no journal method.
- P4 and P5: honestly priced. P4 gives a growth path (about 200 B per reaped request, 200 MB per
  million, escalate at M7 long-lived services) and names the core file. P5 states the real reason
  (an `Aborted` with no intent is ambiguous to recovery) and the fsync is off the commit's happy
  path. Neither blocks M1–M6 sign-off: both are recorded, bounded by process lifetime and workload,
  and need owner approval of core or protocol changes that this review round may not assume.
  Convergence should carry them as open proposals into M7 planning.

## MINOR

1. `channelFor` does metadata syscalls on the owner on every `saveSnapshot`: `create_directories`,
   `stat`, and `weakly_canonical` (several `lstat`), the last computed unconditionally even when
   `stat` succeeded (`store.cpp:4268-4277`). Previously a save to an already-seen directory did no
   syscall. Cheap and not blocking I/O (within plan §14), but per commit; move the
   `weakly_canonical` into the stat-failed branch.
1. `saveSnapshot` resets the shared channel's `durableOps` to 0 without holding the channel mutex
   (`store.cpp:4305-4309`), and with channels now shared across `Store` objects a `Store` whose
   history is shorter than what the directory holds makes the next write a (now atomic) Replace of
   the longer file with its own shorter history, where a same-length divergent history gets the
   refusal `BaseMismatch`. Reachable only when two `Store`s of divergent history save to one
   directory, which is a caller error, and a first save into a fresh channel did the same before, so
   not a regression in kind; but add a test pinning the intended behaviour (refuse or replace), and
   take the mutex for the reset.

VERDICT: CONVERGED
