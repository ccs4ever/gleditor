# Round 4 critique: Vortex Expert (M7-VQL-d code)

Target 2e9e4178 (round 4 changes 1f1d8786..2e9e4178). Tally: 0 BLOCKING, 0 MAJOR, 2 MINOR. Focused
round. Headless (offscreen SDL, dummy audio, software GL, XDG under scratch). My r3 probe file was
rebuilt against the target objects outside the repo; nothing in the tree changed.

## Previous findings

- **r3 M1 (rebind frees a cell a parked script reads): RESOLVED by removal.** `R3Probe.Rebind`
  (script A parked reading `$t`, script B rebinds `$t`, A resumes) now returns `abc`, equal to the
  no-rebind control. `freeable`, `releaseOwned`, `ownedLets_` and the free in
  `publishCompilerBindings` are gone (`vql_engine.cpp` diff is a pure deletion back to the pre-slice
  shape). The new pin `AParkedScriptKeepsTheSessionNameItReadWhenAnotherRebindsIt` passes.
- **r3 M2 (redefining a closure leaks its program): PRE-EXISTING, accepted by the owner decision.**
  `R3Probe.BoundedRedefiningAClosure` still grows 1370 to 167370 over 1000 runs (about 166 a run),
  matching the 165 in section 8c. Truthfully priced, owned by d3 item 3; not a finding.
- **r3 minor 1 (dead `live_` search in `ownOutcome`): FIXED.** The loop is deleted and the comment
  gives the reason; `ownOutcome` now searches only `current_->admitted` then `finished_`.
- **r3 minor 2 (registry-changed test is a count comparison): OPEN, see MINOR 1.**

## Checks in scope

- **(a) The revert is complete and correct.** The rest of `ProgramRun::reclaim` is the pre-slice
  rule: a plain program (`ioActions.empty()`) is kept whole; an I/O script reclaims around rows and
  published streams. `keepAttached_`, `systemDimension`, `QueryExecutionOptions::keepProgram` and
  its one user (mesh evaluator) are deleted consistently (execution.hpp, mesh_evaluator.cpp). My r3
  parked probes pass on the target: `ParkedSurvivesPlainReclaims` (50 rounds of plain join, let,
  lambda and `%` queries between park and resume), `ParkedWithClosureLetAndPlainClosureRuns`,
  `PercentOnSystemDims`, `Rebind`. `ParkedLetSurvivesPlainReclaims` fails only with the typed
  `VQL_SCRIPT_FAILED: Vortex VM supports one active stream cursor at a time` (the r3 observation,
  unchanged, refused by code, not a freed cell). The slice's own `VqlIo*` suite passes (270 tests, 5
  disabled). No slice d path I could find frees a cell a parked activation, owned record, lease,
  request or the VM needs: what remains freeing is the owned-record and refusal-graph `retire()` and
  the I/O-script region reclaim, both as reviewed in rounds 1 to 3.
- **(b) The closure rule is correct.** `reachesClosure` is a `VortexVM::closureValue` read over the
  rows and published cells; closures hold other closures only as closure values reachable through
  those roots' own program, which is kept whole, so checking the roots suffices. It runs only when
  `makesClosures`; a closure from an earlier program lives in that program's kept region. A closure
  returned by an I/O script's later run is an outside cell the later region does not own.
- **(c) Removing the `live_` search leaves no regression.** The shape it served cannot be written
  (names of a parked script are not visible to another), and the entry it would have erased from the
  parked script's list was the defect.
- **(d) No new unbounded growth.** Owned records and refusal graphs are freed in `retire()`
  (`ManyRefusedNamedAwaitRuns...` and `ManyPlainDetachedRefusalRuns...` pass). The join providers
  are stored in `relationProviders_`, an `unordered_map` keyed by the relation name, so
  re-registering each begin replaces rather than accumulates. Join result text is a plain-query cell
  like any other; its growth is priced in 8c.
- **Core files:** `git diff 0e6a672d..2e9e4178` restricted to `apps/common/xanadu/vortex`,
  `apps/common/xanadu/zigzag`, `apps/common/xanadu/store.cpp`, `src` and `include` touches
  `capture_host`, `frontend_diagnostics`, `mesh_evaluator`(round 4 only reverts a line),
  `owned_outcome` only. No `vortex_core`, `vortex_vm`, `manifold`, `arena_manifold`, store, spool or
  compact_op file moved. Confirmed.
- **(e) d3 scope and P12 text** cover my r3 findings: the parked-activation root (d3 item 2 and
  P12's "parked activations' bound slots"), closure environments by name and parameter, closure
  region freeing on redefinition (item 3), the unbound-`%` pins, and the minimal alternative priced.
  One gap, MINOR 1.

## BLOCKING

None.

## MAJOR

None.

## MINOR

1. **r3 minor 2 is deferred in the disposition but absent from d3's scope.** `keepIfNamed` still
   compares registry counts (`execution.cpp`, around the `run->keepIfNamed` lambda), and the roadmap
   6.1c list does not say d3 replaces it with a cell comparison. It only bites once plain programs
   are reclaimed (today a plain program is kept regardless), so it is latent. Add it to d3 item 1 so
   it is not lost.
1. **Section 8c says everything in the table is "as before slice d (the slice did not make it
   worse)".** The `join` rows (266 and 115 a run) are cells from a function this slice introduces;
   the mechanism (a plain program is kept) is pre-existing but the figure is not. Say "the same
   mechanism, now also for `join`", so d3's baseline is not read as pre-slice.

VERDICT: CONVERGED
