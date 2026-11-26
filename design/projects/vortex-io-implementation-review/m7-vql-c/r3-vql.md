# Round 3 critique: VQL Expert (M7-VQL-c code)

Target `e2b23837` (round 2 fixes `cb194c92..e2b23837`). Tally: 0 BLOCKING, 0 MAJOR, 1 MINOR.

`build/vquery` (09:51) postdates the last code commit `f59d0b6d`; the later commits are docs. Driven
headless (offscreen, dummy audio, XDG under `/tmp/claude-1000/r3vql`), REPL by piped stdin and batch
by `-e`. The twelve `VQueryE2ETest` Forget and Stale tests pass.

## Status of round 2 findings

- **m1 (plain-line stale refusal prefix): RESOLVED.** Plain lines print
  `Query failed: VQL_IO_STALE_BINDING: ...` for `io_tag`, `io_bytes`, `io_payload`, `io_captured`
  and a copy made by `let $y := $r`; no `RelationNext` text anywhere in the sessions run.

## `:forget` against its documented semantics

- **Drops uncovered exposures, keeps covered ones.** A session with `capture($r)` on read one and an
  uncovered `io_payload($r2)` on read two refuses `:save` (`NEEDS_CAPTURE`, 1 payload); `:forget`
  prints `Forgot 1 uncaptured payload; unbound $u $t`; a fresh query then saves.
- **Unbinds every non-handle variable, keeps handles.** `$t` (covered text) and an earlier plain
  `let $z := 5` are unbound (the documented price); `$c` (outcome) and `$src` (capability) stay;
  `io_tag($src)` still answers `Capability`.
- **Prints what it unbound; clears last rows.** After `:forget`, `:save` answers "Run a query before
  saving results."; a following `return $k` saves normally.
- **"Nothing to forget" otherwise.** Empty session, all-covered session, a second `:forget` in a
  row, and a session with no I/O runner (`vquery --headless`, variables are left bound there, and no
  row is cleared) all print it and change nothing.
- **Re-exposure counts again.** After `:forget`, reading a payload through a live handle exposes it
  and the rule refuses `:save` again.

## `VQLEngine::eraseVariable` side effects

Erases `env_[name]` only. `bindName` additionally erases `functors_` and `putGetLenses_`; this is
correct for erase because `bindLet` removes `env_[name]` whenever it records a functor, so a functor
name has no `env_` entry, is never in `session_.variables()`, and is never doomed. No mount, view or
functor table is touched; `clearVariables` remains the only wholesale clearer. Plain sessions
without a runner never reach it.

## Persist gate and stale bindings: no regression found

Uncovered `let`-carried text refused, covered accepted, partial capture refused, handle rows
refused, batch `-o` refusals as in round 2. A stale name is refused by tag on plain lines; a rebound
name is live; a stale `submit`/`capture` operand answers the typed code on a plain `effect`.

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. The names `:forget` prints come in hash order

**Problem.** `Forgot 1 uncaptured payload; unbound $w $t $u $z` lists names in unordered-map order,
so two runs of one session need not agree and the line is awkward to assert on. **Failure
scenario.** A script or test that compares REPL transcripts sees a different order after an
unrelated change. **Fix.** Sort `report.unbound` in `ScriptRunner::forget` before returning. Polish.

## Notes

- `return $y` of a stale copy printing the words "stale binding" is the known Vortex-lane marker
  item (the marker is still a row value on `return`); not repeated.

VERDICT: CONVERGED
