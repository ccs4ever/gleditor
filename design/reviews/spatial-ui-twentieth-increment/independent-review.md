# Increment 20 independent status-form review

I reviewed the current status-form and Views diff read-only, alongside the increment-19 failed-form
baseline at
`design/reviews/spatial-ui-nineteenth-increment/native/restart-smoke/frames/02-failed-form.png`. I
did not build, run, or modify product code. The backend cleanup status types were not yet in this
UI-only diff, so this is a review of the proposed form behavior and copy, not a compile or journey
pass.

The baseline form is a centered, dark panel with six clearly aligned fields. Its failed-state note
occupies two lines, leaving comfortable separation before the Action field. The exact requested
carrier, edition, publication name, and publisher remain visible below the note. The increment-20
changes preserve that identity presentation and add one explicit terminal action. Refresh, Retry,
Discard, and Close are in an understandable order; discard is available even when no attempt files
exist, so the UI need not scan disk to decide whether to offer it.

Pending cleanup keeps the failure/cancellation phase and receipt text, removes Retry and Discard
while the worker owns the operation, and offers Refresh for the result. Complete reports a
logical-byte count, including zero, and says the request and completed copies remain. Failed cleanup
keeps the original request error visible and adds a distinct “may be incomplete” cleanup diagnostic,
allowing another explicit cleanup or Retry. These states communicate the separation between deleting
staging data and changing the request lifecycle.

The main visual risk is note growth. The baseline failure already uses two lines. The new
pre-cleanup explanation adds a sentence; Pending adds a result hint; and completion/failure appends
an outcome plus any cleanup error. A normal completed outcome may fit in three lines, while a long
original failure and cleanup diagnostic can grow further. The raster baseline has enough room for a
small increase, but the form’s final height and lower field/footer spacing need confirmation from
native frames for the unstarted, pending, zero-byte success, nonzero success, and partial-failure
states. In particular, long error text must not push the publisher identity or footer below the
visible panel.

For compact copy, lead with a short cleanup outcome: “Partial data removed: N logical bytes. Request
kept; completed copies untouched.” Pending can say “Partial-data cleanup pending. Refresh for
result.” Failure can say “Cleanup failed; some partial data may remain.” followed by the cleanup
diagnostic. Retain the original request error after that summary, and shorten the enrollment
reminder to “Enrollment unchecked; download does not publish.” `Form::describe` exposes the raw note
as the accessibility value even when visual fitting caps it at three lines, so keep the full
diagnostic in the note/status while making the first visible lines carry cleanup outcome and request
state. Keep the six identity fields; a seventh status row risks moving the publisher off small
viewports.

I found no blocking interaction or identity issue in the current UI diff. The follow-up visual pass
should use realistic long diagnostics and inspect the full field list, not only an accessibility
tree; no pixel-fit claim is made by this source-level review.

## Read-only Inbox implementation review

I also reviewed the current `PublicationInbox` cleanup implementation and owning-test diff without
building or running them. Cleanup is represented only in transient status, not the XPI1 journal.
Admission is restricted to Failed or Cancelled requests; setting Pending and enqueueing the ID
happen under the same guard. The single existing worker prioritizes that queue, so cleanup cannot
overlap an active transfer. Retry and duplicate cleanup are rejected while Pending, and a retry
after a terminal cleanup clears only the transient result. Startup creates no cleanup commands, so a
process exit leaves the durable request receipt and any remaining attempt data for another explicit
action.

The worker snapshots the persisted attempt limit, scans direct children of the canonical inbox for
the exact request-ID prefix and canonical decimal suffix, ignores malformed/future/unrelated names,
then validates every owned attempt before deleting the first. Root and nested symlinks and special
files are refused. Logical file lengths use checked `uint64_t` addition. Deletion walks each
preflighted tree without following links, counts bytes as files are successfully removed, never
targets the exact installed request root, and syncs the inbox directory before reporting Complete.
Failures preserve the phase, original error, pin/floor and journal; the transient failure says the
operation may be incomplete and can be explicitly retried. The tests added for request receipt byte
preservation, completed-root protection, malformed/future and unrelated names, symlink/FIFO refusal,
restart-without-cleanup, retry gating, and byte counts align with the intended contract.

I found no blocking cleanup ownership or state-ordering defect. Remaining test limits in the
currently inspected diff: it does not exercise a failure after some files have been removed, an
inbox-directory sync failure, or a second successful cleanup returning zero; the code has explicit
partial-byte and idempotent paths, but these outcomes have not been proven by those tests. No test
result is claimed here. The cleanup code uses path validation followed by path-based deletion under
the documented private-inbox/no-hostile-concurrent- writer assumption; it would need handle-based
no-follow deletion if that threat model changes.

## Settled zero-byte native frame

I independently inspected `native/zero-discard/raw/05-cleanup-settled.png` at 1440×1000 from the
frozen app binary SHA-256 `4b6e1a1386fa842f1715d1e93d9bf9582af5e62336e612cfa01a4fa15080a71d`. The
first word is visibly the complete “Discarded”; the note wraps to exactly three lines. Verification,
requested carrier, requested edition, publication name, requested publisher, and the footer are all
visible, with no overlap or clipped row. The taller note shifts the panel slightly upward while
preserving the identity rows and footer spacing. This supports the settled ordinary zero-discard
frame only. The captured pending frame is a partial redraw with lower fields absent, so it does not
establish pending-state layout. Nonzero completion and cleanup-failure visuals remain unverified.
