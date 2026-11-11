# Round 1 critique: Technical Writer (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 5 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§Global, lines 1–314) Complete Absence of Proposal Register (`P<k>`)

**Problem:** The review brief
(`design/projects/vagent-self-hosting-migration-review/brief-common.md:21-22`) and standing rules
(`AGENTS.md` / `GEMINI.md`) explicitly mandate: "Every change to existing code must be tracked in
the numbered Proposal Register (`P<k>`)." and "Argued, not asserted: Every design choice or stage
boundary must cite the code (`file:line`), price the cost in bytes/cycles/links/cells, and state
what alternative was refused." The target document proposes migrating over 20,000 lines of C++ code,
deleting core compilation files, introducing runtime shims, and adding standard library packages,
but contains zero proposals (`P<k>`), zero file:line citations for modifications, zero price
accounting, and zero refused alternatives.

**Evidence:**

- `grep -rn "Proposal Register" design/vagent-self-hosting-migration.md` returns 0 matches.
- `design/vagent-metacircular-compiler-implementation.md:863-882` defined proposals `P1` through
  `P14`.
- In `design/vagent-self-hosting-migration.md`, Phase 6 (lines 236–244) states:
  > "Replace C++ `parser.cpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`, `expand.cpp`,
  > `resolve.cpp` with thin shims invoking the Stage 1/2 VAgent compiler image. Delete retired C++
  > source files from the `Makefile`." Yet none of these deletions, Makefile changes, or shim
  > implementations are registered as proposals `P15`, `P16`, etc.

**Fix:** Add a dedicated Proposal Register section (e.g. `## 5. Proposal Register (P15–P24)`)
following the exact format of `design/vagent-metacircular-compiler-implementation.md:863-882`.
Number each concrete modification (`P15`, `P16`, ...), target exact files and line numbers
(`file:line`), price the cost in bytes/links/cycles/cells, and articulate what alternatives were
considered and refused.

### 2. (§4, lines 217–244) Incomplete Bootstrapping and Shim Specification

**Problem:** Phase 5 (lines 217–234) and Phase 6 (lines 236–244) specify that C++ compiler files
will be deleted from the `Makefile` and replaced with "thin shims invoking the Stage 1/2 VAgent
compiler image." However, the document provides zero technical specification for:

1. Compiler image serialization and persistence format (e.g., whether the compiled compiler image is
   an in-memory `ArenaManifold` snapshot, an EDL spool, an embedded C++ literal array via
   `xudu-dump`, or a bencode `store.tables`).
1. The concrete C++ API and calling convention of the shims in `apps/common/xanadu/vagent/` (e.g.,
   function signatures, arena parameters, diagnostic return types).
1. The clean-build bootstrapping cycle: how a fresh repository checkout builds without a
   chicken-and-egg failure if the C++ compiler sources are deleted from the `Makefile`. Is there a
   frozen boot image committed to `assets/` or `apps/common/xanadu/vagent/bootstrap/`?
1. Memory ownership, arena lifetime, and isolation between the compiler execution context and user
   agent manifolds.

**Evidence:**

- `design/vagent-self-hosting-migration.md:236-244`:
  > "Replace C++ `parser.cpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`, `expand.cpp`,
  > `resolve.cpp` with thin shims invoking the Stage 1/2 VAgent compiler image. Delete retired C++
  > source files from the `Makefile`. Retain only the hardened engine kernel (`VortexVM`,
  > `ArenaManifold`, `MeshActorRuntime`, `LineageStore`)." No function signatures, header locations,
  > build targets, or image loading protocols are defined.

**Fix:** Specify:

1. The exact boot image artifact path (e.g., `assets/vagent/compiler_stage0.image` or
   `apps/common/xanadu/vagent/bootstrap_image.bin`).
1. The exact C++ shim declarations in `apps/common/xanadu/vagent/host_compile.hpp` (e.g.,
   `std::expected<zigzag::CellRef, std::string> compileToR1(...)`).
1. The deterministic build sequence in `Makefile` guaranteeing that a clean `make -j$(nproc)`
   succeeds from git clone without requiring an existing runtime installation.
1. Arena lifetime and reset semantics across successive compiler invocations.

