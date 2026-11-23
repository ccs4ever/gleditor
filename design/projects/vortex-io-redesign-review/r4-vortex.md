# Round 4 critique: Vortex & Spool-Mesh Expert (vortex-io-redesign.md)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3.1 (Actor I/O lacks a registered proposal): RESOLVED — v0.5 adds P5 with actor runtime, actor
  wire, and VAgent checker/compiler/executor/adapter paths, rationale, retained-state and queue
  costs, and a typed refusal fallback (§12, lines 663–664). P6 also names the system-doc and host
  files with costs and an immutable-snapshot fallback. The direct-submit/await actor restriction and
  staged post-commit path now have a concrete policy (§4, lines 225–247).

## BLOCKING

None.

## MAJOR

### 1. (§4, §12) Actor restore cannot establish the no-resubmission claim

**Problem:** The design says an unresolved actor I/O attempt remains unknown after a crash and that
“restoration or duplicate wakeups never resubmit it,” but it refuses actor checkpoints while service
work is unresolved and gives no durable attempt/reconciliation record outside that checkpoint. If
the process exits after post-commit admission, the latest actor checkpoint can predate the staged
intent and its `dispatched` state. Restoring that checkpoint and replaying/redelivering the actor
turn can stage the same intent again; the volatile IoService `RequestRecord` and correlation map
cannot by itself distinguish that from a fresh post-restore action. P5 prices correlation records
retained until reconciliation, but does not identify persistence that survives the process crash or
a restore admission check. This leaves the advertised no-resubmit guarantee unimplemented across the
recovery case it claims to cover.

**Evidence:** The actor policy requires both checkpoint refusal while service work is unresolved and
no resubmission after crash/restoration (§4, lines 243–246). P5 names `mesh_actor`, actor wire, and
VAgent integration files and prices retained correlation records, but no write-ahead attempt ledger,
checkpoint representation, or restore/reconciliation gate (§12, lines 663–664). The current VAgent
dispatcher marks `dispatched` in the live arena before invoking its effect handler
(`apps/common/xanadu/vagent/adapter.cpp:662-675`); that arena mutation is not itself a durable actor
checkpoint. The portable `ActorContinuation` contains only actor identity/program metadata, spawn
request, and completed message records (`apps/common/xanadu/vortex/mesh_actor.hpp:144-152`), and
`encodeContinuation` serializes only those message-boundary fields
(`apps/common/xanadu/vortex/mesh_checkpoint.cpp:51-90`). Thus the design cannot rely on restoring a
pending VM PC or an unresolved service record from the existing checkpoint representation.

**Fix:** Extend P5 with a durable actor-attempt/reconciliation record keyed by stable actor
generation and staged-intent/request identity, written before service submission. Include the exact
persistence path (for example, `mesh_checkpoint.cpp` plus a versioned continuation/journal
representation, or a separately named service-owned journal), its bounded retention and recovery
cost, and the restore gate: replay only after the original attempt is reconciled; when the record is
absent or uncertain, expose `Unknown` or refuse actor restoration before it can rerun the effect. Do
not depend on the in-arena `dispatched` mark alone. If durable reconciliation is declined, use P5's
typed refusal fallback for actor post-commit I/O.

## MINOR

None.

VERDICT: NOT CONVERGED
