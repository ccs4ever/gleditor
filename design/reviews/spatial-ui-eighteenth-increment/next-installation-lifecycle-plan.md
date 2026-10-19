# Increment 19 prerequisite: durable install and bounded attempt lifecycle

This is a read-only source plan for the PublicationInbox owner. It does not enable a production
transport or change source. The next ordinary network configuration should wait until the installed
snapshot’s file and directory entries have a defined durability contract and failed attempt storage
has explicit bounds and controls.

## What the code owns today

`PublicationInbox::Impl::download()` stages each attempt at `<inbox>/<request-id>.attempt-N`. The
directory and its cache are on the same path root as the final `<inbox>/<request-id>`, so the final
directory rename is intended to stay on one filesystem. The worker fetches the manifest and
dependencies into `attemptRoot/cache`, closes the transport, calls `installPublication()` into
`attemptRoot/store`, removes the cache after that install, writes `publication.xanadoc.partial`,
renames that file to `publication.xanadoc`, renames the whole attempt directory to the request root,
then persists Ready/Complete in LMDB.

A failed or cancelled attempt keeps its `.attempt-N` tree. Retry increments N and starts another
tree. The only `remove_all()` in the inbox is the successful path’s removal of that attempt’s
`cache`; there is no Inbox API or status-form action to discard partial files or forget a request.
The request ceiling is 128 records by default. Reaching it refuses new submissions, including when
the records are all completed or failed. The UI form exposes Refresh, Retry, Cancel and Close, plus
open/subscribe for completed requests.

The byte controls are per carrier/request rather than cumulative disk controls:
`maximumManifestBytes` defaults to 16 MiB, `maximumDependencyBytes` to 1 GiB, and
`maximumDependencies` to 4096. `DownloadTransport::fetch()` checks the metainfo total against the
byte allowance before starting payload download. Retained failure trees from older attempts are
outside those limits. Installation copies validated dependency content into the staged store, so
peak staging also includes both the transfer cache and installed copies plus native store files.

## Durability primitives found

There is no reusable file-and-directory durability helper in the repository.
`SegmentedOpsSpool::flush()` and `SegmentedPrimediaSpool::flush()` use POSIX `fsync()` or Windows
`_commit()` for their active file descriptors. `SegmentedOpsSpool::writeSegmentFile()` itself writes
and closes without a sync. `Store::save()` writes `ops.nodes` through that method and `store.tables`
through `writeStoreTables()`; the latter closes an `ofstream` and checks stream state but does not
sync the file. `Store::save()` also ignores the return value from `UserPermascroll::flush()`. The
downloaded reader store is built with an empty reader permascroll, but the native store files still
need their own sync before installation.

The inbox currently closes the manifest stream and uses two `std::filesystem::rename()` calls with
no file or parent-directory sync. `persist()` commits through LMDB with flags `0` and does not use
`MDB_NOSYNC`; the transaction commit is the journal durability boundary. Do not add a post-commit
`mdb_env_sync()` and then claim the previous record is restored if that later sync fails: it could
fail after the transaction has committed. Preserve LMDB’s commit result as the single
journal-transition result.

