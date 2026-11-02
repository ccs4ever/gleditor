# I3-2 design review, round 3: compiler and codebase expert (persona codebase)

Scope: `i3-2-design.md` revision 3 (uncommitted, 1,136 lines), `decisions.md` section 1,
`i3-2-review/findings-r2.md`, and my round-2 report `review-codebase-r2.md`, against P19 v1.5
sections 2.1, 2.2, 4, 5.1 and 8.2. Code at HEAD `8a4089d9` plus the uncommitted T0 change in the
working tree (`git diff -- apps tests`: `agent_executor.{hpp,cpp}`,
`vagent_epistemic_ingress_test.cpp`). Design citations are HEAD lines; I read them from
`git show HEAD:...` copies under `/tmp/p19-codebase/r3/`. Read-only: the only repository file
written besides the T0 report (`t0-review/review-codebase-r2.md`) is this one.

Method beyond reading. Four probes were compiled outside the repository and linked against the
objects the lead built (`/tmp/p19-codebase/r3/link.sh NAME`; sources in `/tmp/p19-codebase/t0/`),
all headless with isolated XDG:

- `tomb.cpp`: bind and retire 400 distinct principals on one actor.
- `cells.cpp`: what a fully active principal costs in epistemic cells.
- `held.cpp`: whether `state.hostEvents` can be non-empty at rest (T0 report N3).
- `spin.cpp`, `shed.cpp`: the T0 re-checks.

`/tmp/p19-codebase/r3/est3.py` re-derives the estimator numbers. Baseline: `vagent_actor_test`
passes 843 of 843 with the T0 change.

## Earlier findings

| Round-2 finding                           | Status in revision 3 | Why                                                                                                                                                      |
| ----------------------------------------- | -------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M1 settings plumbing                      | Resolved, one gap    | 3.6 names `fromModel`, a Float loader, types, domains, `kIngestOrder` and the note fields, `vagent_settings_test.cpp`. The new `validate()` rules: M4.   |
| M2 revived tombstone cannot be charged    | Resolved             | 3.7 makes `charge` find-or-create and keeps my probe sequence as the test.                                                                               |
| M3 reset epoch bumped at call time        | Resolved             | 5.4 `ResetMark {requested, committed}`: a reset that fails or is revoked supersedes nothing. The charge structure it relies on: M2 below.                |
| M4 new codes chargeable by default        | Resolved             | Section 7 adds `SourceFaulted`, `SupersededByReset`, `SettleFailed` to the `chargeable` exclusions; `health` count on `Charge` and the note.             |
| M5 settle contract incomplete             | Resolved             | 4.1: outcome stays `Committed` and raises the floor; settle and discard skip `admit`; slot reserve; settle runs inline in `deliverHost`; stat key named. |
| m1 test arithmetic for the estimator      | Resolved             | 3.8 and 9: one seeded run, an aggregate after a 3H warm-up. I reproduced the 3.3 table (below).                                                          |
| m2 charging lag                           | Resolved             | 3.2 bounds the lag by the per-source queue share (8): `b^-8 = 1.19` at H 32, at most 2 for any valid H.                                                  |
| m3 shape of `rtSourceDrop`                | Resolved             | 4.2: one parameter (`incBelow`), retire keeps its tail, the I3-1c tests and the resume test gate it.                                                     |
| m4 the stamp                              | Resolved             | 3.4: the stamp follows this event's update, is not replay-deterministic, `FAULTED` kept for restored data.                                               |
| m5 grant constants and token              | Resolved             | 5.2: `RAND_bytes`, fail closed, SHA-256 digest compared with `CRYPTO_memcmp`, 16 grants, 24 hours.                                                       |
| m6 `program.cpp` remap and format version | Resolved             | Section 7: remap and `sound` list both updated; `kProgramFormat` becomes `-4`. The tests that pin `-3`: m3 below.                                        |
| m7 "bounded by bindings" is not a bound   | Resolved in intent   | 6.6 caps exempt creation at `2 x max_sources`. It does not bound tombstones; M3.                                                                         |
| m8 self-clear checked one way             | Resolved             | 5.2 checks at `grantHealth`, at `bindSource` and revive, and at turn time.                                                                               |
| m9 list removal                           | Resolved             | 6.8 tests first, middle and last position and the actions eviction.                                                                                      |
| m10 harness changes                       | Resolved             | 6.8 extends `ProfileHealth` and `EpistemicSnapshot`, adds `decayed()`; 5.6 injects `AgentClock::steady`.                                                 |
| m11 citations moved                       | Resolved             | The design cites HEAD lines and names; I checked all of them (below).                                                                                    |
| D1 reconciler upgrade sheds profiles      | Resolved (T0)        | `upgradeRefusal` with an `UpgradeRoute`; T0 report r2 M1. Section 13's last bullet misstates it: m2.                                                     |
| D2 control posts do not wake the reactor  | Resolved (T0)        | `pushControl` signals. `putBack` is deliberately silent; the design says it signals: m1.                                                                 |
| Owner Q2 reset clears liveness stamps     | Resolved             | 6.4 clears `last_receive` and `last_contact` and says so in the entry.                                                                                   |
| Fit to I3-3 and I4                        | Resolved             | Section 13 records the `last_receive` meaning, origin-keyed uniqueness, the channel for the three settings, the calibration reset inside one turn.       |

