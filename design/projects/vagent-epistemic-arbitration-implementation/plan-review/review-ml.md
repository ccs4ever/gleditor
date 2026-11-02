# Remaining-work plan review, round 1: ML expert and ML gatekeeper (persona ml)

Scope: `remaining-plan.md` (draft 1) and `traceability.md`, against P19 v1.5 sections 2.1, 3.1 to
3.4, 5.1 to 5.3, 6.3 and 8.1, code at HEAD `3ae8d384`. Scratch work is in `/tmp/p19-ml/plan_gate.py`
(pooling, `lcb`, saturation, health knife edge, SGD noise, streak waiting times). Round 2 of the
I3-2 ML review (`i3-2-review/review-ml-r2.md`) is the earlier record.

## Earlier findings

I3-2a's content list absorbs round-2 M1, M2 and M3. Round-2 minor 4 (the "64 events" recovery count
sits on a knife edge) is not carried into the I3-2a test list; finding M5 below gives a computed
example of why it matters beyond that slice.

## What is sound

- **Order of the read path.** Scoring (I4-2), pooling (I4-3), groups (I4-4) and syntax (I4-5) before
  lowering (I4-6) is the right dependency chain. Dependence groups must precede `truest`: with the
  default single group and `rho = 1`, `n_eff = 1` for any group size, so the pooled score is the
  mean logit and **independent of how many sources support a candidate**. I computed this (script
  section 1): 1, 2, 5 and 20 sources of weight 0.7 in one group all give W = 0.700000; five
  independent groups give 0.985748. Without I4-4, supporter count never matters. The plan says the
  reviewers may move I4-4 later; from the ML side it must stay before I4-6.
- **Numerics in emitted IR (O2 a).** One place for the numerics, one reference. `Exp`, `Ln` and
  `Sqrt` are enough for everything in 3.1 to 3.4 (`vortex_vm.cpp:939-941`).
- **Recompute, never increment (I5-1).** Beta state as epoch prior plus ledger sums makes a
  correction exact. Fixing the summation order by first-accept ordinal is the right rule.
- **No row in sections 3, 5.2, 5.3 or 6.3 is deferred without spec authority.** The Deferred rows
  (3.1-21, 3.3-11, 3.4-7, 5.2-9, 5.3-15) each cite a spec sentence, and I checked those sentences.
  Every open ML row has exactly one closing slice. Two schedule notes are in the minor list.
- **Option A (option 2) is right from the ML side.** Applying a label on an observation-ID match
  alone, without the token value match, is the wrong-label hazard 6.3 exists to prevent.
- **Translator rule for floats.** Scalar cells hold canonical bits (R6), so resume and upgrade copy
  learned state bit for bit and never recompute it. This is why the cross-host problem is confined
  to new computation (M5).

## BLOCKING

None.

## MAJOR

### M1. Option B/3 leaves I4 unverifiable on priors and accepts a gate "provisionally"

Problem. With priors only, every source has one weight, so I4-2 to I4-7 can show eligibility, decay,
ties and refusals but not one ML property of arbitration. The plan then says I4's first half "is
accepted provisionally on its ML gate" and closes it at I4-11. The README rule has no provisional
acceptance: a slice is accepted with no BLOCKING and no MAJOR open. Separately, I4-2 and I4-6 need
distinct per-source weights, so a fixture writes alpha and beta directly. I5-1 then defines alpha
and beta as a function of the ledger (6.3: prior plus the ledger's `q*y`, `q*(1-y)` sums), and the
fixture state violates that. The plan's own objection to option 2, a second write path to learned
state, is only avoided in name.

Evidence. Script section 1 (above). At a symmetric prior (`alpha = beta = 1`, the spec default)
every weight is 0.5, decay toward 0.5 does nothing, and every query ties: `truest` can only abstain.
At an asymmetric prior the single discriminator is age, so a passing `truest` test shows "the
freshest observation wins", which a reader will take for reputation. The precedent for the better
option is already in the plan: I3-2c1 is "IR only; no host API yet (the turn is driven by a test
harness until 2c2)".

