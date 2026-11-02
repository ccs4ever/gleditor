# T0 implementation review, round 1: compiler and codebase expert (persona codebase)

Scope: the uncommitted working-tree change at HEAD `8a4089d9` (`agent_executor.{hpp,cpp}`,
`tests/xuzz/vagent_epistemic_ingress_test.cpp`) and the handoff `/tmp/p19-work/t0/handoff.md`,
against the D1 and D2 items of `i3-2-review/review-codebase-r2.md` and F31, F32 of
`i3-2-review/findings-r2.md`; I3-1 section 10 and section 14 (populated roots refuse until the typed
translator exists). Code at HEAD `8a4089d9` plus the diff.

Scratch work is under `/tmp/p19-codebase/t0/` (`spin.cpp`, `shed.cpp`, `cc.sh`, `cc2.sh`). Both
probes link the repository's built objects and the unchanged library; no repository file was edited.
I ran the five new tests (`--gtest_filter='VagentEpistemicUpgrade.*:...'`, headless, isolated): 5
passed.

## Earlier findings

D1 (the reconciler path sheds profiles) is resolved for today's tree but not robustly, see M1. D2
(control posts do not wake the reactor) is resolved for `pushControl`. The `putBack` half of D2's
required fix was my own requirement and it was wrong, see M2.

## What is sound

- `upgradeRefusal` runs before any actor, arena or executor state changes in both overloads
  (`agent_executor.cpp:5096` for the shared-pointer overload, `:5447` for the reconciler overload).
  The only things that precede it are lookups and the shared overload's `NotAuthorized` and
  program-identity checks, which return without effect.

- The shared-pointer overload loses nothing by dropping its in-lambda root check: `UpgradeGraph`
  still refuses a `d.trust` or `d.epistemic` extension when no translator is registered
  (`agent_executor.cpp:342-345`, built with `static_cast<bool>(ownedStateTranslator_)` at `:5300`),
  so the guard is now stated twice, once early and once as defence in depth.

- The reconciler overload now refuses `inTurn`, non-empty `hostEvents`, any post-box item for the
  pid (this covers queued control items and the items `putBack` returned), open choice points and
  open marks. `pendingFor` takes the post-box lock and releases it; no lock order changes.

- Entry points found, all by grep over `apps/` and `tools/`:

  | Entry point                                         | Reaches                          | Guarded |
  | --------------------------------------------------- | -------------------------------- | ------- |
  | `AgentExecutor::upgrade(pid, shared_ptr, grant)`    | `upgradeRefusal` at `:5096`      | yes     |
  | `AgentExecutor::upgrade(pid, CompiledProgram &)`    | `upgradeRefusal` at `:5447`      | yes     |
  | `LocalHost::upgrade(pid, shared_ptr, limit)`        | the first overload               | yes     |
  | `LocalHost::upgrade(pid, CompiledProgram)`          | the second overload              | yes     |
  | `AgentTui::upgrade` (`agent_tui.cpp:649`, `:775`)   | the first overload               | yes     |
  | `apps/vagent/main.cpp`, mesh node, CLI              | no upgrade call                  | n/a     |
  | `MeshActorRuntime::upgrade` (two overloads, public) | only the two executor call sites | see m4  |

- `signalLocked` is called under the mutex by all three enqueue paths; the empty `putBack` is now a
  no-op. `PostBox::descriptor()` still writes once for items posted before the descriptor existed.

- The new helper returns `std::optional<ActorError>`, so no sentinel. Comments say why.

## BLOCKING

None.

## MAJOR

### M1. The reconciler overload is guarded only while no translator is registered

