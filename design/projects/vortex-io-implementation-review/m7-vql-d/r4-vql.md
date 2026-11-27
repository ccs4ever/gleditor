# Round 4 critique: VQL Expert (M7-VQL-d code)

Target: 2e9e4178 (changes since round 3: 1c78288a..2e9e4178). Focused round. `build/vquery` rebuilt
(`make -j$(nproc) vquery`, up to date); REPL scenarios run headless with the XDG dirs under
`/tmp/claude-1000/r4vql`. `VqlIo*:JoinWorld*:VQL*` (1154 tests) pass on the current
`build/xuzz_test`.

Tally: BLOCKING 0, MAJOR 0, MINOR 0.

## Status of round 3

- M1 (closure capturing a lambda parameter bound to a `let` cell reads a freed cell): RESOLVED, by
  removal of the replaced-`let` free (owner decision: reclamation moves to M7-VQL-d3 with P12). No
  cell is freed on a rebind, so there is nothing to read freed. Reproduced in `vquery`:
  - `let $t := "abc"; let $mk := \($s) \($y) $s + $y; let $k := $mk($t); let $t := "N"; $k("!")`
    prints `abc!`.
  - The joined-text variant (`let $t := join(#/d.1%%, "abc")`, then `let $t := 1`) prints `abc!`.
  - Round 2/3 variants: closure capturing a name (`$g(1)` stays `abc!` after `let $t := 1` and after
    `let $t := "Q"`); `def h($y) := $t + $y` binds at call time (`k!` then `Q!`); alias
    (`let $u := $t`, both rebound, `$u` still `Q`); `%` structure (`#/d.1/count()` stays `3` after
    `let $r := 0`); `:forget $u` leaves the alias's cell intact and later reads of `$u` refuse
    cleanly (a parse error, no stale text).
  - The same through an I/O script (an `effect { ... } as $r` line that calls the closure, which
    takes the reclaim-around-rows path): `$k("!")` is `abc!` after `let $t := "N"`, and a closure
    made on the I/O line (`$z`) reads `N2`/`M`-consistently (`N1`, then `N2` after `let $t := "M"`,
    the name binding at call time as it did before slice d).

## Regression checks (clean)

The revert returns `vql_engine.{hpp,cpp}` to the pre-slice files; `match` (arms, `Conflict`,
fall-through), numeric write sources, `join` rows and identity, `summary`, the persist gate, and
stale-binding refusal all pass their suites (`VqlIoMatch*`, `VqlIoJoin*`, `JoinWorld*`,
`VqlIoLetRebind*`, `VqlIoPlainProgram*`, `VQLSemanticsRepairTest`, 10 disabled d3 tests only). The
`closureReached` rule (`execution.cpp:~690-695`) keeps a program whose rows or bindings hold a
closure value; checked against an I/O script holding a closure, no freed read. The `ownOutcome`
change (no search of other parked scripts) matches the visibility rule it cites: another script's
parked names are not visible, so the shape cannot be written.

## BLOCKING

None.

## MAJOR

None.

## MINOR

None. The documented unreclaimed growth (join 266 cells a run, closure redefinition 165, unfreed
REPL rows 267) is owner-deferred to d3 and is not a finding.

VERDICT: CONVERGED
