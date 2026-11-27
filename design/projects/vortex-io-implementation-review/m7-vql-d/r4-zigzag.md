# Round 4 critique: Xanadu/ZigZag Purist (M7-VQL-d code)

Target: `2e9e4178` (round 4 changes `1f1d8786..2e9e4178`). Focused round.

Tally: BLOCKING 0, MAJOR 0, MINOR 1.

Method: diffed `execution.cpp`, `execution.hpp`, `vql_engine.{hpp,cpp}`, `script_runner.cpp` and
`mesh_evaluator.cpp` against the pre-slice commit `d065da02`; ran `make vquery xudu-dump xuzz_test`
(already current); ran the disabled and known-leak tests with `--gtest_also_run_disabled_tests`,
headless with XDG under a scratch directory; probed `vquery` on `slice_then_xanadoc`.

## Status of r3 MAJORs

- **r3 M1 (the REPL never frees the rows it prints): RESOLVED as scope.** The owner moved it to d3
  item 4 (roadmap §6.1c), and §8c's bullet is truthful. `RowsTheCallerKeepsStayLive...` measures 267
  live cells a run for kept rows against 266 for freed rows: the row costs about one cell, the kept
  program the rest.
- **r3 M2 (`systemDimension` destroys unreturned `%` structure on `d.role`, `d.clone` and other
  ranks): RESOLVED.** The list, `keepAttachedCells` and `keepAttached_` are gone and a plain program
  is kept whole. Probing `return #/d.role%"a"/count()` then `return #/d.role/count()` (and the same
  on `d.clone`) gave 1 then 2: the unbound `%` structure survives. d3 item 1 lists `%` survival pins
  on `d.1`, `d.role`, `d.clone`, `d.name`, `d.vars`, `d.values`, `d.stores`, `d.stack` and a user
  rank.
- **r3 m1, m2: RESOLVED** by removal (the trail-test comment and the attached-link rule no longer
  exist).

## (a) The revert restores pre-slice behaviour

- `vql_engine.hpp` and `vql_engine.cpp` have an empty diff against `d065da02`.
- In `execution.cpp` the only difference from pre-slice on the reclaim path is the extra
  `closureReached` keep (`compiled_.makesClosures` plus a read-only `VortexVM::closureValue` check).
  It can only keep more, never free. The plain-versus-I/O decision is otherwise the pre-slice
  expression; mounts and functors are published as before.
- The `ownOutcome` `live_` search is removed; it was unreachable and harmful.
- `keepProgram` is deleted and the mesh evaluator is as before slice d.
- No slice-d path can free a live cell or destroy user structure that the pre-slice engine would
  not. The I/O-script reclaim around rows and streams is the pre-slice rule; any weakness in it is
  pre-existing and out of scope.

## (b) §8c is truthful

- The `DISABLED_` join-and-write I/O run, run enabled, adds exactly 1 live cell and 1 slot a run
  (10000 over 10000 runs): the replaced `let` text cell. This matches §8c ("one text cell"), though
  the figure is not in its table (wording only).
- `join_step` and `join_end` mint one result cell a run; registration replaces the provider and does
  not accumulate. The loop is compiled once, so the cost is not per row: the 266 is mostly the kept
  program.
- Owned records and summary cells are freed in `retire()`; the service request count stays flat.
- The closure-keep rule adds retention only for scripts whose rows or bindings reach a closure;
  before this slice such a script risked a use-after-free, so keeping is the safe direction, and §8c
  states it.
- Plain `join` is new as a feature, but its leak mechanism (a kept plain program) is pre-existing
  and the price is disclosed.

## (c) The disabled and known-leak tests are honest

- The five `DISABLED_` tests have intact bodies; run enabled they fail with the magnitudes §8c
  reports (2662170 live cells against 2170 after 10^4 `join` runs; 10104 against 104 for
  join-and-write). The clear-variables test fails as expected.
- Roadmap §6.1c item 5 names all of them as d3 acceptance tests. The `KnownLeak` tests pass and
  bound the measured figures. Tests with no real assertion without the free were deleted rather than
  left misleading.

## (d) The d3 scope covers the r3 findings

Yes: plain-program reclaim with a VM-provided root set, sound `let` freeing, closure-region freeing,
REPL prior-row freeing, and the unbound-`%` pins on every rank named.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. The `KnownLeak` ceilings are loose

**Problem.** The ceilings are 400, 200, 100, 400 and 300 against measured 266, 115, 46, 267 and 165.

**Failure scenario.** A 50–100 % regression in per-run retention passes unnoticed until d3.

**Evidence.** The `VqlIoKnownLeakTest` ceilings in `tests/xuzz/vql_io_values_test.cpp`.

**Fix.** Optionally tighten to about 1.25× the measured figure now; d3 turns them into equality with
zero, so the stakes are low.

VERDICT: CONVERGED
