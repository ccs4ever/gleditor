# Round 2 Critique: ML Expert (design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## 1. Executive Summary & Audit Overview

In Round 1, the Machine Learning review identified critical mathematical and architectural defects
in the specification:

1. Conflation of uncontextual Beta-UCB with contextual LinUCB without context terms or covariance
   tracking;
1. Dimensionless epistemic trust $w_i \in [0, 1]$ directly equated to physical inverse variance
   $\frac{1}{\sigma_i^2}$, violating dimensional analysis, artificially capping accuracy at
   $\sigma^2 \ge 1.0$, and exhibiting a 0% breakdown point against physical outliers;
1. Contradiction between "Zero new VM opcodes" and reliance on non-existent `#Exp`, `#Ln`, `#Sqrt`
   opcodes in the Vortex VM;
1. Unregularized online SGD with unnormalized raw features causing IEEE 754 overflow and saturation;
1. Constant learning rate violating Robbins-Monro conditions;
1. Confounding physical plan execution failures with epistemic belief refutations (credit assignment
   flaw);
1. Single scalar provenance token incapable of attributing multiple beliefs in BDI plans;
1. Float jitter preventing continuous sensor corroboration under exact symbolic unification;
1. Stale memory decay and unaddressed concept drift;
1. Underspecified native ML skill interface and turn-suspending condition expressions violating
   purity.

Revision v1.1 of
\[`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md`\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md)
comprehensively addresses all Round 1 issues. The mathematical foundations are now rigorous,
probabilistically principled, and fully aligned with the hardware, VM, and language constraints of
`vagent`.

Every blocking and major finding from Round 1 has been successfully resolved. Three minor polish
points are documented in Section 3 for precision.

______________________________________________________________________

## 2. Detailed Audit of Round 1 Issue Resolutions

### 2.1 Separation of LinUCB, Beta-UCB, and Thompson Sampling (R1 Issue 1 — BLOCKING)

- **Round 1 Problem**: Section 3.4 was titled "Contextual Multi-Armed Bandits & Exploration" and
  claimed LinUCB, but provided only the scalar variance of an uncontextual Beta distribution
  $\mu_S + c \cdot \sigma_S$. It lacked context vectors, feature covariance matrices
  $\mathbf{A}_S^{-1}$, and exploration bounds for contextual logistic regression.

- **Resolution in v1.1**: Section 3.4 has been cleanly refactored into three distinct,
  mathematically sound exploration mechanisms:

  1. **Thompson Sampling (Recommended Default)**: Posterior sampling directly from the
     Beta-Bernoulli conjugate distribution:

     ```math
     \tilde{p}_S \sim \text{Beta}(\alpha_S, \beta_S)
     ```

     Eliminates the arbitrary tuning constant $c$ and achieves optimal Bayesian regret bounds.

  1. **Clamped Beta-UCB**: Operates strictly over uncontextual Tier 2 Beta reputations with an
     explicit upper clamp:

     ```math
     \text{Score}_{\text{UCB}}(S) = \min(1.0, \mu_S + c \cdot \sigma_S)
     ```

     Prevents probability simplex escapes beyond $[0.0, 1.0]$.

  1. **Contextual LinUCB**: Explicitly incorporates the ridge-regression exploration bonus with
     feature covariance $\mathbf{A}_S^{-1}$ updated online via the Sherman-Morrison rank-1 formula:

     ```math
     \text{Score}(S) = \mathbf{w}_S^T \mathbf{x} + c \sqrt{\mathbf{x}^T \mathbf{A}_S^{-1} \mathbf{x}}
     ```

- **Evaluation**: The separation is mathematically rigorous, clearly differentiating non-contextual
  from contextual exploration.

- **Status**: **RESOLVED**.

### 2.2 Decoupling Physical Covariance from Epistemic Trust (R1 Issue 2 — BLOCKING)

- **Round 1 Problem**: Section 5.3 set $\frac{1}{\sigma_i^2} = w_i$. This violated dimensional units
  ($[\text{units}]^{-2} \neq \text{dimensionless}$), capped physical measurement variance at
  $\sigma^2 \ge 1.0$, and lacked innovation gating (0% breakdown point against outliers).

