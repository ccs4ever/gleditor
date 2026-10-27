# Round 1 critique: Hardware Fault Specialist (epistemic-arbitration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 3 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

None (Round 1 initial review).

______________________________________________________________________

## BLOCKING

### 1. (§5.3, line 627) Epistemic Weight Equated to Inverse Variance in Gaussian Fusion

Full title: Continuous Gaussian Fusion Equates Dimensionless Epistemic Reputation to Inverse
Variance Without Physical Dimensions or Measurement Scale

**Problem:** The specification attempts to implement continuous Kalman/Gaussian value fusion in
\[§5.3\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L627-L638)
by directly substituting the normalized epistemic reputation weight $w_i \\in [0.0, 1.0]$ for
inverse variance:

```math
\hat{x}_{\text{consensus}} = \frac{\sum_{i=1}^N w_i x_i}{\sum_{i=1}^N w_i}, \qquad \sigma_{\text{consensus}}^2 = \frac{1}{\sum_{i=1}^N w_i}
```

This formulation is physically and mathematically invalid:

1. **Dimensional Incompatibility**: Epistemic reputation $w_i$ is a dimensionless probability-like
   scalar in $[0.0, 1.0]$. In contrast, physical variance $\\sigma_i^2$ carries the squared
   dimension of the underlying physical quantity (e.g. $\\text{m}^2$ for position,
   $(\\text{m}/\\text{s})^2$ for velocity, $\\text{K}^2$ for temperature). Setting
   $\\frac{1}{\\sigma_i^2} = w_i$ forces physical variance to $\\frac{1}{w_i} \\ge 1.0$, regardless
   of whether the physical sensor measures in meters, millimeters, degrees, or millivolts.

1. **False Scaling of Measurement Precision**: A top-tier, highly calibrated RTK GNSS receiver with
   horizontal variance $\\sigma^2 = (0.01,\\text{m})^2 = 0.0001,\\text{m}^2$ has an actual inverse
   variance of $10{,}000,\\text{m}^{-2}$. Under the document's formula, even if assigned maximum
   reputation $w_i = 1.0$, its variance is computed as $\\sigma^2 = \\frac{1}{1.0} =
   1.0,\\text{m}^2$ — an error inflation of four orders of magnitude!

1. **Absurd Synthetic Certainty**: If five degraded or mediocre sensors ($w_i = 0.5$) assert
   measurements, the synthesized variance is $\\sigma^2 = \\frac{1}{5 \\times 0.5} = 0.4$, which the
   runtime claims is *more certain* than a single pristine sensor ($w_1 = 0.95 \\implies \\sigma^2 =
   1.05$). In physical robotics, pooling mediocre, uncalibrated sensors does not beat an authentic,
   high-precision instrument.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L629-L638\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L629-L638):
  > "Treating each weight as inverse variance $\\frac{1}{\\sigma_i^2}$:
  > $\\hat{x}_{\\text{consensus}} = \\frac{\\sum_{i=1}^N w_i x_i}{\\sum\_{i=1}^N w_i}$,
  > $\\sigma\_{\\text{consensus}}^2 = \\frac{1}{\\sum\_{i=1}^N w_i}$"
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L904\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L904):
  `epistemic_gaussian_fuse(values_list, variances_list) -> \hat{x}` lists `variances_list`, but §5.3
  replaces variances with epistemic weights $w_i$.

**Fix:** Decouple physical sensor measurement noise covariance $R_i = \\sigma\_{\\text{meas}, i}^2$
from epistemic source reputation $w_i$. Physical sensors must provide their nominal measurement
variance/covariance $R_i$ (from manufacturer datasheet or dynamic sensor telemetry such as GNSS
HDOP). Epistemic reputation $w_i \\in (0, 1\]$ acts as an **inflation penalty** on measurement
covariance:

```math
\tilde{R}_i = \frac{R_i}{w_i}
```

When $w_i \\to 1.0$, the filter trusts the sensor's physical noise model $R_i$. As $w_i$ degrades
($w_i \\to 0$), $\\tilde{R}\_i \\to \\infty$, naturally down-weighting the sensor in optimal
Gaussian fusion:

