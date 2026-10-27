# Round 1 critique: ML Expert (design/vagent-dynamic-source-weighting.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 7 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§3.4; §8.3) Conflation of Uncontextual Beta-UCB with LinUCB

Full title: Conflation of Uncontextual Beta-UCB with Contextual LinUCB and Missing Contextual
Epistemic Uncertainty

**Problem:** Section 3.4 is titled "Contextual Multi-Armed Bandits & Exploration" and claims to
implement LinUCB for epistemic exploration vs. exploitation. However, the mathematical formulation
provided is strictly non-contextual Beta-UCB / Bayes-UCB; it contains zero context terms
$\mathbf{x}$ and zero covariance tracking. Furthermore, when `model: online_sgd` is selected (§4.2,
line 527), online SGD generates only a deterministic point estimate $\mathbf{w}_S$ with no posterior
distribution or epistemic variance $\sigma_S(\mathbf{x})$, making the specified exploration bonus
mathematically undefined.

**Evidence:** In §3.4 (lines 414–420), the formula given for UCB is:

```math
\text{Score}_{\text{UCB}}(S) = \mu_S + c \cdot \sigma_S
```

Where:

```math
\sigma_S = \sqrt{\frac{\alpha_S \beta_S}{(\alpha_S + \beta_S)^2 (\alpha_S + \beta_S + 1)}}
```

This is the exact variance of the uncontextual Beta distribution $\text{Beta}(\alpha_S, \beta_S)$
from Tier 2. It has no dependence on the context vector $\mathbf{x} \in \mathbb{R}^d$.

In §8.3 (Proposal P19.3, line 917): "Implement online SGD logistic regression and LinUCB scoring in
`std:epistemic`... `epistemic_ucb_score(mu, sigma, c) -> mu + c * sigma`."

In §4.2 (line 527):
`predicate at(...) arbitrated by truest(decay: 2.5s, model: online_sgd, exploration: 0.15)`.

True LinUCB (Li et al., 2010; Chu et al., 2011) evaluates an arm $S$ in context $\mathbf{x}$ using
ridge regression parameters $\hat{\boldsymbol{\theta}}_S = \mathbf{A}_S^{-1} \mathbf{b}_S$ and
computes the Upper Confidence Bound via:

```math
\text{Score}_{\text{LinUCB}}(S, \mathbf{x}) = \mathbf{x}^T \hat{\boldsymbol{\theta}}_S + \alpha \sqrt{\mathbf{x}^T \mathbf{A}_S^{-1} \mathbf{x}}
```

Where $\mathbf{A}_S \in \mathbb{R}^{d \times d}$ is the regularized feature covariance:

```math
\mathbf{A}_S = \mathbf{I}_d + \sum_{\tau} \mathbf{x}_\tau \mathbf{x}_\tau^T
```

The design provides no covariance matrix $\mathbf{A}_S^{-1}$, no matrix inversion in Vortex
bytecode, and no bridge connecting the contextual logistic prediction $\hat{y}_S(\mathbf{x})$ to the
variance $\sigma_S$. If $\sigma_S$ is pulled from the non-contextual Beta prior while $\mu_S$ comes
from SGD, the exploration term is completely blind to context novelty, violating the core regret
bounds of contextual bandits ($\tilde{O}(d\sqrt{T})$).

**Fix:**

1. Explicitly rename Section 3.4 and the primitive `epistemic_ucb_score` to **Bayes-UCB** (or
   **Beta-UCB**), clarifying that Eq. 415 operates over the non-contextual Tier 2 Beta-Bernoulli
   prior.
