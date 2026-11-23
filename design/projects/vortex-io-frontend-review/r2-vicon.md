# Round 2 critique: VIcon (vortex-io-redesign.md)

Paths below are relative to the repository root. Target: v0.10, SHA-256
`fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (§6.5, §11.2 message boundaries): RESOLVED — §6.2 defines optional message capability, one
  complete receive/send message per request, zero-length success, boundary preservation,
  unsupported-capability refusal, and explicit oversized-message disposition. §6.5 has a concrete
  empty-then-nonempty receive and two-send trace; §11.2 requires matching evidence
  (`design/vortex-io-redesign.md:393-405,548-563,1050`).
- R1.2 (§6.5 VICON yield/stdout wording): RESOLVED — the VICON row says VM `write(e)` yields a cell,
  rendering is observational, and an external runner stream sink uses explicit write/await requests.
  A nearby proposed host-sink example keeps the existing VM yield separate from stdout submission
  (`design/vortex-io-redesign.md:584,638-655`).

## VICON follow-up checks

### Pending remains distinct from yield, exhaustion, and failure

The existing VICON `Driver::resume()` still returns `optional<CellRef>` and treats `nullopt` as no
next value (`apps/common/xanadu/vicon/driver.cpp:36-57`). That is current source behavior, not a
required future ABI. The proposal defines Pending as an execution result that returns control to the
host without success or logical failure; the VM retains its cursor, frames, output bindings, guards,
and phase, and resumes the same await. Completion never jumps back through submit
(`design/vortex-io-redesign.md:721-734`). The VICON row assigns its resume path to the frontend, and
the v0.10 table preserves ABI freedom and proposed syntax status (§6.5, lines 478-482, 584).
Therefore the Driver/compiler can replace its optional-only result with an outcome that
distinguishes produced value, exhaustion/failure, and Pending; no source-compatible shim is
required.

A ready request's materialized outcome may backtrack with the result cells, while the retained
RequestRecord allows the same live request to rematerialize the observation in another branch (§7.2,
lines 736-740). A later visit to an explicit submit site is a new request (§4, lines 230-233; §10.2,
lines 1002-1004). This fits VICON's generator behavior: resuming a parked await continues at that
await, and backtracking to the submit call creates a fresh effect.

### Framing, outcomes, and abandonment

The message additions close the earlier zero-byte ambiguity: message transport is an optional
endpoint capability; one successful receive is one complete message (including a zero-byte one),
message sends freeze one message, coalescing preserves boundaries/request identity, and a message
endpoint does not synthesize stream EOF. Oversized-message consumed/retained behavior is typed, and
unsupported adapters refuse before admission (§6.2, lines 393-405; §6.5, lines 548-563). This is a
generic transport contract, so VICON needs no message-specific grammar or opcode.

The rest of the VICON path remains coherent. Empty payload, EOF, pending work, cancellation, partial
progress, failure, overflow, and unknown status are typed data, not logical failure (§6.2, lines
388-391; §6.5, lines 493-507). Bounded byte framing, read-ahead suffix leases, cursor mixing,
frontend-local tokenization, and range reads remain generic policy with grammar in the frontend
(§6.5, lines 657-700). Lens operations and file/media ranges can use the same cells and requests;
capture stays explicit (§3, lines 209-213; §6.5, lines 484-500, 709-717; §8, lines 775-800).

Generator abandonment also has a coherent lifetime rule: lease bookkeeping follows trail release,
discard/cut, frame disposal, and arena destruction (§7.2, lines 747-750); semantic roots include
pending continuations and VM execution state (§8.2, lines 780-795); last-owner abandonment cleans up
unstarted/running native work while retaining bounded cleanup state through backend acknowledgement
(§10.2, lines 990-1004). VICON may release a waiter and branch-local cells on abandon; that cleanup
does not assert that transferred bytes were undone or silently retry the request.

## Dialogue with VProlog and other reviewers

- **VProlog:** The reviewer confirms the v0.10 optional message contract resolves R1.1. We also
  checked Pending architecture: the generic execution result distinguishes Pending from success and
  logical failure; VICON's adapter preserves its cursor/choice state locally, and its current
  `optional<CellRef>` API may change under the explicit ABI freedom. VProlog asked whether the
  shared purity rule conflicts with its explicit effect predicates. I answered that the designation
  of effectful submit syntax versus ordinary pure predicate positions belongs in each frontend;
  VICON uses an explicit gate/action, and this is not a VICON issue. No new VICON finding remains.
- **Wildcard:** Its R2 report confirms the optional message capability now closes the generic gap
  without requiring universal backend support or a batch opcode. It reports no remaining wildcard
  issue.
- **Technical writer:** Its R2 report confirms the VICON row and adjacent example now distinguish VM
  yield, pure rendering, and an optional asynchronous stdout sink. This resolves R1.2; the writer's
  remaining finding concerns VPL Pending branches, not VICON.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## VERDICT: CONVERGED
