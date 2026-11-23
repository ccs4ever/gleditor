# Round 2 critique: VProlog (vortex-io-redesign.md)

Target verified: v0.10, SHA-256 `fcfa22892d147405f78b42a94ff6357727e4173bcdbdf5c19a1b881ee1d8a855`.

Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Previous findings

- **R1.1 — Message-oriented receive/send lacked message boundaries: RESOLVED.** The target now
  defines message mode as an optional endpoint capability. Each successful receive is one complete
  message, including an empty one; message endpoints do not synthesize stream EOF; one send request
  names one frozen message; boundaries and request identities survive native coalescing; unsupported
  adapters typed-refuse before admission; oversized-message consumption/retention is explicit
  (`design/vortex-io-redesign.md:393-406`). The proposed two-receive/two-send trace and acceptance
  gate cover these semantics (`:548-563, 1050`). This matches the cross-role R1 disposition.

## VProlog follow-up checks

- **Current VM mapping remains source-correct.** The target says `Compiler::solve` reaches
  `logic::compileAndRunQuery` and Prolog goals run through VM frames; it assigns the parked
  activation plus Prolog binding/trail and goal-placement policy to the frontend compiler/adapter
  (`design/vortex-io-redesign.md:135-143, 219-221`). Source confirms this path:
  `apps/common/xanadu/vprolog/compiler.cpp:243-250`,
  `apps/common/xanadu/vortex/logic_solve.cpp:79-93`, and
  `apps/common/xanadu/vortex/logic_compile_core.cpp:1-20`. Vlog's clone-rank bindings and arena
  rollback remain Prolog logic state; no such state is requested in the generic Pending protocol.
- **Logical failure stays distinct from I/O status.** The shared effect contract represents terminal
  transport status in typed Outcome cells and says it is never implicitly translated into logical
  failure; it leaves recovery mapping to each frontend (`:256-260`). Pending returns control without
  success or logical failure (`:723-730`). A Prolog adapter may choose a clause or fail after
  inspecting a typed outcome, but I/O EOF, rejection, partial progress, cancellation, overflow, or
  Unknown cannot masquerade as absence of a proof. The explicit VProlog `submit` and `await`
  predicates remain illustrative, not frozen syntax (`:477-526, 582`). The generic rule also
  provides a typed admission refusal, and the VProlog effect-placement policy belongs in its adapter
  (`:244-254, 442-449`).
- **Request identity and resumption are coherent with Prolog backtracking.** The submit action
  creates a fresh request when re-executed, including after backtracking; await observes the named
  request and never reruns submit (`:244-248, 499-503`). Pending parks the same VM cursor/frame and
  execution phase (`:723-734`). Rewind can remove local materialization while the retained
  observation remains available for a later await (`:736-750`). This fits Vprolog's compiled VM
  choices and trailed variable bindings.
- **Framed reads, flush, and cut/abandonment cleanup remain coherent.** The common host trace waits
  for a successful `FlushAfter(promptReq)` outcome before submitting `ReadLine`; Pending re-awaits
  that fence and failure prevents the dependent read (`:528-546`). Framed read-ahead keeps its
  bounded unread suffix at the endpoint; frontend tokenization stays local; raw/framed cursor mixing
  is explicit (`:683-700`). Marks, trails, discard/cut, frame disposal, and arena destruction
  participate in lease bookkeeping (`:747-750`); logical failure, exception, or arena disposal does
  not retract sent bytes, while last-owner abandonment requests best-effort cancellation and retains
  bounded cleanup until backend release (`:990-1004`). A Prolog cut can dispose of local
  choice/trail roots without claiming to rewind input or undo a completed external effect.

## Cross-role dialogue

- **VIcon:** VIcon independently verified that v0.10 resolves the shared message issue and separates
  VM yield, pure render, and an asynchronous stdout sink. We agreed that Pending carries no frontend
  control stack; VProlog preserves its VM activation/choice/trail locally, while VIcon preserves its
  Driver cursor/choices. VIcon also read the shared “pure predicates/filters” rule as applying to
  ordinary pure contexts; each frontend classifies its designated explicit submit action. That is
  consistent with the VProlog row calling submit an explicit predicate outside speculative search,
  and creates no generic execution finding.
- **Wildcard:** Wildcard's R2 report confirms the new optional message contract, oversize handling,
  typed refusal, and trace close its R1 counterexample. I confirmed that this satisfies VProlog too;
  message boundaries are generic transport semantics, while Prolog term/DCG parsing remains
  frontend-owned.
- **R1 disposition:** The editor accepted the single shared message finding and preserved ABI
  freedom and proposed-only notation. The other Prolog-relevant requirements—typed outcomes,
  same-request await, frame suffix ownership, async flush ordering, and cut/trail lease rules—remain
  coherent in v0.10. No source/runtime change or additional target correction is requested.

## MINOR

None.

VERDICT: CONVERGED
