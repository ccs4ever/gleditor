# Round 6 critique: System Architect (epistemic arbitration)

Reviewed the frozen approved input, byte-identical to the current target, as a full architecture
audit. Current arena/VM behavior was checked at HEAD `5ee5f6ffb7ee2877cfdf1473d0c36602bafdab53`.
P19.1–P19.8 are approved; unimplemented prerequisites are not counted as defects. No repository
files were edited.

Tally: **0 BLOCKING, 3 MAJOR, 0 MINOR.**

## Status of previous BLOCKING/MAJOR findings (Rounds 3–5)

- Rollback boundary, rank-walk pricing, arena-cap refusal, candidate/fuel budgets, scope-result ABI,
  host-grant checks and model/learning clarifications — **RESOLVED** in the recorded R3–R5
  dispositions.

## BLOCKING

None.

## MAJOR

### 1. (§3.1, §6.3, §8) Verification supersession has no model-state lifecycle

**Problem:** The design allows a later adjudication to reverse a verdict but only defines
append-only additive Beta updates and ordinary delayed-label SGD. It does not retire the earlier
label's contribution, invalidate affected query/model snapshots, or specify deterministic rebuild
order after correction. A correction consequently leaves a source score trained on a verdict that
the current evidence says is wrong. Rebuilding is unpriced despite the bounded replay/feedback
horizons.

**Evidence:** §3.1 lines 133–139 updates once per pair; §3.3 lines 203–210 addresses delayed labels
and deterministic ordering but not supersession; §6.3 lines 567–573 authorizes a new versioned
reversal. P19.3/P19.5 costs at §8 do not include retraction/replay work.

**Fix:** Define event-scoped supersession with an immutable verification-version chain and a single
active version per adjudicated observation. Rebuild or reversibly update Beta and replay the bounded
SGD label history, version/invalidate tokens and cached features, and price/cap the work. If that
exceeds initial scope, prohibit reversal of trained observations in P19 and allow only new-event
reopening.

### 2. (§2.1, §3.1, §8) Dependence assumptions are absent from policy state

**Problem:** The ranking formula's output depends on group partition and `rho_G`; allowing
independent groups requires an audited declaration. No on-manifold record says who assigns group
membership, binds sources to a group, versions that declaration, or carries the correlation value.
This also prevents implementing predicate/domain-specific reliability without making assumptions
about the single profile-level Beta/SGD fields. A default one-group policy is safe, but the
documented independent-group mode is not implementable from the specified state or priced in the
proposal register.

**Evidence:** §2.1 lines 68–81 gives the only record topology and fields. §3.1 lines 146–159
consumes `G` and `rho_G`, but no matching schema appears. P19.1/P19.2 at §8 omit group/calibration
record costs.

**Fix:** Add policy-owned typed calibration and dependence-group records on `d.epistemic`, explicit
source-occurrence membership and provenance/version edges, and a declared `[0,1]` range for `rho`.
Price membership traversal and records. Keep unassigned sources in one conservative shared group.

### 3. (§5.3, §8.2) Diameter handling is algorithmically under-specified

**Problem:** A single-linkage component can exceed the maximum diameter while remaining connected.
“Preserved as separate clause alternatives” does not specify how that component maps into
alternatives; arbitrary partitioning changes the candidate scores and selected value. This is both a
runtime determinism risk and a resource-budgeting gap.

**Evidence:** §5.3 lines 441–445 defines the conflict but no deterministic decomposition. Acceptance
gates name independent fusion and cold start but omit a chain/over-diameter determinism case (§8.2
lines 648–650).

**Fix:** Choose and state a bounded result: keep every original clause in an oversized component as
an independent singleton alternative, without an inferred partition; add a chain test that
demonstrates endpoint separation and deterministic ranking. If a partitioner is wanted, specify its
tie-breaking and charge its extra work.

## MINOR

None.

VERDICT: **NOT CONVERGED**

VERDICT: NOT CONVERGED
