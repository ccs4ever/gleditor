# Round 1 critique: Codebase Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§Global, lines 1–314) Absence of Proposal Register (`P<k>`) for C++ Changes

**Problem:** The review brief
(`design/projects/vagent-self-hosting-migration-review/brief-common.md:21-22`) and project standing
rules (`AGENTS.md` / `GEMINI.md`) strictly mandate:

> *"Proposal Register: Every change to existing code must be tracked in the numbered Proposal
> Register (`P<k>`)."* *"Argued, not asserted: Every design choice or stage boundary must cite the
> code (`file:line`), price the cost in bytes/cycles/links/cells, and state what alternative was
> refused."*

The target document proposes migrating over 20,000 lines of C++ code, gutting and shimmying 6 core
compilation translation units (`parser.cpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`,
`expand.cpp`, `resolve.cpp`), modifying `meta.cpp` and `executor.cpp`, deleting retired sources from
the `Makefile`, and introducing new test suites. Yet the document contains **zero numbered proposals
(`P<k>`)**, zero line-by-line modification tracking, zero price accounting in cells/links/cycles,
and zero recorded refusals.

**Evidence:** `grep -in "proposal" design/vagent-self-hosting-migration.md` yields 0 matches.

In `design/vagent-self-hosting-migration.md:239-242` (Phase 6):

> *"Replace C++ `parser.cpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`, `expand.cpp`,
> `resolve.cpp` with thin shims invoking the Stage 1/2 VAgent compiler image. Delete retired C++
> source files from the `Makefile`. Retain only the hardened engine kernel (`VortexVM`,
> `ArenaManifold`, `MeshActorRuntime`, `LineageStore`)."*

In `Makefile:733-743, 1196-1277`, `VAGENT_FRONTEND_SRCS`, `VAGENT_SYNTAX_OBJS`,
`VAGENT_RUNTIME_CORE_OBJS`, `vagent`, `vagent_frontend_test`, `vagent_syntax_test`,
`vagent_compiler_test`, and `fuzz_vagent_frontend` all directly compile and link these exact C++
objects. Deleting or replacing them alters the build graph for 8 distinct make targets and test
binaries.

Companion document `design/vagent-metacircular-compiler-implementation.md:863-882` tracks proposals
`P1` through `P14`. The continuation into self-hosting requires numbered proposals `P15` onward.

**Fix:** Add a dedicated Proposal Register section (`## 7. Proposal Register (P15–P22)`) tracking
every concrete C++ change:

- `P15`: Declaration and program AST schema extensions in `apps/common/xanadu/vagent/r1_ast.hpp` and
  `r1_ast.cpp`.
- `P16`: In-language compiler invocation shims in `inlang_parser.cpp`, `inlang_checker.cpp`, and
  `inlang_emitter.cpp`.
- `P17`: Frontend decoupling and C++ shim implementations in `frontend.cpp`, `parser.cpp`,
  `checker.cpp`, `compiler.cpp`, `decls.cpp`, `expand.cpp`, `resolve.cpp`.
- `P18`: Cognitive loop and intention arbitration migration from `executor.cpp` and `meta.cpp` into
  `stdlib/mind.vagent` and `stdlib/arbitration.vagent`.
- `P19`: Program image serialization and bootstrap persistence format in `lineage.cpp` and
  `bootstrap.cpp`.
- `P20`: `Makefile` target updates (`VAGENT_FRONTEND_SRCS`, syntax and runtime test harnesses, new
  quine test).
- `P21`: Elimination of `zigzag::noCell` sentinels across public `vagent` APIs
  (`inlang_checker.hpp`, `inlang_emitter.hpp`, `executor.hpp`).
- `P22`: Fuzz test harness adaptation in `tests/fuzz/fuzz_vagent_frontend.cpp`. Price every proposal
  in bytes, cycles, and links, and cite the exact `file:line` locations.

______________________________________________________________________

