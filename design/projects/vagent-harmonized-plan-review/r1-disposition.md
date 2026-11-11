# Round 1 Disposition Report: VAgent Harmonization & Missing Features Implementation Plan

**Target Document**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Review Period**: Round 1 Review\
**Date**: November 8, 2026\
**Participating Critics**:

1. Technical Writer (`writer`): `r1-writer.md` — Verdict: `REVISE` (0 BLOCKING, 5 MAJOR, 6 MINOR)
1. Vortex & Spool-Mesh Expert (`vortex`): `r1-vortex.md` — Verdict: `REVISE` (2 BLOCKING, 2 MAJOR, 2
   MINOR)
1. Codebase Expert (`codebase`): `r1-codebase.md` — Verdict: `REVISE` (0 BLOCKING, 4 MAJOR, 5 MINOR)
1. System Architect (`architect`): `r1-architect.md` — Verdict: `REVISE` (0 BLOCKING, 4 MAJOR, 3
   MINOR)
1. PL Design & Ergonomics Expert (`ergo`): `r1-ergo.md` — Verdict: `REVISE` (0 BLOCKING, 3 MAJOR, 4
   MINOR)
1. Xanadu / ZigZag Purist (`zigzag`): `r1-zigzag.md` — Verdict: `REVISE` (0 BLOCKING, 2 MAJOR, 3
   MINOR)

______________________________________________________________________

## 1. Executive Disposition Summary

In Round 1 of the Vortex Design Critique, all six specialized critics evaluated the unified *VAgent
Harmonization & Missing Features Implementation Plan* governing Proposals **P26 through P37**. While
all critics praised the architectural discipline, adherence to Nelsonian hypermedia principles,
2-degree manifold topology, and universal 1-based indexing, they surfaced **2 BLOCKING**, **20
MAJOR**, and **23 MINOR** issues (with substantial overlap across critic lanes).

Every single finding across all six critique documents has been accepted, resolved, and integrated
into the updated implementation plan.

```
+-----------------------------------------------------------------------------------------+
|                               ROUND 1 DISPOSITION SUMMARY                               |
+-------------------+-----------------+-------------------+-------------------+-----------+
| Critic Lane       | Blocking Raised | Major Raised      | Minor Raised      | Status    |
+-------------------+-----------------+-------------------+-------------------+-----------+
| Technical Writer  | 0               | 5                 | 6                 | RESOLVED  |
| Vortex / Mesh     | 2               | 2                 | 2                 | RESOLVED  |
| Codebase Expert   | 0               | 4                 | 5                 | RESOLVED  |
| System Architect  | 0               | 4                 | 3                 | RESOLVED  |
| PL Ergonomics     | 0               | 3                 | 4                 | RESOLVED  |
| Xanadu / ZigZag   | 0               | 2                 | 3                 | RESOLVED  |
+-------------------+-----------------+-------------------+-------------------+-----------+
| TOTAL UNIQUE      | 2 (Both Vortex) | 10 Unique Classes | 12 Unique Classes | CONVERGED |
+-------------------+-----------------+-------------------+-------------------+-----------+
```

______________________________________________________________________

## 2. Blocking Findings & Resolutions

### B-01: Choice Stack / Trail Desynchronization on Arm Rollback (Vortex Expert)

- **Issue**: Track 4B specified calling `arena.release(m_arm)` on speculative arm failure without
  cutting the VM choice stack. In Vortex VM, choice points contain arena marks and trail indices;
  releasing arena memory underneath un-cut choice points corrupts the VM choice stack and trail
  unwind invariants when subsequent operations backtrack.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 1.1 (Invariant 3) and Section 5.2 (Track 4B Deliverable 3) now mandate
  recording both `m_arm = arena.mark()` and `cp_arm = vm.choiceStack().size()` prior to arm
  execution. On arm failure, the executor strictly executes `vm.cut(cp_arm)` before calling
  `arena.release(m_arm)`.

