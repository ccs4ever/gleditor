# M7-VQL-d: values (match, owned await, `let` sources, join)

Status: built, gate pending. Slice d of the [roadmap](vortex-io-m7-vql-roadmap.md) §6.1; the surface
is in [`vql-query-language.md`](vql-query-language.md) §4.13 "Values (M7-VQL-d)". No new world
effect, no core file, no on-disk format.

## 1. Scope and the rulings it rests on

a1 outcome `match`, a2 `await($h) as $r` as an owned record, a3 a script-local `let` as a write
source, offset or length, a4 `join`, a5 a payload-less outcome constructor. Owner decision Q1
(2026-11-26): `match` is exhaustive, `_` allowed, a missing tag is a diagnostic before anything
runs.

## 2. Design

**Match (a1).** A new top-level statement `IoMatchStatement` (`ast.hpp`), not a desugaring into
`if io_tag($r) = "..."`. The desugaring was refused: it would pass a capability, a request or a
receipt to the fallthrough silently (`io_tag` answers `Capability`), and exhaustiveness would have
no place to live. The statement lowers (`compile_io_effect.cpp`, `compileIoMatch`) to one relation,
`io_match_tag`, which answers the tag ordinal or refuses a non-outcome as `VQL_IO_MATCH`, then one
`Eq` and `BranchFalseCell` per arm. Binders are the relations the path functions already use
(`io_payload`, `io_error`, `io_detail`, `io_uncertain`, and a new `io_cause` for `partial`), each
bound as a stream that is empty when the outcome lacks the field, so `match` adds one relation and
no opcode. An arm's body is one return item (`parseReturnItem`, extracted from `parseReturnClause`)
compiled as a one-item `return`, so a path or parenthesised value means what it means anywhere.
Exhaustiveness is static, in the parser, against `io_match.hpp`: the operation of `$r` is tracked
from the action that bound it with `as` (a read: twelve tags; a write, append or sync: eight; both
include `Rejected`, the admission refusal, and `Conflict`), `await($h) as $r` inherits the operation
of `$h`, and a name the parser cannot place (a session variable) must name all twelve or use `_`. A
handle, an open capability or a capture receipt is refused by name with what to do instead. A later
`let` or `for` that rebinds the name forgets it, which can only make the check stricter. A failure
the parser cannot know (a session variable that is a capability) is the same code at run time.

**Owned await (a2).** `await($h) as $r` and `cancel($h) as $c` copy the outcome into a script-owned
record through `io_own_copy`, which is `ownOutcome` with `retireRequest = false`
(`script_runner.cpp`): the request stays admitted, so a second `await($h)` finds it, and the
script's end retires it with the rest. Refused: retiring the request as a named read does, because
`handleArm` would then find a freed handle (the behaviour change m7-vql-c r1 refused). The budget
rule is unchanged: a copy past the retention budget leaves the name on the service's outcome.

**`let` sources (a3).** The runner's pre-flight read only session variables, so a `let` of the same
script was refused as unbound. Names bound by an earlier top-level `let` (before any `for`) now skip
that check, and the compiler (`ioBoundCell`) narrows such a name to exactly one value with two
asserts whose text leads with `VQL_IO_EFFECT_ARGUMENT`; `failureOf` turns a VM failure that starts
with a registered `VQL_IO_*` code into that typed diagnostic. Without the narrowing an empty or
several-valued `let` would be written as nothing, or as its own name. The Submit hook resolves
computed slots to their value cell (a `let` slot holds a reference) and `sourceBytes` reads a
constant's value when it has no spooled text. Offset and length are checked by the same `number()`
the hook always used.