### 2. (§1 & §4, lines 16–17, 157–163) R1 AST Declaration Gap and Gate 1 Failure

**Problem:** §1 (lines 16–17) asserts:

> *"1. **The R1 AST Representation**: Homogeneous ZigZag cell graphs representing expressions,
> statements, declarations, and types without C++ AST object pointers."*

§4 (lines 159–163) defines Validation Gate 1:

> *"Parity test harness: parses all 23 `tests/samples/vagent/mini_*.vagent` and stdlib modules with
> both C++ `parser.cpp` and `parser.vagent`; verifies that the emitted R1 AST cell graph is
> structurally isomorphic."*

In the live repository, **the R1 AST representation does NOT support declarations, imports, or
modules.** `apps/common/xanadu/vagent/r1_ast.hpp:13-17` explicitly defines only 18 expression kind
tags:

```cpp
inline constexpr std::string_view kR1AstKinds[] = {
    "lit.int", "lit.float", "lit.bool", "lit.str", "lit.unit", "ident",
    "unary",   "binary",    "call",     "field",   "let",      "assign",
    "if",      "block",     "list",     "cells",   "believes", "code",
};
```

`apps/common/xanadu/vagent/r1_ast.cpp:77-139` (`marshalAstToR1`) takes only `const Expr &expr`,
rejecting any unhandled expression with `unsupported ExprKind` (`r1_ast.cpp:137`), and contains zero
handling for declarations (`Decl` in `ast.hpp:208-231` with 19 `DeclKind`s), `Import`
(`ast.hpp:233-238`), or `Program` (`ast.hpp:240-244`).

Furthermore, `apps/common/xanadu/vagent/inlang_parser.cpp:11-20` is hardcoded to parse only single
expressions:

```cpp
std::expected<zigzag::CellRef, std::string>
parseSourceToR1(zigzag::ArenaManifold &arena, zigzag::vortex::VortexCore &core,
                std::string_view source) {
  auto expr = Parser(source).parseExpression();
  if (!expr) {
    return std::unexpected(
        std::format("parse error: {}", formatDiagnostic(expr.error())));
  }
  return marshalAstToR1(arena, core, **expr);
}
```

All 23 sample files under `tests/samples/vagent/mini_*.vagent` are full programs declaring top-level
events and agents (e.g., `mini_01.vagent:1-9` begins with
`event Add { amount: Int } \n agent Sum { var total: Int := 0 ... }`). Passing any `mini_*.vagent`
file to the C++ `parseSourceToR1` bridge will immediately fail with a parse diagnostic or syntax
rejection.

Additionally, §4 (line 158) misstates the live R1 AST manifold layout: *"Builds R1 AST cells
connected via `+d.step` and annotated with `+d.name`, `+d.kind`, `+d.vars`."* In `r1_ast.hpp:24-29`
and `r1_ast.cpp:141-179`:

- The AST kind tag is the **cell text itself** (`node = arena.makeCell(kindTag)`); there is NO
  `+d.kind` dimension.
- First child expression links from parent via `node +d.grab -> child0` (`r1_ast.cpp:173`).
- Subsequent sibling expressions link via `prevChild +d.step -> nextChild` (`r1_ast.cpp:176`).
- Type annotations link via `node +d.values -> typeCell` (`r1_ast.cpp:147`).
- Source coordinates link via `node +d.vars` records (`r1_ast.cpp:152-164`).

**Evidence:** `apps/common/xanadu/vagent/r1_ast.hpp:13-17` vs `ast.hpp:184-244`.
`apps/common/xanadu/vagent/r1_ast.cpp:136-139, 141-179`.
`apps/common/xanadu/vagent/inlang_parser.cpp:14`. `tests/samples/vagent/mini_01.vagent:1-9`.

**Fix:**

1. Correct the R1 AST topology description in §1 and §4 to match `r1_ast.hpp:24-29` (`node` text =
   kind, `node +d.name` = identifier/token, `node +d.grab` = first child, `+d.step` = siblings,
   `node +d.values` = type annotation).