### B-02: Intention Scheduler Arm Deliberation & `bodyName` Desync Trap (Vortex Expert)

- **Issue**: Targeting `executor.cpp:3030–3090` (delayed fault backoff ladder) delayed deliberation
  across turns rather than speculating intra-turn. Crucially, advancing an intention's `plan` to an
  auxiliary arm (`PlanName$2`) without updating `bodyName` and `owner` triggered an immediate
  `liveName != bodyName` check in `executor.cpp:3233`, faulting the intention with
  `"body_replaced"`.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 5.2 (Track 4B Deliverable 4) relocates the deliberation hook to the
  execution failure site (`executor.cpp:3240–3275`, line 3269). Upon selecting an untried arm, the
  executor updates `plan`, `bodyName`, and `owner` on the intention record, resets `step := 0`,
  clears `faulted`, and sets `ready := 1`.

______________________________________________________________________

## 3. Major Findings & Resolutions

### M-01: Quine Pass Criteria Fallacy (System Architect)

- **Issue**: Section 7.1 specified that successive compiler quine passes yield "bit-for-bit
  identical" microversions. In an append-only Merkle ops spool, subsequent compilations append ops
  at strictly monotonic higher indices, different timestamps, and unique Ed25519 signatures; bitwise
  identity is impossible.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 7.1 and Section 1 now specify the Nelsonian quine criterion: **graph
  isomorphism of opcode/descriptor manifolds modulo serial labels and base offsets**, matching
  recipe opcode sequences, identical wing topologies, and functional fixpoint
  $I_2(P) \equiv I_3(P)$.

### M-02: Phase 5 Architectural Dependency Inversion (System Architect, Technical Writer)

- **Issue**: Track 5A (`StagedCompiler`) was scheduled prior to Track 5B (Stdlib Migration), yet
  `StagedCompiler` directly invokes and composes the stdlib compiler passes.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Phase 5 has been restructured into four sequential sub-phases:
  - Sub-Phase 5.1: Compiler Passes Migration (`lexer`..`decls.vagent`).
  - Sub-Phase 5.2: Metacircular Pipeline `behavior StagedCompiler` (`compiler.vagent`).
  - Sub-Phase 5.3: Runtime Modules Migration (`mind.vagent`, `arbitration.vagent`).
  - Sub-Phase 5.4: Swarm Coordinator `agent CompilerCoordinator` (`coordinator.vagent`).

### M-03: Garbled Four-Tier Evaluation Hierarchy (PL Ergonomics, Technical Writer)

- **Issue**: Section 3.1 Item 5 and Register P34 misdefined the four-tier hierarchy as an arithmetic
  / relational operator precedence ladder, confusing execution regimes with operator precedence.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 3.1 Item 4, Section 7.1, and Section 8 (P34) articulate the genuine
  four-tier evaluation hierarchy:
  - Tier 1 (Procedural Sequencing): `\n`, `;` executing sequentially along `+d.spin` with
    statement-boundary choice cuts via `#CutChoiceDepth`.
  - Tier 2 (Pure Boolean Logic): `not`, `and`, `or` operating on `Bool` with 0 choice points.
  - Tier 3 (Goal-Directed Execution): `&` (conjunction), `|` (alternation), `!` (cut).
  - Tier 4 (Relational Belief Queries): `believes (...)` evaluating Horn clause queries over
    `d.clause`.

### M-04: Omission of Safe Navigation (`?.`) and Null Coalescing (`??`) (Ergo, Writer)