```math
\hat{x}_{\text{consensus}} = \left( \sum_{i=1}^N \tilde{R}_i^{-1} \right)^{-1} \sum_{i=1}^N \tilde{R}_i^{-1} x_i, \qquad \sigma_{\text{consensus}}^2 = \left( \sum_{i=1}^N \tilde{R}_i^{-1} \right)^{-1}
```

______________________________________________________________________

### 2. (§5.3, line 627) Gaussian Fusion Lacks Innovation Gating and Outlier Rejection

Full title: Gaussian Continuous Fusion Has No Innovation Gating, Exposing Physical Actuators to
Catastrophic Outlier Corruption

**Problem:** Section 5.3 calculates $\\hat{x}\_{\\text{consensus}}$ as an unconstrained weighted
average across all asserted continuous values. In real hardware systems, sensors frequently exhibit
soft and intermittent faults:

- GNSS multipath reflections in urban canyons (sudden 50-meter position step).
- Ultrasonic specular bounce / cross-talk producing maximum-range or near-zero phantom readings.
- LiDAR smoke, dust, or rain backscatter returning false obstacles at 0.5 meters.
- Radar ghost reflections or multi-bounce targets.
- Vision feature tracking loss on textureless surfaces or severe lens flare.
- ADC stiction or brownout latching a sensor to rail voltage ($V\_{\\text{max}}$).

If two nominal sensors report position $x_1 = 10.0,\\text{m}$ ($w_1 = 0.90$) and $x_2 =
10.2,\\text{m}$ ($w_2 = 0.85$), while a third sensor suffers a multipath spike to $x_3 =
100.0,\\text{m}$ ($w_3 = 0.40$), the proposed Gaussian fusion computes:

```math
\hat{x} = \frac{0.90(10.0) + 0.85(10.2) + 0.40(100.0)}{0.90 + 0.85 + 0.40} = \frac{9.0 + 8.67 + 40.0}{2.15} = 26.82\,\text{m}
```

The robot's synthesized position jumps instantly by nearly 17 meters! In closed-loop motion control,
this sudden artificial innovation injects a massive control derivative spike, commanding extreme
actuator torque, throwing the robot into violent oscillation or driving it into a physical obstacle.

Real engineering sensor fusion architectures (e.g. Kalman filters, Extended Kalman Filters, Factor
Graphs) **never** perform un-gated linear fusion. Every candidate measurement must pass an
**innovation validation gate** (Chi-squared $\\chi^2$ gate or Mahalanobis distance threshold)
against the prior predicted state $\\hat{x}^-$ before being admitted into fusion.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L625-L642\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L625-L642):
  Section 5.3 specifies raw linear fusion with zero residual check, zero gating, and zero outlier
  exclusion.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L508-L516\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L508-L516):
  Surface syntax `believes consensus at("drone1", let x, let y)` assumes seamless fusion without
  outlier rejection.

**Fix:** Add an explicit **Innovation Gating and Outlier Rejection Pipeline** to continuous
consensus arbitration:

First, prior to fusion, compute Normalized Innovation Squared (NIS) for each candidate sensor $i$
against the median or current predicted state estimate $\\hat{x}^-$:

```math
d_i^2 = (x_i - \hat{x}^-)^T S_i^{-1} (x_i - \hat{x}^-)
```

where $S_i = P^- + \\tilde{R}\_i$.

Second, enforce a Chi-squared gate test: $d_i^2 \\le \\gamma\_{\\chi^2}$ (e.g. $\\gamma = 9.21$ for
2-DOF at $p = 0.01$).

Third, measurements violating the gate are rejected as outliers, assigned $w_i = 0$ for that fusion
turn, and routed to the fault isolation subsystem.

Fourth, support robust M-estimators (Huber or Tukey bi-weight loss) as an alternative option in
`arbitrated by consensus(fusion: robust_gaussian, gate: 3.0_sigma)`.

______________________________________________________________________

### 3. (§3.5, line 442) Continuous Percepts Break Multi-Source Corroboration Formula

