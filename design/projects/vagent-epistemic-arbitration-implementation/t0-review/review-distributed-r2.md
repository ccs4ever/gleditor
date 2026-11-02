# T0 implementation review, round 2: distributed-systems expert (persona distributed)

Scope: the uncommitted working-tree change at HEAD `8a4089d9` (`git diff -- apps tests`:
`agent_executor.{hpp,cpp}`, `tests/xuzz/vagent_epistemic_ingress_test.cpp`), the "Review fixes"
section of `/tmp/p19-work/t0/handoff.md`, and my round-1 report `t0-review/review-distributed.md`.
Paths are relative to `apps/common/xanadu/vagent/runtime/` unless noted. I read the diff and the
surrounding code and ran the eight affected tests (`VagentEpistemicIngress.*Enqueue*`, `*PutBack*`,
`AStopped*`, `VagentEpistemicUpgrade.*`) from the lead's `build/vagent_actor_test`, headless and
isolated: 8 passed (scratch directory `/tmp/p19-distributed/xdg`). The binary is newer than
`agent_executor.cpp` (17:25:07 against 17:24:54). I did not rebuild or run the revert experiment.

## Earlier findings

### M1. `putBack` signals for items that can never run: RESOLVED

The fix has two parts, and both are right.

- `putBack` no longer signals (`agent_executor.cpp:655-662`; the comment states why). The three
  other two paths still do: `push` (`:612`) and `pushControl` (`:652`), and `descriptor()` writes
  once when items already exist. Each kept item signalled when it was posted, so the wake is not
  lost.
- `drainPosted` abandons the posts of an actor that is stopped before its first turn (`:3883-3887`):
  `!state.prepared && runtime_.isStopped(item.pid)`. `fatal()` calls `runtime_.stop(state.pid)`
  (`:1528`) and leaves `prepared` false, so a failed first turn lands in this branch. `isStopped` is
  true for an unknown pid (`vortex/mesh_actor.cpp:1165-1168`), so the erased-actor case is covered
  too. This is the condition I asked for. The abandoned item goes through the existing `abandon`
  lambda, so the queue slot, the outcome log and the charge are settled.

Why silence is safe for a live actor that has not prepared: the first turn runs from `pumpUntilIdle`
(`needsFirstTurn_`), which calls `drainPosted` right after (`:3246`, `:3252`), and
`apps/vagent/main.cpp:1285-1349` ticks and then pumps in the same iteration. The kept item is placed
in the same pump. The handoff names the cost: a host that ticks but never pumps is not woken for a
kept item. That is acceptable; no in-tree host does it.

Tests: `APutBackDoesNotWakeTheReactor` (three ticks, descriptor quiet, the item survives to the
first turn) and `AStoppedActorsUnplacedPostsAreAbandoned` (outcome `Refused`, descriptor quiet) both
pass. Two gaps, neither blocking: the second test stops the actor with `runtime().stop`, which
leaves the same state as `fatal()` but does not drive the failed-`prepare` path; and it posts a
source change, not an `observe` item, so the queue-slot release of `item.ingest` is not asserted.

### M2. The reconciler overload and the translator: RESOLVED (root), DECIDED (policy)

Root half, resolved. `upgradeRefusal` takes an `UpgradeRoute` (`:5069`). Only the schema route can
pass a populated `d.trust` or `d.epistemic` root, and only with a translator registered
(`:5093-5100`). The reconciler overload calls it with `UpgradeRoute::Reconciler` (`:5462`), so it
refuses a populated root whether or not a translator exists, which is correct because it never calls
one. `TheReconcilerRefusesAPopulatedProfileWithATranslator` sets a stub translator, builds a profile
and expects `ExecutionFailed` with nothing changed; it passes. The flight, request, outbox and
`d.pending` checks moved into `upgradeRefusal` for the schema route (`:5079-5090`), so every
`NotQuiescent` now precedes `ExecutionFailed`, as at HEAD.

