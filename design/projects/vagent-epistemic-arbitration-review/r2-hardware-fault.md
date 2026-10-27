# Round 2 critique: Hardware Fault Specialist (epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- **H1.1 (BLOCKING - Epistemic Weight Equated to Inverse Variance in Gaussian Fusion)**: **RESOLVED
  in mathematical formulation; operational ingestion gap remains (see H2.4)**. The invalid
  substitution of dimensionless trust $w_i \in [0.0, 1.0]$ for physical inverse variance
  $\frac{1}{\sigma_i^2}$ has been excised. Section 5.3 now correctly decouples physical noise
  covariance $R_i$ from dimensionless trust $w_i$, using trust as a covariance inflation penalty
  $\tilde{R}_i = R_i / \max(w_i, \epsilon_{\text{floor}})$ and implementing optimal Kalman fusion
  $\hat{x}_{\text{consensus}} = (\sum \tilde{R}_i^{-1})^{-1} \sum \tilde{R}_i^{-1} x_i$.
- **H1.2 (BLOCKING - Gaussian Fusion Lacks Innovation Gating and Outlier Rejection)**: **RESOLVED**.
  Section 5.3 now explicitly mandates Chi-squared ($\chi^2$) Mahalanobis innovation gating
  $d_M^2 = (x_i - \hat{x}_0)^T (\tilde{R}_i + P_0)^{-1} (x_i - \hat{x}_0) \le \chi^2_{\alpha, k}$
  prior to fusion. Outliers failing the gate are rejected, incrementing the source's
  `$reject_count`. Multi-modal or irreconcilable candidate clusters refuse Gaussian fusion and
  preserve distinct disjunctive hypotheses.
- **H1.3 (BLOCKING - Continuous Percepts Break Multi-Source Corroboration Formula)**: **RESOLVED**.
  Section 3.5, §4.4, §5.1, and §8.2 now incorporate continuous metric tolerance clustering
  ($\epsilon_{\text{tol}}$), clustering continuous clauses within
  $\|\mathbf{K}(C_1) - \mathbf{K}(C_2)\|_2 \le \epsilon_{\text{tol}}$ into candidate equivalence
  pools before Bayesian log-odds accumulation or Gaussian fusion.
- **H1.4 (MAJOR - Binary Outcome Backpropagation Causes Credit Misattribution)**: **RESOLVED**.
  Section 1.4 (item 5), §1.5 (`U-C3`), and §6.2 formally decouple plan goal outcome
  ($y \in \{0, 1\}$) from sensory attribution, eliminating the Gettier problem. Sensory updates are
  driven exclusively by explicit verification directives (`confirm`, `refute`) or verified
  perceptual residuals.
- **H1.5 (MAJOR - Dead-Reckoning Drift Dynamics Conflated with Exponential Decay)**: **PARTIALLY
  RESOLVED / PERSISTS AS MAJOR (see H2.2)**. Section 1.2 and §3.1 state the cubic error covariance
  expansion $R_{\text{dead}}(\Delta t) = R_0 + Q_{\text{drift}} \cdot (\Delta t)^3$. However, this
  drift model is completely absent from the Source Profile data structure (§2.3), is omitted from
  the arbitration pipeline (§5.3), and Example 1 (§7.1) still exhibits the anti-pattern of asserting
  raw incremental odometry directly as absolute coordinates.
- **H1.6 (MAJOR - Lack of Sensor Health State Machine and FDI Topology)**: **PARTIALLY RESOLVED /
  PERSISTS AS MAJOR (see H2.1)**. Fields `$status` and `$reject_count` were added to the Source
  Profile diagram (§2.1) and field table (§2.3). However, there is no state transition logic, trip
  threshold $N_{\text{trip}}$, or reset logic, and the arbitration pipelines in §5.1 and §5.3 never
  inspect `$status`—faulted sensors are not disqualified.
- **H1.7 (MAJOR - Absence of Reasonableness Limits and Slew-Rate Gating)**: **UNRESOLVED / PERSISTS
  AS MAJOR (see H2.3)**. Formally accepted in Round 1 disposition (§2.1), but completely omitted
  from the text, grammar, and lowering of v1.1.

______________________________________________________________________

## BLOCKING

None. All three Round 1 BLOCKING findings (H1.1, H1.2, H1.3) have been addressed at the core
mathematical and architectural level.

