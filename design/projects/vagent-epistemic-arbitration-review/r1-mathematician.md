# Round 1 critique: Mathematician & Math Specialist (vagent-epistemic-arbitration)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 1 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

Initial round. No previous findings.

## BLOCKING

### 1. (§3.5, line 442) Independent Evidence Fallacy: Noisy-OR Misapplication

Full title: The Independent Evidence Combination Fallacy: Noisy-OR Misapplication, Uninformative
Coin-Flip Inflation, and Pathological Amplification of Untrustworthy Sources

**Problem:** Section 3.5 derives the consensus corroboration formula:

```math
W(\text{Clause}) = 1.0 - \prod_{S \in \text{Sources}(\text{Clause})} (1.0 - w_S)
```

from the stated assumption:

```math
P(F \text{ false} \mid \mathcal{S}) = \prod_{S \in \mathcal{S}} (1 - w_S)
```

where $w_S$ is the epistemic reliability / posterior probability $P(F = 1 \mid S)$. This derivation
is mathematically invalid and violates fundamental probability theory in four ways:

1. **Multiplying Conditioned Posteriors Is Mathematically False**: Even under conditional
   independence of sources given the state $\neg F$ ($P(S_1 \cap S_2 \mid \neg F) = P(S_1 \mid \neg
   F) P(S_2 \mid \neg F)$), Bayes' theorem establishes that:

   ```math
   P(\neg F \mid S_1, S_2) = \frac{P(S_1 \mid \neg F) P(S_2 \mid \neg F) P(\neg F)}{P(S_1, S_2)} \neq P(\neg F \mid S_1) P(\neg F \mid S_2)
   ```

   Multiplying posterior probabilities $1 - w_i$ directly is a classic probabilistic fallacy.

1. **The Uninformative Coin-Flip Paradox**: Suppose a source has accuracy $w_i = 0.5$ (a completely
   uninformative random coin flip providing zero mutual information). Under the formula:

   - For $N = 2$ uninformative sources: $W = 1 - (1 - 0.5)^2 = 0.75$.
   - For $N = 10$ uninformative sources: $W = 1 - (0.5)^{10} = 1 - \frac{1}{1024} \approx 0.99902$.
     A committee of completely random noise sources achieves arbitrary mathematical certainty of
     truth ($W \to 1.0$) simply by repeating the same assertion.

1. **Untrustworthy Source Reinforcement**: Suppose two sources are known to be untrustworthy ($w_1 =
   0.20, w_2 = 0.20$). If both assert the same claim:

   ```math
   W = 1 - (1 - 0.20)^2 = 1 - 0.64 = 0.36 > 0.20
   ```

   For 4 such sources: $W = 1 - (0.80)^4 = 1 - 0.4096 = 0.5904 > 0.50$. Corroboration by known
   habitual liars flips a false claim into accepted probable truth ($W > 0.5$)!

1. **Category Error (Noisy-OR vs. Sensor Evidence)**: The algebraic form $1 - \prod (1 - p_i)$ is
   the Noisy-OR gate from causal Bayesian networks. Noisy-OR models independent causal mechanisms
   triggering an effect from a zero baseline ($P(\text{effect} \mid \text{no causes}) = 0$). Here,
   sources are *observers/sensors* estimating a latent state of reality, not independent causal
   agents manufacturing the truth of the state.

**Evidence:**

- Doc lines 444–456:
  > "Under the independent evidence assumption: Let $P(F \text{ false} \mid S_i) = (1 - w_{S_i})$.
  > The joint probability that the fact is false given all corroborating sources $\mathcal{S}$ is:
  > $P(F \text{ false} \mid \mathcal{S}) = \prod_{S \in \mathcal{S}} (1 - w_S)$ ... Therefore, the
  > Consensus Corroboration Weight $W(\text{Clause})$ is: $W(\text{Clause}) = 1.0 - \prod_{S \in
  > \text{Sources}(\text{Clause})} (1.0 - w_S)$."
- Doc lines 460–462: Worked example applying Noisy-OR directly to posteriors $0.85$ and $0.70$.

**Fix:** Replace the invalid Noisy-OR formula with the exact Bayesian posterior odds update under
conditional independence of observations given the hypothesis $F \in \{0, 1\}$.

