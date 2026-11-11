# Round 1 Disposition — `design/vagent-metacircular-compiler-implementation.md`

## Summary of Round 1 Verdicts

| Critic Role | Report File       | BLOCKING | MAJOR  | MINOR  | NIT   | Verdict          |
| ----------- | ----------------- | -------- | ------ | ------ | ----- | ---------------- |
| `writer`    | `r1-writer.md`    | 4        | 6      | 3      | 1     | `NEEDS-REVISION` |
| `vortex`    | `r1-vortex.md`    | 3        | 3      | 1      | 1     | `NEEDS-REVISION` |
| `codebase`  | `r1-codebase.md`  | 3        | 4      | 3      | 1     | `NEEDS-REVISION` |
| `architect` | `r1-architect.md` | 1        | 4      | 2      | 1     | `NEEDS-REVISION` |
| `ergo`      | `r1-ergo.md`      | 2        | 4      | 2      | 1     | `NEEDS-REVISION` |
| `zigzag`    | `r1-zigzag.md`    | 3        | 3      | 2      | 0     | `NEEDS-REVISION` |
| **Total**   |                   | **16**   | **24** | **13** | **5** |                  |

______________________________________________________________________

## Disposition Table

### 1. Technical Writer (`writer`)

- **[WRITER-1] BLOCKING — Conflation of `registerHead` signature with `meta.hpp:684` /
  `meta.cpp:2206`:** **Accepted.** `MetaRuntime::registerHead(CellRef head) const` (`meta.hpp:684`)
  is preserved for `d.code` rank registration. The new Stage 2 atomic registration methods on
  `MetaRuntime` are named `registerCode(CellRef head)` and
  `registerAll(std::span<const CellRef> heads)`, which invoke `registerHead` at commit/drain time so
  committed heads sit on both `d.code` and `d.kind-registered`.
- **[WRITER-2] BLOCKING — `programOpcodes_` vs real `opcodeMap_` (`vortex_vm.hpp:269`):**
  **Accepted.** Replaced all references to `programOpcodes_` with `opcodeMap_` (`vortex_vm.hpp:269`)
  and `registerOpcode` with `setOpcodeKind` (`vortex_vm.hpp:154`) / `registerProgramOps`
  (`vortex_vm.hpp:183`).
- **[WRITER-3] BLOCKING — `kMetaDimensionNames` truncation (`lineage.hpp:58-62`):** **Accepted.**
  Preserved all 12 existing entries in `kMetaDimensionNames` (`"d.code"`, `"d.patch"`, `"d.weight"`,
  `"d.synapse"`, `"d.entry"`, `"d.patch-head"`, `"d.validate"`, `"d.refs"`, `"d.ref"`, `"d.recipe"`,
  `"d.refFor"`, `"d.mesh.relocation"`) and appended `"d.lineage"`, `"d.kind-registered"`,
  `"d.pending-reg"`, and `"d.ast.kinds"` (16 entries total).
- **[WRITER-4] BLOCKING — Pending vs carrier rank membership during pending window:** **Accepted.**
  Pending registrations never touch `d.kind-registered` during a turn. Under live marks
  (`outstandingMarks() > 0 || vm.scopedDepth() > 0`), `registerCode` appends a trailed request cell
  to `agentCell +d.pending-reg`. Only Pass 2 of the post-commit drain (`marks == 0`) appends
  surviving heads to `d.kind-registered` and `d.code`.
- **[WRITER-5] MAJOR — Missing Proposal Register entry for Stage 1:** **Accepted.** Added `P13` to
  the Proposal Register covering Stage 1 (`compiler.cpp`, `decls.cpp`, `executor.cpp`,
  `prelude/meta.vagent`).
- **[WRITER-6] MAJOR — Companion doc citation and D6 / D7 integration:** **Accepted.** Updated
  companion header to cite `§1–§7, D1–D7, P1–P8` and integrated `D6` (arena residue retention until
  `promote`) and `D7` (`Code<Cell>` invariant typing + `as_body(c)` before
  `learn_body`/`graft_body`) into Stages 2, 3, 5, and 6.
- **[WRITER-7] MAJOR — Companion doc section number citations:** **Accepted.** Replaced all section
  references with the exact headings in `design/vagent-metacircular-compiler.md` (`§2`, `§3.2`,
  `§3.3`, `§4 D1–D7`, `§6 Q1–Q7`).