### 3. (§1 line 31, §2 lines 74–100) C++ Accounting Contradiction and Omissions

**Problem:** The line count accounting in Section 1 and Section 2 contains critical omissions and
contradictions:

1. Section 1 line 31 states: "`apps/common/xanadu/vagent/` still carries **29,070 lines of C++23
   code**". However, the Subsystem Feasibility table in Section 2 (lines 76–89) sums to only 24,824
   lines. It completely omits `apps/common/xanadu/vagent/frontend.cpp` (54 lines) and all 25 header
   files (`apps/common/xanadu/vagent/*.hpp`), which total 4,087 lines.
1. The table states that ~6,500 lines will be retained in C++ (Tier 3), but summing rows 10–12
   yields 7,129 lines (1,792 + 2,537 + ~2,800), not counting headers.
1. In row "Actor Transport & Runtime" (line 88), `genesis.cpp`, `lineage.cpp`, and `p16.cpp` are
   characterized as "Retained for raw OS, thread, and socket management", whereas they actually
   handle genesis cell graph seeding, Merkle lineage ops, and P16 persistence.
1. Line 97 claims: "Hardware / OS Boundaries: Thread synchronization, `epoll`/POSIX sockets, SDL3
   events, and libtorrent wire packet marshaling." No code in `apps/common/xanadu/vagent/` performs
   libtorrent wire packet marshaling (libtorrent transport is isolated in `identity/`, `store/`, and
   the swarm networking layers).

**Evidence:**

- Running `wc -l apps/common/xanadu/vagent/*.{cpp,hpp}` reveals: `24983 total (.cpp)` +
  `4087 total (.hpp)` = `29070 total`.
- Section 2 table lines 78–89 list only `.cpp` files with 24,824 lines, omitting `frontend.cpp` and
  all `.hpp` files.
- `apps/common/xanadu/vagent/genesis.cpp`, `lineage.cpp`, and `p16.cpp` contain no socket or thread
  APIs.

**Fix:**

1. Revise the Subsystem Feasibility table to account for all `.cpp` and `.hpp` files, including
   `frontend.cpp` (54 lines) and headers (4,087 lines).
1. Clarify how headers are affected by the migration (which headers are retired vs retained).
1. Accurately describe the responsibilities of `genesis.cpp` (genesis bootstrap seeding),
   `lineage.cpp` (Merkle checkpoint lineage), and `p16.cpp` (P16 durable serialization).
1. Remove the ungrounded citation of "libtorrent wire packet marshaling" from §2 line 97.

## MAJOR

### 1. (§Global, lines 1–314) Markdown Hygiene Failure: 43 mdl Violations

**Problem:** The document violates the repository's markdown style and linting gates
(`make format-check` and `make lint`). Running
`mdl -i .mdlrc design/vagent-self-hosting-migration.md` produces 43 blocking errors across 6 rules:

- `MD013` (Line length > 100): 20 lines (lines 14, 23, 24, 31, 145, 146, 147, 155, 160, 161, 189,
  205, 208, 210, 239, 242, 248, 303, 307, 313).
- `MD022` (Headers should be surrounded by blank lines): 8 headers (lines 91, 140, 164, 183, 204,
  217, 236, 278).
- `MD026` (Trailing punctuation in header): Line 278 (`### Track Breakdown:`).
- `MD029` (Ordered list item prefix): Lines 306, 309, 312.
- `MD031` (Fenced code blocks surrounded by blank lines): Lines 220, 231.
- `MD032` (Lists surrounded by blank lines): 9 lists (lines 16, 93, 141, 165, 184, 205, 218, 237,
  279). In addition,
  `mdformat --check --wrap 100 --extensions gfm --extensions dollarmath --extensions frontmatter`
  fails because lines and lists are unformatted.

**Evidence:** Output of `mdl -i .mdlrc design/vagent-self-hosting-migration.md` and
`mdformat --check`.

**Fix:** Format the document using
`mdformat --wrap 100 --extensions gfm --extensions dollarmath --extensions frontmatter` and ensure
`mdl -i .mdlrc` exits with code 0.

### 2. (§6, line 307) Broken API Reference: Non-Existent `zigzag::assertRankInvariants()`

