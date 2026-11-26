# Round 3 disposition (M7-VQL-d code)

Target `7dc93822`. Reports: [`r3-vql.md`](r3-vql.md) and, from the zigzag critic's second round,
[`r2-zigzag.md`](r2-zigzag.md). Both MAJORs and the three zigzag MINORs were reproduced and fixed.
No core file changed; P12 is not needed.

| finding   | verdict   | where                                                                                    |
| --------- | --------- | ---------------------------------------------------------------------------------------- |
| vql M1    | confirmed | `vql_engine.{hpp,cpp}` (`closureEnvs_`, `heldByClosures`), `execution.cpp`, `compiler.*` |
| zigzag M1 | confirmed | `execution.cpp` (`advance`, `ProgramRun::reclaim`)                                       |
| zigzag m1 | fixed     | the name table and its linear scan are gone (below)                                      |
| zigzag m2 | confirmed | `VQLEngine::clearVariables`                                                              |
| zigzag m3 | fixed     | `VqlJoinForeignTest.ALoneRowOfALoadedStoreIsReturnedItself`                              |

## vql M1: a closure holds cells the program never names

Name-based patching failed twice because the question "what does this closure hold" was answered
from the source text. It is answered from the value now: `VortexVM::closureValue(root)` returns the
cells of a closure's captured streams, and the executor, whenever a program compiled a lambda or the
session already holds a closure, reports every closure among the rows and published cells with those
cells, following captured closures (`closureEnvironments`). The engine keeps the table
(`closureEnvs_`), pruned on each use to the closures a binding or a row reaches, and a cell is freed
only when it is unreachable from every root: bindings, rows, mounts, registry, and every cell a
reachable closure captured, transitively. The compiler's `capturedNames` and the engine's
`capturedCells_` are deleted. The conservative option (no free while any binding is a closure) was
not needed: the VM can enumerate the environment through its public read API, so the free stays
precise, and the price is only that a captured cell lives as long as its closure.

Reproduced before the change (`$k("!")` after `let $t := "N"` read the freed cell), also for a
joined text and for a closure passed through a closure. Tests, `VqlIoLetFreeTest`: the r3 scenario,
the joined variant, a closure passed to another closure, `:forget` of the closure then a rebind
frees the cell (and not before), and `clearVariables`.

## zigzag M1: a plain query kept its whole program

Only a script with I/O actions took `reclaimOnClose`. A plain script now does too, with the same
roots (rows plus published cells), unless something names the program that is not a cell root: a
functor or mount binding, a closure value among the results or bindings (its descriptor and frames
are raw references into the region), the dynamic-view registry (`keepIfNamed`, unchanged), or the
mark having been released already (nothing left). A test of the trail for mutating queries was tried
and rejected: the conditional trail grows for every program, so it says nothing. The test used is
structural: a cell of the region linked, on a dimension that is not one of the VM's own (`d.spin`,
`d.cursors`, ...), to a cell outside the region is a root, so the `%` structure a query attaches to
the space survives (`StructureAPercentMintedIsTheUsersNotTheNames` passes). The first attempt
treated any outside link as a root and kept the whole cursor chain (`d.cursors` links each program's
cursor to the previous one); the system-dimension exception is what makes the reclaim complete.

Live cells added per run, 10^4 runs over four rows (arena `liveCells`, rows freed by the caller, as
a REPL does): `return join($rows, ",")` 266 before, 0 after; `let $t := join(...); return $t` 115
before, 0 after; `return $rows/count()` 46 before, 0 after. Slot counts are bounded too. The pins
are `VqlIoPlainReclaimTest` (three bounded, one that rows and a bound text survive the reclaim, one
that a closure among the results keeps its program); all three bounded fail before the change.

## Minors

- **m1.** `capturedCells_` and its `erase_if`-per-publish scan are gone. `closureEnvs_` is empty in
  a session with no closure (no walk at all), and is otherwise pruned to what is reachable; a
  closure that is replaced drops out of it at the next publish.
- **m2.** `clearVariables` clears the closure table and frees what the bindings minted by the same
  `freeable` rule as `:forget`, then clears `ownedLets_`.
- **m3.** A row of a loaded `Store` (an `ArenaManifold` over its manifold) is returned itself by a
  one-row `join`, with its content untouched.

## Side effects of reclaiming plain programs

- The mesh evaluator snapshots the arena to export the compiled program, so it sets the new
  `QueryExecutionOptions::keepProgram` (default false); six functor-frame tests that read the
  program's cells after the run depend on it.
- `VQLSemanticsRepairTest.EmptyWalkOnlyRunsArgumentFreeBuiltinCount` pinned "a refused query leaves
  nothing" with the arena's slot count, which only ever grew while programs were kept; it now uses
  the live-cell count, which is what the test means and which holds.

## Verification

Headless, after `make -j$(nproc)`: `xuzz_test` 5266 passed, `zigzag_test` 269 passed. New tests:
five `VqlIoLetFreeTest`, four `VqlIoPlainReclaimTest` (three bounded pins at 10^4 runs), one
`VqlJoinForeignTest`; fail-before (the source change reverted, tests kept): eight fail, the
foreign-row identity test passes both ways (it pins a claim that already held).