Policy half, decided. The reconciler overload still does not run `compareUpgradeSchemas`
(`:5448-5515`), so a program that changes a bound predicate's policy, arity or parameter types is
accepted when the actor has bindings and no profile yet (a profile appears at the first accepted
reading or first chargeable refusal; once one exists the root guard refuses). The `Ingress::Binding`
snapshot (`policy`, `params`, `domain`, `:707-716`) then validates later readings against the old
shape, and the new program's `observeEntry` runs them. The lead ruled on this as decision D3
("source bindings keep the policy they were bound under after an upgrade; T1-D decides"), which is
the answer to my owner question Q1. I accept it for T0. What remains is a record: the two routes
differ on this point, and the schema route refuses what the reconciler route accepts. The I3-2
design's section 13 sentence "Both overloads already refuse populated roots until a translator is
registered (T0)" is now inaccurate (the reconciler overload refuses always). See MINOR m5.

## What is sound

- Wake protocol. `push` and `pushControl` signal under the `PostBox` mutex through `signalLocked()`
  (`:604-609`), and `putBack` is silent by design with a stated reason. A post cannot be missed:
  tick takes, poll sleeps, a post made after the take signals before the poll. The eventfd is a
  counter drained to empty, so duplicates coalesce.
- `putBack` of an empty vector is a no-op and the abandon rule means a put-back can no longer
  re-queue an item that has no future. The reactor cannot spin on a dead actor.
- `upgradeRefusal` runs before any mutation in both overloads (`:5118`, `:5462`). The quiescence set
  (in turn, host events, any post-box item for the pid, choice depth, open marks) is shared. The
  only stricter checks on the schema route are the ones the reconciler carries by design, and the
  handoff records that the first attempt to unify them broke two in-flight-ask upgrade tests.
- Entry points: `AgentExecutor::upgrade(pid, shared_ptr, grant)`,
  `AgentExecutor::upgrade(pid, CompiledProgram&)`, `LocalHost::upgrade` (both overloads,
  `program.cpp:377-403`) and the TUI `/upgrade` (`tui/agent_tui.cpp:649`, schema route). All reach
  `upgradeRefusal`.
- The post-versus-swap argument from round 1 still holds: a post that lands after `pendingFor` stays
  in `posts_`, not in `State`, and is validated against a binding snapshot that the schema route
  keeps valid by refusing policy and shape changes.

## BLOCKING

None.

## MAJOR

None open.

## MINOR

- m1. `LocalHost::runtime()` (`program.hpp:278`) still exposes `MeshActorRuntime::upgrade`, which
  skips `upgradeRefusal` and leaves `State` pointing at a destroyed context. The handoff records it
  as out of scope (`program.hpp/cpp`). Keep it on the list for the translator work (T1).
- m2. `pendingFor` is false between `take()` and `putBack()` in `drainPosted`. The handoff argues
  that upgrade and drain share the owner thread, which is right today. Add one sentence to the
  `pendingFor` comment so a caller from another thread does not read the post box as complete.
- m3. Tests still missing: a producer thread posting while the owner upgrades; open marks and choice
  points through the executor; a dead-actor `observe` that asserts the queue slot is released. None
  is needed to accept the change.
- m4. No test records that the reconciler overload accepts in-flight asks and outbox entries while
  the schema overload refuses them. `VagentUpgradeTest.InFlightAskSettlesAfterLiveUpgrade` covers
  the reconciler side implicitly; a one-line comment on `upgradeRefusal` already says why.
- m5. Record the route difference in the I3-2 design (section 13, translator list) and in T1-D: the
  schema route compares policy and shape and refuses; the reconciler route does neither and refuses
  only a populated root. D3 covers the decision; the text should say which route does what.

## Questions for the owner

None. Q1 of round 1 is answered by decision D3.

Tally: 0 BLOCKING, 0 MAJOR, 5 MINOR.

Verdict: PASS