**`join` (a4).** `join(rows, separator)` lowers in `compile_path_functions.cpp` to a loop of the
host relation `join_step(row, separator)` and a closing `join_end(separator)` (`join_relation.cpp`,
registered per execution beside `prose_hits`; no core file). The relations assemble a run of spans,
not a string: a row that quotes text of this arena contributes its own spans, a number or a
separator that is no cell's text is interned as scratch text, and a lone row that quotes text is
returned itself. The result is therefore a quotation of its sources with only the separators and
numbers new, as AGENTS.md asks of a cell's content, and `promote()` spools each scratch span as a
splice beside the quoted ones (the separator is the only new primedia). The running byte count is
the scan position, checked on every row against `maxJoinedTextBytes` (default 1 MiB, `limits.hpp`,
equal to the default write bound and asserted so in `script_runner.cpp`): a step past it is "no
tuple" and the program asserts `VQL_IO_BOUND`, so the text is never built past the bound, and
building is linear (the first cut folded `VqlAdd` and recopied a growing string). `""` for no rows.
`vquery --io-write-bytes` raises the bound with the write bound, since a join feeds a write.

**Payload-less constructor (a5).** `OwnedOutcome::summary` mints a record of the tag, the error
code, its detail and the uncertain flag plus the `owned` mark: no payload, observation or Progress
rank. `CaptureHost::advance` falls back to it when the full copy of a failed step is refused or
throws, where it previously fell back to a generic refusal that hid whether the stop was certain.

## 3. Refused alternatives

| alternative                                  | why refused                                                                                 |
| -------------------------------------------- | ------------------------------------------------------------------------------------------- |
| `match` as sugar over `io_tag` and `if`      | no home for exhaustiveness; a capability or receipt falls through silently                  |
| A `match` arm that holds an action           | a match is pure and cannot park; one site per script run                                    |
| Matching an `open` name or a detached handle | not outcomes; refused by name with the path function or the `await` to use                  |
| Retire the request on `await ... as`         | the second `await($h)` then fails on a freed handle                                         |
| Fold `VqlAdd` over `VqlString` for `join`    | round 1: retypes every row into fresh primedia and drops its origin (see 8)                 |
| Joined spans of a *foreign* row              | an arena cell's run has one namespace and `makeQuote` takes one space and no scratch; see 8 |
| A new opcode for text length                 | the one-slice-past-the-bound read needs none                                                |

## 4. Acceptance and tests

`tests/xuzz/vql_io_values_test.cpp`: grammar and static exhaustiveness (including the operation
tables and the refused names), compiled shape (one Submit, a match site that never waits), every tag
selecting its arm and binding its fields (twelve through detached cells, seven through the fake
backend), the stale and non-outcome refusals, exposure through binders, owned await and cancel with
a still-awaitable handle, `let` as source, offset, length and append source with typed refusals for
none, several and non-numeric values, `join` (separators, numbers, numeric-looking text, 10^4 rows
equal to the fold, the bound, a script write), and the summary constructor under arena pressure.
Leak pins of 10^4 runs: match, named await, join-and-write. Fail-before (run against the code with
the change reverted): exhaustiveness check off (2 tests fail), `io_own_copy` retiring (5 fail), the
`let` pre-flight exemption removed (1 fails).

## 5. As built

