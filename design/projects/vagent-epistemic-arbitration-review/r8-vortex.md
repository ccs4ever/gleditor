# Round 8 critique: Vortex & Spool-Mesh Expert (r8-input-v1.4.txt)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 3 MAJOR, 0 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R7: No BLOCKING or MAJOR findings; the R7 focused review converged on the revised label lifecycle
  and fusion semantics. The new integrated runtime introduces lifecycle facts that were not in that
  review baseline.

## BLOCKING

None.

## MAJOR

### 1. (§6.3, lines 674–678) Failed verification delivery is acknowledged by the current adapter

**Problem:** The requirement that a failed verification percept remain retryable cannot be satisfied
by merely making the turn transactional. There are two delivery stages: `MeshActorRuntime::receive`
sends an `ActorAck` when it enqueues the request, before execution; later, `pumpOne` returns
`ActorDeliverValue` or `ActorDown`. `AgentExecutor::handleMessage` discards `runTurn`'s
`TurnOutcome` and returns a successful actor execution result even after the turn rolled back, so
the later completion is `ActorDeliverValue`; the sender then removes its delivery. The proposal
acknowledges a future adapter boundary, but neither P19.5 nor the acceptance gate defines how its
durable application-level commit/retry semantics relate to the transport receipt ack. A failed
verification can therefore be transport-acknowledged and then application-completed as success,
contrary to the stated retry rule.

**Evidence:** The proposal says “Failed verification turns do not acknowledge the event and may be
retried” and “the eventual adapter must implement this delivery/commit boundary”
(`r8-input-v1.4.txt:674–678`). `receive` enqueues and emits `ActorAck` before execution
(`apps/common/xanadu/vortex/mesh_actor.cpp:517–534`); `runTurnOnce` releases the turn mark on
failure (`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1488–1495`), but event dispatch
ignores its outcome and returns an empty successful result
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:1160–1174`). `pumpOne` maps that successful
result to `ActorDeliverValue` (`apps/common/xanadu/vortex/mesh_actor.cpp:593–604`), and receiving
the reply erases the sender's delivery (`apps/common/xanadu/vortex/mesh_actor.cpp:462–481`). P19.5
currently only broadly lists “future executor/runtime ABI/host authorization integration”
(`r8-input-v1.4.txt:728`); the gate at lines 775–779 describes the desired redelivery test but not
the necessary current-runtime change.

**Fix:** Amend P19.5 to name the existing `vagent/runtime/agent_executor.cpp` and
`vortex/mesh_actor.{hpp,cpp}` delivery path and distinguish transport receipt from application
commit. Specify a commit-aware verification response/dedup protocol: transport receipt may be
acknowledged, but a failed verification must remain durably retryable and must not be represented by
a successful application completion; a committed outcome must be persisted before final completion
is acknowledged. Define how ordinary failed user handlers retain their existing message semantics,
and add a regression test that observes both acknowledgment stages and delivery state across failed
then successful verification redelivery and a later unrelated plan failure.

### 2. (§2.1–2.2) Define integrated migration and resume

Full title: P19 state has no concrete path through integrated migration and resume

**Problem:** P19 requires exact epoch-bound migration and checkpoint preservation, but the
integrated runtime has no vagent upgrade reconciler. `MeshActorRuntime::upgrade` delegates all state
translation to a caller-supplied callback; the existing `AgentExecutor::restoreState` is a separate
resume copier with fixed traversal and predicate matching by display name. P19.1's vague “runtime
ABI/executor/reconciler integration when prerequisites exist” does not specify how new calibration,
labels, dedup floors/outcomes, tokens and observation references are remapped through either path.
In particular, name-only predicate matching is weaker than P19's full signature/domain key and could
attach migrated beliefs to a different overload/schema after a program change.

**Evidence:** The proposal requires lookup by full source/predicate/domain/schema/model key and
exact epoch checks (`r8-input-v1.4.txt:94–105`), profile-first migration with reference remapping,
and preservation on checkpoint/resume (`r8-input-v1.4.txt:134–139`). The runtime upgrade accepts a
generic reconciler and installs its returned root without imposing any field policy
(`apps/common/xanadu/vortex/mesh_actor.cpp:683–732`). Resume restore reconstructs only its
enumerated fields/ranks, and matches belief predicates by `text()` equality
(`apps/common/xanadu/vagent/runtime/agent_executor.cpp:3206–3224`, `3233–3247`); it does not
traverse a P19 `d.epistemic` state rank. P19.1 lists future integration but no explicit migration
ABI or files (`r8-input-v1.4.txt:724`).

**Fix:** Expand P19.1 with a concrete migration contract and explicit changes to the existing
runtime migration/resume path. Define identity matching using the full predicate signature plus
domain/schema and source incarnation; require unmatched or changed schemas to expire
labels/completions and invalidate dependent tokens; specify remapping of each owned clone/reference
and preservation of delivery dedup state. Name the actual runtime files/API to change and add
upgrade plus checkpoint/resume tests for overload/schema changes, expired labels, and full-key hash
collisions. State clearly that `MeshActorRuntime::upgrade` is only the atomic context swap/callback
hook, not an implemented vagent reconciler.

### 3. (§6.1, §8.2) Make the ScopedEvaluate graph ABI concrete

Full title: ScopedEvaluate's graph-call ABI is still not implementable from the specification

**Problem:** P19.8 describes the desired lifecycle of a scoped callback but does not define a
bytecode graph ABI that can be emitted, validated, and executed. “Input wing carries a validated
callback entry opcode” and “sentinel completion resumes the instruction's saved successor” leave
unresolved how callback entry/successor are represented in the existing graph, how the callee gets
typed inputs/results, and how a non-returning failure or trap is distinguished from a successful
sentinel arrival without corrupting cursor/choice state. The acceptance gate itself says to “specify
... graph-call ABI,” so this remains a prerequisite rather than a designed interface.

**Evidence:** The proposal introduces `#ScopedEvaluate` and the callback/sentinel protocol without a
concrete operand or graph layout (`r8-input-v1.4.txt:557–578`), then requires the graph-call ABI to
be specified before implementation (`r8-input-v1.4.txt:759–765`). Existing calls and cursor
transitions are implemented by the VM's opcode dispatch and cursor state
(`apps/common/xanadu/vortex/vortex_vm.cpp:1000–1089`); arena rollback is coupled to choice points
(`apps/common/xanadu/vortex/vortex_vm.cpp:216–245, 253–258`). A new opcode also needs the
enum/name/validation/decoding surfaces audited, as the proposal recognizes for P19.8 but does not
make the callback ABI concrete (`r8-input-v1.4.txt:731`).

**Fix:** Before implementation, add a normative ABI table or worked graph showing each input/output
wing, callback entry and completion representation, caller continuation storage, argument/result
frame ownership, and every exit (success, ordinary failure, trap/fuel exhaustion). Define
verifier-enforced purity and scope choice-floor behavior, nested-scope limits, and cleanup order
against actual Vortex cursor/choice semantics. Make P19.8 and its acceptance gate require this
artifact and graph-level tests for each exit path.

## MINOR

None.

VERDICT: NOT CONVERGED