- **[WRITER-8] MAJOR — `checker.cpp` line citations for `std:meta`:** **Accepted.** Updated
  citations to `checker.cpp:966-1025` (builtin table) and `checker.cpp:1138-1264` (`metaForm`,
  `metaRun`, `metaReify`).
- **[WRITER-9] MAJOR — Stage 1 / Stage 4 ordering of `join_batch`:** **Accepted.** Stage 1 wires
  existing `std:meta` verbs + Stage 2 registration verbs through a single
  `MetaRuntime::runMetaHostStep` seam; `join_batch` and `build_fragment` plug into that seam in
  Stage 4.
- **[WRITER-10] MAJOR — R1 AST 2-degree topology:** **Accepted.** Defined parent-to-first-child on
  `+d.grab` and sibling chaining on `+d.step`.
- **[WRITER-11] MINOR — `apps/common/xanadu/vagent/prelude/meta.vagent`:** **Accepted.** Added
  `prelude/meta.vagent` to Track C and Stage 2/4 tasks.
- **[WRITER-12] MINOR — `m0` and `failTurn` line citations in `executor.cpp`:** **Accepted.**
  Corrected to `executor.cpp:256-265`.
- **[WRITER-13] MINOR — Filing `"d.ast.kinds"` in `lineage.hpp`:** **Accepted.** Added
  `"d.ast.kinds"` to `kMetaDimensionNames` (`lineage.hpp:58-62`).
- **[WRITER-14] NIT — Markdown formatting (`mdformat --wrap 100` and `mdl -i`):** **Accepted.**
  Applied `mdformat --wrap 100` and verified `mdl -i` on all files.

______________________________________________________________________

### 2. Vortex Language Expert (`vortex`)

- **[VORTEX-1] BLOCKING — `std:meta` expression lowering vs layer separation:** **Accepted.**
  Dropped the in-fragment `VortexVM` callback idea. Zero-arg roots (`program()`, `agent_cell()`)
  lower in `compiler.cpp` to constant `refCell`s; 1-hop reads (`label_of`, `test_of`, `body_of`)
  lower to `#LINKED` or statement-position host steps; all multi-hop reflection and mutation verbs
  lower via `decls.cpp` statement/let hoisting (`x := <verb>(...)` / `let x := <verb>(...)`,
  `decls.cpp:258-276`) into spine host-step cells dispatched by `VagentExecutor::runHostStep` →
  `MetaRuntime::runMetaHostStep`. `VortexVM` remains 100% free of `vagent` dependencies.
- **[VORTEX-2] BLOCKING — `Code<Cell>` invariance and return value typing:** **Accepted with D7
  Reconciliation.** Per `design/vagent-metacircular-compiler.md:631-642` (`D7`), `register` and
  `register_all` return `Opt<Code<Cell>>` and `Opt<[Code<Cell>]>` (matching `code_named`, `test_of`,
  `body_of`). When `let r := run(c, [args])` executes on `c: Code<Cell>`, `r` is typed `Cell` whose
  content holds the produced scalar/string value, extracted via `value_of(r)` (`checker.cpp:989`
  returns `any`, unifying with `Int`, `Str`, `Bool`, `Float`). In addition,
  `as_body(c: Code): Opt<Code<()>>` accepts the bare `Code` wildcard (`Type{"Code", {}}`) so any
  `Code<T>` narrows cleanly for `graft_body`/`learn_body`.
- **[VORTEX-3] BLOCKING — String primitives require Band A `OpcodeKind` enumerators:** **Accepted.**
  Added `StrLen`, `ByteAt`, `StrSlice`, `IntToStr`, and `StrToInt` as native Band A `OpcodeKind`
  enumerators in `vortex_vm.hpp` and `executeOpcodeBody` (`vortex_vm.cpp`).
- **[VORTEX-4] MAJOR — No `-1` sentinel in `byte_at`; add `str_to_int`:** **Accepted.**
  `byte_at(s: Str, idx: Int): Opt<Int>` and `str_to_int(s: Str): Opt<Int>` return `Opt<Int>`
  (`std::optional` / `Opt::none` on out-of-bounds or invalid integer text, zero sentinels).
