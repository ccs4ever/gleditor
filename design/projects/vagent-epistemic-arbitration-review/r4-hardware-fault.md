# Round 4 follow-up: Hardware Fault Specialist

Target: `/tmp/vagent-epistemic-r3/revision-v1.3.md`, Version 1.3 draft.\
Previous status: Round 3 **NOT CONVERGED**, 0 BLOCKING, 4 MAJOR, 3 MINOR.\
Round 4 tally: **0 BLOCKING, 0 MAJOR, 1 MINOR**.

## Status of Round 3 findings

- **R3.1 health update path and missing-sample/freeze coverage — RESOLVED.** §5.1 makes validation
  and the health update one ingestion/health turn; query evaluation reads committed verdicts and
  explicitly cannot run gates, change counters, refresh timestamps, train, or enqueue verification
  (lines 350–354). Health state separates a latched fault from individual degradation/suspicion
  reasons, accepted readings clear only the innovation streak, and only the corresponding health
  percept clears a warning (356–361). Receive-time heartbeat expiry schedules a health turn even
  without a sample; freeze detection requires excitation, independent constraints, or device
  telemetry, and the draft states the limit that constant reality cannot be distinguished from a
  stuck output by repetition alone (370–374). These are concrete and appropriately bounded.
- **R3.2 generic cubic drift and anchor/variance ambiguity — RESOLVED.** The estimator supplies the
  full current variance; fusion adds no second generic drift term. Absolute aiding updates the
  estimator covariance and correction epoch; `confirm` and `reset` are explicitly not physical
  corrections (190–195). §5.2 preserves the full-current-variance contract and rejects unsupported
  covariance (383–386). The example labels `pose.variance` as including integrated wheel/IMU drift
  (326–327). This avoids claiming one process law for unlike hardware.
- **R3.3 correlated-source overconfidence — RESOLVED for initial scope.** U-C5 requires compatible
  measurements and independent errors, with unknown dependence refusing calibrated fusion (46). §5.3
  refuses fusion with `DependentMeasurements` when dependence/cross-covariance is unknown and
  explicitly lists common maps, perception models, clocks, aiding data, calibration and process
  noise as dependence sources (427–433). It says known correlated fusion and covariance intersection
  are deferred; §9 records matrix/correlated fusion as an extension (626). I do not request those
  deferred APIs in this review.
- **R3.4 frame, unit and time mismatch — RESOLVED.** Continuous predicates require canonical frame,
  units and cluster tolerance, and ingress validates declared schema because the checker cannot
  infer units from `Float` (297–302). Observation records include observed/received times, clock
  mapping and frame/unit schema (74–84); assertions use authenticated percept metadata (282–288).
  The example transforms GNSS into `map_enu` and supplies full current variance in metres squared
  (315–327). §5.2 requires same state/key/frame/unit/time basis, propagated transform uncertainty,
  alignment/freshness limits, and abstention when incompatible (378–386). The timed innovation/slew
  rules also require a compatible positive time interval (388–402).
- **R3 MINOR 1 missing cluster policy — RESOLVED.** The revised `at` declaration supplies canonical
  frame, units, clock mapping, alignment and `cluster_tolerance` (315–320).
- **R3 MINOR 2 median/MAD cold start — RESOLVED.** The draft no longer uses the candidate population
  as a cold-start prior. It marks the gate `UNTESTED`, does not train or blame a source, refuses a
  calibrated fused result, and requires host-validated initialization or an independent reference
  (404–407). The tests explicitly cover refusal under cold start (601–603).
- **R3 MINOR 3 hard-coded trip/inflation constants — RESOLVED.** Defaults are explicitly
  configurable per profile and tied to a declared sample-rate/window interpretation; the draft calls
  them engineering heuristics, not universal settings (363–365).

## Authority check

The draft distinguishes a source name from authenticated identity and says an unprivileged plan
cannot fabricate source/time annotations (86–90, 282–288). Verification requires an authorized
verifier, observation identity, matching values, and independent evidence; a matching value alone is
insufficient (496–500). Reset is explicitly authorized and cannot claim physical recalibration
(356–361). This closes the sensor-claim authority concern at the proposal level.

One small integration detail remains: “authorized reset” and “authorized verifier” do not yet name
the capability/policy that grants either role. This branch explicitly lacks the actor executor and
its runtime ABI, and P19 is unimplemented (20–25, 629), so this is an integration contract to bind
when those prerequisites are specified, not a defect in the sensor arbitration math.

## MINOR

### 1. Bind authorized health actions to an explicit capability policy

**Full title and location:** 1. (§5.1, §6.3, §8/implementation prerequisites) Bind authorized health
actions to an explicit capability policy

**Problem.** The contract correctly restricts reset and verification, but does not identify how the
future host/runtime maps an authenticated principal to `reset` or verification authority. Without
that binding, implementations may mistake transport authentication or source identity for permission
to clear a latched hardware fault.

**Evidence.** Reset is “only authorized” at §5.1 lines 356–361; verification requires an “authorized
verifier” at §6.3 lines 496–500; actor executor/runtime ABI are absent prerequisites at lines 20–25.

**Minimal fix.** In the P17/P18 integration prerequisite, require a host-provided capability/policy
check for `reset` and verifier roles, record the principal and decision in the health/verification
event, and reject unauthenticated or unauthorized requests. Keep authorization distinct from sensor
reliability and from a sensor’s own health report. The document already says P19 remains unapproved
and unimplemented, so implementation details can remain deferred until that boundary is supplied.

## Remaining limits accepted for this review

The source table covers inertial, wheel/visual odometry, GNSS, camera/LiDAR/radar, industrial
probes, and power/timing/transport failure causes (531–553), and correctly limits compensation to
observability, independent evidence, and explicit abstention. GNSS coherent spoofing may pass
innovation gates; steady real signals make stuck detection ambiguous; health flags identify symptoms
rather than root cause. Scalar isotropic fusion is the declared initial scope, and
anisotropic/correlated fusion is explicitly refused/deferred. I found no remaining sensor-specialist
BLOCKING or MAJOR issue within that scope.

VERDICT: CONVERGED
