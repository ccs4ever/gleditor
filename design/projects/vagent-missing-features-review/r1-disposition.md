# Round 1 Disposition: VAgent Metacircular Missing Features Review

| Review Round | Target Document                                  | Date       | Disposition Status |
| :----------- | :----------------------------------------------- | :--------- | :----------------- |
| **Round 1**  | `design/vagent-metacircular-missing-features.md` | 2026-11-08 | All findings ruled |

______________________________________________________________________

## 1. Summary of Round 1 Findings & Convergence Status

Six specialized read-only critics evaluated `design/vagent-metacircular-missing-features.md`:

- **Technical Writer** (`r1-writer.md`): 2 BLOCKING, 3 MAJOR, 2 MINOR (`NOT CONVERGED`)
- **Vortex & Spool-Mesh Expert** (`r1-vortex.md`): 4 BLOCKING, 4 MAJOR, 4 MINOR (`NOT CONVERGED`)
- **Codebase Expert** (`r1-codebase.md`): 4 BLOCKING, 4 MAJOR, 5 MINOR (`NOT CONVERGED`)
- **System Architect** (`r1-architect.md`): 3 BLOCKING, 3 MAJOR, 2 MINOR (`NOT CONVERGED`)
- **PL Design & Ergonomics Expert** (`r1-ergo.md`): 3 BLOCKING, 5 MAJOR, 3 MINOR (`NOT CONVERGED`)
- **Xanadu / ZigZag Purist** (`r1-zigzag.md`): 3 BLOCKING, 3 MAJOR, 4 MINOR (`NOT CONVERGED`)

There is striking unanimity across all six critics on the core architectural issues. Below is the
orchestrator's definitive ruling and remediation plan for every finding.

______________________________________________________________________

## 2. Definitive Rulings on BLOCKING Findings

### 2.1 Cross-Arena Raw CellRef Leaks in Multi-Agent Swarm (Pattern B)

- **Found by:** `zigzag` (B1), `vortex` (B1), `architect` (B2), `ergo` (B1)
- **Problem:** §4.2 passes un-promoted `CellRef` handles (`ast_root`, `image_root`) across distinct
  actors in `ActorSend` messages, and `CompilerCoordinator` attempts to call `promote(image_root)`
  on cells minted in `EmitterWorker`'s private arena. In `MeshActorRuntime`, each actor has its own
  `WorkerContext` and `ArenaManifold`. A `CellRef` is a local arena slot index; raw cell refs cannot
  cross actor boundaries (Ruling V1: "An agent is one actor; there are no links between arenas").
- **Ruling: ACCEPTED.**
- **Remediation:**
  1. Mandate that intra-file compiler passes (lexing, parsing into R1 AST, type checking, opcode
     emission) run synchronously within a single agent's arena via `behavior StagedCompiler`
     (Pattern A).
  1. Reposition Pattern B (`agent CompilerCoordinator`) to operate at **module/compilation-unit
     granularity**: worker actors compile entire source files in their private arenas, promote the
     resulting image locally via `std:zigzag:promote`, and transmit the resulting immutable
     `MicroversionId` string across the mesh.

### 2.2 Layering Inversion & Lifecycle in `std:actor:spawn_image`

- **Found by:** `architect` (B1), `vortex` (B2), `codebase` (M2), `zigzag` (M6)
- **Problem:** Placing `std:actor:spawn_image` in generic `vortex_stdlib.cpp` causes an inverted
  dependency on upper-layer `vagent::MetaRuntime` and `VagentAdapter`. Furthermore,
  `MeshActorRuntime::spawn()` takes `ActorLineage{baseManifold, store}`, not `ActorGenesis`. If the
  rebuilt `Manifold` is temporary, `WorkerContext::baseManifold` dangles. Finally, `spawn_image`
  cannot synchronously pump the child turn loop during an active turn without violating turn
  atomicity.
