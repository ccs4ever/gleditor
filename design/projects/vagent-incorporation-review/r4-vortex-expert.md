# Vortex codebase review of vagent R4 incorporation

## Verdict: CONVERGED — no actionable findings

The current `design/vagent-agent-language.md` matches the frozen `r4-input.txt`. I reviewed the
three new §12.6 examples and their explanations against the language grammar, types, bounded
statement positions, failure/rollback rules, and the cited vicon prerequisite. They are internally
consistent; the scanner example explicitly identifies its unbuilt `std:goal` registrations as a
prerequisite rather than claiming current runtime support. I found no new actionable issue within
this review's scope.

The examples' core claims check out:

- **Loops** (`design/vagent-agent-language.md:2149–2169`): list and range traversal retain bounded
  body writes; the sample reaches 6 through the list then 8 through the while loop. Generator
  extraction is correctly distinguished from list traversal, and failure of the enclosing handler
  rolls back the turn. The suspension caveat agrees with the bounded-position rules.
- **Ordinary methods and contracts** (`design/vagent-agent-language.md:2171–2196`): early `return`
  exits only the method; `store` has an implicit unit result, checks its precondition before writing
  and its postcondition after; a failed precondition fails the call/handler without altering the
  prior-turn value. The restrictions on calling an effectful method from pure conditions/answers
  agree with the spec.
- **String scanning** (`design/vagent-agent-language.md:2198–2219`): `p.text ? (...)` establishes a
  scan cursor, `tab(match("ID:"))` advances over the prefix, and the subsequent `tab(upto(";"))`
  returns the intervening text. The distinction from logic-variable `?` is accurate. The vicon spec
  defines these cursor operations and generator forms
  (`design/vicon-goal-directed-language.md:329–354`); the vagent header and example correctly state
  that the needed `std:goal` registrations are not yet implemented
  (`design/vagent-agent-language.md:9`, `2218–2219`).

The R3 plan-label issue is fixed: the plan is now named `wait_event`
(`design/vagent-agent-language.md:2380`), a legal `Ident`. The earlier incorporation fixes remain
closed: §1's status table is three-column and complete (`:68–101`); §6.5 separates within-source
order from P4 cross-source arbitration (`:800–805`); §11.1 requires deterministic versioned
arbitration and a positive finite actor-service-turn bound for already-admitted local/deadline work
under sustained mailbox load, without claiming wall-clock guarantees (`:1568–1580`); §5.6 disclaims
reproducing external delivery order from restored local readiness (`:607–610`); and §14 includes
selected work and pinned program/runtime policy/grant/limits/settings in the single-turn determinism
condition (`:2620–2626`).

## U-A1–U-A14 disposition

| U-A | Disposition | Review                                                                                                   |
| --- | ----------- | -------------------------------------------------------------------------------------------------------- |
| 1   | Accept      | Same-pid busy-agent replacement remains deferred; checkpoint is the initial boundary.                    |
| 2   | Accept      | Initial single-owner boundary and separately designed cross-owner authority remain explicit.             |
| 3   | Accept      | Inline payload limits remain; immutable handle/resolver extension is deferred.                           |
| 4   | Accept      | Hosted relay remains; hostless dissemination is deferred.                                                |
| 5   | Accept      | Single-minded commitment and drop-before-success precedence are explicit; other strategies are deferred. |
| 6   | Accept      | Colon-separated logical names remain; family-wide migration is separate.                                 |
| 7   | Accept      | Restricted suspension positions remain; continuation reification is deferred.                            |
| 8   | Accept      | Single-turn determinism is distinct from optional captured replay, and selected inputs are specified.    |
| 9   | Accept      | StoreWriter is unavailable initially; crash-safe commit/retry and R8 interpretation remain separate.     |
| 10  | Accept      | Scripted harness, manual clock and mock effects are specified; implementation remains owed.              |
| 11  | Accept      | Urgency remains deferred; P4 ordinary arbitration and finite service are required baseline.              |
| 12  | Accept      | Fixed plan selection remains; a future selector is limited to eligible options.                          |
| 13  | Accept      | Same-owner typed-conversation direction remains deferred and grants no authority.                        |
| 14  | Accept      | Private per-agent program remains baseline; immutable shared base is deferred.                           |

The specification remains labeled unimplemented, preserves the approved P1–P14 register and does not
claim deferred U-A extensions are built. No builds or tests were run; this is a specification and
codebase consistency review.