## Citations: what I checked

Every `path:line` in the design resolves at HEAD to the construct named, with these exceptions:

- `vagent_epistemic_ingress_test.cpp:989`, `:995` (3.8) are working-tree lines; at HEAD they are
  `:988` and `:994` (T0 added `#include <poll.h>`). Cite the tests by name.
- Section 1 says T0 shifts `agent_executor.cpp` by "up to 38 lines"; the diff shifts the end of the
  file by 36.
- Section 3.1 gives `compiler.cpp:5369` for the epoch regression and `:5375` for the mapping
  regression. Both are right (`Lt epoch` then `Lt map`), and `:5554`, `:5565` are the order checks.

Constants and facts verified against the tree: `kObservationOrder` has 23 entries
(`runtime_abi.hpp:602`), `kIngestOrder` 4 (`compiler.cpp:5264`), `arenaSlots` default 2^30 so
`arenaSlots / 64 = 16,777,216`; `cells / 16 = 1,536`; `kMaxObservationNameBytes = 256`;
`Ingress::wallSynchronised` exists (`:767`); `clockInstance` is 16 hex digits (`:761-768`);
`ArenaManifold::link/unlink` at `arena_manifold.hpp:635-648`; `HostBindings` holds no
`AgentExecutor` and `bindSource`, `retireSource` are called nowhere outside the executor and tests;
`OpenSSL` is already linked (`-lssl -lcrypto` in the build log).

Numbers I computed (`est3.py`): the 3.3 table reproduces to the digit (H 16/32/64 at 5 to 50%,
including 12.0% and 132/h at 20% and H 32); `b = 0.978572`, `1/(1-b) = 46.668`,
`floor(0.9/(1-b)) = 42`; `N` after 8 and 9 events is 7.425 and 8.266; the start-up false-alarm rates
are 1.87% and 10.7% at `min_events` 8 and 12.9% and 29.9% at 3; from a saturated clean history a
source refusing every reading reads SUSPECT after 14 events and one refusing alternately after 31 or
32; recovery takes 64 good readings; one early refusal never exceeds `f = 0.102`; the order bounds
`b^-m` are 1.09, 1.19 and 2.0 at m 4, 8 and 32. The cell prices: a fully active principal at HEAD
costs **1,315** cells, 115 after the first reading and 80 per further record (`cells.cpp`), matching
"about 1,320". I did not reproduce the relay-share means or the stalled-pump peaks of 3.2.

## What is sound

- **The slice split and the stub turn.** 2a changes only the profile, the bookkeeping note and the
  read API. 2b removes `rtSettle_` from two tails. 2c1 is host-only against a stub entry that ends
  `HealthUnavailable`, which the executor already treats as a refusal: any typed result other than
  `Committed` is undone (`refusal = code != kTurnResultCommitted`, `agent_executor.cpp:2204`).
- **A refused turn costs no net cells.** `runTurn` releases the mark on a refusal (`:2239`), so
  `SourceFaulted` refused first in the ingestion routine allocates nothing, which is what F23 relies
  on. Because the turn is undone, an IR-decided refusal cannot update `R` and `N` in its own turn,
  so charging `ClockEpochRegression` after the turn (3.2) is the only workable design.
