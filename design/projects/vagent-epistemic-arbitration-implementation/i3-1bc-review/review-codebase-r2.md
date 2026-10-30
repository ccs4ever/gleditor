# I3-1b/1c review, round 2: vagent compiler and codebase persona

Scope: review head `d48e85f8` (adds `0124b0d2`: `max_uncertainty`, I3-1d restore; `e91e8fe8` is the
round-1 fix commit), diffs `00db472b..d48e85f8` for the files named in the brief, against i3-design
sections 3-6, 8, 9, 11, 13 and 14. Method: read the fix commit and the emitted-IR generators,
re-ran `VagentEpistemic*` (92 tests pass, 11 s) and built throwaway probes in /tmp/i3-1bc-codebase
(`probe2.cpp`, compiled with the repo flags, linked against the existing objects; no repo file
touched).

## Probe results

- **Slots per ingestion (M1):** probe `Slots`, 60 committed observations of one key: 287 slots for
  the first, 242 for each later one (round 1: 667). Section 14's "about 250" is right.
  2^30 / 242 is 51 days at 1 Hz in aggregate, as section 9 says.
- **Cycles per ingestion:** 4,037 to 4,296 on a small state (round 1: 12,325), matching the
  claimed 4,362. They do not stay there, see MAJOR 1.
- **ArenaExhausted producer:** exists (agent_executor.cpp deliverHost, `cellCount() >= arenaSlots`
  before the turn), logs the outcome via `finishIngest`, releases the queue slots, is not
  chargeable, and class NeedsHostAction. Test `PastTheArenaAllowanceIngestionRefusesExhausted`
  passes.
- **Failed-turn class (M2):** probe from round 1 (`maxInstructions = 3000`) is pinned by test
  `...TurnExhausted` (ingress_test.cpp ~975): code `TurnExhausted`, NeedsHostAction.
- **noCell and sentinels:** `git diff -U0 00db472b..d48e85f8 -- apps tests` adds no `noCell`.
  New absences are `std::optional` (`SourceChange::incarnation`, `Charge::contact`, `highest`).
  Residuals under MINOR 1.
- **Incarnation advance with changing keys:** probes `AdvanceGrowth` and `Wedge`, MAJOR 1.

## BLOCKING

None.

## Round-1 findings

- **M1 physical slot price: fixed.** Price cut to 242 (record adoption, `putScalar`, positional
  reads), re-measured in section 9, `ArenaExhausted` produced and tested. The remaining cost
  (record about 80, handler frames about 85) is documented as compiler-wide.
- **M2 failed-turn classification: fixed.** `deliverHost` maps `kFailCycles`/`kFailFuel` to
  `TurnExhausted` and anything else to `IngestionFault`; `UnboundIncarnation` is now
  needs-host-action in both design (section 14) and `outcomeClassOf`, and the design text was
  changed to match. One residual, MINOR 3.

## New emitted IR: review

- **Adoption of the observation.** `breakOp(D(obsName), values)` then `rec = obs`
  (compiler.cpp ~5154). `obsName` is the name cell from `rtFind` (`r1_`, compiler.cpp:1538),
  copied into a global right after the call, so no later `rtFind` clobbers it. Nothing reads
  `$turn.observation` afterwards: the only readers of `kTurnObservation` are the executor writer
  (agent_executor.cpp:3732) and the one `rtFind` at the top of `emitObserve`. A refusal after the
  `breakOp` is undone by `arena.release(m0)`: the trail saves older slots before every write, so
  both the unlink and every in-place `mov(D(x), ...)` revert. Verified in
  arena_manifold.hpp:150-180, 750-760. Correct.
- **`readInOrder` / `readAt` against `kObservationOrder`.** `readInOrder` checks each name, so an
  executor/compiler divergence is not silent on ingestion: the turn ends `IngestionFault` (a defect
  code, correctly uncharged). `readAt` checks nothing: it indexes `observationPosition(name)`
  steps from the marker (position i is the (i+1)th advance, consistent with `readInOrder`'s
  marker-then-names walk). It is safe only because every record on `d.epistemic` passed
  `readInOrder` at its own ingestion. See MINOR 2: the two orders are written in two places with
  nothing pinning them together at compile time, and `readAt` on a record from another build
  (a restored checkpoint) misreads silently. A short chain makes `rd(D(0), ...)` run on cell 0
  (no zero guard in the walk).
- **Optional relay loop.** After the 23 required fields the walk scans the trailing fields for
  `relayed_by`; at most about 11 steps. Correct; it returns 0 when absent (VM encoding, allowed).
- **`putScalar`.** Reads the existing value cell with `rtFind`, writes in place, else appends. Used
  only for scalars (last_refusal, last_contact, last_receive, retired, incarnation); safe because
  those value cells are owned by one field (created by `callSetField`, never shared). In place writes
  are trailed, so a refused turn restores them. The monotone `last_contact` guard in
  `emitObserveRefusal` is correct (`Lte contact, last` skips; `branchIfZero(last, fresh)` first).
