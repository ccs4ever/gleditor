# vagent review, round 7 - ergonomics and surface-language critic (focused round)

Document: design/vagent-agent-language.md (2118 lines, v3.4). Read: my R6 critique, the R6
disposition, and the sections the R6 fixes touched: section 8.1 (newline rule), 8.2 (l.1036-1051),
8.8 (l.1197-1201), 8.9 grammar, and the 12.3 and 12.4 examples. Both showcase examples were
re-parsed by hand against the grammar, the newline rule, and the new typing rules.

Tally: 0 BLOCKING, 0 MAJOR, 3 MINOR.

## Round-6 BLOCKING / MAJOR findings

- R6 #1 (type of `:=`, `<-`, `for`, `while`; arms of `if`): RESOLVED. l.1048-1051 state that
  assignment, `for` and `while` succeed with `()`, that arms of `if/else` share a type with `fail`
  and `return` matching any, and that a statement-position `if` discards its arms. The 12.3 line
  `if not charging then charging := adopt ...` and the `for ... do if ... then {..} else drop i`
  line are both well typed under these rules. Chained assignment parses (`Assign` is right
  recursive) and is rejected by the checker, which the text says.
- R6 #2 (entry point): RESOLVED. `module/Agent(literals)` removes the `:` ambiguity (l.1197-1201),
  and 12.4 now has `Dispatcher` spawn `Warehouse` and `Courier` in `on Initialize`, launched by
  `vagent run demo:depot/Dispatcher(40, 5s)`, which uses only literals and a duration.

## BLOCKING / MAJOR

None found. The new rules introduce no grammar conflict and no example is rejected.

## MINOR

- Dangling `else`: `if a then if b then x else y` is not stated to bind to the nearest `if`. One
  sentence in 8.2.
- R6 minors on `effect def` outside `capacity`, `let x := a & b` versus `x := a & b`, a timer
  example, `return` in plans, and newlines outside a `Block` remain unaddressed. None blocks an
  implementer.
- `emit E in s for x` with the Action-level `for` modifier next to the `for ... do` loop on a `;`
  joined line is parseable only by lookahead for `do`; a note in 8.9 would help.

VERDICT: CONVERGED