- **The estimator needs no new opcode and no libm in the read path.** The update uses `Add`, `Sub`,
  `Mul`, `Div`, `Min` and one `Exp` (`vortex_vm.cpp:454-523`, `:922-945`); the state rule and the
  C++ `sourceHealth` use only compare and divide, so the two derivations agree bit for bit.
- **The settle entry is expressible.** `rtSettle_` already detaches `d.pending` per round
  (`compiler.cpp:5684-5722`) and `failNow`s past `belief_event_depth`, so "put the head alone on
  `d.pending`, call it, re-attach the rest" is a small wrapper; the discard by one
  `ArenaManifold::link` call (the call evicts what either end held) cannot fail on a ref read from
  the same arena.
- **The reset generation scheme is correct.** Walking two overlapping resets, a failed first and a
  committed second, `committed = requested-at-call` supersedes exactly the items older than the
  committed call.
- **The profile cell prices.** The 2-cells-a-field accounting reproduces `kHealthFixedCells = 14` (4
  scalars = 8, two headers with a field each = 6), the 142-cell bound at R 3 and A 4, and the
  1,522-cell principal.

## BLOCKING

None.

## MAJOR

### M1. `Logged::counted` breaks the pinned relay rule for lifetime counters

Problem. 3.2 says `Logged` "gains `counted`, set when any of the three has updated the estimator;
`firstCharge` returns false for a counted event." Today `firstCharge` (`agent_executor.cpp:800-813`)
exists to decide the lifetime counters and `last_contact`, and it deliberately lets a direct copy be
charged after a relayed copy of the same event (`chargedDirect`, `:718`: "a relayed copy charged
first must not hide the device's own"). Under 3.1 a relayed copy of a signed-field refusal is a
health refusal, so it sets `counted`; the direct copy that follows then meets `firstCharge == false`
and is not charged at all.

Evidence. `VagentEpistemicIngress.ARelayedRefusalDoesNotHideTheDirectCopy`
(`vagent_epistemic_ingress_test.cpp:1316-1330`): a relayed frame mismatch `e2`, then the direct
copy; the test expects `content_refusals == 1` after the direct copy. With the rule as written it
stays 0 and `last_contact` is never set, which `ADirectContentRefusalChargesHealthAndContact`
(`:785`) and `OnlyADirectEventRefreshesContact` (`:551`) say a direct content refusal must set. The
device demonstrably sent the bytes; a relay first must not erase that evidence.

Consequence. The rule makes `content_refusals` and `last_contact` depend on which copy arrived
first. I3-3 reads `last_contact` for liveness, so a device behind a fast relay would look silent.
The pinned test fails the day 2a is written to the text.

Required fix. State that `counted` gates only the estimator's `health` and `passed` increments, in a
separate function (for example `firstCount(source, incarnation, eventId)`), and that `firstCharge`
keeps its present meaning for `content`, `other` and `contact`. `Charge` then carries `health` and
`passed` as increments the host has already de-duplicated; the lifetime fields are charged exactly
as today. Add the relayed-first-then-direct sequence to the 2a tests with the expectation that
`content_refusals` is 1, `last_contact` is set, and `R` and `N` moved once.

### M2. A note cannot own more than one charge, so the ordering rule has no structure

Problem. 3.2 resolves the order defect (F2) by closing a principal's open `Charge` when `observe`
queues an accept, so the next refusal opens a new charge and a note queued after the accept. 5.4
keys `charges` by (actor, principal, generation) and 6.4 has a committed reset "erase the closed
charges". At HEAD a note carries nothing but the principal (`PostBox::Item::refusalsOf`,
`postRefusals` `:3820-3826`) and reads the single `Charge` for `{pid, principal}` when it is drained
(`:3917-3925`: `charges.find`, `move`, `erase`); `abandon` erases the same key (`:3854`). A
generation in the key does not help: two charges of one principal in one generation, one closed and
awaiting its note and one open, share the key.

Evidence. The code above. The first note to drain takes whatever the map holds for the key,
including refusals decided after the accept that was meant to separate them; the second note then
finds no entry and `continue`s (`:3921-3923`). The ordering the design claims for `observe`-decided
refusals ("keep queue order exactly") is therefore not obtained.

Consequence. The F2 and F19 resolutions both rest on a structure the design does not define, and the
natural reading (extend the map key) does not work. A worker will invent one; the reviewers cannot
check it.

