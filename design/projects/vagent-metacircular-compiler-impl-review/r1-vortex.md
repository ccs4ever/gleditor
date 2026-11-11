# Round 1 Critique — Vortex & VAgent Language Design Expert (`vortex`)

Audit of `design/vagent-metacircular-compiler-implementation.md` focusing on:

1. Grammar, type system (`Code<T>`, `Code<Cell>`, `Code<()>`, `Opt<T>`, `[T]`), and purity/closure
   (`pure`, `closed`, `arity`) soundness across Stages 1–7.
1. Whether expression-position vs statement-position lowering of `std:meta` verbs in Stage 1
   respects Vortex's pure expression / trailed choice-point semantics and `consumeWork` fuel bounds.
1. Whether `P2` (reserved parameter names `"result"`, `"produced"`, `"pure"`, `"closed"`, `"arity"`)
   and `P12` (`str_len`, `byte_at`, `str_slice`, `int_to_str`) are type-sound and complete for
   Stages 5–7.

______________________________________________________________________

## Findings

### [VORTEX-1] BLOCKING: std:meta expression lowering lacks VM dispatch hook

- **Location**: `design/vagent-metacircular-compiler-implementation.md:297-302`,
  `apps/common/xanadu/vortex/vortex_vm.hpp:28-85`,
  `apps/common/xanadu/vortex/vortex_vm.cpp:1450-1892`,
  `apps/common/xanadu/vagent/compiler.cpp:860-905`

- **Evidence**: Line 300–302 states:

  > "Multi-hop/rank-returning verbs (`clauses_of`, `module_named`, `symbol_named`, `plan_named`,
  > `code_named`, `weight_of`, `validate`, `registration`, `dim_named`, `as_body`) lower via
  > `decls.cpp`'s host-step hoisting (`x := <verb>(...)` / `let x := <verb>(...)`,
  > `decls.cpp:218-276`) when in statement/let position, and via a single-entry `std:meta` dispatch
  > hook on `MetaRuntime` during fragment evaluation when in expression position."

  This is architecturally impossible and unsound under the current engine design:

  1. `VortexVM` (`vortex_vm.hpp`, `vortex_vm.cpp`) has no foreign function interface (FFI), host
     call opcode, or callback hook to invoke C++ `MetaRuntime` methods during bytecode evaluation
     (`VortexVM::step`). The opcode set (`vortex_vm.hpp:28-85`) is closed, deterministic, and
     self-contained.
  1. Architectural layer separation (`AGENTS.md:231-236`, `vortex-design-critique/SKILL.md:148`)
     strictly forbids `apps/common/xanadu/vortex/` from depending on `apps/common/xanadu/vagent/`.
  1. Inside `compileFragment` (`fragment.cpp`), an expression body compiles into a bytecode DAG
     without plan steps (`d.step`); it cannot hoist host steps.
  1. Invoking arbitrary C++ manifold traversals from within speculative VM execution (such as during
     choice points or `where` guard expressions) breaks trail isolation and escapes VM instruction
     counting unless every visited cell explicitly bills `core.consumeWork(1)`.
  1. OR-parallel worker threads (`or_parallel.cpp:351-380`) execute `VortexVM` instances across
     worker contexts without a bound `MetaRuntime`.