1. For contextual exploration under `model: online_sgd`:
   - Either adopt **Diagonal Laplace Approximation / Logistic UCB**: Maintain a diagonal feature
     variance vector $\mathbf{v}_S \in \mathbb{R}^d$ on `+d.vars` updated via online Fisher
     information / gradient outer products, yielding contextual variance
     $\sigma_S^2(\mathbf{x}) = \sum_{j=1}^d x_j^2 v_{S, j}$, giving
     $\text{Score}(S, \mathbf{x}) = \hat{y}_S(\mathbf{x}) + c \sqrt{\sum x_j^2 v_{S, j}}$.
   - Or adopt **Contextual Thompson Sampling with Softmax Gating**: Draw sampled logits
     $\tilde{z}_S \sim \mathcal{N}(\mathbf{w}_S^T \mathbf{x} + b_S, \tau_S^2)$ or sample
     $\theta_S \sim \text{Beta}(\alpha_S, \beta_S)$ for Tier 2, which avoids matrix inversions,
     eliminates the need to tune an arbitrary exploration constant $c$, and achieves optimal
     Bayesian regret bounds.
1. Update Proposal P19.3 to specify the exact algorithm: Bayes-UCB for Tier 2, and diagonal variance
   / Thompson Sampling for Tier 3.

______________________________________________________________________

### 2. (§5.3) Dimensionless Epistemic Trust Conflated with Sensor Inverse Variance

Full title: Dimensionless Epistemic Trust Directly Equated to Physical Inverse Variance in Gaussian
Fusion Without Units or Outlier Gating

**Problem:** Section 5.3 specifies continuous value fusion by declaring that epistemic trust weights
$w_1, \dots, w_N$ are directly treated as measurement inverse variance $\frac{1}{\sigma_i^2}$:

```math
\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N w_i x_i}{\sum_{i=1}^N w_i}, \qquad \sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N w_i}
```

This conflation suffers from three fatal mathematical, physical, and algorithmic defects:

1. **Dimensional Incompatibility**: $w_i \in [0, 1]$ is a dimensionless epistemic probability or
   confidence score. Physical measurement variance $\sigma_i^2$ has units
   $[\text{measurement\_unit}]^2$ (e.g. $\text{meters}^2$, $\text{degrees}^2$, $\text{Kelvin}^2$).
   Directly setting $\frac{1}{\sigma_i^2} = w_i$ is dimensionally invalid.
1. **Artificial Precision Cap**: Because $w_i \le 1.0$, the assumption $\frac{1}{\sigma_i^2} = w_i$
   implies that $\sigma_i^2 \ge 1.0$ for all sensors! If a high-precision sensor has
   centimeter-level accuracy ($\sigma_i = 0.01\text{ m} \implies \sigma_i^2 = 10^{-4}\text{ m}^2$,
   precision $\frac{1}{\sigma_i^2} = 10,000$), this formula artificially caps its precision at
   $1.0$, distorting sensor precision by four orders of magnitude.
1. **Catastrophic Outlier Sensitivity (0% Breakdown Point)**: Simple weighted arithmetic averaging
   has a breakdown point of $0\%$. If two healthy sensors assert $(10.0, 20.0)$ with $w = 0.85$ and
   a faulty sensor asserts $(1000.0, 2000.0)$ with $w = 0.20$, the fused coordinates become
   $(\approx 114, \approx 228)$, pulling the agent into a completely fictitious obstacle/location.
   Physical Kalman and Bayesian sensor fusion requires innovation gating ($\chi^2$ / Mahalanobis
   distance validation gate) before fusion.

**Evidence:** In §5.3 (lines 629–638): "Treating each weight as inverse variance
$\frac{1}{\sigma_i^2}$:
$\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N w_i x_i}{\sum_{i=1}^N w_i}$,
$\sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N w_i}$"

Example 1 (§7.1, lines 724–749): Coordinates are $(x, y) \in \mathbb{R}^2$ in meters, yet the fusion
formula in §5.3 is strictly scalar and treats confidence weights as inverse variances.

**Fix:**

