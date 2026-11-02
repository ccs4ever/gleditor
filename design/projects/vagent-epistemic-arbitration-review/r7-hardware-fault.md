# Round 7 follow-up: Hardware Fault Specialist

Target: `/tmp/vagent-epistemic-r6/revision-v1.4.md`. This is a focused follow-up to the Round 6
hardware-fault finding; the revised target-time contract and the still-relevant sensor lifecycle
clauses were inspected.

## Prior finding status

- **R6.H1 (shared physical measurement time): RESOLVED.** §5.2 now declares a physical target time
  `t_star` for each consensus snapshot and requires every input to estimate the state at that time.
  Alignment tolerance is explicitly residual mapped-clock/alignment error, not permission to average
  asynchronous raw coordinates. An upstream estimator that propagates a sample must provide full
  current variance with supported temporal/process and transform uncertainty; otherwise the system
  abstains. The 20 m/s example directly rules out averaging x=0 at t=0 and x=1 at t=0.05 into x=0.5
  merely because the window is 0.05 s. The static-state exception also requires a declared model and
  variance that account for motion.

## Tally

**0 BLOCKING, 0 MAJOR, 0 MINOR.**

## Sensor-scope checks

The revised clause retains the approved scalar-isotropic boundary: it does not require a matrix
covariance API, and unsupported anisotropic uncertainty still abstains. Per-event variance remains
full current variance supplied by the estimator, which owns process evolution; no generic drift term
is added. The independent propagated reference remains gate-only and cannot reuse the candidate or a
prior fused output containing its event. Unknown cross-covariance/dependence remains a refusal. Time
mapping, receive-time liveness, health-only turns, stuck/silence limits, authorized reset, one-time
health inflation, and query purity remain explicit in the inspected sections. No new hardware/sensor
counterexample was found in the Round 7 changes.

VERDICT: CONVERGED
