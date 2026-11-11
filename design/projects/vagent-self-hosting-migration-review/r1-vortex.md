# Round 1 critique: Vortex & Spool-Mesh Expert (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 4 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§1, line 49; §4, line 150) Scope Leak on #Return and #RunResume

**Problem:** In-language parsing (`stdlib/parser.vagent`) relies on recursive descent with deeply
nested activation records. In the Vortex execution model, function and method invocations are
emitted as either `#Call` (`apps/common/xanadu/vagent/compiler.cpp:2185–2214`) or `#RunCall`
(`apps/common/xanadu/vagent/compiler.cpp:2259–2282`). Both calling mechanisms bind parameter values
directly into the executing cursor's `+d.vars` scope rank
(`apps/common/xanadu/vortex/vortex_vm.cpp:1074–1080` and `vortex_vm.cpp:1728–1736`).

However, on function exit, neither `#Return` (`vortex_vm.cpp:1698–1714`) nor `#RunResume`
(`vortex_vm.cpp:1087–1142`) restores or pops the cursor's `+d.vars` rank. `#Return` merely pops the
return opcode off `-d.stack` (`vortex_vm.cpp:1700–1712`), leaving all callee argument cells
permanently linked to `cursor +d.vars`. Because variable lookup in `#Resolve`
(`vortex_vm.cpp:1743–1765`) performs a forward traversal along `+d.vars` via `rankAfter()`, dead
variables from returned callee invocations shadow caller variables or subsequent invocations with
the same identifier. In a recursive descent parser processing thousands of nested expressions,
`+d.vars` leaks every parameter and local allocation unbounded, causing severe name shadowing
corruption, quadratic link traversal overhead, and rapid memory exhaustion. Compiling VAgent in
VAgent is mechanically impossible until the VM or language ABI enforces strict lexical scope
unwinding on return.

**Evidence:**

In `apps/common/xanadu/vortex/vortex_vm.cpp:1074–1080`, `#RunCall` pushes parameter variables onto
`cursor +d.vars`:

```cpp
if (const auto varsHead =
        core_.arena().linked(*cursor, dims.vars, zigzag::Posward))
  zigzag::expectWritten(core_.arena().link(varCell, dims.vars,
                                           zigzag::Posward, *varsHead));
zigzag::expectWritten(
    core_.arena().link(*cursor, dims.vars, zigzag::Posward, varCell));
```

In `apps/common/xanadu/vortex/vortex_vm.cpp:1698–1714`, `#Return` pops only the return address:

```cpp
case OpcodeKind::Return: {
  if (cursor) {
    const auto frame =
        core_.arena().linked(*cursor, core_.dims().stack, false);
    if (frame) {
      auto returnOpVal = core_.arena().asInt64(*frame);
      CellRef returnOp =
          returnOpVal ? static_cast<CellRef>(*returnOpVal) : noCell;
      const auto nextStack =
          core_.arena().linked(*frame, core_.dims().stack, false);
      zigzag::expectWritten(
          core_.arena().link(*cursor, core_.dims().stack, false, nextStack));
      setCursorOpcode(*cursor, returnOp);
      jumped = true;
    }
  }
  break;
}
```

Neither `vortex_vm.cpp:1700–1712` nor `vortex_vm.cpp:1087–1142` (`RunResume`) touches
`cursor +d.vars`.

In `design/vagent-self-hosting-migration.md:150–158`, Track 1.2 mandates a recursive descent parser
in `.vagent`, which will execute tens of thousands of nested calls over the token stream without
scope isolation.

**Fix:** Specify in §2 and §4 a Proposal (`P18`) extending `OpcodeKind::Call` and
`OpcodeKind::RunCall` stack frames on `-d.stack`:

1. The stack frame cell must store both `returnOp` and the previous `varsHead` (`+d.vars` entry
   cell) prior to argument binding: `frame.setLink(-d.stack, nextStack)` and
   `frame.setLink(+d.vars, oldVarsHead)`.
