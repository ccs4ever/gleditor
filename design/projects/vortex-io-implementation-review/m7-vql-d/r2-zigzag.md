# Round 2 critique: Xanadu/ZigZag Purist (M7-VQL-d code)

Target: 7dc93822 (fixes since round 1: 52d120d2..7dc93822). Lane as round 1, plus the narrowed `let`
free (`capturedCells_`), the join paths and their documentation.

Tally: BLOCKING 0, MAJOR 1, MINOR 3.

Method: rebuilt `build/vquery` and `build/xudu-dump` (`make -j$(nproc) vquery xudu-dump`), ran them
headless with XDG dirs under `/tmp/claude-1000/zz2/`. Memory was measured as peak RSS of one REPL
session fed N identical lines (stdin), against `tests/samples/xudu/` stores. No gtest binary was
run, and no cell counter is reachable from `vquery`, so a growth figure below is RSS, not
`liveCells()`; the mechanism is read from the code.

## Status of round 1 findings

- **M1 (join retypes rows, drops origin): RESOLVED.** `join_relation.cpp` assembles one run of the
  rows' own spans plus interned separators (`spansOf`, `joinEndProvider`), and a lone quoting row is
  returned as itself (`walk->only`). Checked with `xudu-dump`: `vquery -o ... 'return ##'` and
  `... 'return join(##, ",")'` over `xanadoc_a` write identical operations ending in the same
  spans, and the permascroll grows by the same 109 bytes in both (11037 to 11146); round 1 saw 63
  extra bytes and a lost origin cell. The one-namespace multi-row path is pinned by
  `QuotingRowsAreJoinedAsTheRunOfTheirOwnSpans` and `ANumberBesideAQuotingRow...` (separator and
  number are scratch spans). `promote()` gives a scratch span an address only there
  (`arena_manifold.cpp:3365-3390`, first span `makeCell`, the rest `spliceCell`), so the separator
  spooling on promote is by the existing road and a mixed quote/scratch run is carried. The
  limitation (several foreign rows are interned text, a lone foreign row keeps identity) is stated
  truthfully in design section 2 and the section 8 table, naming P11 as owner-approved and unbuilt.
  Not a finding. I could not make a multi-row store from the shipped samples (each `xanadoc` has
  one row), so the multi-row path rests on the unit tests, not a CLI run.
- **M2 (a rebound `let` is never freed): PARTLY.** Sound for the pattern the slice is built for,
  not for a plain REPL query; see M1 below.
- **m1, m2, m3 of round 1:** not re-examined (the zigzag lane had no round 2 fixes for them).

## Soundness of the narrowed `let` free

Read `publishCompilerBindings`, `freeable`, `releaseOwned` and the `advance()` ownership filter
(`execution.cpp:672-693`). The four conditions hold up: the incarnation guard makes a reused slot
harmless, the filter admits only cells newer than the pre-compile mark and not I/O cells, the
no-links test keeps `%`-built structure, and the keep-set covers other names, rows the caller
holds, views, lenses, shape handles and mounts. A cell linked after the `let` is caught because
links are read at free time. I found no use-after-free path: a closure's captured cell is
protected by name resolution at the publish of the program that built the closure, and the free
goes through `deleteCell(LeaveBroken)` (P7). The table is pruned by incarnation on each publish.

## BLOCKING

None.

## MAJOR

### M1. A plain REPL query that returns or binds a freshly computed cell (`let $t := join(...)`, `return join(...)`) keeps its whole compiled program every run: unbounded, ~25 KB a run

