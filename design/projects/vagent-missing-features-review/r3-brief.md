# Round 3 Critique Brief: Chained Plan Guards & Tree-Backed Map

**Target Document**: `design/vagent-metacircular-missing-features.md` (§7, §8, and proposals P31–P32
in §9)\
**Context**: Incremental extension to the missing features document incorporating:

1. **Section 7**: Chained Plan Guards & Multi-Arm BDI Deliberation (Proposal P31)
1. **Section 8**: First-Class Tree-Backed `Map<K, V>` on `d.branch` (Proposal P32)
1. **Section 9**: Proposals P31 and P32 in the Proposal Register
1. **Section 10**: Refused alternatives 10.6 (cross-statement side-effect backtracking) and 10.7
   (opaque C++ heap hash maps)

______________________________________________________________________

## Areas for Critique Across All Six Roles

### 1. Technical Writer (`writer`)

- Are the EBNF grammar rules for `PlanBody`, `MultiArmPlanBody`, `PlanArm`, and `Map` unambiguous
  and consistent with existing syntax?
- Are code examples complete, type-correct, and clearly explained?
- Is the document structure coherent, with clean transitions and precise terminology?

### 2. Vortex Expert (`vortex`)

- Does the lowering of multi-arm plans onto `+d.clause` ranks respect VM instruction flow?
- Does BDI plan arm reconsideration on `#Fail` maintain turn atomicity, fuel tracking
  (`consumeWork`), and choice-point stack integrity?
- Is the tree traversal over `d.branch` mechanically sympathetic with Vortex VM opcodes?

### 3. Codebase Expert (`codebase`)

- Are the line anchors in `apps/common/xanadu/vortex/vortex_stdlib.cpp:2650–2695`,
  `vortex_core.hpp:45–65`, `checker.cpp:1380–1400`, and `parser.cpp:570–615` accurate?
- Does `Map<K, V>` properly integrate with existing typechecking rules in `Checker::validateType`
  and subscript type inference?
- How does `PlanName$1`, `PlanName$2` clause naming interact with `buildResolutionIndex` in
  `resolve.cpp`?

### 4. System Architect (`architect`)

- Does multi-arm plan execution preserve Spool-Mesh actor isolation and turn boundaries?
- Does tree-backed `Map` promotion via `std:zigzag:promote` work seamlessly with the Merkle ops
  spool?
- Does `d.branch` adhere to 2-degree rank limits (no fan-in, strict bidirectional links)?

### 5. PL Design & Ergonomics Expert (`ergo`)

- Is multi-arm plan syntax (`where => { ... }` / `case Pattern where guard => { ... }`) intuitive
  and ergonomic?
- Does `Map { ... }` literal syntax clearly differentiate from records without parsing ambiguity?
- Does subscripting `map[k]?` with goal-directed `#Fail` provide a clean developer experience
  compared to verbose `if` chains?

### 6. Xanadu / ZigZag Purist (`zigzag`)

- Does the binary search tree on `d.branch` strictly respect Nelsonian hypermedia principles (no
  flat opaque C++ side tables, no hidden pointers)?
- Are tree nodes first-class cells with spatial transparency, able to be visualized and transcluded?
- Does in-order linking along `+d.step` preserve orthogonal spatial traversal?

______________________________________________________________________

## Instructions for Critics

Review lines 520–780 of `design/vagent-metacircular-missing-features.md`. Write your review to
`design/projects/vagent-missing-features-review/r3-<role>.md`. Conclude with `VERDICT: CONVERGED` if
there are 0 BLOCKING and 0 MAJOR issues, or state your findings.
