# Round 2 critique: VLace (vortex-io-redesign.md)

Target verified: v0.10, SHA-256 `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

## Previous status

R1: 0 BLOCKING, 0 MAJOR, 0 MINOR; `VERDICT: CONVERGED`. The v0.10 snapshot retains the R1 VLace
boundary and records the VLace/VAgent resolution in the R1 disposition. No VLace-specific R1 target
edit was needed.

## R2 review

### Pending is an execution result; terminal outcomes are cells

The target gives the generic trace a `Pending` branch and says `await` may return control to the
host while execution parks (`design/vortex-io-redesign.md:484-515`). It separately defines Pending
as an execution result: the VM retains its cursor, frames, bindings, guards, and execution phase,
then resumes the same await (`:721-734`). The VLace row assigns suspension and EOF/error conversion
to its adapter (`:583`). I read these together: Pending is host/VM control state, not a terminal
`Outcome` value or a forged `IO.*` constructor to which VLace bind continues. The VLace adapter
retains its bind/then suffix while parked, then supplies the terminal typed outcome as the result of
the awaited action when resumed.

This distinction is important for the lazy language, but the v0.10 contract is coherent as written.
It also assigns frontend-local continuation state to each adapter and includes pending continuation
values/results among semantic roots (`:344-355, 780-795`). The generic `Pending | Outcome` Direct
Vortex row (`:586`) is a host execution result; the terminal `Outcome` remains inspectable cell
data. The proposed VLace shorthand neither dereferences a payload nor uses Pending as a terminal
result (`:583`). No VLace-specific correction is needed.

Current behavior remains synchronous and does not demonstrate this proposed state machine:
`runIoOnVm` uses a local continuation vector and executes `getline`, stream output, and effect
leaves inline (`apps/common/xanadu/vlace/lower_io.cpp:96-212`); `driverEvaluateIO` owns a local
arena/VM/compiler for the duration of the call (`:284-350`). The target proposes a new resumable
driver state (`design/vortex-io-redesign.md:221`) and does not imply that current behavior already
supports Pending.

### Pure buffer, admission, flush, and close are distinct

The target now separates a pure bounded frontend-local buffer from explicit `perform(flush buffer)`
admission, and from service-owned buffered writes that already have request identities
(`design/vortex-io-redesign.md:665-681, 702-707`). It states that buffered write submission freezes
one version and mints one request; subsequent await/replay does not resend (`:674-681`). The output
trace shows `WriteBytes` and `FlushAfter(writeReq)` as distinct requests; a dependent input submit
waits for successful flush observation (`:528-546`). Durable `Sync`/peer receipt are separate
guarantees, and graceful close and abort/cancel report their distinct lifecycle/progress semantics
(`:541-546, 676-681`).

Thus lazy construction, append, inspection, render, and forcing pure values stay inert; the explicit
VLace perform site defines multiplicity and fresh effect identity. A repeated perform is new work,
while an automatic resume is the same request (`:244-254, 499-505, 681`). No buffer or flush concern
remains.

### Abandonment and cleanup

The target says a graceful close drains accepted writes, while abort/cancel requests stop and
reports unsent, partial, or uncertain bytes (`design/vortex-io-redesign.md:676-681`). A frontend
continuation can be abandoned without pretending admitted bytes were rolled back; last-owner
abandonment requests best-effort cancellation and retains bounded cleanup state until backend
release acknowledgement (`:990-1004`). This is consistent with VLace's local bind stack and the
shared request lease: losing that stack does not erase the request, transfer receipt, or
uncertainty. A language scope that promises close must retain its own finalizer continuation and
issue the explicit ordered close operation; garbage collection is not a blocking close guarantee
(`:583`). No particular VLace bracket/finalizer syntax is frozen or required by this protocol
review.

### Framing and message mode

V0.10 resolves the generic zero-length datagram gap with optional `ReceiveMessage`/`SendMessage`
semantics, full boundaries, oversized-message disposition, and typed refusal for incapable adapters
(`design/vortex-io-redesign.md:393-405, 548-563`). The generic framing rules preserve bounded
read-ahead suffixes and decoder/framer state while keeping VLace tokenizer grammar frontend-local
(`:657-700`). This does not alter VLace's typed Outcome mapping or require a VLace message-mode
syntax.

## Cross-role dialogue

- **VAgent:** VAgent confirms the two admission policies remain distinct: actor intents fix semantic
  bytes/order before commit and receive durable post-commit admission; VLace's
  `perform(flush buffer)` is itself one fresh buffered-write request. `FlushAfter` is a separate
  ordered request, with Sync and close/abort separate. We agree on cleanup: abandoning VLace's
  continuation does not cancel or undo a request by implication; service-owned request/lease cleanup
  continues through acknowledgement. This confirms the buffer and finalizer interpretation in v0.10
  (`:665-681, 702-707, 990-1004`).
- **Technical writer:** The writer asked whether `perform(flush buffer)` is a local-buffer write
  request or the ordering barrier, and whether the VPL Pending finding transfers to VLace. VAgent
  and I verified the draft's distinction: the perform action submits the frozen local buffer as one
  request; a separate `FlushAfter` request is a barrier. The VPL sketch must branch because it
  dereferences payload, schedules a next read, and routes a catch-all result to abort. VLace's
  compact row does none of those; its adapter owns suspension, so Pending parks the bind
  continuation rather than becoming terminal language data. The writer's concern does not create a
  VLace finding.
- **VPL / wildcard:** The R2 VPL and writer reports identify missing Pending/terminal branches in
  the expanded VPL payload and close examples (`r2-vpl.md`; `r2-writer.md`). I agree for those
  examples. The wildcard reviewer confirms the optional message contract closes the generic message
  gap (`r2-wildcard.md`); I endorse that result and do not duplicate it as a VLace issue.
- **VQL:** Its R2 report notes that the VPL payload/dependent-read example gap does not transfer to
  a compact submit/await row that makes no dependent action (`r2-vql.md`). That is also my
  conclusion for VLace.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