1. Specify the R1 declaration schema (e.g. `kR1DeclKinds` including `"decl.agent"`, `"decl.event"`,
   `"decl.plan"`, `"decl.var"`, `"decl.on"`, `"import"`), parent-child dimension linking for
   declaration members, and register `marshalProgramToR1` in `r1_ast.hpp` / `r1_ast.cpp` as Proposal
   `P15`.
1. Split Validation Gate 1: Phase 1a validates expression-level isomorphism against expression test
   cases; Phase 1b validates declaration-level isomorphism against `mini_01.vagent`–`mini_23.vagent`
   once `P15` lands.

______________________________________________________________________

### 3. (§1 & §2, lines 31, 59, 76–100) Broken LoC Count and Omission of Header Files

**Problem:** §1 (line 31) states:

> *"Currently, `apps/common/xanadu/vagent/` still carries **29,070 lines of C++23 code**..."*

A repository audit reveals that `29,070` is the sum of all 25 `.cpp` files (24,983 lines) AND all 29
`.hpp` files (4,087 lines) in `apps/common/xanadu/vagent/`. However, the Subsystem Feasibility
Matrix in §2 (lines 76–89):

1. **Counts `.cpp` files only**, summing to 24,824 lines. It completely omits all 4,087 lines of C++
   headers (`.hpp`), including structural definition files such as `ast.hpp` (248 lines),
   `records.hpp` (165 lines), `marshal.hpp` (150 lines), and `percept_codec.hpp` (173 lines).
1. **Completely omits `frontend.cpp` (54 lines) and `frontend.hpp` (53 lines)**, which constitute
   the public C++ entrypoint coordinating Parser, Resolve, Expand, and Check (`frontend.cpp:9-52`).
1. **Presents an impossible C++ kernel estimate of ~6,500 LoC.** Line 59 and line 92 assert that the
   retained C++ engine kernel will be "~6,500 LoC C++". Yet summing the Tier 3 `.cpp` files in §2
   alone yields 7,129 lines (`bootstrap.cpp` + `host_compile.cpp` + `r1_ast.cpp` + `inlang_*.cpp` =
   1,792 lines; `adapter.cpp` + `lineage.cpp` + `genesis.cpp` + `p16.cpp` = 2,537 lines; `meta.cpp`
   / `executor.cpp` retained core = ~2,800 lines). When adding the necessary Tier 3 and shared
   header files (`adapter.hpp`, `lineage.hpp`, `genesis.hpp`, `p16.hpp`, `bootstrap.hpp`,
   `host_compile.hpp`, `r1_ast.hpp`, `inlang_*.hpp`, `meta.hpp`, `executor.hpp`, `records.hpp`,
   `marshal.hpp`, `percept_codec.hpp` = 2,544 lines), the retained C++ code is at least **9,673
   lines**—48% higher than the claimed 6,500 LoC.

**Evidence:** Exact line count breakdown in `apps/common/xanadu/vagent/`:

- 25 `.cpp` files: 24,983 lines (`wc -l apps/common/xanadu/vagent/*.cpp`)
- 29 `.hpp` files: 4,087 lines (`wc -l apps/common/xanadu/vagent/*.hpp`)
- Total: exactly 29,070 lines.

Subsystem matrix row 10 (.cpp only): 519 (`bootstrap.cpp`) + 324 (`host_compile.cpp`) + 282
(`r1_ast.cpp`) + 342 (`inlang_checker.cpp`) + 303 (`inlang_emitter.cpp`) + 22 (`inlang_parser.cpp`)
= 1,792 lines. Omitted headers: 191 lines.

Subsystem matrix row 11 (.cpp only): 969 (`adapter.cpp`) + 580 (`lineage.cpp`) + 751 (`genesis.cpp`)
\+ 237 (`p16.cpp`) = 2,537 lines. Omitted headers: 672 lines.

