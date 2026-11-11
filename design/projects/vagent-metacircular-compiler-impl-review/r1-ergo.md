# Round 1 Critique — Programming Language Design & Ergonomics Critic (`ergo`)

Audit of `design/vagent-metacircular-compiler-implementation.md` focusing on author-facing
ergonomics of the `std:meta` surface, boilerplate count and ceremony for VAgent compiler authors
(Stages 5–7), and diagnostic clarity upon refusal.

______________________________________________________________________

## Findings

### [ERGO-1] BLOCKING: The Value-Cell Paradox — cannot mint dynamic scalar cells

- **Location**: `design/vagent-metacircular-compiler-implementation.md:549-571`,
  `design/vagent-metacircular-compiler.md:65-78`, `apps/common/xanadu/vagent/cell_path.cpp:134-138`,
  `apps/common/xanadu/vagent/checker.cpp:686-688`
- **Evidence**: Both `boot_writer(target: Cell, source: Cell): Cell` (line 549) and `join_batch`
  write tuples (`tuple` text == `"write"`, reads `target` cell via `tuple +d.grab`, `value` cell via
  `tuple +d.values`, lines 561-564) require `source`/`value` to be a `Cell`. However, VAgent
  provides no mechanism to create a cell containing a computed scalar value (`Int`, `Str`, `Float`):
  1. `cells"..."` syntax only mints static string literals (`compiler.cpp:1773-1778`) and strictly
     refuses computed creation values (`cell_path.cpp:134-138`:
     `if (create.kind == vql::CreateValue::Kind::Expression) refuse(...)`).
  1. `Cell` and `Int`/`Str`/`Float` are disjoint scalar types in `checker.cpp:686-688`;
     `checker.cpp:680` strictly rejects passing an `Int` or `Str` where `Cell` is expected
     (`DiagnosticCode::TypeMismatch`).
  1. There is no primitive (`make_cell(v)`, `cell(v)`) in `std:meta` or stdlib to box a scalar into
     a cell. The assertion in `design/vagent-metacircular-compiler.md:69-72` that "an int64 arrives
     from any op's bound-output writeback (`x := 25+25` yields a cell holding int64 50...)"
     overlooks that `x` has static type `Int`, so the checker forbids passing `x` to `boot_writer`
     or linking it into a `join_batch` write tuple. An author writing `emitter.vagent` or
     `parser.vagent` has no typed way to write an opcode number (e.g. `OpcodeKind::Add`) or dynamic
     token string into any cell.
- **Fix**: Add a primitive `make_cell(val: any): Cell` (or `cell(val: any): Cell`) to `std:meta` (or
  stdlib), and/or update `join_batch`'s `"write"` tuple specification so that it accepts scalar
  values directly from a value slot rather than requiring a pre-minted donor cell.

### [ERGO-2] BLOCKING: Missing in-language R2 Builder specification forces Stage 5 boilerplate

- **Location**: `design/vagent-metacircular-compiler-implementation.md:197, 584-588`
- **Evidence**: Line 197 states Stage 5 "calls R2/batched join + register_all", and line 585 states
  "Factor `MetaRuntime::buildFragmentFromSpec(CellRef spec)` so Stage 5's emitter... share one
  canonical head + descriptor + `+d.refs` + `+d.recipe` assembly helper." However,
  `buildFragmentFromSpec` is never declared as an `std:meta` in-language verb: it has no signature
  in `checker.cpp`, no lowering arm in `compiler.cpp`, no dispatch in `executor.cpp`, and is not
  listed in `kMetaDimensionNames` or the Proposal Register (`P1`–`P12`). Furthermore, the cell
  schema of `spec` is never defined anywhere in either document. Without an in-language R2 builder,
  Stage 5 (`emitter.vagent`) must fall back entirely to `join_batch`. Emitting a single opcode via
  `join_batch` requires minting ~10–15 tuple cells and ~50 links (wings, slots, recipe, refs,
  steps). For a 50-op fragment, this requires authoring hundreds of lines of low-level
  tuple-construction code in VAgent and creates thousands of intermediate tuple cells in the live
  agent arena that cannot be reclaimed until `promote` (`implementation.md:627-628`).
- **Fix**: Formally specify the in-language R2 builder verb in `std:meta` (e.g.
  `build_fragment(spec: Cell): Opt<Cell>`), specify the structured cell schema of `spec` (descriptor
  fields, param chain, ops DAG, ref/recipe declarations), and wire it through `checker.cpp`,
  `compiler.cpp`, and `executor.cpp`.

