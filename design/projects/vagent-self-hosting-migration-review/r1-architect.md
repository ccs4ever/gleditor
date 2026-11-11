# Round 1 critique: System Architect (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§1 & §4, lines 47, 140) Unbounded AST Manifold Memory Leak

**Problem:** The migration plan completely omits the resource lifecycle and memory retention model
for R1 AST manifolds and intermediate compilation artifacts. In VAgent, parsing source text into an
R1 AST (`std:compiler:parser`) mints thousands of ZigZag cells (token records, expression nodes,
statements, type annotations) connected along `+d.step`, `+d.vars`, `+d.kind`, and `+d.name`. In
`ArenaManifold`, cell slots are strictly appended to `slots_` and cannot be reclaimed individually
(`arena_manifold.hpp:553`); `compact()` only reclaims dead link entries, never cell slots
(`arena_manifold.hpp:750–762`). Cells are reclaimed exclusively on choice-point rollback via
`release(m)`. If `parser.vagent`, `expand.vagent`, `resolve.vagent`, and `checker.vagent` allocate
R1 AST nodes in the running agent's primary `ArenaManifold`, and compilation succeeds, the AST cells
are never rolled back. Every dynamic compilation (via `code"..."`, `eval`, runtime fragment
compilation, or the multi-stage self-hosting quine) permanently leaks $O(N_{\text{source cells}})$
into the agent's live manifold. In Phase 5 (§4, lines 217–235), compiling ~15,000 lines of VAgent
standard library across three bootstrap iterations will leak an estimated $10^5$ to $10^6$ AST
cells, bloating the live manifold, corrupting `rebuildManifold`, and breaking `LineageStore`
checkpoint/promote operations.

**Evidence:** `design/vagent-self-hosting-migration.md:47–68` shows the pipeline:

```
V_Lex -> V_Parse -> V_AST [R1 AST Manifold] -> V_Expand -> V_Resolve -> V_Check -> V_Emit -> register/validate C_Kernel
```

`design/vagent-metacircular-compiler.md:567–570` explicitly specifies:

> "D2 — Representation. R1: a cells-encoded AST exists only inside the compiler artifact — minted by
> its parser, consumed by its emitter, never part of output. The §11 refusal stands for the base
> representation; the AST is compile residue exactly like today's C++ AST."

Yet `design/vagent-self-hosting-migration.md` specifies zero mechanisms for disposing of this
"compile residue".

`apps/common/xanadu/zigzag/arena_manifold.hpp:731–736`:

```cpp
/// Truncate. Cells minted under the mark are *gone*, not garbage.
ArenaManifold *release(const Mark &m) noexcept;
```

`apps/common/xanadu/zigzag/arena_manifold.hpp:750–755`: `compact()` refuses to run while marks are
outstanding and only moves link runs; it does not prune committed `CellSlot` records in `slots_`.

**Fix:** Specify a strict compilation arena lifecycle:

1. Compilations must execute inside an isolated scratch `ArenaManifold` (or a sub-arena scoped by a
   dedicated compilation mark `Mark mCompile = arena.mark()`, or a transient `WorkerContext`
   manifold).
1. The lexer, parser, expander, resolver, and type checker operate exclusively within this transient
   scratch manifold.
1. The emitter either exports the resulting fragment opcode graph across the arena boundary into the
   target manifold using `join_batch` / `build_fragment`, or the compilation runs under a mark where
   the emitted opcode graph is extracted/promoted and all intermediate R1 AST cells are discarded
   via `arena.release(mCompile)`.

______________________________________________________________________

### 2. (§2, line 76) Unjustified Retention of ~7,129 LoC in Tier 3 Substrate

**Problem:** §2 categorizes ~7,129 lines of C++ code under `apps/common/xanadu/vagent/` as "Tier 3
(Permanent Substrate)" and asserts that this retention is "strictly justified" by:

1. 64-byte `CompactOpNode` cache line alignment and `mmap` page protections (`PROT_READ`).
1. `VortexVM::step()` inner bytecode execution cycle.
1. Hardware/OS boundaries (threads, sockets, SDL3, libtorrent).
1. Cryptographic primitives (Ed25519, BLAKE3).