______________________________________________________________________

## MAJOR

### 1. (§2.3, §5.1, §5.3) Sensor Health State Machine and Reject Counter Unwired in Query Pipeline

Full title: Source Profile Health State and Innovation Reject Counter Lack Transition Logic and Are
Never Checked During Belief Arbitration

**Problem:** In response to H1.6, v1.1 added two fields to the Source Profile in
\[§2.1\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L294-L295)
and
\[§2.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L334-L335):

- `$status` (`Int`): Hardware health state: 0 (`NOMINAL`), 1 (`DEGRADED`), 2 (`SUSPECT`), 3
  (`FAULTED`).
- `$reject_count` (`Int`): Consecutive $\chi^2$ innovation test rejection counter.

In
\[§5.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L646-L648),
the spec states that measurements failing the innovation gate increment `$reject_count`.

However, the health state machine remains a disconnected stub:

1. **Missing State Transition & Trip Logic**: The document defines no transition rules or trip
   thresholds. There is no specification of how many consecutive rejects trip a sensor from
   `NOMINAL` to `SUSPECT` or `FAULTED` (e.g. $N_{\text{trip}} = 5$). Nor is there any reset rule: if
   an intermittent sensor passes the innovation gate, does `$reject_count` reset to 0? If so, does
   `$status` recover automatically or remain latched?
1. **Health State Completely Ignored During Arbitration**:
   - In
     \[§5.1\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L593-L615)
     (`truest` selection pipeline), the flowchart and pipeline steps proceed:
     `Collect -> FilterSources -> EvalScores -> Decay -> CloneDeref -> LogOdds -> RegSGD -> GateCheck`.
     The pipeline **never inspects `$status`**. If a sensor is marked `FAULTED` ($3$), its profile
     is still scored via historical Bayesian evidence ($\alpha / (\alpha + \beta)$) and decay. A
     sensor that operated reliably for 10 hours and then suffered catastrophic hardware failure
     (e.g. stiction or bus lockup) retains high historical reputation
     ($\alpha = 500, \beta = 5 \implies
     \mu = 0.99$) and will continue to win `believes truest` arbitration over healthy fallback
     sensors!
   - In
     \[§5.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L628-L661)
     (`consensus` fusion), measurements from a `FAULTED` sensor are still admitted into the
     candidate pool. While individual measurements might fail the $\chi^2$ gate, if a faulted sensor
     latches to a plausible value, it participates in Kalman fusion despite being physically
     faulted.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L294-L295\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L294-L295):
  Profile fields `$status` and `$reject_count` appear in the topology diagram.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L334-L335\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L334-L335):
  Field semantics describe the 4 enum states, but provide zero transition rules.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L593-L615\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L593-L615):
  The `truest` selection pipeline does not check `$status` at any stage.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L646-L648\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L646-L648):
  `$reject_count` is incremented, but never checked against a trip threshold or linked to a state
  transition.

**Fix:** Wire the sensor health state machine into the core arbitration pipeline:

1. In §2.3, define concrete state transition rules:
   - When an observation fails the $\chi^2$ innovation gate: `$reject_count <- $reject_count + 1`.
   - If `$reject_count >= 3`, transition `$status <- 2` (`SUSPECT`).
   - If `$reject_count >= 5` ($N_{\text{trip}}$), transition `$status <- 3` (`FAULTED`).
   - When an observation passes the innovation gate: if `$status == NOMINAL` or
     `$status == SUSPECT`, reset `$reject_count <- 0` and restore `$status <- 0` (`NOMINAL`). Once
     `$status == FAULTED`, it remains latched until an explicit recovery directive or recalibration
     occurs.
1. In §5.1 (Flowchart and lowering): Add a health validation gate immediately following
   `CloneDeref`:
   - If `source_profile.$status >= FAULTED`, immediately disqualify the clause from candidate
     scoring (assign warrant $W = 0.0$ or prune from the candidate set).
   - If `source_profile.$status == SUSPECT`, apply an additional penalty factor (e.g. inflate
     covariance $\times 4.0$ or discount log-odds).
1. In §5.3 (Consensus fusion): Filter out all sources with `$status >= FAULTED` before computing
   $\chi^2$ innovation and Kalman weights.

______________________________________________________________________

