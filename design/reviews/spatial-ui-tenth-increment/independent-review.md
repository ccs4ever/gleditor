# Independent UX and design review

The J24 contract asks the reader to request an exact unavailable target, cancel without losing the
selected link/member, retry after availability changes, and avoid recording a visit before a
successful entry. The live navigation contract also requires distinct pending, unavailable and
missing-version states. The current implementation only partly covers those states.

The link panel exposes a real pending state when an exact document occurrence is selected but its
page geometry has not been built. `Enter` sets priority offsets and waits for `Doc::anchorFor()` to
succeed; the panel says “Entry pending: building exact target page” and shows a Cancel action.
Cancel is also bound to `Alt+Shift+Escape`. This can arise from normal page building for a valid,
distant occurrence, without inventing a transport or availability service. The reader's link and
both endsets remain selected. The status is available through the panel's accessibility source,
although this review did not inspect a native accessibility capture.

The other unavailable cases are not equally represented. Link occurrence resolution indexes
currently open document views and the active manifold. A member with no occurrence in those views is
described as “not in view”; Enter is disabled in the panel. There is no UI request/retry path for
that member, a missing version, or remote content. Saved-visit previews do distinguish a missing
source store, a missing version, a missing cell and a bad range, but that diagnostic is not the J24
endpoint request flow. The immediate focusability failure in `LinkContext::execute()` uses one
generic “open the exact saved document or cell version” message. These are coverage gaps in the
current product state; a fixture that never asks for a missing target is not evidence of a product
failure.

The root change addresses a separate real race. `Views::swingBackToSpan()` can complete a source
arrival in an already-open view without changing the session generation or going through
`LinkContext::execute()`. Before the change, `pendingEntry` could therefore survive that semantic
transition and steal focus later when the queued target page finished building. Clearing pending
entry and stale entry status in `recordSourceArrival()` keeps the selected link, endsets, cursors
and origin intact while preventing that delayed entry. The source arrival itself remains a completed
visit; the abandoned endpoint request adds none. Clearing document-wide priority offsets is
unnecessary and would affect the page loader beyond this request.

The added `ReaderPackageUiTest.NewSourceArrivalCancelsDeferredEntryWithoutLosingTheLink` uses an
actual `Doc` with an initially unbuilt target page, proves the request is pending, records a source
arrival, builds the target page, and advances the frame. It checks that the old request does not
move focus or add a visit, that the selected link context survives, and that a fresh explicit Enter
succeeds. It also checks explicit cancellation before retry. This is useful render/UI integration
evidence for the race and the page-building path; it is not a native Xuzz pointer journey, and it
does not establish that a person can interrupt a pending request before the next frame settles. Keep
that distinction in the final validation record.

`LinkNavigator::supply()` rejects stale generations. Resolution is synchronous in the present
`LinkContext::apply()` path, so there is no ordinary UI route to produce a late stale response
today. The guard is appropriate future-proofing; the current logs and ignored local stale result are
not a user-facing stale state. A test may verify that a stale answer cannot affect a newer
selection, but a native capture should not claim to exercise asynchronous resolution.

The pending/cancel label and control are concise and preserve the link context. The unavailable path
still needs an explicit distinction between an unresolved member, a target whose version is missing,
and content that cannot currently be fetched, with a retry action where retry is meaningful.
Cancellation should remain available from pointer, keyboard and accessibility. If ordinary page
building settles before input can interrupt it, report that as a harness limitation rather than
delaying or simulating a service response.

## Native closed-target checkpoint

I inspected the archived [native report](native-cancel/native-report.txt), lossless
[checkpoint frames](native-cancel/frames/), and per-step
[operation and visit snapshots](native-cancel/snapshots/). This is a native UI run through
authoring, Enter, Origin, closing the destination, unavailable Enter, Cancel through its default
chord and retry. It exercises the closed-target unavailable state and its Cancel action. It does not
exercise a page that is still building or a stale asynchronous completion.

The chosen context is Link 22, Left member 1/2 occurrence 2/2, and Right member 2/3 occurrence 1/1;
both cursors and the origin remain visible in the accessibility trace. Closing the target changes
the selected left occurrence to “a closed version.” Enter exposes “Unavailable: open the exact saved
document or cell version. Cancel to keep reading.” Cancel through `Alt+Shift+Escape` clears that
status; retry restores the same status. Focus remains on the origin document. The activity snapshots
show one root visit at 00-selected, one successful endpoint arrival at 01-enter-b1, then two visits
total at 02-origin through 06-retry. Thus the unavailable attempt, Cancel and retry added no visit.
The source operation dump has the same SHA-256 at all seven checkpoints
(`49a12edef0113d9b6f6b7a120c8f2f7cbf7dd1f6d865e8606dc7dfaac4a164d6`).

I independently decoded the archived 1440×1000 RGB PNG captures
[03-close-b1](native-cancel/frames/03-close-b1.png),
[04-unavailable](native-cancel/frames/04-unavailable.png),
[05-cancel](native-cancel/frames/05-cancel.png), and [06-retry](native-cancel/frames/06-retry.png).
03-close-b1 and 05-cancel are pixel-identical, as are 04-unavailable and 06-retry. Each adjacent
status transition changes exactly 106,628 RGB pixel positions: close→unavailable,
unavailable→Cancel, and Cancel→retry. This confirms the status appears, clears, and returns while
the captured frame returns to its earlier state. The Cancel button is present in the native
accessibility trace only while the status is present.

The reopened hypertime map showed a node labelled “T · Version b1,” but the captured evidence does
not establish that the opened document's store and version authority exactly match the closed
endpoint. Therefore this run does not prove an exact-version recovery defect. It also does not
demonstrate unavailable classes beyond the generic closed-target message, nor pending-page
interruption through native input before the next frame completes.

The source inspection and added `ReaderPackageUiTest` remain separate evidence: that test covers the
actual page-building race and source arrival, while this native run covers the visible
unavailable/cancel/retry flow. Neither is a stale-asynchronous-response demonstration; the current
UI resolver is synchronous. The prior two-document native run did not exercise unavailable entry and
is not used as evidence here.