This justification is an architectural layer misattribution and non-sequitur:

- `CompactOpNode` and `PROT_READ` live in `apps/common/xanadu/compact_op.hpp` and
  `segmented_ops_spool.cpp`.
- `VortexVM::step()` lives in `apps/common/xanadu/vortex/vortex_vm.cpp`.
- Sockets, threads, and libtorrent live in `apps/common/xanadu/identity/` and `mesh_actor.cpp`.
- Ed25519 and BLAKE3 live in `apps/common/xanadu/identity/`.

None of these live in `apps/common/xanadu/vagent/`. Using lower-layer engine and identity substrate
realities to justify retaining 7,129 lines of code inside `apps/common/xanadu/vagent/` violates
`GEMINI.md`:

> *"New C++ code must justify why it isn't being written in Vortex."*

Specifically:

- `inlang_parser.cpp` (22 LoC), `inlang_checker.cpp` (342 LoC), `inlang_emitter.cpp` (303 LoC), and
  `r1_ast.cpp` (282 LoC) are classified as permanent Tier 3 substrate (line 87), yet they duplicate
  `stdlib/parser.vagent`, `stdlib/checker.vagent`, `stdlib/emitter.vagent`, and `stdlib/ast.vagent`.
  Once the self-hosting compiler achieves quine fixed point (Phase 5), these are dead scaffolding.
- `lineage.cpp` (580 LoC) and `genesis.cpp` (751 LoC) are labeled "Retained for raw OS, thread, and
  socket management" (line 88). In reality, `lineage.cpp:1–100` and `genesis.cpp:1–60` contain zero
  OS/thread/socket operations; they only manipulate ZigZag `ArenaManifold` dimensions, `Store`
  tables, and microversions.
- `meta.cpp (validate)` and `executor.cpp (commit)` are labeled as retaining ~2,800 LoC in Tier 3.
  In reality, `validateCode` in `meta.cpp:490–623` is ~134 lines, and the turn commit in
  `executor.cpp:3346–3363` is ~18 lines. The remaining ~2,650 lines are un-migrated C++ logic for
  reflection, turn lifecycle, and outbox handling that have no hardware/OS justification.

**Evidence:** `design/vagent-self-hosting-migration.md:87–89`:

> "| **Bootstrap Bridge** | `bootstrap.cpp`, `host_compile.cpp`, `r1_ast.cpp`, `inlang_*.cpp` |
> 1,792 | **Tier 3 (Permanent Substrate)** | Retained as initial C++ bridge to boot image 0 |" "|
> **Actor Transport & Runtime** | `adapter.cpp`, `lineage.cpp`, `genesis.cpp`, `p16.cpp` | 2,537 |
> **Tier 3 (Permanent Substrate)** | Retained for raw OS, thread, and socket management |" "|
> **Atomic VM Boundary** | `meta.cpp` (validate), `executor.cpp` (commit) | ~2,800 | **Tier 3
> (Permanent Substrate)** | Retained for memory safety and bytecode execution loop |"

`apps/common/xanadu/vagent/inlang_checker.cpp:48–60`: `runInlangChecker` is a C++ AST walker,
completely redundant with `stdlib/checker.vagent`.

`apps/common/xanadu/vagent/lineage.cpp:47–71`: `fileNameSet` calls
`zigzag::DimensionRegistry::instance().getOrCreate()`.

`apps/common/xanadu/vagent/executor.cpp:3346–3363`: Turn commit consists of
`vm.cut(0); arena.discard(m0); meta.drainPendingRegisters();`.

**Fix:**

1. Reclassify `inlang_parser.cpp`, `inlang_checker.cpp`, `inlang_emitter.cpp`, and `r1_ast.cpp` as
   Tier 1 Bootstrap Scaffolding to be completely deprecated and deleted in Phase 6 once Stage 1/2
   self-hosting is reached.
1. Accurately categorize `lineage.cpp` and `genesis.cpp`: they are Manifold/Store lifecycle
   managers. Specify which parts can be driven by VAgent genesis scripts and which parts require C++
   `Store` replay.