Required fix. Say that closing moves the `Charge` into the note: `PostBox::Item` gains
`std::optional<Charge> charge` (its generation inside it), the open charge in `Ingress::charges`
stays one per (actor, principal), and the drain uses the note's own charge instead of looking it up.
A reset's commit then removes closed charges by walking the post box and `state.hostEvents` for
notes whose generation is below the committed one and adds their counts to the lifetime counters
(m17 of round 2). `abandon` reads the charge off the item. List the sites in 5.4's table (the note
branch of `drainPosted`, `abandon`, `dropHostEvents`) and add a test: refuse, accept, refuse for one
principal posts two notes whose counts are 1 and 1, in queue order.

### M3. Charging a health reserve to every profile makes tombstones unbounded in cost

Problem. 6.6 charges "the reserve per existing profile, tombstones included":
`cellsWithin(all) + sum over profiles of (healthBound - health_cells) <= cells`. Nothing bounds the
number of tombstones. `retireSource` of an unseen principal mints one, `emitProfileFor` counts only
live principals against `max_sources` (`compiler.cpp:4691-4730`), and a tombstone is kept for ever
to hold its replay floor. The design's `2 x max_sources` cap is on profiles an exempt path creates
(isolate), not on retirements.

Evidence (`/tmp/p19-codebase/t0/tomb.cpp`: bind and retire 400 distinct principals on one actor):

```text
bound=400 retired=400 epistemicCells=2800
```

A tombstone is 7 cells at HEAD, so 400 fit in 2,800. With 3.8's reserve each costs 7 + 142 = 149, so
**165 tombstones** (24,576 / 149 = 164.9) make every ordinary ingestion refuse `EpistemicBudget` for
the life of the actor, and `validate()` does not see it (it bounds `max_sources`, not tombstones). A
fleet that rotates keys, one new principal per boot, reaches it after 165 rotations.

Consequence. The F27 reserve was meant to guarantee that a live profile can reach its health bound.
Charged to tombstones it instead lets retirement history consume the whole budget, a regression from
today's behaviour where 3,500 tombstones still fit.

Required fix. Charge the reserve only to profiles that can still be acted on and read: live
(non-retired) profiles, plus a tombstone that already holds health structures (its actual cells are
in the census through `health_cells`). A bare tombstone costs only its 7 cells. State a bound on
tombstones as well (for example `retireSource` of an unseen principal past `2 x max_sources`
tombstones refuses `EpistemicBudget`, or the oldest bare unlatched tombstone is evicted with its
floor moved into a host-side floor table; pick one and price the replay risk). Add a 2c2 test:
retire 400 principals, then ingest from a live source and expect `Committed`.

### M4. The new `validate()` rules reject configurations the tree uses and throw at construction

Problem. Section 7 adds to `EpistemicLimits::validate()`:
`cells + cells/16 <= kMaxEpistemicCheckpointCells` (28,672),
`maxSources x (healthBound + kProfileBaseCells) <= cells`, and (4.1) a slot reserve of at least
4,096. `validate()` is not only the settings loader's fallback test. `AgentExecutor`'s constructor
and `runtimeOptions` throw `std::invalid_argument("invalid epistemic admission limits")` when it
fails (`agent_executor.cpp:978`, `:1014` at HEAD;
`VagentEpistemicAbi.InvalidHostLimitsAreRefusedBeforeActorCreation` pins that).

Evidence. Configurations in the tree that the new rules refuse:

| Where                                    | Setting                                 | New rule that fails                                  |
| ---------------------------------------- | --------------------------------------- | ---------------------------------------------------- |
| `vagent_epistemic_ingress_test.cpp:649`  | `cells = 20`, default `max_sources` 128 | 128 x 186 = 23,808 > 20                              |
| `vagent_epistemic_ingress_test.cpp:893`  | `cells = 10`                            | same                                                 |
| `vagent_epistemic_ingress_test.cpp:1046` | `arenaSlots = 1`                        | reserve 0 < 4,096                                    |
| `vagent_settings_test.cpp:143-175`       | `cells = 30000`, `max_sources = 256`    | 30,000 + 1,875 > 28,672; 256 x 186 = 47,616 > 30,000 |

