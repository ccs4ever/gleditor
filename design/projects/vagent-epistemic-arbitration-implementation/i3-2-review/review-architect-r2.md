# I3-2 design review, round 2: System architect (persona architect)

Scope: `i3-2-design.md` revision 2 (commit `cae4a6ed`, unchanged at HEAD), against P19 v1.5 sections
2.1, 2.2, 4, 5.1 and 8.2, the owner's decisions Q1 to Q3 and the two I3-2 decisions in the brief,
and `i3-design.md` section 14. Code at HEAD `3ae8d384`. I read the 34 commits after `8ec71fc6` only
where they touch the paths below. This is my first review of I3-2. Scripts and output are under
`/tmp/p19-architect/` (`est.py`, `budget.py`).

## Earlier findings

None. This persona is new to the review.

## What is sound

- **Estimator choice.** An event-indexed fraction with a bounded recurrence, no clock and nothing to
  rebase on resume fits Q2 (latches for life, decayed counters, ML-reviewed half-life). I recomputed
  the design's figures of 14 events to SUSPECT from a clean history and 64 to recover (`est.py`);
  both hold. The fraction needs no new storage beyond two Doubles.
- **Resume carries the profile graph without new code.** `restoreState` clones each profile over
  `d.vars`, `d.values`, `d.grab`, `d.step`, `d.clone` and `d.epistemic`
  (`agent_executor.cpp:4767-4790`). `fault`, `reasons` and `actions` use those shapes, so they come
  across a checkpoint unchanged. Every new timestamp is wall time, which needs no rebasing, and
  `last_receive` and `last_contact` are already dropped on resume.
- **Authorization stays in C++, learned state stays in the arena.** Grants, the token, the wall and
  steady stamps and queueing are legitimately host concerns (AGENTS.md: authorization, clocks,
  transport). Counters, latch, reasons, audit list and fault provenance are arena cells that a
  reader can inspect.
- **Self-issue rule.** Refusing an issuer that is a bound or named source is a real structural
  defence of P19 4 ("a sensor cannot clear its own fault"), and the token keeps a program that can
  read the arena from forging a grant ID.
- **Absent reads as initial.** Profiles restored from before 2a, tombstones and refusal-only
  profiles all get a defined state (design section 4).
- **The control-item predicate list is complete as stated.** The four places that test
  `sourceChange.has_value()` are `PostBox::pushControl` (`agent_executor.cpp:642-648`), the pump
  insertion (`:3911-3915`), and the `epistemic` and `abandon` lambdas (`:3845-3861`).
- **Deferred items are placed correctly.** Retained transfer, user-level revision, sensor reports,
  language directives and gate rejections each go to the iteration that owns their producer.

## BLOCKING

None.

## MAJOR

### M1. Departures from P19 5.1, 2.2 and 4 are not recorded, and one is a spec conflict

Problem. Section 11 amends the spec for `clear` only. The design also departs from approved text:

1. P19 4 lists "health thresholds" among the typed fields of the predicate's arbitration policy
   record, and P19 5.1 calls the defaults "configurable per-profile engineering heuristics". The
   design makes them three actor-wide settings and rejects a policy key (section 3.3, "Refused
   alternatives").
1. P19 2.2 says upgrade and checkpoint must "Preserve ... health policy". Settings are not in the
   policy hash or the checkpoint, so a resume or upgrade on a host with a different half-life
   silently changes how the stored `R` and `N` are read. `N` is bounded by `1/(1-b)`, so a shorter
   half-life also puts stored values above the new bound that the tests assert.
1. P19 5.1's suspicion rule is "three consecutive rejections" with "accepted measurements clear only
   the innovation rejection streak". The design adds a second SUSPECT source (refusal fraction) that
   accepted readings do heal. Section 3.3 explains why it is not the streak, but section 11 does not
   list it as a spec change.
1. Section 11 words `clear` as removing "degradation reasons an operator placed". Section 7.4 has
   `Clear` remove "every `degraded` reason". The two agree only while no non-operator producer
   exists (see M5 and Fit to later iterations).

Evidence. P19 lines 470-480 (health thresholds in policy fields at the "Policy fields are typed
constants" paragraph), 558-585, 192-252; design sections 3.3, 7.4, 11.
`agent_executor.cpp:4815-4829` compares policy hashes at resume and does not see health settings.

Consequence. P19 8.2's acceptance list ("silence, freeze, persistent degraded reasons and latched
reset obey the health contract") cannot be checked against a spec that no longer describes the
build, and the I3-3 and I4 authors inherit an unrecorded contract.

Required fix. Extend section 11 into a spec-amendment list covering items 1 to 4, each with the
owner's acceptance. Decide item 2 explicitly: either carry the three health settings as a hashed
"health policy" with the profile (so resume checks them), or record that resume under changed
settings reinterprets `R` and `N` and define the rule (rescale `R`, `N` into the new bound, or
refuse). Reword `Clear` to match the spec text or amend the spec to match the code.

### M2. The ungated `upgrade(pid, CompiledProgram&)` path erases the state I3-2 makes valuable

Problem. At HEAD there are two upgrade entry points. The shared-pointer overload
(`agent_executor.cpp:5053`) refuses a populated `d.trust` or `d.epistemic` root while no
`OwnedStateTranslator` is registered (`:5317-5320`; I2U acceptance, `i2u-validation.md:57`).
`LocalHost::upgrade(pid, CompiledProgram)` (`program.cpp:399-403`) calls the other overload
(`agent_executor.cpp:5419-5475`). That one builds `makeStateReconciler` and swaps the context with
no such check. `reconciler.cpp` contains no reference to trust, epistemic, profile or latch (grep
count 0). `executeReconcile` copies agent fields, belief clauses over `d.source`, spaces and
intentions only. Nobody registers a translator (`setOwnedStateTranslator` has no caller), and
`vagent_upgrade_test.cpp:352,424` use the by-value overload without touching P19 roots.

Consequences:

- An upgrade through that path drops every profile, so a FAULTED latch, reasons, `fault` provenance,
  the audit list, `R`/`N`, dedup windows and stream floors all disappear. P19 2.2 says "Model reset
  or program upgrade never clears a hardware fault". Before I3-2 nothing set the latch. From 2c an
  operator isolate is silently undone by any upgrade through this path.
- The host state is not reset by that path: bindings, floors, the latch mirror and any grants stay,
  while the arena has no profile. A mirror-true source then refuses `SourceFaulted` at ingress with
  no arena evidence behind it.
- Its quiescence test is `state.inTurn` only (`:5426`). It does not check `hostEvents`, the post
  box, or `d.pending`, so a queued reset or isolate and an unsettled event list are carried across
  the swap; the shared-pointer overload checks all three (`:5068-5075`).

Required fix. State in the design that 2c is not accepted until this path refuses (or is routed
through the gated one) whenever `d.trust` or `d.epistemic` is populated, and that its quiescence
test equals the gated path's plus the control-item predicate. Add a test per overload: isolate a
source, upgrade, and require either a refusal with the latch intact or a preserved latch. Add
"upgrade clears the host mirror and refuses with grants outstanding, or documents that grants die"
to the same test. Record that I2U's "refuses populated roots" property must hold for every public
upgrade entry.

### M3. The budget exemptions are not implementable and break the checkpoint invariant

Problem. Section 7.4 exempts isolate, clear and reset "from the cell ceiling and from
`ArenaExhausted`, as source changes are from the ceiling". Two things are wrong.

1. Source changes are not exempt from the slot allowance. `deliverHost` tests
   `cellCount() >= arenaSlots` for every entry routine before it runs the turn and finishes the
   change as `ArenaExhausted` (`agent_executor.cpp:1873-1879`). `healthEntry` would be refused at
   exactly the moment the design says it must not be. The design never mentions changing this gate.
1. Exempting the provenance records from `vagent.epistemic_cells` defeats the reason that ceiling
   has its value. `epistemic_limits.hpp:38-41` sets 24,576 so "a full budget still checkpoints",
   with about two operations per cell against `vagent.value_operations` 65,536 (headroom 8,192 cells
   to the 32,768-cell point where checkpoints failed). With the design's own sizes (fault 14 cells,
   action 16, bound of 8 actions, `budget.py`), the exempt records of one profile are 142 cells. 57
   profiles' worth exhaust the headroom, and all 128 default sources would put the actor at 42,752
   cells (85,504 operations). Checkpoint and resume is also the only way to reclaim arena slots, so
   an actor that cannot checkpoint cannot recover from `ArenaExhausted`.

Required fix. Name the gate change: `healthEntry`, `settleEntry` and `discardPendingEntry` bypass
the slot test, with a bound on the slots they may use. Bound the exempt records by reservation, not
by exemption: count a fixed health reserve per bound source inside the ceiling
(`maxSources * perProfileHealthBound`), or add a `validate()` rule that
`cells + maxSources * perProfileHealthBound` stays under the checkpoint limit. Add the checkpoint
test at a full budget with every source isolated, which is the case that fails today.

### M4. Slice 2b leaves the wedge open at its edges and reports a commit as a refusal

Problem. Three gaps in section 6.

1. **The discard turn is assumed to succeed.** If `settleEntry` fails the executor runs
   `discardPendingEntry` "and commits", with no stated path if that turn also fails or is gated (M3
   gate, fuel, a throw). Then `d.pending` keeps the events and the next ordinary turn's own
   `rtSettle_` fails on them. That is the wedge moved to every later dispatch turn, which are not
   behind the slot gate. `checkpoint()` does not test `d.pending` (`:4932-4960`), so a resume
   quietly drops them as well.
1. **Outcome vocabulary.** `outcomeOfCode` maps any code other than `Committed` or `Duplicate` to
   `Refused`. `finishSourceChange` raises the host's incarnation floor only for `code == Committed`
   (`:1936-1962`). A committed advance whose settle failed would be logged `Refused/SettleFailed`,
   would not raise the floor, and would tell a retrying host it failed. The design says the change
   "stands" but does not give the outcome a kind that says so.
1. **Scope against the owner's decision.** The owner said to fix the `-pred` handler wedge. The fix
   covers advance, retire and (2c) health actions. An ingestion whose keyed replacement drops a
   support still runs `rtSettle_` inside the ingestion turn (`compiler.cpp:5668`), so a handler that
   always fails still blocks every replacing reading of that source with `IngestionFault`. The
   design should say whether that is by design.

Required fix. (1) Make the discard unable to fail: no cell minting, fixed fuel, exempt from the slot
gate, and a last resort that the executor performs directly (`arena.unlink` of `d.pending`, which is
the same kind of between-turn cleanup `runTurnOnce` step 8 already does in C++), with a test that
injects a failing discard. (2) Give the outcome a "committed, handlers failed" kind, and list
`finishSourceChange` as a place that treats it as committed. (3) Add a sentence stating the
ingestion residual, or ask the owner (see Questions).

### M5. The lowest privilege can flush the audit list

Problem. Section 5 keeps `actions` to 8 entries, "oldest evicted", and says a reset never clears it,
so "the profile always says that a fault existed, who latched it and who cleared it". A repeat
degrade with the same code "adds no reason ... and is recorded only in `actions`". A holder of a
grant that allows only `Degrade` can therefore issue eight repeat degrades and evict the records of
an earlier isolate and the reset that followed. After a reset the `fault` record is gone, so those
records were the only provenance. The sentence "The host keeps its own log for anything older" has
nothing behind it: the host log is `outcomeLog`, a per-source ring of 64 that is not checkpointed.

Consequence. A privilege-ordered audit trail that a lower privilege can erase. P19 2.2 requires the
latch's provenance to be preserved.

Required fix. Evict by class, never by age alone: isolate and reset records are not evicted by
degrade or clear records; coalesce a repeat degrade or clear into its existing record with a count
instead of appending; or keep a separate small fault-history list (at most the last N latch
changes). State what the host log is, how long it keeps entries, and remove the "anything older"
sentence if there is no such log.

### M6. Settings: a legal configuration disables SUSPECT, and the loader does not refuse

Problem.

1. `N` can never exceed `1/(1-b)`, which is 46.7 at H = 32 and 2.0 at H = 1 (`est.py`). The domains
   `half_life` in [1, 10^6] and `min_events` in [1, 10^6] are independent. `min_events` above
   `1/(1-b)` makes the fraction rule unreachable, and the default `min_events` 3 is already
   unreachable at H = 1 (maximum N is 2.0). Nothing in section 3.3 or 9 couples them.
1. Section 3.4 says out-of-domain values "refuse". The existing loader does not: `loadPositive`
   skips non-positive values, and a failing `EpistemicLimits::validate()` discards the whole
   epistemic block, including `max_sources` and the I3-1 bounds (`agent_executor.cpp:5632-5652`).
   `SettingSpec` carries types only (`system_docs.hpp:922`), so "Domain" in the table has no
   enforcement point. A Double setting with NaN needs its own reader because `loadPositive` is
   Int-only.
1. Section 3.3 says a fresh profile "reads SUSPECT after its first three readings if all three are
   refused". `N` after three refusals is 2.936, below `min_events` 3, so the state flips at the
   fourth reading (`est.py`).
1. Section 9 says "Six new `EpistemicLimits` fields" and lists five.

Required fix. Add a `validate()` rule `min_events <= floor(1/(1-b))` with its own
`EpistemicLimitError`, and say whether a bad health setting falls back per setting or invalidates
the block (per setting is better and needs no change to the other bounds). Correct the table and the
count, and state which owner-visible Schema text carries the interpretation.

### M7. The health-refusal classification lives in C++ and adds an unlisted ABI field

Problem. Section 3.1 defines "health refusals" as four codes. Today the split between `content` and
`other` is made by `Ingress::isContentRefusal` and `Ingress::charge` (`agent_executor.cpp:895-900`,
`:875-885`), a C++ list of six codes, and `$turn.refusal` carries only `content`, `other`, `code`
and `contact` (`runtime_abi.hpp:652-656`). The design needs a third count. It lists no
`kRefusalHealth`-style constant, no change to `Ingress::Charge` and no change to the `note` record
built in `drainPosted` (`:3927-3940`). Section 8's list of C++ responsibilities does not mention
that deciding which refusals are evidence against a source is a policy judgement made in a C++
`static bool`. AGENTS.md requires new C++ to justify why it is not Vortex, and the existing comment
("host bookkeeping about the transport") does not cover a health rule.

Required fix. Pass the per-code counts (at most four health codes plus `other`) in `$turn.refusal`
and let the emitted routine pick the codes that feed the estimator, so the policy is arena-visible
and changes with the program. If the list stays in C++, list the new field in section 9 and add a
paragraph to section 8 justifying it. Resolve the asymmetry in the same step (see m5).

### M8. The host latch mirror is a second decision point with no stated invariants

Problem. Section 7.4 has the executor "mirror the latch host-side once the isolate commits", refuse
`observe` at ingress while it is set, and "rebuild" it after a resume from the arena's refusal. The
design does not say what clears it. The reset epoch is bumped at call time (section 7.3); a reset
whose turn then fails (budget, a revoked token, fuel) leaves a mirror that was either cleared too
early (permissive) or never cleared (a source stuck host-side with an arena that would accept it).
The mirror is also set by an inferred outcome code (`SourceFaulted` seen after resume), which makes
a C++ table the memory of operator-set health state. P19 2.2 and the brief's rule ("no C++ table
stores learned epistemic state") are met only if the mirror is a pure cache of the arena.

It also earns little. The arena refusal creates no cells, and the per-source queue share
(`ingressQueuePerSource`) already bounds a flood. The remaining benefit is skipping about 4.3k
cycles per refused reading, against the wake budget of 4 ingestions (`ingestPerWake`).

Required fix. Either drop the mirror from I3-2 as built before its need (add it with a measurement
when a flood case exists), or specify it as a cache: set only after a committed isolate or an arena
`SourceFaulted` outcome; cleared only by a committed reset, a decommission, or loss of the actor;
never touched by an API call; cleared on any upgrade (M2). Add one test per transition, including a
reset that fails after the API call returned.

## MINOR

- **m1. Stale citations and a nonexistent field.** `compiler.cpp:5257` for the NOMINAL/FAULTED stamp
  is at `compiler.cpp:5512-5522`. `$epistemic_cells` is not a field. The count is the C++ walk
  `epistemicCellsWithin` (`agent_executor.cpp:3305`), which the design should name, since it counts
  whatever hangs off the profile. `agent_executor.cpp:845` for content refusals is at `:882-895`.
- **m2. Slice independence.** 2a and 2b are separately shippable. 2a's `rtHealth` reads `reasons`
  and `fault`, which only 2c creates, so 2a tests cannot cover those branches; say so. 2a also has
  no reader before I4: no host accessor shows an operator `f` or the state, so the only consumer is
  the record stamp. Add a read-only accessor (an IR read entry like `answerEntry`, or a test hook),
  or record that tests are the only consumer. 2c bundles about ten deliverables and depends on 2a
  (reset zeroes `R`, `N`) and 2b. Split off the `rtSourceDrop` refactor (gated by the existing I3-1c
  tests, no behaviour change) so it lands with or before 2b; that leaves 2c as the grant, API,
  epoch, outcome log and `healthEntry`.
- **m3. CompiledAgent growth.** Three more optional entries, each hand-remapped in
  `program.cpp:132-148` with "a missed remap is a silent no-op". Group the six epistemic entries
  into one `std::optional<EpistemicEntries>` (present as a unit, remapped by one function), or add a
  test that every engaged optional on the staging agent is engaged on the persistent one. Also state
  the per-actor IR footprint: every instantiate compiles these routines into each actor's arena, and
  the design gives no before and after cell count.
- **m4. Settle and discard entries.** `discardPendingEntry` is one unlink plus a count. Say why it
  needs an ABI slot and per-actor IR rather than a host call (see M4 fix).
- **m5. Relay asymmetry.** An accepted relayed reading increments `N` (section 3.2) but a refused
  relayed copy moves neither `R` nor `N` (section 3.1). That favours trust, against the section's
  own claim that errors lean toward suspicion. Count relayed readings in neither or in both.
- **m6. Reset bumps the epoch before the reset commits.** A reset that later fails (revoked token,
  `TurnExhausted`) has already caused queued readings to refuse `SupersededByReset`, which is
  `Terminal`. Make the refusal depend on a committed reset, or class it `Retryable` when no reset
  followed.
- **m7. `suspect` has no producer.** The `suspect` reason kind, the step-2 branch of `rtHealth` that
  reads it, and `kind` on the `fault` record (one value, `isolate`) are built before I3-3. Keep the
  list shape but build the branch with I3-3, or test it with a synthetic profile and say so. The
  `kObsHealthSuspect`/`kObsHealthDegraded` stamps are the exception: 2a needs them.
- **m8. Grant details.** `issuer` is asserted by whichever host code calls `grantHealth`; the token
  stops a program from forging an ID, not an in-process caller from minting a grant. Say so. The
  issuer check runs only at grant time; a source bound later with that principal escapes it, so
  recheck at `bindSource` or at action time. `kMaxHealthGrantLifetime` has no value.
  `kMaxPendingHealthActions` (16) is the same kind of bound as `ingressQueue`, which is a setting.
  "Bounded by bindings" for the `max_sources` exemption (7.4) is not a bound, since `bindSource` has
  no count limit (`agent_executor.cpp:3414-3470`).
- **m9. Determinism of `b`.** Section 3.2 does not say who computes `b = 2^(-1/H)`. If the host
  does, two hosts can hold different bits in `R`/`N` for the same events. Compute it in IR through
  `Exp` (I1) from the integer `H`, as `k ln b` already is.
- **m10. Vocabulary against I3-1.** "Reset epoch" collides with P19's calibration/model epoch, which
  a source reset must create (P19 2.1) and I4/I5 label expiry matches on; name the host counter
  `resetGeneration` and record where the calibration epoch will live. `healthOutcome` returns
  `IngestOutcome`; and health actions use receipts and a log while source changes use a
  principal-keyed latest-only map (`sourceChangeOutcome`), two mechanisms for control-item outcomes.
  Counter names `health_refused`, `health_events`, `refused_count` and `content_refusals` are four
  near-synonyms for different things, and `health_events` is a decayed weight, not a count
  (`health_weight` and `health_refused_weight` are clearer).
- **m11. Clear semantics.** `Clear` with a code that matches no reason, and `Clear` on a profile
  with none, have no stated outcome (idempotent success or a refusal).

## Questions for the owner

1. **Isolate drops supports.** P19 5.1 says isolate latches FAULTED and "active fault status
   excludes all source readings", which is I4's eligibility rule. The design also drops supports at
   isolate, which cannot be undone: a reset does not restore them, and a sparse sensor loses its
   last reading. This is the sole reason 2c needs the drop refactor and 2b. Is the drop wanted, or
   should isolate latch only until I4's queries exclude the source?
1. **Ingestion wedge.** Does "fix the `-pred` handler wedge" include a failing handler on a keyed
   replacement inside an ingestion turn (M4 item 3)?

## Fit to later iterations

**I3-3 (heartbeats, clock mapping, sensor reports).**

- Reasons are unique by (kind, code). A sensor-added `low_battery` and an operator-added one
  collide, and "a recovery percept clears only reasons that source added" cannot be met. The reason
  record needs an origin (operator grant, or telemetry source) now, and `Clear` must filter to
  operator-origin (it already must, to match section 11's wording).
- Record that `last_receive` absent means "never supplied a reading" (section 4 does), that every
  health timestamp is wall time and absent when unsynchronised, and that heartbeat expiry adds a
  reason or a separate flag, not a change to `R`/`N`.

**I4 (queries, scoring, gates, directives).**

- Section 4's two rules (stamp for audit, `rtHealth` recomputed at query time) are sound. The query
  needs the three health settings through its own `$turn` record, and a policy snapshot that carries
  them (M1 item 2).
- Isolate and reset leave beliefs retracted through `-pred` events whose handlers may not have run
  (the discard in 2b). I4's "a query of an arbitrated predicate is the arbitrated answer" removes
  the need to read raw supports, but programs that react to `-pred` can miss events. Record the
  discard count in the `stat()` they can read.
- Reset must reset calibration to the declared prior in the same turn, and I4/I5's keyed learner
  match includes a calibration epoch. The host reset counter cannot serve that; the profile will
  need an arena-side epoch that reset increments.
- The retired chain is where isolate and reset park history; I4 re-homes it under the keyed
  calibration record, keeping retained IDs.

**Live-upgrade translator and checkpoint.** Carry, as owned graph: `faulted`, `fault`, `reasons`,
`actions` (with their incarnation and wall time), `health_refused`, `health_events` (as Doubles,
canonical bits), the lifetime counters, dedup, streams and the retired chain. Do not carry grants,
the mirror, the reset counter, queued health actions or the outcome log; say that the operator must
re-grant after a resume or upgrade, and make the upgrade fail while a health action is queued (M2).
`R` and `N` are meaningful only under the half-life that produced them; record `H` with the profile
or rescale on mismatch (M1 item 2). The translator must also preserve the `health` stamp on every
record, which is a string copy.

Tally: 0 BLOCKING, 8 MAJOR, 11 MINOR.

Verdict: FAIL