Consequence. Five ML-gated slices (I4-1, I4-2, I4-3, I4-4 and I4-6) are reviewed against a world
where the thing they exist for cannot move. The review at I4-11 would then be the first real look,
after seven slices are built on fixtures.

Required fix. Option 3': split I5-1.

- I5-1a, right after I4-2 (before I4-3): the ledger entry, the single IR routine `ledgerApply`
  (append or replace by named predecessor, recompute Beta from epoch prior plus ledger in
  first-accept order, bump the state version, refuse `EpistemicBudget` at the cap), driven by the
  test harness exactly as I3-2c1 drives the health turn. It is the only writer of alpha and beta;
  fixtures call it, so no alpha or beta is ever written directly. The target is an observation ID;
  the token match and authorization stay with I3-4a.
- I5-1b, after I3-4c: wire the processing turn to `ledgerApply`, turn `RecordedNotApplied` into
  `Applied`, add the corrections and conflict paths that need the journal.
- Move the ledger-entry shape from V-D into I4-D, since I4-1 freezes it (M2).
- Each ML gate from I4-2 on then states which claims it passes. Reputation separation is claimed
  from I4-6; the full observe, query, verify, label, re-query loop from I4-11, as planned.

If the owner prefers option 3 unchanged, then the fixture must be a harness call into the same
routine, not cell writes, and every I4 ML report must list "reserved to I4-11" claims instead of
passing provisionally.

### M2. The "freeze the whole calibration record" claim is wrong in content and in scope

Problem. I4-1 says it freezes the whole record "so resume and the translator copy the shape once and
I5 does not change it". Four things make that false as written.

1. **The key omits the source.** I4-1 keys a record by "(predicate signature, domain, epoch) on the
   predicate's `d.epistemic` rank". Spec 2.1 (`design/vagent-...md`, "Each source profile heads its
   domain-calibration records ... lookup uses the full key (source identity/incarnation, predicate
   signature, domain/schema ID, calibration epoch, model epoch and feature schema)") puts Beta
   counts per source under the profile. A record without the source is shared by every source, so
   reputation could not separate sources at all. The spec also has a second record kind: the
   policy/domain record that owns the dependence-group registry (3.1). The plan merges the two.
   Matrix rows 2.1-7 (predicate rank) and 2.1-15/37 (source profile rank) are listed separately and
   are closed by one slice.
1. **The policy keys land in at least three bumps, not two.** The risk table says "`vagent-policy-2`
   in I4-1, `-3` in I5-3". But SGD hyperparameters (`eta0`, `lambda_reg`, clip, floor: matrix 3.3-8)
   are "validated" in I5-4, and the feedback horizon, warm-up length and abstention parameters
   (3.3-4, 3.3-12) in I5-5. Each is a key in the closed table (`epistemic_policy.cpp:31-62`) and a
   change to every policy hash.
1. **No slice adds the declaration of a functional, exhaustive alternative set.** I5-1 applies
   "competing values negative only for a declared exhaustive functional set" (6.3). Nothing in I4-1,
   I3-4a or I5-3 declares one. Without a declaration, `confirm` can only label the named
   observations, which is a spec-permitted reading but not what I5-1 says it does.
1. **SGD internals cannot be frozen before I5-D.** Weights, bias and normalizer are vectors whose
   length is the feature count and whose element kinds are decided in I5-D. "Absent-valued" list
   heads are fine; their element shape is not known.

Evidence. Spec 2.1 and 3.1 text above; plan I4-1, I5-1, section 6 risk row on policy bumps;
`epistemic_policy.cpp:31-62` (no `risk`, `c`, `eta0`, `horizon`, or alternative-set key today).