The first three would throw at `LocalHost` construction; the fourth falls back to the defaults for
the whole block, so `max_sources` reads 128, not 256. More generally the two rules together cap
`max_sources` at **145** (cells \<= 26,985 from the first, 26,985 / 186 = 145.08) with the default
`R = 3`, `A = 4`, and at 16 if an operator raises `epistemic_health_actions` to 64
(`healthBound = 1,550`). Today any `max_sources` is valid. None of this is in section 11 or in the
2a-2c2 test tables.

Consequence. Four existing tests fail or crash, and a legal deployment (256 sources, a larger
ceiling) silently loses its configuration. The checkpoint ceiling constant 28,672 is also a guess:
the tree's only measurement is the comment at `epistemic_limits.hpp:33-35` ("24k cells checkpoint
and 32k do not"); 26,112, the default hard ceiling, is between them and unmeasured until the 2c2
test runs.

Required fix. Keep `validate()` to per-field domains and the relations that cannot be exercised by a
legitimate small configuration (`healthPendingPerGrant <= healthPending`,
`H >= ingressQueuePerSource`, `min_events` cap). Move the two capacity relations to a
construction-time `GLEDITOR_LOG_WARN` plus the admission arithmetic, which already refuses ingestion
when the reserve does not fit, and say so in 6.6. If the lead prefers hard validation, record the
`max_sources <= 145` consequence as a departure in section 11, change the four tests, and let the
loader's WARN name the relation that failed.

## MINOR

- **m1. The records say `putBack` signals.** Design section 1 ("`PostBox::signalLocked` is called
  from `push`, `pushControl` and `putBack`"), section 12 F32 ("every enqueue signals the reactor"),
  5.4 and `remaining-plan.md` T0 and X-27 describe the first T0 revision. After the T0 review fixes
  `putBack` is silent on purpose (T0 report r2 M2): a kept post signalled when it was posted, and a
  signal there makes the reactor spin on a post that stays kept. State "`push` and `pushControl`
  signal; `putBack` does not". The same fix added a second abandon path in `drainPosted` (an
  unprepared actor the runtime has stopped); the 5.4 site table row for `abandon` should name it.
- **m2. Section 13's last bullet is wrong after T0.** "Both overloads already refuse populated roots
  until a translator is registered (T0)." The schema overload refuses until a translator is
  registered; the reconciler overload refuses **always**, because it never calls the translator and
  `makeStateReconciler` has no `trust` or `epistemic` dimension. T1 must either teach the reconciler
  the two roots or keep refusing, and the translator tests need both routes.
- **m3. Existing tests the reclassification changes are not listed.** 3.1 moves a mistyped tuple, a
  non-finite value and a non-finite `acquiredAt` from `InvalidObservation` to `InvalidValue` and
  `InvalidAcquisition`. The tests that expect the old code are
  `vagent_epistemic_ingress_test.cpp:276` (`wrongType`), `:283-284` (`nonFinite`) and `:302`
  (`bad.acquiredAt = NaN`); the fuzz harness treats `InvalidObservation` as the one refusal the
  executor answers "by return value only, without logging"
  (`tests/fuzz/fuzz_epistemic_ingress.cpp:237`, `:323`), so it needs the two new codes or
  `make fuzz` fails. `kProgramFormat` `-4` also changes tests that pin `-3`:
  `vagent_live_upgrade_test.cpp:83-87` and `vagent_swarm_packaging_test.cpp:142`, `:189`, `:348`.
  Add all of them to the 2a table and the fuzz file to the 2a file list (3.8 lists the fuzz file
  only under 2c2).
- **m4. "No two hosts can differ by an ulp" (3.2) is false.** Moving `b = 2^(-1/H)` into the IR
  removes the C++/IR split, but the VM's `Exp` is `std::exp` (`vortex_vm.cpp:939`), a libm call
  whose last bit varies across C libraries. `R` and `N` therefore agree across hosts only to a few
  ulps per update, and a reading whose `f` sits within an ulp of `suspect_fraction` can flip state
  on another host. This is consistent with the lead's D1 tolerance rule and with the stamp being
  "not replay deterministic" (3.4); say "within a few ulps" and do not promise bit equality.
