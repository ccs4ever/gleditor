# Increment 20 plan: explicit partial-download cleanup

## Scope and user promise

Add one explicit `Discard partial data` action to a failed or cancelled publication download. It
removes only request-scoped staging attempts and reports the number of logical file bytes removed.
The request receipt remains: stable ID, URI, full verified pin (or mutable observed lower bound),
phase, diagnostic, and monotonically increasing attempt number. Retry continues to use the same
request and existing immutable-pin rules. Completed request roots, including roots that are corrupt
or being inspected after retained verification fails, are never eligible for this action. The action
does not free a request slot. No `Forget` action is included; its ownership rules need a separate
audit of subscriptions, reader packages and opened stores.

Do not add an aggregate staging budget or choose a byte threshold here. The action is explicit
relief for files that already exist, not an admission policy or automatic eviction.

## Existing seams and lifecycle facts

`PublicationInbox::retry()` currently accepts only `Failed` and `Cancelled`, preserves the job ID
and pin/floor, increments its persisted attempt counter, and queues a new isolated `<id>.attempt-N`
stage. Each attempt is rooted beside the final `<id>` snapshot, so cleanup must strictly distinguish
the exact canonical request ID from the longer `.attempt-N` names. Successful install removes its
transfer `cache`; failed or interrupted download/install paths retain the rest for inspection. A
retry does not delete older attempts.

The inbox worker owns network creation, transfer, install, and cancellation. Active cancellation
does not publish `Cancelled` until the transport has acknowledged stop and the download function has
unwound. Queued cancellation has no active worker. Thus a persisted terminal `Failed`/`Cancelled`
phase is a safe admission point for cleanup. Keep cleanup on that same worker queue so the form
never blocks while walking or deleting a large tree, and so cleanup cannot run concurrently with a
transfer. Reject the action if the request is active.

The shared `publication_status_form` currently offers Refresh, Retry for Failed/Cancelled, Cancel
for active phases, and Open/Subscribe for Ready. The Views handler dispatches actions on the
render-state path; filesystem removal must not happen synchronously there. Offer cleanup for every
Failed/Cancelled record, including when no partial files exist. Do not scan the filesystem on the UI
thread to decide whether to show the action. Preserve the existing form evidence and error text;
show that cleanup removes partial bytes only and keeps the failed/cancelled receipt and retry ID.
Completion should report the exact logical bytes removed and return to the same status form. A
transient in-progress indicator may live in memory; it need not create a new persisted phase if the
worker command can report completion independently.

## Safe ownership and deletion algorithm

Under the inbox state lock, accept a cleanup command only when:

- the ID is an exact canonical public-key hex string and identifies an existing request;
- phase is `Failed` or `Cancelled`, and no worker attempt is active;
- the worker has no prior cleanup for that ID in progress.

The worker enumerates only direct children of the canonical inbox directory. An owned candidate name
must be exactly `<id>.attempt-<N>`, where `<id>` is the job's full stored ID, `N` parses as
canonical unsigned decimal without overflow, and `N <= job.attempt`. Never use a loose prefix match
or search below another directory. Unknown IDs, malformed suffixes, and attempts beyond the
persisted counter are not owned by this request and are preserved. Do not delete the exact `<id>`
installed root, LMDB files, another request's directories, or unrecognized entries.

Before deleting anything, preflight every candidate with the existing retained path validator
extended/used to reject symlinks, FIFOs, devices, sockets and other special entries. Check that the
candidate root and parent are the expected inbox paths; do not follow links. Count regular-file
logical lengths with checked `uint64_t` addition. Validate and count all owned candidates before
removing the first one, so a malformed later candidate does not cause a surprising partial cleanup.
Label the result as logical file bytes; sparse files, metadata and physical allocation are not
measured. Keep any symlink or special-file tree intact and report typed refusal rather than
attempting to open or recursively traverse it.

Delete only the prevalidated attempt directories, then sync the inbox directory on platforms with
directory-sync support. The worker removes entries iteratively with stop checks. Cleanup is
repeatable across a process interruption: if deletion is interrupted, the receipt remains and a
later explicit cleanup can remove remaining owned attempts. If deletion or directory sync fails
after some entries were removed, report that cleanup may be partial and leave the request/pin/error
unchanged. Never roll back by recreating files, and never automatically repeat cleanup on startup.

Because files can change between preflight and removal, keep the inbox profile private to the
application and execute the full operation on the single inbox worker. If the repository's threat
model includes a concurrent hostile local writer, the delete walk must use platform-specific
directory handles and no-follow unlink operations; a path-only preflight and deletion would not
close that race.

No persistent journal state is required for an in-progress cleanup if the operation is safely
repeatable and only user initiated. A process exit during the operation leaves the same receipt and
any remaining partial directories; restart preserves the terminal status and does not start
networking or cleanup. Keep the attempt counter unchanged even when all old stage directories are
removed, so future retry receives a new name. Report the measured byte count only after cleanup
succeeds; if the worker stops midway, report the partial outcome as unknown and let a later explicit
cleanup recount what remains.

## Byte observations and limits

There were no surviving `.attempt-*` directories under the current build tree to measure: the owning
tests remove their temporary roots. Checked-in completed sample trees provide only a small baseline,
not a download-budget basis. Their total regular-file bytes range from about 67.5 KB to 444.9 KB;
`published/` dependencies in those trees range from 348 bytes to about 375.3 KB. These are small
fixture publications, not realistic upper bounds. The native store has roughly 65–74 KB `ops.nodes`
files in the samples, while actual attempt trees can also contain the manifest, torrent metainfo,
partial transfer cache, installed dependency copies and temporary native store files.

The current per-request limits remain 16 MiB for a manifest, 1 GiB for dependencies, and 4096
dependencies; they do not cap accumulated failed attempts. The global inbox record cap remains 128.
Do not infer an aggregate budget from fixture size or multiply the 1 GiB limit into a claimed
expected usage. Before proposing such a budget, capture actual partial attempts at resolve,
manifest, dependency and install failure points across the existing publication tests plus
representative authored publications, recording both logical bytes and filesystem allocation. That
measurement task is separate.

## Tests and implementation boundary

Keep behavior tests in `tests/xudu/publication_outbox_test.cpp`; add no transport service or UI test
mock beyond existing deterministic inbox transports. Test that cleanup removes all and only owned
attempt roots, returns the exact logical byte count, preserves the request record byte-for-byte
except for no journal change, retains its full pin/floor/error/attempt counter, and allows an
explicit retry under the next attempt number. Test failed and cancelled requests; reject
queued/active/Ready states. Include unrelated request IDs, malformed attempt suffixes, future
attempt numbers, a completed root beside partial attempts, symlink/special-file refusal,
overflow-sized metadata handling where representable, and idempotent second cleanup returning zero.
Verify that reopening after a simulated partial filesystem cleanup keeps the same terminal request
and does not call the transport factory until Retry.

The root durability increment already establishes staged-file/directory sync and final-root
ordering. Cleanup should sync the inbox directory after deletion so its namespace transition is
durable where supported; Windows remains limited by the platform's unverified directory-sync
semantics. Do not claim a power-cut test from process restart or from a successful syscall trace.

Keep this increment to the Inbox cleanup API, worker-owned command/result seam, the existing status
form and View dispatch, plus owning tests. Do not edit subscription ownership, remove completed
roots, drop journal records, change the request journal format without a specific need, add
settings, or enable ordinary publication transport.