On POSIX, `fsync(2)` documents that syncing a file does not necessarily persist its containing
directory entry; the parent directory must also be synced.
[Linux `fsync(2)`](https://www.man7.org/linux/man-pages/man2/fsync.2.html) On Windows, the existing
`_commit()` use covers file handles only. `MoveFileExW` offers `MOVEFILE_WRITE_THROUGH`; Microsoft
describes it as waiting for a move to be flushed, especially for copy-and-delete moves, but this is
not the same existing contract as POSIX parent-directory `fsync`. The Windows same-volume
directory-rename guarantee needs a deliberate platform contract and validation before claiming
power-loss durability.
[MoveFileEx](https://learn.microsoft.com/windows/win32/api/winbase/nf-winbase-movefileexa),
[Microsoft moving directories](https://learn.microsoft.com/windows/win32/fileio/moving-directories)

## Smallest implementation seam

Keep installation durability in the PublicationInbox owner. Add a narrowly scoped helper there (or a
publication-local helper adjacent to it), not a new general filesystem framework. It should
explicitly sync regular files and containing directories, report every failure, and use
platform-specific paths rather than treating `std::filesystem::rename()` as durable. Do not change
store/publication wire formats.

The final transition should be ordered as follows:

1. Finish and close the transport so no torrent writer can still mutate the cache. Validate carrier
   bytes and construct the native store as today.
1. Flush the completed signed manifest temporary file and every regular file under the attempt tree,
   including `ops.nodes`, `store.tables`, and retained dependency files. Propagate errors; a failed
   sync leaves the job Failed/Cancelled and its attempt data available for inspection.
1. Sync attempt directories bottom-up after their files. Promote `publication.xanadoc.partial` to
   `publication.xanadoc`, then sync the attempt directory so that filename transition is durable.
1. Under the existing inbox guard, check cancellation, rename the whole attempt root to the
   immutable request root, sync the inbox parent directory, set Ready/Complete, and commit the LMDB
   journal transition. Keep cancellation and Ready serialized under this same guard. If a sync or
   rename fails after the root moved, preserve the installed bytes and report failure; never remove
   or overwrite the old root to simulate rollback.
1. Treat Ready as visible only after every preceding durable step and the LMDB commit succeeds. A
   failure to persist Ready leaves the existing Verifying/Failed journal plus an orphan root.
   Startup may use the existing full-pin orphan reconciliation and retained-content verification; it
   must never infer Ready from directory existence alone.

On POSIX, the expected sequence is file `fsync`s, directory `fsync`s from deepest child outward,
`rename()` of the attempt root, then `fsync()` of the inbox parent, followed by LMDB commit. All
paths must remain on the same filesystem. On Windows, sync files with the repository’s `_commit()`
pattern and use a documented same-volume directory move strategy such as
`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` where supported. If the target Windows/filesystem
combination cannot promise durable parent metadata, state the narrower guarantee and keep that
platform out of a power-loss durability claim until validated.

No crash recovery step may delete an installed root or a receipt. A valid root found after a crash
is accepted only when its signed manifest and carrier hash match the full persisted pin, and it
still passes local dependency/store verification. Unmatched, corrupt, or incomplete attempts remain
quarantined to their request-scoped attempt directory and are surfaced as unavailable/failed.

## Root scope correction before implementation

The initial implementation scope is durable installation and explicit discard of incomplete
request-scoped attempts. Do not implement Forget for completed or referenced snapshots until the
publication/pin-lifecycle audit supplies reference ownership checks for subscriptions, saved reader
references and other retained consumers. The proposal below is not authorization to remove pinned
content. Measure the staging requirements before choosing a governed budget.

## Cleanup and disk budget

A storage quota must not silently evict a completed publication or the record that explains it.
Separate the controls:

- Add an explicit `discardPartialFiles(requestId)` action for Failed/Cancelled jobs only. It removes
  only validated, request-scoped `.attempt-N` directories at or below the persisted attempt number,
  never `<inbox>/<request-id>`, never the LMDB journal, and never an active worker’s stage. Return
  the exact removed byte count for the status form. Keep the request ID, pin/floor, phase and
  diagnostic receipt so retry and history remain available. Path validation must reject symlinks and
  unsafe nested files before deletion.
- Add a separate explicit `Forget request` action if freeing one of the 128 journal slots is
  required. Present its consequence in the form. Do not couple it to automatic quota handling. If
  the user explicitly forgets a completed item, first remove its installed root and all its
  request-scoped partial roots, sync the parent directory, and only then delete the journal record
  in a successful LMDB transaction. If deletion or journal commit fails, retain the record and
  report the partial result; never delete the receipt first and allow startup to re-import a
  still-present legacy-looking root.
- Preserve completed roots and request records indefinitely until that explicit Forget action. A
  partial-file discard is not a forget and does not free the request-count slot.

Add a cumulative staging budget separately from the existing per-request `maximumDependencyBytes`.
Do not choose an arbitrary byte number in this increment. First measure real retained dependency
sizes, native-store overhead and peak duplication during install using the existing real test
publications and representative larger authored publications. Then expose a finite configured budget
in the owning system-settings/session seam and display current measured staging usage, budget, and
the per-request limit in the inbox status. Until a finite budget is configured, ordinary network
fetching should remain unavailable with a direct explanation; otherwise retries can multiply up to
roughly a request’s fetched dependency allowance plus copied content on every attempt, while 128
records can remain permanently.

For admission, compute the actual bytes already present in all validated partial attempt trees with
overflow-checked traversal. Before a fetch, use the metainfo total (already checked before payload)
and the remaining allowance to ensure it fits. Reserve space for the installation copy and native
store overhead; rescan after install before the root rename. If the worst-case reservation does not
fit, stop before moving the root, keep files, and offer explicit discard. A budget failure must not
trigger automatic cleanup or silently lower the budget.

## Tests and limits

Use the existing `tests/xudu/publication_outbox_test.cpp` fixture and LMDB journal mutation style;
do not add product test hooks or simulated production transport services.

- Keep the current valid-root/orphan and interrupted-attempt recovery checks. Extend the recovery
  matrix with the actual persisted states on either side of the rename: a staged manifest under
  `<id>.attempt-N` remains Cancelled/Failed and is never opened; a renamed root with the journal
  still Verifying can be locally reverified to Ready only after full-pin and dependency checks; a
  mismatching root is refused and preserved.
- Add ordinary failure cases available without hooks: an existing destination collision cannot
  overwrite sentinel bytes or become Ready; a symlink or unsafe retained tree is refused before file
  reads/removal; failed explicit partial discard preserves completed roots and the exact request
  journal bytes; successful discard reports the measured removed bytes and preserves the pin,
  failure phase, and retry ID.
- Exercise the sync helper on regular files/directories in isolated temporary roots and ensure an
  error return stops Ready. A test-only process kill at every internal rename boundary would require
  a new hook or syscall interposer, so do not pretend a normal process restart proves power-loss
  durability. If a true crash test is required, put it in a separate platform-specific filesystem/VM
  harness that kills a child at the actual fsync/rename boundaries and inspects the recovered disk
  image; keep its result distinct from the fast unit tests.
- Do not force `MDB_MAP_FULL` by filling the shared filesystem or relying on a guessed map
  threshold. Inbox currently fixes LMDB map size at 16 MiB while the whole bounded journal is capped
  at 4 MiB, and no existing test seam can inject a commit failure safely. If a deterministic
  isolated-LMDB method is found, make it a separate test that preserves and byte-compares the old
  journal; otherwise document that the failure-rollback branch lacks direct fault injection rather
  than add a product-only test hook.

The valid claims after unit tests should remain “process-restart recovery and byte-preserving
refusal were tested.” Power-loss durability is claimed only after the platform-specific sync/rename
protocol is exercised against its supported filesystems.
