# Round 2 critique: VQL Expert (M7-VQL-b), target 1a2f94c0

Tally: BLOCKING 0, MAJOR 0, MINOR 2. VERDICT: APPROVE (converged).

## r1 status

- M1 (persist rule): CLOSED. `resultsFromAction = hasIoEffect(ast)`
  (`apps/vquery/main.cpp:639,673`), checked by `-o`/`--in-place` (`:725`) and `:save` (`:917`). A
  failed action script clears `lastResults` and `hasResults` first (`:646-648`). Carry-over across
  scripts: the only variables the REPL sets are `lens` and the `--io-file`/`:open` capabilities
  (`:573`, `:638`), and no VQL syntax names `lastResults`. A cell-id literal was tried by hand in a
  REPL session (`return #314`, `#314`) over an `io_payload` row's id: it returned an empty result,
  so no later plain script reached the bytes. No route found.
- M2 (unbound open): CLOSED. The capability relation is emitted for every open
  (`compile_io_effect.cpp:183-204`); `as` only decides the binding.
- MINOR failed-open message: CLOSED. `failedOpen` (`script_runner.cpp` settle) names `$name`, the
  tag and `VQL_IO_EFFECT_ARGUMENT`. Opens are counted in statement order, matching the relation's
  evaluation order.
- MINOR `--script-timeout` parse: CLOSED. `from_chars` plus a `maxDeadline` range check
  (`main.cpp:66-76`); the runner saturates `now() + timeout`.
- MINOR within vs timeout: sound. The deadline becomes max(current, now + within + min(timeout,
  maxDeadline)); the sum is at most half the tick range, so no overflow. It does not defeat the
  timeout: the request's own deadline still ends it as a TimedOut value, and the script cannot run
  past within + timeout. A script that never uses `within` keeps the plain timeout. Accepted.
- MINOR opened twice: CLOSED (dedupe on insert).
- MINOR history row: NOT FIXED, see N1.

## New findings

N1 (MINOR). `design/vql-query-language.md` row 16.6 is still truncated: the unescaped pipe in
`--script-timeout MS|off` ends the table cell, so the row stops at "`vquery --script-timeout MS`".
Escape the pipe or reword (`MS or off`).

N2 (MINOR). The extended script deadline is never lowered when a `within` request finishes early, so
a later unbounded action in the same script can hang for up to the whole `within` plus the timeout
(`script_runner.cpp` admit, `std::max` on `current_->deadline`). Acceptable and bounded by what the
author wrote; note it in 4.13 or accept.

Reviewed and sound: closeOne/unclosed\_ retry logic, `retireAll(finished_)` moved before the first
admission, expireIfDue cancel ordering, `IoHost::now` as the one clock.
