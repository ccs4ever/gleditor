# Round 9 critique: Hardware Fault Specialist (r9-input-v1.5)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R8.H1 (persist per-observation clock uncertainty): RESOLVED — §2.1 and §3.2 retain mapped
  acquisition time, uncertainty, original clock basis/epoch, and an immutable snapshot or
  addressable mapping version for the observation lifetime.
- R8.H2 (fault-latch migration and clock rebase): RESOLVED — §2.2 explicitly preserves fault and
  warning state, rejects program/model/reboot as reset authority, and specifies checked heartbeat
  rebasing with elapsed downtime or conservative stale handling without a fresh grace period.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

## Verification notes

The immutable candidate hash matches the brief. The observation record now has all source-specific
time material needed for repeatable arbitration: mapped acquisition time and its uncertainty bound,
original clock basis/epoch, received time/basis, and immutable mapping version
(`r9-input-v1.5.txt:85–90`). §3.2 further requires retaining a mapping snapshot or addressable
version and prohibits evaluating an old observation using a replacement mapping
(`r9-input-v1.5.txt:337–344`). Together with the common physical target time and propagated variance
contract (`r9-input-v1.5.txt:590–599`), this closes the earlier gap for facts coalesced from
different sources.

The upgrade/resume translation now explicitly owns both lifecycle paths, compatibility checks,
complete ref remapping, refusal atomicity, and stale-query invalidation
(`r9-input-v1.5.txt:192–232`). Its hardware rules preserve fault latches, reason provenance,
counters, policy, and source binding; prohibit program/model upgrade or reboot from clearing a
fault; and require a checked policy for replacement/reset. Heartbeat state is rebased using a
trusted paired mapping with elapsed downtime and uncertainty, or made stale pending fresh
authenticated input; raw steady-clock epochs and restarted full heartbeat grace periods are
explicitly refused (`r9-input-v1.5.txt:234–242`). The rule that unknown mappings cannot clear a
latch avoids treating uncertainty as recovery.

The runtime evidence supports treating these as future P19 requirements rather than existing
behavior: checkpoint currently refuses host deliveries and effects in flight
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3351–3367`), and resume restores through its
own path (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3408–3461`). Generic upgrade only
calls the supplied reconciler and swaps context/grant
(`apps/common/xanadu/vortex/mesh_actor.cpp:718–730`). The candidate calls out the missing automatic
vagent upgrade pipeline and makes it a prerequisite, so it does not overclaim the callback as
fault-state migration.

Late native work is fenced by immutable observation identity and the complete learner key, with
cancelled/stale/mismatched results forbidden from replacing current features
(`r9-input-v1.5.txt:151–163,413–419,244–249`). It also requires external work to drain or be
verifiably cancelled at upgrade quiescence, and adds an integration regression for stale completions
after incarnation/model/schema changes (`r9-input-v1.5.txt:982–986,1002–1005`). This is sufficient
for the reviewed lifecycle concern; no new behavior is claimed in the existing executor.

VERDICT: CONVERGED
