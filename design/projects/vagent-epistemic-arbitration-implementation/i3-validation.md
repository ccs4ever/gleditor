# I3 validation

I3-1 lands in the slices of its [accepted design](i3-design.md). Each slice records its review and
evidence here.

## I3-1a: arbitration policy syntax and checker

Source baseline: `3e77f522` (accepted I3-1 design).

- **Syntax.** `arbitrated by truest|ranked|consensus(key: literal, ...)` on predicate declarations.
  `arbitrated` is contextual, so it and the strategy words stay usable as names. A leading minus is
  part of a numeric or duration constant.
- **Typed policy.** `epistemic_policy.{hpp,cpp}` holds the closed key table, the typed
  `ArbitrationPolicy`, every domain of design section 2 and a versioned canonical encoding with its
  SHA-256 hash (`vagent-policy-1`).
  - `score_tolerance` resolves to 1e-9 and `allow_untested` to false, so the hash records the value
    in force.
  - The encoding normalises -0.0.
- **Checker.** `InvalidEpistemicPolicy`, `MissingClusterTolerance` and `EpistemicIngressOnly`.
  `believe`, `forget`, `revise` and `believes ... from` are refused on arbitrated predicates.
- **Compiler.** It mints a policy master record on the predicate's `d.epistemic` rank, after the
  duplicate-name check. It emits no `tell`/`untell` test for an arbitrated predicate and guards both
  `rtBSet_` call sites.
- **Upgrade schema and CLI.** The upgrade schema carries the full policy and its hash. A changed,
  added or dropped policy is the refused `EpistemicPolicy` change, which
  `vagent check-runtime --upgrade` reports.

Review (sonnet personas; reports in [i3-1a-review/](i3-1a-review/)):

| Persona           | Round 1        | Round 2        |
| ----------------- | -------------- | -------------- |
| Compiler/codebase | PASS (7 MINOR) | —              |
| ML gatekeeper     | FAIL (3 MAJOR) | PASS (4 MINOR) |

Round 1 ML findings and their fixes:

- **Unresolved `score_tolerance` default:** now resolved and hashed; `allow_untested` added.
- **Hash test that varied one value:** a table that mutates every field and every optional key's
  presence.
- **Untested domains:** a per-key boundary table.
- **Minors:** zero alignment allowed, exact-only integer widening, -0.0, duration and prior
  ceilings, and documented units.

Codebase minors fixed:

- **Negatives:** they now reach the domain rules instead of failing to parse.
- **Mint order:** the master is minted only after the duplicate-name check.
- **Missing include:** `<bit>` added.
- **Design text:** section 2 lists the full key table.
- **Tests:** resume, same-policy upgrade and changed-policy refusal are now tested.

Evidence, headless with isolated XDG state:

- `make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test`: runtime 186, actor
  623 (13 `VagentEpistemicPolicy`), xuzz 2680 passed.
- Mutation checks: emitting `tell` tests for arbitrated predicates fails
  `PeerTellCannotWriteAnArbitratedPredicate`.
- `fuzz_vagent_runtime_policy` (new, in `make fuzz`): about 1.15 million runs in three minutes, no
  crash and no accepted policy outside its domains or with a nondeterministic hash.
- Changed C++ formatted with clang-format 19.1.7.

Building the fuzzer exposed three defects, all fixed:

- Fuzz objects missed `layoutfilade.o` and `focus_manager.o`, so no fuzz target linked.
- Fuzz objects had no header dependencies: a stale sanitizer object produced a spurious crash after
  a layout change.
- libstdc++ 16's `ranges::stable_sort` trips ASan's alloc-dealloc check inside the library; only
  that check is relaxed, in this harness.

Carried MINORs from ML round 2:

- Float duration literals round to whole milliseconds in the lexer.
- `alignment` and `decay` are still accepted on discrete predicates; I4 decides whether discrete
  policies use them.
- The policy master test checks a subset of fields; the hash is the full guard.

I3-1a is accepted.

## I3-1b, I3-1c and I3-1d: ingestion, incarnations, checkpoint and resume

Source baseline: `00db472b` (accepted I3-1a). Slices: `57331549` (typed turn result spike),
`f60e14c3` (sentinel sweep), `70d1b278` to `bdf20382` (I3-1b in five increments), `5a908ea8`
(I3-1c), `4ad3f53a` (fuzzing and simulation), `e91e8fe8`, `0124b0d2` and `08425c60` (review fixes,
`max_uncertainty` and I3-1d), and the closing round-3 commit that adds this record.