- **Fix**: Strictly restrict multi-hop `std:meta` verbs (`validate`, `clauses_of`, `module_named`,
  `symbol_named`, `plan_named`, `code_named`, `weight_of`) to statement-position host steps
  (`decls.cpp:258-276` via `mintHostStep`), where they dispatch cleanly in `runHostStep`
  (`executor.cpp:808-1642`) on the agent's plan spine between VM invocations. For expression
  position inside fragments, permit only 1-hop native opcodes:

  - `label_of(c)` lowers to `#LINKED(c, +d.name)`
  - `test_of(c)` lowers to `#LINKED(c, +d.grab)`
  - `body_of(c)` lowers to `#LINKED(c, +d.spin)`
  - `registration(h)` lowers to `#FIND(h, "$reg_state")` (or `#LINKED(h, +d.kind-registered)`)

  Any expression attempting a multi-hop meta query inside a fragment must be rejected by
  `checker.cpp` with `DiagnosticCode::InvalidPosition` ("multi-hop meta reflection is
  statement-position only; bind via `let x := <verb>(...)` on the plan spine").

### [VORTEX-2] BLOCKING: Invariant `Code<Cell>` trap for non-Cell returns

- **Location**: `design/vagent-metacircular-compiler-implementation.md:351-355` (Stage 2 Track A),
  `763` (P4), `apps/common/xanadu/vagent/checker.cpp:665-674, 1223-1226, 1886-1890`,
  `apps/common/xanadu/vagent/compiler.cpp:935-946`

- **Evidence**: Line 351 specifies:

  - `register(h: Cell): Opt<Code<Cell>>` (impure)
  - `register_all(hs: [Cell]): Opt<[Code<Cell>]>` (impure)
  - `as_body(c: Code<Cell>): Opt<Code<()>>` (pure)

  In `checker.cpp:665-674`, `compatible` enforces that `Code<T>` is strictly invariant in `T`:
  `Code<Cell>` does NOT unify with `Code<Int>`, `Code<Str>`, `Code<Bool>`, or `Code<()>`. When
  `run(c, [args])` is typed in `checker.cpp:1223-1226` and lowered in `compiler.cpp:935-946`, a
  `Code<Cell>` operand causes `metaRun` to return `Cell`. In Stage 5, the metacircular compiler
  compiles arbitrary fragments (arithmetic, conditionals, string operations per lines 608 and 638).
  When a program compiles an integer-returning fragment such as `add(a: Int, b: Int): Int` and
  registers it via `register(h)`, the returned handle has type `Opt<Code<Cell>>`. When unpacked and
  executed via `let res := run(c, [10, 20]);`, `res` is statically typed as `Cell`. Subsequent
  operations expecting `Int` (e.g. `res + 1`, `res == 30`) immediately fail type checking:
  `checker.cpp:1886-1890` reports
  `DiagnosticCode::TypeMismatch: arithmetic requires numeric operands` (got `Cell`). VAgent has no
  dynamic downcast or type-narrowing operator. Furthermore, `as_body(c: Code<Cell>)` explicitly
  demands `Code<Cell>`, rejecting bare `Code` wildcards (`Type{"Code", {}}`) because
  `checker.cpp:672` checks `actual.arguments.size() == expected.arguments.size()`.

- **Fix**:

  1. Type `register(h: Cell)` as returning `Opt<Code>` (the unparameterized bare wildcard
     `Type{"Code", {}}`) and `register_all(hs: [Cell])` as returning `Opt<[Code]>`. In
     `checker.cpp:1223-1226`, when `run` operates on bare `Code` (`operandType.arguments.empty()`),
     it infers result type `unknown`. Under `checker.cpp:656-658`, `unknown` unifies with any
     expected result type (`Int`, `Str`, `Cell`, `Bool`, etc.), allowing callers of runtime-compiled
     fragments to receive and use their true scalar results.
  1. Type `as_body(c: Code): Opt<Code<()>>` to accept bare `Code` (`Type{"Code", {}}`). It accepts
     both bare `Code` and `Code<Cell>` (from `body_of` / `code_named`), verifies at runtime that the
     descriptor specifies `sig.resultType == "()"`, and returns `Opt<Code<()>>` for installation
     into `graft_body` and `learn_body`.

### [VORTEX-3] BLOCKING: String primitives require Band A OpcodeKind opcodes

- **Location**: `design/vagent-metacircular-compiler-implementation.md:691-699, 771` (P12),
  `apps/common/xanadu/vortex/vortex_vm.hpp:28-85`,
  `apps/common/xanadu/vortex/vortex_vm.cpp:1451-1481`,
  `apps/common/xanadu/vagent/compiler.cpp:860-905`

- **Evidence**: P12 asserts:

  > "We add four pure scalar stdlib functions in `apps/common/xanadu/vortex/vortex_stdlib.cpp`
  > (wired in `checker.cpp` and `compiler.cpp`): `str_len`, `byte_at`, `str_slice`, `int_to_str`...
  > Price: 4 pure functions in `vortex_stdlib.cpp` (~60 LoC total), 0 new `OpcodeKind` enum values
  > (dispatched via existing stdlib call mechanism or `OpcodeKind::Add` string concat)."

  This assertion contradicts the repository's execution architecture:

  1. There is no "existing stdlib call mechanism" in `VortexVM`. `VortexVM` executes instructions
     exclusively by switching on `OpcodeKind` in `VortexVM::executeOpcodeBody`
     (`vortex_vm.cpp:1450-1892`).
  1. Every existing string operation in `vortex_stdlib` (`Trim`, `ToLower`, `ToUpper`) is backed by
     an explicit `OpcodeKind` enumerator (`vortex_vm.hpp:75-77`) and executed directly in
     `vortex_vm.cpp:1451-1481`.
  1. `str_len`, `byte_at`, and `str_slice` cannot be synthesized from `OpcodeKind::Add` (which only
     performs string concatenation). Without dedicated opcodes, bytecode emitted by `compiler.cpp`
     or Stage 5's in-language emitter cannot invoke C++ functions in `vortex_stdlib.cpp`.

- **Fix**: Add explicit Band A opcodes to `OpcodeKind` in `apps/common/xanadu/vortex/vortex_vm.hpp`
  before `kLastContentDecodableOpcode`: `StrLen`, `ByteAt`, `StrSlice`, and `IntToStr` (or `ToStr`).
  Implement their execution in `VortexVM::executeOpcodeBody` (`vortex_vm.cpp`) operating directly on
  `CellValue` strings and integers, and add lowering arms in
  `apps/common/xanadu/vagent/compiler.cpp`'s `emitExpr`.

### [VORTEX-4] MAJOR: P12 uses illegal -1 sentinel and lacks str_to_int

- **Location**: `design/vagent-metacircular-compiler-implementation.md:694, 771` (P12),
  `AGENTS.md:213`, `brief-common.md:11`, `apps/common/xanadu/vagent/checker.cpp:1900-1915`
- **Evidence**:
  1. Line 694 specifies `byte_at(s: Str, idx: Int): Int` as returning "unsigned byte `[0, 255]` at
     `idx`, or `-1` if out of bounds." This violates Standing Rule 3 (`brief-common.md:11`,
     `AGENTS.md:213`, `vortex-design-critique/SKILL.md:60`): "No sentinels outside wire/on-disk
     formats: APIs use `std::optional` or `std::expected`, never `noCell` [or sentinel values]". In
     VAgent, absent lookup/index results are typed as `Opt<T>`.
  1. In Stage 7, the in-language lexer and parser (`vagent/parser.vagent`) must parse source code
     into R1 AST nodes. P12 provides `int_to_str(n: Int): Str`, but completely omits
     `str_to_int(s: Str): Opt<Int>`. When parsing numeric literal tokens (e.g. `"1024"`), the parser
     has no primitive to convert token text into an integer scalar for R1 `"lit.int"` nodes.
     Furthermore, to unescape string literals (converting escape sequences like `\n`, `\t`, `\xHH`
     into real characters), the lexer needs a way to convert a byte/codepoint integer back into a
     string: `char_to_str(b: Int): Str` (or `byte_to_str`). Without it, constructing unescaped
     strings is impossible in VAgent without external helpers.