Let prior odds be $O_0 = \frac{P(F=1)}{P(F=0)}$ (default $O_0 = 1.0$ for neutral prior $P_0 =
0.5$). For each source $S_i$ with calibrated reliability $w_i = P(F=1 \mid S_i)$, the Bayes factor
(likelihood ratio) is $L_i = \frac{w_i}{1.0 - w_i}$.

The posterior odds given corroborating sources $\mathcal{S}$ are:

```math
O(F = 1 \mid \mathcal{S}) = O_0 \prod_{S \in \mathcal{S}} \frac{w_S}{1.0 - w_S}
```

In log-odds (logit) space, this is a clean additive update:

```math
\text{logit}(W) = \text{logit}(P_0) + \sum_{S \in \mathcal{S}} \ln \left( \frac{w_S}{1.0 - w_S} \right)
```

Where $W = \sigma(\text{logit}(W)) = \frac{1}{1 + e^{-\text{logit}(W)}}$.

Under this mathematically sound Bayesian formulation:

- Uninformative sources ($w = 0.5$) have $\ln(0.5 / 0.5) = 0$, contributing exactly zero change in
  belief odds.
- Trustworthy sources ($w > 0.5$) contribute positive log-odds, increasing credibility.
- Untrustworthy sources ($w < 0.5$) contribute negative log-odds, correctly penalizing credibility.

______________________________________________________________________

### 2. (§3.4, line 411) Contextual Bandit / LinUCB Misrepresentation & Out-of-Bounds Scores

Full title: Pseudo-Contextual Bandit: Formula Lacks Context Features, Violates LinUCB and Bayes-UCB
Theorems, and Escapes the Probability Simplex

**Problem:** Section 3.4 is titled *"Contextual Multi-Armed Bandits & Exploration"* and cites
*"LinUCB / Bayes-UCB"*, but provides the formula:

```math
\text{Score}_{\text{UCB}}(S) = \mu_S + c \cdot \sigma_S, \qquad \sigma_S = \sqrt{\frac{\alpha_S \beta_S}{(\alpha_S + \beta_S)^2 (\alpha_S + \beta_S + 1)}}
```

This presents three fatal mathematical and theoretical defects:

1. **Total Absence of Context Vector $\mathbf{x}$**: LinUCB (Li et al., 2010) is fundamentally a
   *contextual* bandit algorithm where the payoff expectation is linear in context features
   $\mathbf{x} \in \mathbb{R}^d$, with confidence bound $\alpha_{\text{UCB}} \sqrt{\mathbf{x}^T
   \mathbf{A}^{-1} \mathbf{x}}$ derived from the ridge regression covariance $\mathbf{A} =
   \mathbf{D}^T \mathbf{D} + \mathbf{I}$. The formula in Section 3.4 contains NO context vector
   $\mathbf{x}$, NO covariance matrix $\mathbf{A}^{-1}$, and NO context dependence. Calling this
   scalar Beta-Bernoulli formula "LinUCB" is a factual and mathematical error.

1. **Bayes-UCB Deviation**: Genuine Bayes-UCB (Kaufmann, Cappé, Garivier 2012) uses the $(1 - 1/t)$
   quantile of the Beta posterior distribution $q(\alpha_S, \beta_S, 1 - 1/t)$, NOT a Gaussian-style
   mean plus standard deviation heuristic.

1. **Violation of the Probability Simplex $[0, 1]$**: For a source with the default uninformative
   prior $\alpha = 1.0, \beta = 1.0$, the mean is $\mu = 0.5$ and standard deviation is $\sigma =
   \frac{1}{\sqrt{12}} \approx 0.2887$. For any exploration constant $c > \frac{1 - \mu}{\sigma} =
   \frac{0.5}{0.2887} \approx 1.732$ (such as standard exploration bonuses $c = 1.96$ or $c =
   2.0$):

   ```math
   \text{Score}_{\text{UCB}}(S) = 0.5 + 2.0 \times 0.2887 = 1.0774 > 1.0
   ```

   Because downstream modules pass this score directly as source weight $w_S$, having $w_S > 1.0$
   causes $(1.0 - w_S) < 0$. Feeding negative values into corroboration or evidence combination
   produces probabilities outside $[0, 1]$, corrupting the entire belief model.

**Evidence:**