- **m5. Settings details.** (a) `EpistemicLimits` is aggregate-initialised positionally in
  `checkedEpistemicLimits` (`epistemic_limits.hpp:133`) and `EpistemicLimits{0, 1}` in
  `vagent_epistemic_abi_test.cpp:60`; append the new fields after `arenaSlots`. (b) `loadPositive`
  skips a non-positive value without reaching `validate()` (`agent_executor.cpp:5624-5630`), so the
  new WARN fires only for a positive value that fails `validate()`; say so. (c) The new
  `defaultSettingSpecs` entries need `expectedTypes` `integer` or `float`; a `float` default of
  `0.25` is already a supported shape (`system_docs.cpp:584`).
- **m6. Slot reserve.** `validate()` should bound `arenaSlots + arenaSlots / 64` by the arena's own
  allocation limit, `ephemeralBit` = 2^31 (`manifold.hpp:154`, `arena_manifold.hpp:840`, enforced at
  `arena_manifold.cpp:1190`); `arenaSlots` is unbounded above today, so the reserve can push a large
  setting past the limit, where the arena throws mid-turn. Drop the "at least 4,096" floor (M4) or
  state that tests set the reserve explicitly.
- **m7. Exempt entries have no admission hook.** `spec.admit` is attached only to ingestion and
  bookkeeping turns (`agent_executor.cpp:1865-1869`), and it runs after the routine, on net cells
  (`:2204-2209`). 6.6's "exempt entries run until `cellsWithin(all) <= cells + cells/16`" and the
  fuzz invariant "used cells stay within `cells + cells/16`" need a second `admit` with the headroom
  ceiling on `Isolate`, `Clear`, `Reset` and settle turns; list it in 6.8's file row for
  `deliverHost`.
- **m8. `Ingress::everBound` grows without a bound.** It records every principal ever bound on any
  actor since the executor started (5.2). Each entry is a 64-byte string, so growth is slow, but
  `bindSource` has no cap and the set is never erased. Cap it or erase entries when the last actor
  that bound a principal is forgotten, or document the growth.
- **m9. The observation record writer is not in the 3.8 file table.** `health_fraction` and
  `calibration_epoch` are written by the IR into cells that the host created: `observe` builds the
  record in `kObservationOrder` and compares the order position by position
  (`agent_executor.cpp:3694-3725`), and the routine reads fields through `readInOrder`
  (`compiler.cpp:5227`). 2a must add `r.set(kObsHealthFraction, real(0.0))` at the right position
  and 2c2 must add `calibration_epoch` to the routine's read list; list both under
  `agent_executor.cpp` and `compiler.cpp`.
- **m10. Reasons from a device need no grant.** 6.1 gives every reason a `grant` and `issuer` field.
  State that they are absent for `device`-origin reasons (I3-3), whose provenance is the source's
  own report; otherwise the I3-3 path must invent a grant ID.
- **m11. The plan and 3.6 disagree.** `remaining-plan.md` T0 (X-7) has `AgentSettings::fromModel`
  return `std::expected` instead of falling back to defaults; 3.6 and decision F30(A) keep the
  whole-block fallback with a WARN. The T0 change in the tree does neither. Align the plan with
  F30(A) or record the later change.

## Questions for the owner

1. **Tombstones (M3).** Should a bare tombstone ever hold a health reserve? I recommend no: the
   reserve belongs to a profile that can be acted on, and an operator isolates a bound principal. If
   an operator should be able to isolate a retired principal's tombstone, that action creates the
   structures (counted in the census) through the exempt path.
1. **Capacity relations (M4).** Is `max_sources <= 145` an acceptable ceiling for a store that
   currently allows any value? I recommend a warning and the admission arithmetic, not a refusal.

## Fit to later iterations

Nothing in the design makes I3-3 or I4 unsound. Two additions: I3-3's `last_contact` reads depend on
M1 being fixed (a relayed first copy must not hide the direct one), and T1's translator tests must
name both upgrade routes (m2). Section 13 already carries the rest of my round-2 list.

## Verdict by slice

- **2a: FAIL** (M1, M2).
- **2b: PASS** (m6 and the 4,096 floor in M4 are the only items that touch it; both are one-line).
- **2c1: PASS** (the charge structure it stamps with a generation is M2's, settled in 2a first).
- **2c2: FAIL** (M3, M4).

Tally: 0 BLOCKING, 4 MAJOR, 11 MINOR.

Verdict: FAIL