Subsystem matrix row 12 (.cpp only): ~2,800 lines of `meta.cpp` and `executor.cpp`. Omitted headers:
1,139 lines.

Total Tier 3 retained lines: $7,129 \text{ (.cpp)} + 2,544 \text{ (.hpp)} = 9,673 \text{ LoC}$.

**Fix:**

1. Explicitly list and categorize both `.cpp` and `.hpp` files in §2's Feasibility Matrix.
1. Add `frontend.cpp` (54 lines) and `frontend.hpp` (53 lines) to Tier 1 under Frontend
   Coordination.
1. Correct the target C++ kernel size in §1 (line 59) and §2 (line 92) from ~6,500 LoC to ~9,700
   LoC, or identify specific subsystems within `adapter.cpp`, `lineage.cpp`, `genesis.cpp`, and
   `meta.cpp` that will be deleted to reach the 6,500 target.

______________________________________________________________________

## MAJOR

### 1. (§1 & §2, lines 60, 93–94) Conflation of `ArenaManifold` and `Store` Sealing

**Problem:** §1 (line 60) groups `"ArenaManifold / CompactOpNode / VortexVM Execution Loop"` as a
single kernel box, and §2 (line 93) justifies the permanent C++ substrate via:

> *"1. **Memory & Cache Layout**: 64-byte `CompactOpNode` cache line alignment, virtual memory
> `mmap` page protections (`PROT_READ` sealed segments)."*

This conflates two strictly separated architectural tiers:

1. `ArenaManifold` (`apps/common/xanadu/zigzag/arena_manifold.hpp:24-34`) is strictly ephemeral,
   contains **no operations**, appends **no `CompactOpNode`s**, and has **no `mmap` page
   protections**. Cells are stored in dense 24-byte `CellSlot` records (`arena_manifold.hpp:553`),
   and all cell references carry `ephemeralBit` (`arena_manifold.hpp:28-34`).
1. `CompactOpNode` (`apps/common/xanadu/compact_op.hpp:33-56`) and `PROT_READ` page protections
   belong exclusively to `SegmentedOpsSpool` (`segmented_ops_spool.hpp:35`) and `Store`
   (`store.hpp:52`).

The VAgent compiler pipeline runs entirely within `ArenaManifold`
(`apps/common/xanadu/zigzag/arena_manifold.hpp`). It does not touch `CompactOpNode` directly. The
only point where `CompactOpNode` is involved is when durable agent state or program images are
promoted across the ephemeral boundary into `LineageStore` via
`zigzag::promote(lineage, latest, arena, ...)` (`apps/common/xanadu/vagent/lineage.cpp:347, 539`).
Failing to distinguish these lifecycles obscures a fatal hazard: if intermediate R1 AST cells minted
in the arena are not discarded via `arena.release(m)` before `seedProgramImage` or `regenesisCycle`
runs, `promote()` will attempt to promote hundreds of thousands of ephemeral AST compile-residue
cells into the persistent `LineageStore`, corrupting hypertime and blowing segment limits.

**Evidence:** `apps/common/xanadu/zigzag/arena_manifold.hpp:14-22`:

```cpp
class Manifold;      // ops-backed. Every mutation appends a CompactOpNode.
class ArenaManifold; // dense vectors only. No ops. Dies with its owner.
```

`apps/common/xanadu/vagent/lineage.hpp:214-235`: `regenesisCycle` promotes session cells into
`Store`'s successor band, keeping shadowed cells at or below `imageExtent` out of `extraRoots`.
`design/vagent-self-hosting-migration.md:60, 93-94`.

**Fix:** Restructure §1 and §2 to decouple the ephemeral manifold lifecycle from the durable spool
lifecycle. Explicitly specify that all compilation runs within an isolated scratch `ArenaManifold`
(or under an arena `Mark` released via `arena.release(m)`), ensuring zero AST compile residue
reaches `LineageStore` or `zigzag::promote`.