### 2. (§2.3, §3.1, §5.3, §7.1) Dead-Reckoning Drift Dynamics Disconnected from Profile and Pipeline

Full title: Dead-Reckoning Error Growth Equation Unplumbed in Topology and Arbitration; Worked
Examples Retain Odometry Absolute Position Anti-Pattern

**Problem:** Section 3.1 introduces the cubic dead-reckoning error covariance expansion:

```math
R_{\text{dead}}(\Delta t) = R_0 + Q_{\text{drift}} \cdot (\Delta t)^3
```

While the physics equation is sound, it is completely disconnected from the rest of the
specification:

1. **Zero Storage for Sensor Kinematic Parameters**: The Source Profile table in
   \[§2.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L327-L338)
   contains only scalar Bayesian parameters (`$alpha`, `$beta`, `$decay_rate`, `$weights`, `$bias`).
   There is no field indicating sensor classification (e.g.
   `$sensor_type: ABSOLUTE | DEAD_RECKONING`), nor any storage for baseline noise $R_0$ or drift
   expansion coefficient $Q_{\text{drift}}$.

1. **Arbitration Pipeline Ignores $R_{\text{dead}}$**: In
   \[§5.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L632-L637),
   the covariance inflation step uses static nominal noise:
   $\tilde{R}_i = \frac{R_i}{\max(w_i, \epsilon_{\text{floor}})}$. It never evaluates
   $R_{\text{dead}}(\Delta t)$, meaning the cubic drift formula specified in §3.1 is never executed
   at runtime!

1. **Worked Example Anti-Pattern Retained**: In Example 1 (Rover Navigation,
   \[§7.1\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L736-L738)),
   wheel odometry is still ingested as an unintegrated absolute coordinate generator:

   ```vagent
   on percept OdometryStep as odom {
     assert at(self, odom.x, odom.y) from "sensor:odometry"
   }
   ```

   As detailed in H1.5, wheel odometry measures incremental steps $(\Delta s, \Delta \theta)$.
   Treating it as asserting absolute `(odom.x, odom.y)` means any wheel slip creates an
   uncorrectable step bias that is permanently embedded in `+d.clause` on every turn.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L327-L338\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L327-L338):
  Source Profile has no fields for $R_0$, $Q_{\text{drift}}$, or sensor category.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L384-L389\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L384-L389):
  $R_{\text{dead}}(\Delta t)$ is defined in isolation, with no connection to the profile fields or
  arbitration lowering.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L632-L637\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L632-L637):
  Step 1 of consensus fusion references only static $R_i$, not $R_{\text{dead}}(\Delta t)$.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L736-L738\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L736-L738):
  `OdometryStep` asserts raw coordinates directly to `at(self, odom.x, odom.y)`.

**Fix:** Plumb the dead-reckoning drift dynamics into the profile topology, arbitration pipeline,
and examples:

1. In §2.3, extend the Source Profile field table with optional kinematic fields:
   - `$sensor_kind` (`Int`): `0 = ABSOLUTE` (temporal decay $w_S(\Delta t)$), `1 = DEAD_RECKONING`
     (covariance expansion $R_{\text{dead}}$).
   - `$r0` (`Float`): Nominal baseline covariance $R_0$.
   - `$q_drift` (`Float`): Kinematic drift variance rate $Q_{\text{drift}}$.
1. In §5.3, update Step 1 (Covariance Inflation):
   - For absolute sensors: $R_i(\Delta t) = R_0$.
   - For dead-reckoning sensors: $R_i(\Delta t) = R_0 + Q_{\text{drift}} \cdot (\Delta t)^3$.
   - Inflated covariance: $\tilde{R}_i = \frac{R_i(\Delta t)}{\max(w_i, \epsilon_{\text{floor}})}$.
1. In §7.1, clarify the architectural boundary between raw hardware percepts and belief assertions:
   an odometry skill or dead-reckoning filter integrates incremental encoder steps
   $(\Delta s, \Delta
   \theta)$ into estimated pose with expanding covariance $R_{\text{dead}}$ before asserting
   `at(self, est.x, est.y)` with dead-reckoning provenance.

______________________________________________________________________

### 3. (§4.2, §4.4, §5.1) Complete Omission of Kinematic Reasonableness and Slew-Rate Gating

Full title: Kinematic Slew-Rate Gating and Physical Reasonableness Limits Omitted Despite Formal
Acceptance in Round 1 Disposition

