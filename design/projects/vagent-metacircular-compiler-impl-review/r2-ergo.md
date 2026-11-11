# Round 2 Critique — Programming Language Design & Ergonomics Critic (`ergo`)

Verification audit of the revised `design/vagent-metacircular-compiler-implementation.md` following
Round 1 critique (`r1-ergo.md`) and the Round 1 disposition (`r1-disposition.md`).

______________________________________________________________________

## Verification of Round 1 Findings

### [ERGO-1] The Value-Cell Paradox — `make_cell(val: <any>): Cell`

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 1 (lines 322–324) and Stage 2 Track A (lines 388–389), the design
  specifies `make_cell(val: <any>): Cell` as an impure special form (`MetaForm::MakeCell` in
  `checker.cpp:1138-1163`) that lowers in `compiler.cpp` to `OpcodeKind::New` + `OpcodeKind::Value`
  (`[cell, 0, -1, val]`). This accepts `Int`, `Float`, `Bool`, `Str`, or `Cell` and mints a fresh
  cell containing that value. Stage 4 `join_batch` (line 647) and Stage 5 `emitter.vagent` (line
  735\) cite `make_cell` to assemble argument cells and literal values into the manifold cleanly
  without relying on `cells"..."` computed creation syntax.

### [ERGO-2] Missing in-Language R2 Builder — `build_fragment(spec: Cell): Opt<Cell>`

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 4 (lines 680–697), the design fully specifies the in-language R2
  builder verb `build_fragment(spec: Cell): Opt<Cell>` exposed under `import std:meta` and backed by
  `MetaRuntime::buildFragmentFromSpec`. The input `spec` cell schema is precisely defined:
  - `spec` text: head label (`"src:<n>"`).
  - `spec +d.vars`: `"entry"` (`Int64`), `"result_slot"` (`Int64`), `"produced_slot"` (`Int64`),
    `"result_type"` (`Str`), `"pure"` (`Int64`), `"closed"` (`Int64`).
  - `spec +d.grab` → `+d.step`: parameter chain (`text` = name, `+d.values` → type).
  - `spec +d.refs` → `+d.step`: covered body cells (`Int64`), optional `+d.name` and `+d.grab`
    roles. `build_fragment` mints the complete head, descriptor (with records in canonical order),
    parameter chain, `+d.recipe`, and `+d.refs` apparatus (with `d.mesh.relocation` markers) in a
    single host step. Stage 5 `emitter.vagent` (line 748) leverages `build_fragment` to avoid
    low-level tuple-assembly boilerplate.

### [ERGO-3] `Code` Inspection, Validation, and Status — `head_of(c: Code): Cell`

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 1 (line 321) and Stage 2 Track A (lines 384–385), the design
  introduces `head_of(c: Code): Cell` (pure; identity unboxing of the `Code` ref-box slot at zero op
  cost, accepting the bare `Code` wildcard `Type{"Code", {}}`). Authors holding any `Code<T>` handle
  (from `register`, `compile_fragment`, `code_named`, `test_of`, or `body_of`) can pass it directly
  to reflection, validation, and lifecycle verbs: `registration(head_of(c))`,
  `validate(head_of(c))`, `label_of(head_of(c))`, `unlearn(head_of(c))`, and `prune(head_of(c))`.

### [ERGO-4] Diagnostic Observability — `var_of(owner: Cell, key: Str): Opt<Cell>`

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 0 (line 299), Stage 1 (line 331), and Stage 2 Track A (lines
  386–387), the design adds `var_of(owner: Cell, key: Str): Opt<Cell>` (pure; reads `owner`'s
  `d.vars` rank for `key`). This enables VAgent programs to inspect `fault:<label>` refusal
  tombstones on `agent_cell()` (lines 515–516, 586) and retrieve structured `$validate_error` /
  `$compile_error` messages programmatically.

### [ERGO-5] Pending-Registration Window Diagnostics

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 2 Track B (lines 497–500), the pending registration window semantics
  are documented with clear diagnostic behavior: during the pending window, `registration(head)`
  returns `"pending"`. If an author invokes `run(c)` before turn commit, `RunCall` fails closed
  (`produced = false`) and `MetaRuntime::runFragment` explicitly sets
  `MetaRunOutcome::detail = "pending_registration"`. The documented author idiom
  (`registration(head_of(c)) == "committed"` check and turn commit before execution) prevents silent
  failures.

### [ERGO-6] 2-Degree Clean R1 AST Specification & Source Coordinates

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 4 (lines 658–678), the R1 AST specification is completely
  disambiguated with strict adherence to ZigZag invariants:
  - Parent links posward on `+d.grab` to its first child (`parent +d.grab → child_0`).
  - Sibling children chain posward on `+d.step` (`child_0 +d.step → child_1 +d.step → ...`).
  - Kind tags are stored directly in node cell text (validated against `programRoot +d.ast.kinds`),
    avoiding shared-target link eviction (`ZIGZAG-3`).
  - Fresh per-node type-name cells link on `node +d.values` (`ZIGZAG-8`).
  - Source coordinates are captured via optional `"line"` and `"col"` records on `node +d.vars`.

### [ERGO-7] Stage 7 Character Classification & `StrToInt` Primitive

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 7 (lines 801–803) and §3 Stage 4 Track A2 (`P12`, lines 708–710),
  Track A2 provides native Band A opcodes `StrLen`, `ByteAt` (returning `Opt<Int>`, zero sentinels),
  `StrSlice`, `IntToStr`, and `StrToInt` (`Opt<Int>`). Stage 7 `parser.vagent` implements pure
  VAgent character-classification helpers (`is_space`, `is_digit`, `is_alpha`) over `byte_at`,
  eliminating raw ASCII magic numbers and integer-parsing boilerplate in accordance with `GEMINI.md`
  rules.

### [ERGO-8] Dimension Lookup Ergonomics in Stage 5

- **Status**: **RESOLVED.**
- **Verification**: By introducing `build_fragment(spec)` (lines 680–697), the C++ builder resolves
  all standard compiler apparatus dimensions (`d.grab`, `d.step`, `d.values`, `d.ref`, `d.refs`,
  `d.recipe`, `d.name`, `d.vars`, `d.entry`) in a single host step. VAgent emitter authors no longer
  need to perform ~9 repetitive `dim_named` lookups and unwraps per emitted fragment.

### [ERGO-9] Generic Wildcard for `as_body(c: Code): Opt<Code<()>>`

- **Status**: **RESOLVED.**
- **Verification**: In §3 Stage 2 Track A (lines 381–383) and §5 `P4` (line 872), `as_body`'s
  parameter is typed with the bare `Code` wildcard `Type{"Code", {}}`. Any `Code<T>` handle
  (including wildcard `Code` from `graft_where`/`learn_where` and `Code<Cell>` from `register`)
  narrows to `Opt<Code<()>>` after descriptor verification, adhering to §4 D7.

______________________________________________________________________

SUMMARY: BLOCKING=0 MAJOR=0 MINOR=0 NIT=0 VERDICT: CONVERGED