- **Resolution in v1.1**: Adopted Decision `U-C5` and updated §1.4, §1.5, and §5.3:

  1. **Covariance Inflation**: Physical sensor measurement noise covariance $R_i$ (in physical units
     squared) is explicitly decoupled from dimensionless trust $w_i \in [0, 1]$. Trust acts as a
     covariance inflation penalty:

     ```math
     \tilde{R}_i = \frac{R_i}{\max(w_i, \epsilon_{\text{floor}})}
     ```

     When $w_i \to 1.0$, the sensor achieves its calibrated physical precision $R_i^{-1}$. When
     $w_i \to 0.0$, precision drops toward zero, gracefully down-weighting degraded sources.

  1. **Chi-Squared ($\chi^2$) Innovation Gating**: Outlier rejection is enforced prior to fusion:

     ```math
     d_M^2 = (x_i - \hat{x}_0)^T (\tilde{R}_i + P_0)^{-1} (x_i - \hat{x}_0) \le \chi^2_{\alpha, k}
     ```

     Measurements with $d_M^2 > \chi^2_{\alpha, k}$ are rejected as physical outliers and increment
     `$reject_count`.

  1. **Multi-Modal / Disjunctive Protection**: If candidate measurements form distinct clusters that
     cannot be reconciled within the gate, Gaussian fusion is refused, preserving disjunctive
     hypotheses as distinct choice points rather than synthesizing a fictitious centroid.

  1. **Dynamic Covariance Expansion**: Dead-reckoning drift dynamics are modeled in §3.1 via:

     ```math
     R_{\text{dead}}(\Delta t) = R_0 + Q_{\text{drift}} \cdot (\Delta t)^3
     ```

- **Evaluation**: Flawless formulation. Robust sensor fusion principles (Kalman innovation gating,
  covariance inflation) are faithfully integrated.

- **Status**: **RESOLVED**.

### 2.3 Vortex VM Math Opcodes `#Exp`, `#Ln`, `#Sqrt` (R1 Issue 3 — BLOCKING)

- **Round 1 Problem**: Specification claimed "Zero new VM opcodes" while referencing `#Exp`.
  However, `vortex_vm.hpp` had no `#Exp`, `#Ln`, or `#Sqrt`, making transcendental and geometric
  evaluations unexecutable in bytecode.

- **Resolution in v1.1**:

  - Proposal P19.7 (§8.7) formally adds three elementary math opcodes to `OpcodeKind` in
    `apps/common/xanadu/vortex/vortex_vm.hpp` and `vortex_vm.cpp`: `OpcodeKind::Exp`,
    `OpcodeKind::Ln`, `OpcodeKind::Sqrt`.
  - Instructions operate natively on R6 float scalar bits with NaN and infinity clamping.
  - Document prerequisites (line 9), Approved Decisions (`U-C2`), and Verification Matrix (§9)
    consistently incorporate P19.7.

- **Evaluation**: Eliminates the contradiction and provides native CPU FPU performance without
  costly bytecode polynomial approximations.

- **Status**: **RESOLVED**.

### 2.4 Regularized Online SGD, Welford Normalization, Clamping (R1 Issue 4 — MAJOR)

- **Round 1 Problem**: Online SGD lacked feature normalization, $L_2$ weight decay, and logit
  clamping, causing gradient explosion and floating-point overflow ($e^{50000} \to \infty$) on raw
  physical features (e.g., lux = 1000.0).

