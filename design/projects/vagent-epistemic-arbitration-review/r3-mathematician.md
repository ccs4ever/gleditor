# Round 3 critique: Mathematician & Math Formatting Specialist

Document reviewed: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2.

## Previous findings and status

Round 1 had 3 BLOCKING, 4 MAJOR, and 1 MINOR finding. The r1 disposition marks all eight accepted
and the current text incorporates them: Bayesian-style log-odds (§3.2), bounded Beta-UCB / Thompson
sampling (§3.4), covariance inflation and innovation gating (§5.3), maximum-entropy decay (§3.1),
explicit score composition, SGD regularization, continuous clustering, and math-format fixes. Round
2 has no mathematician report because of quota exhaustion, so there are no Round 2 findings to
disposition. The R1 statuses are accepted, not independently certified as closed here; the new
findings below concern the v1.2 formulation.

**Current tally: 1 BLOCKING, 3 MAJOR, 1 MINOR.**

## BLOCKING

### K3.1 — Source competence is used as a likelihood ratio without specifying the observation model

**Location:** §3.2 lines 426–469; applied again in §5.1 lines 713–715 and §7 examples.

**Problem:** The equation labels `w_S` as a source's warrant, defined as a Beta mean for historical
competence or an SGD prediction, then inserts `w_S/(1-w_S)` as the likelihood ratio for a candidate.
Those are different quantities in general. A Beta posterior mean for accuracy is not itself a
posterior probability that this particular candidate is true, nor is its odds automatically a Bayes
factor. The formula is valid under a specific binary-reporting model: sources emit a positive report
through a symmetric channel with sensitivity = specificity = `w_S`, reports are conditionally
independent, and the source-level `w_S` is calibrated for the relevant context. That model is
neither stated nor guaranteed by the Beta accuracy update. A source with asymmetric false-positive
and false-negative rates needs report-direction-specific likelihood ratios.

There is a related overclaim in the correlation correction. The effective-sample-size factor
`n_eff = n / (1 + (n - 1) rho)` is a design-effect heuristic for equicorrelated observations;
scaling averaged log-odds by it is not generally an exact likelihood under an arbitrary
pairwise-error correlation model. Calling the whole result a Bayesian posterior makes the evidential
semantics stronger than the stated assumptions support.