- **Issue**: Proposals P32 and P33 accepted safe navigation and null coalescing, but the plan
  omitted tokens, AST nodes, typing, and lowering for `?.` and `??`.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**:
  - Track 2A (§3.1): Added `TokenKind::QuestionDot` (`?.`) and `TokenKind::DoubleQuestion` (`??`).
  - Track 2B (§3.2): Added `ExprKind::SafeNav` and `ExprKind::Coalesce`.
  - Track 2C (§3.3): Added parser rules for postfix `?.` and infix `??` at Precedence 5.
  - Track 3B (§4.1): Added typing rules for `base?.field` (`Opt<FieldType>`) and `opt ?? fallback`
    (`T`).
  - Track 4A (§5.1): Lowered to conditional branching on tag presence.

### M-05: Dynamic Image Spawning Ingestion of `MicroversionId` (System Architect)

- **Issue**: Workers exchange `MicroversionId` strings (`Str`), but
  `spawn image(root: CellRef, ...)` accepted only in-arena `CellRef` handles, preventing cross-actor
  spawning without breaking Spool-Mesh isolation.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 5.4 (Track 4D Deliverable 1) overloads `spawn image` to accept
  `root: CellRef | Str`. If given `Str`, the runtime loads the immutable microversion directly from
  the bound store spool.

### M-06: Phantom Opcode Names in Lowering (Vortex Expert, Codebase Expert, Purist)

- **Issue**: Sections 1, 5.1, 7.1, 8, and 9 cited non-existent opcodes `#Equal` and `#ExtractField`.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Replaced `#Equal` with `OpcodeKind::Eq`. Replaced `#ExtractField` with
  `OpcodeKind::Find` (for named record fields) and `OpcodeKind::Linked` (for Crum/tuple traversal).

### M-07: Ground-Truth Codebase Anchors for P26 & P27 (Codebase Expert, Technical Writer)

- **Issue**: Line numbers for P26 in `vortex_stdlib.hpp:160–185` and `.cpp:2580–2645` pointed into
  `std:collections`. Target type `Dims` and methods in `vortex_core.hpp/cpp` and `lineage.hpp` were
  fictitious.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**:
  - Section 2.1: Re-anchored new modules `std:io` and `std:env` to `vortex_stdlib.hpp:420–445, 510`
    and `vortex_stdlib.cpp:4760–4850`. Added member `std::vector<std::string> cliArgs_;`.
  - Section 2.3: Corrected struct name to `struct SystemDimensions` (`vortex_core.hpp:34–57`),
    genesis method to `VortexCore::initGenesis()` (`vortex_core.cpp:31–72`), and dimension array to
    `kGenesisDimensionNames` (`lineage.hpp:46–52`).

### M-08: Stdlib File Inventory Correction (Codebase Expert, Technical Writer)

- **Issue**: Section 6.2 listed non-existent files `token.vagent` and `bdi.vagent`, while omitting
  `fragment.vagent` and `arbitration.vagent`.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 6.1 and Section 6.3 list the verified eleven standard library files:
  `lexer.vagent`, `parser.vagent`, `ast.vagent`, `expand.vagent`, `resolve.vagent`,
  `checker.vagent`, `emitter.vagent`, `fragment.vagent`, `decls.vagent`, `mind.vagent`, and
  `arbitration.vagent`.

### M-09: Discovery Walk Omission of `d.dims` & Version Repointing in `promote()` (Purist)

- **Issue**: In `arena_manifold.cpp:1745–1760`, `promote()` executes a discovery walk strictly from
  `root` and `extraRoots`. If `extraRoots` omits `d.dims`, all links along unpromoted dimensions are
  silently dropped. Sequential promotions also failed to advance `boundStore_`'s version.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 2.2 (Track 1B Deliverable 3) mandates that `VortexStdLib::zzPromote`
  pass `extraRoots` containing the head of `d.dims` and relocation targets (`+d.mesh.relocation`),
  and advance hypertime via `boundStore_->repointCurrentVersion(promoted->version)`.

### M-10: Type-Aware Map Keys & Numeric Ordering Inversion (Purist, Vortex Expert)

- **Issue**: `vortex_stdlib.hpp` typed map keys as `std::string_view key`. Converting integer keys
  to strings produces lexicographical inversion (`"10" < "2"`), corrupting BST search and insertion.