Full title: Multi-Source Corroboration Requires Bitwise Value Equality, Completely Breaking
Continuous Physical Sensor Corroboration

**Problem:** Section 3.5 introduces the consensus corroboration formula:

```math
W(\text{Clause}) = 1.0 - \prod_{S \in \text{Sources}(\text{Clause})} (1.0 - w_S)
```

The document explicitly states: *"When multiple independent sources corroborate the **exact same
fact** (Clause 1), their epistemic weights combine."*

In physical sensor systems, independent sensors observing continuous state variables (position,
speed, range, temperature, pressure) **never assert bitwise identical floating-point values**.

- Sensor $A$ (GPS) asserts `at("rover", 10.00014, 20.00028)`
- Sensor $B$ (LiDAR SLAM) asserts `at("rover", 10.00019, 20.00031)`
- Sensor $C$ (Wheel Odometry) asserts `at("rover", 10.00050, 20.00010)`

Under the P18 belief manifold topology (`d.clause`), because these floating-point arguments differ,
they are minted as **three separate, non-identical clauses** along `+d.clause`. Each clause has
exactly *one* source on its `+d.source` rank.

Consequently:

- `Sources(Clause 1) = { "sensor:gps" }`
- `Sources(Clause 2) = { "sensor:slam" }`
- `Sources(Clause 3) = { "sensor:odometry" }`

The product $\\prod\_{S \\in \\text{Sources}}(1 - w_S)$ evaluates over a single source! The
corroboration weight $W(\\text{Clause})$ is identically equal to the single source's individual
weight $w_i$. The corroboration mechanism **never fires** for continuous physical perceptions,
rendering §3.5 completely non-functional for robotics, navigation, and physical telemetry.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L442-L445\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L442-L445):
  "When multiple independent sources corroborate the **exact same fact** (Clause 1), their epistemic
  weights combine."
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L430-L440\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L430-L440):
  The diagram shows Clause 1 having multiple sources (`'sensor:gps'`, `'peer:scout_1'`), which
  happens only if two sources assert the exact same ground arguments.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L738-L748\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L738-L748):
  In Example 1, GPS, Odometry, and SLAM assert floating-point coordinates `(fix.lat, fix.lon)`,
  `(odom.x, odom.y)`, `(slam.x, slam.y)`. They will never match bitwise.

**Fix:** Extend the continuous arbitration engine with **Spatial/Metric Tolerance Clustering**:

First, when evaluating `believes truest` or `believes consensus` on continuous predicates, clauses
on `+d.clause` must be clustered into an equivalence pool using a metric distance threshold:

```math
\text{dist}(\mathbf{x}_1, \mathbf{x}_2) \le \epsilon_{\text{tol}}
```

(or Mahalanobis distance based on sensor covariance).

Second, allow declarative clustering tolerances in predicate declarations:

```vagent
predicate at(key who: Agent, x: Float, y: Float)
  arbitrated by truest(
    cluster_tolerance: 0.25m, // Cluster beliefs within 25cm
    decay: 1.0s,
    model: online_sgd
  )
```

Third, when clauses cluster within $\\epsilon\_{\\text{tol}}$, their asserting sources are pooled
into an effective multi-source set $\\mathcal{S}_{\\text{cluster}}$ before applying the
corroboration weight formula $W = 1 - \\prod_{S \\in \\mathcal{S}\_{\\text{cluster}}} (1 - w_S)$.

______________________________________________________________________

## MAJOR

### 4. (§6.2, line 673) Binary Outcome Backpropagation Causes Credit Misattribution

Full title: Binary Goal Backpropagation Causes Destructive False Refutations of Healthy Physical
Sensors

**Problem:** Section 6.2 and §7.1 specify that when an intention finishes, the turn settle loop
checks physical plan success or failure:

- Plan Succeeded $\\implies y = 1.0$ (rewards all sources in `EpistemicProvenanceToken` with
  $\\alpha \\leftarrow \\alpha + 1$).
- Plan Failed $\\implies y = 0.0$ (penalizes all sources in token with $\\beta \\leftarrow \\beta +
  1$ and negative SGD error).

