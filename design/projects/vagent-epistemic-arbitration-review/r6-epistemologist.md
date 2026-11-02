# Round 6 Full Review: Epistemologist & Methodology of Certainty

Target: frozen `/tmp/vagent-epistemic-r6/input-v1.3-approved.txt` (SHA-256 verified against
`brief-r6.md`)

Tally: **0 BLOCKING, 2 MAJOR, 1 MINOR.** This is a fresh full pass. It treats P19.1–P19.8 as
approved design proposals while respecting the explicitly deferred exact posterior and general
derived-rule AGM/dependency closure.

## Previous status

- **Round 5: CONVERGED, 0/0/0.** The host/operator grant boundary, refusal of plan-issued labels,
  source self-certification restriction, and ranked tie grouping remain present (§4, §5.3, §6.3).
  Those findings stay resolved.
- The R3 warrant-score, assertion identity/time, unknown-dependence/Sybil-claim, and event-scoped
  defeater concerns are still substantially closed (§2.1, §3.1–§3.2, §6.3). The two majors below
  concern a new unaddressed lifecycle: what happens when an adjudicated label is repeated or
  corrected, and how correlation groups are represented.

## BLOCKING

None.

## MAJOR

### 1. A corrected or multiply adjudicated observation can train the source more…

**Full title and location:** 1. (§2.1, §3.1, §3.3, §6.3) A corrected or multiply adjudicated
observation can train the source more than once, and old learning is never withdrawn

**Evidence:** The Beta update is additive and is defined “exactly once per verification/observation
pair” (§3.1, lines 126–135). Verification records have their own stable IDs and observation IDs
(§2.1, lines 76–81). The design explicitly permits a host adjudication to reverse an old verdict
with a new versioned verification record (§6.3, lines 567–572). Label processing deduplicates by
verification ID and observation ID and updates the Beta/SGD models (§3.3, lines 203–210; §6.3, lines
555–565), but no field or rule identifies a superseded verification, one active label per
observation, or a way to withdraw/recompute an earlier Beta contribution or SGD gradient.

**Counterexample:** For one source observation `O`, an authorized verifier first records `y=0,q=1`;
starting from Beta(1,1), the source becomes Beta(1,2), `μ=1/3`, and the negative SGD gradient is
applied. Later a new versioned verification corrects that same observation to `y=1,q=1`. Under the
written additive update, it becomes Beta(2,2), `μ=1/2`, and the positive SGD gradient is added on
top of the old negative gradient. The corrected active judgment should be represented by the
positive label alone (Beta(2,1), `μ=2/3`, from that same initial prior), or by a clearly specified
adjudication-resolution policy. Similarly, two authorized adjudicators who issue distinct
verification IDs for the same observation can both increment the source's evidence count;
duplicate-ID rejection does not stop the same evidence/observation being relabeled under new IDs.
That counts repeated verdicts as repeated source reports and can inflate or cancel reputation based
on adjudication traffic.

**Minimal fix:** Maintain an active adjudication ledger keyed at least by
`(observation ID, source, predicate/domain/calibration epoch)`, with explicit `supersedes`/version
semantics. Only the effective resolved label should count as one source-report outcome. If
authorities conflict, keep the label pending or resolve it through the host/operator grant policy;
do not silently count both votes as separate observations. On a correction, recompute the affected
Beta sufficient statistics and deterministically replay SGD labels from the affected point (or
disallow corrections after a documented finalization point). Deduplicate the adjudication
evidence/observation pair as well as the transport verification ID.

### 2. The scoring formula needs correlation groups, but the approved state model…

**Full title and location:** 2. (§2.1, §3.1) The scoring formula needs correlation groups, but the
approved state model has no group-membership record