### [ERGO-3] MAJOR: Code handles cannot be inspected, validated, queried, or pruned

- **Location**: `design/vagent-metacircular-compiler-implementation.md:261, 351-355, 516-520`,
  `apps/common/xanadu/vagent/checker.cpp:970-1020`
- **Evidence**: In `checker.cpp:970-1020` and `implementation.md:351-355`, reflection, validation,
  status, and lifecycle verbs are typed as: `registration(h: Cell): Str`, `validate(h: Cell): Bool`,
  `label_of(h: Cell): Opt<Str>`, `clauses_of(h: Cell): [Cell]`, `unlearn(h: Cell): ()`,
  `prune(h: Cell): ()`. Meanwhile, `register(h)` (line 351) and `compile_fragment(...)` (line 516)
  return `Opt<Code<Cell>>`. Because `Code<T>` does not coerce to `Cell` (`checker.cpp:655-676`) and
  VAgent has no `head_of(c: Code): Cell` or `cell_of` primitive, an author who receives a
  `Code<Cell>` handle (from `compile_fragment`, `code_named`, `test_of`, or `body_of`) cannot query
  `registration(c)`, cannot run `validate(c)`, cannot read `label_of(c)`, and cannot `unlearn(c)` or
  `prune(c)`.
- **Fix**: Add `head_of(c: Code): Cell` to `std:meta`, and/or extend `registration`, `validate`,
  `label_of`, `unlearn`, and `prune` in `checker.cpp` to accept bare `Code` wildcard as an
  alternative to `Cell`.

### [ERGO-4] MAJOR: validate(h) discards diagnostics; fault tombstones are inaccessible

- **Location**: `design/vagent-metacircular-compiler-implementation.md:452-453, 516-517`,
  `apps/common/xanadu/vagent/meta.cpp:490-505`
- **Evidence**: In `meta.cpp:490`, C++ `MetaRuntime::validate` returns
  `MetaValidateReport { bool valid; std::string error; }`, which details the exact reason for
  failure (e.g. non-op root, arity mismatch, out-of-bounds target, impure op). However, `std:meta`
  exposes `validate` as `validate(h: Cell): Bool` (`checker.cpp:986`, `implementation.md:287`),
  completely discarding `error`. An author whose hand-assembled or generated graph fails validation
  receives only `false`, with no way to diagnose which op or wing link was rejected. Furthermore,
  when `register` or post-commit drain fails (lines 452-453), the engine writes a `fault:<label>`
  tombstone on `agentCell +d.vars` (`genesis.cpp:219-228` pattern). But VAgent provides no
  reflection function to read `vars` records by name from an arbitrary cell (`agent_cell()`), so the
  recorded refusal reason is completely inaccessible to running VAgent code.
- **Fix**: Add `validate_report(h: Cell): Opt<Str>` (returning `none` on valid, `some(error)` on
  refusal), and add an in-language reflection verb `fault_of(agent: Cell, label: Str): Opt<Str>` or
  `last_fault(): Opt<Str>` so agents can inspect refusal reasons programmatically.

### [ERGO-5] MAJOR: Silent failure of run(c) during the pending-registration window

- **Location**: `design/vagent-metacircular-compiler-implementation.md:428-435`,
  `design/vagent-metacircular-compiler.md:360-363`, `apps/common/xanadu/vagent/vortex_vm.cpp:984`
- **Evidence**: Under the Universal Deferral Rule, `register(h)` in turn T always records a trailed
  pending marker and defers `programOpcodes_` insertion to turn commit (`marks == 0`). `register(h)`
  returns `some(c)` immediately. If an author calls `run(c, args)` in the same turn T,
  `vortex_vm.cpp:984` fails to find the entry terminus in `opcodeMap_` and executes `failJump`. The
  call silently fails closed (`produced = false`) without raising any error or diagnostic
  (`design/vagent-metacircular-compiler.md:361-363`: "indistinguishable from computing nothing until
  the drain lands at turn commit"). An author writing or testing a compiler stage who executes their
  newly registered fragment within the same turn will observe silent execution failure with no
  diagnostic explanation.
- **Fix**: Enhance `RunCall` / `VagentExecutor` to detect when a target fragment is marked
  `"pending"` on `d.vars` and surface a distinct runtime diagnostic/trap (e.g.
  `DiagnosticCode::InvalidPosition` or `PendingRegistrationTrap`) rather than silently failing
  closed, or add a compile-time warning in `checker.cpp` when `run` is invoked on a handle bound
  from `register` in the same lexical block.

### [ERGO-6] MAJOR: R1 AST specification is under-specified on child links and source spans

- **Location**: `design/vagent-metacircular-compiler-implementation.md:572-583`
- **Evidence**: Line 580 specifies: "Each R1 AST node is a cell whose text (or `+d.verb` link to the
  filed `d.ast.kinds` cell) names its kind, `+d.name` holds its identifier/operator/literal token,
  `+d.values` holds its annotated type cell (populated by Stage 6), and `+d.step` chains its child
  AST nodes in order." First, linking children along `+d.step` without specifying how the parent
  links to the first child (e.g. `parent +d.grab → firstChild`) leaves child discovery undefined. If
  a child node has siblings along `+d.step` and its own children along `+d.step`, this violates the
  ZigZag 2-degree invariant (each cell has at most one posward neighbor on `d.step`). Second, R1 AST
  defines no representation for source locations (`file`, `line`, `col`, `span`), making it
  impossible for Stage 6 (`checker.vagent`) to produce source-located compiler diagnostics. Third,
  the specification permits kind to be either cell text or a `+d.verb` link ("whose text (or
  `+d.verb` link...)"), creating ambiguity where `parser.vagent` and `emitter.vagent` could diverge
  on how kind is read.
