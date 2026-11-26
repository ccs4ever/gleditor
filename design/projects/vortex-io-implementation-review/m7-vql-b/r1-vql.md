# Round 1 critique: VQL Expert (M7-VQL-b, language correctness)

Target bda0e78b, scope 27768e54..bda0e78b. Tally: BLOCKING 0, MAJOR 2, MINOR 6.

Checked and found sound: grammar of `as $r` after the effect's closing brace, `within` after the
submit call, and the bare `await(`/`cancel(`/`open(` forms (selected by a `(` lookahead inside
`effect {`, so no clash with the `let` read form). No collisions: `def` wins over `isPathFunction`
(`compile_selector.cpp:278` before `:294`), and nothing in the tree defines io\_*/open/cancel. All
new actions are refused with VQL_IO_QUERY_CONTEXT in row/let/def/watch positions
(`vql_io_script_test.cpp:1156`). io\_* in a program with no provider fails typed
(`vortex_vm.cpp:891`). A script has no loop and `for` is row traversal, so "refused in `for`" agrees
with the plan's once-per-reached-action rule.

## BLOCKING

None.

## MAJOR

### M1. The persist-needs-capture rule does not cover `io_payload` rows

**Problem.** `VQL_IO_PERSIST_NEEDS_CAPTURE` is decided by `endsInAction`: the last statement is an
`IoEffectStatement` (`apps/vquery/main.cpp:628-631`, `:668`). M7-VQL-b added `io_payload($r)`, which
returns the file bytes as a text row from any later statement.

**Failure scenario.**
`vquery -o out.store --io-file src=f -e 'effect { ...read... } as $r; return io_payload($r)'` ends
in a `return`, so `endsInAction` is false and `resultsFromAction` is false. The file's bytes are
written into the permascroll with no capture decision. The same holds for `--in-place` and `:save`.
The e2e test at `vquery_e2e_test.cpp:565` runs exactly this shape but never adds `-o`, and no test
covers it. This breaks the standing rule "input is transient unless explicitly captured".

**Evidence.** `apps/vquery/main.cpp:628-631,668`; `apps/common/xanadu/vql/script_runner.cpp:384`
(the io_payload provider mints a text cell of the bytes).

**Fix.** Make the rule about provenance, not statement position. Set `resultsFromAction` whenever
`hasIoEffect(ast)` is true, or refuse a script that has an action and also persists. Add e2e cases
for `-o`, `--in-place` and `:save` over `effect{..} as $r; return io_payload($r)` and over
`return $r`.

### M2. `effect { open(..) }` without `as` leaks the opened file for the session

**Problem.** The runner closes only capabilities recorded in `Activation::opened`. The only writer
is the `io_open_capability` provider (`script_runner.cpp:540`), and the compiler emits that
`RelationNext` only inside `if (effect.resultVar)` (`compile_io_effect.cpp:186-203`).

**Failure scenario.** `effect { open("big.log") }` (the statement the plan's grammar allows) or
`open(..) as $x` never read. A Complete open mints an endpoint and capability that nobody records,
so `closeOpened` has nothing to close. The file descriptor and endpoint stay open until the host is
destroyed. Each REPL repetition leaks another one. This contradicts the plan's "every ending of the
script ... admits closeEndpoint for it". `TheOpenOutcomeIsTheRowAndTheNameIsTheCapability` only
tests `as $g`.

**Evidence.** `compile_io_effect.cpp:186-203`; `script_runner.cpp:529-541`;
`vql_io_script_test.cpp:1681-1693`.

**Fix.** Register the minted capability at the open action itself, independent of `as`. For example,
have the `io_open` await path always call `io_open_capability`, or have `endActivation` and `finish`
scan the admitted open outcomes through `ports_.capabilityOf`. Add a test with an unbound open and
`openedCount() == 0` afterwards.

## MINOR

1. **`$cap` after a failed open is the outcome, and the failure is a generic script failure.**
   `compile_io_effect.cpp:182-185` says a later submit is "refused by value". In fact `stepSubmit`
   fails the step with "Submit input is not a live CapabilityCell" (`vortex_vm.cpp:4095`), which
   surfaces as VQL_SCRIPT_FAILED (test `vql_io_script_test.cpp:1727` only checks the position). The
   comment is false, and the message does not say the open failed or give its tag. Pre-check in the
   submit staging, or in `admit`, for a variable bound by an open whose outcome was not a
   capability, and emit a typed `VQL_IO_EFFECT_ARGUMENT` naming the open's outcome tag.
1. **`--script-timeout` / `:timeout` accept any size.** `parseScriptTimeout` uses `std::stoll`
   (`main.cpp:62-70`), which throws `std::out_of_range` on a long digit string and aborts the
   process. A value above about 9e12 ms overflows `now() + timeout` at `script_runner.cpp:577`
   (steady-clock nanoseconds). `within` has the `maxWithinMillis` guard, but the script timeout does
   not. Apply the same representability guard and a typed usage error.
1. **`within` longer than the script timeout is silently capped.** With the 30 s default,
   `submit(..) within 60 s` is abandoned at 30 s with VQL_SCRIPT_TIMEOUT, not TimedOut as a value.
   Note the interaction in 4.13 and in `:timeout` help, or warn at the action.
1. **The persist rule over-refuses.** `endsInAction` is also true for a detached read (the row is a
   handle), `await`, `cancel` and `open` (`main.cpp:628-631`). That is conservative, but the
   diagnostic says "holds bytes read" for rows with no bytes. Say so, or set the flag from the
   result kind.
1. **Change-history row 16.6 is truncated.** `design/vql-query-language.md:2725` contains the
   unescaped `ms|s` pipe in a table cell, so the row ends at "within N ms" and loses the open,
   timeout and lastResults text. Escape the pipe or reword (and check mdl/mdformat).
1. **`opened` can hold a capability twice.** `openCapability` pushes on every provider call
   (`script_runner.cpp:540`). If the relation is re-evaluated (retry or backtrack), the script ends
   with two close admissions for one endpoint. Make `opened` a set.

VERDICT: NOT CONVERGED
