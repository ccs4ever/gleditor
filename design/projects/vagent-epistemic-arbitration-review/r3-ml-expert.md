# Round 3 Review: ML Expert

Target: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2\
Scope: calibrated likelihood versus competence; streaming SGD and delayed labels; concept drift;
correlated sources; logistic-bandit confidence; optional asynchronous ML-skill calibration.

## Findings

### MAJOR — Competence is used as a likelihood ratio without a report model

**Location:** §3.2, lines 426–469; §3.3, lines 483–486; P19.2, lines 1059–1065.

**Problem:** The combiner inserts `w_S = μ_S` (a Beta mean) or `ŷ_S` (the logistic model output) as
`logit(w_S)` evidence for a candidate. These quantities are described as source
reliability/competence, i.e. probability the source is correct. Posterior odds require the
likelihood ratio of the *observed report* under the candidate and its alternative,
`P(report | H) / P(report | ¬H)`. Competence alone is not that likelihood ratio except under
restrictive assumptions such as a binary assertion with symmetric errors and a calibrated
correctness probability. The design applies one scalar to arbitrary discrete predicates and then
treats source outputs as independent after an ad-hoc group discount. Pairwise error correlation
alone does not define a joint report likelihood, so `n_eff * mean(logit(w))` is not generally a
Bayesian posterior update.

The optional skill example compounds the ambiguity: `evaluate_defect(...) -> Float` has no declared
meaning (logit, calibrated correctness probability, or arbitrary score), yet its result enters a
pipeline whose outputs are called warrant/probability. A neural score or embedding must not be
silently treated as a probability.

**Evidence:** The design calls `μ_S` / `ŷ_S` the source's “warrant” and adds each source logit
directly to posterior log-odds (437–445). Standard odds updating multiplies prior odds by a report
likelihood ratio; positive-test likelihood ratios depend on sensitivity and false-positive rate, not
accuracy alone ([NCBI, *The Use of Diagnostic Tests: A Probabilistic Approach*][source-1]). For
neural models, calibration means predicted probabilities match empirical correctness frequencies;
raw confidence scores need calibration data ([Guo et al., ICML 2017][source-2]).

**Bounded fix:** Either call this a heuristic ranking score and stop labeling `W` a Bayesian
posterior, or define a response model per predicate/source (sensitivity/false-positive likelihoods,
with context where needed) and update with the likelihood ratio of each observed
assertion/refutation. State the binary/symmetric-error assumptions if retaining `logit(competence)`.
Define the native skill output contract as raw score/logit or embedding versus calibrated
probability; calibrate against explicit verified labels, version the calibrator with the model, and
permit abstention/uncertainty. For correlated sources, use an explicit shared-latent/common-cause or
joint likelihood model, or present the group adjustment as a conservative heuristic rather than
exact Bayesian inference.

### MAJOR — The claimed Robbins–Monro convergence condition is false for the specified schedule

**Location:** Summary lines 66–69; §3.3, lines 495–500; verification matrix line 1166.

**Problem:** With `η_t = η_0 / sqrt(N_S + 1)`, `Σ η_t² = η_0² Σ 1/(N_S+1)` diverges (harmonic
series). The text says the schedule guarantees stochastic convergence, but it does not satisfy the
square-summability condition it previously relied on. Further, convergence claims need assumptions
about bounded/noisy gradients, a fixed objective and feature distribution, and the regularized loss.
Those assumptions do not hold automatically for delayed, selectively verified labels or drifting
sensor distributions.

**Evidence:** Lines 495–500 explicitly make the guarantee; line 1166 proposes a convergence
assertion “under temperature noise” without specifying a sampling/convexity/stationarity regime. The
classical stochastic-approximation conditions include a divergent step-size sum and square-summable
squared steps ([Robbins & Monro, 1951 record][source-3]); `1/√t` fails the latter.

**Bounded fix:** Remove the claimed Robbins–Monro guarantee or choose a schedule appropriate to the
stated objective and assumptions (e.g. `t^-a`, `1/2 < a ≤ 1` under the relevant
stochastic-approximation conditions). If stationarity is not assumed, specify a tracking objective
and use a deliberate forgetting/window/adaptive-step rule; do not claim convergence to a fixed
minimizer.

### MAJOR — Welford normalization is a moving coordinate system, and delayed labels…

**Full title and location:** MAJOR — Welford normalization is a moving coordinate system, and
delayed labels lack an update rule

**Location:** §3.3, lines 483–490, 495–510; §6.3 (settle queue); P19.3, lines 1074–1079.

**Problem:** The design snapshots raw context at ingestion but updates a model on verdict arrival,
potentially much later. It does not say whether the old example is standardized using the
ingestion-time or current Welford statistics. Those statistics change the coordinate system of every
feature; changing means/scales without transforming `w` and `b` changes predictions even when model
parameters are otherwise untouched. Applying stale gradients to current weights also makes update
order/delay affect the result. The settle queue establishes staging, not a delayed-feedback learning
algorithm.

**Evidence:** Lines 483–486 promise the verdict trains on ingestion conditions, while lines 488–490
define mutable online means/variances and lines 506–510 apply an update without a timestamp,
version, or delay policy. Delayed online learning requires explicit handling of feedback arrival and
delay, not ordinary immediate SGD ([Joulani et al., AAAI 2016][source-4]).