- **Stream sub-record.** Per (profile, predicate) record (sequence, epoch, mapping), read by
  `readInOrder` with `kStreamOrder`, written once in `first` in the same order, updated in place
  afterwards. Checks: stale sequence, then epoch regress, then mapping regress within an epoch;
  only a higher epoch passes without the mapping test. Order of the checks is right (a duplicate is
  caught earlier at the dedup list). All writes are in `bookkeeping`, ahead of later refusals, but
  undone by the trail.
- **Dedup positional reads.** Entry is marker, incarnation, event ID, digest, minted in that order a
  few lines below; the three `nextField` steps match. No zero guard, but entries are only minted
  here. Correct.
- **Key-limit current scan.** Counts a support while it has any record with status `current`
  (`readAt` of position 0). The new record is already appended, so the support being written counts
  itself. Correct; but it walks every clause and every source of the predicate, see MAJOR 1.
- **Revive budget loop (emitSourceChange).** Counts non-tombstoned profiles, refuses
  `EpistemicBudget` at `maxSources`. Right, but see MINOR 4 for what the host does with the
  refusal.
- **Optional incarnation.** `find(change, kObsIncarnation, inc)` outside the required loop;
  absent is VM 0, tested by `branchIfZero(inc, ...)` at all three uses (absent profile, advance,
  retire). A retire of a principal no binding names creates no tombstone. Correct.
- **Vortex rule.** Ingestion, bookkeeping and source-change run in emitted IR on the VM. The C++
  that remains is host authorization (bindings, floors, queue shares), transport and the admission
  walk; the last has no justification comment, MINOR 5.

## MAJOR

1. **An incarnation advance never reclaims the predecessor's supports, so a device that reboots
   with changing keys wedges the whole actor and slows every ingestion without bound.**
   compiler.cpp emitSourceChange `advance` (~4640-4660) only sets the predecessor's records to
   `retired` and gives the profile fresh windows. The supports (clauses with their sources) stay on
   the predicate, still counted in the cell ceiling, and `history` trimming runs only when a new
   record lands on the same support, so a key that is never reused is never trimmed. Probe `Wedge`:
   one principal, 4 keys per boot, each boot's keys new (`k<boot>_<n>`, as with tracked object
   IDs), default limits. Epistemic cells go 2,911 at boot 10, 5,751 at 20, 8,591 at 30 (about 284
   per boot) and at **boot 86 the ceiling (24,576) is reached: every ingestion on the actor,
   including a second, honest principal's, refuses `EpistemicBudget`**, which clears only after
   `retireSource` of the noisy one. Cost grows on the way: VM cycles for one ingestion go 4.2k
   (boot 2), 18.9k (boot ~17), 47k (boot ~22), 80k (boot ~86), because the key-limit scan
   (compiler.cpp ~5215-5250) and the `d.epistemic` tally walk every clause and source of the
   predicate; wall time for one ingestion is 4.2 ms at boot 2, 22 ms at boot 40, 42 ms at boot 80
   (the C++ admission walk over up to 24k cells adds to it). A crash-looping device or a host
   that rebinds a flapping boot counter does this in minutes at 1 Hz, and it is the actor-wide
   denial of service that the per-source `max_sources`, queue shares and outcome-log fixes were
   meant to rule out. Section 14 records only that a retired-only support "is still a belief" (I4's
   question), not the budget effect, and the I3-1d resume test fills the ceiling with live keys, not
   with this.
   Remedy: on advance, drop the predecessor's supports that have no current record after
   retirement, or at least trim them to a small retained window (`rtBDrop` plus the unlink the
   retire path already has, and the clone views); alternatively count the retired-only supports
   against `keys_per_source` per predicate and trim the oldest. Add a test that 200 boots with new
   keys keep `epistemicCells()` bounded and leave a second principal ingestible.

## MINOR

