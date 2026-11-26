# Round 2 disposition (M7-VQL-d code)

Target `09635a6c`. Reports: [`r2-vql.md`](r2-vql.md), [`r2-vortex.md`](r2-vortex.md); the zigzag
critic was not run in round 2. Both MAJORs were reproduced and fixed; the MINOR is fixed. No core
file changed. P11 was approved by the owner on 2026-11-26 and is recorded, not built (roadmap §5,
§6.1b).

| finding   | verdict   | fix commit | where                                                                              |
| --------- | --------- | ---------- | ---------------------------------------------------------------------------------- |
| vql M1    | confirmed | `64bce139` | `vql_engine.{hpp,cpp}`, `compiler.{hpp,cpp}` (`capturedNames`), `execution.cpp`    |
| vortex M1 | confirmed | `41246816` | `execution.cpp` (join registration site)                                           |
| vortex m1 | confirmed | `c8d84b22` | `script_runner.cpp` (`ownOutcome` also searches the other live activations' lists) |

**vql M1.** Both scenarios reproduced in `vquery` before the change and corrected after it. A
replaced `let` cell is now freed only when it is (a) still the cell it was (incarnation), (b) named
by nothing else (other names, rows the caller holds, views, lenses, mounts), (c) linked on no
dimension, and (d) not captured. A `%` run, or any structure a path program built, is linked into
the space, so (c) leaves it to the user. A closure keeps the cell its name had when it was made and
the arena records no such reference, so the compiler now reports the session names its lambdas and
frames capture (`CompilationResult::capturedNames`, the union of the three frame capture tables);
the engine resolves them to the cells they hold (the current binding and the one the same script
binds) and keeps those cells, by incarnation, out of every free. Entries whose cell has gone are
pruned on each publish, so the table is bounded by live captured cells. Prices: a cell a closure
captured is never reclaimed when its name is rebound, and a name captured by any frame of a program
protects its cells even when only a `def` named it (over-conservative, never unsound). The unlinked
join result the 10^4 pin is about is neither linked nor captured, so the pin passes unchanged.

**vortex M1.** The providers are registered on the shared VM with no undo, and again at each begin.
Registering once at runner scope was not taken: `QueryExecutor` is also used with no runner (every
`JoinWorld` test), so the registration site stays where the executor begins. A walk is reset at its
first step and taken by its end, and a join has no park inside it, so whichever registration is
current serves all scripts. `prose_hits` has the same shape (older code, not this slice) and is
left.

**vortex minor.** `ownOutcome` searches the current activation, the finished list, then every other
live activation. A test could not be written: a parked script's `let` names are not visible to a
second script until it ends, so the shape (a second script awaiting a handle a still-parked script
admitted) is unreachable through the script surface; the change is the three-line search and the
existing 274 `VqlIo*`/`JoinWorld*` tests pass over it.

## Tests

New tests in `tests/xuzz/vql_io_values_test.cpp`: `VqlIoLetFreeTest` (four: closure keeps a captured
text, closure keeps a captured joined text, a `%`-minted structure survives a rebind, a name that
aliases a replaced cell keeps its text) and
`VqlIoJoinTest.AParkedScriptKeepsItsJoinWhenAnotherScriptRunsAndEnds`. Fail-before (engine free
logic reverted, tests kept): the two closure tests and the structure test fail (`$g(1)` prints `!`;
the three `d.1` cells are gone); the alias test holds both ways, as the round 1 behaviour kept it.
The two-script join test fails before with `no relation provider is registered for 'join_step'`.
Full `xuzz_test` (all tests but the 44 that shell out to `build/xuzz` or draw: the first full run
was made without the headless environment and with a stale `build/xuzz`; after `make xuzz` and the
headless variables those 44 pass, 47 with the suites around them) and `zigzag_test` (269) pass;
`VqlIo*` and `JoinWorld*` are 274 passing.