1. Break down the ~2,800 LoC in `meta.cpp` and `executor.cpp`: only the atomic `validateCode` opcode
   verifier (~150 LoC) and the outer C++ actor turn pump harness `VagentExecutor::stepTurn` (~100
   LoC) belong in Tier 3. The outbox drainage, percept decoding, belief assertions, and intention
   manipulation must be moved into `stdlib/intentions.vagent` and `stdlib/percept.vagent`.
1. Revise §2.1 justification to reflect actual hardware/kernel constraints that apply to `vagent/`
   specifically (e.g., `MeshActorRuntime` callback interface in `adapter.cpp`, atomic `opcodeMap_`
   registration in `meta.cpp`).

______________________________________________________________________

### 3. (§4, line 236) Missing Proposal Register for C++ File Deprecations

**Problem:** `brief-common.md` Standing Rule 5 and `vortex-design-critique` rule 6 mandate:

> "Proposal Register: Every change to existing code must be tracked in the numbered Proposal
> Register (`P<k>`)."

Furthermore:

> "Any change to existing C++ engine, core, stdlib, or runtime files must be isolated in a numbered
> Proposal Register (`P1`, `P2`, ...) stating the exact files touched, why the change cannot live in
> the caller/compiler, the cost, and the fallback if not approved."

`design/vagent-self-hosting-migration.md` proposes Phase 6: "Slashing C++ Redundancies" (§4, lines
236–243), which deletes/replaces `parser.cpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`,
`expand.cpp`, `resolve.cpp`, introduces shims to invoke the in-language compiler image, modifies the
`Makefile`, and alters the C++ engine boundary. Yet the document contains ZERO numbered proposals
(`P<k>`). Without a Proposal Register, reviewers cannot evaluate the exact files touched, C++ API
diffs, performance costs, or fallback strategies.

**Evidence:** `design/projects/vagent-self-hosting-migration-review/brief-common.md:21–22`:

> "- **Proposal Register:** Every change to existing code must be tracked in the numbered Proposal
> Register (`P<k>`)."

`design/vagent-metacircular-compiler-implementation.md:863–900` establishes proposals `P1`–`P14`.
`design/vagent-self-hosting-migration.md` has no proposal register table anywhere in lines 1–314.

**Fix:** Add a dedicated, numbered Proposal Register section (numbering `P15`–`P20`, continuing from
`P14` in `design/vagent-metacircular-compiler-implementation.md:863`):

- `P15`: C++ Compiler Shim Architecture (`apps/common/xanadu/vagent/host_compile.cpp`,
  `frontend.cpp`) to invoke Stage 1/2 VAgent compiler image.
- `P16`: Phase 6 C++ file retirement and `Makefile` pruning
  (`apps/common/xanadu/vagent/{lexer,parser,checker,compiler,decls,expand,resolve}.cpp`).
- `P17`: Scratch Compilation Manifold Lifecycle API in `VortexVM`/`VortexCore`
  (`apps/common/xanadu/vortex/vortex_core.hpp`).
- `P18`: Opcode graph reclamation / fragment tombstone sweep on head replacement
  (`apps/common/xanadu/vagent/meta.cpp`).

______________________________________________________________________

## MAJOR

### 1. (§4, line 183) Recompilation Opcode Leakage and Missing Fragment Reaping

**Problem:** When VAgent programs or fragments are dynamically recompiled, modified, or specialized
(Tracks C1, C2, Track D), new fragment objects (head cell, descriptor, `+d.entry` opcode graph,
recipe, refs) are emitted into the arena. When a fragment or plan is replaced (e.g., an agent learns
an updated plan or recompiles a function), the old fragment head may lose its link or be tombstoned
(`fault:<label>`), but what happens to the underlying opcode graph cells and descriptor records? In
`VortexVM`, `opcodeMap_` maps `CellRef -> OpcodeKind`. In `meta.cpp`, `d.kind-registered` links the
agent to registered heads. If a head is superseded, the old opcode cells remain linked in the
manifold and registered in `opcodeMap_`. `design/vagent-self-hosting-migration.md` does not specify:

1. How old fragment graphs are unlinked and deregistered from `VortexVM::opcodeMap_`.
1. How dead fragment opcodes are pruned during `ArenaManifold::compact()` or lineage checkpointing.
1. Whether fragment replacement causes unbounded growth of `opcodeMap_` in long-running agents.

