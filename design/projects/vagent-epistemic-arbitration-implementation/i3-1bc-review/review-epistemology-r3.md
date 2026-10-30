# I3-1b and I3-1c review, round 3: epistemology persona

Scope: head `08425c60` (round 2 reviewed `d48e85f8`). Read `git diff d48e85f8..08425c60`
(`pushControl`, `finishSourceChange`, `retireSource`, `emitSourceChange` advance drop and trim,
`epistemicCellsWithin`, `restoreState`), then probes in `/tmp/i3-1bc-epistemology-r2/probe.cpp`
against the rebuilt objects (headless, no repo file touched). `vagent_actor_test
--gtest_filter='VagentEpistemic*'`: 97 pass.

## Round-2 findings

- **MAJOR 1 (control items ran newest first): FIXED.** `PostBox::pushControl` inserts after the
  source changes already waiting and ahead of everything else (agent_executor.cpp:590-598), and
  `bindSource` / `retireSource` use it. Probes, each in one pump:
  - `RetireThenBindOnePump` (retire, bind 4): `chg=Committed`, reading at 4 `Committed`, profile 4
    (round 2: tombstoned, `StaleIncarnation`).
  - `BindThenRetireOnePump`: retired, reading refused `UnknownSource`, rebind 5 commits.
  - `TwoBindsOnePump` (5 then 6): profile 6, `chg=Committed`, `bindSource(door, 6)` accepted
    (round 2: floor `{6, strict}`, refused). `TwoBindsPredicates` (2, 3, 4): same, rebind of 4 ok.
  The strict-floor imprecision I named as a second cause is not touched, but with ordered
  execution a stale result for a lower boot can no longer follow a newer commit, so the path that
  reached it is closed. Resume still reaches a strict floor at the stale incarnation, which is
  correct there (the arena's floor is at least that).
- Minors: 1 (dead supports fill the cell budget) FIXED, see below. 2 (host floor 0, retire of
  nothing blocks incarnation 0) FIXED: `RetireNothingFloor` now binds inc 0. 3 (failed revive left
  the binding live) FIXED: after `Refused:EpistemicBudget` a reading is `ingress:UnknownSource`
  (revoked), and after room is made the rebind commits. 4 (no real failed-advance test) REMAINS; the
  new tests cover call order and bounded replacement boots, none induces a failed advance and a
  retry. 5 carried: superseding bind drops the principal's other predicates without a receipt;
  health classification and `epistemicCellsWithin` in C++ (a justification comment was added at
  `epistemicCellsWithin`: host admission after the routine's halt; acceptable).

## The Q1 change (advance drops dead supports), judged epistemically

What I probed (`DropOnAdvance`, `WhereDidTheyGo`, `Heavy`, `ResumeCarriesRetiredChain`,
`DeadSupportsAccumulate`):

- After an advance every predecessor support is gone from its clause, `-at` is raised once per
  dropped support (drops 2 to 4 for two keys), a clause another source also supports survives
  (`rtBDrop` removes only this principal), and `EpistemicView::violation()` is empty. A belief now
  means a current boot asserts it, which restores what the support/belief pair is for: no plain
  query or `+at`/`-at` handler sees a reading held that nothing current backs.
- Provenance and replay floor intact: the moved records keep their incarnation, event ID, digest
  and status `retired`; the profile's incarnation and dedup window are untouched by the drop
  (`freshWindows` still resets only the per-boot dedup and streams, as before).
- One support and at most one current record per (principal, key): holds; after the drop the
  new boot's first reading for the same key creates a fresh support, one current record.
- The history is on the profile chain: `chain` was 4 after the first advance, trimmed to
  `history x keys_per_source` (6 in the probe) after a third boot, oldest boot first. The retire
  turn clears the chain (`chain=0`, tombstone only), and the next advance walks it like any record.
- Resume: the profile chain and its clone occurrences cross a checkpoint (chain 6, views 7 = 6 +
  1 current), `violation()` is empty, and an advance and a retire on the resumed side both commit
  and leave chain 0.
- Cell accounting: `epistemicCellsWithin` now admits the chain. A device reporting new keys every
  boot no longer exhausts the ceiling: the 40-boot run with a 3000-cell ceiling ends with a second
  source still accepted.
- Cost: an advance now settles one `-pred` per support. `Heavy` (24 keys x 2 readings, history 32):
  commits at `maxInstructions` 60000, `TurnExhausted` at 40000 (round 2 survived 30000 with more
  records). All-or-nothing holds (profile stays at the old incarnation, no partial drops,
  `violation()` empty); the default budget (10M) is far above this.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **A failing `-pred` handler wedges the advance and the retire for that principal.** Both turns
   call `rtSettle_` in the same turn, so a handler that fails undoes the whole turn. Probe
   `HandlerFaultWedge` (program with `on -door(?i, ?o) { while d < 1 do () }`): a door reading
   commits (no `+door` handler), then `bindSource(2)` is `Refused:TurnExhausted`, profile stays 1,
   `retireSource` is `Refused:TurnExhausted`, and a rebind at 3 is `TurnExhausted` again. Same
   bytes, same cost, so retrying never helps, and the host loses its removal tool for that source
   (retire) as well as the device's reboot path. The advance is new at this head; the retire has had
   it since `5a908ea8` and my round-1 note that it was "recoverable" was wrong when the cause is
   deterministic. It needs a program whose `-pred` handler fails where `+pred` did not (or a
   source whose drop count outruns the fuel), so it is MINOR, but a source must not be able to hold
   a retire hostage. Remedy: commit the drops and the status changes in the source-change turn and
   deliver the `-pred` events to ordinary handler turns (they are already queued on `$pending`), so
   a handler failure costs that event, not the floor; or at least make a failed settle in these
   two turns a logged `HandlerFault` that still commits the drop.
2. **The retired chain is not ordered by age within one boot.** The chain is built by walking the
   profile's clone rank (newest record first) and appending each support's chain whole, so the
   front of the chain is the support holding the newest record, and the trim ("oldest first") cuts
   that support's oldest records before an older support's records. Across boots the order is right
   (boot 1 is before boot 2), and in the probe the survivors were `b3 a1 | d1 d2 d3 c1`. For
   calibration that keeps the right boots but not strictly the newest N. Remedy: say "oldest boot
   first, then by support" in section 14, or order by receipt time when trimming.
3. **History no longer rejoins the new support for the same key.** `AChangedValueCarriesItsSources
   HistoryAlong` still holds within a boot, but after an advance the same key's earlier readings sit
   on the profile chain, not on the new support. Anything that scores a key from its support's
   history (I3-2 calibration, I4 eligibility) must also read the chain. Section 14 should state it
   so I3-2 does not discover it.
4. **The shared invariant checker does not look at the chain.** `EpistemicView::violation()`
   (tests/xuzz/epistemic_invariants.hpp) walks supports only, so every probe above passed without
   anything checking that chain records are all `retired`, not over the bound, and each has a clone
   occurrence. Add those three checks, and a fuzzer step that bumps the incarnation.
5. **Still no test of a failed advance followed by a successful retry** (round-2 minor 4).
   `maxInstructions` just under the advance cost (about 40000 for the `Heavy` shape) then a host
   that resumes with more is the shape; the retry path is only verified here by probe.
6. **Carried:** a superseding bind silently drops the principal's other predicates; health
   classification in `Ingress::charge` is C++ without a stated reason.

## Invariants, re-checked at this head

- Provenance: supports, profiles, dedup and the chain are keyed by the 64-hex principal; no alias
  or transport peer reaches the arena.
- Replay floor: ordered source changes close the reordering path; tombstones and the profile's
  incarnation survive the drop, the trim, the retire and the resume.
- All-or-nothing: failed advances (fuel), failed retires (handler), refusals inside turns leave
  `violation()` empty and the chain unchanged.
- One support and at most one current record per (principal, key): holds across drop, revive,
  keyless replace, epoch regression and resume.

Verdict: PASS
