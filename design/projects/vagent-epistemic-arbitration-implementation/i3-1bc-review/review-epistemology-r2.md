# I3-1b and I3-1c review, round 2: epistemology persona

Scope: head `d48e85f8` (round 1 was `4ad3f53a`; fixes `e91e8fe8`; plus `0124b0d2` max_uncertainty and
I3-1d resume). Method: read the new `bindSource`, `retireSource`, `finishSourceChange`, `drainPosted`,
`emitSourceChange`, `emitObserve` (keys count, keyless implicit key, epoch rules), `restoreState`,
then probes in `/tmp/i3-1bc-epistemology-r2/probe.cpp` (compiled against the rebuilt objects, no repo
file touched, headless). `./build/vagent_actor_test --gtest_filter='VagentEpistemic*'`: 92 pass.

## Round-1 findings

- **M1 (dead supports consume `keys_per_source`): FIXED.** Probe `M1KeysAfterAdvance`: 4 keys at inc 1,
  advance to 2, then `k9..k12` all commit; `k13` is `EpistemicKeyLimit` (4 current keys). The count
  walks each support's rank for a `current` record (compiler.cpp "keys?" block); invariants hold.
- **M2 (failed advance silent, unretryable): FIXED, visibly.** Probe `M2Heavy` (32 keys, 4 readings
  each, `maxInstructions` 24000 and below): bind 2 gives `sourceChangeOutcome == Refused:TurnExhausted`,
  profile stays at 1 (all-or-nothing: `violation()` empty), new-boot reading `UnboundIncarnation`, and
  every bind now posts the turn (agent_executor.cpp `bindSource`, end), so a rebind re-runs it. Same
  bytes cost the same, so the retry only succeeds after the host raises the budget; that is honest
  and now observable. No test induces a real failed-then-succeeding advance (the test named
  `EveryBindRunsTheSourceChangeSoAFailedOneRetries` only shows a same-boot no-op); add one.
- Minors: 1 (`last_contact` regress) FIXED (compiler.cpp:4486-4497 keeps the maximum). 2 (huge
  incarnation) FIXED (`InvalidIncarnation`). 3 (sentinel 0 in `SourceChange`) FIXED, `optional`; see
  new minor 2 for a sibling. 4 (revive bypasses `max_sources`) FIXED (probe `ReviveBudget`:
  `EpistemicBudget`, source stays refused until another retires, then rebind commits). 5 (alias freed
  by partial unbind) FIXED (`stillUsed`, :764). 6 (supersede drops other predicates) remains, stated
  in a comment only. 7 (C++ health classification, `epistemicCellsWithin`) remains; cross-persona.
  8 (advance/retire results unlogged) FIXED (`sourceChangeOutcome`).
- Keyless implicit key: sound. `rtBSet` with replace and `nkeys == 0` drops the source's other
  clause and moves its history to the new support. Probe `Keyless`: three readings, one current
  clause per source; across an advance still one; `violation()` empty.
- Epoch rules: sound. Probe `Epochs`: stream epoch regression on another key is
  `ObservationTimeMismatch`; older acquisition in the same epoch refused; the same older acquisition
  in a newer epoch accepted; an advance resets the stream (epoch 1 at inc 2 commits).
- `max_uncertainty` (hardware M3): probe `Uncertainty`: 11 s refuses `UncertainAcquisition` at
  ingress, exactly 10 s commits, negative and NaN `InvalidObservation`. Charged as a content refusal
  (`isContentRefusal`), Terminal class. Policy hash bumped to `vagent-policy-2`, and `restoreState`
  compares hashes. Nothing else of mine depends on it; the in-turn tolerance sum is now bounded by
  `2 * max_age`.
- Resume (1d): `TheArenaFloorRefusesABootTheNewHostAccepted` passes; the revoke path works (stale
  advance, `UnknownSource`, then synchronous `StaleIncarnation`, then a higher boot continues).
  Profiles keep `retired`, incarnation, streams and dedup, so the replay floor and tombstones survive.
  `last_receive` and `last_contact` are cleared with `nullopt`, not a sentinel.

## BLOCKING

None.

## MAJOR

