# Round 3 Critique: Epistemologist (vagent Epistemic Arbitration)

Target: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2

Tally: **0 BLOCKING, 5 MAJOR, 1 MINOR.** This review checks epistemic semantics and update
provenance; it does not re-audit the implementation details owned by other critics.

## Previous-finding status

- **R1 BLOCKING/MAJOR:** The R2 disposition reports the seven R1 major findings resolved in v1.2.
  The cited changes are present: plan outcomes are decoupled from belief updates (§6.2), `refute`
  excises a clause and applies an acceptance threshold (§4.3–§5.1), source correlation discounting
  replaces Noisy-OR (§3.2), `truest` is defined as warrant rather than objective truth (§1.2), exact
  ties fail (§5.1), hardware health gates are specified (§2.3, §5.1, §5.3), and fusion is
  innovation-gated (§5.3).
- **R2-M1 (fault-status score handling): RESOLVED.** `FAULTED` sources are excluded (§2.3, §5.1);
  `SUSPECT` sources are discounted (§2.3, §5.1).
- **R2-M2 (near-tie tolerance): OPEN.** v1.2 specifies only an *exact* tie (§1.5 U-C4, §4.4, §5.1);
  see MINOR 1 below.
- **R2-M3 (defeater lifecycle): OPEN and elevated to MAJOR.** v1.2 still introduces the marker
  without defining its matching, invalidation, or expiry semantics (§4.3).

## BLOCKING

None.

## MAJOR

### 1. Source accuracy is not in general the likelihood ratio needed for a…

**Full title and location:** 1. (§3.2, §4.2) Source accuracy is not in general the likelihood ratio
needed for a candidate posterior