Problem. `upgradeRefusal` refuses a populated `d.trust` or `d.epistemic` root only when
`!ownedStateTranslator_` (`agent_executor.cpp:5073`). That condition is right for the shared-pointer
overload, whose `UpgradeGraph` calls the translator. It is wrong for the reconciler overload, which
never calls the translator: `makeStateReconciler` has no `trust` or `epistemic` dimension
(`reconciler.cpp` has no occurrence of either name) and `cloneGraph` follows only the dimensions it
is given. I3-2 registers the translator (`setOwnedStateTranslator`, `agent_executor.hpp:293`, called
nowhere in the tree today). From then on the guard disappears for the reconciler route.

Evidence (`/tmp/p19-codebase/t0/shed.cpp`: bind a source, ingest one observation, upgrade by value;
the second run registers a no-op translator first):

```text
translator=0 profiles before=1
by-value upgrade: refused (error=8)
profiles after=1
translator=1 profiles before=1
by-value upgrade: ACCEPTED (error=0)
profiles after=0
```

Consequence. The defect D1 fixed returns the day I3-2's translator lands: the upgrade drops the
profile, its faulted latch and its incarnation floor, and leaves the belief with supports that have
no records. The owner's Q2 ruling (latches live for life) breaks on the route the TUI does not use.
The new test `APopulatedProfileRefusesEveryUpgradeEntryPoint` cannot see this, since it never
registers a translator.

Required fix. Make the populated-root test depend on the route. Pass it to the helper, for example
`upgradeRefusal(pid, old, TranslatorCarries::Yes|No)`, or keep the root test out of the helper and
call it from each overload: the shared overload refuses when `!ownedStateTranslator_`, the
reconciler overload refuses always, until the reconciler carries both roots. Add a test that
registers a translator, populates a profile, and expects `ExecutionFailed` and an unchanged profile
count from `LocalHost::upgrade(pid, CompiledProgram)`.

### M2. `putBack` now signals on every drain, so a kept post can spin the reactor

Problem. `putBack` signals the eventfd whenever it keeps anything (`:655-661`). `drainPosted` keeps
an item while its state is unprepared (`:3879`). A state created by `drainPosted` or `execute` and
whose actor then dies before its first turn (a `prepare` failure ends in `fatal`, which stops and
reaps the actor; or `runtime().stop(pid)`) is unprepared with no cursor in the runtime. The
unknown-pid test at `:3862` needs both `!states_.contains` and `!localCursor`, so this item is kept
for ever instead of abandoned. Every `tick` and every `drainPosted` inside `pumpUntilIdle` re-puts
it and writes the eventfd. The reactor (`apps/vagent/main.cpp:1279-1350`) polls the descriptor with
`timeoutMs = -1`, so `poll` returns at once, `drainNotifications` empties it, `markDirty` forces a
render, and the loop repeats.

Evidence (`/tmp/p19-codebase/t0/spin.cpp`: launch, `bindSource`, `tick`, `runtime().stop(pid)`, then
1000 reactor iterations of `drainNotifications; tick; pumpUntilIdle`; count iterations for which the
descriptor is readable afterwards):

```text
reactor iterations that wake immediately: 1000 / 1000
```

Before the change the same sequence leaves the descriptor unreadable (`putBack` did not signal), and
the reactor sleeps. The handoff flagged this as "bounded by the first turn"; it is not bounded when
the first turn never happens. The kept item also never releases its queue slot (pre-existing), so
the post is both stuck and now hot.

Consequence. A failed agent start turns the `vagent` reactor into a 100% CPU loop that re-renders
the TUI each pass, in the one situation where a health action for that actor was just queued.

Required fix, two parts, both in `agent_executor.cpp`:

1. Remove `signalLocked()` from `putBack`. A kept item needs no new wake: its enqueue already
   signalled, and the reactor runs `pumpUntilIdle`, which calls `drainPosted` after the first-turn
   wakes and after each message turn (`:3244`, `:3250`), so an item that becomes placeable is placed
   in the same iteration. Nothing that makes a kept item placeable happens outside an owner turn. My
   D2 requirement in `review-codebase-r2.md` (write the eventfd in `pushControl` *and* `putBack`)
   over-reached on `putBack`; `pushControl` is the missing wake.
