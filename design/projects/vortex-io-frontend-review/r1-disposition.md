# Round 1 disposition: vortex-io-redesign.md

## Snapshots and outcome

- Reviewed target: v0.9, SHA-256 `a6325b19e99bdec6f7f6cb6724ed7a6b5ddecf1e7152e345194ae6872eb58535`.
- Revised target: v0.10, 2026-11-22, SHA-256
  `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.
- The v0.8 six-critic convergence remains historical; these eight v0.9 reports are a new review.
- Raw tally across eight reports: 0 BLOCKING, 9 MAJOR, 1 MINOR. These consolidate to four distinct
  major corrections and one minor Markdown correction. VQL and VLace converged in round one; VPL,
  VProlog, VIcon, VAgent, wildcard, and technical writer remain open for round two.

| Role             |     R1 tally | R1 verdict    | Disposition                                                                                                     |
| ---------------- | -----------: | ------------- | --------------------------------------------------------------------------------------------------------------- |
| VQL              | 0B / 0M / 0m | CONVERGED     | No finding. Retained row association, explicit submit multiplicity, and request-cell collection are sufficient. |
| VPL              | 0B / 2M / 0m | NOT CONVERGED | Accept both: remove the misleading `⊃` and add concrete shape/framing/input/output examples.                    |
| VProlog          | 0B / 1M / 0m | NOT CONVERGED | Accept shared message-mode finding. Its source correction confirms the current VM-frame execution path.         |
| VIcon            | 0B / 2M / 0m | NOT CONVERGED | Accept shared message-mode finding and clarify VM yield, pure render, and stdout sink as separate layers.       |
| VLace            | 0B / 0M / 0m | CONVERGED     | No VLace-specific finding. Its dialogue confirms buffer/flush/close phases.                                     |
| VAgent           | 0B / 1M / 0m | NOT CONVERGED | Accept shared message-mode finding; no additional actor-stage/admission change.                                 |
| Wildcard         | 0B / 1M / 0m | NOT CONVERGED | Accept shared message-mode finding.                                                                             |
| Technical writer | 0B / 2M / 1m | NOT CONVERGED | Accept duplicate VPL-shape and message findings; accept Direct Vortex table-cell fix.                           |

## Verified corrections

### 1. Optional message transport is a normative cell protocol capability

**Finding:** Empty payload versus stream EOF and generic adapter-supported framing did not say that
a message request preserves one complete message boundary, including a valid zero-length message.
The datagram requirement appeared only as a test prompt. This was independently reported by VProlog,
VIcon, VAgent, wildcard, and technical writer; it is one shared transport gap, not five frontend
protocols.

**Accepted correction:** The generic protocol now distinguishes optional message transport from byte
streams. `ReceiveMessage` yields one whole message with length/boundary metadata; `SendMessage`
names one frozen message. Zero-length messages do not become EOF; message transports do not
synthesize stream EOF. Native coalescing preserves message boundaries and independent request
identities. A backend without the requested guarantee typed-refuses before admission. Oversize
handling identifies whether the message was consumed/discarded or retained and never presents a
partial payload as a complete message. Examples and acceptance coverage include an empty then
nonempty receive and two independent sends.

**Source check:** Current design distinguished empty payload from EOF but only specified byte reads
and generic adapter framing (`design/vortex-io-redesign.md` v0.9 §6.2 and §6.5). None of the cited
current adapters implements a general message service; v0.10 defines a proposed optional contract,
not a current implementation claim or universal backend requirement.

### 2. VPL shape-preserving request syntax and concrete I/O lifecycle

**Finding:** `⊃ submit¨ Caps ReadSpecs` used VPL's existing disclose/first operator as if it
preserved the mapped request array. The row also lacked a VPL-specific framed input/decode and
output/flush example.

**Source check:** VPL documents `⊃` as disclose/first and clone-master resolution
(`design/vpl-array-language.md:93-94,278-289`). Its current lowering returns the input when not
enclosed and reads through the enclosure link otherwise
(`apps/common/xanadu/vpl/lower_enclose.cpp:75-104`). The request sketch therefore contradicted the
shape promise.

**Accepted correction:** The VPL row now uses `Reqs ← submit¨ Caps ReadSpecs; Out ← await¨ Reqs`
with input-position/shape preservation independent of completion order. A proposed VPL example
specifies empty and ragged views, per-position admission outcomes, bounded framed reads, pure
decoding, `↑`-limited view consumption without source rewind, retained endpoint suffixes, and
buffered output followed by flush outcome and explicit graceful close/abort. This is frontend
lowering and policy; no VPL/core shape or tokenizer opcode is proposed.

### 3. VIcon yield versus runner stdout I/O

**Finding:** “Yield/print alone performs no I/O” overclaimed: the VIcon VM's `write` yields a cell,
while the standalone runner prints yielded values to stdout.

**Source check:** `Compiler::emitCall` emits the VIcon yield/tee path
(`apps/common/xanadu/vicon/compiler.cpp:793-822`); the CLI's `printResults` and `runAll` sink use
`std::cout` (`apps/vicon/main.cpp:41-46,138-152`).

**Accepted correction:** The VIcon row and proposed async example separate yielding a cell to the
host, pure rendering of an available value, and the host's optional stdout stream sink. When that
sink is an external stream, the adapter freezes bytes and uses shared submit/await/flush; it is not
silently exempted from the async contract. No current CLI behavior is claimed to be asynchronous.

### 4. Direct Vortex table cell

**Finding:** The literal `|` in `Pending | Outcome` split the Markdown table cell and escaped
backticks rendered incorrectly.

**Accepted correction:** The cell now uses a normal code span with an escaped union bar:
`Pending \| Outcome`. This is presentation-only.

## Cross-role dialogue and scope

- VQL/VPL and wildcard agree request identity is independent of completion order. VQL retains row
  association; VPL lowering retains input shape/position. A rank of request refs is sufficient for
  cohorts unless a later use case proves common group deadline/accounting/bulk selection; no batch
  opcode, atomic admission, or group-level outcome is implied.
- VProlog/VIcon/wildcard/writer agree that the datagram test prompt was not a contract. VAgent and
  VLace independently endorse the same optional message boundary/refusal rule; no frontend grammar
  enters the service.
- VProlog's R1 source check corrects the stale recursive-resolver account in the older lens bridge:
  `Compiler::solve` calls `VortexStdLib::solveQuery`, which calls `logic::compileAndRunQuery`
  (`apps/common/xanadu/vprolog/compiler.cpp:243-250`,
  `apps/common/xanadu/vortex/logic_solve.cpp:79-93`,
  `apps/common/xanadu/vortex/logic_compile_core.cpp:1-20`). The frozen v0.9 target already reflected
  this; v0.10 keeps that source-verified statement. The old lens-bridge description is not rewritten
  here.
- VLace/VAgent dialogue confirmed pure local buffering versus admission, actor semantic freeze
  before commit versus durable post-commit admission, service-side coalescing, flush barrier versus
  durable sync, and close versus abort. Those resolved policies were retained, not reopened.
- ABI freedom remains explicit: there are no production users or ABI shims to preserve. Examples and
  protocol terms remain proposed; existing on-disk/wire version rules and `CompactOpNode` invariants
  continue to apply. No runtime or source code changed in this round.

## Round 2

All eight reviewers will read the revised v0.10 snapshot, exchange at least one substantive point
with a peer, and issue a round-two verdict. VQL and VLace may confirm convergence without a new
frontend-specific finding; their response should still address cross-role changes. The editor will
verify each new finding against current source and record the next disposition before another target
revision.