1. Reformulate continuous sensor fusion using proper **Covariance Inflation & Sensor Precision**:
   Each continuous sensor profile must record its nominal physical measurement variance
   $\sigma_{0, S}^2$ (in physical units squared) on `+d.vars`. The effective precision $\omega_i$ in
   sensor fusion is:

   ```math
   \omega_i = \frac{w_i}{\sigma_{0, i}^2}
   ```

   Where $w_i \in [0, 1]$ acts as a dimensionless reliability gating weight. When $w_i \to 0$,
   $\omega_i \to 0$ (the faulty sensor is discounted to zero precision). When $w_i \to 1$, the
   sensor retains its calibrated physical precision $\frac{1}{\sigma_{0, i}^2}$.

1. Add **Innovation Gating / Outlier Rejection**: Before computing the weighted consensus, candidate
   assertions must pass an innovation gating filter (e.g. Mahalanobis distance or median absolute
   deviation):

   ```math
   \frac{\|x_i - \text{med}(\mathbf{x})\|}{\sigma_{0, i}} \le \gamma_{\text{gate}} \quad (\text{e.g. } \gamma_{\text{gate}} = 3.0)
   ```

   Candidates outside the $3\sigma$ gate are rejected as physical outliers before computing
   $\hat{x}_{\text{consensus}}$.

1. Support multi-dimensional vector fusion: Extend the formula to $k$-dimensional vectors
   $\mathbf{x}_i \in \mathbb{R}^k$ (e.g., $(x, y, z)$ coordinates) by applying the gated inverse
   variance fusion component-wise or via diagonal covariance matrices.

______________________________________________________________________

### 3. (§8.2; §8.1) Hallucinated `#Exp` and Missing Math Opcodes in Vortex VM

Full title: Contradiction Between "Zero New VM Opcodes" and Reliance on Nonexistent `#Exp`, `#Ln`,
`#Sqrt` Opcodes in Vortex VM

**Problem:** Proposal P19.2 (line 906) explicitly claims that the analytical Bayesian reputation and
decay engine in `std:epistemic` is:

> "Implemented in standard Vortex bytecode instructions (`#Add`, `#Sub`, `#Mul`, `#Div`, `#Exp`)."

Simultaneously, Proposal P19.1 (line 885) claims:

> "Zero new VM opcodes."

However, inspection of the codebase in `apps/common/xanadu/vortex/vortex_vm.hpp` (lines 27–91) and
`vortex_vm.cpp` (lines 366–530) confirms that `OpcodeKind` contains **NO `#Exp` opcode**. The only
arithmetic and math opcodes implemented in the VM are: `Add`, `Sub`, `Mul`, `Div`, `DivMod`, `Mod`,
`Neg`, `Abs`, `Min`, `Max`, `Clamp`. There is no `#Exp`, no `#Ln` (required for binary cross-entropy
and half-life decay $\lambda = \frac{\ln 2}{\tau}$), and no `#Sqrt` (required for standard deviation
$\sigma_S = \sqrt{\dots}$ in UCB).

Because the mathematical formulation in §3.1 ($e^{-\lambda \Delta t}$), §3.3
($\sigma(z) = \frac{1}{1 + e^{-z}}$), and §3.4 ($\sigma = \sqrt{\dots}$) relies on transcendental
functions, claiming pure-Vortex bytecode execution with "Zero new VM opcodes" is an unexecutable
contradiction.

**Evidence:** In `apps/common/xanadu/vortex/vortex_vm.hpp` lines 27–91:

```cpp
enum class OpcodeKind : std::uint8_t {
  // Arithmetic
  Add, Sub, Mul, Div, DivMod, Mod, Neg,
  // Relational & Logic ...
  // Manifold Structural Primitives ...
  // Control Flow ...
  // Math & String Extensions
  Abs, Min, Max, Clamp, Trim, ToLower, ToUpper,
  // Vlog Logic Programming Extensions ...
  ChoiceDepth, Point,
};
```

`OpcodeKind::Exp`, `OpcodeKind::Ln`, and `OpcodeKind::Sqrt` do not exist. Yet
`design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` line 885 claims "Zero new VM
opcodes", while line 906 explicitly cites "`#Exp`".