- Doc lines 411–416:
  > "To balance epistemic exploitation and exploration, the framework implements Upper Confidence
  > Bound (LinUCB / Bayes-UCB): $\text{Score}_{\text{UCB}}(S) = \mu_S + c \cdot \sigma_S$"
- Doc lines 418–424: Derivation using standard Beta variance.

**Fix:**

1. Cleanly separate non-contextual Beta exploration from contextual bandits:
   - For Tier 2 Beta reputation exploration, either:
     - Adopt **Thompson Sampling**: Sample $\tilde{p}_S \sim \text{Beta}(\alpha_S, \beta_S)$.
       Thompson Sampling is theoretically optimal for Bernoulli bandits, naturally bounded in $[0,
       1]$, and eliminates the arbitrary hyperparameter $c$.
     - Or explicitly clamp the UCB score to the probability simplex:
       $\text{Score}_{\text{UCB}}(S) = \min(1.0, \mu_S + c \cdot \sigma_S)$.
1. For Tier 3 contextual bandits, either:
   - Implement authentic LinUCB by maintaining the $d \times d$ precision matrix $\mathbf{A}_S$ on
     `+d.epistemic` with confidence bound $\hat{\theta}^T \mathbf{x} + c \sqrt{\mathbf{x}^T
     \mathbf{A}_S^{-1} \mathbf{x}}$.
   - Or apply Laplace approximation / randomized perturbation to the SGD weights $\mathbf{w}_S$ for
     contextual Thompson Sampling.

______________________________________________________________________

### 3. (§5.3, line 630) Dimensional Inhomogeneity and Arbitrary Scaling in Gaussian Fusion

Full title: Dimensional Incompatibility, Unit-Sensitivity, and Fictitious Variance in Continuous
Kalman / Gaussian Fusion

**Problem:** Section 5.3 specifies continuous value fusion by equating dimensionless epistemic
weights $w_i \in [0, 1]$ directly to physical measurement inverse variance $\frac{1}{\sigma_i^2}$:

```math
\sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N w_i}
```

This violates dimensional analysis and physical statistics:

1. **Dimensional Incompatibility**: Physical coordinates $x_i$ carry physical units (e.g. meters,
   seconds, Kelvin). Variance $\sigma^2$ must carry units of $[\text{unit}]^2$, and precision
   $\frac{1}{\sigma^2}$ must carry units of $[\text{unit}]^{-2}$. Epistemic trust $w_i$ is a
   dimensionless probability or score in $[0, 1]$. Setting $\frac{1}{\sigma_i^2} = w_i$ forces an
   authentic sensor ($w = 1.0$) to have a variance of exactly $1.0$ in whatever arbitrary unit the
   user chose. If a coordinate is expressed in meters ($x = 1.0\,\text{m}$), $\sigma^2 = 1.0$. If
   expressed in millimeters ($x = 1000\,\text{mm}$), physical variance should scale by $10^6$, but
   $1/w_i$ remains $1.0\,\text{mm}^2$, distorting uncertainty estimation by six orders of magnitude.

1. **Conflation of Epistemic Trust with Sensor Measurement Noise**: Epistemic reliability $w_i$ (the
   probability that a sensor is not malfunctioning, occluded, or adversarial) is completely distinct
   from intrinsic physical measurement precision $\sigma_{i,\text{sensor}}^2$. An authentic
   ultrasonic sensor may have high trust ($w = 1.0$) but coarse precision
   ($\sigma = 0.2\,\text{m}$), while a laser rangefinder may have high precision
   ($\sigma = 0.001\,\text{m}$) but moderate trust ($w = 0.6$). Setting variance solely from trust
   assumes all sensors possess identical intrinsic noise.

1. **Zero-Weight Singularity**: If all sources for a predicate have $w_i = 0$ (or weights underflow
   due to decay), the denominator $\sum w_i = 0$, triggering division by zero (`#Div` / NaN).

**Evidence:**

- Doc lines 630–638:
  > "Treating each weight as inverse variance $\frac{1}{\sigma_i^2}$:
  > $\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N w_i x_i}{\sum_{i=1}^N w_i}$,
  > $\sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N w_i}$"

**Fix:** Decouple intrinsic sensor measurement variance from epistemic source trust. Require
continuous percept assertions to supply (or inherit from predicate calibration profiles) their
measurement variance $\sigma_{i,\text{meas}}^2$.