**Problem:** Section 6 line 307 states:

> "All AST and opcode structures minted in `.vagent` are verified with
> `zigzag::assertRankInvariants()` to ensure strict 2-degree bounds (no fan-in collisions on
> `d.step`, `d.scan`, or `d.vars`)." The function `zigzag::assertRankInvariants()` does not exist
> anywhere in the codebase.

**Evidence:**

- `grep -rn "assertRankInvariants" apps/ include/ tests/` yields 0 matches.
- The actual rank invariant verification function in the ZigZag engine is
  `zigzag::crdt::CrdtManifold::verifyManifoldInvariants(const ArenaManifold &manifold)` defined in
  `apps/common/xanadu/zigzag/crdt_zz.hpp:101`.

**Fix:** Replace `zigzag::assertRankInvariants()` with
`zigzag::crdt::CrdtManifold::verifyManifoldInvariants(arena)` or register a proposed top-level
helper `assertRankInvariants` in `apps/common/xanadu/zigzag/arena_manifold.hpp` via the Proposal
Register.

### 3. (§5, line 296) Broken Test Reference: Non-Existent `tests/xuzz/vagent_self_hosting_test.cpp`

**Problem:** Section 5 line 296 specifies:

> "- **Track E (Quine & Integration Suite)**: Multi-stage bootstrapping test suite in
> `tests/xuzz/vagent_self_hosting_test.cpp`." This file does not exist in the repository.

**Evidence:**

- `ls tests/xuzz/vagent_self_hosting_test.cpp` returns `No such file or directory`.
- The actual quine verification test created during the metacircular compiler implementation is
  `tests/xuzz/vagent_metacircular_quine_test.cpp`.

**Fix:** Update the test path to `tests/xuzz/vagent_metacircular_quine_test.cpp` (or register
`tests/xuzz/vagent_self_hosting_test.cpp` as an explicitly newly introduced test file in the
Proposal Register).

### 4. (§0 line 6, §1 line 14) Companion Document and Baseline Omissions

**Problem:**

1. The metadata companion row in §0 (line 6) lists only `design/vagent-metacircular-compiler.md` and
   `design/vagent-agent-language.md`, omitting 4 companion documents designated in
   `design/projects/vagent-self-hosting-migration-review/brief-common.md:4-7`:
   `design/vagent-metacircular-compiler-implementation.md`, `design/vagent-metaprogramming.md`,
   `design/vagent-implementation.md`, and `design/store-slice-convergence.md`.
1. Section 1 line 14 states:
   > "With the completion of Stages 0–7 of the Metacircular Compiler
   > (`design/vagent-metacircular-compiler.md`), the VAgent runtime has achieved:" Stages 0–7 are
   > defined and implemented in `design/vagent-metacircular-compiler-implementation.md:244-828`, not
   > `design/vagent-metacircular-compiler.md` (which covers high-level architecture across 7
   > sections, not the 8 implementation stages 0–7).

**Evidence:**

- `design/projects/vagent-self-hosting-migration-review/brief-common.md:4-7`.
- `design/vagent-metacircular-compiler-implementation.md:244-828` defines Stages 0 through 7.

**Fix:** Add the missing companion documents to the metadata table, and update the citation in §1
line 14 to reference `design/vagent-metacircular-compiler-implementation.md`.

### 5. (§4 vs §5) Contradictory Phasing, Wave, and Track Taxonomies

**Problem:** Section 4 and Section 5 define competing organizational models for the migration:

- Section 4 organizes work into 6 sequential phases (`Phase 1` to `Phase 6`) with sub-tracks named
  `Track 1.1`, `Track 1.2`, `Track 2.1`, `Track 2.2`, `Track 3.1`, `Track 3.2`, `Track 4.1`,
  `Track 4.2`.
- Section 5 organizes work into 4 parallel "Waves" (`Wave 1` to `Wave 4`) with lettered tracks
  `Track A1`, `Track A2`, `Track B1`, `Track B2`, `Track C1`, `Track C2`, `Track D`, `Track E`.
- Most critically, Wave 4 in Section 5 merges Mind/Arbitration (Track D) and Quine (Track E), but
  completely omits Phase 6 (C++ Engine Decoupling and Deprecation). As a result, the C++ deprecation
  phase has no work package or track in the execution topology.

