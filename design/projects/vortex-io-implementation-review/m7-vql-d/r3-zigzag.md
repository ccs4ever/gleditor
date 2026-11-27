# Round 3 critique: Xanadu/ZigZag Purist (M7-VQL-d code)

Target: 57be2657 (round 3 fixes 1c78288a..57be2657). Focused round: the plain-script reclaim rule and
my r2 M1.

Tally: BLOCKING 0, MAJOR 2, MINOR 2.

Method: rebuilt `build/vquery` (`make -j$(nproc) vquery`), headless, XDG dirs under
`/tmp/claude-1000/zz3/`. Memory is peak RSS of one REPL fed N identical lines (python `-I`
harness, no `/usr/bin/time` here), over `tests/samples/xudu/core_hypertext/xanadoc_a`. Structure
survival was probed on `tests/samples/xuzz/slice_then_xanadoc` by counting a rank after an
unreturned `%`. No gtest binary was run.

## Status of round 2 findings

- **r2 M1 (plain query keeps its program, unbounded): PARTLY.** RSS at 16000 lines, r2 then now:
  `return join(##, ",")` 394 to 179 MB; `let $t := join(##, ","); return $t` 177 to 88 MB;
  `return ##/count()` 53 (at 8000) to 48 MB; `return 1` flat at 15 MB. Roughly halved, not flat. See M1.
- r2 m1 (capture table scan): RESOLVED, table and scan are gone. r2 m2 (`clearVariables`): RESOLVED
  by reading `vql_engine.cpp`. r2 m3 (lone foreign row pin): RESOLVED, test named in the
  disposition exists.

## Reclaim rule: what holds up

Read `QueryExecutor::advance` and `ProgramRun::reclaim` (`execution.cpp:331-400, 782-835`). Roots
are rows, published cells, closure environments, and `keepAttached_` cells; `reach` follows every
link inside the region, so a kept cell keeps everything it is linked to in the region. Deletion is
`DeletionMode::Repair`, so the half-links the question asks about do not dangle: a freed region
cell on a rank shared with outside cells is spliced out, and a region cell linked to a kept region
cell is itself kept. A cell structure linked on a user dimension to an outside cell (`#/d.1%"a"`,
`#/d.notes%"a"`, `#/d.7%"m"/d.8%"n"`) survives unreturned; I checked each by counting the rank
afterwards. `#` links are outside links and count. The P7 incarnation guards and `pruneAssemblyState`
cover slot reuse. An island of `%` cells linked only to each other and not returned or bound is
freed; that is unreachable garbage by every navigation, so I do not raise it. No R8 problem: a
released mark already truncated, and `keepProgram` covers the mesh evaluator.

## BLOCKING

None.

## MAJOR

### M1. The shipped REPL still grows without bound, and the disposition's premise ("rows freed by the caller, as a REPL does") is not true of `vquery`

**Problem.** The pins free every returned row after each run (`expectBoundedPlainRuns`). `vquery`
never does: `apps/vquery/main.cpp` has no `freeCell`/`deleteCell`; `lastResults = results`
overwrites and the old row cells stay live. A returned cell the program minted is a root, so it is
kept, so the arena grows by the row per run. Measured above: about 11 KB a run for a returned
`join`, 3 KB for a returned `count`, 0 for a returned store row or a boolean, which fits rows
retained by the REPL. I attribute by elimination (no cell counter reaches `vquery`), not by
instrumenting. The test also pins only `liveCells()` and `cellCount()`, not scratch bytes or
anything outside the cell slots.

**Failure scenario.** The r2 scenario unchanged in kind: one long `vquery` session of
`return join(##, ",")`; 16000 lines are 179 MB against 15 MB for `return 1`.

**Evidence.** `apps/vquery/main.cpp:~905-915` (`lastResults = results`, no free);
`tests/xuzz/vql_io_values_test.cpp:1431-1447` (the caller frees); `r3-disposition.md` ("rows freed by
the caller, as a REPL does"); design section 8c "grows the arena by nothing".

**Fix.** Either make the REPL retire the previous `lastResults` by the engine's `freeable` rule
(not bound, not mounted, no links), which makes the claim true, or say in 8c that the REPL keeps the
last result of every line, and drop "by nothing". Add a pin that runs the real `vquery` path or
states its scope.

### M2. The excluded-dimension list destroys user structure on `d.role`, `d.clone`, `d.name`, `d.vars`, `d.values`, `d.stores`, `d.stack` that no row or binding names, where the same query on `d.1` persists

**Problem.** `systemDimension` (`execution.cpp:318-324`) is a hand-written list of 17 `d.*` ranks,
local to the executor, duplicating knowledge of the VM's dimensions and defined nowhere the VM
could keep it in step with. Several members are not program-private ranks: `d.role` (xanadoc
`role`/`mime` projection, AGENTS), `d.vars`/`d.values` (the system://settings geometry), `d.clone`
(the user's `A><B` views, VQL section 4.6), `d.stores` (the multi-store rank), `d.name`. A link on
one of these to an outside cell is not treated as a root, so the cells are freed in the run that
made them. Before r3 the whole program was kept, so this is a regression, and it is silent.

**Failure scenario** (`slice_then_xanadoc`, headless): `return #/d.1%"a"/count()` then
`return #/d.1/count()` gives 1 (the `a` cell kept; baseline 0). The same pair with `d.vars`,
`d.stack`, `d.stores` gives 0 (baseline 0); with `d.clone`, `d.name`, `d.role` gives 1 (baseline
1, so not 2). Returning the new cell instead (`return #/d.role%"a"`) keeps it (count 2). Only the
unreturned form loses it, which is the shape of a mutating statement in a script ending in a
count or `return 1`.

**Evidence.** `execution.cpp:318-324, 351-362`; the "d.spin, d.cursors" comment at `:288` justifies
two ranks, not seventeen; `StructureAPercentMintedIsTheUsersNotTheNames` binds `$r`, so it passes by
the published-cell root and would pass with `keepAttached` deleted; no test covers an unbound `%` on a
non-`d.1` rank.

**Fix.** Narrow the list to what a program actually hangs its own cells from (`d.spin`, `d.cursors`,
`d.stack`/`d.step`/`d.grab` if the VM does), and expose it from the VM (`dims()`), not as a second
copy in `execution.cpp`; or treat only links to cells the program itself minted on those ranks as
internal. Add the unbound `%` pin on `d.role`, `d.clone` and an arbitrary user rank. Touching
`vortex_vm` for the accessor needs owner approval (P12 if so).

## MINOR

### m1. The comment above the keep decision describes a trail test that is not in the code

`execution.cpp:~808-812` says a run whose trail grew "is kept too". The decision uses
`keepAttached_`, not the trail (the disposition says the trail test was rejected). Fix the comment.

### m2. The attached-link rule has no direct pin

See M2 evidence: the only test that mentions it passes through another root. One unbound `%` on a
user rank, asserted after the run, would pin it.

VERDICT: NOT CONVERGED
