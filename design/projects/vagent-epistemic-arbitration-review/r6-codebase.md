# Round 6 critique: Codebase Expert (epistemic arbitration)

Reviewed the frozen approved input `/tmp/vagent-epistemic-r6/input-v1.3-approved.txt`; SHA-256
matches the current design target. Source claims were checked against HEAD
`5ee5f6ffb7ee2877cfdf1473d0c36602bafdab53` in `/data/git/gleditor`. No repository files were edited.
P19 approval is recognized; absent future implementation is not itself a finding.

Tally: **0 BLOCKING, 3 MAJOR, 0 MINOR.**

## Status of previous BLOCKING/MAJOR findings (Rounds 3–5)

- R3 queue rollback, observation clocks, LinUCB API, profile/token pricing — **RESOLVED** as
  recorded in the R3 disposition and closing reports.
- R4 callback invocation/choice barrier, host grant boundary, list topology, and remaining
  ML/epistemology clarifications — **RESOLVED** in the R4/R5 dispositions. This round independently
  rechecked the full approved target.

## BLOCKING

None.

## MAJOR

### 1. (§3.1, §6.3) A corrected verification leaves old learned evidence active

**Problem:** The specification says a host adjudication may “reverse the old verdict by a new
versioned verification record,” but Beta learning adds `q*y` and `q*(1-y)` once per
verification/observation pair. An opposite new label therefore adds contrary evidence while leaving
the old label's contribution active. The verification record has no `supersedes`/active-version
field, and no retraction, replay, or deterministic model rebuild is defined. `online_sgd` likewise
has no correction path for a superseded delayed label. The adjudicated observation and the trained
source model can consequently disagree after the advertised reversal.

**Evidence:** §3.1, lines 133–139, defines additive once-per-pair updates. §2.1, lines 76–81, lists
verification fields but no supersession/version relation. §6.3, lines 567–573, promises reversal by
a “new versioned verification record.”

**Fix:** Give each verification a stable event ID plus explicit superseded-version linkage and
define the active adjudication per observation. Recompute the affected Beta epoch from active labels
and deterministically replay bounded SGD history in receive order, charging that work to budgets;
alternatively remove the reversal claim from initial P19 and retain fresh-event reopening only. Add
the record fields, invalidation behavior and cost to P19.5/P19.3.

### 2. (§2.1, §3.1) Reliability and dependence-group state lacks keys and schema

**Problem:** The score requires partitioning corroborating sources into dependence groups `G` and
using each group's `rho_G`; separate reliability calibration is scoped by predicate signature and
domain/calibration epoch. Yet the required records put a single `alpha`, `beta`, feature schema,
weights, bias and normalization state on `Source profile`, while no record defines a
source-to-predicate/domain calibration key, audited dependence-group identity/membership, or stored
`rho_G`. The prose mentions predicate-owned “calibration records” and declared audited provenance,
but does not give those records required fields or the topology that joins each source occurrence to
them. An implementation cannot honor “no cross-domain transfer” or use declared independent groups
from the specified schema; it must invent state shape and lifecycle. The valid range for `rho_G` is
also absent from the numeric validation list.

**Evidence:** §2.1, lines 68–81, identifies the ranks and required fields; §3.1, lines 137–139 and
146–159, requires domain-scoped reliability and dependence grouping; §8.1, lines 633–637, validates
numeric parameters but does not constrain `rho_G`.

**Fix:** Specify a typed calibration record keyed by source identity, predicate signature, domain
and calibration epoch, with the Beta/SGD/normalization state there. Add policy-owned audited
dependence-group records with source membership, stable provenance/version and finite
`rho_G ∈ [0,1]`; new/unassigned sources stay in the conservative common group. Define clone/rank
links and price their lookup/storage in P19.1/P19.2.

### 3. (§5.3, lines 441–445) Oversized connected components have no deterministic output partition

**Problem:** Single-linkage defines a connected component, then the design says a component above
maximum diameter is “preserved as separate clause alternatives.” It does not say whether every
original observation becomes a singleton alternative, whether an algorithm subdivides the component,
or which partition result is stable. Different implementations can therefore change cluster scores,
the selected hypothesis and the result while following the same input and threshold.

**Evidence:** §5.3, lines 441–445, specifies connected components and the maximum-diameter exception
but no split algorithm. The query contract elsewhere requires deterministic scoring/order (§5.3,
lines 447–456).

**Fix:** State that an over-diameter component is not partitioned by an inferred clustering
algorithm: preserve each original clause as a singleton alternative for scoring/ranking (or specify
and price one deterministic bounded partition algorithm). Add an acceptance case with a chain whose
endpoints exceed the diameter.

## MINOR

None.

VERDICT: **NOT CONVERGED**

VERDICT: NOT CONVERGED