1. On `OpcodeKind::Return` and `OpcodeKind::RunResume`, the VM must restore `cursor +d.vars` to
   `oldVarsHead`, severing the callee's activation scope rank and preventing dead parameter
   shadowing.

______________________________________________________________________

### 2. (§1, line 22; §4, line 143) Uncut Scanning Choice Points Threaten AST Rollback

**Problem:** In `design/vagent-self-hosting-migration.md:22–24` and lines 143–158,
`stdlib/lexer.vagent` and `stdlib/parser.vagent` are specified to use VIcon goal-directed scanning
(`s ? e`, `tab`, `move`, `match`, `many`, `any`) alongside backtracking alternation (`|`).

In the current engine lowering (`apps/common/xanadu/vagent/compiler.cpp:1633–1653`), a scanning
expression `left ? right` is emitted as `OpcodeKind::ScanPush`, followed by `OpcodeKind::Choice`
with a fail arm pointing to `ScanPop` and continuation pointing to `popSuccess` (`ScanPop`). When
`Choice` executes (`apps/common/xanadu/vortex/vortex_vm.cpp:320–335`), it pushes a `ChoicePoint`
onto `choiceStack_` capturing `core_.arena().mark()`. Crucially, **`popSuccess` does NOT cut the
`ChoicePoint`**. If `left ? right` succeeds, the choice point remains live on `choiceStack_`.

If the parser or compiler subsequently mints AST cells (`stdlib/parser.vagent`), links manifold
structures (`stdlib/decls.vagent`), or generates opcode sequences (`stdlib/emitter.vagent`), those
cells are allocated at cell indices higher than the active choice mark. When any subsequent
grammatical branch fails and invokes `VortexVM::backtrack()` (`vortex_vm.cpp:337–362`), the VM calls
`core_.arena().release(cp.mark)`. `ArenaManifold::release()`
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1612–1700`) calls `slots_.resize(m.cellCount)` and
`links_.resize(m.linkSize)`, **permanently truncating and destroying legitimately emitted AST cells,
tokens, and bytecode graphs** that were allocated after the scanning expression succeeded!
Furthermore, any registration attempted while marks are live is trapped in `+d.pending-reg`
(`apps/common/xanadu/vagent/meta.cpp:3249–3253`), which is itself unwound by `release()`.

**Evidence:**

In `apps/common/xanadu/vagent/compiler.cpp:1637–1649`:

```cpp
const auto popSuccess = mint(OpcodeKind::ScanPop);
bindIn(popSuccess, cursorTemp);
spin(popSuccess, jump(continuation));

const auto popFail = mint(OpcodeKind::ScanPop);
bindIn(popFail, cursorTemp);
spin(popFail, jump(cells_->fail));

