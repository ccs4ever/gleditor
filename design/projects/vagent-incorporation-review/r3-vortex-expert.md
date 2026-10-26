# Vortex codebase review of vagent R3 incorporation

## Verdict: NOT CONVERGED — one actionable minor example error

The R3 target matches the frozen `r3-input.txt` exactly. The R1 status-table, turn-input
determinism, within-source versus cross-source scheduling, and finite actor-service findings are
closed. I found one new, concrete syntax error in the examples: a plan is named with the globally
reserved keyword `event`. Rename that plan label before treating §12.6 as a set of valid
specification examples.

## Finding

1. **[MINOR] The `WaitForEvent` example uses a reserved word as a plan identifier.**
   `plan event for WaitForEvent` appears at `design/vagent-agent-language.md:2307`. The keyword list
   reserves `event` everywhere (`design/vagent-agent-language.md:1033–1040`), while `PlanDecl`
   requires an `Ident` (`design/vagent-agent-language.md:1285`). Therefore this fenced example
   cannot parse as written, despite §12.6 correctly warning that examples are illustrative and not
   verified by a built compiler. Rename the plan label to a non-keyword such as `wait_for_event`;
   keep the goal name and event trigger unchanged.

I reviewed the examples across §12.6 for declaration/keyword use, expression types, bounded
assignment and rollback, plan-failure versus hard-turn rollback, belief source behavior, adoption
versus subgoal waiting, maintain restorers, await capture and deadlines, message staging, effect
suspension, skill replacement, behavior mounting, handler priority, cancellation and orderly
termination. Apart from the reserved plan label above, these examples are consistent with the
specified grammar and semantics. The `Destroy` example's `Logging.info` binding is listed among the
standard capacities (`design/vagent-agent-language.md:1484–1489`).

The earlier integration corrections are present: §1 has complete three-cell rows
(`design/vagent-agent-language.md:68–101`); §6.5 limits fixed ordering to each source and assigns
arbitration to P4 (`design/vagent-agent-language.md:800–805`); §11.1 requires deterministic
versioned arbitration and a positive finite actor-service-turn bound for admitted
host/ready/deadline work under sustained mailbox load, while disclaiming real-time guarantees
(`design/vagent-agent-language.md:1568–1580`); §5.6 distinguishes restored local readiness order
from external delivery order (`design/vagent-agent-language.md:607–610`); and §14 includes the
selected work item, turn time, pinned program/runtime policy/grant/limits/settings in its
determinism condition (`design/vagent-agent-language.md:2620–2626`).

## U-A1–U-A14 disposition

| U-A | Disposition | Review                                                                                                          |
| --- | ----------- | --------------------------------------------------------------------------------------------------------------- |
| 1   | Accept      | Same-pid busy-agent replacement remains deferred; initial checkpoint boundary is explicit.                      |
| 2   | Accept      | Initial single-owner boundary and separate cross-owner authority remain explicit.                               |
| 3   | Accept      | Inline payload limits remain; immutable handle/resolver extension is deferred.                                  |
| 4   | Accept      | Hosted relay remains; hostless dissemination is deferred.                                                       |
| 5   | Accept      | Single-minded commitment and drop-before-success precedence are explicit; other strategies deferred.            |
| 6   | Accept      | Colon-separated logical names remain; family-wide migration is separate.                                        |
| 7   | Accept      | Restricted suspension positions remain; continuation reification is deferred.                                   |
| 8   | Accept      | Initial single-turn determinism is distinct from optional captured replay; full selected input is specified.    |
| 9   | Accept      | StoreWriter is unavailable initially; crash-safe commit/retry and R8 authorship interpretation remain separate. |
| 10  | Accept      | Scripted harness, manual clock and mock effects are specified; implementation remains owed.                     |
| 11  | Accept      | Urgency remains deferred; ordinary P4 cross-source arbitration and finite service are required baseline.        |
| 12  | Accept      | Fixed selection remains; future selector is constrained to eligible options.                                    |
| 13  | Accept      | Same-owner typed conversation direction remains deferred and does not grant authority.                          |
| 14  | Accept      | Private per-agent program remains baseline; immutable shared base is deferred.                                  |

The document preserves the approved P1–P14 register and labels deferred proposals as unimplemented.
No builds or tests were run; this is a specification and codebase consistency review.
