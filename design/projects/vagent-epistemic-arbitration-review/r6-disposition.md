# Round 6 disposition: full review after P19 approval

The owner approved P19.1–P19.8 and requested another full round. All ten roles independently reread
the [approved v1.3 input](r6-input-v1.3-approved.txt), not just previous closures. The input SHA-256
is `c4c9506fd39f40f673931b35f37a6abac6e0510d0ec176507167dcafe050693f`. See
[approval-p19.md](approval-p19.md) and [brief-r6.md](brief-r6.md).

Approval remains in force. The changes below make promised P19 behavior explicit; they do not add
another core proposal, implement the runtime, or promote deferred features into the release.

## Full-round results

| Role            | BLOCKING | MAJOR | MINOR | Report                                  |
| --------------- | -------- | ----- | ----- | --------------------------------------- |
| Writer          | 0        | 2     | 2     | [R6 writer](r6-writer.md)               |
| Vortex          | 0        | 0     | 0     | [R6 Vortex](r6-vortex.md)               |
| Codebase        | 0        | 3     | 0     | [R6 codebase](r6-codebase.md)           |
| Architect       | 0        | 3     | 0     | [R6 architect](r6-architect.md)         |
| PL ergonomics   | 0        | 1     | 0     | [R6 ergonomics](r6-ergo.md)             |
| Xanadu/ZigZag   | 0        | 2     | 1     | [R6 ZigZag](r6-zigzag.md)               |
| ML              | 0        | 2     | 1     | [R6 ML](r6-ml-expert.md)                |
| Mathematics     | 0        | 2     | 0     | [R6 math](r6-mathematician.md)          |
| Epistemology    | 0        | 2     | 1     | [R6 epistemology](r6-epistemologist.md) |
| Hardware faults | 0        | 0     | 1     | [R6 hardware](r6-hardware-fault.md)     |

Counts include repeated observations across personas. The hardware report's literal verdict is NOT
CONVERGED despite zero BLOCKING/MAJOR findings; its timing minor was addressed and the Round 7
follow-up converged. Original verdicts are preserved without rewriting them.

## Findings and rulings

Every finding was accepted. Closely related findings share a correction; the rows below map each
reviewer's numbered findings to the actual v1.4 contract.

| Finding                                                                                    | Reports                                                      | Ruling and v1.4 correction                                                                                                                                                                                                                                                                                      |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Superseded labels remain in Beta/SGD; several adjudicators can count one report repeatedly | Codebase M1, architect M1, writer M2, ML M1, epistemology M1 | Accepted: §6.3 defines one effective adjudication per target, predecessor-checked replacement, Beta recomputation and deterministic bounded SGD replay. A correction replaces a training example at its first-accept ordinal; it is not an additional opposite label                                            |
| Missing calibration key and exact delayed-label match                                      | Codebase M2, architect M2, ML M2, epistemology minor 1       | Accepted: §2.1 separates identity/health profiles from domain-calibration records keyed by source incarnation, predicate/domain, calibration/model epoch and feature schema. Labels/completions must match the full immutable target key before updating; reset/migration cannot transfer old labels implicitly |
| Missing dependence-group records and source membership                                     | Codebase M2, architect M2, ZigZag M1, epistemology M2        | Accepted: §3.1 defines owned group records, issuing authority/audit provenance, versioned membership lists and source clone occurrences. An unaudited/unassigned source cannot acquire a new independent vote; missing independence makes the candidate pool fall back to one common group                      |
| rho has no valid numeric domain                                                            | Math M1; part of codebase M2/ZigZag M1                       | Accepted: finite rho in [0,1], nonempty disjoint groups and validation before arithmetic. Candidate pooling intersects supporting sources with policy groups and omits empty intersections                                                                                                                      |
| Over-diameter connected component has no unique decomposition                              | Codebase M3, architect M3, ZigZag M2, math M2                | Accepted: §5.3 produces exactly one singleton alternative per original observation, marks each diameter-ambiguous and excludes them from consensus. It never infers an arbitrary subset partition                                                                                                               |
| Ranked ties both enumerate and fail                                                        | Writer M1, ergonomics M1                                     | Accepted: §4 gives mode-specific outcomes. Truest fails on an indeterminate maximum; consensus fails without one eligible component; ranked enumerates eligible ties and fails when exhausted                                                                                                                   |
| Verification-version links not inspectable                                                 | ZigZag minor 1                                               | Accepted: immutable verification versions and predecessor references use owned clone occurrences; an active ledger entry names its effective version without shared-endpoint fan-in                                                                                                                             |
| ACCEPTED eligibility wording conflicts with UNTESTED exception                             | Writer minor 1                                               | Accepted: §5.3 states the eligible set directly, with UNTESTED allowed only for policy-enabled truest/ranked cold-start hypotheses                                                                                                                                                                              |
| Duplicate target IDs confused with committed delivery replay                               | Writer minor 2                                               | Accepted: duplicate target IDs within a payload are invalid; an exact committed event replay returns and acknowledges its cached outcome, without retraining; failed/uncommitted delivery can retry once                                                                                                        |
| LCB/age-decay order unspecified                                                            | ML minor 1                                                   | Accepted: §3.4 selects the keyed learner, applies Beta-only LCB, then observation-age decay, then logit clamp/pooling. The aged result remains a heuristic warrant score                                                                                                                                        |
| Clock window mistaken for physical state-time alignment                                    | Hardware minor 1                                             | Accepted: §5.2 requires a shared physical target time. The tolerance bounds residual timing/alignment uncertainty; it cannot authorize averaging asynchronous raw positions. Upstream propagation supplies full compatible variance, otherwise fusion abstains                                                  |

For supersession, starting with Beta(1,1), a positive label of weight 1 yields Beta(2,1). Correcting
that same observation to negative must yield Beta(1,2), not Beta(2,2). Later SGD gradients depend on
earlier weights, so replay replaces the corrected example and recomputes subsequent steps; a single
compensating gradient is insufficient. The active ledger, features and replay have explicit
state/fuel bounds and O(L\*d) cost. Budget/numeric failure leaves the old effective verdict and
model intact. An out-of-horizon correction is explicitly refused rather than approximated.

The lead checked the original text and current arena/VM behavior for the source-backed claims. The
new state is expressed as owned records/ranks in Vortex; C++ remains at the approved VM and host
authority boundaries. P19.1/P19.2 prices now include calibration/group records and lookup walks;
P19.3/P19.5 include bounded training history, replay and invalidation.

## Follow-up details

The core follow-up found one ambiguity introduced by the correction wording: a new adjudication
revision must not reuse a transport event ID with changed contents. Accepted: every correction has a
new immutable event ID, while per-target revision/predecessor metadata describes the chain. Delivery
deduplication still rejects an old ID with a changed canonical payload.

The clarified schema also makes default-strategy resolution explicit: predicates without a policy
keep base query semantics; policy-bearing predicates use their declared strategy unless explicitly
overridden, with the remaining fields validated for compatibility. Numeric validation requires
positive learning rate and valid risk/threshold/tolerance domains. These are specification
clarifications within P19.4/P19.2, not a new implementation scope.

All affected roles checked the corrected v1.4 draft in [Round 7](r7-disposition.md).