**Problem:** In Round 1, finding H1.7 documented that percept assertions on `+d.clause` lack
physical reasonableness limits (e.g. impossible speeds, negative distances, or sensor rail
saturation). In the Round 1 disposition
(\[§2.1\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/r1-disposition.md#L73-L74)),
this was formally accepted:

> "- **H1.7 (MAJOR - Kinematic Reasonableness and Slew-Rate Limits)**: **ACCEPTED**. Add slew-rate
> gating ($\Delta x / \Delta t \le v_{\max}$) in percept ingestion policies."

However, in v1.1, kinematic reasonableness limits and slew-rate filters are **completely missing**:

- Section 4.2 contains no grammar or policy parameters for `max_slew_rate`, `bounds`, or kinematic
  gating.
- Section 4.4 contains no compiler lowering for slew checks during assertion or query resolution.
- Section 5 contains no validation step verifying that an asserted coordinate jump
  $\|\mathbf{x}_{\text{now}} - \mathbf{x}_{\text{prev}}\|$ is physically achievable given elapsed
  turns $\Delta t$ and vehicle velocity limits.

Without slew-rate gating, if an ultrasonic sensor suffers multipath or an ADC glitches to rail
voltage, or GPS reports a 500-meter multipath jump in a 50ms engine turn, the impossible coordinate
bypasses ingestion, enters `+d.clause`, and is accepted if the source has high prior Bayesian
credibility.

**Evidence:**

- \[design/projects/vagent-epistemic-arbitration-review/r1-disposition.md#L73-L74\](file:///data/git/gleditor/design/projects/vagent-epistemic-arbitration-review/r1-disposition.md#L73-L74):
  H1.7 is recorded as ACCEPTED.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L535-L556\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L535-L556):
  Declarative predicate arbitration policies in §4.2 contain only `decay`, `model`,
  `min_confidence`, `fusion`, and `innovation_gate`. Slew rate and physical bounds are absent.
- Grep of `vagent-dynamic-source-weighting-and-epistemic-arbitration.md` for `slew`, `bounds`, or
  `kinematic` returns zero occurrences in specification logic.

**Fix:** Add declarative kinematic reasonableness and slew-rate gating:

1. In §4.2, add optional kinematic constraints to declarative arbitration policies:

   ```vagent
   predicate at(key who: Agent, x: Float, y: Float)
     arbitrated by truest(
       decay: 1.0s,
       model: "online_sgd",
       max_slew: 15.0m_s,        // Maximum physical velocity (15 m/s)
       cluster_tolerance: 0.25m  // Continuous clustering epsilon
     )
   ```

1. In §4.4 and §5.1, lower `max_slew` into a kinematic validation step:

   - When evaluating a candidate coordinate $\mathbf{x}_{\text{cand}}$ against prior state
     $\mathbf{x}_{\text{prior}}$ with time delta $\Delta t$:

     ```math
     v_{\text{implied}} = \frac{\|\mathbf{x}_{\text{cand}} - \mathbf{x}_{\text{prior}}\|_2}{\Delta t}
     ```

   - If $v_{\text{implied}} > v_{\max}$, the candidate is rejected as a physically impossible
     kinematic step (teleportation anomaly), incrementing `$reject_count` and disqualifying the
     assertion.

______________________________________________________________________

### 4. (§4.1, §5.3, §8.2) Ingestion Gap for Nominal Covariance ($R_i$) and Discarded Fused Covariance

Full title: Continuous Consensus Provides No Mechanism to Supply Nominal Covariance ($R_i$) and
Discards Posterior Covariance Output

**Problem:** Section 5.3 successfully resolves the mathematical flaw of H1.1 by decoupling physical
noise covariance $R_i$ from trust $w_i$:

```math
\tilde{R}_i = \frac{R_i}{\max(w_i, \epsilon_{\text{floor}})}, \qquad \hat{x}_{\text{consensus}} = \left( \sum_{i=1}^N \tilde{R}_i^{-1} \right)^{-1} \sum_{i=1}^N \tilde{R}_i^{-1} x_i
```

And
\[§8.2\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L871-L872)
specifies `epistemic_gaussian_fuse(values_list, variances_list, threshold)`.

However, two critical operational gaps exist in the specification:

1. **How is Nominal Covariance $R_i$ Ingested?**:
   - The specification never defines where $R_i$ comes from for a given sensor or clause.
   - In
     \[§4.1\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L530-L533)
     and
     \[§7.1\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L732-L743),
     assertions only state `assert at(self, x, y) from S`. There is no grammar to supply measurement
     variance `with variance: V`.
   - The Source Profile table in §2.3 contains no field for nominal sensor variance.
   - If neither the clause assertion nor the source profile specifies $R_i$, the compiler cannot
     populate `variances_list` for `epistemic_gaussian_fuse`. If the runtime falls back to an
     implicit default $R_i = 1.0$, it silently degenerates back into the dimensionless error
     prohibited by H1.1!
1. **Discarded Posterior Covariance Output**:
   - In §5.3 line 657, the Kalman consensus equation defines $\hat{x}_{\text{consensus}}$, but
     completely omits the synthesized posterior covariance:

     ```math
     \tilde{P}_{\text{consensus}} = \left( \sum_{i=1}^N \tilde{R}_i^{-1} \right)^{-1}
     ```

   - When a plan queries `if believes consensus at("rover", ?x, ?y)`, it receives only the
     coordinate centroid $(\hat{x}, \hat{y})$. In autonomous robotics and closed-loop motion
     control, downstream trajectory planners and Kalman filters **must know the uncertainty**
     (covariance) of the fused fix to adjust speed, corridor clearance, or braking distance.
     Discarding $\tilde{P}_{\text{consensus}}$ discards the primary benefit of Gaussian fusion.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L530-L533\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L530-L533):
  `believes consensus at("rover", ?x, ?y)` binds only coordinates.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L657-L660\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L657-L660):
  $\hat{x}_{\text{consensus}}$ is computed; posterior covariance is neither recorded nor bound.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L871-L872\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L871-L872):
  `epistemic_gaussian_fuse` requires `variances_list`, but the compiler has no way to extract it
  from clauses.