**Fix:** Resolve this contradiction by adopting one of two concrete engineering paths in the
Proposal Register:

- **Option A (Add VM Opcodes in Proposal Register)**: Amend the Proposal Register with a new
  proposal modifying `apps/common/xanadu/vortex/vortex_vm.hpp` and `vortex_vm.cpp` to add
  `OpcodeKind::Exp`, `OpcodeKind::Ln`, and `OpcodeKind::Sqrt` to the VM, pricing the opcode
  additions and citing their dispatch overhead.
- **Option B (Pure Bytecode Polynomial Approximations via Padé / Horner)**: Keep "Zero new VM
  opcodes" and specify that `std:epistemic` evaluates transcendental functions using fast, clamped
  polynomial approximations over bounded domains (e.g. 5th-order Padé approximant or Remez minimax
  polynomial for $\sigma(z)$ over $z \in [-8, 8]$, clamped outside; fast reciprocal square root via
  Newton-Raphson iterations). Option B is strongly recommended to preserve Vortex VM minimalism and
  avoid altering `OpcodeKind` serialization.

______________________________________________________________________

## MAJOR

### 4. (§3.3; §7.3) Unregularized Online SGD with Unscaled Raw Features

Full title: Lack of Streaming Feature Normalization and $L_2$ Regularization in Online SGD Leading
to Floating-Point Overflow

**Problem:** Section 3.3 formulates online logistic regression with no feature normalization logic
and no regularization ($L_2$ weight decay).

1. **Raw Feature Scaling Explodes Gradients**: While line 368 mentions a "normalized feature
   vector", Example 3 (§7.3, lines 818–838) passes raw unnormalized physical variables:
   `var ambient_lux: Float := 100.0`, with prose noting lux ranges from $0.5$ to $1000.0$. If an
   agent processes $\text{lux} = 1000.0$ with error residual $e = (\hat{y} - y) = 1.0$ and learning
   rate $\eta = 0.05$, the single-step weight update is:

   ```math
   \Delta w = - 0.05 \times 1.0 \times 1000.0 = -50.0
   ```

   On the very next turn, the logit $z = w \cdot x = -50.0 \times 1000.0 = -50,000.0$. Evaluating
   $e^{-z} = e^{50000}$ immediately overflows IEEE 754 float exponent, permanently driving
   $\sigma(z) \to 0.0$. The model enters saturation, gradients vanish, and learning freezes.

1. **Unbounded Weights on Separable Data**: Without $L_2$ regularization, if a sensor is
   consistently truthful ($y=1$) under certain conditions, unregularized logistic regression drives
   $\|\mathbf{w}\| \to \infty$.

**Evidence:** In §3.3 (lines 394–400):
$\mathbf{w}_S \leftarrow \mathbf{w}_S - \eta \cdot (\hat{y}_S - y) \cdot \mathbf{x}$. No weight
decay is included. In §7.3 (lines 818–838): Raw `ambient_lux` is passed directly into online
learning without scaling.

**Fix:**

1. **Online Streaming Normalization**: Specify that context vectors must pass through an online
   standardizer (using Welford's streaming algorithm on `+d.epistemic` maintaining running mean
   $\mu_j$ and variance $\sigma_j^2$) to standardize
   $x_j \leftarrow \text{clamp}\left(\frac{x_j - \mu_j}{\sigma_j + \epsilon}, -3.0, 3.0\right)$, or
   require explicit declarative min/max ranges in the predicate arbitration policy.

1. **Coupled Weight Decay ($L_2$ Regularization)**: Add weight shrinkage to the SGD update:

   ```math
   \mathbf{w}_S \leftarrow (1.0 - \eta \lambda_{\text{reg}}) \mathbf{w}_S - \eta (\hat{y}_S - y) \mathbf{x}
   ```

   Where $\lambda_{\text{reg}} \in [10^{-4}, 10^{-2}]$.