- **Ingestion** is the compiled Vortex routine `observeEntry`, run by the VM in its own turn. It
  finds or creates the source's profile, then checks in order: incarnation, dedup, the stream's
  sequence, clock epoch and mapping, and per-key acquisition order. It adopts the executor's record
  as the history entry, keeps one current record and one support per (principal, key), bounds keys
  and history, and is admitted against the cell ceiling. Every exit writes a typed `$turn_result`.
  The design is sections 3–6 of [i3-design.md](i3-design.md); its section 14 records every change
  the reviews made.
- **Host ingress** binds sources with lifetimes, aliases and a synchronous incarnation floor. It
  refuses unbound and malformed input without logging it. It checks every observation-time rule,
  `max_uncertainty` among them, gives each source and relay a fair share of the queue, keeps one
  bounded outcome log per source in which a committed entry is never overwritten, and charges each
  refusal once to the profile in a Vortex bookkeeping turn.
- **Incarnations.** Only binding advances one. The advance turn drops the predecessor's supports,
  settling `-pred`, and keeps their records on a bounded retired history on the profile. The retire
  turn leaves a tombstone that holds the floor. The arena refuses a binding below its floor, which a
  resumed actor exercises.
- **Checkpoint and resume** carry profiles, streams, dedup windows, histories and clone ranks
  through one copy map, and drop steady liveness stamps.

Review (sonnet personas; reports in [i3-1bc-review/](i3-1bc-review/)):

