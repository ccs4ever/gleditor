# Round 3 critique: Programming Language Design & Ergonomics Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2: No BLOCKING or MAJOR findings. The R2 convergence remains valid for the shared effect contract
  and terminal outcomes; this round's finding is new and follows the actor-policy change.
- R1.1 (Shared effect rules stop at the runtime boundary): RESOLVED — the shared rules still define
  fresh Submit, same-request Await, observational rendering, pure guards/predicates/read-only
  answers, and frontend placement/multiplicity obligations (§4,
  design/vortex-io-redesign.md:203–217).
- R1.2 (Terminal outcomes have no language-level result rule): RESOLVED — typed Outcome cells,
  distinct normal EOF, partial payload/progress retention, and frontend recovery obligations remain
  explicit (§4, design/vortex-io-redesign.md:219–223).

## BLOCKING

None.

## MAJOR

### 1. (§4, lines 207–211 and 227–235) Actor refusal comes after the shared Submit/Await split

**Problem:** The shared contract makes Submit the action that admits a new request and Await a
separate observation of an already retained request. The actor rule then says the bridge refuses
inline await “before admitting the request.” If code has already executed Submit to obtain that
request, refusing a later Await cannot undo admission or ensure that no external effect occurred.
The text leaves three materially different policies unresolved: statically reject direct Submit and
Await in an atomic actor callback before execution; allow a reversible staged VAgent intent but let
only the post-commit dispatcher perform IoService Submit; or reject a Submit/Await combined
operation as one unit. These have different effects and recovery behavior. The sentence cannot be
read as a runtime refusal of Await after a distinct Submit while preserving the shared rule that
each Submit admits work.

This also leaves the promised VAgent capability unclear. Existing VAgent distinguishes plan effect
calls at suspension positions from handler effect calls staged without waiting
(design/vagent-agent-language.md:1130–1135); the worked contract says a plan effect stages work and
suspends the plan, whereas a handler stages without receiving its result locally
(design/vagent-agent-language.md:2574–2581). The redesign should say which actor constructs remain
usable under the new actor turn policy and how their staged intent reaches the service.

**Evidence:** The shared contract says every evaluation of Submit “admits a new request” and Await
“observes a named retained request” (§4, design/vortex-io-redesign.md:207–211). The actor rule says
an inline-await placement is refused “before admitting the request” (§4,
design/vortex-io-redesign.md:227–230), while the next paragraph permits a VAgent post-commit effect
to “admit asynchronous service work” (§4, design/vortex-io-redesign.md:233–241). Existing VAgent
rules distinguish a plan's suspending effect from a handler's non-waiting staged effect
(design/vagent-agent-language.md:1130–1135, 2574–2581). The acceptance gate checks refusal before
admission but does not distinguish direct Submit/Await from post-commit staging
(design/vortex-io-redesign.md:634).

**Fix:** State the actor rules separately for the two operations. Specify whether direct Submit and
Await are statically rejected in an atomic actor callback before evaluation/admission. Preserve any
allowed VAgent effect syntax as a reversible staged intent, explicitly not an IoService request;
only after the actor turn commits may the dispatcher call Submit, then deliver its typed completion
in a later ordinary actor turn. If another policy is intended, define how it prevents admission
before the await refusal without deduplicating or retracting a Submit. Extend the actor gate to
prove that direct Submit/Await refusal admits no request and that an allowed post-commit intent is
submitted once and is not resubmitted after restore.

## MINOR

None.

VERDICT: NOT CONVERGED
