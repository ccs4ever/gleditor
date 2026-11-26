# Round 3 critique: Xanadu/ZigZag Purist (M7-VQL-c code)

Target `e2b23837`. Tally: 0 BLOCKING, 0 MAJOR, 0 MINOR.

`build/vquery` was current (linked after `f59d0b6d`; `make vquery` found nothing to rebuild). Every
run was headless with XDG redirected into `/tmp/claude-1000/zz3`.

## Status of earlier findings

- **Round 2 N1, plain `let $y := $ra` aliasing a reused slot: RESOLVED.** Reproduced the round 2
  script: `$ra` read (`Complete`), `let $y := $ra; let $z := $y; return io_tag($y)` (`Complete`), a
  second script reading `$rb` (`EndOfStream`), then `io_tag($y)`, `io_tag($z)` and `io_tag($ra)`
  each answer `VQL_IO_STALE_BINDING`, with no `RelationNext` text. The chained copy `$z` is covered
  too. A `let` made in the same script as its read goes stale the same way.
- **Round 1 M1, stale session-variable aliasing: RESOLVED.** Both the named-read case and the `let`
  copy now answer the typed code, and the stale name given to `capture` or `submit` is typed (round
  2 m1, per the disposition's tests).

## Checks in the lane

- **`IoCellTag::StaleBinding` marker: pure enough.** It is a tagged I/O cell minted through the same
  `mint`/`transact` path as every other I/O cell (`io_cells.cpp:776-780`), runtime-only, so no
  consumer can take it for text and no pointer comparison is needed. `IoCellTag` carries the
  discrimination. No core file changed (`git diff cb194c92..e2b23837` touches only `io_cells.*`
  under `vortex/`). Freed at `~ScriptRunner` after `reconcileBindings()` and
  `forgetStaleVariables()` unbind every name still naming it, so the longer-lived engine never holds
  a name for a freed cell.
- **`:forget`: sound.** Run against a script that read, bound `$t := io_payload($r)`,
  `$u := string($t)` and a closure `$f` over `$t`: `:save` refused with
  `VQL_IO_PERSIST_NEEDS_CAPTURE`, no store written; `:forget` printed
  `Forgot 1 uncaptured payload; unbound $f $u $t`; `$t` and `$u` then print nothing and `$f(1)`
  fails (the closure is gone, so no side route to the text through `functors_`); `:save` after a
  fresh query wrote a store. `active.primedia` held only the structural dimension names (`d.result`,
  `d.source`, `d.editions`, ...) and not one byte of the payload (the pre-`:forget` `:save` was
  refused and appended nothing). The last rows are cleared (`Run a query before saving results.`
  after a forget).
- **Captured data survives.** `capture($r)` then a full-range `let $t := io_payload($r)`: `:forget`
  says `Nothing to forget`, `$t` still prints, `io_captured($c)` and `io_tag($c)` answer, the
  capture store holds one translation, and the permascroll holds the 25 captured bytes once. A
  partial capture (`capture($r, 0, 4)`) plus a whole-range text variable: `:save` is refused twice,
  `:forget` unbinds `$t`, the next `:forget` says nothing to forget, and `:save` of a later
  `io_captured($c)` query wrote one row. The permascroll holds `AAAA` (the captured 4 bytes) and
  structure names, none of the uncaptured remainder.
- **No new unlink loop or side table.** `forget()` and `forgetStaleVariables()` loop over
  `session_.variables()` collecting names then call `eraseVariable` (an `env_` erase); neither
  breaks links or splices cells. `bound_` (name to incarnation) predates this round; the diff adds
  only `ForgetReport` (a `vector<string>`, a return value) beside it.

No findings.

VERDICT: CONVERGED