**Evidence:** `design/vagent-metacircular-compiler-implementation.md:877` (P4):

> "`d.kind-registered` + `d.code` commit; `fault:<label>` tombstones."

`design/vagent-metacircular-compiler.md:604–605`:

> "`-d.lineage` enumerates *all-time* twins — dead copies keep edges until reclamation, so aliveness
> is the reader's filter"

In `design/vagent-self-hosting-migration.md:183–203`, Tracks 3.1 & 3.2 describe emission and
fragment assembly, but offer zero semantics for fragment lifecycle, de-registration, or dead-opcode
garbage collection.

**Fix:** Specify the fragment lifecycle and reaping protocol:

- When a fragment head is superseded on `d.code` or tombstoned on `d.lineage`, its entry in
  `VortexVM::opcodeMap_` must be pruned or marked dead.
- Define a GC sweep (`reap_fragment(head)`) in `stdlib/plasticity.vagent` or `meta.cpp` that unlinks
  the opcode graph from `d.entry`/`d.step` and removes entries from `opcodeMap_`, allowing
  `ArenaManifold::compact()` to recover link entries.

______________________________________________________________________

### 2. (§4, line 140) Compiler Limits Lack Dynamic Configuration

**Problem:** The System Architect skill (`SKILL.md:92–154`) and standing rules require that
configurable parameters must never be hardcoded literals, but loaded dynamically from
layer-appropriate configuration:

> "In `xuzz`, all user-configurable parameters ... must be read **live dynamically from an
> appropriate system xanadoc** (e.g., `system://settings`, `system://layout`, `system://ui`)."
> "Fallback defaults for configuration structs must be cleanly encapsulated with documented
> rationale comments explaining the default value."

In `design/vagent-self-hosting-migration.md`, the compiler pipeline introduces multiple operational
thresholds:

- Maximum token length and scanner backtrack depth in `stdlib/lexer.vagent`.
- Maximum AST recursion depth (to prevent stack overflow on deeply nested expressions) in
  `stdlib/parser.vagent`.
- Type inference fuel / recursion limits in `stdlib/checker.vagent`.
- Maximum emitted opcode graph size / fuel bounds in `stdlib/emitter.vagent`.
- Intention queue capacity and belief journal compaction interval in `stdlib/arbitration.vagent`.

None of these bounds are defined or connected to dynamic configuration! Currently,
`executor.cpp:114–116, 3358` uses `settingReader("vagent.compact_every", 0)` and
`settingReader("vagent.journal_size", ...)`. The migration plan fails to define how the in-language
compiler and runtime read these configuration keys (e.g. `vagent.compiler.max_ast_depth`,
`vagent.compiler.max_fuel`, `vagent.compiler.scratch_arena_size`).

**Evidence:** `apps/common/xanadu/vagent/executor.cpp:114–116`:

```cpp
explicit VagentExecutor(
    std::function<std::optional<std::int64_t>(std::string_view)>
        settingReader = {});
```

`apps/common/xanadu/vagent/executor.cpp:3358`:

```cpp
const auto compactEvery = setting("vagent.compact_every", 0);
```

`design/vagent-self-hosting-migration.md` does not specify any configuration reader or settings
binding in `prelude/meta.vagent` or `stdlib/`.

**Fix:** Specify the configuration protocol for in-language compiler and agent modules:

- Expose a `setting(key: Str) -> Opt<Int>` primitive in `prelude/meta.vagent` bridging to the host
  setting reader.
- Define explicit configuration keys and documented fallbacks for compiler bounds
  (`vagent.compiler.max_depth`, `vagent.compiler.max_tokens`, `vagent.compiler.max_opcodes`,
  `vagent.arbitration.max_intentions`).

______________________________________________________________________

### 3. (§4, line 143) Duplicate Scanning Violates DRY Stdlib Reuse

**Problem:** Track 1.1 states:

> "Implement full tokenization using VIcon string scanning: Keywords ... Operators ... Literals ...
> Comments ... Returns a cell list of Token records (`Token{kind, text, line, col}`)."