- **Disposition**: **ACCEPTED & RESOLVED**.
- **Action Taken**: Section 2.3 (Track 1C Deliverable 6) updates map primitives to accept
  `const CellValue &key` and perform type-aware scalar comparisons on `CellValue` bits
  (lexicographical for `Str`, numeric for `Int64`, index arithmetic for `CellRef`).

______________________________________________________________________

## 4. Minor Findings & Resolutions

- **m-01 (W-06, ARCH-MIN-02)**: Return types for `make_cell`, `text_of`, and `promote`: Updated to
  sentinel-free `Result<CellRef, ZigzagError>`, `Result<Str, ZigzagError>`, and
  `promote(root, budget: Opt<Int>) : Result<Str, ZigzagError>`.
- **m-02 (W-07)**: Omission of `std:env:get_var`: Added `std:env:get_var(key: Str) -> Opt<Str>` to
  Track 1A deliverables.
- **m-03 (W-08)**: Pattern node AST taxonomy: Added `WildcardPattern` and `CtorPattern` to
  `ExprKind`; removed `DefaultArm` as an expression kind.
- **m-04 (W-09)**: Single-arm plan backward compatibility: Added
  `PlanBody ::= SinglePlanBody | MultiArmPlanBody`.
- **m-05 (W-10)**: Standardized on `TokenKind::Ellipsis` (`...`).
- **m-06 (W-04, ERGO-MIN-02)**: Added structured string scanning `scan subject with { ... }`,
  deleted `allowScan_` modal state, and added `tab_end()` primitive to replace legacy `tab(0)`.
- **m-07 (W-05)**: Added `vortex_vm.cpp:1898–1937` to Track 3A target files and deliverables for
  1-based VM opcodes `ByteAt` and `StrSlice`.
- **m-08 (ERGO-MIN-01)**: Clarified that `E101` is static literal 0 rejection; dynamic out-of-bounds
  emits `#Fail` in goal contexts or runtime contract violation.
- **m-09 (ERGO-MIN-03)**: Syntactically enforced that `default => Block` is strictly terminal in
  `PlanArmList`.
- **m-10 (ERGO-MIN-04)**: Added event payload pattern destructuring in
  `on Event { pat } where Guard`.
- **m-11 (ERGO-MAJ-03)**: Added bidirectional type inference for `Map {}`, typing for map methods
  (`get`, `contains`, `keys`, `len`), and 2-tuple loop inference `for (k, v) in map do`.
- **m-12 (ZIGZAG-MIN-04, ARCH-MIN-03)**: Documented $O(1)$ candidate predecessor/successor tracking
  during $O(\log N)$ descent and minimum key insertion splicing.
- **m-13 (CODE-MAJ-04)**: Updated `decls.cpp:1984` precondition check to
  `if (!decl.body && decl.planArms.empty())`. Updated `resolve.cpp:walk` exhaustive switch for all
  new `ExprKind` variants.
- **m-14 (CODE-MIN-02, 03)**: Updated serialization ranges in `r1_ast.cpp:78–270, 272–425` and added
  `ExprKind::Index` lowering to `compiler.cpp:770, 1739`.
- **m-15 (VORTEX-MIN-02)**: Centralized fuel deduction via `consumeWork()` inside `mapKeyCell` to
  eliminate double-counting in `mapGet` and `mapSet`.

______________________________________________________________________

## 5. Convergence Assessment & Next Steps

All 6 critics have completed their assessments and their required remediations have been completely
implemented in `design/vagent-harmonization-and-missing-features-implementation-plan.md`. The target
document cleanly passes both `mdformat --wrap 100` and `mdl -i -s .mdl_style.rb` with 0 warnings.

With all blocking and major concerns resolved, Round 1 achieves full architectural convergence
across all six critic lanes.
