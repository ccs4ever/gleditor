# I3-2 design review, round 2: compiler and codebase expert (persona codebase)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD), my round-1 report, and
the code at HEAD `3ae8d384` under `apps/common/xanadu/` (`vagent/runtime/` `compiler.cpp`,
`agent_executor.{hpp,cpp}`, `runtime_abi.hpp`, `program.{hpp,cpp}`, `epistemic_limits.hpp`,
`epistemic_ingress.hpp`, `reconciler.cpp`, `checker.cpp`, `vortex/vortex_vm.cpp`,
`system_docs.{hpp,cpp}`) and `tests/xuzz/` (ingress, settings, system_docs, policy, invariants),
read against P19 v1.5 sections 2.1, 2.2, 4, 5.1 and 8.2, and the I3-1 design with its section 14.
Read-only: the only repository file written is this report.

Method beyond reading. Three scratch probes were compiled and linked outside the repository against
the objects the lead built (`/tmp/p19-codebase/probe/probe.cpp`, compile line `cc.sh`, link line
from `/tmp/p19-lead/baseline-build.log` for `vagent_actor_test` minus its test objects) and run
headless with isolated XDG. The estimator table was re-derived in `/tmp/p19-codebase/est.py`.
Baseline check: `vagent_actor_test --gtest_filter='*Epistemic*:*Settings*'` passes 111 of 111 at
HEAD.

## Earlier findings

| Round-1 finding                              | Status in revision 2 | Why                                                                                                                                               |
| -------------------------------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 queued action authorized once             | Resolved             | 7.2 rechecks the token when the item becomes a turn, as `observe` does (`agent_executor.cpp:3958-3985`).                                          |
| M2 "shared advance body" is not a parameter  | Resolved, one gap    | 7.5 names `rtSourceDrop`, keeps `freshWindows` in the caller, carries `retained`. See m3 for what it still leaves out.                            |
| M3 reset re-poisoned by earlier refusals     | Resolved in intent   | The reset epoch (7.3) fixes the ordering. It introduces a new defect: M3 below.                                                                   |
| M4 health needs the whole control path       | Resolved, two gaps   | 7.3 names the four places, a receipt and a separate log. Gaps: M4 (charging) and D2 (reactor wake).                                               |
| M5 Int64 to Double breaks readers            | Resolved             | Lifetime counters stay Int; R and N are new fields. `ProfileHealth` and `counter()` keep working. See m10.                                        |
| M6 absence rules for `t_c` and bare profiles | Partly resolved      | `t_c` is gone; `rtHealth` reads absent as initial; tombstones are acted on. The existing `charge` lambda still hard-fails on absent counters: M2. |
| M7 wall time has two owners                  | Resolved             | The request has no `atWall`; the executor stamps wall time when synchronised (7.2).                                                               |
| M8 vocabulary and plumbing not listed        | Partly resolved      | Enumerators, turn results and `outcomeClassOf` are listed. Missing: the limits and note records (M1) and the charging list (M4).                  |
| m1 3% claim false at sample level            | Obsolete             | The estimator changed. I re-derived the new table (m1 below).                                                                                     |
| m2 "VM's own float checks" overstated        | Resolved             | 3.2 now says "bounded by construction". `Div` by zero returns 0.0 without failing (`vortex_vm.cpp:516-523`), so keep the explicit `N > 0` guard.  |
| m3 coalescing window depends on h            | Obsolete             | No clock. A lag in events replaces it: m2.                                                                                                        |
| m4 `Charge::at` as optional                  | Obsolete             | No `at`. The charge does need a `health` count: M1, m5.                                                                                           |
| m5 API shape                                 | Resolved             | Variant request, `inplace_vector<HealthActionKind, 4>`, chaining `revokeHealthGrant`, token, bounded grants. Constants still missing: m5.         |
| m6 settings plumbing                         | Not resolved         | The code path changed at HEAD (drift) and the design still describes the old one: M1.                                                             |
| m7 cell price of reasons                     | Resolved             | Default 4, actions 8, priced in section 9. Exempt actions are not bounded: m7.                                                                    |
| m8 resume write of `t_c`                     | Obsolete             | Nothing to rebase. I checked that R, N, `fault`, `reasons` and `actions` travel with the profile (see What is sound).                             |
| m9 isolate leaves records stamped NOMINAL    | Resolved             | Section 4 (two consumers) and isolate dropping supports. See m4 for what the stamp can now read.                                                  |
| m10 fuel for the health turn                 | Resolved             | Section 9 commits to measuring a 16-record reset before 2c.                                                                                       |