- **[VORTEX-5] MAJOR — `while` and `clause_count` do not exist in VAgent:** **Accepted.** Removed
  `while` and `clause_count` from the R1 AST vocabulary and Stage 5 emitter list; documented that
  Stage 5–7 iteration uses recursive `def` functions and `clauses_of(h)`.
- **[VORTEX-6] MAJOR — Linking committed heads to `d.code` as well as `d.kind-registered`:**
  **Accepted.** `registerCode` invokes `registerHead(head)` upon commit/drain so every committed
  head is on `d.code` (resolvable via `code_named` and `verifyClosed`) as well as
  `d.kind-registered`.
- **[VORTEX-7] MINOR — `$reg_state` transition mechanics:** **Accepted.** With dedicated
  `d.pending-reg` rank (`WRITER-4` / `ZIGZAG-6`), `registration(h)` derives `"committed"` from
  `kindRegistered(h) || imageRef(h)`, `"pending"` from membership on `d.pending-reg`, and `"absent"`
  otherwise — requiring 0 per-head `$reg_state` mutations!
- **[VORTEX-8] NIT — `kArity` representation in `meta.cpp`:** **Accepted.** Documented `kArity[][3]`
  triple table in `meta.cpp:102-158` accurately.

______________________________________________________________________

### 3. C++23 Codebase Expert (`codebase`)

- **[CODEBASE-1] BLOCKING — `kMetaDimensionNames` (`lineage.hpp:58-62`):** **Accepted.** Same fix as
  `WRITER-3` / `ARCHITECT-1` / `ZIGZAG-5` (12 existing entries preserved + 4 new entries = 16).
- **[CODEBASE-2] BLOCKING — `registerHead` overload collision (`meta.hpp:684`):** **Accepted.** Same
  fix as `WRITER-1` (`registerCode` and `registerAll`).
- **[CODEBASE-3] BLOCKING — `boot:writer` / `boot:linker` 4-in `Value` arity and `Return` tail:**
  **Accepted.** Updated Stage 4 so `boot:writer` emits 4-in, 1-out `OpcodeKind::Value`
  (`[target, zero, minusOne, source]`, `compiler.cpp:1211-1223`, `vortex_vm.cpp:1200-1228`),
  `boot:linker` emits 4-in, 1-out `OpcodeKind::Link` (`vortex_vm.cpp:774-796`), and both wrap via
  `emitWrapped` ending in `OpcodeKind::Return` (`compiler.cpp:1888-1895`).
- **[CODEBASE-4] MAJOR — `opcodeMap_` and `setOpcodeKind`:** **Accepted.** Same fix as `WRITER-2`.
- **[CODEBASE-5] MAJOR — TU ownership of `executor.cpp` between Track B and Track C:** **Accepted.**
  Track C exclusively owns `executor.cpp` (adding `MetaRuntime::runMetaHostStep` call and the
  post-commit `meta.drainPendingRegisters()` call at `executor.cpp:3029`). Track B owns `meta.cpp`
  only.
- **[CODEBASE-6] MAJOR — `kArity[][3]` and `Linked` 3/4/6-in arity (`meta.cpp:102-158`,
  `vortex_vm.cpp:812-875`):** **Accepted.** Added `Linked` 3-in, 4-in, and 6-in entries to
  `kArity[][3]` and updated `validate`'s `arity` multimap decode (`meta.cpp:507-522`).
- **[CODEBASE-7] MAJOR — `RunCall` wing slot 2 is `failOp` (`compiler.cpp:2013-2018`):**
  **Accepted.** Corrected `P1` description: `RunCall` left-wing slot 2 is `failOp`
  (`reference(cells_->fail)`), while its continuation (`runResume`) sits on `runCall +d.spin`.
- **[CODEBASE-8], [CODEBASE-9], [CODEBASE-10], [CODEBASE-11] MINOR/NIT — Exact `file:line` citations
  and `noCell` initialization:** **Accepted.** Updated all line numbers in `meta.hpp`, `meta.cpp`,
  `fragment.cpp`, `compiler.cpp`, `checker.cpp`, and `vortex_vm.cpp`, and removed the false claim
  that `runFragment` discards its own mark.

______________________________________________________________________

