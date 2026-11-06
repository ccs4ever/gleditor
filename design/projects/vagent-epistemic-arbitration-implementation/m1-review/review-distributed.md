# M1 review: DIST (concurrency, atomicity, refusal channel, intermittent)

Verdict: FAIL. 1 MAJOR, 9 MINOR. Range `9bd27a03..61e57eae`. The four T1-D round-1 MAJORs are fixed
in code (section "T1-D MAJORs"). The one MAJOR is a resume defect that also explains the carried
intermittent. I ran the built `build/xuzz_test` only, headless, with my own XDG dirs.

## MAJOR

### M1. A checkpoint retypes numeric- and boolean-looking text; resume and upgrade then disagree

`checkpoint` (`agent_executor.cpp:5337`) publishes through `Store::makeCell(text)`, which calls
`inferTextValue` (`store.cpp:767`, `scalar.cpp:48-79`). A live arena text cell has kind `None`
(`wire.cpp:708`); the same cell after `fetchImage` has kind Int64, Double or Bool whenever its text
is an integer, has float syntax (`[0-9]+e[0-9]+` qualifies) or is a `truthLiteral`
(`1 0 true yes on false no off`). `VortexCore::render` (`vortex_core.cpp:296-310`) then returns a
number where the live actor returns a string, and the VM's `Eq` is variant equality
(`vortex_vm.cpp:569`). `copyCell` (`epistemic_translate.cpp:489-497`) copies the kind as it finds
it, so the translator carries the retyping into the new arena. T1c knew of it for field names and
`-0.0` (`t1-validation.md:267`) and fixed the digest for names only.

Failure scenario (from the code; I did not run it): a device whose event IDs or `key` strings are
numeric (`"42"`, `"1e3"`, channel `"1"`, `"on"`) commits `"42"`. After a resume the dedup entry
holds Int64 42 while the retry carries Str "42". The `Eq` at `compiler.cpp:4283-4290` is false, the
entry is skipped, and the retry commits a second time: replay protection does not survive a resume,
and a `key` position that no longer equals its own text splits supports that should group. A resume
and an in-place upgrade of the same actor therefore diverge, which is the property T1 sells.

Observed instance: `kObsClockInstance` (a random 16-hex `Ingress::clockInstance`,
`agent_executor_internal.hpp:~420`) is in every observation record. It is retyped with probability
`(10/16)^16 + 14 (10/16)^15/16 = 1.3e-3` per executor (about 3% per 24-seed run of the random test).
See "Intermittent".

Fix: carry the kind with a text cell across the checkpoint, so `publishValue` for an arena image
writes kind `None` explicitly instead of inferring (the right fix; it also removes the `-0.0` and
field-name special cases), or, if the store road stays as is, have `copyCell` and `restoreState`
re-stamp kind `None` on every cell the registry declares text (`event_id`, `clock_instance`,
`source`, tuple elements typed `Str`, `profile`). Add a resume test with event IDs `"42"`, `"1e3"`,
`"on"` and a retry after the resume that must answer `Duplicate`. Until fixed, give `clockInstance`
a non-numeric prefix so the test stops flaking, and record it as a workaround, not the fix.

## MINOR

### m1. `observe`'s refusal path posts through a closed door (`agent_executor.cpp:3786-3818`)

The `refuse` lambda runs after the first section and takes the ingress mutex with no admission test.
A reader that passed the first section, then loses the race to `acquireSwap`, charges the source,
logs an outcome and `postRefusals` pushes a `refusalsOf` item (`:4136`, plain `push`) after the
report counted `posts == 0`. The item is consumed by the first post-swap drain. Benign today
(charges are host memory, not program state) but the invariant "no post after the report" is false,
and the closed-door reader is charged to the device. Fix: test `admissionClosedLocked` first in that
scope and return `AdmissionClosed` uncharged; extend `APostBeforeTheGateIsCounted...` to a refusing
reading.

### m2. A batch's outcome is kept only if its last member changed something

`agent_executor_health.cpp:624-645`.

