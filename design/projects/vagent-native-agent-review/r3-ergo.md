# Round 3 critique: Programming Language Design & Ergonomics Expert (vagent-native-agent.md v0.3)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 2 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R2.1 (ABI gaps, B1): RESOLVED. Status enum, `append_str`, `set_module`, `add_import`, slot classes
  with once/optional rules, generated constants, `PolicyArg`/`ArbitrationClause` (§6.1-6.2). The
  allowed-child-kind column is "the table lists" and is not printed, which is acceptable for a
  generated `.def`.
- R2.2 (effect `else` absent from the front end): RESOLVED. Prerequisite Q2, and N13 removes the
  design's dependence on it.
- R2.3 (cause unmatchable): RESOLVED. Fixed vocabulary in §10.3, detail on `NativeCallFailed`.
- R2.4 (sequence language under-defined): RESOLVED. Grammar and six rules (§11.4); `delscreen`
  declared; `:=` throughout; `consumed` added; `finally` order and failure-in-cleanup stated.
- R2.5 (read-back): RESOLVED. `len(text)` and `n + 1` buffer bounded by `max_out_bytes`.

## BLOCKING

None.

## MAJOR

### 1. (§11.6, `checker.cpp:1557-1563, 2659`; `parser.cpp:777-786`) The `ReadKey` example does not type-check

**Problem:** Checked against the real parser and checker, the snippet fails in three places.

- `revise last_key(key)` goes through `checkTerm`, which raises `UnknownPredicate` unless a
  `predicate last_key(...)` is declared (`checker.cpp:1558-1563`). The doc declares none, nor says
  who does (the module must record a `PredicateDecl`).
- `screen_terminal` is called "an agent `var`", but a `var` requires `:= <init>`
  (`parser.cpp:783-785`). A `Terminal` has no default: the only way to initialise it is to forge a
  `Terminal{token: "", ...}` record. The honest form is `var screen_terminal: Opt<Terminal> := none`
  (the checker unwraps `Opt` on read, `checker.cpp:535`), and something must set it. Nothing in
  §11.6 does, and `TerminalReadable{token}` carries a `Str` that cannot be turned into a `Terminal`,
  so the handler cannot rebuild it from `r`. `r` is also never used.
- `read_key`'s signature is not shown, and a sequence ending in `getch()` with `fail_if eq -1`
  (§11.2) fails `ffi_error` exactly when no key is available under `nodelay(true)`.

The same snippet has a design hole that the type check hides. N13 releases the grant when the
top-level intention that staged `open_terminal` ends. A `ReadKey` adopted from a handler is a
different top-level intention, so the terminal it reads belongs to an intention that must still be
alive. The doc says a long-lived terminal "holds it in a long-lived intention, such as a maintain
goal's" (N13), but §11.6 shows no such goal, and the `curses_keep` variant (§13.1, "keeps its
terminal in an agent `var`" and "does not close it") would, as a `perform` goal, lose its grant at
plan end with cause `intention_ended`.

**Evidence:** `checker.cpp:1558-1563, 535, 2659`; `parser.cpp:777-786`; doc §11.6, N13, §13.1.

**Fix:** Make §11.6 a complete fragment: `predicate last_key(key: Int)`;
`var screen_terminal: Opt<Terminal> := none`; a `maintain` goal (or the keeper intention) that opens
the terminal, assigns the var, and stays alive; `read_key`'s one-line sequence with a `fail_if` that
tolerates `ERR` (or a `getch` wrapper returning `Int` with no `fail_if`); and drop the unused
`as r`. State in §13.1 that the keeper and `ReadKey` need not share an intention only because the
keeper is long-lived.

### 2. (§13.2 steps 8-9, N13, §13.1) `curses_keep` contradicts the intention-scoped grant it is meant to test

**Problem:** Step 8 says "the original's terminal still works" after a checkpoint and resume, and
step 9 expects, for `curses_keep` after the original stops, a `granted` decision and one
`agent_ended` revocation. Under N13 a `curses_keep` built like `curses_hello` (a `Greet`-style
`perform` goal) has its grant revoked `intention_ended` when the plan finishes, before the harness
checkpoints, so step 8 would see `stale` on the original too and step 9's ledger row would read
`intention_ended`. The test passes only if `curses_keep` keeps a live intention (a maintain goal),
which the doc never states, and it is the same hole as finding 1.

**Evidence:** N13 text (doc lines 249-256), §13.1 last paragraph, §13.2 steps 8-9.

**Fix:** Specify `curses_keep`: a `maintain` goal holds the intention open; the checkpoint is taken
while it is quiescent; its source is shown in a short block like §13.1's. Then the expected rows
(`agent_ended`) are correct, and the test also covers N13's long-lived-intention escape hatch.

## MINOR

- §11.4 rule 1 says arguments are positional, yet `mvinnstr` is declared with four parameters
  (`s: cstring_out(n)`, `n`) and called with three (`mvinnstr(y, x, len(text))`). State that out
  parameters are not arguments, and how `n` is matched to the `cstring_out(n)` it sizes.
- `NativeCallFailed` is an event the runner posts, but §11.4 does not say it is among the
  synthesized declarations (only the change history line 1287 does), so an `on NativeCallFailed`
  handler would hit `unknown event` unless the replay declares it. One sentence in §10.1.
- `step` in `NativeCallFailed` is "from 1" and counts `do` and `let` steps; say `finally` steps are
  not counted and that a cleanup failure does not post the percept (it is only logged).
- The doc says the `Terminal` record carries `{token, term, rows, cols}` but an agent can write
  `Terminal{token: "x", term: "x", rows: 1, cols: 1}` as a record literal. Fine because the token
  fails `stale`, but state it, since a reader will ask.
- `watch_terminal` is `within 1s` and returns `()`; the example uses it as a bare statement at the
  end of a plan. Say once that its failure (for example `stale`) fails the plan and so stops the
  re-arm loop.

VERDICT: NOT CONVERGED (0 BLOCKING, 2 MAJOR)