Consequence. A translator written at T1/T2 over a shape that changes in I5 repeats the cost the plan
wants to avoid, and a per-predicate Beta record is a silent loss of the whole reputation model.

Required fix. I4-D freezes the envelope, not the contents. (a) Two record kinds with their keys and
parent ranks written out: a per-source calibration record (full key from 2.1, parent: source
profile) and a per-domain policy record (parent: predicate descriptor; owns the group registry). (b)
The ledger entry's field list (from 6.3), because I5-1a needs it. (c) One closed policy key table
covering every key of 3.1 to 3.4 and 6.3 (including the alternative-set declaration, warm-up,
horizon, abstention), added once with the `-2` bump; the checker refuses `online_sgd` until I5-4.
(d) A shape version in the state-version cell, with the repository's existing rule that an absent
field reads as its initial value, so SGD internals added in I5 are additive and the translator needs
no change for them. State this in place of "frozen".

### M3. The ledger bound makes a source stop learning, and the cap is decided too late

Problem. 6.3 forbids pruning the active ledger silently and requires a label that does not fit to be
refused as `EpistemicBudget`. Beta state is recomputed from the whole ledger, so a source that
reaches the ledger cap can never be corrected or trained again until an authorized epoch reset. The
plan settles the cell budget at O7 "until I4-11 measures", after I4-1 has frozen the shape that the
cap belongs to, and I5-1 has no stated cap or per-source share.

Evidence. Default `kDefaultEpistemicCells = 24576` (`epistemic_limits.hpp:35`) and "about 80 cells a
record" (matrix 8.P1-6). A ledger entry carries about a dozen fields (6.3), each a `d.vars` cell
plus an owned value, and an immutable verification-version record of similar size. At 40 to 60 cells
per label (my estimate, not measured), the whole ceiling holds on the order of 400 to 600 labels
before any observation, profile or group is stored, shared by up to 128 sources
(`kDefaultEpistemicMaxSources`). That is a handful of labels per source.

Consequence. The learning claim of I5 rests on a number nobody has set. A handful of labels moves a
Beta(1,1) mean by far more than it supports, and then learning stops.

Required fix. I4-D states the per-(source, domain, epoch) label cap, its default and its cell price
(measured on the IR, not estimated), and what an operator does at the cap (epoch reset, or an
owner-approved summarising rule that the spec would need to authorise). Move the O7 decision to
I4-D, not I4-11; keep I4-11 for re-measurement. It is an owner decision if the answer is to raise
the ceiling or to summarise old labels.

### M4. The gate-rejection streak can latch FAULTED on noise; "same shape" conflicts with it

Problem. I4-8 says the gate-rejection estimator has "the same shape as the health fraction", and
also keeps "the streak thresholds (three SUSPECT, five FAULTED)". These are two triggers for one
state, with no statement of which governs. The streak is the one that is unsafe: FAULTED latches and
only an operator `reset` unlatches it (5.1).

Evidence. Expected events to the first run of r consecutive rejections at an iid false-alarm rate p
(exact renewal formula, script last section):

| p (gate false alarm) | run | events | at 1 Hz | at 10 Hz | at 100 Hz |
| -------------------- | --- | ------ | ------- | -------- | --------- |
| 0.05 (95% gate)      | 3   | 8.4e3  | 2.3 h   | 14 min   | 84 s      |
| 0.05                 | 5   | 3.4e6  | 936 h   | 94 h     | 9.4 h     |
| 0.01 (99% gate)      | 3   | 1.0e6  | 281 h   | 28 h     | 2.8 h     |
| 0.01                 | 5   | 1.0e10 | 2.8e6 h | 2.8e5 h  | 2.8e4 h   |

5.2 itself says the nominal rate "does not hold after arbitrary adaptive trust inflation or model
mismatch", so p is a floor. A healthy sensor at 100 Hz behind a 95% gate is latched FAULTED about
every 9 hours, SUSPECT every 84 seconds. Accepted readings clear the streak at once, so the SUSPECT
state also flaps. This is the traffic dependence that led I3-2 to an event-indexed fraction.