1. **Logit Clamping**: Explicitly clamp logits
   $z_S = \text{clamp}(\mathbf{w}_S^T \mathbf{x} + b_S, -15.0, 15.0)$ before evaluating the sigmoid,
   guaranteeing numerical stability in IEEE 754 float.

______________________________________________________________________

### 5. (§3.3; §6.3) Constant Learning Rate Ignores Robbins-Monro Conditions

Full title: Constant Learning Rate ($\eta$) Violates Robbins-Monro Conditions and Fails on
Multi-Rate Features

**Problem:** Section 6.3 line 705 only states that "$\eta$ is capped at $0.05$ per update".

1. A fixed constant learning rate violates the fundamental Robbins-Monro conditions
   ($\sum \eta_t = \infty, \sum \eta_t^2 < \infty$). In stationary or slowly-drifting environments,
   a constant $\eta$ prevents convergence to the empirical risk minimizer, causing perpetual
   limit-cycle oscillation around the optimum.
1. Different context features arrive at vastly different frequencies (e.g. continuous velocity
   updates every turn vs. rare emergency flags occurring once per 1,000 turns). Pure SGD with a
   scalar learning rate under-trains infrequent features and causes frequent features to oscillate.
1. The Source Profile topology in Table 2.2 explicitly allocates `$obs_count` ($N_S$) on `+d.vars`,
   but the mathematical formulation never uses it.

**Evidence:** In §6.3 (line 705): "Learning Rate Bounds: $\eta$ is capped at $0.05$ per update". In
§2.2 (Table line 282): `$obs_count (Int)` total number of assertions received from this source.

**Fix:**

1. Formulate a learning rate schedule tied to `$obs_count` ($N_S$):

   ```math
   \eta_t = \frac{\eta_0}{\sqrt{N_S + 1}} \quad \text{with } \eta_0 = 0.05
   ```

1. For streaming multi-rate features, specify per-coordinate adaptive learning rates via streaming
   AdaGrad: Maintain an accumulated squared gradient vector $\mathbf{G}_S \in \mathbb{R}^d$ on
   `+d.vars`:

   ```math
   G_{S, j} \leftarrow G_{S, j} + g_j^2, \qquad \eta_{t, j} = \frac{\eta_0}{\sqrt{G_{S, j} + \epsilon}}
   ```

   This gives rare features larger updates when they appear and dampens frequent features to ensure
   convergence.

______________________________________________________________________

### 6. (§6.2) Physical Plan Execution Failures Confounded with Epistemic Refutation

Full title: Confounding Physical Execution and Timeout Failures with Belief Source Refutation
(Credit Assignment Flaw)

**Problem:** Section 6.2 states that when an intention completes, if the plan fails due to "Physical
Failure / Timeout", it issues a "Stage Penalty (-1.0) for Token Sources" which triggers a full
refutation ($y = 0.0$) against all contributing belief sources. In autonomous robotics and
multi-agent systems, plan failure frequently occurs due to non-epistemic reasons: an actuator
stalls, a wheel slips, an unmapped obstacle blocks the path, battery runs out, or execution times
out. The source's asserted belief (e.g. `at("waypoint", 10.0, 20.0)`) may be 100% factually
accurate, but the agent failed to get there. Penalizing the asserting source with $y=0$ injects
massive false-negative label noise into the training data. In difficult physical environments, this
causes the agent to systematically conclude that its sensors and peers are untrustworthy, degrading
reliable sources.

**Evidence:** In §6.2 (lines 678–684): `PlanFail` for physical failure/timeout routes directly to
`NegativeReward` (-1.0 penalty). In §6.2 (lines 698–700): "If the plan navigates to (x, y) and
detects an empty void, the sources asserting that location receive a refutation ($y = 0$)."

**Fix:** Strictly decouple **physical plan execution failure** from **epistemic belief refutation**:

1. Plan timeout, actuator exception, or path obstruction must be classified as `ExecutionFailure`,
   which issues **zero epistemic updates** to the belief sources (neutral outcome, no penalty).
1. A negative update ($y = 0$) must be issued **only** upon explicit epistemic contradiction:
   - When a plan reaches the target state and an explicit postcondition or verification assertion
     fails (as shown in Example 1 lines 762–765: `refute at(...)`).
   - When an authoritative ground-truth percept directly contradicts the asserted clause.
1. Update the state machine diagram in §6.2 to route physical timeouts and failures to
   `Neutral ["No Update"]`, reserving `NegativeReward` exclusively for verified epistemic
   refutations.

______________________________________________________________________

### 7. (§6.1; §8.5) Single Provenance Token Incapable of Multi-Belief Attribution

Full title: Single Provenance Token on Intention Frame Incompatible with Multi-Belief Plans

**Problem:** Section 6.1 specifies that an `EpistemicProvenanceToken` is minted on the intention
frame with fields `kTokenClause`, `kTokenSources`, and `kTokenContext`. However, real BDI plans
regularly query multiple distinct beliefs:

```vagent
where: believes truest robot_pose(let rx, let ry),
       believes truest weather_conditions(let w),
       believes truest target_location(let tx, let ty)
```

Allocating a single scalar token slot means that each subsequent `believes truest` query either
overwrites previous tokens, or only the final query receives feedback. If all evaluated sources are
lumped into a single list, credit/blame assignment fails: if the weather prediction was wrong but
the robot pose was correct, both the pose sensor and the weather station are penalized equally
($y = 0$).

**Evidence:** In §6.1 (lines 663–667): Defines fields `kTokenClause`, `kTokenSources`,
`kTokenContext` as single records. In §8.5 (Proposal P19.5, lines 960–961): "Overhead: 1 token cell
\+ 3 link cells per active intention frame".

**Fix:** Represent epistemic provenance as a rank of tokens along `+d.epistemic` off the intention
frame: Each queried belief clause mints its own token cell linking its specific clause, sources,
context, and the specific query goal symbol (`robot_pose`, `target_location`). At turn settle,
outcome attribution evaluates which specific belief postcondition was verified or contradicted,
updating only the relevant token's sources.

______________________________________________________________________

### 8. (§3.5) Corroboration Assumes Independence, Ignores Data Incest and Float Jitter

Full title: Multi-Source Corroboration Violates Conditional Independence and Fails Under Float
Jitter

**Problem:** Section 3.5 models multi-source corroboration using independent evidence combination:

```math
W(\text{Clause}) = 1.0 - \prod_{S \in \mathcal{S}} (1.0 - w_S)
```

1. **Violation of Conditional Independence (Data Incest / Rumor Propagation)**: In multi-agent
   swarms, agents forward percepts to one another. If Peer B forwards Peer A's report to Peer C,
   treating A and B as independent causes unwarranted probability inflation ($W \to 1.0$).
1. **Prior Odds Disregarded**: The formula assumes uniform prior odds ($1:1$). Proper Bayesian
   combination requires multiplying likelihood ratios $\frac{P(S_i \mid F)}{P(S_i \mid \neg F)}$
   scaled by the prior odds $\frac{P(F)}{P(\neg F)}$.
1. **Float Jitter Prevents Corroboration**: In autonomous sensory systems, two sensors measuring the
   same physical phenomenon will assert slightly different continuous floats (e.g.
   `(10.0001, 20.0002)` vs. `(10.0000, 20.0000)`). Because Vlog §6 Horn clauses in P18 unify via
   exact structural `#Unify`, these percepts are stored as two distinct clauses on `+d.clause`. The
   formula in §3.5 only combines sources that assert the *exact same clause cell*. Consequently,
   continuous sensor corroboration will **never trigger** in practice!

**Evidence:** In §3.5 (lines 442–456): Assumes independent evidence and exact matching on Clause 1.
P18 §2.3 (Decision `U-B2`): Conflicting ground facts coexist as distinct clauses along `+d.clause`.