In physical robot operations, plans fail for hundreds of mechanical, environmental, and kinematic
reasons that have nothing to do with the primary navigation sensor:

1. A motor driver trips on thermal overcurrent or battery voltage sag while climbing a slope.
1. A dynamic obstacle (pedestrian, door closing) blocks the planned path, triggering a motion
   timeout.
1. Wheel odometry slips on a patch of oil or gravel, causing a control trajectory error.
1. The waypoint beacon's optical tag is temporarily occluded by foliage.

Under §6.2, if the rover times out or misses the beacon, the system refutes the position belief:
`refute at(self, dest_x, dest_y)`. The healthy, perfectly accurate GNSS receiver or LiDAR SLAM unit
that provided the initial fix is marked erroneous ($y = 0$), incrementing its refutation counter
$\\beta \\leftarrow \\beta + 1$! Within 2 or 3 mechanical hiccups, the agent's most reliable sensor
is degraded into untrusted status ($\\mu < 0.2$), forcing the agent to switch to inferior or
uncalibrated sources.

Conversely, if a broken sensor (e.g. an IMU with severe bias drift) is used in a plan that succeeds
by sheer coincidence (e.g. a wide-area goal or generous waypoint acceptance radius), the faulty
sensor is falsely rewarded with $y = 1$.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L673-L702\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L673-L702):
  Plan outcome backpropagation maps binary plan completion directly to $y \\in {0, 1}$.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L758-L765\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L758-L765):
  `if let landmark := detect_landmark(...) { confirm ... } else { refute ...; fail(...); }`

**Fix:** Replace coarse binary plan-level backpropagation with **Residual-Based Verification and
Isolated Fault Attribution**:

First, do not penalize sensory sources for plan-level execution timeouts, actuator stalls, or path
obstructions. The runtime must distinguish between *deliberative failure* (plan execution aborted)
and *sensory invalidation* (authoritative measurement contradicts asserted belief).

Second, sensory updates should be driven by **continuous residual magnitude**:

```math
\text{error} = \| \mathbf{x}_{\text{asserted}} - \mathbf{x}_{\text{ground\_truth}} \|
```

If $\\text{error} \\le 3\\sigma$, $\\alpha$ receives a fractional or unit reward. If $\\text{error}
\> 3\\sigma$, $\\beta$ is incremented proportionally to the standardized residual $e / \\sigma$.

Third, when multiple sensors contributed to a fused estimate, use **parity space residual analysis**
(analytical redundancy) to isolate the specific deviant sensor rather than penalizing all
contributing sources uniformly.

______________________________________________________________________

### 5. (§3.1, line 313) Dead-Reckoning Drift Dynamics Conflated with Exponential Decay

Full title: Failure to Distinguish Absolute Position Telemetry from Relative Dead-Reckoning
Integration Drift

**Problem:** Section 3.1 models temporal freshness strictly via exponential decay:

```math
\Phi(S, \Delta t) = e^{-\lambda_S \Delta t}
```

In Example 1 (\[§7.1, line
742\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L742)),
the rover handles wheel odometry by asserting absolute coordinates:
`assert at(self, odom.x, odom.y) from "sensor:odometry"`.

This demonstrates a fundamental misunderstanding of dead reckoning versus absolute position sensing:

1. **Nature of Odometry and IMU**: Dead-reckoning sensors (wheel encoders, optical flow, MEMS
   gyroscopes, accelerometers) do not measure absolute position $(x, y)$. They measure *differential
   rates* $(\\Delta s, \\Delta \\theta, \\mathbf{a}, \\mathbf{\\omega})$.
1. **Error Accumulation Physics**:
   - An absolute sensor (e.g. GNSS) has bounded variance over time: $\\sigma\_{\\text{GNSS}}^2(t)
     \\approx \\sigma_0^2$. Its reading becomes stale over elapsed time, which exponential decay
     models acceptably.
   - A dead-reckoning sensor accumulates error monotonically. Integrated wheel slip causes
     positional error variance to grow with distance traveled: $\\sigma\_{\\text{odom}}^2(s)
     \\propto s$.
   - Integrated MEMS accelerometer bias double-integrates into quadratic position drift: $\\Delta
     x(t) = \\frac{1}{2} b_a t^2$. Integrated gyroscope bias random-walks orientation:
     $\\sigma\_\\theta^2(t) \\propto t$.