______________________________________________________________________

### 2. (§4, lines 232–235) Undefined "Bit-Exact Quine" vs Live Monotonic `CellRef`s

**Problem:** §4 Phase 5 (lines 232–235) asserts:

> *"Image 2 and Image 3 must be **bit-for-bit identical** (modulo non-deterministic timestamp salts,
> which are canonicalized). This proves the metacircular compiler is a fixed point."*

In the live repository:

1. `ArenaManifold` cell allocation (`arena.makeCell()`) assigns monotonically incrementing integers
   to `CellRef`s (`arena_manifold.hpp:553`). Compiling the compiler in successive passes will
   allocate different raw `CellRef` values (`head2 != head3`). Raw in-memory manifolds can never be
   "bit-for-bit identical".
1. In the existing quine test harness
   (`tests/xuzz/vagent_metacircular_quine_test.cpp:43-108, 199-219`), the fixed-point test does NOT
   assert bit-for-bit identity. It asserts **structural graph isomorphism** via `snapshotFragment`:
   recipe length, opKinds, ref count, parameter count, result type, and purity/closure flags.
1. If "Image" refers to a serialized `Store` segment (`ops.nodes`), the document defines no
   serialization mechanism, no tool invocation (`xudu-dump`), and no specification of how
   `MicroversionId` Merkle roots and author signatures are canonicalized.

**Evidence:** `design/vagent-self-hosting-migration.md:232-235`.
`tests/xuzz/vagent_metacircular_quine_test.cpp:199-219`:

```cpp
const auto snap2 = snapshotFragment(arena, h.core, h.rt(), head2);
const auto snap3 = snapshotFragment(arena, h.core, h.rt(), head3);
EXPECT_EQ(snap2.recipeLength, snap3.recipeLength);
EXPECT_EQ(snap2.refCount, snap3.refCount);
EXPECT_EQ(snap2.paramCount, snap3.paramCount);
EXPECT_EQ(snap2.resultType, snap3.resultType);
EXPECT_EQ(snap2.pure, snap3.pure);
EXPECT_EQ(snap2.closed, snap3.closed);
```

**Fix:** Replace "bit-for-bit identical" with "normalized structural graph isomorphism (per
`FragmentStructure` in `tests/xuzz/vagent_metacircular_quine_test.cpp:43-52`)", or provide the exact
binary format specification (e.g. canonical Merkle-hashed `SegmentedOpsSpool` segment via
`seedProgramImage`) and state the comparison algorithm.

______________________________________________________________________

### 3. (§1 & §5, lines 18–19, 290) Incoherent Opcode Minting and Genesis Primitives

**Problem:** §1 (lines 18–19) and §5 (line 290) specify:

> *"2. **The Genesis Bootstrap**: Pure in-manifold `writer` (`Value`) and `linker` (`Link`)
> primitives minted at genesis and reachable via `std:meta (boot_writer, boot_linker)`."* *"Track C1
> (`stdlib/emitter.vagent`): Emits wrapped opcode sequences via genesis `boot_writer` and
> `boot_linker`."*

In the live C++ codebase:

1. `boot_writer` (`apps/common/xanadu/vagent/bootstrap.cpp:218-323`) wraps `OpcodeKind::Value` to
   write cell payloads.
1. `boot_linker` (`apps/common/xanadu/vagent/bootstrap.cpp:325-458`) wraps `OpcodeKind::Link` to
   link two cells across a dimension. Neither primitive can mint opcodes or register them in
   `VortexVM::opcodeMap_`. In `apps/common/xanadu/vagent/meta.cpp:3268-3272`
   (`MetaRuntime::registerCode`):

```cpp
for (const auto op : *ops) {
  if (const auto ord = arena().asInt64(op)) {
    vm_.setOpcodeKind(op, static_cast<OpcodeKind>(*ord));
  }
}
```