### 4. Systems Architect (`architect`)

- **[ARCHITECT-1] BLOCKING — `kMetaDimensionNames` truncation and `noDim`:** **Accepted.** Same fix
  as `WRITER-3` / `CODEBASE-1`.
- **[ARCHITECT-2] MAJOR — Schedule Track A2 (`P12` string/int primitives) in Wave 2 before Stage
  5:** **Accepted.** Moved Track A2 (`P12`: `str_len`, `byte_at`, `str_slice`, `int_to_str`,
  `str_to_int`) into Wave 2 (completing at Barrier 2) so Stage 5 (`emitter.vagent`) has `int_to_str`
  for `cap$N` and serial labels.
- **[ARCHITECT-3] MAJOR — Wave 2 `executor.cpp` ownership collision between Tracks E and F:**
  **Accepted.** Track C (Wave 1) introduces `MetaRuntime::runMetaHostStep` in `executor.cpp`; Wave 2
  Tracks E (`host_compile.cpp`), F (`bootstrap.cpp` / `r1_ast.cpp`), and A2 (`vortex_stdlib.cpp` /
  `vortex_vm.cpp`) are 100% file-disjoint.
- **[ARCHITECT-4] MAJOR — Exact Band A / Band B classification for `feature/vql-completion` and
  `epistemic-arbitration`:** **Accepted.** Explicitly enumerated which VQL opcodes belong in Band A
  (pure/data query ops) vs Band B (`InvokeFrame`, `ReturnFrame`, `ScopedChoice`, `PushGuard`,
  `PopGuard`, `BeginMutationGroup`, `EndMutationGroup`, alongside `RunCall`, `RunResume`,
  `ScopedEvaluate`).
- **[ARCHITECT-5] MAJOR — OR-parallel worker phantom ephemeral opcode hazard (`P6`):** **Accepted.**
  `bindProgramOpcodes` filters its memoized opcode list to persistent base cells
  (`!zigzag::isEphemeral(op)`) for `OracleRecompute` sparks, and for `ShadowDeltaPrefix` sparks
  binds hydrated ephemeral opcodes only after verifying `workerArena.contains(op)`.
- **[ARCHITECT-6] MINOR — `MetaRuntimeLimits` settings plumbing:** **Accepted.** Added
  `MetaRuntimeLimits` struct passed into `MetaRuntime` from `VagentExecutor`.
- **[ARCHITECT-7] MINOR — `VortexCore::rawWingSlotsOf` return type:** **Accepted.** Specified
  `std::vector<CellRef> rawWingSlotsOf(CellRef op, bool rightWing) const`.
- **[ARCHITECT-8] NIT — Proposal Register entry for Stage 3 (`compile_fragment`):** **Accepted.**
  Added `P14` to the Proposal Register for Stage 3 host-service compilation.

______________________________________________________________________

### 5. Authoring & Ergonomics Critic (`ergo`)

- **[ERGO-1] BLOCKING — The Value-Cell Paradox (`make_cell`):** **Accepted.** Added
  `make_cell(val: <any>): Cell` (checked via `MetaForm::MakeCell` in `checker.cpp` so it accepts
  `Int`, `Float`, `Bool`, `Str`, or `Cell` and mints a fresh cell with that typed content).
- **[ERGO-2] BLOCKING — Explicit in-language R2 Builder surface (`build_fragment`):** **Accepted.**
  Specified `build_fragment(spec: Cell): Opt<Cell>` with a complete, 2-degree-safe `spec` schema in
  Stage 4 so Stage 5 (`emitter.vagent`) emits fragments concisely without manual descriptor/recipe
  boilerplate.
- **[ERGO-3] MAJOR — Unbox `Code` to `Cell` via `head_of(c: Code): Cell`:** **Accepted.** Added pure
  `head_of(c: Code): Cell` accepting the bare `Code` wildcard so authors can pass any `Code<T>`
  handle to `registration(head_of(c))`, `validate(head_of(c))`, `label_of(head_of(c))`, etc.
- **[ERGO-4] MAJOR — Diagnostic observability via `var_of(cell: Cell, key: Str): Opt<Cell>`:**
  **Accepted.** Added pure `var_of(cell: Cell, key: Str): Opt<Cell>` to `std:meta` so VAgent
  programs can read `fault:<label>` tombstones and `$compile_error` / `$validate_error` records
  directly.
