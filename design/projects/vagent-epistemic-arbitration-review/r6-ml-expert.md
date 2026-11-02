# Round 6 Full ML Review — Approved v1.3

Reviewed the frozen input `/tmp/vagent-epistemic-r6/input-v1.3-approved.txt` (SHA-256:
`c4c9506fd39f40f673931b35f37a6abac6e0510d0ec176507167dcafe050693f`), the P19 approval record and R5
ML report. This is a full read of v1.3, not a diff-only pass.

## Findings

### MAJOR — A versioned reversal can leave the superseded label in both learners

**Location:** §3.1 lines 133–139; §4 lines 297–306; §6.3 lines 555–573.

**Problem:** The specification deduplicates by verification ID and applies a label once per
verification/observation pair, while permitting a “new versioned verification record” to reverse an
old verdict. It never defines that record as superseding the old one or how to undo/rebuild the old
Beta and SGD updates. A new ID therefore appears to be a second label. For a single observation
initially labeled positive and later corrected negative with q=1, Beta(1,1) becomes Beta(2,2),
preserving the mean but falsely shrinking variance from 1/12 to 1/20. The SGD update also retains
the first gradient; applying the opposite label at the current parameters is not equivalent to
correcting the original training example.

**Minimal fix:** Give each correction an explicit `supersedes_verification_id` and enforce one
active adjudication per `(observation, model/domain epoch)`. On replacement, retract and
deterministically replay the affected Beta/SGD event history (or rebuild that learner from its
bounded label log); define conflict and redelivery behavior. Test a positive-to-negative correction
and an idempotent replay of both versions.

### MAJOR — Domain-specific competence state and epoch matching are not fully keyed

**Location:** §2.1 table, lines 76–81; §3.1 lines 126–143; §3.3 lines 203–210, 232–235.

**Problem:** v1.3 requires reliability to be scoped by predicate signature and domain/calibration
epoch, but the source-profile schema lists one `alpha`, `beta`, weights, bias, normalizer and label
count. Delayed-label rejection checks obsolete schema/model epoch, without explicitly checking
predicate/domain and calibration epoch against the learner being updated. If the singular profile
state is used, one source that is accurate for `target_spotted` and poor for `defect_detected` pools
those labels despite the “no cross-domain transfer” rule. Even with nested calibration records
intended, the record key and delayed-label guard are unspecified.

**Minimal fix:** Define the learner/reputation state key explicitly (at least source identity ×
predicate signature/domain × calibration/model epoch × feature schema). Store that key in each
observation and verification, and require an exact key/epoch match before applying delayed labels;
specify reset/migration behavior for domain and calibration epoch changes. If a source-wide model is
intended instead, remove the no-transfer claim and state the shared training domain and feature
schema.

### MINOR — LCB and observation-age decay have no defined composition

**Location:** §3.2 lines 170–182; §3.4 lines 240–243.

**Problem:** Freshness decay acts on an observation's warrant, while the Beta-only LCB is separately
defined as `max(0, μ − c√v)`. The order is unspecified, as is whether age changes the LCB score, its
mean input, or its uncertainty term. For a stale observation, a caller could use an undiminished LCB
and bypass the stated freshness discount.

**Minimal fix:** Define the deterministic score order—for example, calculate the Beta risk score,
then decay that score toward 0.5 by observation age—or define another heuristic explicitly. State
that the resulting value is a ranking score, not a posterior bound.

## Requested edge-case audit

- **R5 intercept regularization and q-weighted gradients:** Closed and correct in §3.3 lines
  219–227. Both gradients use q; L2 applies to w and b; finite positive regularization and `η₀λ ≤ 1`
  keep the shrink factor nonnegative.
- **SGD schedule and convergence:** Closed. `η₀/(n+1)` has divergent step sum and square-summable
  squared steps. The text correctly limits convergence to suitable stationary/noise/convexity
  assumptions and makes no universal convergence or calibration guarantee. n counts accepted labels;
  q is a bounded data-loss weight. No further issue found apart from label replacement semantics
  above.
- **Normalization, warm-up, model epochs and delayed labels:** The frozen normalizer, stored
  normalized context, deterministic receive-sequence update, warm-up Beta fallback and expiry for
  obsolete schema/model epochs address R3. Domain/calibration-key matching remains the major issue
  above.
- **Clipped sigmoid/gradient:** No separate major finding. The residual update remains finite and
  recovers saturated wrong predictions. For clarity, implementation should treat it as the bounded
  residual surrogate implied by the formula rather than claim it is the literal derivative of BCE
  composed with a hard clamp; no convergence guarantee is made.
- **Source selection and validation:** The requirement that calibration/validation data be
  independent of selection is explicit (§3.1 lines 137–139), and future bandits require propensity
  logging (§3.4 lines 245–249). Initial scoring is expressly a warrant heuristic, not a calibrated
  probability. This is adequate for the stated scope; validation must follow the stated independence
  condition.
- **Report-correctness target:** The text now scopes labels to verified reports in a declared
  predicate/domain, makes detector/LLM scores non-probabilistic by default and restricts negative
  labels for competing values to declared exhaustive alternatives (§3.1 and §6.3). It does not
  silently turn agreement or gate acceptance into ground truth. No separate finding.
- **LCB and decay:** See the minor finding above.
- **Deferred bands/matrix/exact posterior:** No finding; they remain explicitly deferred as
  requested.
- **References:** Checked the cited primary venues/pages. Guo et al. supports neural probability
  calibration/temperature scaling; Gibbs–Candès and Areces et al. concern online conformal
  prediction-set coverage under their stated settings, not point-probability calibration. The 2026
  arXiv paper's abstract addresses prompt, decoding-temperature and quantization configuration shift
  and reports EMNLP 2026 acceptance. No citation is being used to claim an LLM confidence score is a
  probability.

## Previous-round closure status

R3 M1–M5 remain closed as recorded in R4/R5. R2 M2.2 (LinUCB covariance/API and cost) remains closed
by deferring contextual bandits. R4 MINOR 1 (intercept regularization) and MINOR 2 (q in SGD) remain
closed. The two major findings above are new Round 6 edge cases.

## Tally and verdict

- BLOCKING: 0
- MAJOR: 2
- MINOR: 1

VERDICT: NOT CONVERGED