- **Fix**:
  1. Change `byte_at`'s signature to `byte_at(s: Str, idx: Int): Opt<Int>`, returning `none` on
     out-of-bounds, matching VAgent's `if let` pattern-matching idioms.
  1. Add `str_to_int(s: Str): Opt<Int>` and `char_to_str(b: Int): Str` to P12, making the string
     scalar manipulation surface complete for Stage 7's lexer/parser.

### [VORTEX-5] MAJOR: Missing while lowering and nonexistent clause_count

- **Location**: `design/vagent-metacircular-compiler-implementation.md:279, 577, 611, 638`,
  `apps/common/xanadu/vagent/compiler.cpp:450-1000, 1425-1590`
- **Evidence**:
  1. Line 279 states: "Today `compiler.cpp` only intercepts `believes(...)` (`:977`) and
     `clause_count(...)` (`:993`)." Line 577 and 638 list `"believes"`, `"clause_count"` as part of
     R1 AST kinds and compiler test corpus. In reality, `clause_count` does not exist anywhere in
     `apps/common/xanadu/vagent/`.
  1. Line 611 specifies for Stage 5: "Control flow (`if`, `while`, short-circuit `&&`/`||`):
     `Branch` and `Jump` with two-hop target operand cells...". However,
     `apps/common/xanadu/vagent/compiler.cpp` contains NO lowering for `ExprKind::While`. In
     `compiler.cpp:997-1000`, `preflight` rejects `While` with `DiagnosticCode::InvalidDeclaration`
     ("construct is not supported by the scalar lowering iteration"), and `emitExpr` (`:1588`) drops
     it to `jump(cells_->fail)`. If Stage 5's in-language emitter claims to reproduce the exact
     emission contract of `compiler.cpp`, it cannot support `while` unless `compiler.cpp` is also
     updated, which is not tracked in the Proposal Register.