1. **The Irreversible Bias Trap**: If odometry asserts `at(self, 10.2, 20.1)` and the wheels slip on
   wet terrain, an unobservable step bias is permanently embedded in `odom.x, odom.y`. Decaying
   credibility $\\Phi(t)$ over time does not correct the bias; when the next odometry step arrives,
   it asserts a new position that inherits the corrupted offset!

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L313-L331\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L313-L331):
  Section 3.1 assumes all sensor degradation is purely temporal half-life decay $\\Phi =
  e^{-\\lambda \\Delta t}$.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L742-L744\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L742-L744):
  `on percept OdometryStep as odom { assert at(self, odom.x, odom.y) from "sensor:odometry" }`
  treats odometry as an absolute position generator.

**Fix:**

1. Explicitly document and model the difference between **Absolute State Percepts** (GNSS, landmark
   beacons, fiducial markers) and **Differential/Incremental Percepts** (wheel odometry, IMU, visual
   odometry).
1. Clarify that incremental percepts should update an internal kinematic state estimator (e.g., in a
   dedicated skill or filter capacity) that propagates state covariance $\\mathbf{P}_{k|k-1} =
   \\mathbf{F} \\mathbf{P}_{k-1} \\mathbf{F}^T + \\mathbf{Q}$, rather than asserting unintegrated
   absolute coordinates directly into `+d.clause`.
1. In §3.1, specify that for dead-reckoning sources, the discount factor is a function of integrated
   distance or motion steps ($\\Delta s$), not merely static wall-clock time:
   $\\Phi\_{\\text{odom}}(s) = e^{-\\lambda_s \\Delta s}$.

______________________________________________________________________

### 6. (§2.1–§2.3, line 228) Lack of Sensor Health State Machine and FDI Topology

Full title: Absence of Hardware Sensor Health State Machine, Fault Isolation Counters, and Hard
Fault Detection

**Problem:** The Source Profile topology on `+d.trust` (\[§2.2, line
277\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L277))
stores only statistical learning parameters: `$alpha`, `$beta`, `$decay_rate`, `$last_seen`,
`$obs_count`, `$weights`, `$bias`.

This model accounts only for gradual, stochastic soft drift. It completely lacks representation for
critical physical sensor failure modes:

1. **Hard Faults**:
   - Total Loss of Signal (cable disconnection, I2C/SPI bus freeze, CAN bus-off state).
   - Brownout / Microcontroller Reset: Sensor MCU reboots due to motor surge, resetting to default
     uncalibrated register states.
   - Mechanical Stiction: MEMS proof mass physically stuck against substrate stop after shock,
     pegging output at constant non-zero value.
   - Frozen Frame Buffer / Stale Driver Cache: Driver repeatedly outputs the identical cached packet
     while the sensor hardware is dead.
1. **Operational Degradation**:
   - Saturation: Gyroscope exceeds dynamic range ($\\pm 2000^\\circ/\\text{s}$), accelerometer clips
     at $\\pm 16g$.
   - Optical Degrade: Camera blinded by sun glare, lens obscured by mud, LiDAR window occluded.
   - GNSS Loss of Fix: Drop from RTK Fixed $\\to$ RTK Float $\\to$ 2D Fix $\\to$ Dead Reckoning.

In all these cases, the sensor's historical Bayesian reputation ($\\alpha / (\\alpha + \\beta)$) may
be very high ($0.98$), because it functioned well for hours. When a hard fault occurs, the agent
will continue blindly selecting its corrupted or frozen percepts for dozens of turns until enough
plan failures accumulate to drag $\\mu$ down!

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L277-L286\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L277-L286):
  The Source Profile field table contains zero fields for operational health state, fault counters,
  or physical error status.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L873-L892\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L873-L892):
  Proposal P19.1 contains no mechanism for immediate fault trip or isolation.

