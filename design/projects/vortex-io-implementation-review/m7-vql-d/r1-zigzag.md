# Round 1 critique: Xanadu/ZigZag Purist (M7-VQL-d code)

Target: 301b9a8d (diff 0e6a672d..301b9a8d). Lane: cells and dimensions against side tables or
serialised text, the `OwnedOutcome::summary` record shape, the `match` tag set, `join` and
transclusion identity, the 2-degree rank invariant, P7 deletion reuse, R8, permascroll policy, and
the `let` leak.

Tally: BLOCKING 0, MAJOR 2, MINOR 3.

Method: read the diff and design; ran `build/vquery` (rebuilt by the VQL critic, has `io_match_tag`)
and `build/xudu-dump` headless, XDG dirs under `/tmp/claude-1000/zz/`. `build/xuzz_test` predates
the slice (no `JoinWorld`), so no gtest was run.

Checked and found sound (no finding):

- **R8.** Nothing in the diff touches `Store` or emits an operation. `summary`, `io_match_tag`,
  `io_cause`, `join` and the `let` narrowing allocate only in the arena or the VM's value slots.
- **2-degree rank rule.** `summary` writes through the same `appendIoField` on Occurrence/State that
  `copy` uses, one field cell per label with its value on State (`owned_outcome.cpp:134-145`). No
  new dimension, no fan-in.
- **P7.** `summary` frees through the existing `freeOutcomeGraph` (`owned_outcome.cpp:171-204`,
  `deleteCell(..., LeaveBroken)`), and `await ... as` records go through `releaseRecords` and
  `retire`. There is no new unlink loop.
- **Mark discipline of `summary`.** It releases the mark on every failure and on a throw
  (`owned_outcome.cpp:131-151`), as `copy` does.
- **Tags.** `io_match_tag` answers an integer from a `switch` (`script_runner.cpp:1475`), so the tag
  is a computed scalar, not text parsed from a cell. Binders reuse the field relations already in
  the tree. No side table.
- **Permascroll writes by `join`.** `join` mints no primedia in the permascroll while the script
  runs. `-o` is the only way it reaches the permascroll (M1).

## BLOCKING

None.

## MAJOR

### M1. `join` retypes its rows into fresh primedia and drops their origin, even for a single row

**Problem.** `join` is a fold of `VqlAdd` over `VqlString` (`compile_path_functions.cpp:366-398`).
Both opcodes build `std::string` values (`vortex_string.cpp:119`, `:158`). The result cell has no
span run naming where the bytes came from. When it is promoted, it is spooled as new text. AGENTS.md
says a cell's content is a run of spans and cells stay quotable; here a quotation becomes retyped
text. The design (§2 a4, §3) never mentions this and lists no refused alternative for it. The
alternative would be to build the result as a multi-span content run, with a separator span between
rows, the way `setContent` allows (`arena_manifold.hpp:947`). The one-row case needs no
concatenation at all, and still copies.

**Failure scenario.** `return ##` over a document cell, saved with `-o`, writes the cell's own
permascroll span `[4,8)` and an origin cell `st#cell=2` (`o2`, ops 9 and 11). `return join(##, ",")`
over the same single row writes a new `home` at `[142,146)`, appended to `active.primedia`, and no
origin cell (`o1`, ops 9-10). The permascroll grew from 112 to 175 bytes for four bytes of text that
already existed. The Spanfilade cannot discover a transclusion between source and result, and the
author's permascroll now holds those bytes as authored text. For captured payload cells (the case
the slice is built for) the same path turns captured, provenanced spans into the author's own
typing, which is the transcopyright hazard the capture journal exists to prevent.

**Evidence.** `compile_path_functions.cpp:366-398`; `vortex_string.cpp:130-170`. Repro:
`vquery --permascroll $PS -o $O $ST -e 'return join(##, ",")'`, then
`xudu-dump --section=ops --permascroll=$PS $O`, against the same run with `return ##`.

**Fix.** At minimum, a row count of one returns the row cell itself (no copy). Better, assemble the
result by `setContent` from the rows' spans plus one interned separator, so quoted rows stay quoted
and only the separators are new. If the owner accepts a computed-text result, add the refused
alternative and its price to design §3 and to `vql-query-language.md` §4.13, and say in the join doc
that its result is new text, not a quotation.

### M2. A `let` rebound each run is never freed, and `join` makes the leaked cell up to 1 MiB

**Problem.** `VQLEngine::bindName` overwrites `env_[name]` without freeing the old cells
(`vql_engine.cpp:424-429`), and the slice makes `let $t := join(...)` the intended way to feed a
write (design §2 a3). Each rerun leaves the previous `$t` cell, and the string it owns in the VM
value slots, alive in the arena. Nothing reclaims it between scripts (`execution.cpp:381-386` rolls
back only the pre-compile mark, and the `let` is published after it).

**Failure scenario.** A long `vquery` REPL session runs the same
`let $t := join(rows, ","); ... write_bytes(0, $t)` script 1000 times with `--io-write-bytes` raised
to the join bound. Each run leaves one `$t` cell holding up to 1 MiB of text. That is roughly 1 GiB
for a repeated one-line script, and `liveCells()` grows by one per run.

**Evidence.** `vql_engine.cpp:424-429`; design §5 "A script `let` is a session variable afterwards
... leaves one cell a run". The pin `ManyJoinAndWriteRunsLeave...`
(`vql_io_values_test.cpp:1040-1071`) frees the old `$text` binding by hand ("the test plays the
owner", `:1048-1055`), so it measures what a runner would leave if it did the freeing, not what it
leaves. The design calls this "engine behaviour", but the slice turned a small leak into a
bounded-only-by-1-MiB one and wrote a test that hides it.

**Fix.** Free the cells a `let` rebinding replaces. The runner already tracks bound handles in
`noteBindings`/`reconcileBindings`; the same place can free a replaced non-handle `let` cell once no
parked activation reads it. Change the pin to run without the manual free. If that is a core change,
make it a numbered proposal `P10`.

## MINOR

### m1. `summary` drops `cause`, so a summarised `Partial` binds nothing for its `why`

`kept` is `{errorCode, stableDetail, uncertain}` (`owned_outcome.cpp:123`). A `Partial` carries
`cause` (`arena_io_cell_store.cpp:245`). `io_cause` (`script_runner.cpp:757-767`) then answers an
empty stream, so `partial($p, $why) => $why` yields no row, silently. The design says the summary
exists so a failed capture step "must still say what it was" (`owned_outcome.hpp:62-71`). Add
`cause` to `kept`. The unit test pins `uncertain` and `owned` but not a `Partial`.

### m2. The `Read` tag set and its comment claim the shared outcome's field set without a pin

`ioMatchFieldsOf` (`io_match.hpp:129-147`) gives `Overflow`, `MessageTooLarge`, `Stale`, `Conflict`
and `Unknown` no binders. Design §5 records this, but `Partial` is the only tag whose cause has a
name relation. Not a defect in itself. Add a test that fails when a new field label appears in
`arena_io_cell_store.cpp` without a binder or a recorded refusal.

### m3. `io_match_tag` mints a scalar cell per match run with no stated owner

`matchTag` answers `query.arena.makeScalarCell(...)` (`script_runner.cpp:1475`). The leak pin
(`vql_io_values_test.cpp:686-715`) passes because the compiled program's slots are reaped by the
mark, but nothing in the code states that. Add a comment naming the reaper, or answer a constant
cell the compiler already holds.

VERDICT: NOT CONVERGED