- **Resolution in v1.1**: Section 3.3 explicitly specifies:

  1. **Online Welford Feature Normalization**:

     ```math
     x_{\text{norm}, j} = \frac{x_j - \hat{\mu}_j}{\hat{\sigma}_j + \epsilon}
     ```

     Running means and variances are tracked online, preventing disparate feature magnitudes from
     distorting gradients.

  1. **Logit Clamping**: $z_S$ is strictly clamped to $[-30.0, 30.0]$ prior to `#Exp`:

     ```math
     \hat{y}_S = \sigma(\text{clamp}(z_S, -30.0, 30.0))
     ```

     Guarantees that $e^{-z_S}$ never overflows or underflows IEEE 754 double precision ($e^{30.0}
     \approx 1.07 \times 10^{13}$).

  1. **Coupled $L_2$ Weight Decay**:

     ```math
     \mathbf{w}_S \leftarrow (1 - \eta_t \lambda_{\text{reg}}) \mathbf{w}_S - \eta_t (\hat{y}_S - y) \mathbf{x}_{\text{norm}}
     ```

     ```math
     b_S \leftarrow b_S - \eta_t (\hat{y}_S - y)
     ```

     Shrinks weights toward zero on non-reinforced features, preventing unbounded weight growth on
     separable data.

- **Evaluation**: The SGD engine is now numerically stable, regularized, and safe against feature
  scale disparities.

- **Status**: **RESOLVED**.

### 2.5 Robbins-Monro Learning Rate Scheduling via `$obs_count` (R1 Issue 5 — MAJOR)

- **Round 1 Problem**: Constant learning rate ($\eta \le 0.05$) violated Robbins-Monro conditions,
  preventing convergence to empirical risk minimizers and oscillating indefinitely. Profile field
  `$obs_count` was unutilized.

- **Resolution in v1.1**: Section 3.3 (line 448) ties learning rate decay directly to `$obs_count`
  ($N_S$):

  ```math
  \eta_t = \frac{\eta_0}{\sqrt{N_S + 1}}
  ```

  Satisfies the square-summable condition $\sum \eta_t^2 < \infty$ while allowing sufficient step
  size $\sum \eta_t = \infty$ for stochastic convergence.

- **Evaluation**: Connects metadata field `$obs_count` directly to convergence theory.

- **Status**: **RESOLVED**.

### 2.6 Credit Assignment: Decoupling Plan Failures from Refutation (R1 Issue 6 — MAJOR)

- **Round 1 Problem**: Goal execution failures (motor stalls, path obstructions, timeouts) routed to
  $y=0$ penalties against belief sources, injecting false-negative label noise and degrading
  reliable sensors.

- **Resolution in v1.1**:

  - Adopted Decision `U-C3` ("Perceptual Verification vs. Epistemic Luck").
  - Section 6.2 strictly decouples plan goal execution outcomes from belief attribution. Motor
    stalls and timeouts generate zero epistemic updates.
  - Updates occur exclusively upon explicit perceptual verification directives (`confirm`, `refute`)
    or verified perceptual residuals.
  - Section 6.3 establishes an Agent Settle Queue on `+d.epistemic` ensuring that verified
    refutations are never rolled back when a speculative plan fails.

- **Evaluation**: Completely eliminates the Gettier credit assignment flaw.

- **Status**: **RESOLVED**.

### 2.7 Multi-Belief Provenance Tokens on Intention Frames (R1 Issue 7 — MAJOR)

- **Round 1 Problem**: Single scalar token on intention frames could not track multiple distinct
  beliefs in complex BDI plans without clobbering or false attribution.

- **Resolution in v1.1**: Section 6.1 specifies that multiple `EpistemicProvenanceToken` cells chain
  along `+d.step` off the intention frame. Each token records `kTokenPredicate`, `kTokenKeyHash`,
  and `kTokenSources` clone view cells on `+d.clone`, enabling precise per-predicate verification.

- **Status**: **RESOLVED**.

### 2.8 Continuous Metric Clustering and Social Epistemic Corroboration (R1 Issue 8 — MAJOR)

- **Round 1 Problem**: Exact symbolic unification partitioned continuous sensor floats into distinct
  clauses, preventing corroboration. Multi-source Noisy-OR combination violated conditional
  independence and suffered from data incest.

