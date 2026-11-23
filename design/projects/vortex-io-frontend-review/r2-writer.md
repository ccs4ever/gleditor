# Round 2 critique: Technical Writer (vortex-io-redesign.md)

Paths below are relative to the repo root. Target v0.10 SHA-256:
`fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`. Tally: 0 BLOCKING, 1 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (VPL `⊃` shape example): RESOLVED — the sketch no longer uses `⊃`; it states that shape and
  input-position mapping survive arbitrary completion order
  (`design/vortex-io-redesign.md:581, 590-598`).
- R1.2 (message boundaries): RESOLVED — optional message capability now defines whole-message
  receive/send, zero-length messages, refusal, oversize behavior, and examples
  (`:393-405, 548-563`).
- R1 minor (Direct Vortex Markdown cell): RESOLVED — union pipe is escaped in a normal code span
  (`:586`).

## BLOCKING

None.

## MAJOR

### 1. (§6.5, lines 603-629) VPL sketches route Pending through payload and abort paths

**Problem:** The VPL read sketch dereferences `Seen.payload` and proceeds to the next read directly
after `await`; it does not show the generic Pending case or discriminate terminal outcomes before
using payload. The output sketch's catch-all `other` arm includes Pending, yet submits `Abort` while
the preceding `FlushReq` may still be running. This leaves the example inconsistent with the shared
rule that Pending parks and later re-awaits the same request, and can teach an adapter to treat a
waiter's Pending result as a terminal flush failure. The branch awaits close/abort but does not show
how its own Pending or typed outcome is inspected.

**Evidence:** The VPL read sequence is `await Req; Data ← decodeUtf8 Seen.payload; … Next ← submit`
(`design/vortex-io-redesign.md:603-609`); its prose lists Empty, Eof, Short, Malformed, Truncated,
Overflow, and MessageTooLarge as distinct outcomes (`:611-615`) without showing the status gate. In
the output sequence, `other` submits Abort after `await FlushReq`, and `CloseSeen`/`AbortSeen` are
not inspected (`:620-634`). The generic protocol says Pending parks and a later await observes the
same request (`:499-503`); the prompt trace explicitly parks on Pending and submits the dependent
read only after successful flush observation (`:530-546`).

**Fix:** Make each VPL await branch on Pending versus terminal outcomes. Pending parks and re-awaits
the same request; decode only outcome variants that carry bytes, and handle EOF/overflow/truncation
as data before advancing the example. For output, only choose graceful close or abort after the
flush request is terminal and its progress/uncertainty inspected. Show Pending for the close/abort
request re-awaiting that same identity, then inspect the terminal close/abort outcome.

## MINOR

None.

## Cross-role dialogue

- **VPL:** The VPL reviewer independently found the Pending/payload guard, Abort race, and close
  outcome gap and included them as one MAJOR in `r2-vpl.md`. This exchange confirms the
  example-level issue. The earlier R1 array-shape and framed-I/O findings are resolved.
- **Wildcard:** Wildcard agrees that v0.10 closes the message-mode finding and identifies the VPL
  Pending handling as a frontend-example clarity issue: payload and dependent reads need a terminal
  outcome gate; Pending re-awaits the same request; Abort should not be selected from a catch-all
  that includes Pending; CloseSeen needs inspection. This confirms the one finding above, without a
  new generic transport requirement.
- **ABI/syntax and VIcon:** The draft explicitly marks §6.5 as proposed pseudocode and says the
  examples do not fix ABI (`:475-482`). VPL no longer borrows `⊃`; the Direct Vortex table renders
  with the escaped union. The VIcon row and adjacent trace separate the existing VM yield, pure
  rendering, and optional asynchronous stdout sink (`:584,638-655`). No remaining issue found in
  these areas.
- The VPL output example keeps buffer construction, admitted WriteReq, FlushAfter, and close/abort
  distinct (`:617-634`); the problem is only its Pending/terminal branching, not the buffer or actor
  staging model.

VERDICT: NOT CONVERGED
