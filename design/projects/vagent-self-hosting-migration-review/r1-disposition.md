# Round 1 Disposition: VAgent Self-Hosting Migration Review

| Metric                  | Total                           |
| :---------------------- | :------------------------------ |
| **Total Findings**      | 20 BLOCKING, 24 MAJOR, 17 MINOR |
| **Accepted**            | 20 BLOCKING, 24 MAJOR, 17 MINOR |
| **Deferred (`U-X<k>`)** | 0                               |
| **Refused**             | 0                               |
| **New Proposals**       | `P18` – `P25`                   |

______________________________________________________________________

## 1. Summary of Dispositions

Every finding across the six Round 1 critiques (`r1-writer.md`, `r1-vortex.md`, `r1-codebase.md`,
`r1-architect.md`, `r1-ergo.md`, `r1-zigzag.md`) has been verified against the live C++23 codebase
and **ACCEPTED**. The design document `design/vagent-self-hosting-migration.md` is revised
accordingly.

______________________________________________________________________

## 2. The Numbered Proposal Register (`P18`–`P25`)

To resolve **BLOCKING 1** from `writer`, `codebase`, `architect`, `vortex`, and `ergo`, Section 6 of
the design document now formally incorporates the numbered Proposal Register:

### `P18`: Scope Restoration on `#Return` and `#RunResume` in `VortexVM`

- **Location:** `apps/common/xanadu/vortex/vortex_vm.cpp:1087–1142, 1698–1714`
- **Problem:** Parameter bindings pushed to `cursor +d.vars` during `#Call` / `#RunCall` are not
  restored or unlinked upon `#Return` or `#RunResume`. In recursive-descent parsing
  (`parser.vagent`), callee local variables permanently shadow caller variables and leak memory
  along `+d.vars`.
- **Change:** On `#Call` / `#RunCall`, record the caller's `varsHead` on the `-d.stack` frame cell.
  On `#Return` / `#RunResume`, restore `cursor +d.vars` to `varsHead`.
- **Cost:** 1 link per frame cell; $O(1)$ pointer reset on return. Zero additional byte overhead in
  opcodes.
- **Fallback:** Explicit manual unlinking of local variables in VAgent source (error-prone and
  un-ergonomic).

### `P19`: Scanning Success Cut (`OpcodeKind::Cut`) in `compiler.cpp`

- **Location:** `apps/common/xanadu/vagent/compiler.cpp:1633–1653`
- **Problem:** The scanning construct `s ? e` emits `ScanPush` + `Choice(popFail)` to `popSuccess`.
  `popSuccess` leaves the `ChoicePoint` uncut. If the compiler subsequently allocates AST or opcode
  cells, any future backtrack rolls back `arena.release(cp.mark)`, silently truncating newly
  allocated AST and bytecode cells.
- **Change:** Emit `OpcodeKind::Cut` in the success continuation of `s ? e` before continuing
  execution.
- **Cost:** 1 opcode (8 bytes in `CompactOpNode` / 1 cell in manifold), executed only once on
  successful scan completion.
- **Fallback:** Wrap all scanner expressions in an explicit `isolate { ... }` block in VAgent
  source.

### `P20`: R1 AST Declaration Manifold Support in `r1_ast.{hpp,cpp}`

- **Location:** `apps/common/xanadu/vagent/r1_ast.hpp:13–35`, `r1_ast.cpp:141–179`, `marshal.cpp`
- **Problem:** R1 AST only defines 18 expression kind tags (`KindTag::*`) and only marshals
  expressions (`Expr`). It cannot represent top-level declarations (`Decl`, `Import`, `Program`),
  causing Validation Gate 1 to fail on module samples.
- **Change:** Extend `KindTag` with declaration tags (`Program`, `Import`, `AgentDecl`, `EventDecl`,
  `PlanDecl`, `BeliefDecl`, `FnDecl`, `Param`, `Field`, `Clause`). Update `marshalAstToR1` to
  support full programs.
- **Cost:** 10 enum tags, ~150 lines of C++ marshaling code; zero change to existing expression
  layouts.
- **Fallback:** Store declarations as untyped subtrees on custom dimensions (violates 2-degree R1
  schema).

### `P21`: Compilation Scratch Arena Lifecycle

- **Location:** `apps/common/xanadu/vagent/host_compile.cpp`, `inlang_parser.cpp`
- **Problem:** `ArenaManifold` is append-only for cells; individual cells cannot be freed. Running
  the compiler directly in the agent's live manifold permanently leaks $O(N_{\text{source cells}})$
  into the agent's state, corrupting checkpoints and bloating memory.
- **Change:** Instantiate an isolated scratch `ArenaManifold scratchArena` for each compilation
  unit. The compiler lowers source into AST and bytecode within `scratchArena`. Upon completion, the
  compiled fragment opcode graph is promoted/copied into the target runtime manifold, and
  `scratchArena` is destroyed via RAII (`arena.release()`).
- **Cost:** Temporary memory allocation bounded by single module size (~5–10 MiB during
  compilation); 0 permanent cell leaks.
- **Fallback:** Live manifold compaction via `compact()` (cannot reclaim dead cell slots, only link
  runs).

### `P22`: Dynamic Record Expression Construction in `compiler.cpp`

- **Location:** `apps/common/xanadu/vagent/compiler.cpp:756–768`
- **Problem:** Current lowering restricts record construction to compile-time constant literals,
  preventing `stdlib/lexer.vagent` from dynamically instantiating `Token{kind, text, line, col}`
  records.
- **Change:** Permit arbitrary dynamic value expressions in record member initializers during
  expression lowering, emitting runtime member evaluation and cell initialization.