- **Resolution in v1.1**:

  - **Continuous Metric Clustering (§3.5)**: The engine clusters clauses along `+d.clause` within
    metric tolerance $\epsilon_{\text{tol}}$:

    ```math
    \text{Cluster}(C_1, C_2) \iff \|\mathbf{K}(C_1) - \mathbf{K}(C_2)\|_2 \le \epsilon_{\text{tol}}
    ```

    This groups nearby continuous percepts into mutual hypothesis clusters prior to arbitration.

  - **Bayesian Log-Odds Evidence Combination (§3.2)**:

    ```math
    \text{logit}(W) = \text{logit}(P_0) + \sum_{C \in \text{Clusters}} (1 - \rho_{|C|}) \sum_{S \in C} \ln \left( \frac{w_S}{1 - w_S} \right)
    ```

    Sub-additive correlation discount factor $\rho_{|C|}$ prevents echo chambers and Sybil attacks
    among shared-provenance peer clusters.

- **Status**: **RESOLVED**.

### 2.9 Maximum-Entropy Memory Decay and Concept Drift (R1 Issue 9 — MAJOR)

- **Round 1 Problem**: Underspecified periodic discount schedule; exponential decay to 0.0 implied
  false certainty ($100\%$ false); no concept drift adaptation in SGD.

- **Resolution in v1.1**:

  - Section 3.1 establishes regression toward maximum entropy ($p_0 = 0.5$):

    ```math
    w_S(\Delta t) = p_0 + e^{-\lambda_S \Delta t} (w_S(0) - p_0)
    ```

  - Evaluated lazily as continuous elapsed turn decay
    $\Delta t = t_{\text{current}} - t_{\text{asserted}}$.

  - Coupled $L_2$ weight regularization ($(1 - \eta_t \lambda_{\text{reg}})$) gracefully attenuates
    stale SGD feature weights over time.

- **Status**: **RESOLVED**.

### 2.10 Native ML Skill Asynchronous Decoupling & Expression Purity (R1 Issue 10 — MAJOR)

- **Round 1 Problem**: Calling heavy external neural runtimes inside synchronous query expressions
  (`where: believes truest`) violated `checker.cpp:2873` purity constraints and introduced
  catastrophic sequential turn latency.

- **Resolution in v1.1**:

  - Adopted Decision `U-C6` and updated §4.4 and §8.6 (Proposal P19.6).
  - Query expressions and condition guards are strictly pure and synchronous.
  - Heavy native ML skills execute asynchronously as plan statements or percept ingestion pipes,
    pre-populating on-manifold feature embeddings for instant synchronous evaluation.

- **Status**: **RESOLVED**.

### 2.11 Minor Issues Audit (R1 Issues 11–13 — MINOR)

- **R1 Issue 11 (BCE Numerical Instability)**: Resolved in §3.3; updates evaluate error residuals
  $(\hat{y}_S - y)$ directly without evaluating unstable logarithms.
- **R1 Issue 12 (Thompson Sampling Alternative)**: Resolved in §3.4; Thompson Sampling is elevated
  to the recommended default exploration policy.
- **R1 Issue 13 (`$obs_count` Integration)**: Resolved in §2.3 and §3.3; `$obs_count` directly
  drives Robbins-Monro learning rate scheduling.
- **Status**: **ALL RESOLVED**.

______________________________________________________________________

## 3. Minor Polish Points (Non-Blocking / Minor)

### M2.1 (§Executive Summary vs. §3.3) Robbins-Monro Denominator Typo

- **Location**: Line 69 (Executive Summary) vs. Line 448 (§3.3).

- **Description**: Executive Summary line 69 writes the Robbins-Monro schedule with a minus sign in
  the denominator: `\eta_t = \frac{\eta_0}{\sqrt{N_S - 1}}`. If $N_S = 0$ (a new source with zero
  observations) or $N_S = 1$, the denominator evaluates to $\sqrt{-1}$ (NaN) or $\sqrt{0}$ (division
  by zero). In §3.3 (line 448), the formula is correctly specified as:

  ```math
  \eta_t = \frac{\eta_0}{\sqrt{N_S + 1}}
  ```

- **Fix**: Update Executive Summary line 69 to match Eq. 448 (`\sqrt{N_S + 1}`).

### M2.2 (§2.3, §3.4, §8.3) Manifold Representation for LinUCB Inverse Covariance

