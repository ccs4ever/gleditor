# Round 8 critique: Mathematician & Markdown Math Specialist (v1.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R6.1 (Dependence parameter domain): RESOLVED — finite `rho` is constrained to `[0,1]` at policy
  load and update, and invalid policy is refused before scoring (§3.1, §8.1).
- R6.2 (Oversized single-linkage component): RESOLVED — v1.4 emits deterministic singleton
  alternatives and excludes them from consensus (§5.3).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Mathematical and lifecycle checks

- The pooled score has consistent effective sample size: for nonempty contributor groups and
  `rho ∈ [0,1]`, the denominator is positive and `1 ≤ n_eff ≤ |G|`. At `rho=0`, the group
  contributes the sum of source logits; at `rho=1`, it contributes their mean. The document labels
  this a ranking heuristic and does not claim posterior calibration (§3.1, lines 161–201).
- The Beta mean and variance formulas are correct for positive shape parameters. Fractional `q`
  updates are explicitly identified as a power-posterior convention, and repeated labels for one
  observation are excluded from independent training (§3.1, lines 145–159). The replacement rule
  removes the superseded effective label by rebuilding Beta state from the epoch prior and active
  ledger; SGD replay replaces the label at its first-accept ordinal and recomputes subsequent
  gradients (§6.3, lines 630–658). This is the required replay semantics for path-dependent SGD, and
  the `O(L*d)` bound matches retained features and replay work.
- Freshness decay tends to 0.5 as age grows and preserves the initial value at age zero; it is
  distinct from physical covariance growth and liveness (§3.2, lines 212–239). Acquisition time,
  receipt time, clock mapping, sequence, and reboot behavior are separately specified (§2.1 and
  §3.2, lines 81–105, 225–230).
- Scalar continuous uncertainty is consistently a variance: `R = r I_k`, with squared units; fusion
  is confined to conditionally independent, aligned isotropic measurements, and unknown dependence
  refuses fusion (§2.1, lines 88–91; §5.3, lines 526–546). No incompatible covariance is silently
  coerced into the scalar path.
- The per-source identity and observation fields distinguish authenticated source incarnation,
  stable event identity, source sequence, acquisition and receipt times, clock map, frame/units,
  variance, and model/calibration version (§2.1, lines 81–105). Full-key equality is authoritative;
  a hash is only an accelerator. Stale labels/completions are refused against the observation's
  exact epoch key, which prevents training a replacement incarnation (§2.1, lines 93–105).
- Migration and replay requirements are specified as future P19 work, not misrepresented as already
  implemented. The live executor's current checkpoint restore copies ordinary fields and beliefs,
  but its belief migration currently clones only term/value/source dimensions
  (`agent_executor.cpp:3200–3204, 3233–3247`); it has no P19 profile/calibration/observation/token
  migration yet. The spec explicitly requires profile-first migration, remapping, full-key
  retention, and replay protection (§2.2, lines 129–139), while P19 remains unimplemented. The
  checkpoint path requires quiescence and persists the reachable state
  (`agent_executor.cpp:3343–3405`); upgrade invokes a caller-supplied reconciler at a quiescent
  boundary and commits the new context only after reconciliation succeeds
  (`mesh_actor.cpp:684–731`). The design correctly treats these as integration points and acceptance
  gates, rather than claiming the present runtime already preserves epistemic records.
- Checkpoint wall-clock initialization for request IDs (`agent_executor.cpp:3285–3293`) is not used
  by the design as an observation-time substitute; v1.4 correctly requires clock-map
  re-establishment or observation expiry after restart (§3.2, lines 225–230).

VERDICT: CONVERGED