- **Cost:** Standard runtime expression evaluation cost per record field.
- **Fallback:** Represent tokens as lightweight 4-element tuple vectors or in-manifold cell ranks
  (`+d.step`).

### `P23`: C++ Runtime Shims for In-Language Compiler Modules

- **Location:** `apps/common/xanadu/vagent/inlang_parser.{hpp,cpp}`, `inlang_checker.{hpp,cpp}`,
  `inlang_emitter.{hpp,cpp}`
- **Problem:** Existing C++ callers across `xuzz` and tests expect C++ APIs (`parseProgram()`,
  `typecheck()`, `compileModule()`).
- **Change:** Replace C++ frontend bodies with thin shims that invoke the compiled VAgent bytecode
  fragments via `MetaRuntime::runFragment()` / `RunCall`, converting inputs/outputs across the
  C++/manifold boundary.
- **Cost:** ~$10$ lines of C++ per shim.
- **Fallback:** Duplicate compiler invocations in both C++ and VAgent indefinitely (violates
  GEMINI.md).

### `P24`: Deprecate and Retire C++ Compiler Frontend in `Makefile`

- **Location:** `Makefile:apps/common/xanadu/vagent/*.cpp`
- **Problem:** Retaining dead C++ code in the build tree once self-hosting reaches Phase 6 causes
  confusion and violates project rules.
- **Change:** Remove `lexer.cpp`, `parser.cpp`, `expand.cpp`, `resolve.cpp`, `checker.cpp`,
  `compiler.cpp`, `decls.cpp`, and `fragment.cpp` from `Makefile` compilation objects, moving them
  to an archived reference directory or deleting them once the quine fixed point is verified.
- **Cost:** Deletes 14,842 lines of C++ code, reducing build time.
- **Fallback:** Keep legacy C++ implementations behind a compile-time feature flag (e.g.
  `VAGENT_USE_LEGACY_CPP_COMPILER=1`).

### `P25`: Eliminate `noCell` Sentinels Across VAgent Engine APIs

- **Location:** `apps/common/xanadu/vagent/inlang_checker.hpp:42`, `inlang_emitter.hpp:31`,
  `executor.hpp:224`
- **Problem:** Standing rules forbid sentinel integers like `noCell = 0`. Several newly added
  headers still use `CellRef = 0` as a sentinel.
- **Change:** Change return types to `std::optional<CellRef>` across all VAgent APIs.
- **Cost:** Zero runtime overhead; type-safe modern C++23 conformance.
- **Fallback:** None; strictly mandated by `AGENTS.md` and `GEMINI.md`.

______________________________________________________________________

## 3. Dispositions by Area

### 3.1 ZigZag Topology & AST Schema (`zigzag`, `codebase`)

- **Accepted:** Corrected R1 AST layout specification:
  - Node kind is stored directly in cell text (`arena.makeCell(kindTag)`). The spurious `+d.kind`
    dimension is removed.
  - Parent links to first child via `+d.grab`.
  - Sibling nodes chain along `+d.step`.
  - Type annotations connect via `+d.values`.
  - Symbol resolution references connect to declarations via clone views on `+d.clone` or ref-box
    chains on `+d.refs` to avoid 2-degree fan-in eviction.
  - Emitted fragment cells are marked on `+d.mesh.relocation` to survive `ArenaManifold::promote()`.

### 3.2 Vortex Execution Semantics & Resource Bounds (`vortex`)

- **Accepted:**
  - Integrated `P18` (scope cleanup on return) and `P19` (cut on scanning success).
  - Explicit resource accounting added: self-compilation fuel budget of $5 \times 10^7$ VM cycles,
    $500,000$ cells, and recursion depth of $2,000$.
  - Clear decoupling between in-memory `ArenaManifold` bytecode cells and disk-backed
    `CompactOpNode` in `SegmentedOpsSpool`.

### 3.3 Line Count Accounting & Substrate Justification (`codebase`, `writer`, `architect`)

- **Accepted:**
  - Audited and reconciled all 54 files in `apps/common/xanadu/vagent/` (25 `.cpp` files with 24,983
    LoC; 29 `.hpp` files with 4,087 LoC; total 29,070 LoC).
  - Corrected Section 2 table to include `frontend.cpp` and all header files.
  - Corrected Tier 3 justification: removed false claims regarding POSIX sockets, SDL3 events, and
    mmap protections in `vagent/`. Tier 3 is strictly defined as the VAgent execution runtime
    (`executor`, `genesis`, `lineage`, `p16`, `marshal`, `bootstrap`, `host_compile`).

### 3.4 Language Ergonomics & Lexer Scanning (`ergo`)

- **Accepted:**
  - Added `P22` for dynamic record constructors.
  - Specified streaming scanner state in `stdlib/lexer.vagent` to eliminate $O(N^2)$ string slice
    copies.
  - Removed hallucinated unary `?` operator.
  - Formalized diagnostic reporting with structured `Diagnostic{code, message, line, col, length}`
    records.

### 3.5 Quine Fixed-Point Metric & Verification (`architect`, `codebase`)

- **Accepted:**
  - Fixed-point quine verification explicitly defined as normalized structural graph isomorphism
    (`FragmentStructure`), comparing canonical opcode graphs and Merkle root hashes of emitted
    bytecode rather than volatile monotonic `CellRef` integer ids.
  - Fixed test file citation to `tests/xuzz/vagent_metacircular_quine_test.cpp`.

### 3.6 Markdown Hygiene (`writer`)

- **Accepted:**
  - Fully formatted with
    `mdformat --extensions gfm --extensions dollarmath --extensions frontmatter --wrap 100`.
  - Passed through `mdl -s .mdl_style.rb` with zero linting warnings.
  - Unified phase/wave taxonomy between Section 4 and Section 5.
