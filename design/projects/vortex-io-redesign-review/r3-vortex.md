# Round 3 critique: Vortex & Spool-Mesh Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (Pending actor I/O lacks a turn/checkpoint contract): PARTLY — §4 now settles the actor
  mechanics and refusal/recovery behavior, but the promised P5 is absent from §12, so the existing
  actor grant/runtime changes implementing that behavior have no concrete registered scope, cost, or
  fallback.

## BLOCKING

None.

## MAJOR

### 1. (§4, §12) Actor I/O lacks a registered proposal

**Problem:** The actor policy now makes concrete implementation commitments: enforce actor I/O byte
and request-count grants, queue typed service completions as later owner turns, and refuse
checkpoint while service work remains unresolved. Those behaviors require changes to the existing
actor grant and runtime boundary. The proposal register still contains only P1–P4 and says
additional files must be registered when discovered. These files and responsibilities are already
identifiable from the new policy and live implementation, so deferring their registration leaves the
implementer without an approved scope, cost, or decline fallback. P4's VAgent adapter scope does not
cover actor grant validation, mailbox/turn scheduling, or checkpoint refusal.

**Evidence:** The new policy requires later-turn delivery and actor grant limits (§4, lines
233–241), and checkpoint refusal while work remains unresolved (§4, lines 237–240). `ActorGrant`
currently contains instruction, cell, output, solution, mailbox, and expiry limits, but no I/O
byte/request limits (`apps/common/xanadu/vortex/mesh_actor.hpp:60-74`). `MeshActorRuntime::pumpOne`
invokes the executor and applies its result synchronously
(`apps/common/xanadu/vortex/mesh_actor.cpp:611-624`), and its public contract is one message or wake
turn (`apps/common/xanadu/vortex/mesh_actor.hpp:244-250`). The register currently ends after P4,
followed only by a rule for proposals discovered by later gates (§12, lines 651–660); it has no
actor-runtime proposal.

**Fix:** Add P5 naming at least `apps/common/xanadu/vortex/mesh_actor.hpp/.cpp` for grant
accounting, owner-queued completion turns, and `NotQuiescent` checkpoint refusal, plus the exact
VAgent adapter or executor file(s) that connect post-commit outbox entries to the service. State why
these changes cannot live solely in a caller, price retained request/completion state and
grant-accounting work, and define the fallback if P5 is declined: actor post-commit service I/O is
refused with a typed outcome, while the host remains nonblocking. Keep the already specified
inline-await refusal, later-turn delivery, and no-resubmission recovery as the acceptance contract.

## MINOR

None.

VERDICT: NOT CONVERGED