**Evidence:** Lines 426–438 define `p` as source reliability/competence and `w_S` as `mu_S` or an
SGD confidence output. Lines 441–446 use log-odds of that `w_S` as evidence. Lines 455–464 give a
scalar pairwise correlation coefficient and the effective-sample-size discount without a joint
observation model. Bayes' odds form requires multiplying prior odds by a likelihood ratio, where the
likelihood ratio is `P(E|H)/P(E|not H)`; it is not generally the odds of a posterior score. See
[Stanford Encyclopedia of Philosophy, Bayes' theorem][source-1] and the
[National Academies reference manual][source-2], which also cautions that conditional independence
assumptions require justification.

**Minimal fix:** Either define the narrow symmetric binary observation model explicitly and say the
source reports are conditionally independent across groups, with correlation discounting explicitly
marked as an approximation; or store calibrated sensitivity and specificity (or direct,
context-conditioned likelihood ratios) and combine each observed report with its direction-specific
Bayes factor. Define whether `online_sgd` predicts correctness or
`P(candidate true | report, context)`; only the latter, calibrated against the same stated prior,
can be converted to a Bayes factor with the corresponding prior correction. If retaining the
effective-sample-size expression, call it a heuristic tempered log-opinion pool rather than an exact
Bayesian posterior. Add a small numerical example showing a positive and a negative report and the
prior used.

## MAJOR

### K3.2 — The stated Robbins–Monro schedule does not meet the usual square-summability condition

**Location:** §3.3 lines 495–500; P19.3 line 1077.

**Problem:** The schedule `eta_t = eta_0 / sqrt(N_S + 1)` is called a Robbins–Monro schedule “to
guarantee stochastic convergence.” Although its sum diverges, its squared sum is
`eta_0^2 sum 1/(n+1)`, which also diverges. It therefore fails the classical Robbins–Monro
sufficient step-size conditions `sum eta_n = infinity` and `sum eta_n^2 < infinity` under the usual
bounded-variance noise assumptions. The current guarantee is unsupported. (An `n^-1/2` schedule can
be useful for finite-time optimization/regret, but that is a different claim and needs stated
assumptions/objective.)

**Evidence:** The update schedule is at lines 495–500. The original [Robbins–Monro paper][source-3]
states a finite sum of squared step coefficients for its convergence theorem; a
[formalized exposition of the theorem][source-4] gives the familiar conditions explicitly.

**Minimal fix:** If asymptotic stochastic approximation convergence is intended, use
`eta_t = eta_0 / (N_S + 1)^a` with `1/2 < a <= 1`, and state the assumptions (bounded/controlled
gradient noise, objective conditions, and treatment of regularization). Otherwise retain `a = 1/2`,
remove “guarantee convergence,” and describe the finite-time optimization rationale and its limits.

### K3.3 — Gaussian fusion ignores the cross-correlation already admitted by the source model

**Location:** §3.2 lines 455–464 and §5.3 lines 805–844.

**Problem:** The document explicitly models correlated source errors (gossip peers, shared I2C bus),
but Gaussian fusion still adds each measurement's precision independently: `P = (sum R_i^-1)^-1`.
That covariance is only justified for mutually independent measurement errors (conditional on the
state). Relayed/copy observations or shared sensor errors counted as separate measurements make the
fused covariance overconfident; the scalar source-correlation discount used for discrete warrant is
not applied to continuous fusion and is not a substitute for a measurement cross-covariance matrix.

**Evidence:** Correlation groups and `rho_G` are introduced at lines 455–464; fusion precision
addition appears at lines 838–844. Standard Gaussian/linear fusion with correlated observations
requires the joint observation covariance; the current equation assumes block-diagonal covariance.

**Minimal fix:** State and enforce that only conditionally independent measurements enter this
fusion equation, collapsing duplicate/relayed readings first; or define a joint block covariance `R`
and use generalized least squares, `P = (H^T R^-1 H)^-1`, `xhat = P H^T R^-1 z` (with the model's
observation matrices specified). Do not report the independent-noise covariance as calibrated
uncertainty when the correlations are unknown.

### K3.4 — Numeric domain and failure behavior of new math operations is underspecified

**Location:** §2.3 lines 344–346; §3.2 lines 437–438; §3.3 lines 488–493; §5.3 lines 793–844; P19.7
lines 1128–1141.

**Problem:** The proposal says `Ln`/`Sqrt` operate with “NaN and infinity clamping,” but gives no
clamp values or refusal/error behavior. It also proposes IEEE `ln(x)` and `sqrt(x)` without domains.
Meanwhile fusion requires finite positive-definite covariance (and invertible sums); Welford
normalization can divide by a zero/near-zero standard deviation; covariance inflation depends on a
positive `epsilon_floor`; and variance annotations may be malformed or non-finite. NaNs/infinities
can defeat comparisons such as the innovation gate and acceptance thresholds. “Clamp” is not a
defined numerical contract and can silently turn invalid input into a plausible confidence or
covariance.

**Evidence:** P19.7 lines 1134–1138 describes both IEEE operations and unspecified clamping.
`epsilon` and `epsilon_floor` are not assigned a positive value at lines 488–490 and 805–809. Fusion
inversion is specified at lines 811–844 without positive-definiteness/finite checks.

**Minimal fix:** Specify one consistent VM contract: either standard IEEE-754 results plus explicit
finite/domain checks at every epistemic call site, or typed arithmetic refusal on
out-of-domain/non-finite inputs. Require `alpha,beta > 0`, `0 < epsilon_w < 0.5`,
`epsilon_floor > 0`, finite nonnegative variances with positive-definite covariance, and a strictly
positive normalization floor. Define how singular covariance and zero-variance exact observations
are handled. Avoid silently clamping invalid inputs to ordinary finite evidence.

## MINOR

- **K3.5 — `with uncertainty: ?sigma` conflates scalar and matrix uncertainty.** §4.1 lines 613–615
  and §5.3 lines 846–849 expose a scalar name, while §5.3 carries `P_consensus` as covariance and
  supports `k` continuous coordinates. Define this binding as a covariance matrix (or explicitly say
  it is only a scalar isotropic standard deviation); otherwise clients cannot know whether values
  are variance, standard deviation, or matrix entries.

VERDICT: NOT CONVERGED

[source-1]: https://plato.stanford.edu/entries/bayes-theorem/
[source-2]: https://www.ncbi.nlm.nih.gov/books/NBK621595/
[source-3]: https://doi.org/10.1214/aoms/1177729586
[source-4]: https://drops.dagstuhl.de/opus/volltexte/2022/16740/pdf/LIPIcs-ITP-2022-31.pdf