| Persona           | Round 1 (`4ad3f53a`) | Round 2 (`d48e85f8`) | Round 3 (`08425c60`) |
| ----------------- | -------------------- | -------------------- | -------------------- |
| ML gatekeeper     | FAIL (4 MAJOR)       | PASS (7 MINOR)       | PASS (6 MINOR)       |
| Epistemology      | FAIL (2 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |
| Hardware/fault    | FAIL (5 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |
| Compiler/codebase | FAIL (2 MAJOR)       | FAIL (1 MAJOR)       | PASS                 |

The owner decided two questions along the way: a `max_uncertainty` policy key (hardware round-1
MAJOR 3), and that an advance drops supports left with no current record while keeping their history
(open question 1, which codebase round 2 made blocking). The other open questions are written up in
[i3-open-questions.md](i3-open-questions.md).

Evidence, headless with isolated XDG state:

- `make -j$(nproc) vagent vagent_runtime_test vagent_actor_test build/xuzz_test xuzz`: runtime 186,
  actor 700 (100 `VagentEpistemic*`), xuzz 2680 passed. Two capture-dependent drag tests and two
  timing benchmarks failed intermittently over the slice. Each passes when rerun alone, and none
  touches vagent code.

- **Mutation checks.** Each new guard was mutated and a test failed:

  - stale-acquisition and revocation-at-drain checks, refusal classification and contact refresh;
  - incarnation ordering, the arena floor, tombstone budget exemption and retire excision;
  - sticky log, fair share, per-wake cap and epoch regression;
  - current-only key count, synchronous floor and uncertainty bound;
  - resume copy map, call-order source changes, support drop and history trim;
  - charge-once.

  One mutation survived because a redundant guard covers it: moving the advance to the back of the
  post box.

- **Noisy-sensor simulator** (32 seeds, `GLEDITOR_NOISE_SEEDS=32`): invariants held at every step,
  with 4,047 commits and every noise class reached.

- **`fuzz_epistemic_ingress`** (in `make fuzz`): more than 60 minutes in total, about 2 executions
  per second. It found two executor defects (an unlogged non-finite reading, and the ingress state
  of a stopped actor), the lifetime-saturation overflow, and five harness corrections. Each is fixed
  and in the corpus as a regression input. The seed corpus reaches 17 outcome codes.

- **Cost.** An ingestion went from 12,096 to 4,362 VM cycles, and from about 670 to about 250 arena
  slots. A source rebooting with new keys plateaus at about 1,500 epistemic cells over 160 boots.

- Changed C++ is formatted with clang-format 19.1.7. Markdown passes mdformat and mdl. No `noCell`
  was added.

Carried, recorded in design section 14:

- a failing `-pred` handler blocks the advance and retire for its source;
- the retired history is per profile and not age-ordered within one boot;
- health counters stay lifetime-cumulative (open question 2, I3-2);
- a source refused from its first event has no profile (open question 3);
- handler frames cost about 85 slots per activation, a compiler-wide cost;
- the fuzzer launches an actor per input.

I3-1b, I3-1c and I3-1d are accepted.

## I3-2c1: health grants, host API, post order and stub turn

Implemented from the I3-2 design (revision 4) without a per-slice review, by the lead's instruction:
one full review follows the whole implementation.

- **Added.** `health_grants.{hpp,cpp}` (grant, token, request, receipt, outcome and audit types),
  `agent_executor_health.{hpp,cpp}` (`grantHealth`, `revokeHealthGrant`, `healthAction`,
  `healthActionBatch`, `healthOutcome`, `setHealthAuditSink`, the in-flight reset snapshot),
  `agent_executor_internal.hpp` (executor-private types moved out of `agent_executor.cpp`), the
  `healthEntry` stub (`HealthUnavailable`) in `epistemic_emit_health.cpp`, and the limits
  `epistemic_health_pending`, `_pending_per_grant` and `epistemic_control_per_wake`.
- **Fix during completion.** The earlier partial work had rewritten the `Ingress` bookkeeping while
  moving it, which broke seven existing tests (relayed-refusal charging, content-refusal counters).
  The original bodies and comments were restored and only the new members were added.
- **Tests.** `tests/xuzz/vagent_epistemic_grants_test.cpp`: refused grants, duplicate ids, unknown
  actor, issuer as a bound or named source, token distinctness, wrong token, unnamed source and
  kind, revocation, the stub outcome, audit sink and a throwing sink, unknown receipt.
- **Result.** `vagent_actor_test`: 917 passed. `xuzz_test`: all failures (38) are E2E tests that
  need the `vpl`, `vprolog` and `vquery` binaries, which are absent because the full `make` stops at
  a link error in the TUI objects (`TerminalDevice` undefined in `build/vagent`'s link), a state
  that predates this slice.

Closed in the completion pass (no per-slice review, by the lead's instruction):

- **Behaviour fixed.** `healthAction` held the ingress lock while `sourceHealth` took it again for a
  reset, so every reset against a source with a profile deadlocked; the snapshot is now read before
  the lock. `wouldChangeHealth` read the in-flight set without the ingress lock in the turn path
  and now takes it. A committed reset raised no floor, because nothing ever wrote
  `ResetMark::committed`; `Ingress::requestReset`, `commitReset` and `supersededByReset` now hold
  the three steps, `finishHealthItem` commits a reset's generation when the turn commits, and the
  queued-reading check uses the predicate. `HealthAudit::actionSeq` is a per-actor counter starting
  at 1, taken for every audited outcome (a `NoChange` is not audited and takes none) and dropped
  with the actor.
- **Resume and upgrade.** A resume loses grants and their tokens, bindings and aliases, the outcome
  logs, the reset marks, the in-flight set, the estimator marks and the audit sequence (it restarts
  at 1 under the new instance label); it keeps what the arena holds, so a replay of the profile's
  last sequence is refused `StaleSequence` after a rebind and a bind below the profile's
  incarnation is refused (matrix 1-10). An in-process upgrade keeps the pid, so the grant and its
  token, the binding, the health outcome ring and the audit sequence stay. Both are tested.
- **Tests.** `vagent_epistemic_grants_test.cpp` grew from 10 to 22 cases: the post order at the
  `PostBox` (an isolate behind a held item, `putBack` keeping control first, which calls wake the
  reactor), the reset floor at the `Ingress` (the snapshot decides, nothing is superseded until a
  reset commits, a late commit does not lower the floor, the floor is erased with the actor), a
  revoked token queued ahead of its turn, a refused action spending no receipt, audit or allowance,
  quota exhaustion not keeping an isolate or reset out (12 of 16 slots for degrade and clear), the
  control cap deferring without dropping, `actionSeq`, the upgrade and the resume.
- **Fuzzer.** `fuzz_epistemic_ingress.cpp` has a health step: isolate, degrade, clear (with and
  without a code), reset and the isolate batch, each under a valid, a revoked and a wrong-kind
  grant, requiring no receipt for what the grant forbids, a typed outcome for what it allows, and no
  belief change from the stub. The harness has no `FuzzTarget` type; its header comment is the
  statement of what it checks and was updated.
- **Result.** `vagent_actor_test`: 929 passed. `xuzz_test`: 3098 passed, the E2E tests included,
  because the full `make` now links (it exited 0). `make fuzz_epistemic_ingress` built after one
  Makefile line added the shared TUI objects to `FUZZ_CORE_OBJS`, which every core-linking fuzz
  target lacked since the vagent TUI joined the core sources; 416 inputs in 60 s ran clean, with
  `ASAN_OPTIONS=alloc_dealloc_mismatch=0` for a libstdc++ `stable_sort` temporary-buffer report
  that is the toolchain's and not ours.
- **Limitations.** The reset floor is tested at the `Ingress` and not end to end, because the stub
  cannot commit a reset; 2c2 adds the end-to-end case when `healthEntry` can. The control cap is
  tested for "defers, does not drop" and not for the wake boundary, which the public API does not
  expose. A reset against a source with a profile always has weight or a reading to change, so the
  "idle reset is `NoChange`" case needs a profile with zero weight, which only 2c2's arena records
  can make. The `traceability.md` rows 4-18 and 8.2-4c are the lead's to close.

## I3-2c2: health records and the Vortex health turn

Implemented from the I3-2 design (revision 4) without a per-slice review, by the lead's instruction:
one full review follows the whole implementation.

- **Added.** `healthEntry` is real IR in `epistemic_emit_health.cpp` (`Gen::emitHealthEntry`),
  replacing the `HealthUnavailable` stub. It finds the principal's profile (an isolate or a degrade
  creates it for a bound source, using the bound incarnation the host now passes; any other kind
  without a profile ends `NoProfile`) and runs one of four bodies. Isolate latches `faulted`,
  records `fault_seq` and withdraws every support. Degrade adds an operator `degraded` reason,
  unique by (kind, code, origin), with at most `R - 1` operator reasons. Clear removes operator
  `degraded` reasons only, by code or all, never touches the latch, and refuses `NotOperatorReason`
  when the code is held by another origin. Reset checks `expect_reset_epoch`, clears the latch,
  reasons, `R`, `N`, the rejection streak and the two liveness stamps, adds the closed charges to
  the lifetime counters, withdraws supports, raises each stream's sequence floor to
  `min(in-flight maximum, last sequence + sequence_gap)` (creating the stream when absent) and
  increments `reset_epoch`. All four end in one shared audit-entry routine with the eviction rules
  of design 6.5: merge an ordinary entry equal in (kind, code, issuer), let a safety entry evict the
  oldest ordinary one, never evict the entry that is the live fault, and drop an ordinary entry
  that finds only safety entries.
- **`rtSourceDrop_`** takes a `cause` (`a3`) and stamps `retired_by` (`advance`, `isolate`,
  `reset`) on each record it retires; a record a newer reading had already superseded is left
  without it. Retire keeps its own walk and stamps `retire` only on a pinned record, because the
  others are excised and a stamp on them cost enough to push a 12-record retire past the grant the
  settle tests use. The drop is shared by advance, isolate and reset.
- **Ingestion** refuses `SourceFaulted` right after finding the profile (class `NeedsHostAction`,
  never charged), and writes the profile's `reset_epoch` into the new record's `calibration_epoch`
  (the host still writes 0, which stays when the profile was never reset).
- **`rtHealth_`** now reads the reasons: a `suspect` reason, else the fraction, decides SUSPECT; a
  `degraded` reason gives DEGRADED with its own code. The host `readSourceHealth` applies the same
  order and fills `reasons`, `resetEpoch`, `faultSeq`, the fault's code, issuer, grant and
  incarnation, and whether a liveness stamp is present.
- **Host.** `deliverHost` reads the snapshot from the state in hand (`sourceHealth` refuses a state
  that is in a turn, so the 2c1 `NoChange` pre-check never ran and every no-op cost a turn),
  passes `reasons`, `actions`, `incarnation`, the `floors` record from the in-flight snapshot and
  the bounded `removed` text (items of at most 64 bytes, 1,024 in all, ending `+N`), and settles the
  queued `-pred` events after a committed health turn exactly as after a source change, so a failing
  handler cannot undo an isolate or a reset. It also rechecks a queued ingestion against the
  committed reset mark before its turn and finishes it `SupersededByReset`, which 2c1 had only in
  `drainPosted`. `wouldChangeHealth` now knows reasons, the liveness stamps and the epoch.
- **Limits.** `vagent.epistemic_health_reasons` (default 3, range 2 to 16) and
  `vagent.epistemic_health_actions` (default 4, range 2 to 64) in `epistemic_limits.hpp`,
  `validate()`, `defaultSettingSpecs()` and `AgentSettings::fromModel`.
- **Tests.** `vagent_epistemic_health_test.cpp` gains `VagentEpistemicHealthActions` (14 cases):
  isolate before the first reading (sticks, refuses `SourceFaulted` after a resume, writes no
  counter and no cell), isolate drops supports and stamps `retired_by`, an advance stamps
  `advance`, latch/reasons/actions survive an advance, a retire and revive and a checkpoint,
  degrade unique and bounded, clear scope and `NotOperatorReason`, reset clears and keeps the right
  things and bumps the calibration epoch, a reset with a failing handler commits, a reset stamps
  what it withdraws, the idle reset is `NoChange`, repeated actions never evict the fault, a sensor
  cannot clear its own latch (4-20), and a 16-record reset against half the grant. The grants test
  has the end-to-end reset floor (a reading queued before a committed reset is superseded and its
  resend is `StaleSequence`; one queued after commits) and the tests that asserted
  `HealthUnavailable` now assert the real outcome. The ABI test pins the new names.
- **Fuel.** A reset of 16 records (16 keys, 16 `-at` handlers) costs 13,438 VM cycles including its
  16 settle turns, against a 5,000,000 grant (half the default): 0.27 percent of it.
- **Result.** `vagent_actor_test`: 943 passed. See the lead's report for `xuzz_test` and the full
  `make`.

State-version bump sites (R3-11; no version cell is written here). Each carries a comment naming
I4-1b, and I4-1b must bump the epistemic-state version at every one when it creates the cell:

1. `emitHealthEntry`, the isolate body (latch changes).
1. `emitHealthEntry`, the degrade body (reasons change).
1. `emitHealthEntry`, the clear body (reasons change).
1. `emitHealthEntry`, the reset body (latch, reasons and `reset_epoch` change).
1. `emitRtSourceDrop`, which withdraws supports for an advance, an isolate and a reset.
1. The retire tail in `emitSourceChange` (withdraws supports).
1. `emitRtHealthUpdate`, when the derived state class differs before and after (never on `R` or `N`
   drift inside a class).
1. The ingestion routine, where a reading supersedes a current record.

Not done in this slice, with the reason:

- The host latch cache (`Ingress::latched`, design 6.5 R3-13) is not built: the arena refuses a
  latched source's readings, so correctness does not depend on it, but a babbler still costs a turn
  per reading until it exists.
- The second admission hook (the headroom ceiling for exempt entries), the live-profile reserve and
  `health_cells` (design 6.6) are not built; nothing writes `health_cells`. Reasons, actions and
  `retired_by` are therefore uncounted by any reserve, and the tombstone budget tests of 6.8 are
  not written.
- `dropped_content` and `dropped_other` are passed as 0: splitting a coalesced charge at the reset
  call is not done, so the closed charges are not yet discarded or counted.
- The second half of the wedge test (after a reset one non-replacing reading per key commits) is
  not written, and `vagent_epistemic_resume_test.cpp` has no new case; the checkpoint and resume
  claims are tested in the health test.
- `wouldChange(Clear {code})` is true when any `degraded` reason holds the code, so a code held only
  by a device reason still runs the turn and answers `NotOperatorReason`, as the design wants.

## I3-2c3: latch cache, budgets, dropped charges

Implemented from the I3-2 design (revision 4) without a per-slice review, by the lead's instruction.
This closes the items 2c2 left out; what is still open is listed at the end.

- **Latch cache (6.5).** `Ingress::latched` is a set of (actor, principal) and `refusedWhileFaulted`
  a count, both host memory and both erased by `forget`. `observe` refuses `SourceFaulted` (a new
  `EpistemicRefusal`, class `NeedsHostAction`) right after the binding check, before the queue, and
  logs, charges and posts nothing. The set is filled by an arena `SourceFaulted` refusal reaching
  `finishIngest` and by a committed (or `NoChange`) isolate in `finishHealthItem`; a committed reset
  empties it and zeroes the count; an actor stop and an in-process upgrade empty it (a resume is a
  new pid, so it starts empty). `SourceHealth::refusedWhileFaulted` shows the count, and
  `sourceLatchCached` reads the set for tests and operators. The arena stays the authority: the
  cache only ever refuses what the arena would.
- **Dropped charges (5.4, 6.4).** `IngressCharge` carries the reset generation it was opened under
  and moved to `agent_executor_health.hpp`. A reset call that would change something detaches the
  open charge into `QueuedHealth::closed` (the split: a refusal after the call opens a fresh charge
  under the new generation); the turn passes its `content` and `other` as `dropped_content` and
  `dropped_other`, and the IR adds them to the lifetime counters, now creating the counter when an
  isolate-made profile has none. A reset that does not commit (`StaleHealthState`, `NoChange`,
  revoked) puts the charge back, merged into whatever opened since. A note already holding a charge
  of a lower generation than a committed reset runs as lifetime-only: `health`, `passed` and
  `contact` are dropped when it runs, so a note drained before the commit is caught too. The note
  record is now built when the note runs (`refusalNote`), not when it is drained.
- **Budgets (6.6).** The health turn writes `health_cells` (`kHealthFixedCells` 14 plus
  `kReasonCells` 16 and `kActionCells` 20 per live entry) before every commit. `epistemicReserve`
  sums `healthBound - health_cells` over live profiles and skips tombstones; `admitCells` is the one
  hook: ordinary entries (ingestion, bookkeeping, a degrade on a live profile) need census plus
  reserve within `cells`, exempt ones (isolate, clear, reset, any action on a tombstone, settle
  turns) need the census within `cells + cells / 16` (`kHeadroomDivisor`). The health slot gate is
  now `arenaSlots + slotReserve()`. A construction WARN prints `maxSources x (healthBound +
  kProfileBaseCells)` against `cells`; the checkpoint-limit relation is not warned because
  `kMaxEpistemicCheckpointCells` does not exist yet (see limits).
- **Incarnation bound.** The `max Int64` handed to `rtSourceDrop_` is a genuine numeric maximum
  (records below the bound are retired, so an isolate and a reset retire every boot's); it is not an
  absence marker. Both call sites now say why.
- **Tests.** Health test (+17): a babbler of 500 readings costs no cycles and leaves another source
  committing, set by isolate, cleared by reset (the next reading commits), a reset that commits
  nothing leaves it, set by an arena refusal after a resume, forgotten by an actor stop, a refused
  upgrade leaves latch and cache; three dropped-charge cases (call-time charge dropped with the new
  one kept, an earlier closed note dropped too and a queued reading superseded, a stale reset
  reopens its charge); `health_cells` per population and a full structure counted against the
  constants (129 cells measured at R = 3, A = 4 against the bound 142); reserve threshold (one cell
  short refuses, exact commits, a degrade still fits where a reading does not); an isolate and a
  reset land on a full budget; latched tombstones fill the headroom and then refuse
  `EpistemicBudget`; 400 bare tombstones leave a reading, a degrade and a checkpoint working; at the
  slot reserve isolate and reset run past the allowance and end `ArenaExhausted`. Settle test: the
  second half of the wedge test (after a reset one non-replacing reading per key commits and the
  next replacing one refuses again). Resume test: latch, reasons, audit entries, `action_seq` and
  `reset_epoch` cross a checkpoint and resume, the cache starts empty and the arena refusal sets
  it, and a retry under the old epoch is stale on the new host. ABI test pins the new names.
- **Result.** `vagent_actor_test`: 960 passed. `xuzz_test`: 3,128 passed, plus the E2E drag test,
  which failed once under load and passed alone (the known flake). The full `make` exited 0 and
  `make fuzz_epistemic_ingress` built.
- **Ambiguities and how they were read.** (1) `health_cells` is the worst-case size of the live
  entries, not the measured cells, so the reserve never undercounts what an entry can grow to; the
  census of a profile can only be below it. (2) A degrade on a tombstone draws on the headroom (the
  design says every action on a tombstone does). (3) A closed charge is detached at the call rather
  than copied into its note, because a copy would have to be undone when the reset fails; the
  reopened charge can reorder its effect on `R` and `N` against refusals decided in between, which
  is bounded by the per-source queue share and was already accepted for notes (R3-2). (4) The
  construction WARN covers one of the two capacity relations.
- **Limits.** The upgrade row of the cache table is tested only as a refusal: a latched profile
  leaves the arena non-empty, and the in-process upgrade needs it empty, so no upgrade that
  empties the cache can meet a latch today. `kMaxEpistemicCheckpointCells` and the "fill to the hard
  ceiling and checkpoint" test (6.6) do not exist; the full-budget checkpoint tests of I3-1 pass
  unchanged. The 100 Hz, eight-babbler acceptance figure (29 percent without the cache) is a
  throughput claim; the test shows the cause (no turn, no queue slot) and not the rate. The
  runbook test of 6.6 (revoke, isolate, drain, checkpoint, resume) is not written.
  I3-2 is complete apart from those three and the I4-1b version-bump sites listed under 2c2.

## I3-2a-r: the health fraction's `b` by series, parameterised cells, burst tests

The fix slice of I4-D1 section 12 (decisions 0.2, 2026-11-05: after T1, before I3-3). Written for
the ML milestone review. Nothing is committed by the slice worker.

### What changed

- **`b` and `1 - b` are T0** (I4-D1 3.9). `Gen::emitFractionDecay`
  (`vagent/runtime/epistemic_emit_health.cpp:610`) keeps `x = Div(-ln2, H)` and forms
  `em = expm1_series(x)`, `b = 1 + em`, `1 - b = Neg(em)`; `H = 1` branches to `b = 1 - b = 1/2`
  exactly. The series is `Gen::emitExpm1Series` (`:595`): the Horner form of 3.3, 18 terms, one
  `Mul` and one `Add` per term and a final `Mul` (36 opcodes with the start), coefficients `1/k!` as
  the nearest doubles (`kExpm1SeriesCoefficients`, `epistemic_limits.hpp:161`). The cap is still
  `Div(1, 1 - b)`; the rescale-then-update order is untouched.
- **The note mixture's `b^n`** (`emitFractionNote`, `:717`): `n = 1` takes `b` and `1 - b`
  themselves (`:736`); otherwise `n x > -1/2` takes the series (`:743`), else `Exp` and `1 - b^n` by
  `Sub` (`:750`). Only a note with `n ln2 / H >= 1/2` (`n >= 24` at `H = 32`) is still T1.
- **The host agrees bit for bit.** `healthDecay`/`healthWeightCap` (`epistemic_limits.hpp:202-217`)
  now compute the same sequence in C++ (`expm1Series`, `:182`; `healthDecayPair`, `:202`), so the
  `min_events` domain bound `floor(0.9 / (1 - b))` (`validate()`) and `sourceHealth`'s read-only
  rescale use the routine's `b`. This is C++ rather than Vortex because it is the host's own
  configuration check and read API, which run outside any VM; it mirrors, not replaces, the IR.
- **Parameterised** (I4-D1 section 12 item 1; mechanical). `FractionCells{hits, weight}`
  (`compiler_gen.hpp:68`) names the two state cells; `kHealthFractionCells` (`:74`) is today's
  `health_refused_weight`/`health_weight`. `emitHealthRescaled` (`:632`) and `emitHealthStore`
  (`:693`) take it; the update body is `emitFractionNote(profile, cells, k, c, halfLife)` and the
  read/accept/decision is
  `emitFractionRead(profile, cells, halfLife, accepted, minEvents, threshold, f, over)` (`:771`),
  which takes `b` (through `H`) and the evidence floor. The two internal routines `rtHealthUpdate_`
  (`:803`) and `rtHealth_` (`:824`) pass `kHealthFractionCells`; their ABI is unchanged. I4-8
  instantiates the same emitters with its gate cells (one internal routine per instance, or a
  routine taking the names as `Str` arguments; that choice is I4-8's).
- **Docs.** I3-2 3.2 carries a superseded note on `Exp(-ln2 / H)`; 3.3's claim now reads
  "independent refusals" with the burst table below.

### Measured

- **`b` against the reference.** The 10 `fraction_b` vectors (`H` from 1 to 10^6): `b` and `1 - b`
  bit-identical, and `healthWeightCap` is `1 / bits.one_minus_b` bit for bit.
- **`b` against libm.** Over every admissible `H` in [1, 10^6], the series `b` differs from
  `std::exp2(-1/H)` on this host (glibc, x86-64) by at most **1 ulp**.
- **The guards never fire on single events.** For every `H` in [1, 10^6], `fl(fl(b cap) + 1) <= cap`
  (also checked to `2^20` in Python). The update is monotone in `N`, so `N <= cap` after every
  single-event update, and `R <= N` by monotonicity of `b R + h` against `b N + 1`: the routine's
  two `Min` guards are inert outside a multi-event note, and the routine is `fraction_run`'s
  operation sequence exactly. (`fraction_run` has no guards; that is why they agree.)
- **The five `fraction_run` vectors through the IR** (one event per turn, `H` from the vector): `R`,
  `N` and `f` (`sourceHealth().refusalFraction`) bit-identical to `bits.*` in all five, including
  `H = 4096` over 600 events.
- **Bursts** (`H = 32`, `suspect_fraction` 0.25, `min_events` 8, 10^6 events after a 3H warm-up,
  bursts starting at rate `0.05/m` and merging on overlap as `i32a_burst.py` draws them, seed 19,
  `mt19937_64`; I4-D1's table, from numpy and another generator, in brackets):

| Bursts of | marginal rate | SUSPECT onsets per 10^6 | time in SUSPECT |
| --------- | ------------- | ----------------------- | --------------- |
| 1         | 0.0500        | 0 (0)                   | 0 (0)           |
| 4         | 0.0490        | 121 (126)               | 0.063% (0.07%)  |
| 8         | 0.0486        | 911 (970)               | 0.967% (0.96%)  |
| 16        | 0.0478        | 2,819 (2,868)           | 4.69% (4.9%)    |

- **Steady-state run length.** From near steady state (300 clean events after the bursts), 13
  consecutive refusals leave NOMINAL and the 14th reads SUSPECT, through the IR, as section 12 says.
- **Cost.** `MeasureTheCostOfAnIngestionAndABookkeepingTurn`: an ingestion 3,394 to 3,440 VM cycles
  (+46), a refusal and its bookkeeping turn 1,938 to 1,983 (+45); the emitted code grows the arena
  at launch from 27,421 to 28,037 slots (+616: the series is emitted inline three times, twice in
  `rtHealthUpdate_` and once in `rtHealth_`). Wall time of the 1,500-event health test is unchanged
  (6.3 s before, 6.0 s after).

### Tests (`tests/xuzz/vagent_epistemic_health_test.cpp`, all in `vagent_actor_test`)

- `Model` (`:63`) now forms `b^n` as the IR does and does one operation per statement, so it is an
  operation-for-operation mirror; the existing tests keep their 1e-9 tolerances and pass.
- New: `TheHostDecayIsTheFractionBVectorsBitForBit` (`:2157`),
  `TheCapBoundsTheUpdateAtEveryHalfLife` (`:2172`), `TheFractionRunMirrorIsTheReferenceBitForBit`
  (`:2199`; `FractionRun`, `:2130`, is `fraction_run` transliterated and checked against the five
  vectors' bits), `TheFractionRunVectorsRunThroughTheRoutine` (`:2211`),
  `BurstsAtFivePercentReachSuspect` (`:2261`; bounds are an order of magnitude and the ordering, not
  the table: m = 1 at most 5 onsets, m = 4 in [30, 500], m = 8 in [300, 3000], m = 16 in \[1000,
  8000\], strictly increasing), `BurstySequencesRunTheSameThroughTheRoutine` (`:2311`; bursts of 4
  and 8 at 5%, 800 events then 300 clean then 14 refusals, 1,114 events per burst size: `R`, `N`
  bits equal `FractionRun` after every event, the host state every event, the IR's stamp at every
  state change and every sixteenth reading; 10.7 s), `ABurstAsOneNoteTakesTheSeriesOrExp` (`:2368`;
  `k` = 2, 8, 23 on the series and 24, 30 on `Exp`, bit-equal to `Model` and within 1e-12 relative
  of `k` single refusals).
- **Result.** `vagent_actor_test`: 1,058 passed, 1 failed in the full run:
  `Translate.AFullBudgetResumesWithinThePeakAndNotInPlace` filled to 19,042 cells against the 22,118
  it requires while a fuzzer build ran in parallel; alone it passes three times out of three
  (stopping at `EpistemicBudget` with 22,156 cells), so it is load-sensitive and not this slice.
  `xuzz_test`: 3,233 passed; two timing benchmarks
  (`ArrayfiladeBenchmarkTest.Subscripting10000Cells`,
  `TranscopyrightBenchmarkTest.SeekableDecryptionLatencyUnder1ms`) failed under the same load and
  pass alone. `make -j` (everything) exited 0; `make fuzz_epistemic_ingress` built, and runs of 300
  (seed 2, complete) and about 600 (seed 1, stopped by a timeout under load) inputs found nothing,
  with `ASAN_OPTIONS=alloc_dealloc_mismatch=0`. The Python stand-in `check_vectors.py ... host`
  still passes (it is unchanged).

### Deviations and decisions

1. **`n = 1` shortcut in the mixture.** Section 12 says the mixture's `b^n` takes the series when
   `n ln2 / H < 1/2`. For `H >= 2` and `n = 1` that is the same bits as `b`. At `H = 1` it is not in
   the series' range and would call `Exp(-ln2)`; taking `b` for `n = 1` keeps a single refusal T0 at
   every `H` and makes `geo = (1 - b)/(1 - b) = 1` exactly, so a single-refusal note is
   `R <- b R + 1`, `N <- b N + 1` bit for bit, as `fraction_run` has it.
1. **The `Min` guards stay.** They are inert on single events (measured above) and only a
   multi-event note can reach them; removing them would be a behaviour change outside this slice.
1. **I4-D1 Q6.** The vectors and the checker already sit where 4.4 lists them and are already
   committed (`72ce6145`, with I4-D1 revision 2), so there is nothing to commit with this slice;
   settled as proposed. This slice is the first C++ consumer of `fraction_b` and `fraction_run`; the
   remaining routines' C++ test (`VagentEpistemicVectors`) is still I4-2's.
1. **The 5.3 rows.** With `b` T0, the "as built" guards of I4-D1 5.3 (`H * 2^-48` on the fraction,
   `4 db (H/ln2)^2` on `N >= min_events`) retire for single-event updates; a long note (`Exp`) still
   carries them. I4-D1 itself is not edited here; the lead may strike the "as built" clauses.

### Limits

- The series is emitted inline three times rather than as one internal routine; the +616 launch
  slots are the price, and a shared routine (as `decay` will want in I4-2) would cut them to about a
  third.
- Host and IR agree bit for bit only while the C++ compiler does not contract `y * acc + c` into an
  FMA. The host code puts each operation in its own statement, which stops clang's default
  `-ffp-contract=on`; GCC's default (`fast`) may contract across statements when FMA is enabled
  (`-march` with FMA). The build passes no `-march`, so x86-64 baseline has no FMA; a build that
  adds one should add `-ffp-contract=off` for these headers.
- The burst figures are from one seed of one generator; the test asserts bounds, not counts. The
  VM-level burst test sees few SUSPECT events (10 and 4 of 1,114), so the onset/clear edges it
  checks through the IR are few; the 13/14 edge is the deterministic one.
- The SUSPECT reading under bursts is still transient and latches nothing (3.4); the latch risk is
  I4-8's, repaired there by the slow pair (I4-D1 7.1, 7.2).