**Evidence:** The pool scores sources partitioned into groups `G`, applies a group-specific `rho_G`,
and uses those groups to decide whether the evidence is downweighted (§3.1, lines 146–159). It says
unknown dependence defaults to one group and independent groups require declared, audited
provenance. The on-manifold record inventory in §2.1 (lines 68–81) defines source profiles,
observations, query tokens and verifications, but no correlation-group/provenance record or
source-to-group relation. Its observation record records source identity, event ID and schema but
not ancestry/dependence group; the listed policy fields in §4 (lines 322–327) do not define one
either. Thus the conservative all-unknown-in-one-group case is implementable, but the distinct-group
case used by the formula has no specified input or lifecycle. An external table or ad hoc operator
convention would decide a material part of ranking despite the document's on-manifold state rule.

**Counterexample:** Two sensors have separately authenticated identities but share an antenna, map,
clock or learned detector. Another pair has separately audited independent modalities. Without a
stored membership/provenance record and rules for its assignment/version, scoring cannot distinguish
those cases: either place every source in the one unknown group and never realize distinct-group
corroboration, or provide an undocumented group partition that could overcount common-mode errors.
The explicit warning that a group is not a Sybil defense does not itself specify the required
grouping data.

**Minimal fix:** Add an on-manifold correlation/provenance record keyed by a stable group ID, with
source memberships, the provenance/common-cause basis, `rho` or dependence classification, assigning
authority/evidence and version. Define how a source observation resolves to its group(s) for a
candidate and how group changes affect retained observations. Keep unknown membership in the single
conservative group and retain the statement that grouping is not Sybil resistance; no proof of
independence is required here.

## MINOR

### 1. (§2.1, §3.1) Predicate/domain-scoped competence has no explicit keyed storage shape

**Evidence:** §3.1 says reliability is scoped by predicate signature and domain/calibration epoch
with no implicit transfer (§3.1, lines 133–139). The profile record, however, lists only one
`alpha, beta` pair and one learned weight set (§2.1, lines 76–80). Predicate descriptors are said to
head calibration records, but no required calibration-record key or link from a source identity to
its per-domain statistics is given (§2.1, lines 68–74).

**Counterexample:** A source is well calibrated for weather reports and poor for pose estimates. If
the listed profile-wide Beta pair is used, weather confirmations raise the pose score despite the
explicit no-transfer rule. If the intent is a separate per-predicate record, its key and lookup rule
are missing from the schema.

**Minimal fix:** Specify a `ReliabilityCalibration` record keyed by
`(source identity, predicate signature, domain/calibration epoch)` and name where its Beta state and
model parameters live. This is a small schema clarification; the intended semantic boundary is
already stated.

## Other reviewed scope

- **Warrant semantics:** The score is explicitly an operational heuristic, not posterior
  probability; exact posterior is correctly deferred (§3.1, lines 141–168).
- **Verification evidence and authority:** Host/operator grants, independent evidence, no
  self-certification by the reporting source, negative-observation coverage requirements and
  separate transactional verification turns are explicit (§4, lines 291–313; §6.3, lines 542–565).
  These do not eliminate adjudicator fallibility, which the document acknowledges.
- **Observation identity and time:** Stable observation IDs, authenticated source identity,
  acquisition/receive times, clock mapping, frame/schema and replay protection are specified (§2.1,
  §3.2); verdicts target referenced observations rather than mutable current claims (§6.3).
- **Refutation and defeaters:** Exact event IDs/windows, bounded replay horizon and new-event
  reopening are explicit; general Horn-rule closure is properly deferred (§6.3, lines 567–576).
- **Covariance/fusion interpretation:** The text keeps physical variance and score separate, labels
  trust inflation as a robustness heuristic, requires conditional error independence for consensus,
  refuses unknown dependence, and disclaims validated coverage absent calibration (§5.2–§5.3, lines
  403–478). I find no additional epistemic blocker here within the stated initial scope.

## Verdict

**VERDICT: NOT CONVERGED** — a host correction cannot currently replace its former training effect,
and the algorithm lacks an on-manifold representation for its dependence-group input. The
domain-scoped calibration schema also needs a small keyed-record clarification.

VERDICT: NOT CONVERGED
