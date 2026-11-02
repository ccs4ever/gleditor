# Round 7 Epistemology Review

Target: `/tmp/vagent-epistemic-r6/revision-v1.4.md` (focused verification of the R6 findings and
their immediate interactions).

Tally: **0 BLOCKING, 0 MAJOR, 0 MINOR.**

## Previous status

**Round 6: NOT CONVERGED, 0 BLOCKING / 2 MAJOR / 1 MINOR.** The two majors concerned
correction/multiple-adjudication effects being added cumulatively to Beta/SGD, and the absence of a
persisted dependence-group membership schema. The minor concerned missing keyed storage for
predicate/domain-scoped calibration.

## R6 finding status

1. **MAJOR — corrected/multiple adjudications can train more than once: CLOSED.** Sections 2.1 and
   6.3 now specify an active-label ledger, immutable verification-version history, target
   identity/version, effective ID and predecessor links; at most one effective adjudication per
   report; predecessor compare-and-swap with atomic conflict refusal; and distinct IDs for
   correction events. Beta is recomputed from the declared prior and active ledger. SGD is reset and
   deterministically replayed in retained first-accept order, with a revision replacing its
   predecessor's slot. The replacement, defeater and model publish transactionally, and
   budget/numeric failure preserves the prior state. Cached outcomes make exact redelivery
   idempotent. This closes the R6 counterexample: if O's label changes from y=0 to y=1, the
   effective Beta state is reconstructed as prior+(0,1), not prior+(0,1)+(1,0), and the prior
   gradient is absent from replay. Concurrent revisions against a stale predecessor are refused
   rather than double-counted. Replay horizon, retained training data and state-version invalidation
   are also specified.

1. **MAJOR — dependence-group input lacked on-manifold representation: CLOSED.** Sections 2.1 and
   3.1 define a typed Dependence group record and a policy/domain calibration registry with stable
   IDs/epochs, finite rho, authority/audit evidence, and owned membership lists containing clone
   occurrences of authenticated source identities. Membership resolves by source identity and
   predicate/domain/epoch. Partitions must be disjoint, nonempty and audited; an unassigned/new
   source or unknown cross-group relation conservatively collapses the current candidate pool to one
   rho=1 group. This makes the distinct-group input representable and versioned without claiming
   that grouping authenticates principals or proves independence.

1. **MINOR — keyed domain calibration shape missing: CLOSED.** Section 2.1 specifies per-source
   domain-calibration records, with a full lookup key including source identity/incarnation,
   predicate signature, domain/schema, calibration/model epochs and feature schema. Beta counts and
   model state live on that keyed record; delayed labels require an exact target-to-live-key match.
   This rules out accidental transfer of weather calibration to pose reports.

## Focused interaction check

The active correction scheme is coherent with verification redelivery: transport ID/payload dedup is
distinct from the per-target active-label predecessor chain, while the chain keeps an immutable
audit history and only the current version trains. Each revision retains its first-accept ordinal,
so replay order does not depend on correction arrival order. Multiple authorized adjudicators can
request revisions, but stale/conflicting revisions do not become extra samples. Group registry
updates invalidate cached query state, and unknown membership remains conservative. These rules do
not imply exact Bayesian calibration or full derived-rule truth maintenance, neither of which is
required by the approved scope.

## Verdict

**VERDICT: CONVERGED.** The R6 two majors and minor are closed in the revised text. No new blocking,
major or minor epistemic/methodological issue was found in the requested focused check.

VERDICT: CONVERGED
