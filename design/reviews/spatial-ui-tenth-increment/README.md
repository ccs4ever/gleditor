# Tenth spatial UI checkpoint

This increment starts at `237d6b1e` and fixes a stale local endpoint request surviving a completed
source return. Native unavailable-target/cancellation evidence is recorded separately from the
owning deferred-page regression; neither is promoted to complete J24/J25 acceptance.

## A source return supersedes a deferred entry

`LinkContext::execute(Enter)` can defer a chosen endpoint until its exact local page geometry is
built. Most subsequent Link commands clear that request. Pouch Swing Back uses
`Views::swingBackToSpan()` and `recordSourceArrival()` instead. Returning to an already-open source
need not change the session's view epoch, so the old request could survive that legitimate arrival.
When its page became ready, `advanceFrame()` replayed the old Enter, moved focus and appended an
unwanted completed visit.

`recordSourceArrival()` now clears the pending request and stale entry status before recording the
new source arrival. It retains the pinned Link, both endsets, side/member/occurrence cursors and
origin. The source arrival still records its existing legitimate visit. Shared document build
priorities are retained; no loader, service, camera, serialization or public API is added.

The owning regression uses a real `Doc` whose target page has not been built, establishes pending
entry, exercises explicit Cancel, starts another pending entry, and records an independent source
arrival without changing the session epoch. It then builds the real target page and advances the
frame. Before the fix, the old entry remains pending, steals focus and adds a third visit where only
the two legitimate arrivals should exist. After the fix, no latent focus or visit occurs; a fresh
explicit Enter still succeeds. Link state and source operation count remain intact. This is a
deterministic UI integration test with a recording render device, not a native pointer reproduction
of the timing window.

The baseline test fails as recorded in `red-test.log.gz`; the final UI suite passes 235/235. The red
source artifact records the minor later assertion guard separately. Canonical GNU Make builds
succeed. `candidate-provenance.json` binds the product patch, source and test binaries. The
unchanged shared library and library/Xuzz test binaries allow reuse of the previous 799/799, 99/99
and owning mip/cache 53/53 results; they are not new suite runs or performance acceptance.

## Native scope and remaining gates

The native run authors a real quotation and a two-left/three-right Link through existing controls,
then exercises a closed exact target. Seven measured checkpoints record visits 1→2→2 and
byte-identical source operation dumps through Origin, closing the target, unavailable refusal,
Cancel and retry. Left place 2/2 and Right member 2/3 remain selected. Decoded frames show the
unavailable message clearing on Cancel through `Alt+Shift+Escape` and returning on retry; the
closed/Cancel frames match exactly, as do the refused/retry frames. Lossless frames, snapshots,
command lines and frozen binary hashes are retained under `native-cancel/` with an independent
review.

The Hypertime map exposes a visible `Version b1` action and opens document text, but the saved
occurrence still reports a closed version. Exact authority of that reopened view has not been
established alongside the saved occurrence; recovery is unresolved, not accepted or diagnosed from
the title alone. Establish that identity and verify a subsequent explicit Enter in the next
increment. The native unavailable route exercises existing UI behavior; the new source-arrival fix
is proved by the separate owning red/green regression.

Actual native interruption of a naturally pending page build is not established by the UI test. The
current settled-input automation can miss a short pending interval; do not simulate a missing
service or inject latency to claim a journey pass. Asynchronous remote resolution is not present in
the current synchronous path, so stale remote-response acceptance remains open. Freshly resolved
members with no open occurrence, missing versions and locked/remote targets need their own native
coverage and clearer taxonomy. Complete J24/J25, the 16+16 journeys, native AT-SPI and optimized
performance remain open. Publication/pin-lifecycle review still precedes D2 history enumeration.