1. **Reserved values for absence still present (owner's rule).** Fixed since round 1: `highest` is
   `optional`, the `Logged{eventId = {}}` revocation entry is gone, and no `noCell` was added.
   Remaining: `IngestOutcome::code` (epistemic_ingress.hpp:110, "empty unless Refused") and
   `Ingress::Charge::code` (a charge exists only after a code is set, so it can simply be
   required); `FrameBody` fields `frame/make/enter/result` default to `{}`, that is CellRef 0
   (compiler.hpp:70-76); `ArbitrationPolicy::maxUncertaintyMs{}` defaults to 0, which a
   hand-built policy then reads as "refuse every reading with uncertainty above zero" (the parser
   resolves an absent key to `max_age`, but the default-constructed struct does not).
2. **The two field orders are unpinned.** `kObservationOrder` (runtime_abi.hpp:599) and the
   `r.set(...)` sequence in `observe` (agent_executor.cpp ~3452-3500) are maintained separately.
   `readInOrder` fails loudly if they diverge (every ingestion `IngestionFault`), but only in a
   test run. Build the record from the array (iterate `kObservationOrder` with a name-to-value
   switch) or add a unit test that walks a written record against the array; add a one-cell
   guard to `readAt` (`branchIfZero(f, malformed)`) so a short or foreign chain faults instead of
   reading cell 0.
3. **`catch (const std::exception &)` maps every exception to `kFailFuel`** (runTurnOnce ~2080),
   so a `bad_alloc` or logic error from the arena is reported as `TurnExhausted` ("grow the
   grant") instead of `IngestionFault`. Narrow the catch to the arena's limit exception, or carry
   the exception type in `cause`.
4. **A refused revive leaves the binding live.** `finishSourceChange` revokes only on
   `StaleIncarnation`; on `EpistemicBudget` (full `max_sources` at revive) the binding stays, the
   source's events are accepted and then refused in-arena as `StaleIncarnation` (Terminal, charged
   to a tombstone) while the real reason (`EpistemicBudget`, needs host action) is visible only
   through `sourceChangeOutcome`. Revoke on any refused change, or log the in-arena refusal with
   the change's code.
5. **Why-not-Vortex comment missing** on `epistemicCellsWithin` (agent_executor.cpp ~3099), and
   the walk runs once per committed ingestion over up to `cells + 1` cells with an
   `unordered_set` (see MAJOR 1 timings). One sentence naming the reason (host admission against a
   host setting, bounded by the ceiling) settles the rule; an `rtCount` in the turn would drop the
   per-ingestion hash set.
6. **`max_uncertainty` default does not bound anything.** It resolves to `max_age`
   (compiler/policy: `ms("max_uncertainty").value_or(maxAgeMs)`), so a policy that says nothing
   still lets a claimed uncertainty of `max_age` double both windows (accepting a reading up to
   `2 * max_age` old). The owner chose an optional key, so this is by design, but the
   hardware-M3 closure should say that only a policy that sets the key is bounded. Nothing else
   depends on it: the in-routine supersession tolerance (`unc + acquiredUnc + skew`) reads the
   bounded value, and the new refusal sits before the window checks.
7. **Design vs code on exhaustion.** Section 14 says at `ArenaExhausted` "no retire, advance or
   bookkeeping turn is attempted"; only ingestions are gated (deliverHost). Source-change and
   refusal-bookkeeping turns still run past the allowance. They mint few slots, so the code is
   the better behaviour (retire still works); amend the design sentence.
8. **I3-1d restore, latent only.** `restoreState` copies profile clone rings through the shared
   map; a clone view whose record's support was dropped (a pinned record of a retired principal,
   available once I3-4 sets pins) is copied without its record, and the advance and retire walks
   (`recordOf`: `rd(view, values, neg)`) then walk from cell 0. Unreachable today (no code pins);
   give `recordOf` a zero guard before I3-4. The policy-hash comparison before copying
   (`was != now` throws) is right; tests for live profile and tombstone floors pass.
9. **Floor map growth.** `retireSource` of a never-seen principal creates a `floors` entry
   `{0, strict}` that is only erased with the actor, and makes incarnation 0 for that principal
   unbindable. Host-authorized, so low; skip the entry when `highest` is empty and the floor is
   absent.
10. **Setters still `void`:** `forgetIngress`, `postRefusals` (agent_executor.hpp:523-525),
    `Ingress::log/forget/release/unbind`. Chain them if AGENTS.md is applied strictly.
11. **Stats pollution (round-1 7):** unchanged (`refused_Duplicate`, `refused_NoProfile` bump on
    routine events). Dedup window trim removes one entry per push, so lowering `epistemic_dedup`
    on a restored store shrinks it only over time.
12. **Fuzzer:** still a fresh actor per input (known, not re-raised); the new scripted steps and the
    17-code seed corpus are a clear improvement.

## Round-1 MINOR list status

| Round 1 | Status |
| ------- | ------ |
| 1 reserved values | partly fixed (`highest`, `Logged`); rest in MINOR 1 |
| 2 queue counters / Pending leak | fixed (`abandon`, `dropHostEvents`, `Undelivered`) |
| 3 half-sized log | fixed (one in-place entry per event, per source) |
| 4 diagnostics | fixed (`kIngressLog` debug lines on refusal, drop, revoke, supersede) |
| 5 void setters | remains (MINOR 10) |
| 6 counters / lock | fixed (counters after the push; hash outside the lock) |
| 7 stats | remains (MINOR 11) |
| 8 why-not-Vortex | remains (MINOR 5) |
| 9 test quality | partly: new tests cover starved class and `ArenaExhausted`; the duplicated rank walks in `EpistemicView` and the fixture remain |
| 10 fuzzer | known, open |

AGENTS.md style: formatting follows clang-format; comments explain why; no `std::cout`; categories
used (`kIngressLog`); `optional` for absences throughout the new C++; `std::in_range` for Int64
bounds. No new violations beyond MINOR 1.

Verdict: FAIL
