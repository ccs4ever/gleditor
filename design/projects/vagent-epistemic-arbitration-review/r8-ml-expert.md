# Round 8 critique: ML Expert (v1.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7: no open ML findings. R7 recorded 0 BLOCKING and 0 MAJOR; the active-label correction/replay
  and full learner-key issues it revisited are explicitly addressed in v1.4.

## BLOCKING

None.

## MAJOR

### 1. (§2.1, lines 71–85) Coalesced clauses cannot own source-specific observations

**Problem:** The observation record is owned by a clause, but the integrated belief store coalesces
an identical ground term into one clause with multiple source cells. Consequently distinct source
events for the same proposition cannot each be represented by the clause's single observation
metadata record. If the first event's ID/source/time is attached to the clause, later sources
inherit the wrong observation identity and can be deduplicated, verified, decayed, or trained as
though they were the first event. A single clause-level record also cannot hold distinct per-source
calibration keys and event-time features. This breaks the required one-vote-per-original-observation
semantics and full-key delayed-label safety precisely when identical assertions are coalesced.

**Evidence:** The design says “a clause heads observation metadata” (§2.1, lines 71–73), while its
observation table requires a stable event ID and source identity/incarnation (lines 83–85). The
runtime explicitly defines “A clause's sources [as] peer cells on `d.source`”
(`apps/common/xanadu/vagent/runtime/compiler.cpp:3947–3951`). On an existing equal term it adds
another source to the same clause (`compiler.cpp:4172–4192`); only when no equal clause exists does
it create a new clause (`compiler.cpp:4195–4202`). Resume likewise copies the clause and its term
and sources as a unit (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3233–3247`). Thus one
clause may represent multiple independently received assertions.

**Fix:** Give each source assertion its own observation identity and metadata owner. Either make the
observation/source occurrence the owner (with the shared clause serving only as the keyed
proposition), or explicitly retain per-source observation records beneath the clause and make each
record—not the clause—the arbitration, deduplication, decay, and verification target. Define the
2-degree-safe rank topology and update migration/resume ordering to copy every such record and its
source/profile clone occurrence. State how an authorized correction/retraction maps back to the
exact source occurrence while preserving clause coalescing for query semantics.

## MINOR

None.

VERDICT: NOT CONVERGED