**Problem:** The formula calls `W` a Bayesian posterior warrant, sets the prior to `P0 = 0.5`, and
adds `logit(w_S)` for each report, while `w_S` is the source-level Beta mean or SGD prediction
(§3.2, lines 426–469). A source's aggregate rate of correct assertions is not generally the
likelihood ratio for this particular report. The substitution is valid only under an explicit
reporting model—e.g. binary exhaustive claims, calibrated sensitivity and specificity (with the
required symmetry for `logit(w_S)` to be the report's log-likelihood ratio), a suitable candidate
prior, and conditional independence after grouping. None of these assumptions is stated, and the
same source reliability is applied to different predicates and claim types. The fixed `P0 = 0.5` is
also an equal-odds hypothesis prior, not a generally uninformative prior for a keyed fact with an
unequal base rate.

The distinction matters operationally: a score can rank sources usefully without being a calibrated
probability that a candidate is true. The current `min_confidence`/`theta_min` and
`with uncertainty` surfaces invite probability interpretation. Bayesian diagnostic practice
distinguishes the test's likelihood ratio from the prior odds and resulting posterior odds; e.g.
[Understanding the properties of diagnostic tests—Part 2: Likelihood ratios][source-1] states
post-test odds = pre-test odds × likelihood ratio. For testimony, the evidence is the particular
utterance in its context, with other background evidence relevant to how much it supports the
proposition; see the [Stanford Encyclopedia of Philosophy overview of testimony][source-2].

**Evidence:** §3.2 lines 426–469 defines `w_S` as source reliability/model output and directly adds
its logit to a “Bayesian posterior”; §4.2 exposes `min_confidence`, while §2.3 stores source-wide
α/β. The document does not supply predicate-specific base rates or a false-positive/false-negative
model for reports.

**Fix:** Keep the first implementation bounded: call the result a *warrant score* and avoid
describing it as a calibrated posterior/“confidence,” or state and enforce a minimal binary
observation model with predicate/context-specific prior odds and report likelihoods
(`P(report | claim)` and `P(report | not claim)`). Only use `logit(w_S)` as a likelihood term when
the calibration assumptions actually hold; state the independence assumptions under which multiple
terms can be added.

### 2. Verification updates can be self-certified and are not tied to an auditable…

**Full title and location:** 2. (§4.3, §6.1–§6.3) Verification updates can be self-certified and are
not tied to an auditable observation

**Problem:** The prose says directives run “when ground truth is verified” and acknowledges fallible
verifiers, but the actual interface accepts any ground `confirm`/`refute` plus a caller-chosen
`weight` that defaults to 1.0 (§4.3, lines 664–696). There is no verifier identity, evidence/event
reference, outcome type, or check that the caller has access to independent evidence. A source can
therefore cause its own assertion to be confirmed by invoking the directive (directly or through a
plan), then receive the positive α update. The statement that a missed detection “must pass a weight
below 1” is a convention, not an enforced or calibrated property. The settle queue preserves the
update, but does not establish that its evidence was independent or valid.

This is especially unsafe for negative observations. A detector’s non-detection is evidence against
presence only to the extent the search had a chance to detect the target. A single scalar weight
cannot, by itself, encode distinct hit and miss likelihoods, coverage, or search conditions.
Empirical work on reasoning from absent observations finds inference strength depends on how
expected the evidence was
([Hsu et al., *When Absence of Evidence Is Evidence of Absence*][source-3]); the
philosophy-of-science discussion likewise emphasizes the conditions and auxiliary assumptions under
which non-detection bears on a hypothesis ([Philosophy of Science, 2025][source-4]).

**Evidence:** §4.3 lines 686–700 updates sources agreeing/contradicting a tuple and permits
`weight: Expr` with a default of 1; §6.1 provenance tokens record the *asserting* sources, not who
verified or which observation was used; §6.3 persists the queued update without an evidence payload.

**Fix:** Make a verification update consume an explicit evidence record: verifier/source identity,
assertion/event being checked, observed outcome, and (for negative results) coverage/detection
conditions or an outcome-specific calibrated likelihood. Do not let an arbitrary caller-selected
scalar stand in for that record. If calibrated verification is out of scope, describe
`confirm`/`refute` as privileged operator adjudications and enforce that boundary; do not claim
sensor verification or anti-self-certification.

### 3. Time and identity are attached to profiles, while verdicts need to attach to…

**Full title and location:** 3. (§2.3, §3.1, §4.3, §6.1, §6.3) Time and identity are attached to
profiles, while verdicts need to attach to assertion events

**Problem:** §3.1 defines decay from each clause’s `t_asserted`, but the only timestamp field
specified in the profile is `$last_seen`, defined as the time the *source last asserted or was
verified* (§2.3, lines 331–337). A source-level clock cannot age each claim independently: a fresh,
unrelated report can refresh a stale clause if `$last_seen` is used, and the design does not say
where the per-assertion timestamp is stored. Feedback has a related identity gap: the intention
token carries a predicate, key hash, and source masters (§6.1), while `confirm`/`refute` classify
each source by its *current* clause for the key (§4.3, lines 686–691). A delayed verdict can
consequently train a later assertion by the same source, or penalize a report that was not the one
actually checked. For moving or changing predicates such as `at` or `temperature`, a disagreement
outside epsilon is not necessarily a false assertion unless the observations refer to the same
time/window.

P18 explicitly stores peer names and host percept drivers on `+d.source`, while P19’s examples
identify sources by strings such as `"sensor:gps"`; P19 does not specify an authenticated principal
or source-incarnation/version. A recycled name after device replacement can inherit the old profile
unless reset perfectly tracks identity changes.

**Evidence:** §3.1 lines 385–389 requires `t_asserted`; §2.3 has only source-level `$last_seen`;
§4.3 defines agreement against “its current clause for the key”; §6.1 token omits an assertion
ID/time; §6.3 queues source-level updates. P18 §2.3 (P18 commit `f033eda8`) identifies peer agent
names and host percept drivers as provenance values, not authenticated incarnation identities.

**Fix:** Stamp every assertion with an immutable event/claim ID and observation time (and a source
principal/incarnation). Carry that exact ID and the checked observation ID through the provenance
token and settle queue, and apply verdicts only to the assertions they adjudicate. Keep `$last_seen`
as profile activity metadata. Define a time/window dimension for changing predicates, and scope
learned reliability to the relevant predicate/context or explicitly document the intended
cross-domain transfer.

### 4. (§3.2, §7.2) Correlation groups do not establish the stated Sybil resistance

**Problem:** The design says echo chambers and Sybil swarms are capped at one voice per group (§3.2,
lines 455–464), but an ungrouped source is its own group, `rho_G` is a scalar stored on the group
head, and the spec does not say who authenticates group membership or provenance. A Sybil can
therefore present new source names as separate ungrouped sources, each receiving the full `n_eff=1`
contribution. The formula only caps copies that have already been placed into the same group; it
does not itself identify common control or common ancestry. The foundational Sybil result is
precisely that one faulty principal can present multiple identities and defeat redundancy absent an
identity/trust mechanism ([Douceur, *The Sybil Attack*, IPTPS 2002][source-5]).

Separately, `n_eff = n / (1 + (n-1)rho)` is the familiar effective sample-size/design-effect
approximation for exchangeable equicorrelated observations; applying it to the average of
heterogeneous log-odds as an exact Bayesian likelihood is not justified by merely storing a
group-level pairwise `rho`. This may be a useful discounting heuristic, but the spec currently calls
the result a Bayesian posterior.

**Evidence:** §3.2 lines 441–464 defines the scalar group discount, makes ungrouped identities
singleton groups, and asserts Sybil swarms are capped; §7.2 labels the example “Byzantine Fault
Tolerance.” No authenticated group-binding or Sybil admission rule is defined in P19.1–P19.7.

**Fix:** For the minimal implementation, describe `rho`/`n_eff` as an explicit heuristic and remove
the claim of Sybil resistance. If the claim is required, specify who assigns an authenticated
trust-domain/provenance group, how relayed ancestry is propagated, and how identity creation is
constrained. State the exchangeability/equal-correlation approximation; otherwise use an explicit
dependence model or avoid “Bayesian posterior.”

### 5. (§4.3, §5.1) The refutation defeater has no scope or reopening rule

**Problem:** v1.2 now says `refute` excises the currently matching clause and “marks an epistemic
defeater for the key tuple” (§4.3, lines 671–674). It never defines how the marker affects
subsequent assertion ingestion or arbitration, how long it remains, what exact tuple/time interval
it covers, or what evidence clears it. If it suppresses every later matching assertion, a temporary
fact (e.g. an obstacle/location) can remain false forever; if it is merely decorative or cleared at
turn end, the next identical assertion can win again immediately. This leaves the core
defeasible-revision behavior underdetermined after the existing clause has been excised.

**Evidence:** §4.3 names a key-tuple defeater but contains no lifecycle rule; §5.1 filters
candidates by source health and ingestion-gate verdict only (lines 735–748), with no defeater check.
This is the unresolved R2-M3 concern from the previous review.

**Fix:** Define a small event-scoped tombstone policy: exact claim/event and observation window it
defeats; whether incoming assertions are rejected, retained-but-defeated, or allowed as new
evidence; and the one explicit reopening rule (for example, a later observation with a distinct
event ID from an admissible verifier, or an expiry/epoch boundary). Avoid permanent suppression
keyed only by predicate arguments.

## MINOR

### 1. (§1.5 U-C4, §4.4, §5.1) Near-equal scores are still treated as a unique winner

**Problem:** v1.2 requires a *unique* maximum but only treats exactly equal computed values as a
tie. Floating-point roundoff can turn epistemically indistinguishable candidates into a unique
winner; clause order may then affect which score accumulates rounding error.

**Evidence:** U-C4 says “exact tie”; §4.4 lines 714–716 and §5.1 lines 750–754 likewise test a
unique maximum without a score tolerance. This was R2-M2 and remains open.

**Fix:** Define a score comparison tolerance in the score’s numeric domain and treat candidates
within it as tied. Keep the tolerance configurable only if the engine needs different scales;
otherwise state one safe default and test boundary cases.

## Verdict

**VERDICT: NOT CONVERGED** — five major semantics remain underspecified or are overstated as
Bayesian/Sybil-resistant guarantees. The minimum useful repair is to make the scores honest about
their calibration status, make verification updates auditable and event-bound, and give defeaters an
explicit temporal lifecycle.

VERDICT: NOT CONVERGED

[source-1]: https://pmc.ncbi.nlm.nih.gov/articles/PMC5950618/
[source-2]: https://plato.stanford.edu/archives/sum2020/entries/testimony-episprob/
[source-3]: https://doi.org/10.1111/cogs.12356
[source-4]: https://doi.org/10.1017/psa.2025.10125
[source-5]: https://www.microsoft.com/en-us/research/publication/the-sybil-attack/