One retraction. In round-1 M8 I wrote that a missed `program.cpp` remap is "a silent no-op entry
after program upgrade". That is wrong: the executor takes its entries from
`CompiledProgram::instantiate`, which recompiles the unit into each context (`program.cpp:166-185`,
`agent_executor.cpp:1268-1283`). The remapped copy in `agents()` is read only for names and
container metadata (`reconciler.cpp:432`). The design repeats my overstatement; see m6.

## What is sound

- **The estimator is expressible with the opcodes that exist.** `Add`, `Sub`, `Mul` and `Div` on
  Doubles (`vortex_vm.cpp:454-523`) and `Exp` and `Ln` on a Float input only
  (`vortex_vm.cpp:922-945`) cover `R <- b^k R + (1 - b^k)/(1 - b)`. Rounding is monotone, so
  `R <= N` survives the two updates. Every operand must be a Double (`cFloat`); the integer settings
  need the `Add x, cFloat(0.0)` conversion the compiler already uses (`compiler.cpp:5756`).
- **Resume needs nothing new.** `restoreState` clones each profile through `vars`, `values`, `grab`,
  `step`, `clone` and `epistemic` (`agent_executor.cpp:4762-4783`), so R, N, `fault`, `reasons` and
  `actions` travel with value kinds intact, and only `last_receive` and `last_contact` are cleared.
  An event-indexed estimator has no clock to rebase. This is the right call.
- **The census counts the new records for free.** `epistemicCellsWithin` walks every profile and
  follows `vars`, `values`, `grab`, `step` and `clone` from it (`agent_executor.cpp:3305-3340`), so
  the provenance and action records are in the ceiling without a code change. (The design's
  `$epistemic_cells` is that function; there is no such identifier.)
- **`putScalar` creates an absent field** (`compiler.cpp:1441-1455`), so the R and N update is
  tolerant of a profile restored from before 2a.
- **Settle after commit fits the turn machinery.** A turn is bracketed by `arena.mark()` and either
  `release` (undo) or `discard` (`agent_executor.cpp:2082-2230`), a failing handler lands on
  `failOp` through `#Fail`, and the settle loop detaches the queue a round at a time
  (`compiler.cpp:5684-5722`). A second turn that runs `rtSettle_` and halts, with a third that
  detaches `d.pending`, is a small addition inside `deliverHost`.
- **The health settings stay out of the policy hash** (3.3 refuses a policy key), so a tuning change
  cannot refuse resume or upgrade.
- **Lifetime counters as Int, R and N as new Doubles** removes the round-1 reader breakage in
  `epistemic_invariants.hpp:70-71` and the 25 `EXPECT_EQ(counter(...))` sites.
- **Table in 3.3 reproduces.** `/tmp/p19-codebase/est.py` gives the same fractions and transition
  rates for H = 16, 32 and 64 at 5 to 50 percent, 14 events to SUSPECT from a clean history and 64
  events back from saturation.

## BLOCKING

None.

## MAJOR

### M1. Settings plumbing names a path that no longer exists and omits the limits records

Problem. Section 9 says "Six new `EpistemicLimits` fields ... `validate()` cases and
`EpistemicLimitError` enumerators ... a `defaultSettingSpecs` entry, a `settings::` name, the
expected list in `system_docs_test.cpp`". At HEAD the store values reach the executor through
`AgentSettings::fromModel` (`agent_executor.cpp:5496-5655`, new since `8ec71fc6`), which the design
never names. That function loads the epistemic block with `loadPositive`, a helper for positive
`size_t` values only. It then keeps the block only if `ep.validate()` succeeds (`:5649`), so one bad
value silently resets every epistemic setting, `cells` and `max_sources` included, to its default
(`vagent_settings_test.cpp:246` pins this: `EXPECT_EQ(settings.epistemic, EpistemicLimits{})`).

