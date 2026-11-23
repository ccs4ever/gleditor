# R4 frontend language review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `86b68b2aab972fbe0cee7c5b81f20ab6da219cc3fbc0b79d86d7905b47c273ed`
- Scope: restored VProlog row and six-frontend coverage; VPL waiter ordering, Prolog admission
  unwrapping, callable forms, placement, outcomes and parked ownership.
- Result: **0 BLOCKING, 0 MAJOR, 0 MINOR — ACCEPT.** The VProlog row is present again and unwraps
  the managed submission sum before awaiting. All six language rows are present; the VPL
  one-active-waiter protocol is concrete and consistent with the shared frontend token.

## Checks

### VProlog/Vlog

The planned query is:

```prolog
io_submit(Cap, read(Off, Max), Submission),
( Submission = admitted(Req) -> io_await(Req, Outcome)
; Outcome = Submission )
```

This fixes the prior sum/await mismatch: only the `admitted(Req)` arm reaches `io_await/2`; refusal
binds the typed `refused(OutcomeCell)` value to `Outcome`. This is compatible with the current
lexer/parser's `->` and `=` operators (`apps/common/xanadu/vprolog/token.hpp`, `lexer.cpp`,
`parser.cpp`), with the plan explicitly assigning effect-goal recognition and lowering to a
query-driver extension. It keeps refusals and terminal I/O outcomes out of Prolog logical failure.

Placement is restricted to an explicit top-level query effect goal outside user clauses and
speculative search, with `PROLOG_IO_EFFECT_CONTEXT` for other placement. Pending is runner state,
not a solution or failed goal; the parked activation retains `CompiledQuery`, VM cursor,
substitutions/bindings, and Vlog trail/choice state, then resumes the same request without
re-running submit. These are the needed Prolog-owned continuation and backtracking boundaries; no
native request ID enters a term.

### VPL

The row retains the shape-preserving `Admitted`/`Refused` sum, requires equal shapes before
admission, and admits left-to-right. One active waiter per VPL activation parks on the first pending
position. Other requests may complete out of order and retain their outcomes; wake re-awaits only
the parked position, retires its waiter, then continues scanning left-to-right. Refused and terminal
positions are skipped without changing shape. Activation cancellation drops its waiter only. The
carrier fields `submissions`, `outcomes`, `nextPosition`, current request and waiter are sufficient
to correlate wakeup and preserve position. This resolves R2/R3.

### Six-language roster and boundary

The table has VQL, VPL, VProlog/Vlog, VLace, VIcon, and VAgent rows. Direct Vortex remains
explicitly identified as the shared-host baseline, not a seventh bridge. The common protocol
continues to make submit refusal a managed sum value, keeps Pending separate from terminal outcomes,
and leaves RequestId/WaiterId/ContinuationId out of language values. Frontend syntax, placement,
multiplicity and continuation mapping stay in frontend runners; the plan does not add
language-specific policy to VortexCore/VM.

## Verdict

**ACCEPT.** No remaining frontend issue found in the reviewed scope. The grammar extensions and
result carriers are still planned APIs, but the plan labels them as such, assigns explicit
lowering/placement rules, and defines unsupported gates without a synchronous fallback.

## Review limits

Reviewed the frozen plan at the recorded SHA and current VProlog parser/token support for its
proposed `->`/`=` expression. No tests were run and no implementation files were changed.
