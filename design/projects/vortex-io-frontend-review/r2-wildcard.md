# Round 2 critique: Wildcard (vortex-io-redesign.md)

Target: v0.10, SHA-256 `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (§6.5, §11.2): RESOLVED — §6.2 now defines optional message-oriented endpoint capability,
  `ReceiveMessage` and `SendMessage` semantics, including complete-message boundaries, zero-length
  success, no synthetic stream EOF, typed unsupported-capability refusal, and oversize consumed vs.
  retained behavior. §6.5 gives a concrete trace and §11.2 requires corresponding acceptance
  evidence.

## Review of the revised message contract

The new contract closes the ambiguity without requiring universal datagram support or changing byte
stream semantics. A zero-byte datagram remains a complete message with its own request/outcome
identity; it is neither stream EOF nor an incomplete frame. A send request freezes one message, and
native coalescing cannot merge semantic request or message boundaries. When an incoming message is
larger than the requested bound, `MessageTooLarge`/overflow cannot masquerade as a complete partial
message; the adapter declares whether it consumed/discarded the message or retains it for a later
bounded receive, and refuses the capability if it cannot report that fact. The generic host can
therefore request message semantics explicitly, while a stream-only or otherwise incapable backend
can refuse before admission.

The other wildcard scenarios remain adequately separated by capability and backend behavior:
full-duplex progress is bounded per endpoint direction/request where the adapter permits it, with
unsupported guarantees typed-refused; mutable file/media reads bind retained observations and
bounded byte ranges to declared source consistency; flush/close, cancellation, partial effects, and
late completion have separate typed lifecycles; and endpoint read/write ordering follows declared
backend concurrency rules. Nothing in the message addition requires a VPL array policy, frontend
lexer, universal datagram backend, or new group/batch opcode.

## Cross-role dialogue

- **VPL:** VPL asked whether the new contract leaves a wildcard issue. I found none in the generic
  message boundary. Its request shape remains per-position in lowering, while request identity and
  message framing stay transport-level.
- **Technical writer:** The writer independently verified the optional message capability, full
  message receive/send semantics, zero-length behavior, no synthetic EOF, oversize handling, and
  refusal path. We also discussed a separate VPL example ambiguity: its framed trace accesses
  `Seen.payload` and submits a later read without illustrating how `Pending` is re-awaited; its
  output `other` arm can include `Pending` yet submit `Abort` while the flush may still be running.
  I agree this merits a VPL/writer example correction, not a wildcard or shared message-protocol
  finding. I sent the issue to VPL; it should show terminal-status gates and explicit same-request
  re-await/cancellation policy.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## VERDICT: CONVERGED