`finishHealthItem` writes the ring entry when `outcome.kind` (the last member's) is not `NoChange`.
A batch whose first members committed and whose last is `NoChange` leaves `healthOutcome` `Unknown`.
The design drops only an all-`NoChange` record. Fix: decide on `partialItems`
(`any_of kind != NoChange`).

### m3. Grant budget spent before the action can refuse (`agent_executor.cpp:1586-1599`)

`remainingActions--` precedes the `ArenaExhausted` return, so a refused action burns a grant action.
A batch is also not atomic against the grant: members past the remaining count answer
`UnauthorizedHealthAction` at turn time after the earlier ones committed, while the call returned a
receipt. Fix: decrement after the turn commits; state the batch partial-failure in the guide.

### m4. `resume` still mutates the host before the runtime can refuse

`agent_executor.cpp:5493-5515`.

T1-D m7b is open: `trustNode` and `issuer_.issue` run before `runtime_.resume`, and `resumes_++`
advances. The typed codes are clean (decided before), but a `Runtime` refusal from `runtime_.resume`
leaves the node trusted and a capability issued. Fix: `trustNode` after success, or add both to the
failure-path assertions. `resumeRefusal` also returns `nullopt` for a non-`CompiledProgram`
(`:5434`), so that case is decided at apply time with a live pid, as the design table says it should
not be; state it.

### m5. Per-pid host memory is never reclaimed

`lastRefusals_` (`:999`), `healthOutcomeRing_` and a lapsed `Admission` entry (`SwapGate` leaves
`pausedUntil` set, `agent_executor_internal.hpp:678-681`) outlive the pid; `forgetIngress`
(`:4096-4101`) clears only `healthActionSeq_` and `Ingress` maps. Growth is by pid, not by input,
but a long-lived host that resumes often accumulates. Fix: erase all three in
`forgetIngress`/`retire`.

### m6. Thread contract of the health calls is unwritten (`agent_executor_health.cpp:~213,~437`)

`observe` is documented any-thread and is. `healthAction`, `healthActionBatch`, `revokeHealthGrant`
and `healthOutcome` read `states_` and `runtime_.localCursor` unlocked (`:~213`, `:~437` reads
`checkpointsProduced`), so they are pump-thread only. `healthOutcome` is the one an operator thread
would poll. Fix: say pump-thread in the header, or move the `states_` reads under `healthMutex_`
with the writers.

### m7. `upgradeQuiescing` clobbers an operator pause (`program.cpp:398-427`)

It pauses for exactly `timeout` and calls `resumeAdmission` on every exit, which also cancels a
longer pause the operator had set. A drain that ends within the last tick reaches `acquireSwap`
after the pause lapsed and is refused `NotQuiescent{admission, 0}` (fails closed, but the subject
count `0` is `Open` and reads as noise). Fix: restore the previous `pausedUntil`; pause `timeout`
plus a settle margin; name the lapse.

### m8. No real-thread test of the gate

Every admission test uses `swapPoint` hooks (`vagent_epistemic_admission_test.cpp:205,275,590,625`),
which prove the two sections but not the lock order. `fuzz_epistemic_ingress` should run `observe`
and `healthAction` on two threads against `upgrade` under TSAN once, and the lock-order comment
(`agent_executor.hpp:1019-1023`) should be checked by it. Lock order itself reads correct: every
site takes ingress then health, `finishHealthItem` takes them one at a time, and nothing takes
ingress while holding health.

### m9. Latch-cache transition not tested after T1c (`vagent_epistemic_health_test.cpp:1697-1708`)

Rows set-by-isolate, set-by-arena-refusal, cleared-by-reset, stop and resume have tests. The
"committed upgrade clears the cache" row is only exercised as a refusal, with a comment that a
committed one is unreachable; since T1c the schema road carries a latched profile, so it is
reachable. Add the row.

## T1-D MAJORs (round 1 DIST)

- M1 observe two sections: fixed. Second section tests admission in the push scope
  (`agent_executor.cpp:3960`), first section is an early-out, hook `BetweenObserveSections`.
  Residual: m1 above.
- M2 health push outside any critical section: fixed. One scope, ingress then health, admission,
  back-pressure, receipt, ticket, reset generation and push (`agent_executor_health.cpp:222-310`,
  `:370-420`); every refusal precedes the ticket, so nothing leaks against `healthPending`.
- M3 resume budget refusals before the grant: fixed for `LocalHost` through `ResumeGrantProbe`
  (`program.cpp:355`, `agent_executor.cpp:5448`), which calls the same `grantLimits` as the
  authorizer. A host without a probe falls to apply time with `lastRefusal(pid)`; stated in the
  code.
- M4 stale binding for a removed predicate: fixed. `checkPredicateTransfer` keys on bound set union
  policy-bearing predicates, not on populated state, on both roads and again under the locks in
  `acquireSwap` (`agent_executor.cpp:5813`).
- T1-D m2 (gate released after the tail): fixed; the gate is a local held to function end (`:5859`)
  and cleared by RAII. m3 (pause ttl) is effectively handled by pausing for `timeout`, see m7. m4
  (mailbox count) fixed, `firstBusy` carries it.

## Intermittent: `Translate.ResumeAndUpgradeAgreeOverRandomIngestion`

Verdict: a real defect (M1 above), not a test-isolation issue. The test is the detector that happens
to roll the dice 24 times.

- XDG: no dependence. `LocalHost` and `checkpoint` use `UserPermascroll()` and `xanadu::Store`
  default constructors, which are in memory (`user_permascroll.cpp:342`); the only disk use is
  `/tmp/vagent-host-<random>-<epoch>`. After 6 concurrent processes sharing one XDG tree (3 repeats
  each, all passed) the shared `data/` directory was still empty.
- Time: wall-time enters only through `max_age 10s` and `base` set in `SetUp` (re-run for each of
  the 24 seeds); a seed would need a 5 s stall to go stale, and a stale refusal is mirrored on the
  resume and upgrade sides, so it could not fail equality.
- Seeds and ordering: the 24 seeds come from a fixed LCG; the order of events is deterministic.
- What is random: `std::random_device` in `Ingress::clockInstance`, `createMutableKeys`, health
  tokens. Only `clockInstance` reaches the digest (it is a record field).
- Runs: 15 sequential repeats with my own XDG, all pass (5.9 to 7.6 s each). 24 parallel processes,
  `Translate.*`, shuffled, 2 repeats each (48 runs of the suite): one failure,
  `Translate.PopulatedStateSurvivesResumeAndUpgrade` at `vagent_epistemic_translate_test.cpp:310`
  (`b.digest() != source`), in the second repeat, after `AnApplyTimeResumeFailure...` and
  `Bindings...` and passing in the first repeat of the same process. That is the same family: a
  resume digest mismatch with no state difference, in a test that populates records. Per-test
  probability `1.3e-3` fits one failure in about 250 digest comparisons; the random test makes 24
  executors, hence its observed rarity of a few percent.
- Not reproduced on demand (the id cannot be seeded). Confirm by making `clockInstance` `"%016llx"`
  produce an all-digit value in a unit test of `inferTextValue` and a resume.

## Confirmations

- hw M1 (R3-5, refusals never raise the mark; the estimator cannot freeze): CONFIRMED. `raiseMark`
  is called only by a commit (`agent_executor.cpp:1807`) and a pass credit within
  `healthSequenceGap` of the mark (`:4003-4007`); refusals only `rememberCounted`, bounded by
  `healthReplayRing` (`agent_executor_internal.hpp:490-495`); `estimatorCounts` refuses at or below
  the mark and for a counted sequence. The stated bound holds: a replay archive larger than the
  ring, all above the mark, is re-counted until the next commit (host memory, lost on restart).
  Related: M1 does not touch this row.
- hw M2 (R3-13, latch cache never an authority; one test per transition): CONFIRMED with m9. Set by
  arena `SourceFaulted` (`:1812`) and committed or `NoChange` isolate
  (`agent_executor_health.cpp:562`); cleared by committed reset (`:530`), `forgetIngress` on stop
  (`:1122`, `:2047`, `:6208`) and both upgrade roads (`clearLatchCache`, `:6183`, `:6287`, `:6298`);
  a resume is a new pid. Observe refuses from the cache before queueing or charging (`:3776`). A
  stale-set cache only refuses; a stale-clear one defers to the arena. Missing test: m9.
