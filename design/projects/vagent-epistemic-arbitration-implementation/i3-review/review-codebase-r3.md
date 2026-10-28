# I3-1 revision 3 review: compiler and codebase persona

Base: apps/common/xanadu/vagent/runtime and apps/common/xanadu/vortex. Read only; nothing built.

## Round-2 findings, verified

- M1 (refusal codes lost on undo): NOT RESOLVED, see new finding 1. The mechanism chosen cannot work
  as written.
- M2 (budget arithmetic): resolved in principle. The field-based estimate is plausible and the
  ceiling (32768) sits under `ValueLimits::operations` (65536, mesh_value.cpp 395-448). See finding
  5 for the two things still open (counter accuracy, baseline graph).
- M3 (`from <alias>`): resolved. Static refusal is feasible in the checker; `believes … from` is a
  parsed `from Postfix` and the predicate name is static.
- M4 (predecessor double count): resolved. Principal-keyed support text means `rtHasSrc`'s
  `Eq "src?"` (compiler.cpp 4066) matches across incarnations; rule-based ineligibility is cheap.
- M5 (restore copier): accepted as a spike gate for 1d. Reasoning matches the code
  (agent_executor.cpp 263-266, 330, 357).
- M6 (bookkeeping): partly resolved, see finding 3.
- Minors: `wallNow()` correct (agent_executor.cpp 1091); policy-schema population correctly labelled
  new work (upgrade_schema.hpp 40 unused); fuel complexity stated; split into 1a-1d is sound.

## Findings

1. BLOCKING: `$turn_result` written before failing is destroyed by the failure itself. The turn's
   base choice point records an arena mark (`pushChoicePoint`, vortex_vm.cpp 204-240, called at
   agent_executor.cpp 1633), and the `Fail` opcode runs `backtrack()` which calls
   `core_.arena().release(cp.mark)` (vortex_vm.cpp 254) before control reaches `failOp`. Every write
   made after the mark, including a `$turn_result` field cell, is truncated before `runTurnOnce` can
   read it (it reads only after the loop, step 5-7 at 1672-1699, and the existing `$answer` read at
   1679 is on the success path for exactly this reason). So "each refusal sets `$turn_result` before
   failing" (§6) yields nothing, and `TurnOutcome::result` would be empty on every refusal. Fix:
   refusals must not use `Fail`. A refusal sets `$turn_result` and jumps to `haltOp_` (as
   `emitDispatch` does, compiler.cpp 5227), `runTurnOnce` reads the field after the run and releases
   `m0` itself when the code is anything other than `Committed` or `Duplicate` (the undo it already
   does for `readOnly` turns, 1699). Only fuel exhaustion and cycle limits keep the generic
   `kFailFuel`/`kFailCycles` path, correctly classed `Retryable`. A spike is needed for a
   `jump(haltOp_)` taken from inside nested `callLabel` frames (the call stack must be unwound by
   step 8's `arena.unlink(cursor, ...stack...)`, 1707, which suggests it is safe, but prove it).
   Rewrite §5 and §6's last paragraph accordingly. Also state that `$turn_result` is declared in
   `runtime_abi.hpp` beside `kFieldAnswer` (520) and is cleared per turn.

1. MAJOR: the `Duplicate` outcome commits a write (refresh contact time) but §5 says only failed
   turns report. `runTurnOnce` must report `result` on committed turns too, and the adapter must map
   `Duplicate` to commit and every other non-`Committed` code to undo. Specify the closed set of
   result codes and that an unknown code is treated as a failed turn. Add a test per code.

1. MAJOR: bookkeeping routing cannot be decided on the host thread. "If the principal already has a
   profile the adapter runs a bookkeeping turn", and observe-time refusals "for a principal with a
   profile queue the same bookkeeping turn", but `observe` runs on a host thread (`percept`/PostBox
   model, agent_executor.cpp 2696-2735) and the profile lives in the arena, which only the owner
   thread may read. Post a `RefusalNote` item with the code unconditionally (bounded by the
   per-source share) and let `drainPosted` run `observeRefusalEntry`, whose Vortex body does nothing
   when no profile is found. State that the bookkeeping turn can itself exhaust fuel and then only
   the outcome log has the refusal. `PostBox::Item` needs a kind enum (it currently has
   `bool percept`, 526-533); add it once for observation, note and (later) verification items.

1. MAJOR: a refusal at ingestion after partial work (profile creation, transfer splice) is only safe
   because of the release; with finding 1's change, the release is now explicit and must also cover
   the `rtSettle_` events the routine raised into `d.pending`. Confirm the order in §6: the routine
   reaches `rtSettle_` only on the commit path, since settle runs user `+pred` handlers that must
   never run for a refused observation. State this.

1. MINOR: the counter and baseline. (a) `$epistemic_cells` is a hand-maintained sum of per-structure
   constants; require a test that after every operation the counter equals the real arena cell delta
   (`arena.cellCount()`), including excision, or it will drift and either refuse early or overshoot.
   (b) The checkpoint count is all reachable cells from the root, and the core hangs its standard
   library off the agent cell (restoreState comment, agent_executor.cpp 3388-3393). Measure that
   baseline in 1b and set `vagent.epistemic_cells` as a stated fraction of
   `valueOperations - baseline`, not a bare 32768. (c) `$epistemic_cells` itself must be copied by
   `restoreState`'s field loop (scalar field, `copyCellData`) and checked against a recount after
   restore.

1. MINOR: dedup, stream and key lists need an explicit eviction in Vortex; excising a dedup pair
   adjusts the counter. History of pinned records is unbounded once I3-4 pins; state that pinned
   records count against `vagent.epistemic_cells` and refuse new records rather than evicting.

1. MINOR: static refusal and guard. "No tell/untell test emitted" is feasible (`emitTellTests`,
   compiler.cpp 5265), but the compiler assertion that `beliefChange` never reaches `rtBSet` for an
   arbitrated predicate is only decidable where the predicate is a compile-time name; peer tells
   arrive by event type string. Say the guard is a compile-time assert at each emission site plus
   the omission of tell tests. Add a test that a peer tell for an arbitrated predicate is counted
   unhandled and changes nothing.

1. MINOR: tests. The listed three test files are fine; confirm whether `tests/xuzz` actor tests are
   globbed into `xuzz_test` or listed in the Makefile (lines 676-686 show only the frontend and
   runtime-core lists), and state which. The 1d spike needs its own throwaway test file, not a
   permanent one.

## Feasibility summary

Everything else in revision 3 is implementable as written: `observeEntry` and `observeRefusalEntry`
as new entries beside `dispatchEntry`, `turnFields` for inputs, `rtFindClause`/`rtFindSrc`/
`rtObserveSet`, the splice before `rtBDrop` (it only unlinks the old support, compiler.cpp
4089-4115, so the spliced list survives), principal-keyed supports, the checker refusals, settings
and the schema population. Size: 1b about 1500 lines with tests, 1c about 400, 1d about 700
including the spike; the split is right.

## Tally

BLOCKING 1, MAJOR 3, MINOR 4.

VERDICT: FAIL
