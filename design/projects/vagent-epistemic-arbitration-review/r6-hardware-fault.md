# Round 6 review: Hardware Fault Specialist

Reviewed the full v1.3 design and frozen approved input. The frozen input is byte-identical to the
branch document (SHA-256 `c4c9506fd39f40f673931b35f37a6abac6e0510d0ec176507167dcafe050693f`).
P19.1–P19.8 approval is recorded in `approval-p19.md`.

## Previous finding status

- **R4 hardware MINOR 1 (authorized reset/verifier policy): RESOLVED.** §4 now gives
  reset/isolate/degrade and verification separate host-issued grants, bounded scopes and expiry,
  deny-by-default validation, and recorded principal/grant decisions. Sensor identity, transport
  authentication and sensor reliability do not confer those permissions.
- Round 4's sensor-specific limits remain intact: health and observation validity are separate;
  reset does not claim physical recalibration; no universal drift law or unsupported correlated
  fusion was introduced.

## Tally

**0 BLOCKING, 0 MAJOR, 1 MINOR.**

## MINOR

### 1. Make the shared measurement time part of the alignment contract

**Full title and location:** 1. (§4–5.3, lines 323–324, 405–407, 458–478) Make the shared
measurement time part of the alignment contract

**Problem:** The design correctly requires the same physical state and says a native
estimator/transform supplies aligned values. However, the policy exposes a duration-valued
`alignment` tolerance alongside clock mapping and freshness, without saying whether that tolerance
bounds residual clock/time uncertainty after alignment or simply permits observations whose
timestamps differ by that duration. The latter interpretation admits valid independent readings of a
moving target that are not measurements of the same state, then averages them as one state.

**Counterexample:** A target moves at 20 m/s along one axis. Two independent sensors report x=0 m at
t=0 and x=1 m at t=0.05 s. Both have small positive isotropic variance and satisfy a 0.05 s
alignment window. Direct precision fusion returns x=0.5 m, which is not the target position at
either acquisition time. Frame/unit equality and independent sensor noise do not repair temporal
misalignment. The spec's estimator-alignment requirement can prevent this, but the policy's time
tolerance should not be mistaken for the alignment operation itself.

**Minimal fix:** State that consensus inputs must already describe the same state at one declared
target time. The tolerance may bound residual mapped-clock uncertainty/alignment error; it cannot
authorize fusion of raw observations at different state times. If an upstream estimator
aligns/extrapolates a sample, it must provide the resulting full current variance, including its
supported temporal/process uncertainty, under the existing scalar-isotropic contract. Otherwise
abstain. Add the moving-target example to the existing incompatible-times fusion test.

**Source check:** Geneva, Eckenhoff and Huang's primary ICRA paper on asynchronous sensor fusion
explains that different sampling rates produce asynchronous observations and evaluates explicit
measurement alignment against naive fusion without alignment: [paper PDF][source-1]. This supports
the narrow distinction between clock tolerance and state-time alignment; it does not imply the
proposal must adopt that paper's estimator.

## Hardware and sensor checks with no further finding

- Per-observation acquisition time, mapped clock version/uncertainty and sequence are distinct from
  receive-time liveness. Restart invalidates the old steady-clock comparison absent remapping.
  Stale/future/replayed inputs are refused.
- The current full variance is attached to each continuous observation; estimator propagation owns
  process evolution, and P19 does not add a generic drift term. Reset invalidates old observations
  and learned/calibration state but explicitly does not reset the physical estimator or assert
  recalibration.
- Independent propagated references are used only for gating, never fused as a second measurement;
  the candidate and prior fused outputs containing the same event are excluded as corroboration. The
  design explicitly rejects unknown dependence and identifies shared aiding, maps, clocks,
  calibration and process noise as dependencies.
- Scalar `r I_k` fusion is limited to same-state aligned measurements with independent errors.
  Anisotropic/correlated covariance remains refused/deferred; the returned variance is explicitly
  heuristic absent calibration. This preserves the approved scope without demanding a matrix API.
- Silence, missing samples, freeze evidence and false-positive limits are distinct. Receive-time
  heartbeat expiry does not diagnose cause; a stuck constant output needs excitation, independent
  constraints or device telemetry. Slew checks require positive time separation and do not divide by
  duplicate/out-of-order intervals. Inconsistency is not automatically source attribution.
- Health is latched separately from individual warning reasons. Accepted fixes only clear the
  innovation streak; recovery percepts clear matching warnings; faulted readings are excluded; reset
  requires authority and a new eligible reading. Health inflation is explicitly applied once before
  gating/fusion, and query reads do not mutate health, counters, timestamps or training state.
- Event ID deduplication, observation-bound verification, current health reads and generator
  belief-version checks make relayed evidence, delayed completions, source resets and evolving
  health observable. No additional physical-model defect was found in those paths.

## Primary-source checks

The existing Cambridge inertial-navigation report supports the document's distinction between bias,
white-noise and temperature-dependent inertial errors and their different integrated effects:
[Woodman, *An Introduction to Inertial Navigation*][source-2]. The existing common-process-noise
citation is bibliographically verified as Bar-Shalom and Campo, “The Effect of the Common Process
Noise on the Two-Sensor Fused-Track Covariance,” IEEE TAES 22(6), 1986, DOI
[10.1109/TAES.1986.310815][source-3]. The Round 6 sensor table remains appropriately qualified and
does not claim these sources establish a universal detector.

VERDICT: NOT CONVERGED

[source-1]: https://pgeneva.com/downloads/papers/Geneva2018ICRA.pdf
[source-2]: https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-696.html
[source-3]: https://doi.org/10.1109/TAES.1986.310815