- **`io_cause`** is a new path function (the `partial` pattern's second binder). The cell store
  keeps the cause as a number; the runner names it.
- **`$why` of `partial` is the cause**, not an error code (a Partial has none).
- **`cancelled` and `timed_out` bind the uncertain flag**; `overflow`, `message_too_large`, `stale`,
  `conflict` and `unknown` bind nothing, and read the outcome through `$r`.
- **A script `let` is a session variable afterwards**, and the cells its program minted are freed
  when the name is rebound or erased (round 1, section 8); the join leak pin no longer frees by
  hand. The claim that this bounds the arena holds for a plain query too since round 3 (section 8c):
  before it, only a script with an I/O action reclaimed its program.
- **The capture-host fallback is unit-tested at the constructor** (fits where the copy is refused),
  not end to end: forcing a failed capture step under an allocation limit needs a harness the
  capture tests do not have. Slice f touches that code and should add it.
- **Stale names within a script**: a variable freed by this script's first admission is still read
  as the old cell until the script ends, as for every other reader; the typed refusal appears from
  the next script, which is what the existing tests pin.
- **`vquery`** constructs the runner with `maxJoinedTextBytes` equal to `--io-write-bytes` (round
  1). A join over about ten thousand rows still needs a higher `maxCycles` than the default 100000,
  which the slice-m constants audit should surface as a flag.
- Core files: none changed. No proposal.

## 6. Notes for slice e

Close is another action kind that names an outcome; `noteIoName` in the parser should record it (a
close's outcome is a write-class outcome) so `match` on it is exhaustive over eight tags. Closing a
host-bound name must leave it stale: reuse `reconcileBindings` and the stale marker rather than a
new state. `retireRequest = false` is the model for any action whose handle outlives its first
await.

## 7. Gate round 1 fixes

Reports: [`r1-vql.md`](projects/vortex-io-implementation-review/m7-vql-d/r1-vql.md),
[`r1-vortex.md`](projects/vortex-io-implementation-review/m7-vql-d/r1-vortex.md),
[`r1-zigzag.md`](projects/vortex-io-implementation-review/m7-vql-d/r1-zigzag.md); the per-finding
verdicts are in
[`r1-disposition.md`](projects/vortex-io-implementation-review/m7-vql-d/r1-disposition.md).

- **A read ends with `Conflict`** (the file adapter ends a raced read with it): twelve tags, and a
  read's exhaustive match must name `conflict`. A match without `_` now ends in a `VQL_IO_MATCH`
  refusal when no arm named the tag, so a table that lags an adapter cannot return an empty result.
- **A number is written as its canonical text** (R6), the rendering `join` and `string()` give it,
  so a numeric `let` source no longer writes nothing and reports `Complete`.
- **`await($h) as $r` of a handle an earlier script admitted** is an owned record: the lookup also
  searches the finished list.
- **A refusal's Rejected outcome graph is freed when its submission retires** (`retire()`), for a
  plain detached refusal as well as `await ... as`; pinned at 10^4 runs.
- **`join` keeps what its rows quote** (section 2); the cost is one relation call a row.
- **A rebound or erased `let` frees the cells its program minted** (`publishCompilerBindings` takes
  the minted cells, found by incarnation against the pre-compile mark; I/O cells are skipped; a cell
  the new bindings, other names, the rows the caller holds, the dynamic-view registry or a mount
  still name is kept; deletion is `ArenaManifold::deleteCell`, the P7 path).
- Minors fixed: `summary` keeps `cause`; an unbound name in `match` says it is unbound; a bare
  binding tag is told to write `complete(_)`; the per-row bound; `--io-write-bytes`.

## 8. Round 1 refused alternatives and open prices

- **Multi-row `join` over foreign rows is copied text.** A row that is a store's cell (every row of
  a loaded store in `vquery`) is a proxy whose spans are addressed in its own store's scrolls; an
  arena cell's content run has no per-span namespace, and `makeQuote` takes one space and no scratch
  span. So a lone foreign row is returned itself (identity kept, `-o` writes the same operations as
  `return ##`, checked with `xudu-dump`), and several foreign rows are joined as interned text.
  Keeping them quoted needs a core change to `ArenaManifold` (a mixed-namespace run or a quote with
  a scratch separator): proposal P11, not made here. **The owner approved P11 on 2026-11-26** as its
  own small core slice, `M7-VQL-d2 (P11)`, after this gate and reviewed by the zigzag and Vortex
  critics (roadmap §5, §6.1b). Until it lands, several foreign rows are interned text.
- **`join` now needs a host that registers `join_step` and `join_end`** (`QueryExecutor` does); a
  bytecode file from `vqueryc` runs only under such a host, as `find` already requires `prose_hits`.
- **Double retention charge** (an owned record charges the chunk its service outcome also holds) is
  deferred: charging once needs lease identity in the budget, and the failure is a budget-edge
  fallback to the service outcome, never an overrun.

## 8b. Gate round 2 fixes

Reports: [`r2-vql.md`](projects/vortex-io-implementation-review/m7-vql-d/r2-vql.md),
[`r2-vortex.md`](projects/vortex-io-implementation-review/m7-vql-d/r2-vortex.md); verdicts in
[`r2-disposition.md`](projects/vortex-io-implementation-review/m7-vql-d/r2-disposition.md).

- **A replaced `let` frees only a cell nothing else reaches.** "Minted by this program" was not
  "owned by this name". A cell is freed only when it has no link on any dimension (`%` and path
  programs build structure the user keeps) and no closure of any program captured its name (the
  compiler reports the captured names; the engine keeps those cells, by incarnation, and never frees
  them). Price: a cell a closure captured is not reclaimed when its name is rebound.
- **`join_step` and `join_end` outlive the run that registered them.** They were registered on the
  shared VM with an undo that nulled them, so one script's end unregistered them under a parked
  script. There is no undo now; each begin registers again, which is safe because a walk is reset at
  its first step and no park falls inside one.
- **An await of a handle another parked script admitted** is searched in that script's list too.

## 8c. Gate round 3 fixes

Reports: [`r3-vql.md`](projects/vortex-io-implementation-review/m7-vql-d/r3-vql.md),
[`r2-zigzag.md`](projects/vortex-io-implementation-review/m7-vql-d/r2-zigzag.md); verdicts in
[`r3-disposition.md`](projects/vortex-io-implementation-review/m7-vql-d/r3-disposition.md).

- **A freed `let` cell is unreachable from every root, by construction.** Rounds 1 and 2 guessed
  what a closure holds from the names its program mentioned (`capturedNames`, removed). A closure
  value records the cells of its environment (`VortexVM::closureValue`), so the executor reports, at
  each publish, every closure among the bindings and rows with the cells it captured (and the
  closures those capture), and the engine keeps that table pruned to what a binding or row reaches.
  A replaced cell is freed only when no binding, row, mount or registry entry names it, no reachable
  closure captured it, and it has no link. A closure made through a lambda parameter
  (`\($s) \($y) $s + $y`) is covered, which a name could not be.
- **A plain query's program is reclaimed like an I/O script's**: the rows and the published bindings
  are the roots, and everything else the run allocated is freed when it ends. The program is kept
  when something names it that is not a cell root: a functor or mount binding, a closure value among
  the results, a dynamic-view registry entry; and a region cell linked on a dimension that is not
  the VM's own to a cell outside the region (what a `%` builds) is a root too, so structure a query
  attaches to the space survives. Measured at 10^4 runs over four rows (live cells added per run):
  `return join(...)` 266, `let $t := join(...)` 115, `return $rows/count()` 46, all now 0 (pinned by
  `VqlIoPlainReclaimTest`; before the fix the three tests fail with those figures).
- `clearVariables` frees what the bindings minted, as `:forget` does, and drops the closure table.
- **Prices.** A cell a live closure captured is not reclaimed until the closure is dropped (the
  table is exact, so it is freed at the next publish or `:forget` after that). A program whose
  results or bindings include a closure value keeps its whole region. A session that holds a closure
  walks its bindings on each publish that frees a cell, and each publish that makes a lambda or
  follows a closure; a session without closures pays nothing (the table is empty). Cells of a parked
  script's program that a second script rebinds are not tracked: the scripts' names are not visible
  to each other until one ends (section 6), so the shape is unreachable through the script surface.

## 9. Change history

| version | date       | change                                                             |
| ------- | ---------- | ------------------------------------------------------------------ |
| 0.1     | 2026-11-26 | Design and as-built for M7-VQL-d; gate pending                     |
| 0.2     | 2026-11-26 | Gate round 1 fixes (sections 7 and 8); join rewritten              |
| 0.3     | 2026-11-26 | Gate round 2 fixes (section 8b); P11 approved as slice d2          |
| 0.4     | 2026-11-26 | Gate round 3 fixes (section 8c): closure-sound free; plain reclaim |