1. In `drainPosted`, abandon (with `abandon(item)`, so queue slots, the outcome log and charges are
   released) any item for a pid the runtime has no cursor for, whether or not a `State` exists:
   replace `!states_.contains(pidKey(item.pid)) && !runtime_.localCursor(item.pid)` with
   `!runtime_.localCursor(item.pid)`. A prepared state without a cursor is already abandoned by the
   `!live` branch, so only the unprepared-and-dead case changes. The lead should confirm no code
   creates a `State` for a pid before the runtime spawns it (`resume`).

Tests. Change `APutBackWakesTheReactorToo` to assert the opposite (a put-back leaves the descriptor
unreadable) and add one for the dead-before-first-turn item: after `runtime().stop`, a drain
abandons it, the ingress outcome log holds the `Undelivered` outcome for it, and `pendingFor(pid)`
is false so `checkpoint` and upgrade stop refusing for it.

## MINOR

- **m1. Error precedence is now split inside the shared overload.** The order is `NotQuiescent`
  (helper) < `ExecutionFailed` (root) < `NotQuiescent` (flights, requests, outstanding, outbox,
  pending) < `ExecutionFailed` (intention). A populated root also now precedes the schema
  `ResourceLimit` and `ExecutionFailed` checks that used to come first. Nothing depends on it: the
  three suites pass (the lead's run: runtime 186, actor 841, xuzz 3005) and no test in
  `vagent_upgrade_test.cpp` or `vagent_live_upgrade_test.cpp` populates a root. A caller waiting for
  quiescence can be told "retry" and then "never". Move the shared overload's extra quiescence
  checks above the root test, or say in the helper's comment that the root test comes second.
- **m2. The wake test's comment overclaims.** `EveryEnqueuePathMakesTheWakeDescriptorReadable` says
  "exactly once", but `drainNotifications` reads until empty, so a double write passes. Read the
  eventfd counter once (it returns the sum) and assert it is 1, or drop "exactly".
- **m3. Gaps in the tests.** (a) `hostEvents` non-empty (call `executor().tick()` after the binds,
  before any turn) is the new refusal for the reconciler route and is untested. (b) `d.trust` only
  and `d.epistemic` only are not told apart; one profile populates both. (c) Open marks and choice
  points cannot be set from outside at rest, so they stay untested. (d) The precedence in m1 has no
  test. (e) No test for M1.
- **m4. Public `LocalHost::runtime()` still reaches `MeshActorRuntime::upgrade` directly** (two
  public overloads, `mesh_actor.hpp:212-216`), which checks `running`, `drained` and, in the
  prepared overload, marks and choice points, but knows nothing of P19 roots. `AgentTui` holds the
  runtime reference and today calls only the executor. State this in the executor's header comment
  ("upgrade through the executor only"), or make the executor's `runtime()` access narrower in I3-2.
- **m5. `LocalHost::upgrade(pid, CompiledProgram)` retains the program before it can be refused**
  (`program.cpp:399-401`). Each refused attempt leaves a `CompiledProgram` in `programs_` and in the
  executor. The brief's "before any mutation" holds for the actor, not for the host table. Retain
  after the refusal check, or release on failure.
- **m6. Formatting is unverified.** The handoff used clang-format 23 on changed lines; the
  repository requires 19 (`make format-check`). Run `make format` with 19 before the commit.

## Questions for the owner

- **Q1.** After a live upgrade, `Ingress::Binding` (policy, parameter kinds, `domain` string with
  the policy hash) stays as computed from the old program, on both routes. With an empty root the
  upgrade is accepted, and a new program that changes the arbitrated predicate's policy keeps
  stamping the old domain on later ingestions. Should `upgrade` refuse when any binding exists whose
  predicate's policy hash differs in the new program, or rebind? I did not test this; it may belong
  to I3-2.

Tally: 0 BLOCKING, 2 MAJOR, 6 MINOR.

Verdict: FAIL