**Fix:** Extend the Source Profile cell topology along `+d.vars`/`+d.values` with an explicit
**Sensor Health and FDI Descriptor**:

- `$health_state` (`Int` enum): `0 = NOMINAL`, `1 = DEGRADED`, `2 = SUSPECT`, `3 = FAULTED`,
  `4 = OFFLINE`.
- `$consecutive_rejects` (`Int`): Counter incremented when innovation gate rejects an observation;
  trips to `FAULTED` when exceeding threshold $N\_{\\text{trip}}$ (e.g. 5 consecutive rejections).
- `$last_heartbeat` (`Int`): Engine turn of last raw packet received; trips to `OFFLINE` if
  $(t\_{\\text{now}} - t\_{\\text{hb}}) > \\text{timeout}$.
- `$status_flags` (`Int` bitfield): Hardware flags (`BIT_SATURATED`, `BIT_CALIBRATING`,
  `BIT_STALE_CACHE`, `BIT_NO_FIX`).

In the arbitration pipeline (§5.1), immediately disqualify any source whose
`$health_state \ge \text{FAULTED}` or whose `$status_flags` indicates invalid data, bypassing the
slow Bayesian decay.

______________________________________________________________________

### 7. (§4.1–§4.2, line 471) Absence of Reasonableness Limits and Slew-Rate Gating

Full title: Lack of Declarative Kinematic Reasonableness Limits, Value Bounds, and Slew-Rate Filters
on Percept Ingestion

**Problem:** Under the current specification, percepts asserted from external sources are placed
onto `+d.clause` without any physical reasonableness verification. If a hardware glitch, GNSS
spoofing attack, or integer wraparound produces an physically impossible measurement:

- Ultrasonic range reports $-5.0,\\text{m}$ (negative distance).
- Battery sensor reports $450,\\text{V}$ on a $24,\\text{V}$ bus.
- GPS coordinates jump $500,\\text{km}$ in a single 50ms engine turn ($\\text{speed} =
  10{,}000,\\text{km}/\\text{s}$).
- Accelerometer reports $250g$ on a ground rover.

The assertion enters `+d.clause`. If the asserting source has an established high trust score,
`believes truest` will bind this physically impossible value and feed it to downstream plan logic.

Every reliable autonomous system enforces **reasonableness checks (range limits and rate-of-change /
slew-rate limits)** at the ingestion boundary before data can enter belief arbitration.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L471-L540\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L471-L540):
  Neither `EpistemicModifier` nor `ArbitrationPolicy` supports physical boundary checks or slew-rate
  limits.
- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L128-L132\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L128-L132):
  Wildly differing facts (`(10.0, 20.0)` vs `(45.0, 80.0)`) are ingested side-by-side without any
  kinematic check whether a jump from $(10, 20)$ to $(45, 80)$ is physically attainable within the
  elapsed time.

**Fix:** Add declarative **Reasonableness and Kinematic Slew Limits** to predicate arbitration
policies:

```vagent
predicate at(key who: Agent, x: Float, y: Float)
  arbitrated by truest(
    bounds_x: [-1000.0, 1000.0],
    bounds_y: [-1000.0, 1000.0],
    max_slew_rate: 15.0m_s,     // Max velocity: 15 m/s
    max_accel: 5.0m_s2          // Max acceleration: 5 m/s^2
  )
```

Lower this policy in `compiler.cpp` to emit inline validation checks during percept assertion or
arbitration. Percepts violating kinematic limits are rejected immediately, preventing impossible
teleportation beliefs from contaminating deliberation.

______________________________________________________________________

## MINOR

### 8. (§7.1, line 738) Percept Declarations Lack Sensor Quality Telemetry and Covariance

Full title: Worked Examples Model Sensors as Plain Raw Primitives Without Uncertainty Telemetry

**Problem:** In Example 1 (§7.1), sensor percepts are defined with primitive coordinates only:

- `GPSFix as fix { fix.lat, fix.lon }`
- `OdometryStep as odom { odom.x, odom.y }`
- `SLAMMatch as slam { slam.x, slam.y }`