- **Fix**:
  1. Remove all references to `clause_count` from lines 279, 577, and 638.
  1. Either add a numbered proposal (`P13`) to implement `ExprKind::While` lowering in
     `compiler.cpp` (emitting loop header `ChoiceDepth`, condition branch, body jump back to header,
     and exit jump); or explicitly specify that `while` loops are not in the scalar lowering subset,
     and that all iterative loops in Stages 5–7 are structured as tail-recursive functions/methods.

### [VORTEX-6] MAJOR: Omission of d.code linking breaks codeNamed lookup

- **Location**: `design/vagent-metacircular-compiler-implementation.md:425-426, 448-450`,
  `apps/common/xanadu/vagent/meta.cpp:278-280, 2754`

- **Evidence**: In Stage 2 Track B (`registerHead` and `drainPendingRegisters`), line 425 states:

  > "append `head` to `agentCell`'s `+d.kind-registered` rank (`binds_.dims.kindRegistered`), and
  > link a 'committed' state record on `head`."

  Line 448 states:

  > "ensure $h_i$ sits on `agentCell +d.kind-registered`."

  Neither location links `head` to `agentCell +d.code`. However:

  - `MetaRuntime::codeNamed(label)` (`meta.cpp:278-280`) resolves labels exclusively by walking
    `zigzag::rankAfter(arena(), binds_.agentCell, binds_.dims.code)`.
  - `MetaRuntime::verifyClosed` (`meta.cpp:2754`) uses `codeNamed(name)` to verify that `run`-role
    and `call`-role dependencies are resolvable. If runtime-registered heads are appended only to
    `+d.kind-registered` and not `+d.code`, `codeNamed` fails to find them, and any subsequent
    fragment referencing a runtime-registered head by name fails `verifyClosed` with
    `"run-role record '...' unresolved — fail-until-resume"`.

- **Fix**: Update `registerHead` (line 425) and Pass 2 of `drainPendingRegisters` (line 448) to link
  `head` to BOTH `+d.kind-registered` (the carrier for opcode re-registration) and `+d.code` (the
  namespace rank for `codeNamed`), OR update `codeNamed` in `meta.cpp:278-280` to search
  `binds_.dims.kindRegistered` in addition to `binds_.dims.code`.

### [VORTEX-7] MINOR: Specify $reg_state transition mechanics on vars rank

- **Location**: `design/vagent-metacircular-compiler-implementation.md:426, 431, 448`
- **Evidence**: Line 431 specifies linking a trailed `"$reg_state" -> "pending"` marker on `head`'s
  `d.vars` rank. Line 448 states: "flip `$reg_state` from 'pending' to 'committed'". In ZigZag
  `ArenaManifold`, changing a record value from `"pending"` to `"committed"` requires either: (a)
  `core_.writeValue(valCell, "committed")`, or (b) unlinking the `"pending"` cell and linking a
  `"committed"` cell. Since this occurs during Pass 2 of `drainPendingRegisters` at
  `outstandingMarks() == 0`, specifying the exact mechanism clarifies that it leaves zero trailing
  garbage.
- **Fix**: Clarify that Pass 2 updates the value cell of the `"$reg_state"` record via
  `zigzag::expectWritten(core_.writeValue(statusValCell, "committed"))`, costing 0 new cells and 0
  new links.

### [VORTEX-8] NIT: ArityRule in meta.cpp already supports range bounds

- **Location**: `design/vagent-metacircular-compiler-implementation.md:375-378`
- **Evidence**: Line 375 says: "Update `OpcodeKind::Linked`'s `ArityRule` from `{4, 4, 1, 1}` to
  allow 4 or 6 left-wing inputs (`minLeft = 4, maxLeft = 6` — extending `ArityRule` with
  `minLeft`/`maxLeft` or an explicit check...)". In `apps/common/xanadu/vagent/meta.cpp:105-131`,
  `ArityRule` already has `{minLeft, maxLeft, minRight, maxRight}` fields.
- **Fix**: Clarify that updating `Linked`'s arity rule to `{4, 6, 1, 1}` in `kArity` requires only
  updating the table literal in `meta.cpp`, requiring no struct alterations.

______________________________________________________________________

SUMMARY: BLOCKING=3 MAJOR=3 MINOR=1 NIT=1 VERDICT: NEEDS-REVISION