`registerCode` requires each opcode cell to contain an `Int64` scalar equal to
`static_cast<std::int64_t>(OpcodeKind)`. However,
`apps/common/xanadu/vagent/stdlib/emitter.vagent:10-27` currently emits string tags like `"#ADD"`,
`"#SUB"`, which `registerCode` will reject because `asInt64(op)` returns `nullopt`. Furthermore,
`inlang_emitter.cpp:34-42` currently mints opcodes by calling `vm.mintOpcode(kind)`, which is a C++
host method. The document nowhere specifies how `emitter.vagent` writes integer scalar opcodes into
cells without relying on C++ host helpers.

**Evidence:** `apps/common/xanadu/vagent/bootstrap.cpp:218-458`.
`apps/common/xanadu/vagent/meta.cpp:3268-3272`.
`apps/common/xanadu/vagent/stdlib/emitter.vagent:10-27`.
`apps/common/xanadu/vagent/inlang_emitter.cpp:34-42`.

**Fix:** Specify that `stdlib/emitter.vagent` emits integer scalar cells representing `OpcodeKind`
ordinals using `makeScalarCell` / `boot_writer`, and update `stdlib/emitter.vagent` to return
integer ordinals instead of string tags `"#ADD"`.

______________________________________________________________________

### 4. (§6, lines 309–311) Unaddressed `noCell` Sentinels Across VAgent C++ APIs

**Problem:** §6 (lines 309–311) claims:

> *"3. **Zero Sentinels**: All missing nodes, lookup misses, and optional branches use `Opt<Cell>`
> (`None` / bare cell), preserving the strict zero-sentinel rule across both C++ and VAgent
> boundaries."*

While `.vagent` surface syntax supports `Opt<Cell>`, the live C++ codebase in
`apps/common/xanadu/vagent/` heavily relies on `zigzag::noCell` in its public function signatures
and structs, violating the core rule in `AGENTS.md`:

> *"No sentinels outside wire and on-disk formats: In an API, 'no cell', 'no index' and the like are
> `std::optional` (or `std::expected`), never a reserved value such as `noCell`."*

Examples in the live code:

- `apps/common/xanadu/vagent/inlang_checker.hpp:42`: `zigzag::CellRef params = zigzag::noCell`
- `apps/common/xanadu/vagent/inlang_emitter.hpp:31`: `zigzag::CellRef params = zigzag::noCell`
- `apps/common/xanadu/vagent/executor.hpp:40, 43, 47, 71, 84, 224`:
  `CellRef payloadCell{zigzag::noCell}`, `zigzag::CellRef sender = zigzag::noCell`
- `apps/common/xanadu/vagent/records.hpp:41, 57`: `zigzag::CellRef anchor = zigzag::noCell`

If Phase 6 replaces C++ compiler passes with shims, the C++ boundary signatures must be audited and
cleaned to eliminate `noCell` in favor of `std::optional<zigzag::CellRef>`.

**Evidence:** `apps/common/xanadu/vagent/inlang_checker.hpp:42`.
`apps/common/xanadu/vagent/inlang_emitter.hpp:31`. `apps/common/xanadu/vagent/executor.hpp:224`.
`AGENTS.md` standing rule on sentinels.

**Fix:** Track an explicit C++ API cleanup proposal (`P21`) in the Proposal Register converting all
`CellRef ... = noCell` parameters across `apps/common/xanadu/vagent/*.hpp` to
`std::optional<zigzag::CellRef>`.

______________________________________________________________________

### 5. (§2, line 97) Erroneous OS Substrate Justification ("SDL3 Events")

**Problem:** §2 (line 97) justifies retaining ~6,500 LoC in C++ citing:

> *"3. **Hardware / OS Boundaries**: Thread synchronization, `epoll`/POSIX sockets, SDL3 events, and
> libtorrent wire packet marshaling."*