Real industrial sensor drivers provide critical quality indicators alongside raw values:

- GNSS: Fix type (RTK, 3D, DGPS), Horizontal Dilution of Precision (HDOP), number of tracked
  satellites, estimated horizontal accuracy ($\\sigma$).
- LiDAR/Vision: Inlier count, feature tracking confidence, covariance matrix $\\mathbf{P}$, match
  residual.
- IMU: Accelerometer clipping flags, calibration temperature, gyro angular random walk estimate.

Omitting sensor uncertainty telemetry forces the dynamic weighting system to guess sensor quality
solely from generic environmental context (e.g. ambient light), ignoring the sensor's own onboard
diagnostic telemetry.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L738-L748\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L738-L748):
  Percept handlers ingest raw coordinates without quality flags or covariance metrics.

**Fix:** Enrich the worked examples to demonstrate idiomatic representation of sensor quality
telemetry:

```vagent
on percept GPSFix as fix {
  if fix.hdop < 2.5 and fix.satellites >= 6 {
    assert at(self, fix.lat, fix.lon) from "sensor:gps"
      with variance: fix.horizontal_accuracy ** 2
  }
}
```

______________________________________________________________________

### 9. (§4.3, line 548) Lack of Sensor Recalibration and Fault Recovery Directives

Full title: Surface Syntax Missing Directives for Hardware Recalibration and Fault Acknowledgment

**Problem:** Section 4.3 provides `confirm`, `refute`, `reinforce source`, and `penalize source`.
However, it lacks operational commands for managing physical sensor lifecycles:

- `recalibrate source "sensor:imu"`: Initiates a stationary Zero Velocity Update (ZUPT) or bias
  estimation routine.
- `reset source "sensor:gps"`: Clears transient fault counters and reinstates a tripped sensor after
  reboot.
- `isolate source "sensor:wheel_odometry"`: Manually disconnects a slipping or damaged encoder from
  arbitration without waiting for statistical decay.

Without these, agents cannot execute physical sensor recovery maneuvers when faults are detected.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L548-L561\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L548-L561):
  Only statistical trust modifiers are exposed.

**Fix:** Add operational sensor control directives to `vagent` surface syntax:

```ebnf
SensorDirective = ( 'recalibrate' | 'isolate' | 'reinstate' ) 'source' StringLiteral [ 'with' RecordLiteral ] ;
```

Lower these to runtime helpers that trigger calibration routines, toggle `$health_state`, and clear
fault trip counters.

______________________________________________________________________

### 10. (§3.3, line 368) Context Feature Vector Omits Hardware Diagnostic Telemetry

Full title: Online Learning Context Vector Focuses on Environment While Ignoring Sensor Bus Health
and Latency

**Problem:** Section 3.3 presents the context vector:

```math
\mathbf{x} = \begin{bmatrix} \text{ambient\_light} \\ \text{sensor\_temperature} \\ \text{speed} \\ \text{battery\_level} \\ \text{distance\_to\_target} \end{bmatrix}
```

While `sensor_temperature` and `ambient_light` are relevant, this vector ignores the most predictive
hardware indicators of physical sensor failure:

1. **Packet Age / Latency Jitter**: Milliseconds elapsed between hardware measurement acquisition
   and engine turn ingestion.
1. **Signal-to-Noise Ratio (SNR)**: Average satellite carrier-to-noise ratio ($C/N_0$) or LiDAR
   return intensity.
1. **Bus CRC / Frame Error Rate**: Intermittent packet drops on serial/SPI/CAN links preceding a
   hard disconnect.
1. **Vibration Spectrum / High-Frequency Shock**: Dynamic vibration saturating MEMS proof masses.

**Evidence:**

- \[design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L372\](file:///data/git/gleditor/design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md#L372):
  Feature vector is restricted to external environmental conditions.

**Fix:** Clarify in §3.3 that $\\mathbf{x}$ is domain-configurable and can include hardware bus
diagnostics (`bus_error_rate`, `packet_latency_ms`, `snr_db`, `vibration_rms`) alongside
environmental percepts.

______________________________________________________________________

VERDICT: NOT CONVERGED
