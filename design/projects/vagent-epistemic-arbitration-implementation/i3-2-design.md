# I3-2 design: source health, settle-after-commit and operator health actions

Status: revision 3. Revision 1 (`e226c507`) failed all four reviews; revision 2 (`cae4a6ed`) failed
all seven round-2 reviews with no BLOCKING finding and 47 MAJOR ones, merged into 33 findings F1-F33
and 38 MINOR ones m1-m38 ([findings](i3-2-review/findings-r2.md), [reports](i3-2-review/)). Section
12 maps every one to its resolution. Authority: approved P19 v1.5 sections 2.1 (health
preservation), 4 (`EpistemicHealthGrant`, `UnauthorizedHealthAction`) and 5.1 (latch, reasons,
displayed state); the owner's decisions on [open question 2](i3-open-questions.md) (latches for
life, counters as decayed rates with an ML-reviewed half-life) and on revision 1 (**add an operator
`clear`** that removes degradation reasons and never unlatches; **fix the `-pred` handler wedge**);
and the lead's rulings in [decisions.md](decisions.md) section 1, with the merged owner questions
taking their recommended answers. Code citations are `path:line` at HEAD `8a4089d9`; paths are
relative to `apps/common/xanadu/` unless they start with `tests/`. Revision 2 cited `8ec71fc6`.

## 1. Scope, slices and what T0 already did

I3-2 gives each source profile a health that the arena keeps, that a restart of the device cannot
shed, that an authorized operator can change, and that evidence can heal. It ships as four slices,
each of which builds, passes every earlier test and can be reviewed and committed alone:

| Slice | Content                                                                                                            | Gate               |
| ----- | ------------------------------------------------------------------------------------------------------------------ | ------------------ |
| 2a    | estimator, health refusal classes, derived state and stamp, `sourceHealth`, settings plumbing, the I3-1 charge fix | ML, then all three |
| 2b    | settle after commit, one settle turn per pending event, `rtSourceDrop`, checkpoint refuses a linked `d.pending`    | all four           |
| 2c1   | grants, tokens, host health API, authorization at every boundary, control-item ordering, host stamps, a stub turn  | security, then all |
| 2c2   | Vortex health turn (isolate, degrade, clear, reset), arena markers, audit, budgets and reserves, idempotency       | all four, ML       |