Consequence. Taking the streak literally contradicts 5.1's own "declared sample-rate/window
interpretation" and produces a health system that latches healthy devices at the rates the plan
tests (0.1, 1, 100 Hz in I3-2a).

Required fix. In I4-D, decide and write one rule. My recommendation: share the update routine and
the evidence-floor rule with the health fraction (one IR function, `b`, `min_events`), but keep
separate state (`gate_rejected`, `gate_events`, with `N` counting only tested events, never
`UNTESTED` ones), separate settings (half-life, threshold, floor) and separate nominal rates (a
content-refusal rate is about 0; a gate rejection rate is the gate's own false-alarm rate). Use the
fraction for SUSPECT and for FAULTED, with FAULTED also requiring a minimum number of events. Keep
the streak only if the owner wants it as the spec's literal rule, and then require a time or event
window so it cannot latch at 100 Hz. This is an amendment to 5.1's "three consecutive" text and
belongs in the E0 bundle (O5), together with the I3-2 change to the refusal fraction. Test: the
false-latch rate over 10^7 simulated healthy events at p = 0.05 and 0.01, and a common- mode drift
where rejections arrive in runs. I4-8 must also show that a rejection trains nothing and blames no
sensor (5.2-15).

### M5. The cross-host contract is incomplete: libm decisions flip, with no seam to test it

Problem. The risk table says results are "compared to an independent reference within stated
relative error; tie tolerance far above libm error; replay deterministic on one host and within the
stated error across hosts". That is the right outline. It omits that several designed decisions are
hard thresholds on libm-dependent values, and it has no test that shows the claim.

Evidence.

- The VM calls the host libm directly: `std::exp`, `std::log` (`vortex_vm.cpp:939-941`). The
  Makefile has no `-ffast-math`, `-Ofast`, `-march=native` or `-ffp-contract` setting (grep), so the
  compiler is not the issue; libm and the target's FMA contraction of any composite C++ in the VM
  are.
- A threshold decision already in the plan flips with a one-ulp `exp` difference. The I3-2a recovery
  from saturation (`H = 32`, threshold 0.25 = 2^-2, so `b^64 = 0.25` exactly in real arithmetic):
  the recovery event is 64 with the host's `exp`, 64 with `exp` minus one ulp, and 65 with `exp`
  plus one ulp (script section 2). The same step is SUSPECT on one host and NOMINAL on another.
- SGD itself is safe: with the L2 term the iteration is contractive. With 30% of `exp` calls
  perturbed by +1 ulp and 30% by -1 ulp, 20 seeds, d = 8, L = 100, 1,000 and 20,000 labels, the
  largest weight difference is 1.7e-16, 1.1e-16 and 2.2e-16. So the stated tolerance for the learned
  weights can be tight (say 1e-12).
- The decisions that can flip are the ones with a boundary: the tie group (`score_tolerance`), the
  `min_confidence` cut, the health thresholds, the innovation gate (`d^2 <= g_k`) and the
  cluster-adjacency test. The probability that a given decision lands within libm error of its
  boundary is tiny, but a test that is built on a boundary hits it every time.
- A plain Python reference shares the host's libm for `exp` and `log`, so it cannot detect a libm
  disagreement and is not "independent" in the way the contract needs (M6).

Consequence. A checkpoint that moves hosts can answer a query differently without any bug, and a
test built on an exact boundary will fail on another CI image.

Required fix. I4-D writes the contract with these parts: (1) values are compared to a high-
precision reference (M6) within a stated relative error derived from the operation count; (2)
learned state is stored as bits and never recomputed on resume or upgrade, so it is identical across
hosts until a correction forces a replay; (3) on one host, replay from the ledger with no correction
is **bit-equal** to the incremental result, not "within stated error" (I5-4 as worded would pass a
real order bug); (4) every golden vector that feeds a decision carries a `margin` (distance to the
boundary in relative terms) and the test fails if a margin is below 1e-12, except for ties built
from bit-identical inputs, which are exact on every host; (5) tests never pin a count that sits on a
boundary (the health "64 events" included; the I3-2a list should say "about 64"); (6) the user guide
states that a decision within about 1e-12 of its boundary may differ between hosts. If the owner
wants bit-exact answers across hosts, the VM needs its own `Exp` and `Ln`, which changes accepted I1
work; the plan should record that as a rejected alternative.

### M6. The reference-vector approach is useful but not enough for the I4 gate as written

Problem. I4-2 says "an independent Python reference in `tools/` generates the golden vectors". Three
weaknesses. (1) The author of the IR is the author of the reference, in the same slice, and the ML
review sees both at once; independence here is a name. (2) CPython `math.exp`/`math.log` call the
host libm, as the VM does, so shared libm error is invisible (M5). (3) Golden vectors chosen by the
implementer cover the designed cases; the sums over sources (compensated or rescaled), the clamp
edges and the ordering are exactly where mistakes hide.

Evidence. `vortex_vm.cpp:939-941`; plan I4-2, I4-3, I4-8, I4-9, I5-1, I5-4 all reuse
`tools/vagent-epistemic-vectors.py`.

Consequence. The same file gates six ML slices; a defect in it passes all of them.

Required fix. (a) Write the reference in I4-D, before any IR exists, from the spec formulas, and
have ML and NUM review it as a design artefact. (b) Compute it with `mpmath` or `fractions` at 50
digits (a dev tool, not a build dependency), and emit the vectors as a committed data file with the
generator's seed, so one command reproduces them. (c) Add three kinds of checks that need no
reference: closed forms (the spec's own example, prior 1/2 with w = 0.8: one positive report 0.8, an
independent opposite report back to 0.5; `rho = 0` gives `n_eff = |G|`, `rho = 1` gives 1; decay at
one half-life is exactly halfway to 0.5), metamorphic properties (permutation of sources leaves W
unchanged, `logit(w)` is odd about 0.5, a neutral source changes nothing, scaling all `q` by zero is
a no-op), and a seeded random differential run of several thousand cases. (d) For SGD add a
hand-computed one-step case and a "loss decreases on a separable stream" property. This is also the
place for the margin rule in M5.

### M7. `risk: "lcb"` at an uninformative prior makes unknown sources vote against their own claim

Problem. 3.4 takes `max(0, mu - c*sqrt(v))` as the weight and 3.1 pools its logit, with no floor at
0.5. A source whose lower bound is below 0.5 contributes a **negative** logit. A fresh source at
`alpha = beta = 1` has `mu = 0.5`, `v = 1/12`, so any `c > 0` puts it below 0.5. Unknown becomes
anti-evidence, and each additional independent fresh supporter lowers the candidate's score.

Evidence (script, last-but-one section):

| c   | lcb              | logit   | W, 1 fresh source | W, 3 independent fresh sources |
| --- | ---------------- | ------- | ----------------- | ------------------------------ |
| 0.5 | 0.3557           | -0.594  | 0.356             | 0.144                          |
| 1.0 | 0.2113           | -1.317  | 0.211             | 0.0189                         |
| 1.7 | 0.0093           | -4.674  | 0.0093            | 8.1e-7                         |
| 2.0 | 0 (clamped 1e-6) | -13.816 | about 1e-6        | about 1e-18                    |

Consequence. With `lcb` and `c >= 1.7`, a fresh source is a veto; with any `c > 0`, three
independent corroborating fresh sources rank below one. `min_confidence` (0.5 is natural) then
rejects every candidate supported only by fresh sources. The behaviour is spec-compliant and unsound
for the purpose the spec gives `lcb` ("conservative").

Required fix. This is the owner's call (new question O9): (a) floor the lower bound at 0.5 before
decay, so ignorance is neutral and a source can only be down-weighted to neutral by uncertainty; or
(b) keep the text and require tests and user-guide wording that state that `lcb` with a symmetric
prior inverts the effect of support. Either way I4-2 needs a test for it, and the choice must be
settled in I4-D, since it changes the golden vectors.

## MINOR

1. **Gate placement.** Add a limited ML read ("named items only") to: I4-5 (the
   `UnsupportedRiskModel` rule for `lcb` with `online_sgd`, and the variance type kept distinct from
   a score); I4-7 (`ranked`'s non-transitive tie group and the stale-state rule); I4-10 (a
   self-authored fact scored as testimony is the self-confirming loop of 3.1, so check which source
   weight it earns); I3-2c1 (the reset epoch and `retired_by` that later expire labels); I3-4b (the
   replay floor against the feedback horizon of 3.3). None needs a full review; a short section in
   the existing reviewers' round suffices.
1. **Tie tolerance saturates.** `score_tolerance` is absolute in W-space (1e-9, 5.3). Since
   `W = sigma(s)`, candidates whose logit sums exceed about 20.7 tie regardless of their difference:
   15 independent groups of weight 0.8, 10 of 0.9, or 5 of 0.99 are enough (script). `truest` then
   abstains between a candidate with 10 independent supporters and one with 30. It is what the spec
   says; test it, put it in the user guide, and ask the owner (part of O9) whether the tolerance
   should be in logit space.
1. **A weaker corroborating source lowers the score** in one group with `rho = 1`: sources 0.9 and
   0.55 give W = 0.768, below the 0.9 of the strong source alone (script). It follows from
   `n_eff = 1` and averaging. Put the property in I4-3's tests and the I4-11 examples so nobody
   reads the pool as accumulating evidence inside a group.
1. **Priors-only demos.** At an asymmetric prior, `truest` selects by age. I4-6 tests and the I4-11
   worked examples must say so, and the user guide must not show "most reliable source wins" before
   labels exist.
1. **O6 and 5.2-6.** Dropping the profile-variance path for v1 is a scope change to a spec sentence
   (5.2: "otherwise use an explicitly supplied profile variance ..."). Record it in the E0 amendment
   bundle, and mark 5.2-6 Deferred there, so the row is not closed without authority.
1. **Spec orderings to pin in I5-D.** 3.3 says "receive-sequence order" and 6.3 says "first-accept
   order"; state that they are one order, and what a revision does to it (6.3 gives it the
   predecessor's place).
1. **Abstention rule.** 3.3 says "abstain when validation does not support deployment" and gives no
   criterion; the matrix closes 3.3-12 in I5-5. I5-D must define it (held-out or prequential loss
   against the Beta baseline, with a minimum sample), since it is the one sentence of 3.3 that needs
   an invented rule. It is a design decision within the spec; record it.
1. **`decay` semantics.** The policy has one `decay` duration (`epistemic_policy.cpp:40`); the spec
   writes `lambda_S` per source, and 3.2 says static facts "may set lambda = 0". Say what an absent
   `decay` means (my reading: lambda = 0) and whether a per-source value exists (matrix 3.2-2
   "semantics of absence undefined").
1. **F2 Standing check.** Row 8.P6-3 ("only `beta_reputation` is admissible, so the fallback is the
   only mode") stops being true at I5-4. Re-verify it at F2 and when I5-4 lands.
1. **Test for the stable sum.** I4-2 lists "compensated or rescaled sums". Add a case where naive
   summation of 128 logits of mixed sign loses a stated number of bits and the IR does not.

## Questions for the owner

1. **O9 (new).** `lcb` at a symmetric prior (M7): floor the lower bound at 0.5, or keep the text and
   document the inversion?
1. **O10 (new, may merge with O9).** Should `score_tolerance` stay in W-space, given the saturation
   above, or be defined in logit space?
1. **O7, earlier.** The per-source label cap and what happens at it (M3) before the calibration
   shape is frozen; raising the ceiling or summarising old labels both need the owner.
1. **Cross-host contract.** Is "bit-equal on one host, within a stated tolerance across hosts" the
   meaning of "deterministic" in 3.3 and 6.3? The spec never says cross-host; I read it as
   same-host. The lead can decide this inside the approved spec, but the owner should see the
   sentence in the user guide.

## Answers to ordering questions A to E and owner decisions O1 to O8

| Item | Answer                                                                           | Reason                                                                                                                                                                                                      |
| ---- | -------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| A    | Agree (option 2)                                                                 | A label needs the token's value match; observation IDs alone are the hazard 6.3 names. With I5-1a (M1) the apply routine is earlier, only the authenticated caller waits.                                   |
| B    | Disagree: option 3'                                                              | Split I5-1: apply routine and ledger after I4-2 with a harness caller (the I3-2c1 precedent), wiring after I3-4c. One writer, no fixture writes, no provisional acceptance (M1).                            |
| C    | Disagree in part                                                                 | Keep I4-1 for the calibration record and I4-4 before I4-6 (count only matters with groups). Do not "freeze the whole shape": freeze the envelope, keys and ledger entry, add SGD internals additively (M2). |
| D    | Agree (option 3)                                                                 | Bits are copied, not recomputed. Add the calibration and ledger shape to T2 with a same-host bit-equality property test.                                                                                    |
| E    | No ML objection                                                                  | Option 2 is outside my discipline. The T2 claim should name learned state (Beta, weights, normalizer, ledger) as surviving bit-for-bit.                                                                     |
| O1   | Not ML                                                                           | Architect and owner.                                                                                                                                                                                        |
| O2   | Agree (a)                                                                        | One place for the numerics and one reference; actors never load `vortex_stdlib`.                                                                                                                            |
| O3   | No ML objection                                                                  | Tokens must carry the state version and observation versions; (b) satisfies that.                                                                                                                           |
| O4   | Not ML                                                                           | Security and distributed systems.                                                                                                                                                                           |
| O5   | Agree, add two items                                                             | Include the gate-rejection amendment (M4) and the O6 outcome on 5.2-6 (minor 5) in the bundle.                                                                                                              |
| O6   | Agree with `t_star` and the reference grant; owner must decide the variance drop | A scope change to a spec sentence needs the owner (minor 5). The `t_star` default belongs to SENS.                                                                                                          |
| O7   | Disagree on timing                                                               | Decide the label cap before I4-1 freezes the shape (M3), re-measure at I4-11. Owner, because raising the ceiling or summarising labels changes the spec's bounds.                                           |
| O8   | Not ML                                                                           |                                                                                                                                                                                                             |

## Answers to the plan's section 9 questions for the ML expert

- **Is I5-1 before I4-8 the right place for Beta labels?** The position is better than I4 or I3-4
  but still late. Split it (M1): the ledger and apply routine right after I4-2, the wiring after
  I3-4c. I4-8 does not depend on I5-1 technically.
- **Is the reference-vector approach enough for the I4 gate?** No, not as worded (M6). It is enough
  once the reference is written before the IR, computed at high precision, accompanied by closed
  forms and metamorphic properties, and reviewed in I4-D.
- **Should the health fraction and the gate-rejection estimator share one shape?** Share the
  routine, the floor rule and the event-indexed form; do not share state, thresholds or nominal
  rates (M4). The streak alone cannot be kept at the thresholds in 5.1.
- **Is the cross-host tolerance contract in the risk table the right one?** Right outline, five
  missing parts (M5): bits are authoritative, same-host replay is bit-equal, decision margins in the
  vectors, no boundary-pinned counts, a documented sentence.

Tally: 0 BLOCKING, 7 MAJOR, 9 MINOR.

Verdict: FAIL
