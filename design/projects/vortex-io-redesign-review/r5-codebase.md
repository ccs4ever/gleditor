# Round 5 critique: Codebase Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Target SHA256:
`dcdd62c33bb1d1c7abe3e7f4d83d4c99d81a16210ec4637e1bb651a8c9d8d207`. Tally: 0 BLOCKING, 0 MAJOR, 0
MINOR.

## Status of previous BLOCKING/MAJOR findings

- R4: No unresolved Codebase BLOCKING or MAJOR findings. This round checks only the new
  actor-recovery scope.

## Source and proposal check

P5 registers the actor runtime, checkpoint continuation, new bounded journal, actor wire, and VAgent
staging/dispatch paths. It prices journal records/bytes, asynchronous flush, recovery/floor
reconciliation, correlation, and completion queues, and refuses actor I/O or restore/replay without
reconciled durable proof (`design/vortex-io-redesign.md:238–260, 676`). U-6 requires crash injection
around flush, submission, receipt, checkpoint publication, and older-floor restore; it also requires
version refusal, bounded retention, reconciliation before pruning, and no payload capture
(`design/vortex-io-redesign.md:692`).

These are genuine gaps in the current implementation, and P5 names the relevant source seams: the
portable `ActorContinuation` has no causal floor
(`apps/common/xanadu/vortex/mesh_actor.hpp:144–152`); the checkpoint encoder writes version 1 and
the decoder requires exactly that seven-field shape
(`apps/common/xanadu/vortex/mesh_checkpoint.cpp:80–102`); and `checkpointView` checks actor/mailbox/
choice state and delivery records but has no service-attempt ledger
(`apps/common/xanadu/vortex/mesh_actor.cpp:899–922`). P5 explicitly registers the checkpoint files
and new journal, with a refusal fallback; no existing source claims to implement the new durable
proof.

The existing VAgent `dispatched` marker is a cell-state write before its inline handler call
(`apps/common/xanadu/vagent/adapter.cpp:662–685`). The design says it prevents ordinary repeated
drains while explicitly declining an exactly-once guarantee
(`design/vortex-io-redesign.md:133–138`); §4 separately requires a durable attempt ledger before new
actor service submission. It does not claim the current marker is an independent durable journal.
Evidence's `EffectRuntime` is described as a separate existing runtime; the design does not claim an
actor integration for it (`design/vortex-io-redesign.md:140–148`).

## BLOCKING

None.

## MAJOR

None.

## MINOR

None.

VERDICT: CONVERGED
