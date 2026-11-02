# T0 implementation review, round 2: compiler and codebase expert (persona codebase)

Scope: the uncommitted working-tree change at HEAD `8a4089d9` after the review fixes
(`apps/common/xanadu/vagent/runtime/agent_executor.{hpp,cpp}`,
`tests/xuzz/vagent_epistemic_ingress_test.cpp`), the fix notes in `/tmp/p19-work/t0/handoff.md`
(section "Review fixes"), and my round-1 report `t0-review/review-codebase.md`. Code is read at
`git diff -- apps tests` as of 2026-11-02 17:41. Read-only: this report is the only file written.

Method. I read the whole diff, then re-ran my two round-1 probes against the rebuilt objects
(`/tmp/p19-codebase/r3/link.sh` links `spin.o` and `shed.o` against `build/` as the lead left it;
compile line `/tmp/p19-codebase/t0/cc.sh`). I ran the seven new tests by name and the whole
`vagent_actor_test` (headless, isolated XDG): 843 of 843 passed, the seven new ones included.
`git clang-format --binary /tmp/p19-lead/bin/clang-format-19 --style=file --diff` (19.1.7) reports
"did not modify any files".

## Earlier findings

| Round-1 finding                                                    | Status                         | Evidence                                                                       |
| ------------------------------------------------------------------ | ------------------------------ | ------------------------------------------------------------------------------ |
| M1 reconciler guarded only while no translator is registered       | Resolved                       | `UpgradeRoute` parameter; probe and test below                                 |
| M2 `putBack` signals on every drain, a kept post spins the reactor | Resolved (different mechanism) | `putBack` silent; stopped, unprepared actor's posts abandoned; probe 0 of 1000 |
| m1 error precedence split inside the shared overload               | Resolved                       | all `NotQuiescent` checks now precede `ExecutionFailed`, in the helper         |
| m2 "exactly once" comment overclaims                               | Resolved                       | comment says the probe "shows each path writes, not how many writes it makes"  |
| m3 gaps in tests                                                   | Partly resolved                | (e) added; (a), (b), (c), (d) below                                            |
| m4 `LocalHost::runtime()` reaches `MeshActorRuntime::upgrade`      | Open, recorded nowhere         | handoff: "not done, by scope"; not in `decisions.md`, the plan or the design   |
| m5 `LocalHost::upgrade(pid, CompiledProgram)` retains before check | Open, recorded in the handoff  | handoff only                                                                   |
| m6 formatting unverified                                           | Resolved                       | clang-format 19.1.7, no diff on changed lines                                  |
| Q1 bindings keep the old policy after an upgrade                   | Resolved as a decision         | `decisions.md` D3: T1-D decides rebind, refuse or migrate                      |

### M1 (reconciler with a translator): resolved

`upgradeRefusal(pid, old, UpgradeRoute)` computes
`translates = route == Schema && ownedStateTranslator_` and refuses a linked `d.trust` or
`d.epistemic` root whenever `!translates` (`agent_executor.cpp:5093-5100` in the working tree,
helper at `:5068-5101`). The reconciler route therefore refuses a populated root with or without a
translator, which is what the code needs: `makeStateReconciler` has no `trust` or `epistemic`
dimension and never calls the translator.

Probe `/tmp/p19-codebase/r3/shed` (bind, ingest one reading, `LocalHost::upgrade` by value), run
without and with a no-op translator registered:

```text
translator=0 profiles before=1
by-value upgrade: refused (error=8)
profiles after=1
translator=1 profiles before=1
by-value upgrade: refused (error=8)
profiles after=1
```

Round 1 printed `ACCEPTED` and `profiles after=0` for the second case. The new test
`VagentEpistemicUpgrade.TheReconcilerRefusesAPopulatedProfileWithATranslator` registers a translator
and asserts `ExecutionFailed` with the context pointer, program reference and profile count
unchanged; it passes. The schema route still passes a populated root once a translator is
registered, which is the right behaviour for I3-2 (T1 writes the translator). The helper's comment
says why the two routes differ.

### M2 (spin): resolved

`putBack` no longer calls `signalLocked` (`agent_executor.cpp:654-664`, with a comment that says why
it is silent). `drainPosted` abandons any post for an unprepared actor the runtime has stopped
(`:3881-3887`): `MeshActorRuntime::isStopped` is `actor ? actor->stopped : true`
(`mesh_actor.cpp:1165-1168`), so it is true for an unknown pid, for one `fatal()` stopped before its
first turn, and for a lease that expired first (`:598-599`). That covers my `!localCursor` case and
the lead's two sources of the same defect. It does not misfire on a live actor that has not yet run
its first turn: `launch` leaves `stopped == false`.

Probe `/tmp/p19-codebase/r3/spin` (launch, bind, tick, `runtime().stop`, then 1000 reactor passes of
`drainNotifications; tick; pumpUntilIdle`, counting passes after which the descriptor is readable):

```text
reactor iterations that wake immediately: 0 / 1000
```