1. **Control items run in reverse call order; `retireSource` then `bindSource` in one pump kills the
   revived source, and two binds in one pump wedge the host floor.** `PostBox::putBack` inserts at
   the front (agent_executor.cpp:584-588), and `bindSource` / `retireSource` each call it with one
   item (:3273, :3297), so items queued by consecutive calls drain newest first. The source-change
   turn is the authority for incarnation and tombstones, so order is semantics.
   - Probe `RetireThenBindOnePump`: bind 3, commit, `retireSource`, `bindSource(4)`, one pump. The
     bind-4 advance runs first (profile 4), then the retire tombstones it at 4. Result: `obs@4 ==
     Refused:StaleIncarnation`, profile 4 retired. The host did what a re-enrolment does (retire, then
     bind the new boot) and the device is dead until it reboots again, which the host cannot cause.
     A rebind of 4 changes nothing (`revive` needs a strictly higher incarnation).
   - Probe `TwoBindsOnePump`: profile 3; `bindSource(5)` then `bindSource(6)`, one pump. Advance 6
     commits, then advance 5 is `StaleIncarnation`; `finishSourceChange` (:1809-1811) sets the host
     floor to `{6, strict}`. `sourceChangeOutcome` reports Stale for a healthy source, readings at 6
     commit, but `bindSource(door, 6)` now refuses `StaleIncarnation` synchronously (probe confirms),
     so the source can never be bound for another predicate until its next boot. `TwoBindsPredicates`
     (2, 3, 4 in one pump) ends the same way.
   - Cause beyond order: `strict` means "tombstone or refused advance", but a Stale for a lower
     boot than a live profile says nothing about the live boot, yet the code makes the floor strict at
     `max(floor, stale)`, which equals the live incarnation after the newer advance committed.
   - Round 1 recorded retire-then-bind as order-preserving; it no longer is, or never was in this
     form, and no test pumps once across two control calls.
   - Remedy: keep call order. Give `PostBox` a control insert that goes after the last control item
     already queued and ahead of ingestions (or stamp items with a monotone call number and sort on
     drain). Independently, in `finishSourceChange` make a Stale floor strict only when the arena's
     reason was a tombstone, or never raise a floor that a committed advance already set at or above
     the stale incarnation. Test: retire, bind(n+1), one pump, reading commits; bind n+1, bind n+2,
     one pump, bind of another predicate at n+2 is accepted.

## MINOR

1. **Dead supports across boots now starve the cell budget instead of the key count.** Probe
   `DeadSupportsAccumulate` (cells 3000, history 2, keys 4): a source that bumps its incarnation and
   reports new keys each boot reaches `EpistemicBudget` at inc 11, and a second source is then also
   `EpistemicBudget`; `retireSource` of the first frees it. M1's fix removed the lockout of the
   successor but each boot still leaves up to `keys_per_source` believed supports that nothing
   reclaims. At 24576 cells and about 80 cells a record, a crash-looping tracker whose target IDs
   drift can fill the ceiling in tens of boots. Section 14 records the belief side as open for I4; it
   should also record this budget side. Remedy: open question Q1 option 2 (drop the support in the
   advance, settling `-pred`) fixes both; until then, say so in section 9 and have I3-2 count
   `EpistemicBudget` caused by one profile's dead supports as a host alarm.
2. **Host floor is a C++ `0` for "no floor", and a retire of nothing blocks incarnation 0.**
   `Ingress::Floor::incarnation{}` is 0 for "none" and `floors[...]` default-inserts it (:1802,
   :3291). Probe `RetireNothingFloor`: `retireSource` of an unseen unbound principal, then
   `bindSource(..., 0)` is `StaleIncarnation` (floor `{0, strict}`); inc 1 binds. Section 14 says a
   retire of an unnamed principal "leaves nothing"; the arena does, the host does not. This is the
   sentinel rule (host-side state is not wire or disk). Remedy: `optional<Floor>`; create the entry
   only when there is an incarnation to hold. Same for the `strict`-with-no-floor case.
3. **A failed revive leaves the binding live.** Probe `ReviveBudget`: after `Refused:EpistemicBudget`
   from the revive, readings at the new boot are `StaleIncarnation` (Terminal class, so a host that
   follows the class never resends), not charged. The log is honest but the class points the wrong
   way, and the binding is neither revoked (as the stale case is) nor flagged. Remedy: revoke on
   `EpistemicBudget` in `finishSourceChange` as for Stale (the retry is a rebind anyway), or make
   readings against a failed-revive tombstone `NeedsHostAction`.
4. **No test for a genuinely failed advance followed by a successful rebind.** See M2 above; a
   `maxInstructions` just under the advance cost and then a host that restarts with more is the
   shape. Also add a test of two control calls in one pump (major 1).
5. **Carried, unchanged:** a superseding bind silently drops the principal's other predicates
   (no receipt); `Ingress::charge` classifies health and `epistemicCellsWithin` walks the arena in
   C++ where section 6 specifies Vortex (no justification written). The resume arena-floor revocation
   is correct, but it raises the host floor to the stale incarnation `strict` and then to each higher
   stale boot in turn; harmless, and subsumed by the fix to major 1.

## Invariants, re-checked

- Provenance: supports, profiles and dedup entries are keyed by the 64-hex principal everywhere,
  including after resume; no alias or transport peer reaches the arena. Relayed copies still charge
  `refused_count` only.
- Replay floor: ingestion step 2 refuses lower and higher-than-profile incarnations in the arena,
  tombstones refuse readings, resume carries both. The only way found to defeat intent is major 1
  (a legitimate boot tombstoned by reordering); a lower boot cannot be accepted.
- All-or-nothing: every probe including failed advances and refusals inside turns leaves
  `EpistemicView::violation()` empty.
- One support and at most one current record per (principal, key): holds across advance, revive,
  keyless replace and epoch regressions (`violation()` empty in every probe).

Verdict: FAIL