Order and why: 2a changes the profile and the bookkeeping note, which nothing else touches. 2b comes
before any health action because isolate and reset drop supports and a failing `-pred` handler must
not block them. 2c1 builds and tests the whole host side against a stub turn that answers
`HealthUnavailable`, so the authorization surface is reviewed before it can change any state; 2c2
replaces the stub. Moved out (section 10): cross-key retained transfer and user-level revision (I4),
sensor-originated health reports (I3-3), and the `reset`, `isolate`, `degrade` and `clear`
directives in the actor language (I4, onto 2c2's turn).

**Done in T0, not I3-2 work.** T0 is an uncommitted change in the working tree. `upgradeRefusal`
(`agent_executor.cpp`, new) holds the quiescence and populated-root guards that both `upgrade`
overloads now share, so the `CompiledProgram&` overload no longer sheds `d.trust` and `d.epistemic`
(F31). `PostBox::signalLocked` is called from `push`, `pushControl` and `putBack`, so every post-box
enqueue wakes a reactor (F32). T0's tests are in `tests/xuzz/vagent_epistemic_ingress_test.cpp`.
This design assumes both. Line numbers for `agent_executor.cpp` below are HEAD's, which T0 shifts by
up to 38 lines.

**Entry order and files.** E1 (plan decision) puts the code generator's declaration in a private
header, `vagent/runtime/compiler_gen.hpp`, so new emitted routines live in new files
(`compiler_health.cpp`); if E1 has not landed when 2a starts, 2a lands it first and moves nothing
else. New host code goes in `vagent/runtime/epistemic_health.{hpp,cpp}` (state, snapshot, bounds)
and, from 2c1, `vagent/runtime/health_grants.{hpp,cpp}` (token, grant, request, receipt). The
Makefile source lists and the `vagent_actor_test` object list name each new file.

## 2. What exists at HEAD

From I3-1, a profile on `$agent +d.trust` (`compiler.cpp:4691`, `emitProfileFor`) holds `faulted` (a
Bool latch nothing sets), four Int64 lifetime counters (`rejection_streak`, `reject_count`,
`content_refusals`, `refused_count`), `streams`, `dedup`, `last_receive`, `last_contact` and
`last_refusal` (`runtime_abi.hpp:635-651`). Nothing reads a counter. The host charges refusals in
`Ingress::charge` (`agent_executor.cpp:877`) into a coalescing
`Charge {content, other, code, contact}` (`:748`), posts one note per principal, and `drainPosted`
turns the note into `observeRefusalEntry` (`compiler.cpp:4756`). `isContentRefusal` (`:895`) is the
list of codes that feed `content_refusals` and `last_contact`; `chargeable` (`:902`) is a deny list.

Four defects at HEAD matter here. `charge` (`compiler.cpp:4808`) hard-fails on an absent counter, so
a refusal of a revived bare tombstone is lost (F8). Ingestion stamps `health` NOMINAL or FAULTED
from the latch only (`compiler.cpp:5517`). `emitSourceChange` and `rtObserve` end in `rtSettle_`
(`compiler.cpp:5089`, `5136`, `5668`), so a failing `-pred` handler undoes the advance, retire or
ingestion that raised it. The epoch and mapping regressions of a stream both end
`ObservationTimeMismatch` (`compiler.cpp:5369`, `5375`).

## 3. Slice 2a: estimator, derived state, read API, settings

### 3.1 Which outcomes are evidence about a source

A **health refusal** is a refusal whose cause is the bytes or clock the device itself produced. The
set is decided at the check site and published as a versioned table in `epistemic_health.hpp`
(`kHealthRefusalCodesVersion = 1`, pinned by the ABI test). C++ decides it because most checks run
in `observe`, before the arena sees the event (decision F6); the IR-decided `ClockEpochRegression`
is counted through the same host list.

| Code                         | Decided at                                                                 | Basis         | Counts for a direct copy | For a relayed copy |
| ---------------------------- | -------------------------------------------------------------------------- | ------------- | ------------------------ | ------------------ |
| `ObservationFrameMismatch`   | `observe`                                                                  | signed fields | yes                      | yes                |
| `MissingVariance`            | `observe`                                                                  | signed fields | yes                      | yes                |
| `InvalidVariance`            | `observe`                                                                  | signed fields | yes                      | yes                |
| `InvalidValue` (new)         | `observe`, tuple not finite or mistyped                                    | signed fields | yes                      | yes                |
| `InvalidAcquisition` (new)   | `observe`, `acquiredAt` or uncertainty not finite, or uncertainty negative | host-mapped   | yes                      | no                 |
| `ClockEpochRegression` (new) | `observeEntry`, `compiler.cpp:5369`                                        | host-mapped   | yes                      | no                 |

A relay can alter only what the origin did not sign (the digest, `agent_executor.cpp:3666-3668`), so
a relayed copy counts only for refusals decided on signed fields; otherwise a hostile relay could
make its victim SUSPECT (decision F1(b), which counts relayed outcomes once per event).

Not health refusals, and moving neither `R` nor `N` (m1) in the host or the IR: `StaleObservation`,
`ObservationTimeMismatch` (the host clock-basis and future-time checks, and the order checks at
`compiler.cpp:5554`, `5565`), `UncertainAcquisition` and the new `ClockMappingRegression` (the
mapping version of a stream regressed within one epoch, today `compiler.cpp:5375`). They are the
**host time class**: a link outage, a clock-mapping fault or a congested relay produces them from a
healthy device, and one host fault would move every source at once. They still feed the lifetime
`content_refusals` total and `last_contact`, as today. I3-3 reinstates them once path lateness is
separable (section 13). `InvalidObservation` keeps only what is wrong with the request itself (empty
event ID, an integer past Int64, tuple arity, oversize encoding) and stays diagnostic.

`isContentRefusal` gains `InvalidValue`, `InvalidAcquisition`, `ClockEpochRegression` and
`ClockMappingRegression`, so the lifetime total and the contact stamp still include every code that
showed a live device. A new `Ingress::isHealthRefusal(code, relayed)` reads the table above.

### 3.2 The estimator

Health is an exponentially weighted fraction indexed by **events**, not seconds. Two Double fields
on the profile, `health_refused_weight` (`R`) and `health_weight` (`N`, renamed from rev 2's
`health_events` because it is a decayed weight, m36), both absent until first written and then read
as 0. With half-life `H` events and `b = 2^(-1/H)`, computed in the IR as `Exp(-ln2 / H)` from the
integer setting (m9), so no two hosts can differ by an ulp:

- an event **accepted** (committed, direct or relayed, never a duplicate): `R <- b R`,
  `N <- b N + 1`;
- `k` health refusals: `R <- b^k R + (1 - b^k)/(1 - b)`, `N <- b^k N + (1 - b^k)/(1 - b)`, with
  `b^k = Exp(-k ln2 / H)`;
- after each update `N <- min(N, 1/(1 - b))` and `R <- min(R, N)`, so a shrunken `H` takes effect at
  the next update (m10, decision F33).

The fraction is `f = R / N` for `N > 0`. A source falling silent keeps its fraction: P19 5.1 says
silence establishes missing data, not its cause. The update needs no clock, so nothing rebases on
resume and `R`, `N` checkpoint as they are.

**One population, once per event (F1, F4, m2).** Every distinct (incarnation, event ID) updates `R`
and `N` once, by the first of: a health refusal, a commit, a pass credit.

- A health refusal counts for a direct or a relayed copy (table above) the first time the event is
  charged. `Logged` (`agent_executor.cpp:709`) gains `counted`, set when any of the three has
  updated the estimator; `firstCharge` returns false for a counted event.
- A **pass credit** answers load bias: a reading that passed every check in `observe` and was then
  dropped by `refuse(IngressBackpressure)` (`:3770`) adds one to the principal's `Charge::passed`
  and sets `counted`. Without it `f` is bad-offered over bad-offered plus good-served: with 1000
  readings offered and 5% bad, every bad one is charged in `observe` while only 2% of the good ones
  are served, `f` peaks at 0.78 against 0.06 with the credit (simulation, section 9). When the
  device resends the dropped event and it commits, the executor tells the turn
  (`$turn.ingest.credited`) and the IR skips the accept update.
- A commit of an event already `counted` by a health refusal cannot happen: the same signed bytes
  refuse the same way. An event whose log entry was evicted (`outcome_log` entries per source,
  default 64) can count twice; that is the bound on the error.
- Relay share therefore does not change `f`: at a 20% defect rate the mean of `f` is 0.199 at 0%,
  50% and 100% of events relayed. Revision 2's rule gave 0.199, 0.111 and 0.
- A bookkeeping turn applies its pass credits first, then its refusals, so any within-note disorder
  errs toward suspicion.

**Order (F2).** Notes could drain after accepts queued behind them, which errs toward trust, and
coalescing errs toward suspicion; revision 2 claimed only the second. Two rules bound both. First,
`observe` closes a principal's open `Charge` when it queues an accept for that principal, so the
next refusal opens a new charge and a note queued after the accept: refusals decided in `observe`
keep queue order exactly. Cost: an alternating refuse/accept source posts one note per refusal run,
at most one extra turn per accepted reading, and a pure refusal flood still coalesces to one note.
Second, a refusal decided in the arena (`ClockEpochRegression`) is charged after the turn that found
it, behind at most `ingress_queue_per_source` (8) events of the same principal. A displaced member's
weight is off by `b^(-m)`, `m <= 8`: 1.19 at the defaults, at most 2 for any valid `H >=` that
bound, which `validate()` requires (3.3). A test measures both directions.

### 3.3 Settings, defaults and what they mean

| Setting (`vagent.` prefix)     | Field in `EpistemicLimits` | Type  | Default | Domain                                   |
| ------------------------------ | -------------------------- | ----- | ------- | ---------------------------------------- |
| `epistemic_health_half_life`   | `healthHalfLife`           | Int   | 32      | \[`ingress_queue_per_source`, 10^6\]     |
| `epistemic_suspect_fraction`   | `suspectFraction`          | Float | 0.25    | finite, (0, 1\]                          |
| `epistemic_suspect_min_events` | `suspectMinEvents`         | Int   | 8       | [1, floor(0.9 / (1 - b)) = 42 at H = 32] |

`min_events` is the weight `N` at which `f` starts to decide, not a count: `N` after `k` events is
`(1 - b^k)/(1 - b)`, 7.43 after 8 and 8.27 after 9, so the first reading that can read SUSPECT is
the ninth (the revision 2 default of 3 was reached at the fourth, where one refusal in four gave
0.2526). A value above `1/(1 - b)` would disable SUSPECT; `validate()` caps it at 90% of that bound.
`EpistemicLimitError` gains `InvalidHealthHalfLife`, `InvalidSuspectFraction` and
`InvalidSuspectMinEvents`, so a warning can name the field.

Simulated (Bernoulli refusals, 4x10^5 events, seed 1, warm-up 3H; the generator is the test
`HealthEstimatorSimulation` of section 9, not a script, m7): the share of events spent SUSPECT, and
transitions per hour at 1 Hz.

| True refusal rate | H = 16       | H = 32 (default) | H = 64     |
| ----------------- | ------------ | ---------------- | ---------- |
| 5%                | 0, 0         | 0, 0             | 0, 0       |
| 10%               | 0.2%, 6/h    | 0, 0.1/h         | 0, 0       |
| 15%               | 3.9%, 76/h   | 0.6%, 12.5/h     | 0, 0.4/h   |
| 20%               | 19.9%, 256/h | 12%, 132/h       | 5.1%, 48/h |
| 30%               | 76%          | 85%              | 93%        |
| 50%               | 100%         | 100%             | 100%       |

From a clean history a source refusing every reading reads SUSPECT after about 14 events and one
refusing half of them after 31 to 33; from SUSPECT at a 100% rate it returns to NOMINAL after about
64 good readings (m3: tests assert bounds, not these counts). A fresh profile false-alarms within
its first 12 events 1.9% of the time at a true 5% refusal rate and 10.7% at 10% (13% and 30% at
revision 2's `min_events` of 3). A repaired source reporting once an hour stays SUSPECT for about 64
hours; only a reset shortens that, and the Schema notes say so (m12).

The declared interpretation (P19 5.1): **SUSPECT means about a quarter or more of the source's last
few dozen outcomes were unusable.** A source refusing under 10% stays NOMINAL; between 15% and 25%
the state is noisy by nature. The state is for display, the latch and audit; I4 uses the continuous
`f` (section 13). This is not P19's "three consecutive rejections", a streak of gate rejections that
I4 adds to the state rule with its own thresholds (three SUSPECT, five FAULTED).

Validation: `validate()` rejects `healthHalfLife` below `ingressQueuePerSource` or above 10^6, a
non-finite, non-positive or above-1 `suspectFraction`, and a `suspectMinEvents` of 0 or above
`floor(0.9 / (1 - b))`. Refused alternatives are unchanged from revision 2: a time-decayed count
(traffic-dependent, unreachable for slow sources), a time-decayed fraction (the half-life would have
to follow each binding's cadence), a policy key or grant field (health is per profile, and a changed
key would refuse resume), and a ring of the last `N` outcomes (cells proportional to `N`).

### 3.4 Derived state, the stamp and what each state licenses

The state is derived, never stored, by one emitted routine `rtHealth(profile, limits)` returning the
state and the code that decided it (m13: it divides only when `N >= min_events`, and never calls
`branchIfZero` on a string):

1. `faulted` latched: **FAULTED**, with the fault's code (from 2c2; before it, code `faulted`).
1. A `suspect`-kind reason, or `N >= min_events` and `f >= suspect_fraction`: **SUSPECT**, code of
   that reason or `refusal_fraction`.
1. A `degraded`-kind reason: **DEGRADED**, with its code.
1. Otherwise **NOMINAL**.

An absent field reads as its initial value, so a tombstone or a profile restored from before 2a has
a state. 2a creates no `reasons` list, so its tests reach step 1 and 2 only and the reason branches
are tested by a synthetic profile in 2a and by real actions in 2c2 (m33, m35).

**The stamp (m11).** Ingestion applies its own update, then writes the record's `health` (the state)
and the new `health_fraction` (`f` after the update; a Float, `kObsHealthFraction`, one position
after `health` in `kObservationOrder`, which grows to 24 entries). A stamp therefore describes the
source including this event, depends on the order charges were applied and is not replay
deterministic; it exists for audit and for calibration's view at arrival. `FAULTED` stamps become
unreachable once 2c2 refuses latched readings, and the constant stays for restored data.

**What each state licenses (m5).** NOMINAL: nothing withheld. DEGRADED: displayed; I4 applies the
policy's inflation once. SUSPECT: displayed and, in I4, inflated; its readings are still stored and
still calibrate. FAULTED: no new reading is recorded (2c2) and existing supports are withdrawn. I4's
eligibility recomputes the state at query time through `rtHealth` without writing, never from the
stamp; an isolated source's earlier records stay stamped as they were.

A profile created by refusals alone (I3-1 question 3) has no `last_receive`. That state is named:
nothing of it is eligible, its fraction and latch apply, a reset of it retires nothing, and I3-3
treats an absent `last_receive` as "never supplied a reading".

A new incarnation inherits `R` and `N` (the advance keeps counters as it keeps the latch; the
fraction heals on the new device's traffic) and a reset zeroes them (m4).

### 3.5 The read API (F18)

```cpp
enum class HealthState : std::uint8_t { Nominal, Degraded, Suspect, Faulted };

struct SourceHealth {            // 2a fields; 2c2 adds the rest, see 6.8
  std::string principal;
  HealthState state;
  std::optional<std::string> reasonCode;   // what decided it; empty for Nominal
  double refusalFraction;        // f, 0 while N is 0
  double weight;                 // N
  bool faulted, tombstone;
  std::uint64_t incarnation;
  std::int64_t contentRefusals, refusedCount, rejectCount, rejectionStreak;
  std::optional<std::string> lastRefusal;
};
[[nodiscard]] std::optional<SourceHealth>
sourceHealth(const identity::ActorPid &pid, std::string_view principal) const;
[[nodiscard]] std::vector<SourceHealth> sourceHealths(const identity::ActorPid &pid) const;
```

Both are read-only host calls on the pump's thread (as `epistemicCells`, `agent_executor.cpp:3305`
region), `nullopt` or empty for an unknown actor, an actor in a turn, or no profile. They are C++
because a read-only turn would spend arena slots, which are never reused, on every poll; C++ derives
the state from the cells with the same constants. The routine of 3.4 is the one the arena uses, so a
test sweeps profiles through every state and asserts the C++ view equals the stamp the IR wrote. A
view that disagrees is a test failure, not a second authority. `sourceHealths` is bounded by
`max_sources`.

### 3.6 Settings plumbing (F30)

`AgentSettings::fromModel` (`agent_executor.cpp:5496`) loads the epistemic block through
`loadPositive` (`:5624`), which reads Int only, then validates the whole block and keeps the
defaults for all of it if any field fails (`:5649`). 2a adds a Float loader (`model.getDouble`,
finite check) beside it, loads the three fields, keeps the whole-block fallback (decision F30(A))
and adds a WARN naming the failing field through a new `epistemicLimitErrorName`. A bad health
setting therefore also resets `max_sources` and `cells` to defaults; the Schema notes and the test
say so. Each setting gets a `settings::` name (`system_docs.hpp:294-318` region), a
`defaultSettingSpecs` entry with `expectedTypes` `integer` or `float` (`system_docs.cpp:1014`
region), a line in the expected list of `tests/xuzz/system_docs_test.cpp:184` region, and Schema
text with the interpretation of 3.3. `tests/xuzz/vagent_settings_test.cpp` is in the 2a gate. A
changed setting does not rescale history: `R` and `N` are read under the settings in force and
clamped at the next update (A2, section 11). The three settings are not in the policy hash.

### 3.7 The I3-1 charge fix (F8)

`charge` becomes find-or-create: when the counter field is absent it creates it with the increment
(`callSetField`, as `emitProfileFor` does), and `R`/`N` use the same helper. A revived bare
tombstone, a profile restored from before 2a and a profile an operator left without counters are all
chargeable. The probe sequence is the test: bind at 4, retire an unseen principal, bind at 5, one
frame mismatch, and expect `content_refusals == 1` with `R` advanced.

### 3.8 Slice 2a: files, split, price and tests

| Where                                                       | What                                                                                                                                                          |
| ----------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `vagent/runtime/runtime_abi.hpp`                            | constants of section 7; `kObservationOrder` 24 entries; `kIngestOrder` grows by `half_life`, `suspect_fraction`, `min_events`, `credited`                     |
| `vagent/runtime/compiler.cpp`, `compiler_health.cpp`        | `rtHealthUpdate_`, `rtHealth_`, the stamp, the charge fix, the two split time codes, note fields `health`, `passed`                                           |
| `vagent/runtime/agent_executor.cpp/.hpp`                    | `Charge::health/passed`, `Logged::counted`, `isHealthRefusal`, close-on-accept, `InvalidValue`/`InvalidAcquisition` in `observe`, `sourceHealth`, `fromModel` |
| `vagent/runtime/epistemic_limits.hpp`, `epistemic_health.*` | three fields, `validate()`, `HealthState`, `SourceHealth`, the counted-code table                                                                             |
| `system_docs.hpp/.cpp`, `program.hpp`                       | three settings; `kProgramFormat` becomes `vagent-program-4` (the `$turn` records change shape)                                                                |

C++ versus Vortex. The estimator, the state rule and the stamp are emitted IR. C++ is the counted
list and the once-per-event bookkeeping (a decision made in `observe` before the arena sees the
event: transport), the pass credit (a host queue condition) and the read API (above).

Price at the defaults (H 32, history 4, keys 4, 128 sources, 24,576 cells). Cells: 4 per profile
(`R`, `N`), 2 per record (`health_fraction`), so a record is about 82 cells and a fully active
principal of 16 records plus its profile takes about 1,360 cells (1,320 before): the ceiling still
holds 18. 2a also measures a fresh profile after one commit and pins `kProfileBaseCells` (expected
44\) for 6.6. Cycles, estimated: about 100 VM cycles per ingestion (an `Exp`, a `Div`, two field
finds and the state rule) and about 100 per bookkeeping turn, against the measured 4.3k of an
ingestion (`epistemic_limits.hpp:36` comment); 2a measures both and its gate allows 5%. Slots and
scratch bytes per ingestion are measured before and after (m6). Subnormal `R` is legal: it decays to
0 after about 34,000 further events and a test round-trips subnormals through scalar encoding and a
checkpoint.

Tests (new file `tests/xuzz/vagent_epistemic_health_test.cpp` unless named):

| Test                                                                                                                                                    | Finding       |
| ------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------- |
| closed form for `k` refusals, `m` accepts and a mixed batch; bounds `0 <= R <= N <= 1/(1-b)` after 10^6 updates; clamp after `H` shrinks                | F2, m10       |
| SUSPECT at the `min_events` edge (not before the ninth reading); one early refusal then clean readings never reads SUSPECT; `validate()` caps           | F3            |
| relay share 0, 50, 100% at a 20% defect rate gives the same mean `f`; a relayed unsigned-basis refusal moves nothing; a resend counts once              | F1, m2        |
| stalled pump, 1000 offered, 5% bad: NOMINAL with the credit; a credited event that later commits is not counted twice                                   | F4            |
| epoch regression is a health refusal; mapping regression, stale, time mismatch, `UncertainAcquisition` move nothing; NaN tuple; RTC-reset; counter-loss | F5, m1        |
| the counted-code table equals the ABI list; a charge carries `health` and the note records it                                                           | F6            |
| the F8 probe sequence                                                                                                                                   | F8            |
| order: refusal then queued accept then refusal; arena-decided refusal behind 8 events; weight error within `b^-8`                                       | F2            |
| the stamp holds state and `f`; resume keeps `R`, `N`; tombstone and a restored pre-2a profile have a state; synthetic reasons reach DEGRADED/SUSPECT    | m11, m33, m35 |
| `HealthEstimatorSimulation`: one seeded run per rate, fraction of steps SUSPECT after 3H against the table; 30% reads SUSPECT, 5% NOMINAL               | m7, m8        |
| subnormal round trip through scalar encoding and checkpoint; slots and scratch per ingestion                                                            | m6            |
| `sourceHealth` equals the IR stamp across every state; unknown actor, in-turn, no profile                                                               | F18           |
| `vagent_settings_test.cpp`: each field, invalid value falls back for the whole block with a WARN naming it                                              | F30           |
| existing tests that expect `ObservationTimeMismatch` for an epoch or mapping regress (`vagent_epistemic_ingress_test.cpp:989`, `995`) change code       | F5            |

## 4. Slice 2b: settle after commit

### 4.1 The contract

Today a source change drops supports through `rtBDrop`, which queues `-pred` events on
`$agent d.pending`, and `rtSettle_` runs their handlers inside the same all-or-nothing turn. A
handler that always fails therefore makes the advance, the retire and (from 2c2) the reset and
isolate refuse for ever: the repair path fails exactly when a source sends garbage.

From 2b `sourceChangeEntry` (and 2c2's `healthEntry`) end without settling, so the turn commits its
state change with the belief events still queued. The executor then settles them **one top-level
event per turn**, inline in the same wake and before any other item or turn for that actor:

- **`settleEntry`** (new `CompiledAgent` entry, present exactly when `observeEntry` is). Emitted IR:
  detach `d.pending`, keep its head event `h` and the remainder `r`, put `h` alone on `d.pending`,
  call the existing `rtSettle_` (`compiler.cpp:5684`, unchanged: it settles `h` and everything `h`
  raises, bounded by `vagent.belief_event_depth` rounds), re-attach `r` and end `Committed`.
  Termination: each iteration of the executor loop removes one top-level event, settled or
  discarded, and only a settle turn adds events (consumed by that turn or undone with it), so the
  loop runs at most as many times as the change queued events.
- **Discard is executor-side C++**, not an entry (m14, F10). After a settle turn fails and is
  undone, the executor unlinks the head event (`ArenaManifold::unlink` and `link`,
  `zigzag/arena_manifold.hpp:635-648`: agent to next event) and counts it. It is C++ because
  recovery from a failed VM turn must not depend on the VM budget that just ran out; it mints no
  cell and cannot fail (a refused `link` on a ref read from the same arena falls back to unlinking
  the whole queue and a WARN).
- **Gates.** Settle turns carry no `admit` hook, so the cell ceiling does not stop them (they mint
  what the program's handlers mint, which is the agent's own budget). The slot gate
  (`agent_executor.cpp:1873`) becomes `cellCount() >= arenaSlots` for ordinary entries and
  `>= arenaSlots + slotReserve` for settle and (2c) health entries, with
  `slotReserve = arenaSlots / 64` (16,777,216 slots at the default; `validate()` requires at least
  4,096). A settle the gate refuses is treated as a failed settle and discarded.

**Outcome (F9, F10).** The change turn's outcome is published once, after its settle loop, so a
caller never sees a committed change with a stale report. It stays `Committed` and raises the replay
floor (`finishSourceChange`, `agent_executor.cpp:1936`); a failed event does not turn it into a
refusal, because the change did commit and a retry would be destructive (F17):

```cpp
struct SettleReport {
  std::uint32_t settled{}, discarded{};
  cpp26::inplace_vector<std::string, 4> names; // distinct event types discarded, each bounded
};
// IngestOutcome gains: std::optional<SettleReport> settle;
```

`stat()` counts `settle.discarded`. `SettleFailed` is a stat/log label (`refused.SettleFailed`), not
an outcome code, and joins the `chargeable` exclusions so no future path can charge it (F7).
`outcomeClassOf` needs no entry for it. Each settle turn increments `$turn_count`, so a change costs
one more turn per event; request IDs and `ready` stamps need only monotone counts (m15).

**What a program sees.** Its `-pred` handlers run after the belief change, as before, in their own
turn each. A handler that fails no longer undoes the change it reacts to, and no longer discards its
siblings' events (revision 2's "a correct handler sees every event once" was false for a batch). Two
behaviour changes, both recorded in section 11: a failing event is lost for that handler (the
program sees it in `stat()` and the outcome); and a cascade runs depth-first inside its parent's
turn, so the order of top-level events is kept but a cascade event now precedes the next top-level
one (before, rounds ran breadth-first).

**Ingestion keeps the wedge (F11, accepted residual).** `rtObserve` still ends in `rtSettle_`
(`compiler.cpp:5668`): a reading that replaces a key and raises `-old` for a handler that always
fails is refused `IngestionFault` for ever, and the key cannot update. This is the I3-1 contract
(all-or-nothing ingestion) and stays; an operator isolate or reset unwedges the source, and the
Schema notes and the program documentation say so (A4, section 11).

**Checkpoint.** `checkpoint` (`agent_executor.cpp:4932`) refuses a linked `d.pending` with the new
`CheckpointRefusal::SettlePending`; after 2b nothing can leave it linked between wakes except a
crash inside the loop, which a checkpoint must not preserve.

### 4.2 The drop body (m33, m37)

The advance's two walks and trim in `emitSourceChange` (`compiler.cpp:5001-5091`) become one emitted
routine, `rtSourceDrop_(profile, incBelow, retained)`: retire each record below `incBelow`, move its
support to the profile's retired chain, drop emptied supports through `rtBDrop_`, trim to
`retained`. The advance calls it with the new incarnation; 2c2's isolate and reset call it with the
largest Int64. Retire keeps its own tail (it excises records and unlinks clone occurrences, a
different walk) and `freshWindows` stays in the advance's caller. The refactor lands in 2b because
2b already rewrites the tails of both entries; every I3-1c advance and retire test and
`vagent_epistemic_resume_test.cpp` gate it.

### 4.3 Slice 2b: files, price and tests

Files: `compiler.cpp` (`emitSourceChange` tails, `rtSourceDrop_`), `compiler_health.cpp`
(`settleEntry`), `runtime_abi.hpp` (`CompiledAgent::settleEntry`, `kStatSettleDiscarded`),
`program.cpp:132-141` (remap) and `agent_executor.cpp:1276` (add to the `sound` list, m34),
`agent_executor.cpp/.hpp` (`deliverHost`, `finishSourceChange`, `checkpoint`),
`epistemic_ingress.hpp` (`SettleReport`), `epistemic_limits.hpp` (reserve and its validation). C++
is the scheduling of the settle loop, the discard and the gates; the settle itself is IR.

Price at the defaults: no arena cells. A change that drops `n` supports costs `1 + n` turns instead
of one; `n <= keys_per_source x predicates = 4 P`, so a source bound on three predicates costs at
most 13 turns, below 56k cycles at the 4.3k an ingestion turn costs (an upper bound: settle turns
skip the observation walks). The turn setup cost is measured in 2b and the gate is that a 16-event
retire completes in one wake without delaying the next ingestion by more than the same 13 turns.

| Test                                                                                                                                                | Finding  |
| --------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| an always-failing `-pred` handler: advance and retire commit, `settle.discarded == 1` with the name, `stat()` counts it, the next ingestion commits | F9, F10  |
| two predicates, one handler failing: the other sees its event exactly once, after the change                                                        | F9       |
| a batch too big for the instruction grant as one turn, each event small enough alone: all settle                                                    | F9       |
| top-level order kept; a cascade runs inside its parent's turn; a runaway cascade fails its parent's turn only                                       | 4.1      |
| the settle loop runs before a queued observation in the same wake and before a mailbox turn                                                         | F10      |
| the discard succeeds with an instruction grant of 1 and with the slot gate closed; the queue is then empty and consistent                           | F10, m14 |
| one publication: `sourceChangeOutcome` never shows a committed change without its report; a discarded event still raises the replay floor           | F10      |
| `checkpoint` refuses `SettlePending` with a linked `d.pending`, and succeeds once settled; resume keeps no pending events                           | F10      |
| every I3-1c advance and retire test, `vagent_epistemic_resume_test.cpp` and the I3-1 ingestion wedge test unchanged                                 | m37, F11 |
| `SettleFailed` is never charged and posts no note                                                                                                   | F7       |

## 5. Slice 2c1: grants, tokens and the host health API

### 5.1 The trust boundary (F12)

`grantHealth`, `revokeHealthGrant`, `healthAction`, `healthActionBatch` and `healthOutcome` are
host-only methods of `AgentExecutor`, on the same footing as `bindSource` and `checkpoint`. They are
unreachable from an envelope, a percept, `deliver`, a native skill (`HostBindings`) and an upgrade,
and a program cannot name them. **The executor trusts its embedder:** whoever holds the object is
the operator, and `issuer` is audit data the embedder asserts, not an authenticated identity. The
embedding host authenticates its operators; I3-3's adapter and I4's directive path add
authentication where operator commands arrive over a network. The issuer is a `HealthIssuer`, a type
constructible only from a validated 64-hex lowercase principal token (`fromPrincipal`), so a free
string cannot reach a record.

Tests: a bound sensor, a relay and the owner without a token are each refused
`UnauthorizedHealthAction` (P19 section 4); a source test asserts none of the five names appears in
`host_bindings.{hpp,cpp}` and that `HostBindings` holds no `AgentExecutor` handle.

### 5.2 Grants and tokens (F13-F15)

```cpp
enum class HealthActionKind : std::uint8_t { Isolate, Degrade, Clear, Reset };

struct EpistemicHealthGrant {
  std::string id;            // audit label: [A-Za-z0-9_.:-], 1..kMaxObservationNameBytes
  HealthIssuer issuer;
  std::vector<std::string> sources;                    // 1..max_sources distinct principals
  cpp26::inplace_vector<HealthActionKind, 4> actions;  // 1..4, no repeats
  std::chrono::steady_clock::duration lifetime;        // (0, kMaxHealthGrantLifetime]
};

class HealthGrantToken;  // 16 bytes, private; no default constructor; no formatter or stream
[[nodiscard]] std::expected<HealthGrantToken, EpistemicRefusal>
grantHealth(const identity::ActorPid &target, const EpistemicHealthGrant &grant);
[[nodiscard]] std::expected<std::size_t, EpistemicRefusal>
revokeHealthGrant(const identity::ActorPid &target, std::string_view id);
```

- **Token.** 16 bytes from OpenSSL `RAND_bytes`; if it fails, `grantHealth` fails closed with
  `EntropyUnavailable` (no fallback, no seed parameter). The executor stores only the SHA-256 digest
  and compares digests with `CRYPTO_memcmp` over the actor's at most 16 grants, so a lookup leaks
  nothing about the token. The token never enters the arena, a log line, a checkpoint or an outcome;
  the formatter is deleted. `kMaxHealthGrantLifetime` is 24 hours, a working session: a longer
  authority is re-issued, and a grant lifetime is steady-clock time, which does not advance while
  the host sleeps (m24; the `AgentClock` seam makes `CLOCK_BOOTTIME` a one-line change).
- **ID.** The id is an audit label and authorizes nothing. Records carry `"<id>@<instance8>"`, the
  first eight hex digits of the executor's `clockInstance` (`agent_executor.cpp:761`), so an id
  reused after a restart is distinguishable in old `actions` records (m28).
- **`grantHealth` refusals** (`InvalidHealthGrant` unless named): empty, oversized, repeated or
  non-principal `sources`; empty or repeated `actions`; a duplicate id on the actor; zero or
  oversized `lifetime`; a bad id; more than `kMaxHealthGrants` (16) live grants on the actor after
  expired and revoked ones are purged (m29); `UnknownActor`; `SelfCertification`;
  `EntropyUnavailable`. There is no wildcard source.
- **Self-certification (F14, decision).** A health grant's issuer may never be a principal that
  sources readings. Enforced at three places, host-wide (any principal bound on any actor of the
  executor, tombstones included), on canonical tokens (uppercase hex is not a token, so no case
  aliasing): `grantHealth` refuses an issuer that is in `sources`, in `Ingress::everBound` (every
  principal bound on any actor since the executor started) or has a profile on the target actor,
  tombstone or not; `bindSource`, including the revive path, refuses a principal that is the issuer
  of any live grant, with `SelfCertification` before it changes anything; and the turn-time check of
  5.3 repeats both against the target's profiles. A device key delegated by a master identity
  (`DeviceDelegation`) is a different token and is a **recorded limit**, as is any issuer who also
  runs a bound sensor through a different key.
- **Revocation (F15).** `revokeHealthGrant` returns the number of queued requests it cancelled, or
  `UnknownGrant`. Linearisation point: a request is authorized iff its token is live at the check in
  `deliverHost`; a turn already past the check completes, and a revoked-then-regranted id under a
  new token authorizes nothing queued under the old one.

### 5.3 Requests, receipts and outcomes (F16, F17, F24)

```cpp
struct Isolate { std::string code; };
struct Degrade { std::string code; };
struct Clear   { std::optional<std::string> code; };  // one operator code, or every operator reason
struct Reset   { std::uint64_t expectResetEpoch; };   // the epoch the caller read; required
using HealthAction = std::variant<Isolate, Degrade, Clear, Reset>;

struct HealthActionRequest { HealthGrantToken token; std::string principal; HealthAction action; };
struct HealthBatchRequest  { HealthGrantToken token; std::vector<std::string> principals;
                             Isolate action; };               // 1..max_sources, applied in order
struct HealthReceipt { std::string instance; std::uint64_t counter; };

struct HealthItemOutcome { std::string principal; IngestOutcome outcome; };
struct HealthOutcomeView {
  enum class State : std::uint8_t { Pending, Known, Unknown } state;
  std::vector<HealthItemOutcome> items;
  bool checkpointed{false};  // a checkpoint of the actor has completed since the commit
};
[[nodiscard]] std::expected<HealthReceipt, EpistemicRefusal>
healthAction(const identity::ActorPid &target, const HealthActionRequest &request);
[[nodiscard]] std::expected<HealthReceipt, EpistemicRefusal>
healthActionBatch(const identity::ActorPid &target, const HealthBatchRequest &request);
[[nodiscard]] HealthOutcomeView
healthOutcome(const identity::ActorPid &target, const HealthReceipt &receipt) const;
```

**Synchronous refusals** (`EpistemicRefusal`, nothing queued): `UnauthorizedHealthAction` (unknown,
expired or revoked token; a principal the grant does not name; a kind it does not allow);
`InvalidHealthCode` (a code outside `[a-z0-9_.-]{1,64}`, or one of the reserved derived names
`refusal_fraction`, `rejection_streak`, `fault`; `Clear.code` and `Isolate.code` use the same rule,
m30); `InvalidPrincipal` (not a lowercase 64-hex token); `InvalidHealthRequest` (empty or oversized
batch, a missing `Reset` epoch); `UnknownActor`; `IngressBackpressure` (the bounds below).

**Bounds (F16).** Two settings: `vagent.epistemic_health_pending` (default 16, the actor total) and
`vagent.epistemic_health_pending_per_grant` (default 2). A batch is one pending item and one slot
against both. `Isolate` and `Reset` have a reserved lane: `Degrade` and `Clear` may hold at most
`pending - 4` of the total, so a degrade-only holder cannot keep `Isolate` from queueing. A third
setting, `vagent.epistemic_control_per_wake` (default 16), caps the health turns (a batch counts one
per principal) one wake runs; the rest wait at the front and keep their order, and each started
change finishes its settle loop (4.1). A batch of 100 isolates takes seven wakes at the default, and
an ingestion wake runs between them.

**Receipts (F17).** The ticket is (executor instance, counter), so tickets from two hosts or a
restart never collide. `healthOutcome` returns `Pending` for a queued item, `Known` with the
outcomes for a finished one, and `Unknown` for another instance or one the log evicted. Pending
items live in their own map and are never evicted; finished ones go to a per-actor ring of
`outcome_log` (64) entries, never into the source-change map, whose entries drive binding
revocation. `checkpointed` answers F24's question: the executor counts successful checkpoints per
actor and each outcome records the count at its commit.

**Turn-time recheck (F15).** When a queued request becomes a turn, `deliverHost` looks the grant up
by the token digest under the ingress mutex and rechecks live and unexpired, principal, kind and the
F14 conditions. A failure finishes the item `Refused` with `UnauthorizedHealthAction`
(`NeedsHostAction`), as a queued observation whose binding lapsed finishes `UnknownSource`. The
check runs for every health `HostEvent`, so an item moved from the post box to `hostEvents` is
rechecked too.

### 5.4 Order and host state (F19, F25, F32)

Health actions are control items: ahead of queued observations, behind control items already
waiting, in call order. Two named predicates replace `sourceChange.has_value()`, which only two
sites tested (the other two sites keep and account; revision 2 misdescribed them): `isControl(item)`
(source change or health) and `owesAccounting(item)` (ingest, note, source change or health), on
`PostBox::Item` and, mirrored, on `HostEvent`. T0 already wakes the reactor from every enqueue
(F32). Every site that must change, with a test each:

| Site (HEAD)                                            | Change                                                                         |
| ------------------------------------------------------ | ------------------------------------------------------------------------------ |
| `PostBox::pushControl` (`:642`)                        | order by `isControl`                                                           |
| `PostBox::putBack` (`:650`)                            | stable-partition kept items so control items stay ahead of observations        |
| `drainPosted` insertion (`:3911`)                      | order `hostEvents` by `isControl`                                              |
| `drainPosted` `epistemic` lambda (`:3859`)             | keep a health item while the actor is in a turn                                |
| `drainPosted` `abandon` (`:3844`)                      | a health item finishes `Undelivered` and resolves its pending entry            |
| `drainPosted` `else` branch to `finishEffect`          | add a health branch before it; a health item never reaches `finishEffect`      |
| `deliverHost` exhausted path (`:1873`)                 | finish a health item with `ArenaExhausted` past the reserve                    |
| `dropHostEvents` (`:1967`) and `orderlyStop` (`:2013`) | account every owed item; `orderlyStop` calls `dropHostEvents` before `clear()` |
| `deliverHost` `admit` (`:1865`)                        | `Degrade` is admitted against the ceiling, the other kinds are exempt (2c2)    |
| `outcomeClassOf` (`:3399`)                             | explicit entries (section 7); the default no longer hides new codes            |

**Reset generation (F19).** The revision 2 epoch was bumped at call time, so a reset that was
revoked or failed had already refused queued readings. Now each (actor, principal) has
`ResetMark {requested, committed}` in `Ingress`, erased in `forget(pid)` and not in `unbind`, so a
rebind keeps it (m16). `healthAction(Reset)` increments `requested` and snapshots the principal's
highest admitted sequence per predicate (`Ingress::admittedSeq`, raised in `observe` for every
reading that passes its checks) into the queued item, in one critical section with the push. Every
ingest item, note and `Charge` carries the `requested` it was created under, and `charges` is keyed
by (actor, principal, generation), so a refusal after the reset call opens a fresh charge and note.
A reset that **commits** (2c2) sets `committed = requested-at-call`; an item is superseded iff its
generation is below `committed`, and the check runs in `drainPosted` and again in `deliverHost` for
items already turned into events, finishing them `SupersededByReset` (`Terminal`: the reading
predates the reset and must never count as the new eligible observation P19 5.1 requires). A reset
that fails, is revoked or is a no-op supersedes nothing.

### 5.5 The stub turn

2c1 emits `healthEntry` as a routine that ends `HealthUnavailable` (`NeedsHostAction`) for every
kind, remapped in `program.cpp` and in the `sound` list. Everything on the host path therefore runs
end to end: validation, grants, ordering, rechecks, receipts, bounds, per-wake caps and the audit
sink (invoked with the `Refused` outcome). No state changes, so the slice is safe alone, and 2c2
replaces only the routine.

### 5.6 Slice 2c1: files, price and tests

Files: `health_grants.{hpp,cpp}` (token, grant, request, receipt, store), `agent_executor.cpp/.hpp`
(methods, `Ingress` grants, `everBound`, `admittedSeq`, `ResetMark`, the sites above, the audit sink
type and `setHealthAuditSink`, chaining), `epistemic_limits.hpp` (`healthPending`,
`healthPendingPerGrant`, `controlPerWake` with `validate()`: all at least 1, per-grant at most the
total, total above the reserved lane), `compiler_health.cpp` (the stub), `runtime_abi.hpp`,
`program.cpp`, `system_docs.*`. C++ is justified entirely: authorization, clocks, entropy, transport
and scheduling are the permitted categories; the arena sees none of it.

Price: no arena cells. Host memory: at most 16 grants of about 200 bytes and 16 pending items per
actor, plus 64 outcomes, all bounded by settings. Cycles: a refused request costs one mutex hold and
at most 16 `CRYPTO_memcmp` calls.

| Test                                                                                                                                               | Finding  |
| -------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| each synchronous refusal; a bound sensor, a relay and the owner without a token                                                                    | F12      |
| no token in `SPDLOG_LEVEL=trace` output, a checkpoint or any outcome; 10^5 tokens without collision; `RAND_bytes` failure fails closed; zero token | F13      |
| issuer bound after the grant, bound before it, in a different actor of the executor, a tombstone, a non-canonical string, a bind of an issuer      | F14      |
| revoke then regrant under one id: the queued item refuses; `revokeHealthGrant` counts cancelled items and reports `UnknownGrant`                   | F15      |
| expiry between queue and turn (injected `AgentClock::steady`); a revoked token for an item already in `hostEvents`                                 | F15      |
| 16 degrades queued then an isolate by another grant runs first; per-grant bound; a 100-source batch over several wakes with an ingestion between   | F16      |
| ticket collision across two executors; pending never evicted; `Unknown` after eviction; `checkpointed` flips after a checkpoint                    | F17, F24 |
| a failed, revoked or stubbed reset supersedes no queued reading and no charge; a charge opened after the call has its own note                     | F19      |
| order with a queued rebind; one test per site of 5.4; `orderlyStop` with queued health items accounts them                                         | F25      |
| a control post wakes the reactor (T0's test, kept)                                                                                                 | F32      |
| `ResetMark` and `admittedSeq` survive unbind and rebind, and are erased with the actor                                                             | m16      |
| the audit sink receives a turn-time refusal; a throwing sink is logged and ignored                                                                 | F28      |

## 6. Slice 2c2: the Vortex health turn

### 6.1 Profile health structures

All owned by the profile, on the I3-1 shapes (records on `d.vars`/`d.values`, lists on
`d.grab`/`d.step`), created on first use, absent reading as empty or 0, copied with the profile on
resume and kept by an advance, a retire and a revive (a replacement device inherits its
predecessor's fault and reasons until the host's reset says otherwise, P19 2.1).

| Structure      | Fields, in order                                                                                        | Cells (worst) | Role                                          |
| -------------- | ------------------------------------------------------------------------------------------------------- | ------------- | --------------------------------------------- |
| scalars        | `reset_epoch`, `action_seq`, `fault_seq`, `health_cells` (Int)                                          | 8             | epoch (F21); every accepted action; the fault |
| `reasons` list | per entry `kind`, `code`, `origin`, `grant`, `issuer`, `incarnation` \[, `wall`\]                       | 14 (16)       | live reasons, unique by (kind, code, origin)  |
| `actions` list | per entry `seq`, `kind`, `code`, `grant`, `issuer`, `incarnation`, `count` \[, `wall`\] \[, `removed`\] | 16 (20)       | audit window                                  |
| list headers   | one cell and one field each                                                                             | 6             |                                               |

Constants `kHealthFixedCells = 14` (scalars plus headers), `kReasonCells = 16`, `kActionCells = 20`
are ABI and a test counts a full structure against them. The **bound** of a profile is
`healthBound = 14 + 16 R + 20 A`, 142 cells at the defaults `R = 3` reasons and `A = 4` actions
(`vagent.epistemic_health_reasons` in [2, 16], `vagent.epistemic_health_actions` in [2, 64]);
`health_cells` holds the cells in use, maintained by the health turn.

- **`fault` is `fault_seq`**, the `seq` of the `isolate` entry that latched the profile; 0 when not
  latched. That entry cannot be evicted while it is the live fault. Provenance lives once, in the
  action list. A reason carries its own provenance because it is the live state.
- **`reasons`** has `kind` `degraded` or `suspect` and `origin` `operator` (now) or `device` (I3-3).
  Operator reasons may fill at most `R - 1` slots; the last is reserved for evidence-driven reasons
  (F29). A new operator reason past that refuses `HealthReasonsFull` (`NeedsHostAction`: clear one).
- **`wall`** is present only when the executor's wall clock is synchronised
  (`Ingress::wallSynchronised`); an unsynchronised stamp is absent, never 0.
- **`removed`** on a `clear` or `reset` entry is one bounded text (at most 1,024 bytes) the host
  composes from the pre-turn snapshot: the cleared codes and, for a reset, the removed fault's code,
  issuer, grant, incarnation and time (F28). One text cell keeps the audit window small.

### 6.2 The turn

`healthEntry` replaces the 2c1 stub. It reads `$turn.health`: `source`, `kind`, `code`, `grant`,
`issuer`, `wall` (optional), `incarnation` (the bound incarnation, optional), `retained`,
`max_sources`, `profiles_cap` (twice `max_sources`), `reasons`, `actions`, and for a reset
`expect_reset_epoch`, `floors` (a record predicate to Int), `removed`, `dropped_content` and
`dropped_other`. It finds the principal's profile; a tombstone is acted on like any profile. With no
profile: `Isolate` and `Degrade` for a bound principal create one through the shared profile routine
(a bound source can be isolated before its first reading), `Isolate` exempt from `max_sources` but
not from `profiles_cap` (past it, `EpistemicBudget`); every other kind, and any kind for an unbound
principal, ends `NoProfile` (`NeedsHostAction`: bind it first).

| Kind      | Effect                                                                                                                                                                                                                                                                                    |
| --------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Isolate` | `NoChange` if latched. Else append an `isolate` entry, set `faulted` and `fault_seq`, drop every support through `rtSourceDrop_(profile, max Int64)`; no refutation, label or calibration move (m20). Later readings refuse `SourceFaulted`                                               |
| `Degrade` | `NoChange` if an operator `degraded` reason with that code exists. Else add the reason and an entry                                                                                                                                                                                       |
| `Clear`   | With a code: remove that operator `degraded` reason; absent is `NoChange`; present with another origin refuses `NotOperatorReason` (Terminal). Without: remove every operator `degraded` reason. Never touches the latch, `suspect` or `device` reasons, readings or calibration (A1, A9) |
| `Reset`   | `StaleHealthState` if `expect_reset_epoch != reset_epoch`, committing nothing. Else 6.4                                                                                                                                                                                                   |

Every action that changes state appends or merges an action entry (6.5) and refreshes
`health_cells`; a degrade is admitted against the ceiling like an ingestion, the other kinds are
exempt (6.7).

### 6.3 Idempotency (F17)

Before a turn the executor derives the principal's `SourceHealth` snapshot (3.5) and a pure function
`wouldChange(action, snapshot)` decides: a request that would change nothing finishes `NoChange` (a
new `IngestOutcome::Kind`) **without a turn, a slot or an action entry**. The IR repeats each test,
so a divergence between the two costs a wasted turn, never a wrong state. A retried `Reset` cannot
destroy new evidence: the second carries the old `expect_reset_epoch` and refuses
`StaleHealthState`, durably and across a resume, because the epoch is arena state. A reset of a
profile that is clean, has no current record and matches the epoch is `NoChange`. A discarded settle
event is a warning on a committed change, not a refusal (4.1), so nothing invites the caller to
retry it.

### 6.4 Reset in full (F20, F21, m17-m19)

A committed reset, in one turn: clears the latch and `fault_seq`; empties `reasons`; zeroes `R`, `N`
and `rejection_streak`; increments `reset_epoch`; clears `last_receive` and `last_contact` (m19) and
says so in the entry; adds `dropped_content` and `dropped_other`, the counts of the closed charges
(5.4) that the commit discards, to the lifetime counters (m17); drops every support through
`rtSourceDrop_`; and **raises each stream's sequence floor** to `floors[predicate]` where that is
higher, creating the stream when the predicate has none. The floors are the highest sequences the
host admitted before the call (F20, decision A), so a resend of a superseded reading refuses
`StaleSequence` on this host after a restart and on any other host the actor is resumed on. The
dedup window and the streams otherwise stay (P19 2.1), as do `refused_count`, `content_refusals` and
`last_refusal`. On commit the executor sets `ResetMark.committed`, erases the closed charges and
resets the principal's `refusedWhileFaulted`.

**Markers (F21).** `reset_epoch` is written into each new record's `calibration_epoch` (today the
constant 0, `agent_executor.cpp:3725`) by the ingestion routine, so a pre-reset label keys
differently from the reset learner's (P19 2.1; I4 depends on it). `rtSourceDrop_` gains a `cause`
and writes `retired_by` on each record it retires, one of `superseded`, `advance`, `retire`,
`isolate`, `reset` (2 cells per retired record, at most 16 a profile). Both are pinned by the ABI
test. A reset invalidates by receipt time, so a reading acquired before and delivered after counts
as new; I3-3 and I4 compare mapped acquisition with a reset time (m18, section 13).

### 6.5 Isolate and the audit window (F7, F28, F29, m20, m21, m26, m27)

**While latched**, the ingestion routine's first step after finding the profile refuses
`SourceFaulted` (`NeedsHostAction`) before it reads or writes anything else. `chargeable` excludes
`SourceFaulted`, `SupersededByReset` and `SettleFailed`, so such a refusal posts no note, rewrites
no `last_refusal`, and a relayed copy of an isolated origin charges neither origin nor relay (F7).
There is no host latch mirror (F23, decision A): the arena refusal allocates no cell, and the
per-source queue share bounds a flood; each flooded reading still costs a turn (about 4.3k cycles),
and a flooder keeps the queue non-empty, so to reach the quiescence a checkpoint or upgrade needs
the host retires or unbinds it (m23). `finishIngest` counts these in `Ingress::refusedWhileFaulted`,
which `SourceHealth` shows; it is host-only and erased in `forget`. A sensor learns it is isolated
from the refusal; that is accepted, and the refusal is neither charged nor logged per event (m27). A
latched tombstone holds its profile (about 44 plus 142 cells) for the life of the actor; the
operator retires the old principal, since a latch protects an identity and a new key is a new
principal (m21).

**Audit window.** Entries are `isolate`, `degrade`, `clear` or `reset`, with the executor's grant
label, issuer, bound incarnation, wall time and `seq` from `action_seq`. Eviction is by class when
the list is at its bound `A`:

- safety entries (`isolate`, `reset`) evict the oldest ordinary entry if any, else the oldest safety
  entry that is not the live fault;
- an ordinary entry (`degrade`, `clear`) that equals a stored ordinary entry in (kind, code, issuer)
  increments its `count`; else it evicts the oldest ordinary entry, and if none is stored the entry
  is not stored at all (the action still took effect).

The lowest privilege therefore cannot evict an isolate or reset entry. `actions_evicted` is
`action_seq` minus the entries stored, derived, never written. The arena window is recent history;
the durable record is the **audit sink**, a host callback
`setHealthAuditSink(std::function<void(const HealthAudit &)>)` called on the pump thread after a
health turn finishes and before its outcome is visible, with the entry's fields, the steady time,
the wall time when synchronised, the outcome and the removed text. It is called for committed
actions and for turn-time refusals, not for `NoChange` or synchronous refusals (the caller has
those); a throwing sink is caught and logged. Durability is the sink's job; there is no host log
inside the executor, and revision 2's sentence about one is gone.

### 6.6 Budgets and reserves (F26, F27)

**The cell ceiling (decision F27(A), applied per profile).** The decision charges a health reserve
of `max_sources x healthBound` against the ceiling. At the defaults that is 128 x 142 = 18,176 of
24,576 cells, leaving about 6,400 for ingestion (fewer than 80 records), and 18 fully active
principals fit today. This design keeps the invariant the decision wants, that a profile can always
reach its health bound, but charges the reserve **per existing profile**, tombstones included, so
the cost follows use: 16 fully active principals at about 1,520 cells each (16 records of about 82
cells, 24 cells of `retired_by`, the profile's 44, 142 reserved) fit where 18 did. Flagged for the
lead in the handoff.

- Ordinary admission (ingestion, bookkeeping, `Degrade`):
  `cellsWithin(all) + sum over profiles of (healthBound - health_cells) <= cells`.
  `epistemicCellsWithin` (`agent_executor.cpp:3305`) already walks the profiles, so it reads
  `health_cells` there.
- Exempt entries (`Isolate`, `Clear`, `Reset`, settle, profile creation for an isolate):
  `cellsWithin(all) <= cells + cells / 16` (the headroom, 1,536 at the default) and profiles (live
  plus tombstones) at most `2 x max_sources` for creation. A degrade-only grant therefore cannot
  starve ingestion, and a full budget with every source isolated still checkpoints because
  `validate()` requires `cells + cells / 16 <= kMaxEpistemicCheckpointCells` and
  `max_sources x (healthBound + kProfileBaseCells) <= cells` (`kProfileBaseCells` is measured in 2a;
  expected 44; 128 x 186 = 23,808 fits). `kMaxEpistemicCheckpointCells` starts at 28,672 and the 2c2
  test, which fills to the hard ceiling with every source isolated and checkpoints, fixes it; if
  that test fails, the constant and the headroom shrink.
- **Slots (F26).** Exempt entries run until `cellCount() >= arenaSlots + slotReserve` (4.1). The 2c2
  test measures the slots one maximal reset (16 records) uses and asserts the reserve covers
  `healthPending` of them. No-op requests allocate nothing (6.3).
- **Runbook when slots run out.** Ordinary entries refuse `ArenaExhausted`; isolate and reset still
  run to the reserve. Isolate the flooding source (it stops growth but frees nothing, F26), then
  `checkpoint`, `resume` (the copy reclaims unreachable slots), re-bind each source at its current
  incarnation and re-grant: bindings, grants and tokens are host state.

### 6.7 Durability, the reflash constraint and the read API (F22, F24)

| Survives                                               | Does not                                                       |
| ------------------------------------------------------ | -------------------------------------------------------------- |
| a device reboot, advance, revive and retire (arena)    | a host crash, unless a checkpoint followed the commit          |
| a checkpoint and resume, on this host or another       | grants, tokens, bindings, `ResetMark`, `admittedSeq`, outcomes |
| a live upgrade once the translator exists (section 13) | `refusedWhileFaulted` and the audit sink's own storage         |

A host that keeps its own durable isolate list can replay it after a resume: a repeated isolate is
`NoChange`. A source needs a **durable or fresh incarnation**: a persistent boot counter, a trusted
clock or a host handshake (A3). A device with none cannot be an arbitrated source, and the revision
2 runbook "reset, then rebind at a higher incarnation" is deleted because such a device cannot sign
a higher one and a reset keeps the stream floors. `SourceHealth` gains: `reasons`, `actions`,
`faultSeq`, `resetEpoch`, `actionSeq`, `actionsEvicted` and `refusedWhileFaulted`.

### 6.8 Slice 2c2: files, split, price and tests

Files: `compiler.cpp`/`compiler_health.cpp` (`healthEntry`, `rtSourceDrop_` cause, `SourceFaulted`
check, reset-epoch stamp, the stream-floor write), `runtime_abi.hpp`, `agent_executor.cpp/.hpp`
(`deliverHost` health branch and gates, `wouldChange`, composition of `removed`, `finishHealth`,
admission arithmetic, the sink's call for committed actions), `epistemic_limits.hpp`
(`healthReasons`, `healthActions`, reserve and headroom validation), `system_docs.*`,
`tests/xuzz/epistemic_invariants.hpp` and `tests/fuzz/fuzz_epistemic_ingress.cpp`. The state change,
all four kinds, the drop, the floors and the admission test inside the turn are IR; C++ is the
pre-check, the snapshot and `removed` composition (host formatting of data it already read), the
gates (host admission, as `epistemicCellsWithin` is today) and the sink.

Price at the defaults: per profile at most 142 reserved cells (3 reasons, 4 actions) and 2 per
retired record (`retired_by`, at most 32); a fully active principal about 1,520 cells. Cycles,
estimated: a health turn about 300 plus about 150 per record dropped; a reset of 16 records is about
2.7k cycles plus 16 settle turns (4.1). It is **measured against `vagent.max_instructions` before
2c2 lands**, and the gate is that a 16-record reset fits the default grant with half to spare.

| Test                                                                                                                                                     | Finding  |
| -------------------------------------------------------------------------------------------------------------------------------------------------------- | -------- |
| isolate before the first reading sticks, drops supports, refuses later readings in the arena (also after resume) and fits a full arena                   | 6.2      |
| a latched source: a reading changes no counter or `last_refusal`, posts no note, charges neither origin nor a relay                                      | F7       |
| reboot (advance), retire and revive, checkpoint and resume, keep latch, reasons, actions and `reset_epoch`                                               | F24      |
| degrade is unique by code; clear removes only operator `degraded` reasons and never unlatches; a `device` reason survives `Clear {}` and refuses by code | F29      |
| reset clears latch, reasons, `R`, `N`, streak, liveness stamps; keeps dedup, streams and diagnostics; a reading after it is weighted from the prior      | 6.4      |
| a retried reset refuses `StaleHealthState`; no-op actions return `NoChange` with no turn, slot or entry; `wouldChange` equals the IR on every state      | F17      |
| queue a reading, reset, resend it: superseded, then `StaleSequence`; also after a resume on a second executor                                            | F19, F20 |
| a reading queued after the reset call is not superseded; a coalesced charge is split at the call and the closed part is dropped at commit                | F19, m17 |
| `calibration_epoch` equals `reset_epoch` on new records; `retired_by` set by advance, retire, isolate, reset; both pinned by the ABI test                | F21      |
| 8 repeated degrades cannot evict an isolate or reset entry; `count` merges repeats; the reset entry holds the removed fault; `actions_evicted`           | F28      |
| audit sink: committed, turn-time refusal; reset with a failing `-pred` handler commits with `settle.discarded`                                           | F28, F9  |
| ordinary admission with every profile at its bound; a degrade-only grant cannot starve ingestion; exempt creation stops at `2 x max_sources`             | F27      |
| a full budget with every source isolated checkpoints and resumes; `kMaxEpistemicCheckpointCells` is the measured value                                   | F27      |
| slots: a maximal reset fits the reserve; at the reserve every health entry refuses `ArenaExhausted`; the runbook works                                   | F26      |
| 6.1 cell counts equal `kHealthFixedCells`, `kReasonCells`, `kActionCells`; list removal at first, middle and last position                               | m38      |
| the isolate drop creates no label and no refutation; a latched tombstone's cost                                                                          | m20, m21 |

The fuzzer gains grant and health-action steps (each action under a valid, a revoked and a
wrong-kind grant) and the invariants: a refused action changes nothing; a faulted source's later
readings change nothing; `0 <= R <= N <= 1/(1-b)` against the largest bound in force; used cells
stay within `cells + cells/16`; a post-reset reading is never superseded. The harness
(`epistemic_invariants.hpp:27`, `:34`) extends `ProfileHealth` and `EpistemicSnapshot` with `R`,
`N`, the reasons, the actions and the scalars, and adds a `decayed()` check (m38).

## 7. Vocabulary and settings

Every name below has an ABI-test pin (`vagent_epistemic_abi_test.cpp`); a missed constant, entry or
remap is a test failure. `program.cpp:132-141` remaps every `CompiledAgent` entry from the staging
agent to the persistent copy, and `agent_executor.cpp:1276` lists the entries `prepare` checks as
sound; `instantiate` recompiles (`program.cpp:166-185`), so the remap is not load-bearing for a
running actor; both lists are updated for consistency (m34). `kProgramFormat` becomes
`vagent-program-4` in 2a and does not change again.

**ABI constants** (`runtime_abi.hpp`):

| Slice | Constants                                                                                                                                                                                                                                                                                                                                                                       |
| ----- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a    | profile `health_refused_weight`, `health_weight`; record `health_fraction` (`kObservationOrder` has 24 entries); states `SUSPECT`, `DEGRADED`; note `health`, `passed`; ingest `half_life`, `suspect_fraction`, `min_events`, `credited` (`kIngestOrder` has 8); `kHealthRefusalCodesVersion` 1; turn results `ClockEpochRegression`, `ClockMappingRegression`                  |
| 2b    | `CompiledAgent::settleEntry`; `kStatSettleDiscarded` (`settle.discarded`)                                                                                                                                                                                                                                                                                                       |
| 2c1   | `$turn.health` (`kTurnHealth`) and its field names; kinds `isolate`, `degrade`, `clear`, `reset`; turn results `HealthUnavailable`, `SupersededByReset`, `UnauthorizedHealthAction`; `CompiledAgent::healthEntry`                                                                                                                                                               |
| 2c2   | profile `reset_epoch`, `action_seq`, `fault_seq`, `reasons`, `actions`, `health_cells`; reason and action field names; origins `operator`, `device`; record `retired_by` and its five values; `kHealthFixedCells` 14, `kReasonCells` 16, `kActionCells` 20; turn results `SourceFaulted`, `StaleHealthState`, `NotOperatorReason`, `HealthReasonsFull`; outcome kind `NoChange` |

**`EpistemicRefusal`** (each with an `epistemicRefusalName` case): 2a `InvalidValue`,
`InvalidAcquisition`; 2c1 `InvalidHealthGrant`, `UnauthorizedHealthAction`, `InvalidHealthCode`,
`InvalidHealthRequest`, `UnknownGrant`, `SelfCertification`, `EntropyUnavailable`. New turn-result
codes are strings (`kTurnResult...`), as the existing IR-decided ones are.

**`outcomeClassOf`** (`agent_executor.cpp:3399`) gets explicit entries, so the default `Terminal`
stops hiding new codes (F25):

| Class             | Codes                                                                                                                                                |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| `NeedsHostAction` | `SourceFaulted`, `UnauthorizedHealthAction`, `NoProfile`, `HealthReasonsFull`, `HealthUnavailable` (plus the existing ones)                          |
| `Terminal`        | `ClockEpochRegression`, `ClockMappingRegression`, `InvalidValue`, `InvalidAcquisition`, `SupersededByReset`, `StaleHealthState`, `NotOperatorReason` |
| `Retryable`       | none new                                                                                                                                             |

`chargeable` excludes `SourceFaulted`, `SupersededByReset` and `SettleFailed` (F7) and every code of
2c1 that can reach `finishIngest`.

**Settings** (`EpistemicLimits` field, `settings::` name, `defaultSettingSpecs` entry, expected list
in `system_docs_test.cpp`, Schema text; each has an `EpistemicLimitError` enumerator and a
`fromModel` load, the Float one through the new loader of 3.6):

| Setting (`vagent.`)                  | Field                   | Default | Validation                              | Slice |
| ------------------------------------ | ----------------------- | ------- | --------------------------------------- | ----- |
| `epistemic_health_half_life`         | `healthHalfLife`        | 32      | `ingressQueuePerSource <= H <= 10^6`    | 2a    |
| `epistemic_suspect_fraction`         | `suspectFraction`       | 0.25    | finite, in (0, 1\]                      | 2a    |
| `epistemic_suspect_min_events`       | `suspectMinEvents`      | 8       | `1 <= m <= floor(0.9 / (1 - 2^(-1/H)))` | 2a    |
| `epistemic_health_pending`           | `healthPending`         | 16      | 5 to 4,096                              | 2c1   |
| `epistemic_health_pending_per_grant` | `healthPendingPerGrant` | 2       | 1 to `healthPending`                    | 2c1   |
| `epistemic_control_per_wake`         | `controlPerWake`        | 16      | 1 to 10^6                               | 2c1   |
| `epistemic_health_reasons`           | `healthReasons`         | 3       | 2 to 16                                 | 2c2   |
| `epistemic_health_actions`           | `healthActions`         | 4       | 2 to 64                                 | 2c2   |

Derived or fixed, with reasons: `slotReserve = arenaSlots / 64` and the hard-ceiling headroom
`cells / 16` scale with the settings they protect, and have no setting of their own;
`kMaxHealthGrants` 16 and `kMaxHealthGrantLifetime` 24 hours are grant policy of the API, not
deployment tuning; the reserved lane of 4 requests is the smallest number that lets an operator
isolate and reset while a degrade-only holder is at its bound. `validate()` also keeps
`cells + cells/16 <= kMaxEpistemicCheckpointCells` and
`maxSources x (healthBound + kProfileBaseCells) <= cells` (6.6). An invalid value anywhere in the
block keeps the defaults for the whole block with a WARN naming the field (F30, decision A).

## 8. Why Vortex, and the C++ that is not

Everything that is state or a rule about state is emitted IR run by the VM: the estimator, the
health state, the three structures, the four actions, the drop body, the settle entry, the
`SourceFaulted` refusal, the reset epoch and floor writes. The C++ is limited to categories
AGENTS.md permits:

| C++                                                                                | Slice  | Why it is not Vortex                                                                                |
| ---------------------------------------------------------------------------------- | ------ | --------------------------------------------------------------------------------------------------- |
| counted-code table, once-per-event bookkeeping, pass credit, closing a charge      | 2a     | decided in `observe`, before the arena sees the event (transport); the list is published and pinned |
| `sourceHealth` read API                                                            | 2a     | a read-only turn would spend arena slots, which are never reused, on every poll                     |
| settings load and validation                                                       | 2a-2c2 | host configuration                                                                                  |
| settle scheduling, the discard, the slot gates                                     | 2b     | recovery from a failed VM turn cannot depend on the budget that failed; gates are host admission    |
| grants, token, entropy, digest compare, issuer rules, queues, receipts, ticket log | 2c1    | authorization, clocks, entropy and transport                                                        |
| `ResetMark`, `admittedSeq`, generation stamps                                      | 2c1    | transport ordering state, not checkpointed                                                          |
| `wouldChange`, `removed` text composition, admission arithmetic, sink calls        | 2c2    | host formatting of data already read, host admission against a host setting, a host callback        |

## 9. Price, measurement gates and the simulation

Prices at the defaults (H 32, history 4, keys 4, 128 sources, 24,576 cells, 3 reasons, 4 actions),
cumulative within the 24,576-cell ceiling. Cycle figures are **estimates**; the gate column is what
the slice measures before it merges.

| Slice | Cells                                                                              | Cycles (estimate)                                    | Gate                                                                                                                   |
| ----- | ---------------------------------------------------------------------------------- | ---------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| 2a    | +4 a profile, +2 a record; a principal about 1,360 (1,320); 18 fit                 | +100 an ingestion, +100 a bookkeeping turn           | ingestion at most 5% over the 4.3k baseline; slots and scratch per ingestion                                           |
| 2b    | none                                                                               | `1 + n` turns a change, `n <= 4 P`                   | a 16-event retire in one wake                                                                                          |
| 2c1   | none (host memory bounded by settings)                                             | one mutex hold, at most 16 digest compares a request | none beyond the tests                                                                                                  |
| 2c2   | up to 142 reserved a profile, +2 a retired record; a principal about 1,520; 16 fit | about 300 + 150 a record dropped                     | a 16-record reset within half of `vagent.max_instructions`; checkpoint at the hard ceiling; slots a maximal reset uses |

**The simulation is a test, `HealthEstimatorSimulation`** in `vagent_epistemic_health_test.cpp`
(m7). It uses `std::mt19937_64` seeded 1, 4x10^5 Bernoulli events per rate, a 3H warm-up, and
asserts the table of 3.3 within 2 percentage points of SUSPECT share. Its other cases reproduce the
numbers cited above: the relay shares (mean `f` 0.199 at 0%, 50% and 100%, against 0.199, 0.111 and
0 for revision 2's rule), the stalled-pump case (peak `f` 0.78 without the credit and 0.06 with it),
the start-up false-alarm rates (1.9% and 10.7% at `min_events` 8; 13% and 30% at 3), and the order
error bound `b^-m` (1.09 at `m` 4, 1.19 at 8, 2.0 at 32).

## 10. Moved out of I3-2, and recorded as not done

- **Cross-key retained transfer.** P19 2.1 moves a removed support's retained records "to an owned
  retained-observation list under the keyed calibration record". That record is I4's; until it
  exists, every removed support's history stays on the profile's retired chain, and reset and
  isolate use the same chain. I4 re-homes it, keeping retained IDs, outcomes and floors.
- **User-level revision.** I3-1a statically refuses every program writer of an arbitrated predicate;
  letting a program assert or retract one needs the AST and lowering I4 adds.
- **Sensor health reports.** "A sensor's authenticated health report may add a configured warning"
  needs a schema and a telemetry path; it lands with I3-3, adding `device`-origin reasons whose
  recovery percept clears only reasons that source added. The self-clear rule holds from 2c1: no
  sensor identity can hold a health grant (5.2).
- **The language directives.** `reset`, `isolate`, `degrade` and `clear source` lower in I4 onto
  `healthEntry`, under the same grant check. Until then P19 8.2's "unauthorized reset is rejected"
  is met at the host API only.
- **Gate rejections.** `rejection_streak` thresholds (three SUSPECT, five FAULTED) and the gate
  estimator are I4's, with the gates that produce them.
- **A second grant bit for resets (m22).** Reset is the most destructive kind. One bit per kind and
  the explicit `sources` list stay; a separate bit for a reset over several principals is an open
  choice for the owner, not built.
- **A host latch mirror (F23, decision A).** Revisit only with a measured flood case.
- **Cross-host resume fencing (m25).** `resume` does not fence an instance still running elsewhere,
  so two actors can hold divergent latches. Out of scope; the translator and I5 own it.
- **A grant-gated clear of stream floors (F22 option B).** Not built; it would widen replay to
  `max_age`.

## 11. Departures from P19 v1.5

The spec is not edited. A1-A4 are the amendments of [decisions.md](decisions.md) section 3; A5-A10
are new in this revision. All are pending owner ratification and are folded into spec v1.6 at F1.

| #   | Spec section | Departure                                                                                                                                                                                                                                                                                                  | Source        |
| --- | ------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------- |
| A1  | 5.1          | `clear` removes operator-placed degradation reasons; it never unlatches FAULTED and never touches calibration                                                                                                                                                                                              | owner, rev 1  |
| A2  | 5.1          | health thresholds are three actor-wide settings, not policy fields; SUSPECT from content refusals is an event-indexed decayed fraction; a healing fraction is a second SUSPECT source; on resume `R` and `N` are read under the settings in force and `N` is clamped at the next update (F33 items 1 to 3) | owner Q2, F33 |
| A3  | 2.1, 5.1     | a source needs a durable or fresh incarnation; one without cannot be an arbitrated source                                                                                                                                                                                                                  | F22           |
| A4  | 5.1          | a program whose own handler for an arbitrated predicate always fails cannot ingest that predicate; an operator isolate or reset unwedges it                                                                                                                                                                | F11           |
| A5  | 5.1          | reasons carry an `origin`; `Clear {}` removes operator-origin reasons only and `Clear {code}` refuses another origin; one reasons slot is reserved for evidence-driven reasons (refines A1; F33 item 4)                                                                                                    | F29, F33      |
| A6  | 2.1, 5.1     | a committed reset names and bumps a `reset_epoch` (a retry of a stale epoch refuses), stamps it into new records' `calibration_epoch`, and raises each stream's sequence floor to the highest admitted before it                                                                                           | F17, F20, F21 |
| A7  | 5.1          | isolate withdraws the source's supports at once and refuses its later readings, instead of only excluding it from I4's eligibility                                                                                                                                                                         | epistemology  |
| A8  | 5.1          | relayed outcomes count in `R` and `N` once per (incarnation, event ID), and a reading that passed every check but was dropped under load counts as accepted                                                                                                                                                | F1, F4        |
| A9  | 5.1, I3-1    | belief events raised by a source change or health action run in their own turns after the change commits, one top-level event per turn; a failing event is discarded alone and reported                                                                                                                    | F9, F10       |
| A10 | 5.1          | I4 may score with a smooth penalty computed from `(R, N)` as one of P19's "configurable heuristics", combined with the discrete inflation by maximum                                                                                                                                                       | owner Q2, F33 |

**Owner questions** ([findings-r2.md](i3-2-review/findings-r2.md) section 4) answered as
recommended: relayed outcomes count once per event (Q1, F1); a smooth penalty is accepted (Q2, A10);
isolate drops supports and makes no label (Q3, 6.2); the ingestion wedge stays (Q4, A4); resume
fencing is out of scope (Q5, section 10); the reset cut is a sequence floor (Q6, 6.4); per-event
settle turns (Q7, 4.1); host-wide self-certification, delegation a recorded limit (Q8, 5.2);
`HostBindings` never reaches the API (Q9, 5.1); the audit sink is in scope (Q10, 6.5);
refused-while- isolated readings are counted host-side (Q11, 6.5); `UncertainAcquisition` is host
time class (Q12, 3.1); no operator override of device warnings now (Q13, I3-3); devices without a
boot counter are unsupported (Q14, 6.7); no non-destructive forgiveness (Q15, 3.3); a mistaken
isolate costs calibration, per P19 5.1 (Q16); the upgrade guard covers both overloads (Q17, T0); a
reset clears the liveness stamps (Q18, 6.4); no mirror (Q19, section 10).

## 12. Round-2 findings and their resolution

Round 1 (revision 1, commit `e226c507`) is resolved in revision 2; its mapping is in commit
`cae4a6ed`. The table maps every round-2 finding.

| Finding | Resolution                                                                               | Section      |
| ------- | ---------------------------------------------------------------------------------------- | ------------ |
| F1      | relayed outcomes count in `R` and `N` once per event; signed-basis rule for relays       | 3.1, 3.2, A8 |
| F2      | order error restated both ways, bounded by the queue share; close a charge on accept     | 3.2          |
| F3      | `min_events` 8 as a weight; `validate()` caps it and requires `H`; start-up rates        | 3.3          |
| F4      | pass credit for readings dropped after their checks; `credited` skips the later accept   | 3.2          |
| F5      | classified at the check site: epoch regression and bad values are health; time class out | 3.1          |
| F6      | `health` count on `Charge` and the note; versioned table; C++ justified                  | 3.1, 7, 8    |
| F7      | exclusions for `SourceFaulted`, `SupersededByReset`, `SettleFailed`; latch check first   | 4.1, 6.5, 7  |
| F8      | `charge` and the estimator find-or-create; the probe sequence is the test                | 3.7          |
| F9      | one settle turn per top-level event; discard alone; counts and names reported            | 4.1          |
| F10     | inline in `deliverHost`; gates; executor-side discard; one publication; `SettlePending`  | 4.1          |
| F11     | accepted residual, recorded as A4                                                        | 4.1, 11      |
| F12     | host-only boundary; `HealthIssuer`; `host_bindings` check                                | 5.1          |
| F13     | `RAND_bytes`, fail closed; digest and `CRYPTO_memcmp`; non-formattable; constants        | 5.2          |
| F14     | enforced at grant, bind and revive, and turn time; host-wide; delegation a limit         | 5.2          |
| F15     | lookup by token digest at the check; `revokeHealthGrant` counts; linearisation point     | 5.2, 5.3     |
| F16     | per-grant and total bounds, reserved lane, batch isolate, per-wake cap, all settings     | 5.3, 7       |
| F17     | `expectResetEpoch`; `NoChange` without a turn; ticket with instance; tri-state outcome   | 5.3, 6.3     |
| F18     | `sourceHealth`, `sourceHealths`, `refusedWhileFaulted`                                   | 3.5, 6.7     |
| F19     | reset generation per principal; commit-time floor; closed charges; checked twice         | 5.4, 6.4     |
| F20     | stream sequence floors raised by a committed reset                                       | 6.4          |
| F21     | `reset_epoch` into `calibration_epoch`; `retired_by`                                     | 6.4          |
| F22     | runbook deleted; durable or fresh incarnation required (A3)                              | 6.7, 11      |
| F23     | no host latch mirror; the arena refusal and the queue share bound a flood                | 6.5, 10      |
| F24     | which reboots it survives; `checkpointed`; `NoChange` replay; grants not durable         | 5.3, 6.7     |
| F25     | `isControl`, `owesAccounting`, the site table, explicit `outcomeClassOf`                 | 5.4, 7       |
| F26     | named gate change, slot reserve above the allowance, no-op allocates nothing, runbook    | 4.1, 6.6     |
| F27     | per-profile health reserve inside the ceiling, hard headroom, `2 x max_sources`          | 6.6          |
| F28     | evict by class, repeat counts, reset copies the fault, audit sink                        | 6.5          |
| F29     | `origin`, operator-only `Clear {}`, reserved slot and names                              | 6.1, 6.2, A5 |
| F30     | `fromModel`, Float loader, whole-block fallback with a WARN, tests                       | 3.6, 7       |
| F31     | shared upgrade guards for both overloads                                                 | 1 (T0)       |
| F32     | every enqueue signals the reactor                                                        | 1 (T0), 5.4  |
| F33     | amendments A2, A5, A10; resume rule                                                      | 11           |

| Minor | Resolution                                                   | Section  | Minor | Resolution                                                           | Section  |
| ----- | ------------------------------------------------------------ | -------- | ----- | -------------------------------------------------------------------- | -------- |
| m1    | stale and time mismatch move neither field                   | 3.1      | m20   | the isolate drop makes no label; `retired_by` gives the cause        | 6.2, 6.5 |
| m2    | once per event                                               | 3.2      | m21   | a latched tombstone's growth stated; operator retires                | 6.5      |
| m3    | "about 64", "31 to 33"; tests assert bounds                  | 3.3      | m22   | one bit per kind; second bit an open choice                          | 10       |
| m4    | advance inherits `R`, `N`; reset zeroes                      | 3.4      | m23   | a flooder keeps the queue non-empty; retire or unbind it             | 6.5      |
| m5    | what each state licenses; the I4 list                        | 3.4, 13  | m24   | steady-clock lifetime recorded; 24 hour bound                        | 5.2      |
| m6    | slots, scratch and subnormals measured                       | 3.8      | m25   | resume fencing out of scope, recorded                                | 10, 13   |
| m7    | the simulation is a named, seeded test                       | 3.3, 9   | m26   | `actions` holds accepted actions; refusals reach the sink            | 6.5      |
| m8    | one seeded run asserting after a 3H warm-up                  | 3.8, 9   | m27   | `SourceFaulted` disclosure accepted, not charged or logged           | 6.5      |
| m9    | `b` computed in the IR from the integer `H`                  | 3.2      | m28   | grant label carries the instance                                     | 5.2      |
| m10   | clamp at update; fuzzer uses the largest bound               | 3.2, 6.8 | m29   | expired and revoked grants purged                                    | 5.2      |
| m11   | `f` in the stamp; not replay-deterministic; after the update | 3.4      | m30   | code charset and length; principal check                             | 5.3      |
| m12   | the 64 hour delay in the Schema notes                        | 3.3      | m31   | scored results omit issuer, grant and operator code (I4)             | 13       |
| m13   | guard before dividing; no `branchIfZero` on a string         | 3.4      | m32   | citations by name and HEAD line; the count is `epistemicCellsWithin` | 2, 6.6   |
| m14   | the discard is executor-side                                 | 4.1      | m33   | 2a cannot reach reason branches; `rtSourceDrop_` lands in 2b         | 3.4, 4.2 |
| m15   | `$turn_count` per settle turn noted                          | 4.1      | m34   | remap, `sound` list, `kProgramFormat` decided                        | 7        |
| m16   | maps erased in `forget`, not `unbind`; test                  | 5.4      | m35   | synthetic-profile tests for the unproduced branches                  | 3.4, 3.8 |
| m17   | dropped charges added to lifetime counters at commit         | 6.4      | m36   | `health_weight`, `ResetMark`; two outcome mechanisms explained       | 3.2, 5.4 |
| m18   | reset invalidates by receipt time; recorded                  | 6.4, 13  | m37   | `rtSourceDrop_` takes `incBelow`; retire keeps its tail              | 4.2      |
| m19   | liveness stamps cleared and recorded in the entry            | 6.4      | m38   | harness, `decayed()`, removal tests, injected steady clock           | 5.6, 6.8 |

## 13. Obligations on later iterations

**I3-3 (heartbeats, clock mapping, sensor reports).**

- Reasons already carry `origin`; a recovery percept clears only reasons its source added, and
  uniqueness is (kind, code, origin). One slot is reserved (6.1).
- A sensor's health report is a separate path that adds `device`-origin `degraded` or `suspect`
  reasons with its own per-source bound. It never uses `healthEntry` with a sensor issuer.
- Reinstate the censored time refusals once path lateness is separable; own the
  `UncertainAcquisition` and mapping-regression classification and a time-class fraction with a
  `common_cause` test; add `sequence_regression` as a diagnostic; keep the counted-code list a
  versioned constant.
- An absent `last_receive` means "never supplied a reading". Silence timers skip latched sources. A
  reset clears both liveness stamps (6.4); heartbeat expiry adds a reason or a flag and never moves
  `R`, `N`. Each `actions` entry carries `wall` only when synchronised, and the heartbeat rebase
  must not use the grant clock.
- The acquisition-time reset cut (F20 option B) and the reset's receipt-time limit (m18) depend on
  the mapping; a relay declaration belongs on the binding, designed once for events and health
  reports.
- Authentication (O4): the reference adapter makes `issuer` and origin signatures real outside
  tests.

**I4 (queries, scoring, gates, directives).**

- Eligibility recomputes the state at query time through `rtHealth`, never from the stamp. The three
  health settings reach query and dispatch turns through a reserved agent field or the scoped
  evaluation grant.
- I4 owes: the stamp may weight, never exclude; a post-reset reading is weighted from the prior; the
  penalty is a function of `(R, N)` with shrinkage (A10), and discrete inflation and the smooth term
  combine by maximum; inflation is keyed on the reason class `rtHealth` returns; calibration trains
  on readings whatever their stamp, or on gate-passed readings only, so excluding SUSPECT readings
  cannot stop it learning the drift that made them suspect.
- The gate estimator shares the `(R, N)` shape and floor; `rejection_streak` thresholds apply to the
  state rule only. Use `f` only now that F1 and F4 hold.
- Calibration resets inside `healthEntry`'s reset turn and keys on the arena `reset_epoch`
  (`ExpiredLabel` depends on it). The isolate drop produces no label. The retired chain is re-homed
  under the keyed calibration record.
- Directives lower onto `healthEntry`; the check is the token at the host edge. Scored results and
  `answer` records omit `issuer`, `grant` and operator codes (m31).
- Programs that react to `-pred` can miss discarded events; surface `settle.discarded` in `stat()`
  (done in 2b). Eligibility must not read profile count as live-source count. If `+pred`/`-pred`
  should fire on changes of the arbitrated answer, the isolate drop's events change meaning.

**I5.** The keyed learner and label expiry match on the calibration epoch; cross-host resume fencing
and delegation chains for `DeviceDelegation` (m25, F14) have no earlier home.

**Live-upgrade translator (T1).**

- Carry as owned graph: `faulted`, `fault_seq`, `reasons` (with `origin`), `actions`,
  `health_refused_weight`, `health_weight` (Doubles, canonical bits), the scalars, lifetime
  counters, dedup, streams, the retired chain, `reset_epoch`, `retired_by`, and the `health` and
  `health_fraction` stamps. Never clear a latch.
- Do not carry grants, `ResetMark`, `admittedSeq`, queued health actions or the outcome log;
  operators re-grant. Refuse while a health action is queued or `d.pending` is linked.
- Health settings are not in the policy hash; a changed `H` does not rescale history; a shrinking
  `reasons` or `actions` bound truncates by 6.5's eviction priority and counts `actions_evicted`.
- A test asserts `settleEntry` and `healthEntry` are engaged after `instantiate`. Both overloads
  already refuse populated roots until a translator is registered (T0).
