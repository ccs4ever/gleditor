# Round 9 critique: ML Expert (v1.5)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R8.1 (Coalesced clauses cannot own source-specific observations): RESOLVED — v1.5 keeps the shared
  clause but gives each source-support cell its own bounded observation-history rank and each
  observation its own identity, time/uncertainty, variance, features, verdict and profile occurrence
  (§2.1, lines 94–125). Retraction transfers retained records; retired corrections cannot make an
  older observation current again.

## BLOCKING

None.

## MAJOR

None.

## Verification

- **Distinct observations and source attribution:** Same-tuple new events create a new
  observation/version only for that authenticated source; exact retries refresh nothing; another
  source retains an independent observation. The same-value, event retry and retention behavior is
  explicit (§2.1, lines 101–125). The edge recipe gives reciprocal rank/clone ownership and directs
  migration to reconstruct through a ref map (§2.1, lines 127–144). Runtime currently coalesces
  equal terms and adds source cells (`apps/common/xanadu/vagent/runtime/compiler.cpp:4172–4208`), so
  the proposed per-support ownership preserves existing query semantics while closing the R8
  ownership gap. Existing tests establish source coalescing and keyed replacement
  (`tests/xuzz/vagent_belief_test.cpp:313–350,470–486`); v1.5 correctly treats the new epistemic
  cases as future P19 acceptance tests, not passing coverage.
- **Epoch and feature safety:** Learners are keyed by source incarnation, predicate/domain,
  calibration/model epochs and feature schema; delayed labels/completions must derive and exactly
  match that key from their immutable observation (§2.1, lines 151–163). The translator specifies
  full typed manifest compatibility, full-key collision checking, mapped graph reconstruction, epoch
  expiry and atomic refusal on incompatible or unbudgeted changes (§2.2, lines 192–232).
  Per-observation time mapping snapshots and uncertainty are preserved or made stale; model reset
  cannot clear hardware latches (§2.2, lines 234–242). These are concrete required behaviors, not
  claims about the current copier, which only copies fixed dimensions and matches predicates by name
  (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3200–3247`).
- **Active labels and correction replay:** The active ledger keeps one effective adjudication per
  target, corrections require a new event ID and predecessor CAS, replace rather than add training
  evidence, and deterministically rebuild Beta/SGD state under the same full epoch key (§6.3, lines
  795–829). A retired observation's label may be corrected without restoring its current-reading
  status (§2.1, lines 119–125). This avoids both duplicate learning and correction-driven
  resurrection of stale readings.
- **Exactly-once learning boundary:** The candidate distinguishes transport acknowledgement from
  application commit and specifies issuer pending state, receiver intake journal, separate
  processing turn, atomic model/ledger/outcome commit, cached result replay and bounded retries
  (§6.3, lines 831–875). This addresses actual runtime behavior: `MeshActorRuntime::receive` acks on
  enqueue (`apps/common/xanadu/vortex/mesh_actor.cpp:517–534`), normal dispatch discards the
  `runTurn` outcome (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1160–1180`), and `settle`
  exchanges the host event batch before ignoring each turn outcome (`agent_executor.cpp:1284–1292`).
  The candidate explicitly assigns this guarantee to new P19.5 application journals rather than
  mistaking the transport ack for committed learning.

## MINOR

None.

VERDICT: CONVERGED
