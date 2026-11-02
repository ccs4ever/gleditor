# Round 4 critique: Programming Language Design & Ergonomics Expert (vagent-native-agent.md v0.4)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 0 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings

- R3 M1 (§11.6 `ReadKey` / `Keyboard` type-checking): RESOLVED. §11.6 provides a complete `Keyboard`
  agent declaring `predicate last_key(code: Int)`, `var screen: Opt<Terminal> := none`, goal
  adoptions, and `plan read for ReadKey` with `if key then revise last_key(key)`. The `read_key`
  sequence is typed over `libc` `fgetc` with `none_if eq -1` returning `Opt<Int>`. This parses and
  type-checks against `parser.cpp:777-787, 1357-1369` and `checker.cpp:1147-1180, 1557-1590, 1760`.
- R3 M2 (`curses_keep` vs N13 grant scoping): RESOLVED. N13 and §10.1 define agent-scoped grants via
  `hold_terminal(t)`. §13.1 specifies that `curses_keep` calls `hold_terminal(t)` to keep the grant
  across intentions and quiescent checkpoints, aligning with §13.2 steps 8–9 where the original's
  terminal remains valid and revocation is recorded as `agent_ended` when the agent ends.

## BLOCKING

None.

## MAJOR

None.

## MINOR

- §6.4 illustrates the C++ wrapper with unqualified free-function style calls (`agent("Greeter")`,
  `plan.let("t", call("open_terminal", {lit(24), lit(80)}))`), whereas §13.1 lines 1064–1072 use
  member functions on builder `b` (`b.plan(...)`, `b.call(...)`, `b.lit(...)`, `b.name(...)`,
  `b.emit(...)`, `b.record(...)`). Align §6.4 with §13.1 by prefixing with `b.` to avoid confusion
  over whether node constructors are member methods on `b` or free functions.
- In §13.1, the C++ snippet begins `auto plan = b.plan("greet").forGoal("Greet");`, but in the
  accompanying vagent source and AST, `plan greet for Greet` is an agent member inside
  `agent Greeter`. Clarify how `plan` is registered with the agent builder (e.g. `ag.member(plan)`
  or `ag.plan(...)`) so the resulting op log reproduces `AgentDecl::members` rather than a top-level
  `PlanDecl`.
- In §11.4 Rule 3 and §10.3, foreign call failures post a `NativeCallFailed` percept. §10.1 or §11.2
  should explicitly list `event NativeCallFailed` (with fields
  `method: Str, step: Int, ret: Int, errno: Int`) among the synthesized declarations so that an
  agent's `on NativeCallFailed` handler type-checks against `checker.cpp` without needing an
  explicit user declaration.
- In §11.4 Rule 1, `n` is passed as an argument to `mvinnstr(y, x, len(text))`, where `n` sizes
  `cstring_out(n)`. Clarify explicitly that out-parameters are skipped during positional
  argument-to-parameter matching, so that the 4-parameter signature `(y, x, s, n)` binds the 3
  arguments `(y, x, len(text))` directly to `(y, x, n)`.

VERDICT: CONVERGED