Evidence and consequences:

- The half-life, threshold fraction (Doubles) and the integer bounds need a `model.getDouble` branch
  that does not exist. Without it the three settings are inert: the host runs the defaults whatever
  `system://settings` says.
- Section 3.4 says out-of-domain values "refuse". The loader does not refuse; a typo in the
  half-life drops the operator's `max_sources` and `cells` too. The design must say which behaviour
  it adopts.
- The count is wrong: section 9 says six fields; sections 3.3 and 5 define five (half-life,
  fraction, min events, reasons, actions).
- The field types are not given (`size_t` or `double`), so `validate()` domains (NaN, infinity, the
  10^6 caps) cannot be reviewed.
- The tests the plumbing needs are `vagent_settings_test.cpp` (defaults at `:30-42`, custom values
  at `:165-177`, invalid values at `:234-246`) and `system_docs_test.cpp:174-184`, not only the
  latter.
- The limits reach the turn through the `ingest` record, which `emitObserve` reads with
  `readInOrder` against `kIngestOrder` (`compiler.cpp:5264-5271`), written in lockstep at
  `agent_executor.cpp:3996-4014`. The refusal note (`:3917-3955`), which needs the half-life for the
  R and N update, and the health record need the same treatment, and 3.4 promises the "turn record
  carries the values used". None of `kIngestHalfLife` and its siblings, nor a note field for the
  health-refusal count (`kRefusalHealth`), is in the section 9 constants list.

Required fix. Name `AgentSettings::fromModel` and the new loader branch for Doubles; state the types
and domains per field; decide and test the invalid-value behaviour (recommend: keep the
silent-default behaviour and say so, since changing it is outside this slice); list the `ingest`,
note and health record field names and extend `kIngestOrder` and its writer together; correct the
count; add `vagent_settings_test.cpp` to the 2a gate.

### M2. A revived bare tombstone cannot be charged, so its health never moves

Problem (a latent I3-1 defect in the routine 2a edits). A retire of a bound principal with no
profile mints a bare tombstone with only `incarnation` and `retired` (`compiler.cpp:4930-4943`). A
later rebind at a higher incarnation revives it by setting `retired` false and calling
`freshWindows` (`:4997-4999`), which adds streams and dedup but no counters. `emitObserveRefusal`'s
`charge` lambda finds each counter with `rtFind_` and ends `IngestionFault` when it is absent
(`compiler.cpp:4808-4817`).

Evidence (probe 1, `ProbeBareTombstoneRevivedThenRefused`): bind `kP3` at 4, retire, bind at 5, send
one frame-mismatch reading.

```text
PROBE1 observe -> ObservationFrameMismatch
PROBE1 content_refusals=-1 (-1 means absent) refused_count=-1 IngestionFault-stat=1
PROBE1 control kP1 content_refusals=1
```

The bookkeeping turn fails and is undone; the coalesced charge is already erased, so the refusal is
lost. The control source, whose profile was created by a reading, counts it.

Consequence. 2a puts the R and N update in this same all-or-nothing turn (3.2). For such a source
the undo discards it too, so a source that sends nothing but bad frames never reaches SUSPECT. A
rebound source after an operator retire is the common repair path, so this is where health matters.
The design's round-1 resolution row ("absent reads as initial; tombstones acted on") covers
`rtHealth` but not this lambda.