**Fix:**

1. Support nominal variance specification in assertions or source profiles:
   - Allow optional variance annotation on continuous assertions:
     `assert at(self, fix.lat, fix.lon) from "sensor:gps" with variance: fix.accuracy_sq`.
   - In §2.3, add `$nominal_variance` (`Float`, default $1.0\,\text{unit}^2$) to the Source Profile
     to serve as the fallback when an assertion omits dynamic variance telemetry.
1. In §5.3, record and expose posterior consensus covariance:
   - Compute $\tilde{P}_{\text{consensus}} = (\sum_{i=1}^N \tilde{R}_i^{-1})^{-1}$.
   - Store $\tilde{P}_{\text{consensus}}$ as an epistemic attribute on the synthesized consensus
     clause.
   - Allow query syntax to optionally bind uncertainty:
     `believes consensus at("rover", ?x, ?y) with uncertainty: ?sigma`.

______________________________________________________________________

## MINOR

### 5. (§5.3, line 640) Ambiguity of Prior State ($\hat{x}_0, P_0$) in Innovation Gating Formulation

Full title: Chi-Squared Innovation Gate References Unbound Prior State ($\hat{x}_0, P_0$) in
Stateless Consensus Queries

**Problem:** Section 5.3 specifies the Mahalanobis innovation test:

```math
d_M^2 = (x_i - \hat{x}_0)^T (\tilde{R}_i + P_0)^{-1} (x_i - \hat{x}_0) \le \chi^2_{\alpha, k}
```

In a tracking filter, $\hat{x}_0$ and $P_0$ represent the predicted state and covariance propagated
from the previous time step. However, `believes consensus P(...)` is invoked as a general
declarative query on a set of coexisting clauses along `+d.clause`.

If the agent has no pre-existing belief or prior state for predicate `P`, what values are used for
$\hat{x}_0$ and $P_0$? If $\hat{x}_0$ is uninitialized, the innovation gate cannot evaluate.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L640-L644\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L640-L644):
  $\hat{x}_0$ and $P_0$ appear without definition of how they are acquired or initialized.

**Fix:** Clarify the reference state for innovation gating:

1. When a prior belief exists for the key tuple from turn $t - 1$, $(\hat{x}_0, P_0)$ is the prior
   belief state propagated with process noise $Q \Delta t$.
