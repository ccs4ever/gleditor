# Round 3 critique: Hardware Fault Specialist

Target: `design/vagent-dynamic-source-weighting-and-epistemic-arbitration.md` v1.2\
Previous status: Round 2 `NOT CONVERGED` (0 BLOCKING, 4 MAJOR, 3 MINOR).\
Round 3 tally: **0 BLOCKING, 4 MAJOR, 3 MINOR**.

## Round 2 finding status

- **H2.1 health state machine unwired — PARTIALLY RESOLVED.** The state transitions and FAULTED
  exclusion are now specified in §2.3, §5.1, and §5.3 (lines 350–366, 741–747, 822–832). But §4.4
  says ingestion gates run once per assertion and queries only read their verdict (721–723), while
  §5.3 says consensus evaluation runs the gates and stages their health-state effects (811–832).
  §6.3 documents a settle queue for `confirm`/`refute`, without specifying how a pure query enqueues
  a gate result (886–898). Also no heartbeat, timeout, or stuck-output detector drives health when a
  source stops changing or stops reporting. See MAJOR 1.
- **H2.2 drift disconnected — RESOLVED as a wiring/example finding.** The profile has
  `$sensor_kind`, `$r0`, `$q_drift`, an expansion formula, and the rover example now integrates
  increments before asserting a pose (338–340, 408–422, 925–930). The remaining physical-model and
  anchor semantics are a new MAJOR 2, not the old missing-plumbing defect.
- **H2.3 slew gating absent — RESOLVED in the specification.** `max_slew` is declared and its gate
  is described in §5.3, and rejected values are excluded from arbitration (629–630, 782–791). The
  worked example still omits the required `cluster_tolerance`; see MINOR 1.
- **H2.4 variance ingestion/posterior discarded — RESOLVED for scalar isotropic measurements.**
  `with variance`, profile `$r0`, `MissingVariance`, and returned uncertainty are specified
  (653–660, 793–809, 838–849). The interface still silently narrows general vector/matrix covariance
  to scalar isotropic noise; see MAJOR 3.
- **M5 prior for innovation gate — RESOLVED textually** by defining propagated prior or median/MAD
  fallback (818–820); the fallback needs further caveats (MINOR 2).
- **M6 cluster tolerance policy omission — RESOLVED in §4.2** (644–648); one worked example fails to
  follow it (MINOR 1).
- **M7 recovery directives — RESOLVED** by `reset`, `isolate`, and `degrade` (662–701).

## MAJOR

### 1. Health transitions have no single executable trigger path; silent and stuck…

**Full title and location:** 1. (§4.4, §5.1, §5.3, §6.3) Health transitions have no single
executable trigger path; silent and stuck sources remain unobserved

**Problem.** The spec makes expressions pure and says the query consumes an ingestion verdict
(721–723), but §5.3 assigns innovation/slew gate execution and health-state updates to consensus
evaluation (782–832). The truest path also needs an “ingestion verdict accepted?” (745–747), but
neither a verdict field nor the event that creates it is defined. The documented settle queue is
specifically triggered by `confirm`/`refute` statements (893–898); no matching queue contract covers
gate outcomes. As written, implementers can reasonably create a query-time mutation (violating
purity) or no health transition at all. More seriously, the trip counter advances only on rejected
observations. A frozen but plausible value may keep passing; a disconnected or silent device sends
no rejection to trip. A stuck-at output is a recognized sensor fault mode, and gating alone cannot
reliably detect it.