const auto choice = mint(OpcodeKind::Choice);
bindIn(choice, reference(popFail));
```

`popSuccess` jumps directly to `continuation` without emitting `OpcodeKind::Cut` or passing a
`ChoiceDepth` barrier.

In `apps/common/xanadu/zigzag/arena_manifold.cpp:1671–1673`:

```cpp
slots_.resize(m.cellCount);
links_.resize(m.linkSize);
content_.resize(m.contentSize);
```

Rollback unconditionally truncates all newly allocated cells.

**Fix:** In §4 (Phases 1–3), define an explicit transaction boundary and scanning choice-point cut
invariant:

1. Amend `compiler.cpp` scanning lowering: every successful exit from a scanning expression `s ? e`
   must execute `OpcodeKind::Cut` to discard the scanner's choice point before any persistent value
   or cell is returned.
1. In `stdlib/parser.vagent` and `stdlib/emitter.vagent`, specify that speculative grammar branches
   (`ruleA | ruleB`) must isolate AST cell creation: either parse speculatively into an ephemeral
   sub-arena/branch, or assert that grammar alternatives do not mix scanning backtracking with
   side-effecting code registration.

______________________________________________________________________

### 3. (§4, line 217) Unbudgeted Execution Limits (maxCycles, maxFuel, maxCells)

**Problem:** Compiling ~15,000–20,000 lines of VAgent source in VAgent through recursive descent,
macro expansion, type checking, and opcode emission will execute tens of millions of VM instructions
and allocate hundreds of thousands of manifold cells. However, `VortexVM::run()`
(`apps/common/xanadu/vortex/vortex_vm.hpp:274` and `vortex_vm.cpp:274`) has a hardcoded default
cycle ceiling:

```cpp
ExecutionResult run(CellRef cursor, std::size_t maxCycles = 100000, std::stop_token stopToken = {});
```

100,000 cycles is insufficient even to tokenize a single large standard library file (e.g.
`checker.cpp` is 2,950 LoC; tokenizing it via VIcon scanning takes an estimated 4–8 million VM
cycles). Furthermore, when running under `MeshActorRuntime`
(`apps/common/xanadu/vortex/mesh_actor.hpp:60–74`), actor execution is strictly policed by
`ActorGrant::maxInstructions`, `ActorGrant::maxCells`, and `ActorGrant::maxLiveCells`
(`mesh_actor.cpp:164–170`). Exceeding these limits aborts execution with `ActorError::NotAuthorized`
or exhaustion.

The migration plan in `design/vagent-self-hosting-migration.md` contains zero price accounting for
byte sizes, cell counts, instruction counts, or fuel consumption across the compilation stages. It
fails to specify:

1. What cycle budget (`maxCycles`) is required to self-compile the full compiler suite.
1. What arena cell capacity (`maxCells`) is necessary to host the compiler's R1 AST and opcode graph
   simultaneously without triggering arena compaction failure or out-of-memory panics.
1. Whether self-compilation executes as a monolithic turn (which would violate turn latency limits
   in `MeshActorRuntime`) or segmented across multiple actor turns.

**Evidence:**

- `design/vagent-self-hosting-migration.md:217–235` (Phase 5) shows Stage 1, Stage 2, and Stage 3
  compiling `stdlib/*.vagent` with no cycle, cell, or turn budget specified.
- `apps/common/xanadu/vortex/vortex_vm.hpp:274`: `maxCycles = 100000`.
- `apps/common/xanadu/vortex/mesh_actor.hpp:63–68`: `maxInstructions`, `maxCells`, `maxLiveCells`.

**Fix:** Add a dedicated "Resource Accounting & Execution Budgeting" subsection to §4:

1. Price the cycle and cell cost per compilation stage: establish realistic budgets (e.g., $5 \times
   10^7$ VM cycles and 500,000 arena cells per self-hosting stage).
1. Specify that the Stage 0/1 bootstrap driver explicitly passes an unconstrained or elevated cycle
   limit (`maxCycles = 100'000'000`) when invoking the compiler image.
1. Define the multi-turn batching protocol if compilation is executed within `MeshActorRuntime`, or
   specify that self-hosting quine verification runs in a dedicated batch execution mode.

______________________________________________________________________

### 4. (§1, line 60; §2, line 93) Conflation of CompactOpNode and ArenaManifold

**Problem:** §2 (line 93) justifies retaining the permanent C++ substrate by citing:

> *"1. Memory & Cache Layout: 64-byte `CompactOpNode` cache line alignment, virtual memory `mmap`
> page protections (`PROT_READ` sealed segments)."* and §1 (line 60) depicts the C++ kernel as:
> `C_Kernel["ArenaManifold / CompactOpNode / VortexVM Execution Loop"]`

This conflates two completely separate architectural layers:

1. `CompactOpNode` (`apps/common/xanadu/compact_op.hpp`) is the 64-byte OSMIC operation record
   appended to an on-disk `SegmentedOpsSpool` (`Store`).
1. Bytecode opcode graphs emitted by `compiler.cpp` / `fragment.cpp` and interpreted by `VortexVM`
   are **pure `ArenaManifold` cell graphs** (`zzcell` structures linked along `spin`, `grab`,
   `step`, and `values`). `VortexVM::step()` never reads, writes, or indexes `CompactOpNode`
   structures during execution.

Furthermore, §4 Track 3.2 (lines 194–199) claims that `stdlib/decls.vagent` will lower agent
declarations, event handlers, and plan rules into the manifold. It fails to clarify whether
`decls.vagent` lowers into ephemeral `ArenaManifold` cells or emits persistent OSMIC Structure
operations (`OpKind::Structure`). If it emits persistent operations, it must interface with
`Store::sliceGenesis` or `batch_orchestrator.cpp`. If it lowers into `ArenaManifold`,
`CompactOpNode` alignment is completely irrelevant to the compiler backend.

**Evidence:**

- `apps/common/xanadu/vortex/vortex_vm.hpp:185–188`: `mintOpcode`, `assembleSequence`, and
  `setOpcodeKind` operate solely on `zigzag::CellRef` in `ArenaManifold`.
- `apps/common/xanadu/vortex/vortex_vm.cpp:320–330`: `step()` fetches opcodes via
  `core_.arena().asInt64()` and steps along `core_.dims().spin`.
- `design/vagent-self-hosting-migration.md:60, 93` directly cites `CompactOpNode` as part of the
  execution loop and backend compiler boundary.

**Fix:** Clarify the distinction between ephemeral execution manifolds and durable store
persistence:

1. In §1 (Figure 1) and §2 (lines 91–95), clarify that the permanent C++ substrate manages
   `ArenaManifold` cell allocation and the `VortexVM::step()` interpreter cycle. Remove the
   reference to `CompactOpNode` from the execution loop.
1. In §4 (Phase 3, Track 3.2), explicitly state that `stdlib/emitter.vagent` and
   `stdlib/decls.vagent` emit `ArenaManifold` cell graphs. If agent persistence into a `Store` is
   required, define the bridge mechanism (e.g., via `Store::sliceGenesis` or a bencode serialization
   bridge) instead of conflating it with the VM's in-memory opcode graph.

______________________________________________________________________

## MAJOR

### 1. (§1, line 18; §5, line 290) Insufficiency of boot_writer and boot_linker

**Problem:** §1 lines 18–20 highlights:

> *"2. The Genesis Bootstrap: Pure in-manifold writer (`Value`) and linker (`Link`) primitives
> minted at genesis and reachable via `std:meta (boot_writer, boot_linker)`."* and §5 line 290
> specifies: *"Track C1 (`stdlib/emitter.vagent`): Emits wrapped opcode sequences via genesis
> `boot_writer` and `boot_linker`."*

`boot_writer` (`apps/common/xanadu/vagent/bootstrap.cpp:199–320`) wraps `OpcodeKind::Value`, which
writes or splices content into an **already existing cell**. `boot_linker`
(`apps/common/xanadu/vagent/bootstrap.cpp:321–440`) wraps `OpcodeKind::Link`, which establishes a
dimension edge between **two already existing cells**. **Neither primitive can allocate a fresh
cell!** An AST parser or opcode emitter cannot construct AST trees, token records, or bytecode
graphs using only `boot_writer` and `boot_linker` without cell allocation. They require
`std:meta (make_cell)` (`apps/common/xanadu/vagent/meta.cpp:3013–3017`) or `core.allocateLink()`.
Describing `boot_writer` and `boot_linker` as the sole foundation for in-language emission is
incomplete and misleading.

**Evidence:**

- `apps/common/xanadu/vagent/bootstrap.cpp:235–240`: `mintBootstrapWriter` targets an existing cell
  passed as input 0.
- `apps/common/xanadu/vagent/bootstrap.cpp:359–365`: `mintBootstrapLinker` targets existing cells
  passed as inputs 0 and 3.
- `apps/common/xanadu/vagent/meta.cpp:3013–3017`: `make_cell` allocates a fresh `CellRef` via
  `arena().makeCell()`.

**Fix:** In §1 (line 20) and §5 (line 290), explicitly include `make_cell` alongside `boot_writer`
and `boot_linker`:

> *"Track C1 (`stdlib/emitter.vagent`): Allocates opcode and operand cells via `make_cell`, writes
> opcodes and values via `boot_writer`, and links graph topology (`spin`, `grab`, `step`, `values`)
> via `boot_linker`."*

______________________________________________________________________

### 2. (§4, line 217) Unspecified Host Driver Protocol and Compiler ABI

**Problem:** Phase 5 outlines a 3-stage bootstrap: `Stage 0 (C++) -> Image 1 -> Image 2 -> Image 3`,
asserting bit-for-bit identity between Image 2 and Image 3. Phase 6 states:

> *"Replace C++ parser.cpp, checker.cpp, compiler.cpp, decls.cpp, expand.cpp, resolve.cpp with thin
> shims invoking the Stage 1/2 VAgent compiler image."*

However, the document completely omits the specification of:

1. **The Compiler Image ABI**: What is the entry point function/agent of the compiled compiler
   image? What are its parameter types (`src: Str, moduleName: Str`) and return type (`CellRef`
   image head)?
1. **I/O Injection**: Vortex has no filesystem I/O opcodes. How does the host feed `.vagent` file
   contents into the VM?
1. **Execution Shim**: How do the C++ "thin shims" invoke the image? Do they use
   `VortexVM::spawnCursor()`, bind parameters via `VortexCore::bindInput()`, call `VortexVM::run()`,
   and inspect the output descriptor?
1. **Test Harness Deception**: The current test
   `tests/xuzz/vagent_metacircular_quine_test.cpp:131–197` does NOT execute an in-language compiler.
   It calls C++ functions `parseSourceToR1`, `runInlangChecker`, and `runInlangEmitter` sequentially
   on a trivial arithmetic expression `(x * 2) + (y * 3)`. The plan lacks a roadmap to replace these
   C++ helper calls with actual VM execution of `stdlib/parser.vagent` and `stdlib/emitter.vagent`.

**Evidence:**

- `design/vagent-self-hosting-migration.md:217–243` contains no ABI signature, data marshalling
  format, or shim specification.
- `tests/xuzz/vagent_metacircular_quine_test.cpp:131, 134, 138`: Step 1 calls C++ `parseSourceToR1`,
  C++ `runInlangChecker`, C++ `runInlangEmitter`.

**Fix:** Add an "In-Language Compiler Driver & Invocation Protocol" specification to §4:

1. Define the top-level compiler signature in `stdlib/compiler.vagent`:
   `def compile_module(source: Str, module_path: Str): Opt<Cell>`
1. Define the C++ shim in `apps/common/xanadu/vagent/inlang_driver.cpp`:
   - Instantiates `WorkerContext`.
   - Injects source string into an arena cell.
   - Executes `vm.run(cursor, maxCycles)`.
   - Extracts the resulting `imageRecord` cell and passes it to `vm.registerProgramImage()`.
1. Update `vagent_metacircular_quine_test.cpp` to execute the compiler image via `vm.run()` rather
   than C++ AST harnesses.

______________________________________________________________________

### 3. (§2, line 88; §4, line 183) Omission of Handle Relocation (d.mesh.relocation)

**Problem:** In `MeshActorRuntime`, actors and compiled fragments must be portable across worker
threads, processes, and remote network nodes (`apps/common/xanadu/vortex/mesh_actor.hpp:160–163`).
When code is compiled into fragment objects (`apps/common/xanadu/vagent/fragment.cpp:90–111`), all
internal operand cell references (jump targets, branch arms, choice fail targets) must be
inventoried on `+d.refs` and tagged along `+d.mesh.relocation` with `"cell"`
(`apps/common/xanadu/vagent/meta.cpp:1729–1740` and `fragment.cpp:98–100`). When a fragment is
copied, reified, or migrated in an `ActorCheckpointView` (`mesh_actor.hpp:154–158`), the runtime
reads `+d.mesh.relocation` to rewrite internal cell references into the destination arena's address
space.

§3 line 120 and §4 lines 183–193 specify that `stdlib/fragment.vagent` will assemble fragment
objects and `+d.refs` records, but omit the relocation tagging protocol (`d.mesh.relocation`,
`d.recipe`). Any fragment emitted by `emitter.vagent` lacking `+d.mesh.relocation` tagging will
contain raw, untranslatable `CellRef` integer addresses, causing dangling pointers, crashes on actor
checkpoint/resume, and breaking actor mobility in Spool-Mesh.

**Evidence:**

In `apps/common/xanadu/vagent/fragment.cpp:98–100`:

```cpp
zigzag::expectWritten(arena.link(marked, fdims.meshRelocation,
                                 DimVector::POS, arena.makeCell("cell")));
```

In `apps/common/xanadu/vagent/meta.cpp:1737–1738`:

```cpp
zigzag::expectWritten(arena.link(slot, binds_.dims.meshRelocation, Posward,
                                 arena.makeCell("cell")));
```

In `apps/common/xanadu/vortex/mesh_actor.hpp:144–146`: Portability requires clean state
serialization.

**Fix:** In §3 and §4 (Track 3.1), explicitly mandate that `stdlib/fragment.vagent` emit the
complete relocation apparatus:

1. Emit `d.mesh.relocation` edge with label `"cell"` for every internal reference box cell.
1. Construct the self-describing `d.recipe` sequence recording member positions and taxonomy
   (matching `apps/common/xanadu/vagent/fragment.cpp:103–110`).
1. Verify that all fragments compiled by `stdlib/emitter.vagent` pass `MetaRuntime::headShape()`
   relocation and signature preflight before registration.

______________________________________________________________________

## MINOR

### 1. (§2, line 76; §4, line 236) Missing Proposal Register Entries (`P<k>`)

**Problem:** Standing rules in `AGENTS.md`, `GEMINI.md`, and `brief-common.md:21–22` mandate:

> *"Every change to existing code must be tracked in the numbered Proposal Register (`P<k>`)."* The
> target document proposes migrating over 20,000 lines of C++ code, modifying `meta.cpp`,
> deprecating six compilation files, adding standard library modules, and changing `Makefile`, but
> introduces zero proposals (`P<k>`). Following `P1` through `P14` in
> `design/vagent-metacircular-compiler-implementation.md:863–882`, subsequent proposals (`P18`+)
> must track these changes.

**Evidence:**

- Zero occurrences of `P<k>` in `design/vagent-self-hosting-migration.md`.

**Fix:** Add a Proposal Register table to §2 tracking proposals `P18` (Stack frame variable
restoration), `P19` (Scanning cut invariant), `P20` (In-language compiler driver shim), and `P21`
(C++ compiler file deprecation).

______________________________________________________________________

### 2. (§1, line 54) Conflation of Compile-Time Lowering and Runtime Loop

**Problem:** In Figure 1 ("Target Architecture"), the diagram illustrates:

```
V_Check --> V_Emit["std:compiler:emitter (Vortex Opcode Graphs)"]
V_Emit --> V_Decls["std:compiler:decls (Manifold Lowering)"]
V_Decls --> V_Mind["std:agent:mind (Goal/Plan Arbitration)"]
```

`V_Decls` is compile-time declaration lowering into the manifold. `V_Mind` is the runtime cognitive
loop (BDI plan arbitration and intention queuing) that executes when the agent receives percepts or
turns. Showing `V_Decls` flowing sequentially into `V_Mind` as part of the compiler pipeline
conflates ahead-of-time compilation with runtime agent execution.

**Evidence:**

- `design/vagent-self-hosting-migration.md:54–57`.

**Fix:** Redraw Figure 1 to decouple the compilation pipeline (`V_Lex` $\to$ `V_Parse` $\to$ `V_AST`
$\to$ `V_Expand` $\to$ `V_Resolve` $\to$ `V_Check` $\to$ `V_Emit` $\to$ `V_Decls` $\to$ Manifold
Image) from the runtime agent execution loop (`Turn Percept` $\to$ `V_Mind` $\to$ `V_Arbitration`
$\to$ `V_Intentions` $\to$ `VortexVM`).

______________________________________________________________________

VERDICT: NOT CONVERGED
