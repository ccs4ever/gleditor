# Round 4 ML Review — v1.3

Scope: Round 3 major findings, Round 2 covariance API/complexity finding, and native model
versioning/calibration claims.

## Prior findings status

- **R3 M1 — competence versus likelihood / correlated evidence:** **CLOSED.** §3.1 (lines 140–164)
  explicitly makes the log-opinion pool a warrant-ranking heuristic, distinguishes report
  correctness from a hypothesis likelihood ratio, gives the restricted symmetric-binary case, and
  states correlation averaging is not an exact joint likelihood.
- **R3 M2 — `η₀/√n` convergence claim:** **CLOSED.** §3.3 (lines 212–228) uses `η₀/(n+1)`, correctly
  gives divergent `Σηₙ` and square-summable `Σηₙ²`, and limits convergence to stated conditions
  without a universal guarantee.
- **R3 M3 — changing Welford coordinates and delayed labels:** **CLOSED.** §3.3 (lines 202–210)
  freezes a warm-up normalizer per epoch, stores the normalized feature snapshot, defines
  deterministic arrival-order application, and expires labels outside the feedback horizon or model
  epoch.
- **R3 M4 — unsupported logistic LCB/LinUCB confidence:** **CLOSED.** §3.4 (lines 234–247) limits
  LCB to Beta reputation as a heuristic and defers contextual bandits, explicitly rejecting linear
  ridge LinUCB as a logistic-SGD bound.
- **R3 M5 — L2 mistaken for drift adaptation:** **CLOSED.** §3.3 (lines 228–232) scopes the model to
  stationary epochs, makes host-signalled epoch reset explicit, and defers automatic drift
  detection; it no longer credits L2 with drift handling.
- **R2 M2.2 — LinUCB matrix storage and O(d) price:** **CLOSED BY DEFERRAL.** Contextual bandits and
  their covariance state are outside initial P19.3 scope (§3.4, lines 238–247); P19.3 now prices SGD
  only (proposal register). No missing `A⁻¹` field or misleading LinUCB arithmetic price remains in
  the initial scope.
- **Native model versioning/calibration:** **CLOSED.** §3.4 (lines 249–265) versions the model,
  feature schema, observation and calibration epoch; refuses stale completions and raw scores as
  probabilities/covariances; and links calibration, shift and configuration-shift research. The 2026
  arXiv manuscript is correctly identified as a manuscript and reports EMNLP 2026 acceptance; its
  abstract covers prompt template, decoding temperature and weight quantization shifts
  ([arXiv:2608.01460][source-1]). The cited techniques have distinct scopes: Guo et al. on neural
  probability calibration ([ICML 2017][source-2]); Gibbs & Candès on online prediction sets under
  arbitrary shifts ([JMLR 2024][source-3]); Areces et al. on online conformal coverage
  ([ICML 2025][source-4]). The draft does not present conformal coverage as point-probability
  calibration.

## New findings

### MINOR 1 — Logistic intercept has no regularizer or explicit bound

**Location:** §3.3, lines 218–225.

**Problem:** The update applies L2 shrinkage to `w` but not `b`. If a source's accepted labels
contain only one class (plausible during a long all-correct or all-incorrect run), the unregularized
logistic intercept has no finite empirical-risk minimizer; it can move toward the clamped logit
boundary and make `ŷ` nearly 0 or 1. The document disclaims universal convergence/calibration, so
this is a bounded edge-case clarification rather than a blocker.

**Fix:** State that `b` is regularized or clamped, or keep Beta reputation as the active model until
the logistic epoch has both positive and negative verified labels and define a restart rule for
one-class feedback.

### MINOR 2 — Verification weight `q` is not carried into the SGD update

**Location:** §3.1, lines 132–134; §3.3, lines 218–222; §6.3 verification weight contract.

**Problem:** The text defines `q` as the verification weight and uses it for fractional Beta
updates, while the logistic update uses the binary `y` with an unweighted gradient. It is unclear
whether low-weight labels should affect Beta and SGD differently.

**Fix:** Either multiply the SGD gradient (and intercept gradient) by `q`, or say explicitly that
`q` weights only Beta counts and logistic SGD accepts only unit-weight labels.

## Tally and verdict

- BLOCKING: 0
- MAJOR: 0
- MINOR: 2

VERDICT: CONVERGED

[source-1]: https://arxiv.org/abs/2608.01460
[source-2]: https://proceedings.mlr.press/v70/guo17a.html
[source-3]: https://jmlr.org/beta/papers/v25/22-1218.html
[source-4]: https://proceedings.mlr.press/v267/areces25a.html
