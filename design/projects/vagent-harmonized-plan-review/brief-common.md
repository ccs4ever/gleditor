# Round 1 Critique Brief: VAgent Harmonization & Missing Features Implementation Plan

**Target Document**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Companion Documents**:

- `design/vagent-metacircular-missing-features.md` (Proposals P26–P30, P36–P37)
- `design/vagent-language-harmonization.md` (Proposals P31–P35)
- `design/vagent-metacircular-compiler-implementation.md` (Metacircular Pipeline Architecture)

**Context & User Mandate**: **ALL PROPOSALS (P26–P37) ARE FORMALLY ACCEPTED.** The focus of this
critique is the **Implementation Plan**: its phasing, dependency ordering, C++23 codebase
integration, AST structures, operational lowering, Spool-Mesh turn boundaries, and test matrix
completeness.

______________________________________________________________________

## Areas for Critique Across All Six Roles

### 1. Technical Writer (`writer`)

- **Lane**: Specification clarity, structure, internal consistency, cross-references, test matrix
  traceability, and markdown hygiene.
- Verify that every proposal (P26–P37) is accounted for in the phase breakdown, test matrix, and
  proposal register.
- Check that all EBNF productions, type names, token names, and diagnostics match the accepted
  companion specs.
- Ensure the document adheres to `mdformat --wrap 100` and `mdl -i`.

### 2. Vortex & Spool-Mesh Expert (`vortex`)

- **Lane**: VM execution semantics, bytecode graph lowering, choice stack / trail safety, Spool-Mesh
  turn atomicity, and fuel accounting.
- Check Phase 1C and Phase 4C: does tree descent over `d.left` and `d.right` properly consume work
  (`core_.arena().consumeWork()`) on each hop? Does subscript lowering avoid linear `#Find` scans?
- Check Phase 4A: is pattern matching lowered strictly to deterministic `#Equal`/`#Branch` decision
  trees without `#Choice`/`#Fail` choice stack leaks?
- Check Phase 4B: does intra-turn arm speculation properly capture `m_arm = arena.mark()` and
  release uncommitted cells and un-stage `+d.outbox` entries on arm failure?

### 3. Codebase Expert (`codebase`)

- **Lane**: Ground-truth verification against the live C++23 repository, exact file anchors, line
  numbers, class/method signatures, and AST structures.
- Verify line anchors and file paths in `vortex_stdlib.hpp/cpp`, `vortex_core.hpp/cpp`, `token.hpp`,
  `parser.cpp`, `ast.hpp`, `checker.cpp`, `compiler.cpp`, `decls.cpp`, and `executor.cpp`.
- Verify AST definitions: `struct PlanArm`, `Decl::planArms`, and new `ExprKind` enumerators.
- Check plan lowering: verify `decls.cpp:1268–1301, 1979–2000` is the correct home for plan clause
  construction, and `MetaRuntime::planNamed` resolves the primary arm.

### 4. System Architect (`architect`)

- **Lane**: Phasing logic, dependency DAG, Spool-Mesh actor isolation, memory safety, and quine
  verification.
- Check phase ordering: does Phase 1 (Substrate) strictly precede Phase 2 (Syntax), Phase 3
  (Semantics), Phase 4 (Lowering), Phase 5 (Stdlib), and Phase 6 (Quine)?
- Verify Spool-Mesh actor isolation: do workers exchange immutable `MicroversionId` strings rather
  than ephemeral raw `CellRef` pointers?
- Check memory bounds and resource quotas (`maxCells`, `maxInstructions`, scratch buffer caps).

### 5. Programming Language Design & Ergonomics Expert (`ergo`)

- **Lane**: Surface language ergonomics, 1-based indexing semantics, canonical `Opt<T>`, operator
  hierarchy, delimiter orthogonality, and stdlib migration ergonomics.
- Check Phase 3A: is 1-based indexing ($[1, N]$, $N+1$ append, `E101` on 0, closed slice $[i..j]$)
  consistently defined across collections, strings, and loops?
- Check Phase 3B: is the distinction between soft unwrap `opt?` (goal-directed `#Fail`), assertive
  hard unwrap `opt!` (`ContractViolation`), and safe lookup `map.get(k)` (`Opt<V>`) ergonomic?
- Check Phase 2C: are multi-arm plan discriminators (`where =>`, `case =>`, `default =>`) and
  delimiter orthogonality (`(...)` tuples, `[...]` lists, `{...}` records) clean and unambiguous?

### 6. Xanadu / ZigZag Purist (`zigzag`)

- **Lane**: Nelsonian hypermedia principles, strict 2-degree rank limits, and Store-Slice
  convergence.
- Check Phase 1C: does the 2-degree binary search tree on orthogonal dimensions `d.left` and
  `d.right` strictly respect 2-degree bounds ($\text{deg}^+ \le 1$, $\text{deg}^- \le 1$)?
- Check container stability: does `mapRoot` (text `"map"`) insulate user variables from tree
  rotations, and is the in-order sequence on `+d.step` strictly invariant under rotations?
- Verify persistence: is `std:zigzag:promote` the exclusive mechanism for persistence, and are disk
  writes strictly prohibited?

______________________________________________________________________

## Instructions for Critics

Write your structured review to: `design/projects/vagent-harmonized-plan-review/r1-<role>.md`

Format your review at \<= 100 columns with fenced code blocks surrounded by blank lines. Classify
findings into BLOCKING, MAJOR, MINOR. Conclude with `VERDICT: CONVERGED` if 0 BLOCKING and 0 MAJOR
issues, or `VERDICT: NOT CONVERGED` / `VERDICT: REVISE` if any BLOCKING or MAJOR issues are
identified.