**Problem.** `let` freeing narrows what one name owns, but the arena region of the program that
minted the cell is not reclaimed for a script with no I/O action. `advance()` keeps the mark
(`settleMark(!(noMutations && noNewResultCells))`, `execution.cpp:~716`) because the result cell is
new, then calls `keepCompiledProgram()` for any script with no action
(`execution.cpp:~736-739`); only a script with I/O actions takes `reclaimOnClose` around its rows
and published roots. So the freed `$t` cell is one cell out of the program's hundred-odd, and the
rest stay live for the session. This is the "~114 live cells a run" the round 2 fixer saw; it is a
real unbounded leak, not a driver artefact. The mechanism is older than the slice (a computed
`##/count()` shows it), but `join` is a slice d deliverable whose intended use is `let $t :=
join(rows, sep)` (design section 2 a3), and design section 8 says the freed cell makes the leak
"bounded"; for a plain query it is not.

**Failure scenario.** A `vquery` REPL, one long session, the line `let $t := join(##, ",")`
repeated, or `return join(##, ",")`. Peak RSS (N lines): `return 1` 18.5 MB at 16000, `return ##`
21 MB at 16000 (flat), but `return join(##, ",")` 214 MB at 8000 and 394 MB at 16000;
`let $t := join(##, ","); return $t` 62 MB at 4000 and 177 MB at 16000; `let $t := "abc"` 55 MB at
16000; `return ##/count()` 53 MB at 8000. Growth is linear in runs, not in text size, so it is the
program region (one join program is about six times a `count`), and it never returns. No
I/O-action variant was cleanly measurable (the second run of a repeated `open ... for write create`
refuses), so the claim for scripts with actions rests on the code path (`reclaimOnClose`).

**Evidence.** `execution.cpp:716-744` (mark kept for a new result cell; `keepCompiledProgram` for a
script without actions); `execution.cpp:672-693` (frees only the `let` cell);
`design/vortex-io-m7-vql-d.md` section 8 and the "A script `let` is a session variable" bullet
(claims the rebinding bounds the leak); the pin
`ManyJoinAndWriteRunsLeaveTheArenaAndTheServiceBounded` runs a script with I/O actions, so it never
exercises this path.

**Fix.** Either run the same `reclaimOnClose(rows + publishedCells)` for a plain script whose
program minted only cells the roots reach (the rows and `publishedCells` are already the keep
list), or record the leak in design section 8 and the REPL help as a refused price, with the
number above, and correct the "bounded" claim. The first is an `execution.cpp` change (not a core
file); the unconditional keep for `noNewResultCells == false` may be the reason it was avoided, in
which case state that. If neither is done, this stays open.

## MINOR

### m1. A captured name rebound each run is never freed, and the table scan is linear

`capturedCells_` keeps the old cell of a captured name for as long as that cell lives, and the cell
lives because it is in the table (`vql_engine.cpp`, `freeable`). A script that rebinds a captured
`$t` and rebuilds its closure every run (`let $t := join(...); let $g := \($x) $t + "!"`) adds one
entry a run that is never pruned, and `erase_if` walks the whole table on every publish. Measured:
the capturing script took 30 s for 4000 runs against 16 s for the same script with a closure that
does not capture `$t`; memory was about equal, because the closure program already dominates (M1).
It is the documented price in the round 2 disposition, and the entry is dominated by M1, so it is
not raised. Note it in design section 8 with the quadratic scan, or drop the entry when the closure
that made it is itself replaced.

### m2. `clearVariables()` leaves `ownedLets_` and `capturedCells_` behind

`VQLEngine::clearVariables()` (`vql_engine.cpp:132`) clears `env_` but neither table. The
incarnation guards make a stale entry harmless, but a session that clears variables never frees the
cells they named, and the entries stay. `eraseVariable` does release; make `clearVariables` do the
same or say why not.

### m3. The join doc names no pin for a lone foreign row

Design section 8 says a lone foreign row is returned itself and that `-o` writes the same
operations as `return ##`, "checked with `xudu-dump`". I reproduced this by hand and it holds, but
no test pins it (the unit tests use arena-local rows, `quotedHome`, never a loaded store's row).
Add a `JoinWorld` test over a row of a loaded store, so the claim does not rest on a manual check.

VERDICT: NOT CONVERGED
