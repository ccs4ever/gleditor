# Round 2 Critique: Epistemologist (vagent Epistemic Arbitration)

Paths below are relative to the repo root (`/data/git/gleditor`). Tally: 0 BLOCKING, 0 MAJOR, 3
MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- **R1.1 (Epistemic Luck and Pragmatic Conflation in Settle Loop)**: **RESOLVED**. Automated plan
  outcome backpropagation is completely removed (§1.4, §1.5 `U-C3`, §6.2, §8.5). Belief attribution
  is strictly decoupled from worldly plan execution success and restricted to explicit perceptual
  verification steps (`confirm`, `refute`, residual parity), eliminating Gettier cases and unfair
  penalties from motor stalls.
- **R1.2 (Failure of AGM Contraction: Refuted Beliefs Persist)**: **RESOLVED**. `refute` performs
  structural 1D rank excision on `+d.clause` and links an epistemic defeater marker (§4.3).
  `believes truest` enforces a minimum epistemic acceptance threshold $\theta_{\min}$ (§4.2, §4.4,
  §5.1, §8.4), failing on insufficient warrant and correctly representing suspension of judgment
  ($\text{Susp}(P)$) in accordance with the AGM Success Postulate of Contraction.
- **R1.3 (Independence Fallacy in Testimony Corroboration)**: **RESOLVED**. Flawed Noisy-OR evidence
  multiplication is replaced with Bayesian posterior log-odds accumulation incorporating
  sub-additive correlation discounting $(1 - \rho_{|C|})$ per provenance cluster (§1.4, §3.2, §8.2).
  This neutralizes gossip echo chambers, correlated sensor failure compounding, and Sybil attacks.
- **R1.4 (Category Error: Warrant Conflated with Truth)**: **RESOLVED**. The specification
  explicitly clarifies in the Executive Summary (§1.2, §4.1) that `truest` denotes subjective
  epistemic warrant under bounded rationality rather than ontological correspondence with external
  reality.
- **R1.5 (Arbitrary Peer Disagreement Resolution via CellRef)**: **RESOLVED**. Decision `U-C4`
  formally adopts the Principle of Indifference (§1.5 `U-C4`, §5.1, §5.2). Exact ties branch into
  distinct choice points or fail under `truest`, eliminating non-epistemic allocator pointer bias
  from deliberation and learning.
- **R1.6 (Undercutting vs. Rebutting Defeat in Sensors)**: **RESOLVED**. Transient hardware
  impairments are decoupled from permanent prior degradation. §2.3 registers hardware health state
  `$status` (`NOMINAL`, `DEGRADED`, `SUSPECT`, `FAULTED`) and `$reject_count`. Execution timeouts no
  longer penalize priors (§6.2), and §5.3 applies transient $\chi^2$ innovation gating without
  mutating $(\alpha_S, \beta_S)$.
- **R1.7 (Continuous Fusion and Golden Mean Fallacy)**: **RESOLVED**. Continuous fusion is gated by
  $\chi^2$ Mahalanobis innovation testing (§1.4, §1.5 `U-C5`, §5.3). Mutually incompatible, bimodal
  candidate clusters refuse arithmetic fusion and are preserved as distinct disjunctive hypotheses.

## Status of previous MINOR findings

- **R1.8 (Temporal Decay of Historical Facts vs. Fluents)**: **RESOLVED**. Decay is specified
  per-predicate in declarative policies (§4.2) rather than globally on sources. Historical facts do
  not decay ($\lambda = 0$), and decay regresses toward the uninformative prior $p_0 = 0.5$ (maximum
  entropy) rather than false certainty (§3.1).
- **R1.9 (Verification Example Bug: Confirming Unasserted Claims)**: **RESOLVED**. Example 2 (§7.2)
  confirms and refutes the exact candidate variables bound during deliberation. Example 1 (§7.1)
  verifies arrival percepts via metric $\epsilon$-clustering (§3.5).

______________________________________________________________________

## BLOCKING

*None. All Round 1 BLOCKING findings have been resolved satisfactorily.*

______________________________________________________________________

## MAJOR

*None. All Round 1 MAJOR findings have been resolved satisfactorily.*

______________________________________________________________________

## MINOR

### 1. (§2.3, §5.1) Explicit Operational Weight Clamping for Faulted Sources (`$status`)

**Problem:** §2.3 registers `$status` (0: `NOMINAL`, 1: `DEGRADED`, 2: `SUSPECT`, 3: `FAULTED`) on
the Source Profile cell, and §5.3 increments `$reject_count` when sensory innovation fails. However,
§5.1 does not explicitly document the exact arithmetic rule applied to $w_S$ during candidate
evaluation when `$status` is non-zero.

**Fix:** In §5.1 and §8.2, add a brief operational specification: when `$status == 3` (`FAULTED`),
the source's weight is masked to $w_S = 0.0$ (or excluded from the log-odds sum); when
`$status == 1` (`DEGRADED`) or `2` (`SUSPECT`), its variance is inflated by an additional penalty
factor or weight discounted by a scalar factor (e.g. $0.5 \times w_S$), without mutating the
long-term Bayesian conjugate prior $(\alpha_S, \beta_S)$.

### 2. (§1.5 `U-C4`, §5.1) Configurable Indifference Tolerance Threshold ($\epsilon_{\text{tie}}$)

**Problem:** Decision `U-C4` adopts the Principle of Indifference for exact ties between candidate
hypotheses. In IEEE 754 floating-point arithmetic, two epistemic peers with nearly identical
evidence may differ by tiny machine precision epsilon ($|W_1 - W_2| \sim 10^{-15}$), which could
accidentally circumvent indifference branching.

**Fix:** Specify a small default indifference threshold $\epsilon_{\text{tie}}$ (e.g. $10^{-6}$) in
§5.1: if $|W_A - W_B| \le \epsilon_{\text{tie}}$, the candidates are treated as an epistemic tie
under Decision `U-C4`.

### 3. (§4.3, §6.3) Defeater Cell Lifecycle and Cleanup

**Problem:** §4.3 notes that `refute` "marks an epistemic defeater for the key tuple". The document
should clarify the lifecycle of this marker cell: whether it persists until explicitly overwritten
by a subsequent `confirm` on the same key tuple, or is reclaimed during turn settle when candidate
clauses are excised.

**Fix:** In §4.3 and §6.3, clarify that the epistemic defeater marker cell on `+d.epistemic` off the
predicate descriptor remains active to suppress newly arrived percepts matching the refuted tuple
until a fresh observation is explicitly confirmed or an invalidating epoch expires.

______________________________________________________________________

VERDICT: CONVERGED