Required fix. Make `charge` find-or-create (reuse `putScalar`'s absent branch, or have
`emitProfileFor`'s counter initialiser run on revive). Add a 2a test with the probe's sequence that
expects `content_refusals == 1` and R advanced, and the tombstone cases of section 4 for `rtHealth`.

### M3. The reset epoch is bumped at call time, so a reset that never happens discards readings

Problem. 7.3 bumps the principal's epoch "At `healthAction(Reset)`", under the ingress mutex, and
refuses every item queued under an older epoch with `SupersededByReset`, a Terminal outcome. 7.2
rechecks the grant when the action becomes a turn and refuses `UnauthorizedHealthAction` if it was
revoked or expired. The two rules compose badly: an operator revokes a mistaken reset grant (or the
reset turn fails: `TurnExhausted`, a refused settle, an unbound principal's `NoProfile`) and the
readings queued before the call are already marked superseded. They are refused as "never resend"
though nothing was reset, and the host is told the bytes are dead.

Required fix. Bump on commit, with an ordering key that survives the delay. Stamp every queued
observation and note with a per-actor queue sequence at queue time. `healthAction(Reset)` records
its own sequence number in the item. When the reset turn commits (in the new finish step for health
items), store `resetFloor[principal] = thatSequence`. At the point an item becomes a turn
(`drainPosted` for observations, `deliverHost` for a prebuilt note), refuse `SupersededByReset` only
if its sequence is below the floor. Items queued after the call but before the commit keep their
sequence above the floor and run normally. The coalesced charge entry is dropped on commit, not on
call. Tests: a revoked reset leaves queued readings committable; a failed reset likewise; a reading
queued between the call and the commit survives.

### M4. New refusal codes are chargeable by default, which defeats the ingress mirror

Problem. `Ingress::chargeable` is a negative list (`agent_executor.cpp:902-910`): any code not named
is charged. 7.4 says `SourceFaulted` is "never charged" and that the mirror makes `observe` refuse
at ingress "without spending arena slots". But `observe`'s `refuse` lambda charges through
`firstCharge`/`charge` and posts a bookkeeping note for every chargeable code (`:3592-3615`), and
`finishIngest` does the same for arena refusals (`:1911-1932`). With the list unchanged, each
reading from an isolated source bumps `refused_count`, rewrites `last_refusal` and posts a
bookkeeping turn, which is the flood the mirror exists to stop. `SupersededByReset` has the same
default.

Required fix. State in section 9 that `SourceFaulted`, `SupersededByReset` and `SettleFailed` are
added to `chargeable()`'s exclusions, and that the health refusals of 3.1 are a new subset of
`isContentRefusal` (today that set also holds `StaleObservation` and `ObservationTimeMismatch`,
`:895-901`), which means `Charge` gains a `health` count and the note a field for it (see M1). The
fuzz invariant "a faulted source's later readings change nothing" is the test; add a unit test that
an isolated source's reading changes neither counters nor `last_refusal` and posts no bookkeeping
turn.

### M5. The settle contract in the executor is incomplete in three places

Problem. 6 says the turn "commits ... the executor then runs ... a settle turn" and that on failure
"the change's outcome becomes `SettleFailed` ... its state change stands". Against `deliverHost`
(`agent_executor.cpp:1851-1893`) and `finishSourceChange` (`:1936-1966`):

1. **The floor is not advanced.** `finishSourceChange` records the replay floor only when the code
   is `Committed` (`:1949-1953`), and for any other code it falls through to the stale/budget
   handling and returns. A `SettleFailed` advance has changed the arena's incarnation, but the host
   floor stays behind, so `bindSource` stops refusing a lower incarnation synchronously. The arena
   still refuses it, so this heals, but the design says the change stands. `SettleFailed` must be
   treated as `Committed` for the floor and still reported as the outcome.
1. **The extra turns must bypass the gates.** `deliverHost` refuses any entry once
   `arena.cellCount() >= arenaSlots` and applies `spec.admit` to bookkeeping turns (`:1855-1875`). A
   commit that crosses the slot allowance would then leave its events queued and refuse the settle
   turn, which is the leftover-event state 6 exists to prevent. The settle and discard turns, like
   2c's exempt actions, must skip both. (7.4 says the exemption for health actions; 6 never says it
   for settle.)
1. **Where it runs and what it counts.** The settle turn must be run inside `deliverHost` before
   `finishSourceChange` reports, not through the `settle()` batch loop (`:1969-2000`), which would
   interleave other host events. "stat() counts the discarded events" needs a named stat key
   (`kStatSettle...`) and a C++ walk of `d.pending` before the discard turn; say so.

Required fix. Add these three rules to section 6, and a test per rule: a `SettleFailed` advance
raises the host floor; a settle turn runs at the slot allowance; two queued source changes keep call
order across a failed settle.

## MINOR

- **m1. Test arithmetic for the estimator** (`est.py`). With the defaults, a fresh profile reads
  SUSPECT after one refusal among its first four readings (R/N = 0.2527 at N = 3.87), not only after
  three refused of three. Two refusals in five also reads SUSPECT. A 5 percent peer started from a
  fresh profile has a 13.4 percent chance of a SUSPECT excursion in its first 1,000 events (2,000
  trials), and a 30 percent source reads SUSPECT at a sampled instant 85.8 percent of the time. The
  2a "30% reads SUSPECT, 5% peers NOMINAL" test must assert an aggregate (fraction of steps after a
  warm-up of about 3H events) with a seeded generator, not a sampled state. Running it at 0.1, 1 and
  100 Hz tests nothing new, since the estimator has no clock; one run suffices.
- **m2. Charging lag.** The "below 2 for m < H" bound in 3.2 holds for m < 32 (default H). A note
  runs behind the ingestions already turned, up to `ingress_queue` (64), so the factor reaches
  `2^(64/32) = 4` for refusals in flight under a full queue. State the bound as a function of the
  queue and H, or cap the lag by bounding queued ingestions ahead of a note.
- **m3. Shape of `rtSourceDrop`.** In the code the advance has two walks: the first marks records
  below `inc` retired (`compiler.cpp:5003-5020`), the second moves every record's support chain and
  drops it (`:5021-5050`). The only parameter reset and isolate need is the first walk's filter:
  pass `inc` for the advance and the largest Int64 for "every record", with `retained`. The retire
  does not move records to the retired chain; it excises them (`:5093-5140`), so "mode: every
  record, then excise" is a different tail, not a mode. Recommend retire keep its own tail and share
  only the drop-one-support step, and change only the removal of its `rtSettle_` call (`:5136`) for
  2b. Gate for the refactor, by name: `AHigherIncarnationRetiresEveryKeyAndRestarts`,
  `RetiringASourceDropsItsSupportsAndKeepsAFloor`, `RetiringAnUnseenPrincipalLeavesABareTombstone`,
  `TheHistoryBoundExcisesRetiredRecordsAndViews`, `ReplacementBootsWithNewKeysStayBounded` and
  `vagent_epistemic_resume_test`.
- **m4. The stamp.** After 2c a latched profile refuses `SourceFaulted` before the stamp code runs
  (`compiler.cpp:5514-5522`), so the `FAULTED` stamp becomes unreachable; keep it only as a guard
  and say so. Section 4 does not say whether the stamp is read before or after this reading's own
  `N <- bN + 1`; the stamp test needs one answer. `kObsHealthSuspect` and `kObsHealthDegraded` are
  listed, but `kObservationOrder` is positional (`runtime_abi.hpp:609`), so check that a longer
  health string needs no layout change (it does not; it is a value).
- **m5. Grant constants and token.** `kMaxHealthGrantLifetime` and the code-length bound ("oversized
  code") have no value; `HealthGrantToken` and `HealthReceipt` are used and not defined. The
  executor has no random source beyond one `std::random_device` for the clock instance
  (`agent_executor.cpp:762`). A token that authorizes safety actions should come from a CSPRNG; the
  tree already uses OpenSSL `RAND_bytes` (`user_permascroll.cpp`, `transcopyright_crypto.cpp`). Name
  it. Compare tokens in constant time.
- **m6. The `program.cpp` remap and format version.** Add the three new entry labels to the remap
  for consistency with the existing three (`program.cpp:129-144`), but record that it is not
  load-bearing (retraction above). The check that matters is `prepare`'s `sound()` list
  (`agent_executor.cpp:1275-1283`), which tests neither the existing observe entries nor would test
  the new ones; add them. Also decide `kProgramFormat` (`program.hpp:41`): the compiled profile
  layout changes in 2a, and the design says a pre-2a profile "has a state", so keep the version and
  test the restore, or bump it and delete that sentence.
- **m7. "Bounded by bindings" is not a bound.** `bindSource` has no cap on bindings
  (`agent_executor.cpp:3425-3496`). Isolate is exempt from `max_sources` and the ceiling (7.4), and
  grants allow up to 16 x 128 named sources, so an operator can create up to 2,048 profiles. At the
  design's own ~150 cells per profile that is about 300,000 cells, against `vagent.value_operations`
  = 65,536, so the actor can no longer checkpoint. Give the exempt path a hard cap (for example
  `2 x max_sources` profiles) and say what is refused beyond it.
- **m8. The self-clear rule is checked one way.** 7.1 refuses a grant whose issuer is a bound or
  named source, but `bindSource` never checks existing grants, so binding the issuer afterwards
  restores the forbidden state. Re-check at `healthAction` and at turn time, or refuse the bind.
- **m9. List removal.** `clear` removes one reason from a `grab`/`step` list; the head hangs off
  `grab` and the rest off `step`, so removing the first element is a different relink from the
  middle or last (see the dedup trim at `compiler.cpp:5418-5430`). `unlinkFromRank` does one
  dimension. Add three tests (first, middle, last) and an eviction test for the actions list.
- **m10. Test harness changes.** `ProfileHealth` and `EpistemicSnapshot` gain R, N, the latch, and
  the three structures, or the "a refused action changes nothing" invariant is vacuous; add an
  assertion that the reads are engaged. R and N need a `decayed()` helper beside `counter()`
  (`vagent_epistemic_ingress_test.cpp:176`, which returns -1 for a Double). Grant expiry tests must
  inject `AgentClock::steady`; `advanceSimulatedTime` moves only `wallNow()`
  (`agent_executor.hpp:732`) and never the steady clock, so it cannot expire a grant.
- **m11. Citations moved.** Since `8ec71fc6`: `emitProfileFor` is `compiler.cpp:4691`,
  `emitObserveRefusal` `:4756`, `emitSourceChange` `:4858`, `emitObserve` `:5164`, `emitSettle`
  `:5684`; `Ingress::isContentRefusal` is `agent_executor.cpp:895`, not `:845`; `outcomeClassOf`
  `:3399`; profile constants `runtime_abi.hpp:636-650`; `CompiledAgent` entries `:860-880`. The
  design's citations should cite names, not lines.

## Questions for the owner

1. **Live upgrade (D1 below).** I3-1 section 10 recorded the owner's ruling that live upgrade keeps
   refusing populated roots until the typed translator exists. Confirm that this applies to the new
   `upgrade(pid, CompiledProgram)` path as well, and that the guard goes in before 2c.
1. **Reset and liveness.** Reset drops supports but leaves `last_receive` and `last_contact` (7.4).
   Should a reset clear them, so I3-3 does not read a reset source as live? I recommend clearing
   them and recording it in the action record.
1. **Mirror lifetime.** Should the ingress mirror of a latched source clear only on a committed
   reset (recommended), never on a retire and revive?

## Fit to later iterations

**I3-3 (heartbeats, clock mapping, sensor reports).**

- Nothing makes it unsound. The estimator has no clock, so clock mapping and heartbeat intervals do
  not interact with it. I3-2 must record that I3-3 owns the `last_receive` meaning: absent is "never
  supplied a reading", and a reset leaves it set (question 2).
- A sensor-added reason must be able to carry no grant ID. 5 gives every reason a `grant` field;
  state that it is optional and that provenance for telemetry reasons is the source's own token.
  Uniqueness by (kind, code) means an operator's `degrade X` and a sensor's report of code X are one
  reason with the first provenance, so "its recovery percept clears only reasons that source added"
  cannot clear it. Either namespace codes by origin or key uniqueness by (kind, code, issuer).
- `at_wall` is written only when the wall clock is synchronised; I3-3's clock mapping should reuse
  that rule.

**I4 (queries, scoring, gates, directives).**

- `rtHealth(profile, limits)` is read in ingestion, where `$turn.ingest` supplies the limits. A
  query or dispatch turn has no such record, so I4's "recompute at query time" has no source for the
  three settings. I3-2 should record the channel: either a reserved agent field the executor
  refreshes at each turn start, or the three values in the scoped-evaluation grant that already
  reaches the VM (`scopedEvaluationGrant(grant, settings_.epistemic)`, `agent_executor.cpp:2107`).
- R and N are the continuous fraction I4 wants; the gate estimator must share them or add a sibling
  pair. Say which, and record that `rejection_streak` thresholds apply to the state rule only.
- 7.4 says I4 resets calibration "in this same turn". `healthEntry` should be emitted so that the
  calibration reset is one more call inside the entry, not a second turn.
- `healthEntry` creates profiles outside `max_sources` for isolate; I4's eligibility must not read
  profile count as the live-source count.

**Live-upgrade translator.**

- The translator must carry `health_refused`, `health_events`, `faulted`, `fault`, `reasons` and
  `actions` verbatim, since dropping them sheds an operator's latch and its provenance. They hang
  off `d.trust` and `d.epistemic`, the two roots the old path refuses to touch.
- Record that the health settings are not in the policy hash, so a translator need not compare them.
- D1 below is the blocker: today a second path discards these roots without a word.

## Drift audit 8ec71fc6..HEAD

The 34 later commits touched the listed files. I diffed each. Items that break or weaken an accepted
P19 invariant or conflict with the I3-2 design come first, with severity.

### D1. MAJOR: the new reconciler upgrade path sheds source profiles, latches and floors

Invariant: I3-1 section 10 and section 14: "Live upgrade keeps refusing populated roots until the
typed translator exists", and the owner's Q2 ruling that latches live for life.

What changed. `AgentExecutor::upgrade(pid, CompiledProgram &)` (`agent_executor.cpp:5418-5467`, with
`LocalHost::upgrade(pid, CompiledProgram)`, `program.cpp:399`) runs `makeStateReconciler`
(`reconciler.cpp`, new in `b6a3d769`). The old path refuses when `d.trust` or `d.epistemic` is
linked and no translator is registered (`agent_executor.cpp:5314-5319`). The new path has no such
check, and the reconciler's `AgentDims` omit `trust` and `epistemic` (`reconciler.cpp:459-475`). Its
`cloneGraph` follows only the dimensions it is given and ignores every other link
(`reconciler.cpp:94-130`), so it neither copies nor reports them. It does copy the arbitrated
predicate's clauses and their `source` ranks (`:620-668`).

Evidence (probe 2, `ProbeReconcilerUpgradeShedsProfile`, with the latch set by hand as isolate
will):

```text
PROBE2 before: profiles=1 faulted=1 clauses=1
PROBE2 old path (shared_ptr overload): refused
PROBE2 new path (CompiledProgram overload): accepted
PROBE2 after: profiles=0 clauses=1 support-history=0
```

Consequence. The upgrade silently drops the profile (and so the FAULTED latch, the incarnation
floor, the dedup window and every history record) and leaves the belief with a support that has no
records: the invariant `violation()` already checks, "support without a profile". For I3-2 it means
an operator's isolate is shed by a program upgrade, which contradicts "latches for life" and the
whole of 2c. `vagent_upgrade_test.cpp` uses this overload eight times and none covers an arbitrated
predicate, so nothing fails today.

Required fix, before 2c (it belongs in the commit that introduces the first latch writer, or
before): in `upgrade(pid, CompiledProgram &)` refuse with the same error when `d.trust` or
`d.epistemic` is linked on the old agent and no `ownedStateTranslator_` is set; or make the
reconciler fail when it meets either root. Add a regression test using the probe's sequence
(`profiles()` unchanged and the upgrade refused), and record in the I3-2 design that the guard
covers both overloads. The TUI uses the old overload (`agent_tui.cpp:649`), which is guarded.

### D2. MAJOR: control posts do not wake the reactor, so a health action can wait for ever

Invariant: none of the accepted P19 text; it conflicts with I3-2 7.3, which routes health actions
through `PostBox::pushControl`, and with the safety purpose of isolate.

What changed. `PostBox::push` now writes an `eventfd` when one exists
(`agent_executor.cpp:602-610`), and `notificationDescriptor()` exposes it so a reactor can
`poll(2)`. `pushControl` and `putBack` (`:642-655`) were not changed and do not write it. The
`vagent` reactor blocks with `timeoutMs = -1` whenever there is no pending input and no actor
deadline (`apps/vagent/main.cpp:1279-1320`).

Evidence (probe 3):

```text
PROBE3 after drain readable=0
PROBE3 after bindSource (pushControl) readable=0
PROBE3 after retireSource (pushControl) readable=0
PROBE3 after observe (push) readable=1
```

Consequence. An operator's `healthAction(Isolate)` from another thread is queued and sits until some
other post or the next timer. Bind and retire already have this behaviour (an I3-1c defect); 2c adds
the case where it matters.

Required fix. Write the eventfd in `pushControl` and `putBack` (factor the write out of `push`), and
add a unit test beside the probe: after `drainNotifications`, a control post makes the descriptor
readable. Name the requirement in 7.3.

### Other changes, none of which breaks a P19 invariant

- **Arbitrated predicates stay ingress-only.** `assert` and `retract` (and `... from`) are new
  `EBeliefChange` kinds (`ast.hpp`, `parser.cpp:1575-1585`). The checker's `EpistemicIngressOnly`
  test runs after the kind switch for every kind (`checker.cpp:2680-2686`), the
  `for believes ... from` and `findall ... from` forms refuse arbitrated predicates (`:2536-2545`,
  `:2593-2599`), and the compiler's own guard still precedes the `from` evaluation in `beliefChange`
  (`compiler.cpp:6048-6052`). `rtBSet_` and `rtBDrop_` still have the same call sites; `Retract`
  shares the `Forget` branch.
- **P19 emitters untouched.** No diff hunk falls in `emitProfileFor`, `emitObserveRefusal`,
  `emitSourceChange`, `emitObserve` or `emitSettle` (hunks are at `effectCall`, `mintPredicates`,
  `findallExpr`/`forBelieves`/`beliefChange`, `emitInit`, `emitDispatch`, `capacityCall` and `run`).
  `run` still emits the three entries together (`:7536-7541`).
- **`branchIfZero` changed meaning** (`compiler.cpp:920-924`): from `Eq r, 0` to `Not r`, a
  truthiness test (`VortexCore::evaluateTruthiness`, `vortex_core.cpp:323`). Cell refs, Ints,
  Doubles and Bools behave as before; a string register holding "", "0" or "false" now takes the
  zero branch. I3-2 IR must not call it on a string value, and the 2a estimator must test Doubles
  explicitly (`Div` by zero returns 0.0).
- **`KeyMustPrecede`** (`checker.cpp`, and a compiler `bail`) now refuses a key parameter after a
  non-key one. Before, `nkeys` silently stopped at the first non-key. This tightens arbitrated
  predicates and cannot weaken them.
- **`AgentSettings::fromModel`/`fromStore`/`fromManifold` and `EpistemicLimits::operator==`** are
  new: they are the settings path M1 describes. The `==` is `= default`, so Double fields compare
  exactly, which is right for validated values.
- **`launch` now creates the `State` early** (`agent_executor.cpp:2984-2990`) and `live(state)`
  replaced `state.context && state.prepared` in several readers. `bindSource` and `drainPosted`
  accept a launched but unprepared pid, as before. No behaviour change for P19.
- **`advanceSimulatedTime`** offsets `wallNow()` (`agent_executor.hpp:732`) but not the steady
  clock. Binding and grant lifetimes use `clock_.steady()`, so simulated time cannot expire them
  (m10); wall windows in `observe` do move.
- **Old `upgrade` no longer runs the container `hooks`** (`agent_executor.cpp`, deleted loop). The
  skill upgrade hook now runs from the `Upgrade` dispatch (`compiler.cpp:6789-6812`). Not P19.
- **`ast.hpp`/`token.hpp`/`upgrade_schema.cpp`**: `assert` and `retract` are now reserved words, and
  the schema hash gains tags for the new kinds. A program that used either as an identifier no
  longer parses; the P19 test programs pass (111 of 111).
- **`epistemic_limits.hpp`**: `operator==` only. **`system_docs.{hpp,cpp}`**: new millisecond
  variants, `max_instructions`, `spawn_stamp`, TUI settings, plus formatting changes. The Schema
  page still lists no `vagent.epistemic_*` setting. **`lexer.cpp`, `parser.cpp`** otherwise
  formatting and the `from` forms.

## Summary of counts

Tally: 0 BLOCKING, 7 MAJOR (M1 to M5, D1, D2), 11 MINOR.

Verdict: FAIL
