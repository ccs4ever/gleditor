# Round 8 critique: Hardware Fault Specialist (r8-input-v1.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7.H1 (shared physical measurement time): RESOLVED — §5.2 names a common target time and requires
  each input to estimate that state, with an explicit moving-target counterexample.

## BLOCKING

None.

## MAJOR

### 1. (§2.1, lines 83 and 225–230) Persist per-observation clock uncertainty

**Problem:** The observation record persists `observed time`, `received time`, and a clock mapping
version, while §3.2 separately requires a mapping uncertainty. It does not say that the uncertainty
value (or immutable mapping snapshot sufficient to reproduce it) is stored with each observation.
The version alone cannot reproduce the uncertainty if a mapping is revised or retired. Then a
coalescing query cannot verify that the sample's residual mapped-clock error satisfies its alignment
tolerance after migration or restart. This matters even when every sensor supplies a valid physical
variance: temporal alignment error is a distinct contributor and shared clocks are an explicitly
listed common failure cause (§5.2, lines 526–532).

**Evidence:** The required observation fields at `r8-input-v1.4.txt:79–86` include a clock mapping
version but no mapping uncertainty. The time contract at `r8-input-v1.4.txt:225–230` requires both
mapped acquisition time and mapping uncertainty, and restart/reboot must establish a new mapping or
expire affected observations. The coalescence contract bounds residual clock/alignment error at
`r8-input-v1.4.txt:465–473`. Runtime checkpoints preserve actor values across resume
(`tests/xuzz/vagent_resume_test.cpp:254–280`), while turn times use wall-clock milliseconds
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1401–1408`); therefore any P19 timestamp and
uncertainty needed after resume must be part of migrated state, rather than inferred from the new
host clock.

**Fix:** Add an owned, immutable per-observation mapped acquisition time and mapping uncertainty
(including the mapping/clock epoch and units) to the observation record, or specify an equivalent
versioned mapping record whose old versions remain addressable for the full observation lifetime.
Require the query to compare the propagated uncertainty against its alignment tolerance, and
invalidate/expire observations when resume cannot restore that mapping. Include a migration and
checkpoint test that changes the host clock epoch and proves old observations either retain the same
mapping and uncertainty or become ineligible.

### 2. (§5.1, lines 440–452; §8.2, lines 775–782) Specify fault-latch migration and clock rebase

**Problem:** The proposal defines authorized reset, persistent warning reasons, latching, and source
incarnation changes, but its lifecycle rules do not explicitly bind those invariants to P17 upgrade
reconciliation and checkpoint/resume. This is material because the live upgrade API delegates all
state copying to a caller-supplied reconciler, then swaps the actor context and grant
(`apps/common/xanadu/vortex/mesh_actor.cpp:683–731`); there is no generic field allowlist or health
state migration in that path. Resume reconstructs the executor around a new host and clock
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3408–3461`). The implementation already keeps
in-flight effects in an executor-side table and fails effect waits as migrated
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:2673–2689`), so P19 cannot rely on those
runtime tables to preserve pending native epistemic work. Without a required migration rule, an
upgrade could drop a latched FAULTED state or clear a degradation reason, while a resumed heartbeat
deadline could be judged against a different wall-clock epoch. That would admit a source without
authorized reset or make silence age incomparable.

**Evidence:** The health contract says `isolate` latches FAULTED and only authorized `reset`
unlatches it; recovery clears only its own reason (`r8-input-v1.4.txt:440–450`). Heartbeat expiry is
based on receive time (`r8-input-v1.4.txt:452`). Reconciliation migration order and checkpoint
preservation are specified generally at `r8-input-v1.4.txt:134–139`, but no rule says fault latches
and reason provenance survive upgrades/resume or how their deadlines are rebased. The acceptance
gate names “checkpoint clock rebasing” at `r8-input-v1.4.txt:780–782`, but does not define the
required behavior. `MeshActorRuntime::upgrade` invokes the external reconciler and adopts its
returned root (`apps/common/xanadu/vortex/mesh_actor.cpp:718–730`); the resume test preserves timer
values unchanged across hosts (`tests/xuzz/vagent_resume_test.cpp:254–280`), demonstrating that
executor deadlines already have migration semantics that need explicit treatment.

**Fix:** Make P19.1 migration semantics explicit: preserve source identity/incarnation, latched
fault, per-reason provenance, reject counters, and health policy across upgrade unless an authorized
reset or declared new source epoch explicitly changes them. For resume, persist the clock
basis/epoch and last receive time; rebase remaining durations from a trusted paired clock sample or
expire the affected liveness state and require fresh authenticated input. Do not silently clear a
latch or compare raw steady-clock values across reboot. Add upgrade and resume acceptance cases for
a latched fault, an uncleared warning, a recovered single reason, and a heartbeat whose old clock
epoch is unavailable.

## MINOR

None.

VERDICT: NOT CONVERGED