Round 1 printed 1000 / 1000. The tests `APutBackDoesNotWakeTheReactor` (three ticks, descriptor
quiet, the post survives to the first turn and then commits) and
`AStoppedActorsUnplacedPostsAreAbandoned` (outcome `Refused` present, descriptor quiet) pass. The
abandoned post goes through the existing `abandon` lambda, so queue slots, charges and the outcome
log are released. One cost the handoff names is real and acceptable: a host that ticks but never
pumps is not woken for a kept post. The `vagent` reactor pumps (`apps/vagent/main.cpp`).

The outcome code is `Refused`/`Undelivered`-class through `outcomeOfCode(kTurnResultUndelivered)`;
the test asserts the kind, not the code string. See m7.

### m1 to m6

- **m1.** The helper now holds the schema route's flights, requests, outstanding deliveries, outbox
  and pending checks (`:5078-5087`), so on both routes every `NotQuiescent` precedes the root's
  `ExecutionFailed`. The remaining `ExecutionFailed` for an active intention follows in the
  overload, as at HEAD. Resolved.
- **m2.** The comment above `EveryEnqueuePathMakesTheWakeDescriptorReadable` now says what the test
  shows. Resolved.
- **m3.** (e) Resolved: the translator test. (a) `hostEvents` non-empty: the handoff says it is
  "unreachable through the public API". It is reachable, and the guard is load-bearing there; see
  m7. (b) root-only populated cases and (c) open marks and choice points: still untested; (b) is
  hard to build because one profile populates both roots, and (c) cannot be set from outside at
  rest. Accepted as recorded gaps. (d) the precedence has no test; the helper's comment states the
  order. Accepted.
- **m4.** Still open. The handoff says `LocalHost::runtime()` exposes `MeshActorRuntime::upgrade`
  and that the files are outside the allowed set. Nothing in `decisions.md`, the plan, or the I3-2
  design records it. Record it (see N2).
- **m5.** Still open, handoff-only. Same remedy.
- **m6.** Resolved.

## What is sound

- `upgradeRefusal` runs before any actor, arena or executor state changes on both routes (shared
  overload at `:5118`, reconciler overload at `:5462`). The helper is `const` and returns
  `std::optional<ActorError>`; no sentinel.
- Entry points, by grep over `apps/`: `AgentExecutor::upgrade` (two overloads), `LocalHost::upgrade`
  (two overloads) and `AgentTui::upgrade` (shared overload). All reach the helper. `apps/vagent`,
  the mesh node and the CLI make no upgrade call. The one exception is the public
  `LocalHost::runtime()` route (m4).
- `PostBox::signalLocked` is called under the mutex by `push` and `pushControl`. `putBack` of an
  empty vector is a no-op and signals nothing. The cross-thread ordering is unchanged.
- The seven new tests fail without the fixes (handoff: reverting the route check, the abandon and
  the put-back signal fails exactly three; the earlier four failed against the first revert). I
  re-verified the behaviour independently by the two probes above.

## BLOCKING

None.

## MAJOR

None. Both round-1 MAJOR findings are resolved.

## MINOR

- **N1. The records still say `putBack` signals.** `remaining-plan.md` T0 "Content" says
  "`PostBox::pushControl` and `putBack` wake the reactor as `push` does (X-27)" and its test row
  asks for a wake "through `pushControl` and through `putBack`". `i3-2-design.md` section 1 says
  "`PostBox::signalLocked` is called from `push`, `pushControl` and `putBack`", and section 12 F32
  says "every enqueue signals the reactor". After the fix `putBack` is silent by design (a kept post
  signalled when it was posted). Correct the three statements, or a later reader will "restore" the
  signal and bring back the spin. The I3-2 round-3 report repeats this as m1.

- **N2. Record m4 and m5.** Add to `decisions.md` (or the T1 slice): "upgrade through the executor
  only; `LocalHost::runtime()` still reaches `MeshActorRuntime::upgrade`; the by-value
  `LocalHost::upgrade` retains the program before the executor can refuse it". T1-D is the natural
  home.

- **N3. `hostEvents` non-empty is testable and the handoff is wrong about it.** `settle` holds back
  ingestions past `epistemic_ingest_per_wake` (default 4) at the front of `state.hostEvents`
  (`agent_executor.cpp:1986-1994` at HEAD). Probe `/tmp/p19-codebase/t0/held.cpp`: bind, post seven
  readings, one `tick()`:

  ```text
  after one tick: 3 of 7 readings still pending
  by-value upgrade: refused (error=10)
  ```

  Error 10 is `NotQuiescent`. With the `!old.hostEvents.empty()` test removed the upgrade would be
  accepted and the three held ingestions lost with their queue slots, so the check is load-bearing.
  A test is a dozen lines: seven observes, one `tick()`, both `LocalHost::upgrade` overloads refuse
  `NotQuiescent`, then `pump()` and the same upgrade is accepted.

- **N4. The dead-actor test asserts the kind, not the code.** Add
  `EXPECT_EQ(outcome->code, "Undelivered")` so a change of outcome class for abandoned posts is
  seen.

## Questions for the owner

None new. Q1 of round 1 is decision D3.

Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

Verdict: PASS
