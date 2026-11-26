# Round 1 disposition (M7-VQL-d code)

Target `301b9a8d`. Reports: [`r1-vql.md`](r1-vql.md), [`r1-vortex.md`](r1-vortex.md),
[`r1-zigzag.md`](r1-zigzag.md). All six MAJORs were reproduced against the code and fixed; none was
rejected. No core file changed; one proposal (P11) is recorded for the part of zigzag M1 that needs
one. Every new test was run against the pre-fix code and failed there (see the last section).

## MAJOR findings

| finding   | verdict   | fix commit | where                                                                                |
| --------- | --------- | ---------- | ------------------------------------------------------------------------------------ |
| vql M1    | confirmed | `4fde1533` | `io_match.hpp`, `compile_io_effect.cpp`                                              |
| vql M2    | confirmed | `4fde1533` | `script_runner.cpp` (`sourceBytes`)                                                  |
| vortex M1 | confirmed | `7c2ee0a1` | `script_runner.cpp` (`ownOutcome`)                                                   |
| vortex M2 | confirmed | `7c2ee0a1` | `script_runner.cpp` (`retire`)                                                       |
| zigzag M1 | confirmed | `da55759b` | `join_relation.{hpp,cpp}`, `compile_path_functions.cpp`, `execution.cpp`; partly P11 |
| zigzag M2 | confirmed | `e90123a3` | `vql_engine.{hpp,cpp}`, `execution.cpp`                                              |

**vql M1.** `Conflict` added to the read set (twelve tags). The recheck of each class against what
its adapters can produce: write, append and sync end in `Complete`, `Partial`, `Failed`,
`Cancelled`, `TimedOut`, `Conflict` (generation CAS in `file_source_adapter.cpp`,
`source_adapters.cpp`) and `Rejected`, `Unknown` by the service, matching the table; a read
additionally ends `EndOfStream` and, through the raced-read branch, `Conflict`; `Overflow`,
`MessageTooLarge` and `Stale` are produced by no shipped adapter but belong to the plan's read
vocabulary and stay; open, close and capture bind capability, receipt or an outcome the parser
already refuses to match by name. The runtime guard is an `Assert` after the last arm of a match
with no `_` (`VQL_IO_MATCH`, with the statement position), so a table that lags an adapter again is
a refusal, not an empty result.

**vql M2: canonical text, not a refusal.** A number written as a write source is its canonical text
(R6 shortest round-trip, the rendering `join` and `string()` already use). Reasons: a script that
builds `let $t := $count + 1` and writes it means the digits; a refusal would force `string($t)`
boilerplate for what every other consumer of the value does implicitly; and the result is
deterministic, which is all R6 asks. What is refused stays refused: no value, several values.

**vortex M1.** `ownOutcome` looks the entry up in the current activation and then in `finished_`, so
a handle an earlier script left running yields an owned record. The record is released with the next
admission and the name then answers the typed stale refusal. Test: the cross-script await, with
`ownedRecordCount() == 1` (0 before) and the stale refusal from the script after.

**vortex M2.** The refusal's Rejected outcome graph is freed in `retire()`'s request-less branch,
after the submission is released (the root-cause fix the report asked for); the explicit free in
`ownOutcome` is gone. Pins at 10^4 runs for `await ... as` on a refusal and for a plain detached
refusal (about 5 and 6 cells a run before).

**zigzag M1: identity kept, with one priced exception.** `join` is now a loop over two host
relations, `join_step` and `join_end`, registered per execution beside `prose_hits` (a non-core
file). A row that quotes text of this arena contributes its own spans; a number or a separator is
interned as scratch text; a lone quoting row is returned itself. `promote()` already spools a run
per span (first span as is, the rest as splices), so only the separator becomes new primedia.
Checked end to end: `vquery -o` of `join(##, ",")` over the sample store writes the same operations
and grows the permascroll by the same 113 bytes as `return ##` (before: a fresh `home` and no origin
cell). The bound is checked on every row and the run is built once at the end, which also fixes the
quadratic-copy MINOR.

The exception, **P11**: a row that is a store's cell in the composite arena (every row in `vquery`)
is a proxy whose spans are addressed in its own store's scrolls. An arena cell's content run has no
per-span namespace and `makeQuote` takes one space and no scratch span, so a join of *several*
foreign rows cannot be one quoting cell without a core change to `ArenaManifold` (a mixed-namespace
run, or a quote that admits a scratch separator). Those rows are joined as interned text, which the
design records as an explicit refused alternative with its price. A lone foreign row keeps its
identity. **P11 (proposal, not made): let an arena cell's content run carry per-span namespaces, or
let `makeQuote` take a scratch span, so a multi-row join over a loaded store stays a transclusion.**

**zigzag M2.** `publishCompilerBindings` takes the cells each binding's program minted (found by
incarnation against the pre-compile mark; I/O cells are skipped, so a handle or an owned outcome is
never freed here) and frees the previous ones through `ArenaManifold::deleteCell` (the P7 path, no
new unlink loop) when the name is rebound, replaced by a functor or erased. A cell the new bindings,
any other name, the rows the caller holds, the dynamic-view registry or a mount still names is kept
(the registry guard was found by an existing test the first cut broke). The manual free in the 10^4
pin is removed and the pin passes.

## MINOR findings

| finding                                       | disposition                                                                                           |
| --------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| vql 1: quadratic join, bound checked at end   | fixed with zigzag M1 (linear, per-row bound)                                                          |
| vql 2: `--io-write-bytes` and the join bound  | fixed: `vquery` builds the runner with `maxJoinedTextBytes = writeBytes`                              |
| vql 3: unbound name in `match`                | fixed: `$never is not bound to an outcome; name one with as first`                                    |
| vql 4: bare `complete`                        | fixed: the arity message tells the `complete(_)` form                                                 |
| vortex 1: double retention charge             | deferred: charging once needs lease identity in the budget; the edge is a fallback, never an overrun  |
| vortex 2: no position on run-time failures    | partly: the new unmatched-tag assert has its statement; `let` and `join` failures deferred to slice f |
| vortex 3: capture fallback not end to end     | deferred to slice f as the design says (needs a capture-step allocation-limit harness)                |
| zigzag m1: `summary` drops `cause`            | fixed, with a test on a summarised `Partial`                                                          |
| zigzag m2: binder and field-label consistency | deferred: the field list is private to `owned_outcome.cpp`; a test would scan source text             |
| zigzag m3: match scalar owner                 | fixed as a comment naming the reaper (the executor's mark), pinned by the existing 10^4 match run     |

## Tests

New tests: 16 in `tests/xuzz/vql_io_values_test.cpp` (conflict grammar and run cases, unmatched tag,
bare-tag hint, unbound name, numeric `let` source with four expressions, cross-script await, two
refusal leak pins, four join identity cases, summary cause) plus the 10^4 join pin made honest.
`VqlIo*`, `JoinWorld*`: 269 pass. Fail-before (all source under `apps/` reverted, tests kept): 15
tests fail, among them every new one except the lone-number join and the operation-of-await case,
which hold both ways.