- **[ERGO-5] MAJOR — Pending-registration `run(c)` behavior & pre-check:** **Accepted.** Documented
  the `registration(head_of(c)) == "committed"` pre-check idiom and noted that
  `MetaRuntime::runFragment` sets `MetaRunOutcome::detail = "pending_registration"` when a head is
  still on `d.pending-reg`.
- **[ERGO-6] MAJOR — R1 AST child links, type annotations, and source locations:** **Accepted.**
  Specified `+d.grab` (first child), `+d.step` (next sibling), per-node `+d.name` (token), per-node
  `+d.values` (type string cell), and `d.vars` `"line"`/`"col"` records.
- **[ERGO-7] MINOR — Character classification helpers in Stage 7:** **Accepted.** Noted that
  `is_digit`, `is_alpha`, `is_space` are 3-line pure VAgent prelude helpers built on `byte_at` in
  `parser.vagent` (following `GEMINI.md`: leverage existing primitives in Vortex rather than adding
  C++ bloat).
- **[ERGO-8] MINOR — Dimension lookup caching in Stage 5 emitter:** **Accepted.** `build_fragment`
  resolves standard apparatus dimensions in C++ once, and `emitter.vagent` binds its dimension cells
  once at compile start.
- **[ERGO-9] NIT — `as_body` accepts bare `Code` wildcard:** **Accepted.** Signature is
  `as_body(c: Code): Opt<Code<()>>`.

______________________________________________________________________

### 6. ZigZag / Xanadu Data-Model Purist (`zigzag`)

- **[ZIGZAG-1] BLOCKING — `join_batch` 2-degree fan-in eviction on shared operand cells:**
  **Accepted.** Changed `join_batch` tuple schema so each tuple links on `+d.grab` → `+d.step` to
  fresh per-tuple argument cells holding `make_cell` / ref-box values (`[target, source]` for
  `"write"`, `[from, dim, dir, to]` for `"link"`), matching `fragment.cpp:353-369`'s `linkTuple` and
  `#LINKED`'s ref-box rank (`vortex_vm.cpp:864-868`). Zero fan-in on target graph cells!
- **[ZIGZAG-2] BLOCKING — P16 relocation & 2-degree rank design for `+d.lineage` (`P5`):**
  **Accepted.** Modeled `d.lineage` as a pure native 2-degree ZigZag rank where `sourceHead` sits at
  the posward tail (`twin_k +d.lineage → ... → twin_1 +d.lineage → sourceHead`, spliced via
  negward-of-`sourceHead` insertion at `reify`). Walking `+d.lineage` posward from any `twin`
  reaches `sourceHead` at the rank tail; walking `-d.lineage` negward from `sourceHead` enumerates
  all-time twins! Requires **0** `Int64` ref-carrier cells and **0** P16 relocation hooks because
  native links promote automatically!
- **[ZIGZAG-3] BLOCKING — R1 AST kind tags stored as cell content, never fan-in `+d.verb` links:**
  **Accepted.** Every R1 AST node stores its kind tag string as its own cell content, validated
  against the `d.ast.kinds` vocabulary rank on `programRoot`.
- **[ZIGZAG-4] MAJOR — R1 AST parent-to-child dimension (`+d.grab` / `+d.step`):** **Accepted.**
  Same fix as `WRITER-10` / `ERGO-6`.
- **[ZIGZAG-5] MAJOR — `kMetaDimensionNames` preservation + `"d.ast.kinds"`:** **Accepted.** Same
  fix as `WRITER-3` / `CODEBASE-1` / `ARCHITECT-1`.
- **[ZIGZAG-6] MAJOR — Dedicated trailed `+d.pending-reg` rank:** **Accepted.** Same fix as
  `WRITER-4`.
- **[ZIGZAG-7] MINOR — `rankContains` idempotency check in `registerCode`:** **Accepted.** Added
  explicit `kindRegistered(head)` / `codeRegistered(head)` check before linking.
- **[ZIGZAG-8] MINOR — Per-node type cells on R1 AST nodes:** **Accepted.** Each R1 AST node links
  `+d.values` to its own freshly minted type-name cell.