- **Fix**: Disambiguate R1 AST: parent links to its child rank via `+d.grab Posward`, and child
  nodes chain along `+d.step`; specify a standard source location record on `+d.pos` carrying `line`
  and `col` int cells; and standardize kind representation as a single canonical form (e.g. node
  cell text matches kind name, with `+d.verb` pointing to `d.ast.kinds`).

### [ERGO-7] MINOR: Primitive character inspection forces raw ASCII magic numbers in Stage 7

- **Location**: `design/vagent-metacircular-compiler-implementation.md:685-699`
- **Evidence**: `P12` adds only four scalar string functions: `str_len`, `byte_at`, `str_slice`, and
  `int_to_str`. In VAgent, there are no character literals (`'a'`). Because `byte_at` returns
  integer byte values [0..255], an author implementing `parser.vagent` must hand-write raw integer
  comparisons for every ASCII character (`b >= 97 && b <= 122` for lowercase, `b >= 65 && b <= 90`
  for uppercase, `b >= 48 && b <= 57` for digits, `b == 32 || b == 10 || b == 13 || b == 9` for
  whitespace). This introduces hundreds of lines of error-prone magic numbers and boilerplate into
  `parser.vagent`.
- **Fix**: Provide character classification functions in `vortex_stdlib.cpp` or a standard prelude
  module (e.g. `is_alpha(b: Int): Bool`, `is_digit(b: Int): Bool`, `is_space(b: Int): Bool`,
  `is_ident_start(b: Int): Bool`, `is_ident_continue(b: Int): Bool`).

### [ERGO-8] MINOR: Repeated dim_named lookups create boilerplate and runtime overhead

- **Location**: `design/vagent-metacircular-compiler-implementation.md:354, 558-571`
- **Evidence**: Assembling fragment graphs via `join_batch` requires passing dimension cells
  (`dim: Cell`). To construct link tuples, an author must look up ~9 standard compiler dimensions
  (`"d.grab"`, `"d.step"`, `"d.values"`, `"d.ref"`, `"d.refs"`, `"d.recipe"`, `"d.name"`,
  `"d.vars"`, `"d.entry"`), each returning `Opt<Cell>`. Repeated lookups and `?` unwraps add
  boilerplate across every emitter routine.
- **Fix**: Add a convenience helper in `std:meta` returning a standard dimension record (e.g.
  `meta_dims(): MetaDims` where fields are `grab`, `step`, `values`, etc.), or provide
  dimension-specific named constants.

### [ERGO-9] NIT: as_body is monomorphic to `Code<Cell>` instead of generic Code wildcard

- **Location**: `design/vagent-metacircular-compiler-implementation.md:355`
- **Evidence**: `as_body(c: Code<Cell>): Opt<Code<()>>` restricts its argument strictly to
  `Code<Cell>`. If an author holds a handle typed as bare `Code` (the wildcard accepted by
  `graft_where`/`learn_where` per `checker.cpp:999`), passing it to `as_body` causes a type
  mismatch.
- **Fix**: In `checker.cpp`, type `as_body`'s argument as bare `Code` wildcard (i.e. `Code` or
  `Code<T>`), allowing any `Code` handle whose descriptor specifies `result == "()"` to be narrowed
  to `Code<()>`.

______________________________________________________________________

SUMMARY: BLOCKING=2 MAJOR=4 MINOR=2 NIT=1 VERDICT: NEEDS-REVISION
