# Round 2 critique: Programming Language Design & Ergonomics Expert (vagent-native-agent.md v0.2)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 4 MAJOR, 5 MINOR.

Method: read the brief, SKILL §2.5/§3, my r1 and the disposition, then the v0.2 doc (§6, §10, §11,
§13), `design/vagent-agent-language.md` §6.6, §8.5–8.9, and the real front end
(`apps/common/xanadu/vagent/runtime/ast.hpp`, `parser.cpp`, `checker.cpp`, `wire.hpp`). Nothing was
built or run.

## Status of previous BLOCKING/MAJOR findings

- R1.1 (ABI incomplete, example not writable): PARTLY. The generic `vn_api` replaces the manifest
  gap, but it cannot set string lists or optionals and has no slot table (new B1).
- R1.2 (`t.scope`, Step args are runtime values): RESOLVED. `Terminal{token,term,rows,cols}`,
  `open_terminal(rows, cols)`, and `t.term` as a sequence argument are typed.
- R1.3 (stringly `[Step]` batch): RESOLVED. Typed named sequences replace it (§11.4).
- R1.4 (failure semantics, leak on failure, no `else` example): PARTLY. One failure rule and
  `finally` exist; no `else` example, the cause is unmatchable (M2), and the effect-call `else` the
  doc depends on is not implemented (M1).
- R1.5 (versioning, sentinels, variadics): RESOLVED. `version`/`size`, `vn_node{0}` invalid,
  `error()`, no variadics. Error codes themselves are still unenumerated (in B1).
- R1.6 (statement ops, brackets, handler vs plan): RESOLVED. Statements are `Stmt`/`Expr` nodes.
  Recovery is not expressible, see M1.
- R1.7 (FFI manifest gaps): PARTLY. `within`, `fail_if`, out-params, `consumed` and `in_` landed.
  The listing still omits `delscreen`; `cstring(n)` is unsafe and unbounded (M3, M4).
- R1.8 (authority surface): RESOLVED. Synthesized capacities, cause table, opaque token.

## BLOCKING

### 1. (§6.1–6.2, §13.1) `vn_api` cannot build the first test's own agent, and is not specified as "the whole ABI"

**Problem:** The setters cover str/int/float/bool/child/append, but `ast.hpp` has slot classes the
table cannot reach:

- **String lists.** `UsesDecl::capacities`, `SkillDecl::implements`, `CapacityDecl::extends`,
  `ModuleDecl::path`, `ImportDecl::path/names` and `PlanDecl::params` are `vector<string>`
  (`ast.hpp:300-306, 380, 386, 451, 469`). `append` takes a `vn_node`. §13.1's `uses Terminals` and
  `uses Curses` therefore cannot be recorded. Unless `set_str` on a list slot appends (unstated).
- **Optional slots.** `MethodSig::withinMs`, `result`, `HandlerDef::priority`,
  `EventTrigger::binder`, `ImportDecl::alias`, `Stmt::type`, `LifecycleDef::param`,
  `EventDecl::extends`. Unset versus present is not defined, and there is no way to clear.
  `withinMs` is required by §6.3 only `when isEffect`, a conditional rule with no slot-level
  notation.
- **Presentation flags** `hasParens`, `hasParams`, `hasBody`. They are in the tree, so §13.2 step 2
  (replayed `Unit` equals `parseUnit` of the source) compares them. The module must set them exactly
  as the parser would, and nothing says so.
- **Variant choice.** `Member`, `TopDecl`, `Expr`, `HandlerDef::trigger` are variants chosen by the
  child's kind. The doc gives no allowed-child-kind set per slot, so `set_child(EAwait...)` into
  `HandlerDef::trigger` is a runtime refusal nobody can predict. `Unit::module`/`imports` are not
  `TopDecl`, yet the only entry point is `add_decl`. `ModuleDecl`/`ImportDecl` are listed under
  "declarations" in the §6.2 table.
- **Enums and multi-field nodes.** Enums are bare integers, and `ELiteral` has five value fields
  that depend on `kind` (`durationMs`, `text`, `boolValue`). A module author gets raw `uint32_t`
  kind/slot numbers from a host-side `.def`; the doc never says `vagent_native.h` ships generated
  enums for them.
- **Errors.** "0 on success, nonzero otherwise": the codes are not listed, so a module cannot tell a
  bad slot from a bad value from a limit. The overwrite rule for setting a slot twice is missing.

The C++ layer used by §13.1 (`b.plan(..).forGoal`, `b.call`, `b.record`, `plan.let`, `.stmt`) is
described by name only, yet it is what the equality test exercises.

**Fix:** Generate `vagent_native.h` from `node_kinds.def` with `VN_KIND_*`/`VN_SLOT_*` enums. Print
the slot table in the doc (or an appendix): kind, slot, class
(`str|int|float|bool|enum|child|child-list|str-list`), required/optional, allowed child kinds. Add
`append_str`, `clear`, `add_import`/`set_module`. List the error codes. State overwrite semantics,
and which flags the layer sets.

## MAJOR

### 2. (§11.4, §13.1, `parser.cpp:1466-1499`) The `else` recovery the failure design depends on does not exist in the front end

**Problem:** `ECall` has no `orElse`. Only `EAchieve`, `EAsk` and `EAwait` carry one
(`ast.hpp:233-246`), and `parseOptionalElse` is called only for those. The specification's
`EffectRecovery` (`vagent-agent-language.md:1307, 1397`) is not implemented in the AST, the parser
or the checker (`checkElse` is reached only from those three). Because N4 says native ops describe
exactly "what vagent source can describe", a native module cannot express
`draw_greeting(...) else ""` today, and the doc is silent on it. The consequence for §13.1 is
concrete: when `draw_greeting` fails the plan fails and `close_terminal(t)` is never reached. The
token lives in the plan's frame, so no later plan for `Greet` can close it. The grant lives until
the agent stops (agent-scoped, NQ8 is open), so a long-lived agent that retries leaks one pty per
failure.

