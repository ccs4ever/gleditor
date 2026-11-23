# Round 2 critique: VQL (vortex-io-redesign.md)

Target verified: v0.10, SHA-256 `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous findings

- R1: No BLOCKING or MAJOR findings were raised. The VQL requirements for explicit submit
  multiplicity, retained request references, row association, and frontend-owned continuation were
  carried forward and remain satisfied.

## R2 review

The message-mode addition preserves VQL's source semantics: the shared contract says one receive
observes a whole message, distinguishes a zero-length message from stream EOF, preserves separate
request identities across coalescing, and typed-refuses unsupported guarantees
(`design/vortex-io-redesign.md:548-563`). This does not alter VQL's row mapping. Requests retain
independent identities and outcomes; completion can reorder while VQL associates each retained
request cell with its source row (`:571-576`).

The buffering and example revisions also preserve the VQL contract. VQL's proposed row explicitly
keeps submission out of row traversal, projection, and `render`; the compiler owns submission-site
multiplicity, and querying staged rows stays pure (`:578-586`). A fresh visit to submit creates a
new request, while await/resume observes the retained request (`:499-505, 580`). Pure rendering
consumes materialized cells only; when a CLI/host writes bytes to stdout, that is an adapter stream
sink under the shared submit/await/flush protocol, not VQL render (`:513-515, 638-655, 709-716`).

The VPL framed-input example's missing Pending branch does not transfer to VQL. The VPL sketch
immediately dereferences `Seen.payload`, submits a dependent read, and routes a catch-all outcome to
abort (`:603-635`). VQL's row is only a proposed submit-then-await shorthand (`:580`); it neither
consumes a payload nor takes a dependent action from the await result. The generic worked trace
spells out Pending, terminal payload-bearing and payload-less outcomes, and same-request resumption
(`:484-505`); the VQL row reiterates that resume awaits the retained request. No VQL-specific
Pending branch defect remains.

Query early-stop and resource ownership remain coherent. VQL may stop evaluating future row-level
submit sites after a limit or short circuit; that does not erase requests already admitted. The
service retains requests outside solver marks and, on last-owner abandonment, removes unstarted work
or requests best-effort cancellation for started work, retaining bounded records and leases until
backend release acknowledgement (`:455-459`). Cancelling an awaiter stops only that waiter; request
cancellation reports progress/uncertainty (`:517-522`). Backtracking may remove materialized result
cells while retaining the request observation for a later await, and lease cleanup follows trail,
cut, and frame ownership (`:721-750`). Read-ahead suffixes remain endpoint-owned under bounded
leases, so trimming query/view results does not rewind source consumption or discard bytes needed by
a later framed read (`:683-700`).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Cross-role dialogue

- **VPL:** VPL confirms that pending payload and dependent-read handling is a real gap in its
  expanded frame example. I agree with that finding for VPL. Its generic contract fix does not need
  to be duplicated as a VQL finding: VQL's compact row consumes no payload or dependent result, and
  the generic trace already gives the Pending/same-request rule (`:484-505, 580`). VPL and I also
  retain the shared rule that stopping future submissions leaves admitted requests and prefetched
  endpoint suffixes under their existing service/endpoint ownership (`:455-459, 683-700`).
- **Technical writer:** The writer independently identifies the same VPL Pending/payload/abort
  example issue. I agree that the example should branch before payload access, defer dependent reads
  until terminal success, and inspect close/abort outcomes. VQL does not have those control-flow
  branches in its sketch, and the generic trace and ownership rules cover its adapter; no duplicate
  finding remains.
- **Wildcard / R1 disposition:** The R1 disposition confirms that independent request cells preserve
  VQL row association and that a generic group/batch identity is unnecessary absent shared group
  deadline/accounting requirements (`r1-disposition.md`, “Cross-role dialogue and scope”). The v0.10
  message and buffering changes preserve that separation; the wildcard R2 report also finds no
  remaining generic message or group gap.

VERDICT: CONVERGED