Epistemic trust $w_i \in (0, 1]$ acts as an information precision weight:

```math
\tau_i = \frac{w_i}{\sigma_{i,\text{meas}}^2}
```

The fused continuous estimate and fused variance are:

```math
\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N \tau_i x_i}{\sum_{i=1}^N \tau_i}, \qquad \sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N \tau_i}
```

Guarded by an epsilon threshold $\sum \tau_i > \epsilon > 0$ to prevent division by zero.

______________________________________________________________________

## MAJOR

### 4. (§3.1, line 319) Exponential Decay Conflates Information Loss with Certainty of Falsehood

Full title: Temporal Freshness Decay Drives Stale Beliefs to Certainty of Falsehood Rather Than
Maximum Entropy

**Problem:** Section 3.1 defines the Freshness Factor as $\Phi(S, \Delta t) = e^{-\lambda_S \Delta
t} \in (0, 1]$. In Section 4.4 and Section 5.1, this factor is multiplied into the belief score. As
elapsed time $\Delta t \to \infty$, $\Phi \to 0$, driving $w_S \to 0$.

In probability theory, $w_S = 0$ denotes mathematical certainty that the proposition is **false**
($P(F = 0) = 1.0$). Information degradation over elapsed time represents an increase in entropy
(loss of mutual information $I(X; Y_{\Delta t}) \to 0$), which causes beliefs to relax toward the
uninformative prior (e.g. $p \to 0.5$ or base rate $p_0$), NOT toward certainty of falsehood!

Under the current specification, if an agent observes that a valve is open ($w = 0.95$), and 1 hour
passes without new telemetry, the belief decays to $w = 0.001$. The system and downstream logic will
treat this stale belief as active $99.9\%$ certainty that the valve is *closed*!

Furthermore, the document specifies two uncoordinated time-decay mechanisms:

- Freshness factor $\Phi = e^{-\lambda \Delta t}$ applied to assertions at query time (§3.1).
- Memory retention discount $\alpha \leftarrow 1 + \gamma(\alpha - 1)$ applied to source reputation
  (§3.2). The interaction between these two temporal decays is unspecified, causing
  double-discounting of stale assertions from aging sources.

**Evidence:**

- Doc lines 319–326:
  $\Phi(S, \Delta t) = e^{-\lambda_S (t_{\text{current}} - t_{\text{asserted}})}$.
- Doc lines 356–360: $\alpha_S \leftarrow 1.0 + \gamma(\alpha_S - 1.0)$.
- Doc lines 570–574: Query lowering multiplies freshness decay and scores into consensus.

**Fix:** Formulate temporal decay as relaxation toward the maximum-entropy prior $p_0$:

```math
w_S(\Delta t) = p_0 + e^{-\lambda_S \Delta t} (w_S(0) - p_0)
```

Where $p_0 = 0.5$ (or predicate prior base rate). When $\Delta t \to \infty$, $w_S(\Delta t) \to
p_0$ (neutral / zero information), rather than converging to $0.0$ (certain falsehood). Clarify the
division of labor between assertion freshness decay and source reputation memory retention.

______________________________________________________________________

### 5. (§3, line 311; §4.4, line 569) Absence of Mathematical Multi-Tier Composition Function

Full title: Multi-Tier Epistemic Scoring Hierarchy Lacks a Unified Mathematical Formulation

**Problem:** The specification introduces four tiers of epistemic scoring (Tier 1 Temporal Decay,
Tier 2 Analytic Bayesian Reputation, Tier 3 Online SGD, Tier 4 Native ML Skills), but never provides
a mathematical formula defining how they compose into the source weight $w_S$.

In Section 4.4 (lines 570–574) and Section 5.1 (lines 595–598), the pipeline shows:

1. Evaluates $\mathbf{w}_S$
1. Calculates $\Phi(S, \Delta t)$
1. Evaluates $\sigma(\mathbf{w}_S^T \mathbf{x} + b_S)$
1. Combines weights via consensus formula

If the tiers are composed multiplicatively ($w_S = \Phi \cdot \mu_S \cdot \hat{y}_S$), confident
scores severely compound-discount each other (e.g. $0.8 \times 0.8 \times 0.8 = 0.512$, turning
three confident evaluations into a coin flip). If they are mutually exclusive models selected by
`arbitrated by truest(model: ...)`, the document contradicts itself by mandating in Section 4.4 and
Section 5.1 that all three are computed in sequence for every candidate clause.