**Fix:**

1. Clarify that the exact formula in §3.5 applies to **discrete symbolic ground facts** (e.g.
   `obstacle("sector_7")`, `leader("scout_1")`).
1. For continuous attributes, specify that corroboration occurs via **spatial clustering /
   $\epsilon$-ball tolerance**: Assertions within a Euclidean distance threshold $\epsilon$ (or
   matching key bins) are clustered into a single equivalence class before corroboration.
1. Add a source origin graph or damping factor $\rho \in (0, 1)$ to downweight corroborating reports
   originating from the same peer neighborhood to prevent data incest.

______________________________________________________________________

### 9. (§3.2; §3.3) Underspecified Memory Decay and Lack of Drift Tracking in SGD

Full title: Underspecified Rolling Horizon Trigger in Beta Priors and Absence of Concept Drift
Tracking in SGD Weights

**Problem:**

1. Section 3.2 line 357 states: "To prevent early observations from dominating forever, a retention
   discount $\gamma \in (0.9, 1.0)$ is applied periodically:
   $\alpha_S \leftarrow 1.0 + \gamma(\alpha_S - 1.0)$". The document never specifies what triggers
   this periodic application. If applied every turn in an actor running at 60 Hz, $\alpha$ decays to
   $1.0$ within seconds ($\gamma^{60} \approx 0.05$). If applied only upon receiving an assertion,
   idle sources never decay while bursty sources decay instantly.
1. In Section 3.3, learned weights $\mathbf{w}_S$ have no mechanism to handle concept drift. When an
   environment undergoes a non-stationary shift (e.g. seasonal change, sensor lens smudge, permanent
   odometry wheel wear), previously learned weights persist indefinitely, preventing adaptation.

**Evidence:** In §3.2 (line 357): "applied periodically:
$\alpha_S \leftarrow 1.0 + \gamma(\alpha_S - 1.0)$". In §3.3: No concept drift detection or weight
decay schedule in online SGD.

**Fix:**

1. **Time-Consistent Continuous Beta Discounting**: Replace ambiguous "periodic" discounting with
   exponential elapsed-turn discounting evaluated lazily upon access:

   ```math
   \Delta t = t_{\text{current}} - t_{\text{last\_seen}}
   ```

   ```math
   \alpha_S(t) \leftarrow 1.0 + (\alpha_S(t_{\text{prev}}) - 1.0) \cdot e^{-\lambda_{\text{drift}} \Delta t}
   ```

   ```math
   \beta_S(t) \leftarrow 1.0 + (\beta_S(t_{\text{prev}}) - 1.0) \cdot e^{-\lambda_{\text{drift}} \Delta t}
   ```

1. **Concept Drift Adaptation in SGD**: Introduce an online drift detector (e.g. Page-Hinkley test
   on prediction error residuals $e = |\hat{y}_S - y|$) or continuous weight decay
   $\mathbf{w}_S \leftarrow \mathbf{w}_S \cdot (1 - \lambda_w \Delta t)$ so that older weights decay
   gracefully in the absence of reinforcement.

______________________________________________________________________

### 10. (§7.4; §8.6) Underspecified Native ML Skill Interface and Batched Inference

Full title: Lack of Tensor/Embedding Representation, Standardization, and Batched Evaluation in
Native ML Skill Interface

**Problem:** Proposal P19.6 describes integrating external neural runtimes (ONNX Runtime, TensorRT,
llama.cpp) via `native skill`. However, the specification lacks:

1. **Tensor / Embedding Representation**: It is completely unspecified how high-dimensional
   embeddings or percept tensors are stored in `ArenaManifold`. Are they linked lists of Float
   cells, raw IEEE 754 byte buffers in R6 scalar nodes, or external handle IDs?
1. **Scoring Metric**: Is arbitration performed via cosine similarity, inner product, or
   cross-encoder logit?
