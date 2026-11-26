# Round 3 critique: VQL Expert (M7-VQL-d code)

Target: 7dc93822 (round 2 fixes 0fa2c336..7dc93822). Focused round. `build/vquery` rebuilt
(`make -j$(nproc) vquery`); REPL scenarios run headless with the XDG dirs under
`/tmp/claude-1000/r3vql`. `VqlIo*:JoinWorld*` (274 tests) pass on the current `build/xuzz_test`.

Tally: BLOCKING 0, MAJOR 1, MINOR 0.

## Status of round 2

- M1 (freeing replaced `let` cells breaks closures and `%`-built structure): PARTLY. Both round 2
  scenarios are fixed (`$g(1)` stays `abc!` and `cdcd!` after `let $t := 1`; `#/d.1/count()` stays
  `3` after `let $r := 0`). Variants that hold: a `def` capturing `$t` (`h("!")` is `k!` after a
  rebind, the name binds at call time); a closure passed as an argument to another closure
  (`$h($g, 1)` is `abc!` after `let $t := "Q"`); capture by alias (`$u` captured, then `$u` and `$t`
  both rebound: `abc!`); two names captured in one script and rebound twice; rebind inside a script
  line (`let $t := "y"; let $t := "w"`) and on plain REPL lines; `:forget` (a no-op for a captured
  cell, nothing freed wrongly); a `%` structure still reachable through a second name. One
  closure-reaches-a-freed-cell path remains (M1 below).

Regression checks (clean): the join providers registered at each `begin` do not disturb a second
script (the new parked-script test passes; `join` rows and the per-row bound behave as in round 2).
`ownOutcome` searching the other live activations changes nothing in the VQL lane: the 274 `VqlIo*`
and `JoinWorld*` tests pass.

## BLOCKING

None.

## MAJOR

### M1. A closure that captures a lambda *parameter* bound to a `let` cell still reads a freed cell

**Problem.** The round 2 fix protects the cells of the session *names* a frame captures
(`CompilationResult::capturedNames`, `compiler.cpp:545-549`, resolved to cells in
`publishCompilerBindings`, `vql_engine.cpp:~200-215`). A closure can also capture a cell through a
lambda parameter: `\($s) \($y) $s + $y` captures `$s`, which is not a session name, so
`capturedNames` does not carry it, yet the argument cell it is bound to is the cell a session name
holds. The arena records no such reference, so `freeable` (`vql_engine.cpp:271-283`) sees the cell
as owned, unnamed, unlinked and uncaptured, and frees it on the next rebind of the name.

**Failure scenario.** In `vquery`:

```
let $t := "abc"
let $mk := \($s) \($y) $s + $y
let $k := $mk($t)
$k("!")          -> abc!
let $t := "N"
$k("!")          -> !        (expected abc!)
```

The surviving closure `$k` silently reads a deleted cell as the empty text, the same defect class as
round 2 M1. It reproduces with a joined text (`let $t := join(#/d.1%%, "abc")`, `$k("!")` goes from
`abc!` to `!`), and it does not need the rebind to name `$t` again: any `let` of `$t` suffices.
Passing a closure through a closure (`$h($g, 1)`) is safe only because `$g` captured the name `$t`
directly.

**Evidence.** `apps/common/xanadu/vql/compiler.cpp:545-549` (capture set = frame free-capture names
only, which are session names); `apps/common/xanadu/vql/vql_engine.cpp:271-283` (`freeable` consults
only `named`, `capturedCells_`, incarnation and links). No test in `VqlIoLetFreeTest` binds a
let-cell through a parameter of a closure-returning lambda.

**Fix.** Protect by what a closure holds, not by what a program names. Either (a) when a closure is
made, record the cells of its captured environment (parameters included) in the session's
`capturedCells_` by incarnation, which the engine can read from the closure value at publish; or (b)
the narrow option: free only the join/string result cells the 10^4 pin is about (unlinked scratch
text no closure value can hold, i.e. when no live binding holds a closure at all), and skip the free
whenever any session binding is a closure. Add a test for the scenario above, with and without a
joined text.

## MINOR

None.

VERDICT: NOT CONVERGED