**Evidence:**

- Doc lines 570–574: Sequential list of evaluations without a combination operator.
- Doc lines 595–600: Mermaid flowchart showing linear pipeline `Freshness --> Rep --> ML --> Corrob`
  without combination semantics.

**Fix:** Provide an explicit mathematical definition for source scoring:

1. If models are mutually exclusive, define the dispatch formula:

   ```math
   w_S = \begin{cases}
     \text{decay}(\mu_S, \Delta t, \lambda_S) & \text{if } \text{model} = \text{beta\_reputation} \\
     \text{decay}(\sigma(\mathbf{w}_S^T \mathbf{x} + b_S), \Delta t, \lambda_S) & \text{if } \text{model} = \text{online\_sgd}
   \end{cases}
   ```

1. If models form a hierarchical ensemble, provide the formal opinion-pooling formula (e.g.
   log-linear pooling or Bayesian model averaging with explicit mixture weights).

______________________________________________________________________

### 6. (§3.3, line 382) Missing Regularization, Sigmoid Clamping, and Feature Scaling in Online SGD

Full title: Unbounded SGD Weight Growth, Floating-Point Sigmoid Overflow, and Feature Scaling
Contradictions

**Problem:** The online SGD formulation:

```math
\mathbf{w}_S \leftarrow \mathbf{w}_S - \eta \cdot (\hat{y}_S - y) \cdot \mathbf{x}, \qquad b_S \leftarrow b_S - \eta \cdot (\hat{y}_S - y)
```

exhibits three numerical and statistical vulnerabilities:

1. **No L2 Regularization (Weight Explosion)**: Online SGD on binary cross-entropy with linearly
   separable context features causes $\|\mathbf{w}_S\| \to \infty$. This saturates the sigmoid
   activation ($\hat{y} \in \{0.0, 1.0\}$), causing the derivative $\hat{y}(1 - \hat{y}) \to 0$ to
   vanish and permanently freezing learning.

1. **Floating-Point Overflow in Sigmoid Evaluation**: The formula $\sigma(z) = \frac{1}{1 + e^{-z}}$
   is numerically unstable. When evaluated in pure Vortex bytecode using `#Exp`, any input $z <
   -709.78$ causes $e^{-z}$ to overflow 64-bit IEEE 754 float to `+Infinity`, resulting in
   floating-point traps or NaNs.

1. **Feature Scaling Contradiction**: Section 3.3 asserts that $\mathbf{x}$ is a "normalized feature
   vector", but Section 7.3 (Example 3, lines 818–837) feeds raw physical illuminance
   (`ambient_lux := ls.lux`, $\text{lux} = 1000$). With $\eta = 0.05$, a single update produces a
   weight step of $\Delta w = 0.05 \times 1.0 \times 1000 = 50.0$, driving $z = 50,000$ and
   triggering immediate overflow.

**Evidence:**

- Doc line 368: `\mathbf{x} = \phi(\text{context}) \in \mathbb{R}^d`
- Doc lines 379, 395: Unregularized SGD update equations.
- Doc lines 818–837: Example 3 passing raw lux values ($1000.0$).

**Fix:**

1. Add L2 weight decay to the update rule:

   ```math
   \mathbf{w}_S \leftarrow (1.0 - \eta \lambda_{\text{reg}}) \mathbf{w}_S - \eta (\hat{y}_S - y) \mathbf{x}
   ```

1. Clamp logit activations to $[-30.0, 30.0]$ before `#Exp` and use branch-free stable evaluation:

   ```math
   \sigma(z) = \begin{cases} \frac{1}{1 + e^{-z}} & \text{if } z \ge 0 \\ \frac{e^z}{1 + e^z} & \text{if } z < 0 \end{cases}
   ```

1. Specify an online running Welford normalizer
   $\tilde{x}_i = (x_i - \bar{x}_i) / (\hat{\sigma}_i + \epsilon)$ in `std:epistemic`, or clamp
   features to $[0.0, 1.0]$.

______________________________________________________________________