In the live repository, `apps/common/xanadu/vagent/` is a headless cognitive agent and compiler
runtime with zero windowing or GUI dependencies. Running `grep -rn "SDL" apps/common/xanadu/vagent/`
returns **0 results**. SDL (SDL2/SDL3) is utilized exclusively by `src/` (the editor rendering
library) and `apps/xuzz/` (the UI application). Citing SDL3 events as a justification for the VAgent
C++ substrate is factually inaccurate.

**Evidence:** `grep -rn "SDL" apps/common/xanadu/vagent/` yields no matches.
`design/vagent-self-hosting-migration.md:97`.

**Fix:** Strike "SDL3 events" from line 97 and cite actual VAgent OS dependencies: timer deadlines
(`adapter.cpp`), worker thread execution loops, POSIX sockets, and libtorrent peer transport.

______________________________________________________________________

## MINOR

### 1. (§2 vs §3, lines 86, 115, 123) Discrepancy Between Matrix and Stdlib Layout

**Problem:** In §2's Subsystem Matrix (line 86), the target destination for Plan Selection &
Arbitration is listed as `stdlib/arbitration.vagent, stdlib/intentions.vagent`. In §3's standard
library file tree (lines 108–125), `stdlib/intentions.vagent` is completely missing, while
`stdlib/ast.vagent` is introduced. Conversely, `stdlib/ast.vagent` is omitted from the Subsystem
Matrix in §2.

**Evidence:** `design/vagent-self-hosting-migration.md:86`: lists `stdlib/intentions.vagent`.
`design/vagent-self-hosting-migration.md:115, 123`: lists `stdlib/ast.vagent`, omits
`intentions.vagent`.

**Fix:** Reconcile §2 and §3: include `ast.vagent` in the matrix under AST/Parser and clarify
whether `intentions.vagent` is a distinct module or subsumed by `stdlib/arbitration.vagent`.

______________________________________________________________________

### 2. (§5, line 296) Phantom Test File Citation (`vagent_self_hosting_test.cpp`)

**Problem:** Track E cites `tests/xuzz/vagent_self_hosting_test.cpp` as the location for the
multi-stage bootstrapping test suite. This file does not exist in the repository; the existing quine
test is `tests/xuzz/vagent_metacircular_quine_test.cpp` (221 lines). If a new test suite file is
intended, it must be noted as a new file and tracked in the Proposal Register.

**Evidence:** `ls tests/xuzz/vagent_self_hosting_test.cpp` fails (`No such file or directory`).
`tests/xuzz/vagent_metacircular_quine_test.cpp` exists in the repository.

**Fix:** Update the citation to `tests/xuzz/vagent_metacircular_quine_test.cpp` or register
`tests/xuzz/vagent_self_hosting_test.cpp` as a newly created test binary in Proposal `P20` and the
`Makefile`.

______________________________________________________________________

### 3. (§4, line 149) Discrepancy in `Token` Record Schema vs `token.hpp`

**Problem:** §4 Track 1.1 (line 149) states that `stdlib/lexer.vagent` returns "Token records
(`Token{kind, text, line, col}`)". In `apps/common/xanadu/vagent/token.hpp:46-52`, `Token` is
defined as:

```cpp
struct Token {
  TokenKind kind{TokenKind::EndOfFile};
  std::string text;
  SourceLocation loc;
  ...
};
```

where `loc` is a nested `SourceLocation` (`token.hpp:14-18`:
`{std::size_t line, std::size_t column}`). Flattening this into `{kind, text, line, col}` creates an
impedance mismatch between C++ and VAgent representations.

**Evidence:** `apps/common/xanadu/vagent/token.hpp:14-18, 46-52`.
`design/vagent-self-hosting-migration.md:149`.

**Fix:** Clarify whether the VAgent record conforms to
`{kind: Str, text: Str, loc: SourceLocation{line: Int, col: Int}}` or document the deliberate
flattening.

______________________________________________________________________

VERDICT: NOT CONVERGED