1. When evaluating a stateless consensus query without a prior belief, perform **median-referenced
   innovation gating**: $\hat{x}_0 = \text{median}(\{x_i\})$, and $P_0 = \text{MAD}^2$ (Median
   Absolute Deviation), or use pairwise mutual Mahalanobis consistency tests between candidates.

______________________________________________________________________

### 6. (§4.2, line 540) Declarative Policy Examples Omit Clustering Tolerance Parameter

Full title: Policy Examples in §4.2 Omit Declarative Syntax for `cluster_tolerance`
($\epsilon_{\text{tol}}$)

**Problem:** Section 3.5 introduces $\epsilon_{\text{tol}}$ as the metric distance threshold for
clustering continuous clauses into mutual hypothesis pools. However, in
\[§4.2\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L540-L553),
the declarative predicate policy examples omit this parameter:

```vagent
predicate at(key who: Agent, x: Float, y: Float)
  arbitrated by truest(
    decay: 1.5s,
    model: "online_sgd",
    min_confidence: 0.6
  )
```

Without declarative policy configuration, $\epsilon_{\text{tol}}$ defaults to an unspecified
constant, preventing developers from configuring domain-appropriate tolerances (e.g. 0.2 meters for
a mobile robot vs. 50 meters for a maritime vessel).

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L540-L553\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L540-L553):
  `cluster_tolerance` is absent from the policy declaration example.

**Fix:** Explicitly include `cluster_tolerance` in the §4.2 policy examples and document its default
value (e.g. `cluster_tolerance: 1.0` if omitted).

______________________________________________________________________

### 7. (§4.3, line 557) Surface Syntax Missing Operational Sensor Recovery Directives

Full title: Absence of Directives for Hardware Recalibration and Fault Recovery (`recalibrate`,
`reset`, `isolate`)

**Problem:** Section 4.3 provides `confirm` and `refute` for ground belief truth maintenance.
However, once a hardware sensor trips into `FAULTED` status via consecutive innovation rejections,
there is no surface directive to reset fault counters, trigger calibration maneuvers, or isolate a
misbehaving sensor:

- `reset source "sensor:gps"`: Resets `$status <- NOMINAL` and `$reject_count <- 0` after physical
  reboot.
- `isolate source "sensor:imu"`: Forces `$status <- FAULTED` or unlinks the source without waiting
  for gate trips.
- `recalibrate source "sensor:imu"`: Initiates a zero-velocity update or bias compensation routine.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L557-L570\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L557-L570):
  Only `confirm` and `refute` are provided.

**Fix:** Add operational source directives to `vagent` grammar:

```vagent
// Resets fault counters and restores status to NOMINAL after recovery
reset source "sensor:gps"

// Manually isolates a malfunctioning or uncalibrated hardware sensor
isolate source "sensor:wheel_odometry"
```

Lower these to bytecode routines that directly manipulate the source profile master on `+d.trust`.

______________________________________________________________________

## VERDICT: NOT CONVERGED

While the revised specification (v1.1) successfully resolves all three Round 1 BLOCKING findings by
introducing covariance-inflated Kalman weighting, $\chi^2$ Mahalanobis innovation gating, and
continuous metric tolerance clustering ($\epsilon_{\text{tol}}$), convergence cannot be granted.
Four MAJOR hardware fault issues remain unresolved:

1. **H2.1 (MAJOR)**: Sensor health state `$status` and reject counters `$reject_count` were added to
   the profile table, but have zero transition logic and are completely unplumbed into the
   arbitration pipelines (§5.1 and §5.3)—faulted sensors are never disqualified.
1. **H2.2 (MAJOR)**: The cubic dead-reckoning drift expansion formula is disconnected from the data
   structures and arbitration pipeline, while Example 1 continues to treat wheel odometry as an
   absolute coordinate generator.
1. **H2.3 (MAJOR)**: Kinematic reasonableness limits and slew-rate gating were formally accepted in
   the Round 1 disposition, but completely omitted from the text and grammar of v1.1.
1. **H2.4 (MAJOR)**: Continuous consensus provides no mechanism or syntax for sensors to supply
   nominal variance $R_i$, and discards the fused posterior covariance
   $\tilde{P}_{\text{consensus}}$ required for closed-loop motion control.

A focused revision resolving these 4 MAJOR items will achieve convergence for the Hardware Fault &
Sensor Specialist persona.