### 7. (§1.1, §3.2, §7.1) Continuous Percepts vs. Discrete Bernoulli Verification

Full title: Continuous Metric Coordinates Modeled as Discrete Bernoulli Trials Without Neighborhood
Tolerances

**Problem:** Predicates such as `at(who, x, y)` operate over continuous spatial domains
$\mathbb{R}^2$. Under continuous probability measures, the probability of an exact coordinate match
between two independent sensors is zero ($\mathbb{P}(X_1 = X_2) = 0$).

However:

1. Section 3.5's corroboration logic requires sensors to assert the "exact same fact", which almost
   surely never occurs for floating-point sensor percepts.
1. Section 3.2 and Section 6.2 model truth evaluation as a binary Bernoulli trial ($y \in \{0, 1\}$)
   via `confirm` and `refute`.

For continuous physical measurements, an assertion is never exactly true ($y = 1$); it exhibits
residual error $\| \mathbf{x}_{\text{obs}} - \mathbf{x}_{\text{true}} \|$. Without an epsilon metric
or tolerance radius, physical sensor verifications will almost surely fail exact equality, marking
all sensors as refutations ($y = 0$) and causing their Beta reputations $\alpha / (\alpha +
\beta)$ to collapse to zero.

**Evidence:**

- Doc lines 128–132: Coexistence of continuous floating-point assertions `at("rover", 10.0, 20.0)`
  vs. `(10.2, 20.1)`.
- Doc line 442: Corroboration condition requiring the "exact same fact".
- Doc lines 759–765: Binary `confirm` / `refute` on floating-point coordinates.

**Fix:**

1. Define an epistemic neighborhood metric for continuous predicates: two assertions corroborate if
   $\| \mathbf{x}_1 - \mathbf{x}_2 \| \le \epsilon_{\text{tol}}$.

1. Extend outcome verification to accept continuous likelihood rewards:

   ```math
   y = \exp\left(-\frac{\|\mathbf{x}_{\text{asserted}} - \mathbf{x}_{\text{ground\_truth}}\|^2}{2 \sigma_{\text{tol}}^2}\right) \in [0.0, 1.0]
   ```

   Updating Beta parameters continuously: $\alpha \leftarrow \alpha + y$, $\beta \leftarrow \beta +
   (1.0 - y)$.

______________________________________________________________________

## MINOR

### 8. (§5.4, line 648) KaTeX Formatting and Hygiene Defects Across Document

Full title: Line-Split Inline Math, Non-Standard Beta Function Notation, Sloppy Probability Event
Syntax, and Float Literals in Display Math

**Problems & Fixes:**

1. **Line-Split Inline Math** (§5.4, lines 648–649): `$\mathit{dense} \ge` is split across lines
   with indentation before `m_0.\mathit{cellCount}$.`. This risks rendering failures in strict
   CommonMark/KaTeX parsers. Furthermore, C++ arena variables (`dense`, `m_0.cellCount`) must not be
   set in LaTeX math mode; replace with backtick code spans: `` `dense >= m_0.cellCount` ``.
1. **Non-Standard Beta Function Notation** (§3.2, line 340): `\text{B}(\alpha_S, \beta_S)` renders
   upright in text font; replace with standard mathematical Roman `\mathrm{B}(\alpha_S, \beta_S)` or
   italic $B(\alpha_S, \beta_S)$.
1. **Improper Probability Event Syntax** (§3.5, lines 445, 449):
   `P(F \text{ false} \mid \mathcal{S})` places adjacent terms without a connective, resembling
   multiplication. Replace with standard notation $P(\neg F \mid \mathcal{S})$ or $P(F = 0 \mid
   \mathcal{S})$.
1. **Floating-Point Literals in Display Equations** (§3.5, lines 455, 460):
   `W(\text{Clause}) = 1.0 - \prod ... (1.0 - w_S)` uses programming float literal `1.0` in
   theoretical display equations; replace with mathematical integer $1$: $W = 1 - \prod(1 - w_S)$.
1. **Misleading Flowchart Node Labeling** (§6.2, line 683): Mermaid node `Stage Penalty (-1.0)`
   implies subtracting $1.0$ from Beta parameters, which would corrupt the prior. Clarify node text
   to `Increment Refutations (beta += 1.0)`.

______________________________________________________________________

VERDICT: NOT CONVERGED
