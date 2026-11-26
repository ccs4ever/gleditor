# Round 2 critique: VQL Expert (M7-VQL-d code)

Target: 09635a6c (round 1 fixes 52d120d2..09635a6c). Focused round. `build/vquery` rebuilt
(`make -j$(nproc) vquery`); every scenario run headless with the XDG dirs under
`/tmp/claude-1000/r2vql`, in the REPL (a session is where rebinding is observable).

Tally: BLOCKING 0, MAJOR 1, MINOR 0.

## Status of round 1

- M1 (read tag set omits `Conflict`): RESOLVED. A read match naming eleven tags is refused with a
  message listing `conflict` among the missing ones; the twelve-tag form and the `conflict` arm
  parse. The fall-through `Assert` (`VQL_IO_MATCH`) follows the last arm of a match without `_`.
- M2 (numeric `let` write source writes nothing): RESOLVED. `let $t := 42`, `1 + 2`, `2.5`,
  `join(1, ",")` and `0.1 + 0.2` write `42`, `3`, `2.5`, `1`, `0.30000000000000004`; `Complete`
  reports the matching byte count.

## Regression checks

- `join` as host relations: sound. `join(1, 2)` is `1`; the bound is per row (`--io-write-bytes 8`
  with a script `let $t := join(...)` gives `VQL_IO_BOUND` before the text is built); a lone row is
  returned itself; an empty walk is `""`. The persist exposure gate still holds:
  `join(io_payload($r), "-")`, bound by `let` or returned, with `-o` gives
  `VQL_IO_PERSIST_NEEDS_CAPTURE` (rows pass through `io_payload`, which exposes). The walk's "never
  interleaves" claim holds because rows and separator are computed before `#JOIN_BEGIN`. The
  `vqueryc` requirement is stated in design section 8 and matches the `find`/`prose_hits` precedent.
- A name aliasing a replaced cell (`let $u := $t; let $t := 1; $u`) keeps its text, as do
  `let $m := $m` and `let $n := id($n)`.

## BLOCKING

None.

## MAJOR

### M1. Freeing replaced `let` cells breaks a surviving closure and unwires structure a `%` minted

**Problem.** `publishCompilerBindings` frees every cell the replaced binding's program minted unless
a name, a row the caller holds, a view or a mount names it (`vql_engine.cpp:227-276`). "Minted by
this program" is not "owned by this name": a cell can be reached through paths other than those the
`named` set covers. Two such paths are reproducible in the REPL.

**Failure scenario 1 (closure capture).** `let $t := "abc"` then `let $g := \($x) $t + "!"`; `$g(1)`
prints `abc!`. `let $t := 1` frees the cell `$g`'s capture snapshot refers to (the closure cell is
`$g`'s, the captured value cell is `$t`'s); `$g(1)` now prints `!`. Same with a joined text
(`let $t := join(#/d.1%%%, "cd")`, `$g(1)` goes `cdcd!` to `!`). The closure survives, its snapshot
reads a deleted cell as the empty text, silently: a surviving name observes a freed cell. Before the
fix the snapshot kept the cell.

**Failure scenario 2 (structure the user created).** `let $r := #/d.1%%%` then `#/d.1/count()` is
`3`; `let $r := 0` and `#/d.1/count()` is `0`. `%` is VQL's cell-creation sugar; the three cells
were linked along `d.1` from `#` and are reachable from the root, not only through `$r`. Rebinding
an unrelated-looking name deletes them (`deleteCell` with `LeaveBroken` breaks the links), so a REPL
user's built structure disappears. Before the fix the count stayed 3.

**Evidence.** `apps/common/xanadu/vql/vql_engine.cpp:227-276` (`named` covers env, other owned
names, `keep`, view registry and mounts, not closure captures and not cells linked into the space);
`apps/common/xanadu/vql/execution.cpp:672-690` (ownership = incarnation newer than the pre-compile
mark and not an I/O cell, which includes structure minted by `%` and captured cells). Missing test:
nothing in `VqlIo*`/`JoinWorld*` rebinds a name a closure captured or a name holding `%` cells.

**Fix.** Own only what is provably the binding's alone: a cell that is not linked on any dimension
(no neighbours) and that no closure capture graph names. Concretely, skip a candidate that has any
link (`%` and path-mutating programs) and scan the live closure cells' capture records for the
candidate, or retain a count of captures when a closure is made. If that makes the free too
conservative for the 10^4 pin, narrow it to the join/string result cells (unlinked scratch-text
cells), which is what the leak finding was about. Add tests for both scenarios above.

## MINOR

None.

VERDICT: NOT CONVERGED
