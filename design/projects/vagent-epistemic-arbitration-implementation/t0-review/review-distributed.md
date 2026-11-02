# T0 implementation review, round 1: distributed-systems expert (persona distributed)

Scope: the uncommitted working-tree change at HEAD `8a4089d9` (`agent_executor.{hpp,cpp}`,
`tests/xuzz/vagent_epistemic_ingress_test.cpp`) and `/tmp/p19-work/t0/handoff.md`, against P19 v1.5
section 2.2 (upgrade) and the I3-1/I3-2 wake and quiescence requirements. Code read at HEAD plus the
diff. Review is by reading; I did not build or execute anything (no `make` allowed), so the busy
loop below is a traced argument, not a measured one. Paths are relative to
`apps/common/xanadu/vagent/runtime/` unless noted.

## What is sound

- Wake protocol on the normal path. `signalLocked()` is called with `PostBox::mutex` held
  (`agent_executor.cpp:602-607`, `push` 609-613, `pushControl` 644-653, `putBack` 655-661), and
  `descriptor()` creates the eventfd and writes once if items already exist (`:618-632`). The
  eventfd is a counter and `drainDescriptor` reads it to empty (`:633-639`), so many signals
  coalesce. A post is never lost: in `apps/vagent/main.cpp:1285-1349` the order is tick (take),
  poll, drain, then tick again with no poll between drain and the next take. A post that lands after
  the take signals before poll; one that lands between poll-return and drain is taken by the tick
  right after the drain. The worst case is a spurious wake with an empty queue.
- The take-to-putBack window. A post made by another thread inside the window is signalled by
  `push`; `putBack` signals again. No wake is lost, and the two writes collapse into one drain.
  Before the change, a `pushControl` (bind/retire) never signalled at all: a reactor with timeout -1
  slept through it. That defect is fixed.
- `putBack` of an empty vector is a no-op (`:657`), so an ordinary drain with nothing kept does not
  self-signal.
- `upgradeRefusal` runs before any mutation in both overloads (`:5059-5078`, call sites `:5096`,
  `:5447`) and returns the same typed errors as the I2U path. Choice points, open marks, in-turn,
  host events (this includes queued source-change and ingestion turns) and any post-box item for the
  pid are refused. `MeshActorRuntime::upgrade` re-checks `running`, `drained`, `choiceDepth` and
  marks (`vortex/mesh_actor.cpp:767-770`).
- Post versus swap on the I2U route. All of `upgrade` runs on the owner thread. A producer thread
  can still enqueue after `pendingFor` and before the swap, but the item stays in `posts_` (not in
  `State`, so the `make_unique<State>(old)` copy is unaffected) and runs after the swap. That is
  safe on this route only because `upgradeChangeRequiresRefusal` refuses `EpistemicPolicy`,
  `PredicateArity`, `PredicateParameterType` and `PredicateKeyPositions` changes
  (`upgrade_schema.cpp:723-731`), so the binding snapshot (`Ingress::Binding::policy/params/domain`)
  stays valid for the new program.
- Error precedence. Order is now: `NotQuiescent` (shared), `ExecutionFailed` (populated root),
  `NotQuiescent` (flights, requests, deliveries, outbox, pending). A populated root with in-flight
  asks now returns the permanent `ExecutionFailed` instead of retryable `NotQuiescent`. That is
  correct (retrying cannot help until a translator exists). Nothing in-tree branches on the order;
  the lead's runs (186, 841, 3005) are green.
- Entry points found: `AgentExecutor::upgrade(shared_ptr, grant)`,
  `AgentExecutor::upgrade( CompiledProgram&)`, `LocalHost::upgrade(shared_ptr)`
  (`program.cpp:377-395`), `LocalHost::upgrade(CompiledProgram)` (`:399-402`), and the TUI
  `/upgrade` (`tui/agent_tui.cpp:649`, shared overload). `apps/vagent/main.cpp` and the CLI call
  none of them. There is one more, see M2.

## BLOCKING

None.

## MAJOR

### M1. `putBack` now signals for items that can never run: an unbounded busy loop

Problem. `drainPosted` keeps an item when `!state.prepared` (`:3879`) before it asks whether the
actor is alive (`:3883`). An actor whose `prepare` fails goes through `fatal` (`:1521-1530`, called
at `:1466-1468`): `runtime_.stop`, `forgetIngress`, `context = nullptr`, and `prepared` stays
`false`. `states_` still holds the pid (created by `launch`, `:2994`), so the "unknown pid" branch
(`:3869-3875`) does not fire either. The same holds for any actor stopped before its first turn
(spawn refused later, stopped by a parent or supervisor, lease expired). Every later post for it (a
`bindSource` already queued, an `observe`, a retry) is kept, `putBack` re-queues it and, with this
change, writes the eventfd.

Evidence (trace, not run). In `apps/vagent/main.cpp:1285-1349` each iteration is

```text
tick -> drainPosted -> take/keep/putBack(signal) -> poll(fd readable) returns 0 ms
     -> drainNotifications -> tui.markDirty -> next iteration
```

That is 100% of a core, a full `tui.render()` per spin (`markDirty` at `:1349`), and the poll
timeout never matters. Before the change this was a silent leak (the item is never abandoned, so
`ingress_->release`, the outcome log and the per-source queue slot of `observe` are never settled,
and `ingressQueue` backpressure becomes permanent); now it is also a spin. The handoff flagged it
and left it ("bounded by the first turn"); that bound only holds for an actor that will in fact
prepare.

Required fix (in `drainPosted`, `:3878-3886`, not in `PostBox`). Decide runnability from the
runtime, not from `state.prepared`:

```text
if runtime_.isStopped(item.pid) (true for an unknown or erased pid): abandon(item)   // never runs
else if !state.prepared                                     : keep   // first turn still owed
else if state.inTurn && epistemic(item)                      : keep
else                                                         : place
```

`MeshActorRuntime::isStopped` already exists (`vortex/mesh_actor.cpp:1165-1168`, `.hpp:283`) and
returns `true` for an unknown pid. This also replaces the `states_.contains || localCursor` test.
Then signal in `putBack` only for items kept for a reason that a later event ends, which is now only
"actor owes a first turn", and that case is already followed by `pumpUntilIdle`, which calls
`drainPosted` after the first turn (`:3244`, `:3250`), so it needs no signal from `putBack` at all
in the reactor. If the lead wants the signal kept for hosts that run the first turn elsewhere, the
alternative is to signal at the end of a successful `prepare` (`:1410`) when
`posts_->pendingFor(state.pid)`. That wakes exactly when a kept item became runnable. Either way
`putBack` must not signal unconditionally. The `state.inTurn && live && epistemic` keep (`:3879`) is
not reachable from `tick` or `pumpUntilIdle` (neither runs inside a turn); if a host callback ever
re-enters, it has the same spin, so give it the same treatment or delete it.

Test required. Launch an actor, make its first turn fail (a program whose `prepare` is refused, or
`runtime().stop(pid)` before the first turn), `bindSource` / `observe`, `tick()`,
`drainNotifications`, `tick()` again: the descriptor must be unreadable, `ingestOutcome` must report
the undelivered outcome and the queue slot must be released. This fails on the current tree and on
the baseline.

### M2. The reconciler overload cannot honour "a translator is registered"

Problem. `upgradeRefusal` lets a populated `d.trust` / `d.epistemic` root through when
`ownedStateTranslator_` is set (`:5073`). On the I2U route that is right: the translator is invoked
inside the lambda (`:5351-5360`). The reconciler overload (`:5441-5515`) never calls the translator,
and `reconciler.cpp` has no mention of trust or epistemic. Once I3-2 registers a translator, a
reconciler upgrade of a populated actor passes the guard and silently drops the profiles, faulted
latches and incarnation floors, which is exactly what the guard text says it prevents. Second, the
reconciler overload does not run `compareUpgradeSchemas`, so a new program that changes an
arbitrated predicate's policy or shape is accepted even though the live `Ingress` bindings
(`Binding::policy/params/domain`, snapshotted at `bindSource`, `:3463-3470`) are not revalidated by
either route. Queued and later observations are then checked against the old policy and run by the
new program's `observeEntry`. The refused-by-the-I2U-route invariant is gone on this route even with
empty roots.

Evidence. `agent_executor.cpp:5073` (no route parameter), `:5441-5515` (no translator, no schema
comparison), `reconciler.cpp` (grep for trust/epistemic/policy: none), `upgrade_schema.cpp:723-731`.
`ownedStateTranslator_` has no in-tree caller of its setter today (`agent_executor.hpp:294`), so the
translator hole is latent, not live. The policy hole is live as soon as any source is bound.

Consequence. A second route silently disagrees with the first about what an upgrade may discard,
which is the defect T0 set out to remove, and the one place where it is invisible to the current
tests (they run with no translator).

Required fix. Give `upgradeRefusal` a route flag (or split the root check out): the reconciler
overload refuses a populated root unconditionally (`ExecutionFailed`) until it is made to call the
translator; and it refuses any change `upgradeChangeRequiresRefusal` flags when the pid has an
`Ingress` binding or an arbitrated predicate (or simply runs the same `compareUpgradeSchemas`
refusal loop as `:5133-5137`). Add a test with a stub translator set and a populated root through
the reconciler overload.

## MINOR

- m1. `LocalHost::runtime()` (`program.hpp:278`) exposes both `MeshActorRuntime::upgrade` overloads
  directly. A caller of `host.runtime().upgrade(...)` skips `upgradeRefusal` and leaves `State`
  pointing at a destroyed context. Nothing in-tree does it; document it as unsupported or make the
  executor the only friend. The runtime itself cannot enforce the epistemic guard because it does
  not know the roots.
- m2. `pendingFor` is false inside the `take()` to `putBack()` window of `drainPosted`. Safe today
  because `drainPosted` and `upgrade` share the owner thread. Say so in the `pendingFor` comment so
  a future caller from another thread does not assume the post-box is a complete queue.
- m3. Tests. `EveryEnqueuePathMakesTheWakeDescriptorReadable` and `APutBackWakesTheReactorToo` would
  fail without the fix (the first on `bindSource`/`retireSource`, the second on `putBack`), so they
  are meaningful, but `APutBack...` locks in the signal that M1 says is wrong in the dead-actor
  case; keep it for the live-first-turn case. Missing: M1's dead-actor test; a cross-thread test (a
  producer thread posting while the owner upgrades; assert the item runs after the swap and none is
  lost); the reconciler overload with a translator (M2); open marks and choice points through the
  executor (only covered by the runtime's own check).
- m4. `AEmptyEpistemicRootStillUpgradesBothWays` and `AQueuedSourceChangeIsNotQuiescent` exercise
  `LocalHost` only. They pass identical runs through both overloads, which is the point, but the
  reconciler overload accepts in-flight asks and outbox by design; add one test that records that
  difference so a later "tidy-up" does not unify them by accident.

## Questions for the owner

- Q1. Should an upgrade of an actor with live source bindings be refused or should it re-snapshot
  the bindings? The I2U route refuses policy changes, so today the answer is "refuse"; M2 asks the
  reconciler route to follow the same answer. Confirm.

Tally: 0 BLOCKING, 2 MAJOR, 4 MINOR.

Verdict: FAIL