1. **Catastrophic Sequential Turn Latency**: If evaluating `believes truest P(...)` over $K$
   candidate clauses requires invoking the native skill $K$ times sequentially—each time yielding
   the turn via `haltOp_` (P18.6)—evaluating a single query requires $K$ turn resumptions and $K$
   isolated single-sample tensor inferences, causing massive latency and destroying throughput.
1. **Signature Inconsistency**: Proposal P19.6 names the prelude capacity `EpistemicModel`, but
   Example 4 (§7.4) invents `SceneVerifier` with an ad-hoc method
   `verify_scene_plausibility(image_ref: CellRef, candidate_text: Str): Float`. The compiler cannot
   lower declarative arbitration policies without a standardized capacity ABI.

**Evidence:** In §8.6 (Proposal P19.6, lines 973–980): "Define the `EpistemicModel` capacity in
prelude... Native invocation yields the turn via haltOp\_". In §7.4 (lines 846–854):
`capacity SceneVerifier { def verify_scene_plausibility(...) }`.

**Fix:**

1. Define the canonical prelude capacity in §8.6:

   ```vagent
   capacity EpistemicModel {
     def score_candidates(context_ref: CellRef, candidate_clauses: List<CellRef>): List<Float>
   }
   ```

1. Specify that native neural arbitration evaluates candidates in a **single batched call**: All
   candidate clauses are collected and passed simultaneously to the native skill, requiring only
   **one** turn yield via `haltOp_` and enabling SIMD/GPU tensor batching in ONNX / TensorRT.

1. Specify embedding storage: Embeddings are stored as contiguous IEEE 754 binary buffers in R6
   scalar blob cells, and similarity is computed as normalized dot product (cosine similarity).

______________________________________________________________________

## MINOR

### 11. (§3.3, line 388) Numerical Instability in Direct Binary Cross-Entropy Evaluation

**Problem:** Section 3.3 writes the loss as
$\mathcal{L} = - (y \ln \hat{y}_S + (1 - y) \ln(1 - \hat{y}_S))$. Directly evaluating $\ln \hat{y}$
when $\hat{y} \to 0$ or $\ln(1 - \hat{y})$ when $\hat{y} \to 1$ produces $-\infty$ or NaN in
floating-point arithmetic. **Fix:** Specify that during runtime, only the error residual
$e = (\hat{y}_S - y)$ is computed directly. If the loss $\mathcal{L}$ is evaluated for
logging/convergence monitoring, it must use the numerically stable log-sum-exp formulation:
$\mathcal{L} = \max(z_S, 0) - z_S y + \ln(1 + e^{-|z_S|})$.

### 12. (§3.4, lines 411–425) Absence of Thompson Sampling as an Alternative to UCB

**Problem:** Contextual and multi-armed bandit literature consistently demonstrates that Thompson
Sampling has lower empirical regret and fewer hyperparameter tuning issues than UCB (which is highly
sensitive to the exploration constant $c$). **Fix:** Mention Thompson Sampling as an approved
exploration policy: `arbitrated by truest(model: beta_reputation, exploration: thompson_sampling)`.
For Beta-Bernoulli, the engine simply draws $\tilde{p}_S \sim \text{Beta}(\alpha_S, \beta_S)$ and
chooses the argmax.

### 13. (§2.2, Table line 282; §3.1–§3.4) Unused Profile Field `$obs_count`

**Problem:** Table 2.2 allocates `$obs_count (Int)` on `+d.vars`, but it is never referenced
anywhere in the mathematical formulations in §3 or §5. **Fix:** Explicitly connect `$obs_count` to:

- Learning rate decay schedule: $\eta_t = \frac{\eta_0}{\sqrt{\text{\$obs\_count} + 1}}$.
- Burn-in / warm-up threshold: A source must have $\text{\$obs\_count} \ge 10$ before online SGD
  contextual gating activates over Tier 2 Beta reputation.

______________________________________________________________________

VERDICT: NOT CONVERGED