**Fix:** State the gap, and either propose it (add `ECall::orElse`, parser, checker; the slot
appears in the ABI for free) or scope grants to the intention now (NQ8 becomes a ruling). Show the
recovery form in §13.1 as `let line := draw_greeting(...) else ""` followed by `close_terminal(t)`.

### 3. (§11.4, §10.3) `ffi_error:<step>:<ret>:<errno>` cannot be used by an `else`

**Problem:** `cause` is a `Str` (`checker.cpp:2659`) and the language has `==`, `!=` and `++` on
strings and no prefix test, split or length. A plan can only compare the whole string, so it cannot
ask "was this a failed `refresh`" or "was errno EIO". `<step>` is undefined for a `do` step (no
name), and `<ret>` for a pointer-returning step is a number or `null`? The other foreign causes
(`ffi_range`, `ffi_argument`, `ffi_encoding`, `busy`, `native_protocol`) are scattered across §11.2,
§11.5 and §9; §10.3's table covers only authorities. The recovery contract of a sequence is
therefore unwritten.

**Fix:** Cause is a fixed vocabulary matched by `==` (`ffi_error`, `ffi_range`, ...), in one table
beside §10.3. The detail (step ordinal and name, `ret`, `errno`) goes to a declared event
(`ForeignFailed{capacity, method, step, ret, errno}`) and to the host log. Name every `do` step by
ordinal, or require a label.

### 4. (§11.2 vs §11.4) The sequence language is under-defined and contradicts its own declarations

**Problem:**

- **Step results.** "A function's result is a record of `ret` and its out-parameters" (§11.2), yet
  `sp := newterm(...)` is passed straight to `delscreen(sp)` as a `Screen`, and `back.s` is a field.
  Is a result a record only when out-params exist? A function with an out-param named `ret` clashes.
- **Missing declarations.** `delscreen` is not in the `fn` list, though `Screen destroy delscreen`
  and `finally delscreen(sp)` use it. No `fn` carries `consumed`, though `stdio.close` and the
  handle rule rely on it.
- **`finally` order.** The text says cleanups run "in reverse order", but the written list
  (`endwin, delscreen, close(inp), close(out)`) is already in reverse creation order, so reversing
  it again closes `out` first. A failure inside `finally` (e.g. `endwin`'s `fail_if eq -1`) has no
  rule. `finally` appears before `result`, but `result` must read `back` before cleanup frees it.
  Double cleanup with the handle's `destroy` rests only on `consumed`.
- **Typing.** `result` types against the `-> Str` signature only for a single scalar. No form for a
  record result, nor for a step whose result is `()`.
- **Syntax.** `let x = e` in a sequence versus `let x := e` in vagent. `out` is both a modifier
  keyword and a parameter name (`newterm(type, out: Stream, in_: Stream)`). Mixed positional and
  named arguments (`mvinnstr(y, x, n: 255)`) have no grammar.
- **Synthesis.** `Curses` and `CursesFfi` come from nowhere in §11.2, whose listing has
  `library ncurses` and no skill line. The library-to-capacity-to-skill rule is the R1.7(g) question
  again.

**Fix:** Give the mini-language a half page of grammar and rules: result shape, one evaluation order
for `finally` (written order, "each whose operands were bound"), failures in `finally` (logged,
ignored), `result` evaluated before cleanup, `:=` throughout, naming of the synthesized capacity
(`capacity <Library name TitleCase>`, `native skill <Name>Ffi`).

### 5. (§11.2, §11.4, §13.2 step 7) The example's read-back is wrong as written

**Problem:** `mvinnstr(y, x, n: 255)` on an 80-column screen reads from column 4 to the right margin
and returns blanks for empty cells, so the text is `"hello from a native agent"` followed by about
50 spaces. §13.2 step 7 asserts the exact string. The runner buffer is "`n` bytes", but ncurses
writes `n` characters plus the terminating NUL, so `n` bytes overflows by one. `n` is
agent-controlled (any Int), with no bound stated, so one effect can ask for a gigabyte. The same
`255` is written once for the buffer and once as the count, which the sequence mini-language cannot
derive from `text` because it has no expressions (not even `len`).

**Fix:** Runner allocates `n + 1`, bounded by a setting. Add one pure step expression, `len(x)`, or
a runner function such as `str.trim_end`, so the example reads back exactly `text`. Correct the
test.

## MINOR

- §13.1's source type-checks against the real checker: `let t := open_terminal(..)` is a legal
  suspension position, record-typed effect results travel as `WireRecord`, two `uses` lines are two
  `UsesDecl`, and `install HostTerminals()` plus `install CursesFfi()` share no capacity. Say so
  once, and say that only the `else` form (M2) is not available.
- §6.2 lists `ModuleDecl` and `ImportDecl` with declarations, yet `add_decl` takes `TopDecl` only.
- `fail_if` offers `null|eq|ne|lt`; `gt`, `le` are one line each and ncurses-adjacent libraries
  return `< 0` errors or `> n` counts.
- The `Terminal (grant)` type row hardcodes the record name; state the naming rule for a second
  authority kind (`<Kind>` TitleCase, `<Kind>Revoked`, `<Kind>Readable`).
- `close_terminal` is `effect ... within 1s` yet is used as the cleanup step of the only example;
  say what a plan does if it times out (the grant remains until stop).

VERDICT: NOT CONVERGED (1 BLOCKING, 4 MAJOR)
