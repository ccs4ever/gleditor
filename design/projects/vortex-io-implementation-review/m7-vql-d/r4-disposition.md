# Round 4 disposition (M7-VQL-d code)

Target `57be2657`. Reports: [`r3-zigzag.md`](r3-zigzag.md) and [`r3-vortex.md`](r3-vortex.md), both
NOT CONVERGED (0 BLOCKING, 2 MAJOR, 2 MINOR each). Both MAJORs of each were reproduced by the
critics; none is fixed by patching. **Owner decision (2026-11-26, binding):** engine memory
reclamation for plain queries moves out of slice d into a new slice `M7-VQL-d3`, done with core
proposal P12 (the VM exposes which cells and dimensions are its own state, so a reclaim has a
principled root set), plus REPL result-row freeing. Slice d ships no reclaim that can destroy user
structure or free a cell anything still reads. Order: d, d2 (P11), d3 (P12), e. P12 itself still
needs its own approval when its exact diff is proposed. No core file changed.

| finding   | verdict                 | resolution                                                                                                              |
| --------- | ----------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| zigzag M1 | confirmed, pre-existing | the REPL never frees printed rows: recorded under d3 with the measurement (about 267 live cells per `join` row a run)   |
| zigzag M2 | confirmed               | removed with the plain-query reclaim: `systemDimension` and `keepAttachedCells` are deleted, plain queries keep program |
| zigzag m1 | fixed by removal        | the comment described the trail test; the code it sat on is gone                                                        |
| zigzag m2 | fixed by removal        | the attached-link rule is gone, so it needs no pin; d3 owns the unbound-`%` pins on `d.role`, `d.clone`, user ranks     |
| vortex M1 | confirmed               | the replaced-`let` free is removed; a pin for the parked scenario is added and passes with no freeing                   |
| vortex M2 | confirmed, pre-existing | redefining a closure keeps its program: recorded under d3, measured at 165 cells a run                                  |
| vortex m1 | fixed                   | the dead `live_` search in `ownOutcome` is deleted, with the reason in a comment                                        |
| vortex m2 | deferred to d3          | `keepIfNamed` compares registry counts, not cells; narrow (needs an interleaved drop and add), d3 compares cells        |

## What was reverted

- The plain-script `reclaimOnClose`, `ProgramRun::keepAttachedCells`, the `systemDimension` list,
  and `QueryExecutionOptions::keepProgram` with the mesh evaluator's use of it. `keepProgram`
  existed only to opt the mesh evaluator out of the plain reclaim, so it is deleted rather than left
  inert: an inert option is a promise the next reader cannot check. The reclaim rule is the one
  before round 3: a plain program is kept, an I/O script reclaims around the rows and published
  streams. One rule from round 3 stays and is smaller: a program that compiled a lambda and whose
  rows or bindings reach a closure value is kept too (a closure's descriptor is a raw reference into
  it); the check is a `VortexVM::closureValue` read, with no table.
- The replaced-`let` free: `publishCompilerBindings` takes bindings and functors as before slice d;
  `ownedLets_`, `closureEnvs_`, `heldByClosures`, `freeable`, `releaseOwned` and
  `ClosureEnvironment` are gone, and `vql_engine.{hpp,cpp}` are the pre-slice files. It failed
  soundness three times: names a closure mentions, cells a closure captured through a parameter, and
  now a parked script reading a session name (the roots are the VM's to state).
- `ownOutcome` no longer searches the other parked scripts' admissions.

## What was kept

Every other fix of rounds 1 to 3: `match` and its `Conflict` and fall-through rules, numeric write
sources, the owned cross-script await of a finished handle, the refusal graph freed in `retire()`,
`join` as host relations with identity preservation and its registration that outlives one run,
`summary` with its cause, the per-row and write bounds.

## Tests

- **Kept, pass with no freeing:** a closure keeps the text it captured (by name, by joined text, by
  lambda parameter, through another closure), `%` structure survives a rebind, an alias keeps its
  text, a rows-and-bound-text query survives, a closure among the results keeps its program. The
  suite names `VqlIoLetFreeTest` and `VqlIoPlainReclaimTest` became `VqlIoLetRebindTest` and
  `VqlIoPlainProgramTest`, since nothing is freed.
- **Added:** `VqlIoLetRebindTest.AParkedScriptKeepsTheSessionNameItReadWhenAnotherRebindsIt` (the r3
  vortex M1 scenario: reads `abc`); three `VqlIoKnownLeakTest` tests measuring the unreclaimed price
  (join 266, let-join 115, count 46; closure redefinition 165; unfreed rows 267 live cells a run)
  and bounding it by a ceiling, so the numbers in the design are reproducible.
- **Disabled (`DISABLED_`, bodies intact, owned by d3):** the three bounded plain-query pins, the
  clear-variables free, and the arena half of the join-and-write run; the join-and-write test keeps
  the service and file assertions at 200 runs. `ForgettingTheClosureLetsTheCellGo...` is deleted:
  with no free its assertions are trivially true and its name claims a free.
- `VQLSemanticsRepairTest.EmptyWalkOnlyRunsArgumentFreeBuiltinCount` is back to `cellCount()`, as
  before slice d.

## Verification

Headless, after `make -j$(nproc)`: `xuzz_test` 5265 passed (17 disabled, five of them the d3 pins
above), `zigzag_test` 269 passed.