**Evidence:**

- Compare Mermaid diagram in §4 (lines 131–138) with Mermaid diagram in §5 (lines 251–276).
- Section 5 lines 278–297 define Tracks A1 through E, with no track for C++ engine deprecation.

**Fix:** Harmonize Section 4 and Section 5 into a single, unified taxonomy. Add
`Wave 5: C++ Deprecation & Engine Hardening` (Track F) to Section 5, and use consistent track
identifiers throughout both sections.

## MINOR

### 1. (§3 lines 120, 124, §5 line 280) Inconsistent Standard Library Module Naming Conventions

**Problem:**

- In §3 line 120, `stdlib/fragment.vagent` has no module name in parentheses, unlike all other rows.
- In §3 line 124, `stdlib/plasticity.vagent` is named `std:plasticity`, whereas mind and arbitration
  are prefixed with `std:agent:*` (`std:agent:mind`, `std:agent:arbitration`).
- In §5 line 280, Track A1 refers to `std:goal` scanning (`tab`, `many`, `match`), whereas line 49
  labels it `VIcon Scanning` and §1 line 22 calls it "Native VIcon Goal-Directed Scanning". The
  relationship between `std:goal` scanning primitives and `std:compiler:lexer` should be explicit.

**Evidence:** `design/vagent-self-hosting-migration.md:49, 120, 124, 280`.

**Fix:** Update line 120 to `stdlib/fragment.vagent (std:compiler:fragment)`, update line 124 to
`std:agent:plasticity`, and clarify the import of `std:goal` within `std:compiler:lexer`.

### 2. (§4 line 158, line 186) Unspecified AST Dimension Schema and Opcode Typo

**Problem:**

- Line 158 states: "Builds R1 AST cells connected via `+d.step` and annotated with `+d.name`,
  `+d.kind`, `+d.vars`." In the R1 AST specification (`apps/common/xanadu/vagent/r1_ast.hpp:24-40`),
  AST nodes are annotated with `+d.values` (type/value), `+d.name`, and `+d.kind`. `d.vars` is
  reserved for parameter descriptors and variable bindings on agents/modules.
- Line 186 specifies prologue emission `produced = false`, `ChoiceDepth`, `Choice(failArm)`. In
  `apps/common/xanadu/vagent/inlang_emitter.cpp`, `Choice` opcodes take an operand slot index, and
  `produced` is a flag cell linked to the fragment descriptor.

**Evidence:** `apps/common/xanadu/vagent/r1_ast.hpp:24-40`,
`apps/common/xanadu/vagent/inlang_emitter.cpp:45-80`.

**Fix:** Change `+d.vars` to `+d.values` on line 158, and clarify the slot indexing and flag cell
linkage on line 186.

### 3. (§4 lines 159–162, 178–181, 200–202) Vague Validation Gate Acceptance Criteria

**Problem:** Validation Gates 1, 2, and 3 use abstract assertions ("structurally isomorphic", "zero
diagnostics", "behavioral identity") without defining executable comparator functions, error code
sets, or test harness APIs.

**Evidence:** Lines 159–162, 178–181, 200–202.

**Fix:** Specify exact verification functions (e.g.
`isomorphicAstGraphs(arena1, root1, arena2, root2)`), expected diagnostic symbols (`TypeMismatch`,
`UnknownIdentifier`), and VM state comparison metrics.

### 4. (§4 line 207) Ungrounded "Vortex/Vlog Logic Unification" Claim

**Problem:** Line 207 asserts:

> "Belief pattern matching: queries agent belief manifold using Vortex/Vlog logic unification." The
> VAgent agent cognitive loop uses manifold graph traversals and pattern filters, not a full Prolog
> resolution engine. Citing "Vlog logic unification" is ungrounded and misleading.

**Evidence:** `apps/common/xanadu/vagent/meta.cpp` and `apps/common/xanadu/vagent/executor.cpp`.

**Fix:** Replace "Vortex/Vlog logic unification" with "in-manifold pattern matching and belief rank
traversal".

______________________________________________________________________

VERDICT: NOT CONVERGED