Rule in `GEMINI.md`:

> *"New Vortex standard library code in Vortex must leverage existing Vortex standard library
> functions unless absolutely necessary."*

`apps/common/xanadu/vicon/` already implements goal-directed scanning primitives and scanning
environments. Furthermore, `VortexCore` has Band A string opcodes (`StrLen`, `ByteAt`, `StrSlice`,
`IntToStr`, `StrToInt` per `P12`, `vortex_vm.cpp:109–118`). The migration plan does not clarify
whether `stdlib/lexer.vagent` imports and consumes `std:scan` / `std:goal` from the shared Vortex
standard library or reimplements scanner backtracking and position tracking (`&pos`, `&subject`)
from scratch. Re-implementing scanning primitives inside `vagent` violates DRY and creates two
parallel string scanning engines in the engine.

**Evidence:** `design/vagent-self-hosting-migration.md:143–146`:

> "Track 1.1: `stdlib/lexer.vagent`: Implement full tokenization using VIcon string scanning ...
> Returns a cell list of Token records (`Token{kind, text, line, col}`)."

`design/vicon-goal-directed-language.md` defines the standard VIcon scanning semantics.

**Fix:** Explicitly specify that `stdlib/lexer.vagent` imports scanning primitives from the unified
`std:scan` / `std:goal` library, leveraging existing `ScanPush`, `ScanPop`, `ScanTab`, `ScanMove` VM
opcodes rather than inventing vagent-private scanning mechanisms.

______________________________________________________________________

## MINOR

### 1. (§4, line 220) Ambiguous Fixed-Point Quine Verification Metric

**Problem:** §4 states:

> "Image 2 and Image 3 must be **bit-for-bit identical** (modulo non-deterministic timestamp salts,
> which are canonicalized). This proves the metacircular compiler is a fixed point."

In ZigZag manifolds and Merkleized stores, "bit-for-bit identical" is ambiguous. Are we comparing
serialized bencoded `ops.nodes` files? Segmented ops spools? Or the in-memory cell graph topology?
In `SegmentedOpsSpool`, op indices and Merkle roots depend on segment sealing order and local scroll
allocations (`Store::tables`). A true fixed-point check in hypermedia systems must verify:

1. Identical topological isomorphism of the emitted opcode graphs (matching `OpcodeKind`, wing slot
   arities, and relative branch offsets).
1. Byte-exact equality of the canonical bencode serialization of the resulting sealed store image
   (`store.tables` and `ops.nodes`).

**Evidence:** `design/vagent-self-hosting-migration.md:232–234`:

> "Image 2 and Image 3 must be bit-for-bit identical (modulo non-deterministic timestamp salts,
> which are canonicalized)."

`apps/common/xanadu/compact_op.hpp:36–40`: `CompactOpNode` struct layout and alignment.

**Fix:** Clarify the exact quine comparison protocol: Image 2 and Image 3 must produce identical
canonical bencoded `ops.nodes` Merkle roots via `tools/xudu-dump` or `Spool::merkleRoot()`, ensuring
cryptographic, deterministic fixed-point convergence.

______________________________________________________________________

### 2. (§4, line 158) Unbounded AST Marshalling Memory in Dual-Run Tests

**Problem:** Validation Gate 1 compares the AST emitted by C++ `parser.cpp` with the AST emitted by
`parser.vagent`. C++ `parser.cpp` uses C++ heap objects (`std::unique_ptr<Expr>`, `ast.hpp:10–248`),
which `r1_ast.cpp` converts to R1 cells via `marshalAstToR1`. The plan does not quantify or bound
the allocation overhead of dual-running both parsers during Wave 1 and Wave 2, which could cause
test suite slowdowns or high memory pressure in CI.

**Evidence:** `apps/common/xanadu/vagent/r1_ast.cpp:20–50`: deep traversal and recursive cell
allocation for every AST node.

**Fix:** Add a memory and latency probe budget for Validation Gate 1 (e.g., peak memory during
parsing must not exceed $2\times$ baseline, and parse latency for stdlib must remain
$< 50\text{ms}$).

______________________________________________________________________

VERDICT: NOT CONVERGED
