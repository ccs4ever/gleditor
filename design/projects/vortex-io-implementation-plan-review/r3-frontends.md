# R3 frontend language review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `c1032a0507b45c7352758d0f66ac03eb328b3d080d0a60379b8d3f34e78e1891`
- Scope: R2 VPL waiter/order and Prolog admission-unwrapping fixes; remaining
  six-frontend coverage only.
- Result: **1 MAJOR, 0 BLOCKING, 0 MINOR — REVISION REQUIRED.** VPL's
  single-active-waiter left-to-right protocol now resolves the R2 concurrency
  concern. The frozen plan no longer contains any Prolog frontend row or
  callable/lowering path, despite claiming six bridge APIs and M7 coverage.

## Finding

### F1 — MAJOR: VProlog bridge contract is absent from §10

The §10 table lists VQL, VPL, VLace, VIcon, VAgent, and Direct Vortex. It has
no `vprolog / Vlog` row. Searching the complete frozen plan finds only generic
mentions of Prolog failure/purity and the first-release refusal for
speculative effects; it defines no `io_submit` form, sum unwrapping, typed
outcome term, effect placement diagnostic, continuation carrier, or resume
path for VProlog. This leaves the earlier R2 question unanswered: there is no
current Prolog code showing refusal kept as a value and only
`admitted(Req)` passed to `io_await`.

The omission conflicts with the title/scope and §2's "Six bridge APIs in §10",
the §10 instruction that every frontend bridge has a complete trace, and M7's
requirement to implement/test all six. VProlog is one of the six named
languages in the redesign, and its source boundary is real: `Compiler` builds
`CompiledQuery{goals, variables}` and `solve()` returns `LogicSolution` values
through Vortex/Vlog (`apps/common/xanadu/vprolog/compiler.hpp`). Without a
contract, the plan says neither how an admission sum is safely unwrapped nor
how Pending retains the query cursor, substitutions and trail.

**Concrete fix:** restore a dedicated VProlog row. Show a sum-aware branch or
explicit unwrapping helper so refusal binds/returns its tagged outcome and
only `admitted(Req)` reaches `io_await(Req, Outcome)`. Specify that this is a
top-level explicit effect goal, prohibited inside user clauses/speculative
search with `PROLOG_IO_EFFECT_CONTEXT`; Pending returns a runner token that
retains the compiled query, cursor, bindings/substitution and trail, then
re-awaits the same Req. Do not call refusal ordinary predicate failure. Update
M7 tests/coverage only if the existing generic checklist did not already
intend the restored row.

Evidence: frozen plan §10 lines 688–695 omits Prolog; §2 inventory line 57
claims six bridge APIs; §13 M7 requires six language bridges. Prolog source
boundary is `apps/common/xanadu/vprolog/compiler.hpp` and the solve pipeline
described in `design/vortex-io-redesign.md` §2.3.

## R2 resolution audit

| R2 finding | R3 disposition | Evidence / remaining note |
| --- | --- | --- |
| VPL multi-pending `FrontendToken` ownership and wake policy | **Resolved** | §10 now retains `VplView submissions/outcomes`, `nextPosition`, and one active `(requestCell, waiterCell, position)`; scans left-to-right, parks on first Pending, re-awaits that position on wake, then advances. Other requests continue; ready/refused slots remain fixed; activation cancellation drops only its waiter. This is a concrete deterministic protocol despite out-of-order completion. |
| Prolog awaited submission sum rather than admitted request | **Not resolved; contract omitted** | No Prolog row or `io_submit`/`io_await` example remains anywhere in the frozen plan. Restore the corrected row as described in F1. |

## Tally and verdict

| Severity | Count |
| --- | ---: |
| Blocking | 0 |
| Major | 1 |
| Minor | 0 |

**Verdict: REVISION REQUIRED.** The VPL update is adequate. Restore the
VProlog bridge contract and refusal-unwrapping trace to make the promised six
frontend implementation plan complete.

## Review limits

Read the frozen plan at the recorded SHA and the Prolog compiler/driver
boundary. No tests were run and no implementation files were changed.