**Evidence.** [§4.4 lines 707–723](baseline-v1.2.txt#L707),
[§5.1 lines 741–747](baseline-v1.2.txt#L741), [§5.3 lines 811–832](baseline-v1.2.txt#L811),
[§6.3 lines 886–898](baseline-v1.2.txt#L886). A recent FDD review catalogs stuck, drift, bias,
noise, hysteresis, saturation, spike, and abrupt faults as distinct mechanisms
([Jombo et al., IFAC SAFEPROCESS 2024][source-1]).

**Minimal fix.** Choose one lifecycle: preferably validate each timestamped percept at ingestion,
emit an immutable accepted/rejected/status event, and apply its status/counter update in a
documented settle phase; make `truest` and `consensus` read only the persisted verdict. Specify
freshness/heartbeat timeout and a stuck detector appropriate to source kind (e.g. no-change over a
meaningful input excitation), as separate flags from innovation rejection. State limits: a constant
real-world quantity is indistinguishable from a stuck sensor without excitation or independent
evidence, and a timeout identifies missing data, not its cause.

### 2. The dead-reckoning model uses the wrong anchor and one cubic law is asserted…

**Full title and location:** 2. (§2.3, §3.1, §5.3) The dead-reckoning model uses the wrong anchor
and one cubic law is asserted for unlike mechanisms

**Problem.** `$anchor` is defined as the time of the last `confirm` or `reset` (337), but §3.1 calls
it time since the last absolute correction (419), and §5.3 requires exactly that correction time
(793–799). An ordinary accepted GNSS/landmark correction is not specified to update the profile
anchor, so dead-reckoning uncertainty can grow indefinitely after successful corrections. Also,
`R0 + Q_drift * dt^3` is not a generic covariance law for both IMU and wheel odometry. Inertial
error depends on the integrated noise/bias model and state dynamics; a constant accelerometer bias
gives position error proportional to time squared (and its variance scales with time to the fourth
if bias is modeled as random), while wheel encoder uncertainty often depends on distance,
quantization, wheel-radius error, and slip. The document’s single scalar `$q_drift` cannot express
those state- and axis-dependent mechanisms. Cambridge’s inertial-navigation note explicitly
distinguishes error sources and shows constant accelerometer bias double-integrating into quadratic
position error ([Woodman, 2007][source-2]).

There is also a covariance precedence ambiguity: §5.3 first selects clause `with variance:` or
profile `$r0`, then states dead-reckoning uses `$r0 + Q dt^3` (793–803). It is unclear whether a
per-observation `est.r0` replaces `$r0` or is the baseline to which drift is added. In §7.1 the
estimate supplies `est.r0`, so a reader can double-count or discard estimator-provided uncertainty.

**Evidence.** [§2.3 lines 337–340](baseline-v1.2.txt#L337),
[§3.1 lines 408–422](baseline-v1.2.txt#L408), [§5.3 lines 793–803](baseline-v1.2.txt#L793),
[§7.1 lines 925–930](baseline-v1.2.txt#L925).

**Minimal fix.** Treat the upstream estimator’s timestamped covariance and process model as
authoritative for integrated pose; define a generic dead-reckoning profile only as a documented
fallback with units and model domain. Update its correction epoch on each accepted absolute
correction, separately from `confirm`/`reset`. Define whether assertion variance is the full current
measurement covariance or only the baseline, and prohibit adding the same process uncertainty twice.
Do not claim one exponent applies to every IMU/encoder.

### 3. Gaussian fusion still assumes independence while common-cause grouping…

**Full title and location:** 3. (§3.2, §5.3, §8.2) Gaussian fusion still assumes independence while
common-cause grouping applies only to discrete log-odds

**Problem.** The scalar correlation group and effective-sample discount are defined for Bayesian
testimony in §3.2 (455–464), but the covariance fusion equation in §5.3 independently sums every
source’s information matrix (841–843). This is only valid under strong assumptions: observations
refer to the same state at compatible times and frames, and measurement errors are independent (or
cross-covariance is otherwise accounted for). Sensors can share GNSS corrections/constellations,
clock and coordinate transforms, a common map or perception model, power/ground, communications bus,
mounting/calibration, or the same environmental obstruction. Two sensors on different buses can
still share these common causes; two sensors on one bus need not have correlated measurement errors.
The existing I2C/gossip group heuristic is therefore neither a sufficient model nor applied to
continuous fusion. Treating correlated estimates as independent makes the reported posterior
covariance overconfident. Multisensor fusion literature explicitly treats cross-covariance/common
process noise as necessary when estimates are dependent ([Bar-Shalom & Campo, 1986, DOI][source-3];
see also [IEEE review of track-to-track fusion][source-4]).

**Evidence.** [§3.2 lines 455–464](baseline-v1.2.txt#L455) limits correlation handling to combined
warrant; [§5.3 lines 811–843](baseline-v1.2.txt#L811) has no fusion cross-covariance or common-mode
term; [§8.2 lines 1059–1065](baseline-v1.2.txt#L1059) prices a values/variances-only routine.

**Minimal fix.** State the independence/same-state/same-time assumptions as preconditions and
represent provenance/common-mode groups in continuous fusion too. Where cross-covariance is known,
include it; where it is unknown, use a conservative method such as covariance intersection or refuse
to claim a calibrated posterior. A group label or arbitrary scalar `rho` is only a conservative
heuristic unless justified by a measured correlation model; it cannot make shared fault modes
disappear.

### 4. Values have no enforced coordinate frame, units, or observation-time…

**Full title and location:** 4. (§4.2, §5.3, §7.1) Values have no enforced coordinate frame, units,
or observation-time contract before distance gating and fusion

**Problem.** The rover example sends GPS latitude/longitude, odometry pose, and SLAM coordinates
into the same `at(self, x, y)` predicate (921–933), but they are not generally expressed in the same
units or frame. Latitude/longitude are angular coordinates; odometry and SLAM commonly use local
metric frames, potentially with different origins and axes. The equations compute Euclidean
distance, use a scalar isotropic variance, and fuse raw coordinates, without a transform, datum, or
measurement timestamp. A coordinate-frame/unit mismatch invalidates `cluster_tolerance`, `max_slew`,
innovation distance, and the fused mean regardless of source reliability. GPS accuracy also depends
on local signal blockage, multipath, atmospheric conditions, and receiver features; its reported
variance alone cannot promise integrity ([GPS.gov accuracy guidance][source-5],
[FAA AIM on GNSS interference][source-6]).

**Evidence.** [§4.2 lines 621–660](baseline-v1.2.txt#L621),
[§5.3 lines 782–820](baseline-v1.2.txt#L782), [§7.1 lines 921–933](baseline-v1.2.txt#L921). The
cited guidance says user accuracy depends on satellite geometry, blockage, atmosphere, and receiver
design, with multipath and interference among degrading factors ([GPS.gov][source-7]).

**Minimal fix.** Require every continuous predicate policy to name the canonical coordinate frame,
units, and measurement-time basis. Transform each percept into that frame before
storage/gating/fusion; reject incompatible or stale measurements. Include the observation transform
in the covariance, and be explicit that a reported accuracy estimate is not an integrity guarantee
or spoofing detector.

## MINOR

1. **(§7.1) Worked policy violates the new checker contract.** The `at` predicate has `Float`
   arguments but no `cluster_tolerance`, despite §4.2 requiring it for any non-key float argument
   and returning `MissingClusterTolerance` (644–648, 907–909). Add the required tolerance (and a
   matching policy for `max_slew` if the example is intended to exercise that gate).
1. **(§5.3) Median/MAD cold-start gate is underspecified.** Median/MAD over one or two observations
   has no robust rejection power, and measurement covariance is not incorporated into the fallback
   prior. Specify warm-up behavior and a fail-safe/abstain condition; a population median is not
   independent ground truth and can reject a valid minority when faults are correlated.
1. **(§2.3, §5.3) Fixed trip/inflation constants are not portable across sampling rates or sensor
   classes.** A five-reject trip means milliseconds at high-rate IMU versus hours for a slow
   environmental probe; fixed multipliers 2 and 4 need per-profile calibration or
   time-window/evidence-based defaults, documented as heuristic rather than universal safety
   settings.

## Sensor fault model and compensation boundary

A useful design split is by the value being estimated and the dominant error process, not by a
single `ABSOLUTE`/`DEAD_RECKONING` label:

- **IMU:** accelerometer/gyro white noise, bias instability/random walk, temperature dependence,
  scale-factor/cross-axis error, vibration, saturation, axis or packet faults. Propagate a state
  covariance in the estimator; use temperature/calibration telemetry, range/clipping checks,
  residuals against independent motion constraints, and an absolute aiding update. No simple trust
  score or one `dt^3` term removes unobservable bias.
- **Wheel/visual odometry:** increments or relative pose, with errors driven by distance, turns,
  wheel-radius/track calibration, slip, texture/lighting, and scene geometry. Integrate in a pose
  estimator and grow covariance from the motion/process model; use GNSS, landmarks, or loop closure
  to bound drift. Slip can be common to left/right encoders, so two encoder channels are not
  independent confirmation.
- **GNSS:** absolute global fix, with changing geometry, blockage, multipath, atmospheric delay,
  interference/jamming/spoofing, receiver mode, and correction-service dependencies. Gate
  quality/integrity indicators and innovation, but retain independent inertial/landmark checks; a
  plausible coherent spoof can pass ordinary innovation gating. FAA advises monitoring anomalies and
  planning to operate without GPS ([FAA AIM][source-8]); GPS.gov lists multipath and blockage among
  accuracy factors ([GPS.gov][source-9]).
- **Camera/LiDAR/radar perception:** outputs are detections/features with data-association and
  algorithm confidence, not necessarily Gaussian point measurements. Occlusion, glare/low light,
  rain/fog, dirty windows, motion blur, calibration/extrinsic drift, false returns, and shared
  model/training failures alter bias and missed-detection rates, often nonlinearly. Carry
  modality-specific quality/residual/visibility signals and detection likelihoods; do not convert a
  detector score directly into a physical covariance without calibration. An NHTSA test report found
  degradation varied with sensor and condition; rain affected tested camera and LiDAR performance
  ([NHTSA ESV sensor characterization][source-10]).
- **Environmental/industrial probes** (temperature, pressure, flow, gas, dissolved oxygen):
  offset/gain drift, aging/poisoning, hysteresis, response lag, fouling, range saturation,
  wiring/ADC faults, and calibration drift are common. A stuck value can look valid in a steady
  process. Use process-model residuals, calibration/maintenance records, range and rate checks, and
  redundant reference channels where independence is justified. NIST documents that thermocouple
  drift in prolonged elevated-temperature service is an application concern
  ([NIST thermocouple calibration/drift report][source-11]).

Compensation is bounded: analytic redundancy works only when the process model is adequate and
faults are observable; thresholds trade missed detections against false alarms; redundancy does not
help against shared power, environment, calibration, software, map, or GNSS correction failures. For
safety-relevant action, expose disagreement/unknown state and require an independent integrity
mechanism or conservative fallback; epistemic reputation is not a safety certification.

## References

- [GPS.gov, GPS Accuracy][source-12]
- [FAA Aeronautical Information Manual, GNSS interference][source-13]
- [Woodman, An Introduction to Inertial Navigation (University of Cambridge, 2007)][source-14]
- [Bar-Shalom & Campo (1986), common process noise][source-15]
- [NHTSA, A Method for the Characterization of Perception Sensors][source-16]
- [Jombo, Zhang & Lu (IFAC 2024), sensor fault diagnosis][source-17]
- [NIST, CSTL Annual Report FY04: Thermocouple drift characteristics][source-18]

VERDICT: NOT CONVERGED

[source-1]: https://uhra.herts.ac.uk/id/eprint/14536/
[source-10]: https://www-nrd.nhtsa.dot.gov/pdf/ESV/Proceedings/27/27ESV-000160.pdf
[source-11]: https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7202.pdf
[source-12]: https://www.gps.gov/gps-accuracy/
[source-13]: https://www.faa.gov/air_traffic/publications/atpubs/aim_html/chap1_section_2.html
[source-14]: https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-696.pdf
[source-15]: https://doi.org/10.1109/TAES.1986.310815
[source-16]: https://www-nrd.nhtsa.dot.gov/pdf/ESV/Proceedings/27/27ESV-000160.pdf
[source-17]: https://uhra.herts.ac.uk/id/eprint/14536/
[source-18]: https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7202.pdf
[source-2]: https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-696.pdf
[source-3]: https://doi.org/10.1109/TAES.1986.310815
[source-4]: https://ieeexplore.ieee.org/document/572239/
[source-5]: https://www.gps.gov/gps-accuracy/
[source-6]: https://www.faa.gov/air_traffic/publications/atpubs/aim_html/chap1_section_2.html
[source-7]: https://www.gps.gov/gps-accuracy/
[source-8]: https://www.faa.gov/air_traffic/publications/atpubs/aim_html/chap1_section_2.html
[source-9]: https://www.gps.gov/gps-accuracy/