- **Location**: §2.3 (Table 2.3), §3.4 (Eq. 483), §8.3 (Proposal P19.3 line 887).
- **Description**: Section 3.4 specifies contextual LinUCB with exploration bonus
  $c \sqrt{\mathbf{x}^T \mathbf{A}_S^{-1} \mathbf{x}}$ updated via Sherman-Morrison rank-1 updates.
  Table 2.3 lists `$weights` as a head cell linking a 1D rank of R6 float scalar cells along
  `+d.step`, but does not define where the $d \times d$ inverse covariance matrix
  $\mathbf{A}_S^{-1}$ resides. Proposal P19.3 line 887 cites "Pricing: $O(d)$ arithmetic
  operations". For a general $d \times d$ matrix, vector-matrix-vector multiplication and rank-1
  updates require $O(d^2)$ operations. (If a diagonal covariance approximation is used, complexity
  is $O(d)$).
- **Fix**: In §2.3, clarify that for sources configured with `model: "linucb"`, the $d \times d$
  matrix $\mathbf{A}_S^{-1}$ is stored as a flattened $d^2$ rank along `+d.step` off `$covariance`
  (or as a diagonal variance vector if diagonal Laplace approximation is selected), and update P19.3
  pricing to note $O(d^2)$ operations for full matrix LinUCB.

### M2.3 (§3.2, §8.2) Boundary Clamping for Log-Odds Likelihood ($\epsilon_{\text{prob}}$)

- **Location**: §3.2 (Eq. 406), §8.2 (P19.2 item 2).

- **Description**: In Bayesian posterior log-odds accumulation:

  ```math
  \text{logit}(W) = \text{logit}(P_0) + \sum_{C} (1 - \rho_{|C|}) \sum_{S \in C} \ln \left( \frac{w_S}{1 - w_S} \right)
  ```

  If an un-smoothed weight reaches $w_S = 1.0$ or $w_S = 0.0$, the argument evaluates to
  $\ln(\infty)$ or $\ln(0)$, generating $\pm \infty$. While Laplace-smoothed Beta reputations
  ($\alpha, \beta \ge 1.0$) naturally keep $\mu_S \in (0, 1)$, user-asserted static weights or
  decayed profiles could theoretically hit the boundaries.

- **Fix**: In §8.2, specify that `epistemic_log_odds_combine` clamps input weights to
  $[\epsilon_{\text{prob}}, 1.0 - \epsilon_{\text{prob}}]$ (e.g. $\epsilon_{\text{prob}} = 10^{-6}$)
  prior to evaluating $\ln \frac{w_S}{1 - w_S}$.

______________________________________________________________________

## 4. Acceptance Verification Battery Audit

The verification plan in §9 aligns directly with repo conventions:

- `tests/xuzz/vortex_vm_test.cpp`: Unit tests for `#Exp`, `#Ln`, `#Sqrt` domain boundaries and FPU
  accuracy.
- `tests/xuzz/vortex_stdlib_test.cpp`: Asserts regularized online SGD, Robbins-Monro convergence,
  Welford normalization, $\chi^2$ innovation gating, and log-odds accumulation.
- `tests/xuzz/vagent_compiler_test.cpp`: Tests AST lowering of `truest`, `ranked`, `consensus`,
  `confirm`, `refute`, inner evaluation scoping ($m_{\text{eval}}$), and expression purity.
- `tests/xuzz/vagent_executor_test.cpp`: Verifies settle queue persistence across plan failure
  rollbacks and asynchronous native ML skill execution.
- `tests/xuzz/vagent_reconciler_test.cpp`: Asserts state reconciliation of `+d.trust` profiles and
  learned weights across P17 live upgrades.

The test plan satisfies the single-location rule in `AGENTS.md` and provides thorough coverage.

______________________________________________________________________

## 5. Verdict

All 3 BLOCKING and 7 MAJOR issues from Round 1 have been completely resolved with sound mathematical
formulations and zero violations of Vortex, ZigZag, or vagent invariants. The three minor polish
points identified are non-blocking.

VERDICT: CONVERGED
