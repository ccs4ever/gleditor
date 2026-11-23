# Round 2 critique: VPL (vortex-io-redesign.md)

Paths below are relative to the repo root. Target snapshot: v0.10, SHA-256
`fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`. Tally: 0 BLOCKING, 1 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (shape-preserving request sketch): RESOLVED — `⊃` was removed; the table and VPL section
  state per-position mapping plus empty/ragged/shape-mismatch behavior
  (`design/vortex-io-redesign.md:581, 590–598`).
- R1.2 (VPL framed input/output lifecycle trace): RESOLVED — v0.10 adds framed input, pure decode,
  early `↑` with retained endpoint suffix, and frozen write/flush/close/abort examples
  (`design/vortex-io-redesign.md:600–635`).

## Cross-role dialogue

- **Technical writer:** The writer confirms both R1 findings are addressed, but points out the VPL
  framed example dereferences `Seen.payload` without handling `Pending` or payload-less terminal
  statuses; the output `other` arm can include `Pending` and submit `Abort` while the flush is still
  running. The writer also asks that close outcome be inspected. I agree this is a lifecycle gap,
  not a request for frozen VPL syntax: `await` explicitly returns `Pending | Outcome` in the
  language-neutral sketch, so examples should make the frontend decision visible.
- **Wildcard:** Wildcard independently agrees the optional message contract is now closed: one
  complete message per receive, zero-length is not EOF, sends retain separate identities, and
  unsupported boundary guarantees typed-refuse. Wildcard agrees with the writer on the VPL Pending
  paths and recommends same-request re-await, dependent actions only after terminal outcomes, and
  explicit cancellation when aborting a still-pending flush. No message-specific issue remains.

## BLOCKING

None.

## MAJOR

### 1. (§6.5, lines 604–629) VPL examples skip the Pending outcome branch

**Problem:** The framed-read sketch executes `Seen ← await Req`, immediately reads `Seen.payload`,
and submits `Next` without showing what happens when await returns `Pending` or a terminal outcome
without payload. The write sketch sends every non-`Complete` result, including `Pending`, into an
`Abort` submission while `FlushReq` may still be running. The document explicitly allows `await` to
return `Pending` and requires resumption to observe the same request rather than resubmit (§6.5,
lines 471–489; §7.1, lines 622–633). A frontend implementer could therefore access a missing
payload, submit dependent input too early, or race an abort against an unfinished flush. `CloseSeen`
is awaited but its typed result is not observed before the example ends.

**Evidence:** The generic trace is `seen = io.await(req)` with `Pending -> return   pending(req)`
and terminal cases separated (§6.5, lines 469–482). The VPL read sketch accesses `Seen.payload`
directly after await (§6.5, lines 603–615); the output sketch handles only `Complete` versus
`other`, with `other` immediately submitting `Abort` (§6.5, lines 620–635). The proposed Direct
Vortex signature also states `Await(...) -> Pending | Outcome` (§6.5, line 586).

**Fix:** Split each VPL await result into `Pending`, payload-bearing terminal outcomes, and terminal
outcomes without payload. On `Pending`, retain the request and resume/re-await that same request; do
not submit the next sequential read or dereference payload until a terminal result makes that action
valid. For flush, distinguish Pending from terminal refusal/failure; either re-await the same flush
or explicitly request cancellation/abort under a stated policy, preserving its partial/unknown
progress. Inspect the close outcome as typed data as well. Keep these as illustrative control-flow
cases; no new VPL opcode, core mode, or ABI commitment is needed.

## MINOR

None.

VERDICT: NOT CONVERGED