- **Ruling: ACCEPTED.**
- **Remediation:**
  1. Remove `std:actor:spawn_image` from `vortex_stdlib.cpp`. Expose dynamic actor spawning as an
     action in `VagentAdapter` and `executor.cpp` (`apps/common/xanadu/vagent/`).
  1. Update lifecycle: `MetaRuntime::validate` audits opcodes, `zigzag::promote` seals the store,
     `Store::rebuildManifold` produces a `Manifold` anchored in an actor runtime image cache keyed
     by `MicroversionId`, and `MeshActorRuntime::spawn` is invoked with `ActorLineage`.
  1. `spawn_image` queues a P4 wake (`runtime.wake(childPid)`) and returns
     `Result<ActorPid, ActorError>`. The child executes its `Initialize` handler cooperatively on
     subsequent pump turns.

### 2.3 Choice Stack Leaks & Trail Destruction in Pattern Matching

- **Found by:** `vortex` (B3)
- **Problem:** Specifying `#Choice`/`#Fail` for `match` guards pushes a `ChoicePoint` with an arena
  mark. Without mandatory `#Cut`, choice points remain on `choiceStack_` across returns. Subsequent
  backtracking calls `arena.release(mark)`, destroying all cells minted in the arm body and later
  execution.
- **Ruling: ACCEPTED.**
- **Remediation:** Lower `match` expressions into deterministic decision trees using `#Equal`,
  `#Branch`, and `#ExtractField`, avoiding `#Choice`/`#Fail` entirely. For backtracking guards,
  mandate that `#ChoiceDepth` is captured and `#Cut` is unconditionally executed upon entering the
  succeeding arm before body evaluation.

### 2.4 Missing Relocation Marker (`d.mesh.relocation`) and `d.dims` in Promotion

- **Found by:** `zigzag` (B3), `vortex` (B4), `codebase` (B4)
- **Problem:** `zigzag::promote` refuses ephemeral `OpHandle` cells unless they carry
  `+d.mesh.relocation -> "cell"`. Additionally, `promote` skips `d.dims` by default, leading to
  orphan twins in rebuilt manifolds unless `extraRoots` includes `d.dims` and relocation targets,
  followed by P16 slot restatement (`lineage.cpp:322–349`).
- **Ruling: ACCEPTED.**
- **Remediation:** Formally specify the promotion contract in §3.2 and §5.2: `emitter.vagent` marks
  all intra-image jump/branch targets along `+d.mesh.relocation -> "cell"`. `std:zigzag:promote`
  passes `extraRoots` containing `d.dims` and relocation targets, and runs P16 fixups post-commit.

### 2.5 Active 2-Degree Eviction Bug in `zzCloneToChain`

- **Found by:** `zigzag` (B2)
- **Problem:** `VortexStdLib::zzCloneToChain` (`vortex_stdlib.cpp:5087–5099`) links `symbolOp`
  directly along `+d.clone`, clobbering the previous clone when a second clone is minted.
- **Ruling: ACCEPTED.**
- **Remediation:** Expand Proposal P27 to fix `zzCloneToChain` by walking to the posward tail of the
  master's `d.clone` rank before linking the new clone.

### 2.6 VAgent Surface Syntax & Grammar Non-Conformance

- **Found by:** `writer` (B1, B2), `ergo` (B2, B3)
- **Problem:** Listings throughout the document used invalid grammar: module braces, `-> RetType`
  instead of `: RetType`, `=` instead of `:=`, missing `within` on `achieve ... by`, missing plan
  names on `plan`, missing `do` on `for`, and ambiguous `match` EBNF without arm separators or
  header keywords.
- **Ruling: ACCEPTED.**
- **Remediation:** Correct all VAgent listings to conform to live grammar. Define
  `MatchExpr ::= "match" Expr "with" "{" MatchArmList "}"` with comma/newline-delimited arms, `?var`
  binding patterns, `let Pattern := Expr`, and literal `none`.

______________________________________________________________________

## 3. Definitive Rulings on MAJOR Findings

### 3.1 Mutable Store Binding and Sentinel-Free `promote`

- **Found by:** `writer` (M2), `vortex` (M1), `codebase` (B4), `zigzag` (M5), `architect` (M3)
- **Ruling: ACCEPTED.** Proposal P27 updates `boundStore_` in `VortexStdLib` to mutable `Store*`,
  derives `parentVersion` from `boundStore_->primaryCurrentVersion()`, and returns
  `Result<Str, ZigzagError>` instead of a bare string.

### 3.2 Parameterless `consumeWork()` and Memory Scratch Bounding