**Bounded fix:** Freeze normalization after a documented warm-up, or transform weights/bias whenever
the scaler changes. Store scaler version and normalized feature snapshot with each example; define
whether delayed gradients are applied in arrival order, replayed in event-time order, or dropped
after a bounded delay, and cap/expire the pending-label queue. Test with out-of-order labels and
scaler updates.

### MAJOR — No logistic-model uncertainty supports the stated LCB or LinUCB scores

**Location:** §3.4, lines 515–548; P19.3 lines 1074–1080.

**Problem:** `risk: "lcb"` is defined using `μ_S − c σ_S`, but §2.3 supplies no posterior
uncertainty for online-SGD weights or `ŷ_S`. The contextual exploration formula is the standard
linear-reward LinUCB bonus using `A⁻¹`; for logistic/GLM predictions, a valid confidence bound
depends on the link, curvature/Fisher information, parameter constraints and a confidence
construction. Saying its linear predictor is a “logit-space bound” and applying sigmoid does not
establish coverage. Comparing it to Beta posterior means also mixes uncertainty models unless
calibration and scale are specified. This affects active verification prioritization, not the
deterministic query path.

**Evidence:** Lines 516–518 offer the LCB on either Beta `μ_S` or model output `ŷ_S`; lines 540–548
apply `A⁻¹` to the logistic predictor without a GLM confidence derivation. Primary work on logistic
contextual bandits derives link-aware bounds and explicit assumptions
([Jun et al., AISTATS 2021][source-5]); recent GLM confidence-sequence work likewise makes the
confidence construction explicit ([NeurIPS 2024][source-6]).

**Bounded fix:** Keep Beta-UCB/Thompson sampling only for Beta-Bernoulli reputation. For contextual
logistic verification, either defer LinUCB/LCB until a specified GLM posterior/confidence-sequence
method and assumptions are implemented, or label the score heuristic and remove any
confidence/regret interpretation. Do not compare raw linear predictors, calibrated probabilities,
and Beta means as if they share uncertainty semantics.

### MAJOR — L2 shrinkage is not concept-drift adaptation

**Location:** §3.3, lines 472–510; summary lines 66–69; §7.3, lines 985–1003.

**Problem:** v1.2 has no drift detector, forgetting factor, sliding window, or adaptive horizon for
the logistic model. The L2 term is applied only when a verified label triggers an update; it shrinks
parameters toward zero per update and does not preferentially discount stale evidence by age.
Between labels it does nothing. Thus the Round 2 assertion that coupled L2 “gracefully attenuates
stale SGD feature weights” did not resolve the earlier concept-drift finding. Also, a fixed
Robbins–Monro schedule tends to reduce adaptation precisely as a source’s behavior changes.

**Evidence:** §3.3’s complete model update is at lines 502–510 and contains only per-example
regularization; no state or operation tracks model age/drift. Distribution-shift adaptation requires
an explicit mechanism; adaptive conformal inference for arbitrary shifts, for example, tunes its
update step and states its guarantee as local regret rather than stationary convergence
([Gibbs & Candès, JMLR 2024][source-7]).

**Bounded fix:** Choose one bounded policy (e.g. a capped rolling/prequential window, or a
drift-triggered reset/step-size increase), specify which stored state resets and how delayed labels
are handled, and evaluate abrupt and gradual drift. If drift handling is deferred, remove the claim
that SGD covers concept drift and mark the model stationary-only. Online conformal methods can
provide prediction sets/abstention under defined shift assumptions; they do not turn model scores
into calibrated probabilities ([Bao et al., JMLR 2025][source-8]).

## Previous-round status

- **Round 2 M2.1 (summary schedule typo): RESOLVED.** Current summary uses `N_S + 1` (line 68).
- **Round 2 M2.2 (LinUCB covariance storage and O(d) price): OPEN.** P19.3 still prices the routine
  at `O(d)` (line 1079), while §3.4 requires a full `A⁻¹` and Sherman–Morrison update (lines
  544–548); §2.3 has no `$covariance`/matrix field (lines 328–343). Full-matrix operations are
  quadratic in feature dimension. Fold into the logistic-bandit fix: choose a diagonal approximation
  with stated guarantees/cost, or store the full covariance and state O(d²) memory/arithmetic.
- **Round 2 M2.3 (finite logit inputs): RESOLVED.** `ε_w = 10⁻⁶` clamps reliability in §3.2 (lines
  437–438).
- **Round 1 concept-drift issue: NOT RESOLVED.** L2 weight decay is not temporal forgetting or drift
  detection (finding above).

## Verdict

**VERDICT: NOT CONVERGED** — five major corrections remain in the interpretation of source
reliability, SGD guarantees/update semantics, drift handling, and contextual uncertainty. These can
be addressed without adding a heavy native model: make the epistemic evidence model explicit, keep
async skills as score/feature producers with a separately calibrated and versioned interface, and
defer unsupported bandit confidence claims.

VERDICT: NOT CONVERGED

[source-1]: https://www.ncbi.nlm.nih.gov/books/NBK235178/
[source-2]: https://proceedings.mlr.press/v70/guo17a.html
[source-3]: https://cir.nii.ac.jp/crid/1362262946383268736
[source-4]: https://ojs.aaai.org/index.php/AAAI/article/view/10320
[source-5]: https://proceedings.mlr.press/v139/jun21a.html
[source-6]: https://doi.org/10.52202/079017-3960
[source-7]: https://jmlr.org/beta/papers/v25/22-1218.html
[source-8]: https://www.jmlr.org/papers/volume26/24-0452.html