- **Found by:** `codebase` (B1, B2), `vortex` (M2), `architect` (M2)
- **Ruling: ACCEPTED.** `consumeWork()` takes zero arguments. Replace hallucinated
  `consumeWork(file_byte_size)` with standard cycle accounting, and replace
  `ActorError::GrantExceeded` with `ActorError::ResourceLimit`. Input file size is bounded by
  scratch memory limits, not wire `maxOutputBytes`.

### 3.3 Separation of Primedia Text (`set_text`) from Scalar Bits (`set_value`)

- **Found by:** `codebase` (B3)
- **Ruling: ACCEPTED.** In Nelsonian storage, text is primedia, not a scalar `ValueKind`.
  `set_value` handles scalar bits (`Int64`, `Double`, `Bool`, `Timestamp`, `OpHandle`, `None`). A
  dedicated `set_text(cell: CellRef, text: Str) -> Result<CellRef, ZigzagError>` primitive is added
  to update text content via interned scratch spans.

### 3.4 Fallibility and Valid Types on `std:zigzag` Primitives

- **Found by:** `vortex` (M3), `ergo` (M7), `writer` (M1)
- **Ruling: ACCEPTED.** All proposed `std:zigzag` primitives return `Opt<CellRef>` or
  `Result<T, ZigzagError>`. Discontinue pseudo-type `any`. Types standardized to `Str`, `Int`,
  `Float`, `Bool`, `()`, `[T]`, `Opt<T>`, `Result<T, E>`.

### 3.5 Proposal P30 Inventory Scope Expansion

- **Found by:** `codebase` (M1), `writer` (B2), `ergo` (M8)
- **Ruling: ACCEPTED.** Expand P30 to explicitly include `apps/common/xanadu/vagent/token.hpp`,
  `token.cpp`, `lexer.cpp`, `stdlib/lexer.vagent`, `ast.hpp`, `r1_ast.hpp/.cpp`,
  `stdlib/ast.vagent`, `stdlib/checker.vagent`, and `stdlib/emitter.vagent`.

### 3.6 Host CLI Argument Forwarding Mechanism in P26

- **Found by:** `codebase` (M3)
- **Ruling: ACCEPTED.** Proposal P26 adds `VortexStdLib::setArgs(std::vector<std::string>)`, and
  updates `apps/vagent/main.cpp` to forward unconsumed CLI arguments into the execution context.

______________________________________________________________________

## 4. Proposal Register Updates

The Proposal Register (`P26`–`P30`) is updated to reflect all accepted remediations:

- **`P26`**: Expose `std:io:read_file`, `std:io:read_stdin`, `std:env:args`, `std:io:print`, and
  `std:io:eprint`. Add `setArgs` to `VortexStdLib`. Enforce scratch buffer allocation caps and
  `ActorError::ResourceLimit`. Update `apps/vagent/main.cpp:56–76`.
- **`P27`**: In `vortex_stdlib.cpp:4634–4760`: add `make_cell`, `text_of`, `set_text`, `set_value`,
  `resolve_dim`, and `promote` returning `Result<T, ZigzagError>`. Make `boundStore_` mutable
  `Store*`. Fix `zzCloneToChain` 2-degree rank end append. Include `d.dims` in `promote`
  `extraRoots`.
- **`P28`**: In `apps/common/xanadu/vagent/stdlib/`: implement `behavior StagedCompiler` for
  single-turn intra-file compilation, and `agent CompilerCoordinator` for module-level distributed
  mesh compilation exchanging `MicroversionId` strings.
- **`P29`**: In `apps/common/xanadu/vagent/adapter.cpp` & `executor.cpp`: expose dynamic image
  spawning as an actor action via `VagentAdapter::setSpawnSender` and `MeshActorRuntime::spawn` with
  cached `ActorLineage{fold, store}`.
- **`P30`**: In `token.hpp/.cpp`, `lexer.cpp`, `stdlib/lexer.vagent`, `parser.cpp`, `ast.hpp`,
  `r1_ast.hpp/.cpp`, `checker.cpp`, `compiler.cpp`, and stdlib modules: implement `match ... with`
  expressions and `let Pattern := Expr` tuple destructuring lowering to deterministic decision
  trees.
