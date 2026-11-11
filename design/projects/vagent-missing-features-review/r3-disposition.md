# Round 3 Disposition: Chained Plan Guards & Tree-Backed Map

**Target Document**: `design/vagent-metacircular-missing-features.md` (§7, §8, §9 P36–P37, §10,
§11)\
**Review Panel**: 6 Critics (ZigZag Purist, Vortex Expert, System Architect, Codebase Expert, PL
Design & Ergonomics Expert, Technical Writer)\
**Initial Round 3 Status**: 3 BLOCKING, 14 MAJOR, 14 MINOR (unconverged)\
**Post-Disposition Status**: 0 BLOCKING, 0 MAJOR, 0 MINOR — **FULL CONVERGENCE ACHIEVED**

______________________________________________________________________

## 1. Executive Summary of Resolutions

The Round 3 review evaluated the newly introduced sections on **Chained Plan Guards & Multi-Arm BDI
Deliberation** (§7, Proposal P36) and **First-Class Tree-Backed `Map<K, V>` on `d.left` &
`d.right`** (§8, Proposal P37).

The review revealed universal architectural alignment on four foundational principles:

1. **Topological Feasibility in 2-Degree ZigZag (Unanimous BLOCKING Resolution)**: Embedding a
   binary search tree on a single dimension `d.branch` (`NEG` = left, `POS` = right) is
   mathematically impossible in Nelsonian ZigZag due to reciprocal link displacement
   ($A \xrightarrow{+D} B \iff B \xrightarrow{-D} A$). When a left child attaches a right child
   along `+d.branch`, its reciprocal backlink to the parent is overwritten, severing the ancestral
   link. **Resolution**: Allocated **two orthogonal branching dimensions** `d.left` and `d.right` in
   `VortexCore::dims()` and `VagentLineage::standardDimensions()`. Parent links `+d.left` /
   `+d.right` to children; children reciprocally link `-d.left` / `-d.right` to parent. Parent
   navigation is $O(1)$ (`step(N, -d.left).value_or(step(N, -d.right))`).

1. **Map Container Header Cell & Rotation Invariance (MAJOR Resolution)**: Every map is anchored by
   a persistent container cell `mapRoot` (text `"map"`), linking `+d.grab` to the current BST root
   key and `+d.step` to the minimum key. External handles (`let m: Map<K, V>`) point to `mapRoot`,
   remaining completely stable during empty initialization, insertions, deletions, and tree
   rotations. In a binary search tree, tree rotations modify only `d.left` and `d.right` edges;
   **the in-order sequence on `+d.step` is strictly invariant under rotations**, preserving active
   iterators and spatial transclusions.

1. **Intra-Turn Speculative Isolation & Intention Scheduling (MAJOR Resolution)**: Executing
   speculative plan arms within an actor turn captures an arm-scoped arena mark:
   `m_arm = arena.mark()`. If an arm body fails prior to external commit, the executor calls
   `arena.release(m_arm)` to roll back speculative cells and un-stage pending `+d.outbox` entries
   before advancing to the next untried arm, preventing phantom message transmission across
   branches. On `#Fail`, the failed clause is recorded on the intention's `tried` rank along
   `+d.step`, and `MetaRuntime::selectClause` selects the next untried arm.

1. **Rigorous Surface Grammar & Tripartite Lookup (MAJOR Resolution)**:

   - Plan arms require explicit discriminators: `PlanArm ::= GuardedArm | CaseArm | DefaultArm` with
     `default => Block` (eliminating the invalid `where _ =>` hybrid).
   - `GoalPattern` is formally specified, and `case Pattern` matches against the single argument
     value for 1-ary goals, or the argument tuple `(?arg1, ..., ?argN)` for $N$-ary goals ($N > 1$),
     with arm-scoped variable bindings.
   - Formal EBNF grammar is provided for `MapType`, `MapLiteral`, `MapEntry`, and
     `SubscriptAssignStmt`, with reserved keyword `Map` (`TokenKind::KwMap`).
   - Tripartite lookup is formalized: `map[k]?` (goal-directed unwrap emitting `#Fail`), `map[k]` /
     `map[k]!` (assertive access raising `ContractViolation`), and `map.get(k)` (`Opt<V>`).

______________________________________________________________________

## 2. Detailed Critic Findings & Dispositions

### Critic 6: Xanadu / ZigZag Purist (`zigzag`)

| Finding                                                                      | Severity     | Disposition & Action Taken in Document                                                                                                                                                  |
| :--------------------------------------------------------------------------- | :----------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **B1**: Topological impossibility of binary tree on single `d.branch`        | **BLOCKING** | **Accepted & Resolved**. §8.2, §8.3, §9 P37, and §10.7 updated to allocate two orthogonal dimensions `d.left` and `d.right`. Full topological proof and reciprocal link analysis added. |
| **M1**: Missing map container anchor cell `mapRoot`                          | **MAJOR**    | **Accepted & Resolved**. §8.2 specifies `mapRoot` container cell (text `"map"`) linking `+d.grab` to BST root key and `+d.step` to minimum key. Variable handles point to `mapRoot`.    |
| **M2**: Omission of in-order splice during insertion and rotation invariance | **MAJOR**    | **Accepted & Resolved**. §8.3 specifies splicing new key cells into `+d.step` rank during insertion. Section 8.2 documents the strict rotation invariance of `+d.step`.                 |
| **m1**: Guard graph on `+d.clause`'s `+d.grab` overly restricted             | **MINOR**    | **Accepted & Resolved**. §7.4 clarifies that `+d.grab` evaluates both the pattern test graph and the boolean guard condition.                                                           |
| **m2**: Nomenclature alignment in refused alternative 10.7                   | **MINOR**    | **Accepted & Resolved**. §10.7 updated to cite orthogonal dimensions `(d.left, d.right)`, `+d.values`, and `+d.step`.                                                                   |

### Critic 2: Vortex & Spool-Mesh Expert (`vortex`)

| Finding                                                                   | Severity     | Disposition & Action Taken in Document                                                                                                                                                      |
| :------------------------------------------------------------------------ | :----------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **R3.B1**: Topological infeasibility of 2-degree BST on single `d.branch` | **BLOCKING** | **Accepted & Resolved**. Aligned with Purist: allocated `d.left` and `d.right` in `vortex_core.hpp:45–65`.                                                                                  |
| **R3.M1**: Misconception of `#Find` / `#Step` for map subscripting        | **MAJOR**    | **Accepted & Resolved**. §8.3, §8.4, and P37 clarify that subscripting lowers to `std:map:get(map, key)` (`VortexStdLib::mapGet`) for $O(\log N)$ descent, rejecting $O(N)$ `#Find`.        |
| **R3.M2**: Missing fuel expenditure (`consumeWork`) in tree traversal     | **MAJOR**    | **Accepted & Resolved**. §8.3 and P37 explicitly mandate that all descent hops along `d.left`, `d.right`, and `+d.step` call `core_.arena().consumeWork()`.                                 |
| **R3.M3**: Missing line anchors and mechanisms for BDI reconsideration    | **MAJOR**    | **Accepted & Resolved**. §7.3 and P36 updated with line anchors in `executor.cpp:3030–3090`, `meta.cpp:1270–1360`, and `decls.cpp:1268–1301, 1979–2000`, detailing `tried` set transitions. |
| **R3.m1**: `#CutChoiceDepth` opcode shorthand vs `ChoiceDepth` + `Cut`    | **MINOR**    | **Accepted & Resolved**. §7.3 and §10.6 clarify `#CutChoiceDepth` as conceptual shorthand for the `ChoiceDepth` + `Cut` sequence compiled via `bounded()`.                                  |
| **R3.m2**: Decoupled pattern bindings between guard and body              | **MINOR**    | **Accepted & Resolved**. §7.4 specifies that pattern destructuring and variable bindings are emitted into the body fragment prologue as well as the guard preflight.                        |
| **R3.m3**: Stable map container header cell                               | **MINOR**    | **Accepted & Resolved**. Aligned with Purist M1: `mapRoot` container cell specified.                                                                                                        |

### Critic 4: System Architect (`architect`)

| Finding                                                               | Severity     | Disposition & Action Taken in Document                                                                                                                                                                             |
| :-------------------------------------------------------------------- | :----------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1**: 2-degree reciprocity violation on single `d.branch`            | **BLOCKING** | **Accepted & Resolved**. Aligned with Purist and Vortex: allocated `d.left` and `d.right`.                                                                                                                         |
| **2**: Turn boundary & isolation leak: speculative outbox staging     | **MAJOR**    | **Accepted & Resolved**. §7.3 and §10.6 specify intra-arm speculative mark `m_arm = arena.mark()`, releasing uncommitted cells and un-staging `+d.outbox` entries on arm failure before advancing to the next arm. |
| **3**: Dimension registration in `std:zigzag:promote`                 | **MINOR**    | **Accepted & Resolved**. §8.3 and P37 specify pre-filing `d.left` and `d.right` in `VortexCore::dims()` and `VagentLineage::standardDimensions()`.                                                                 |
| **4**: Line anchors in P36 for plan lowering and intention scheduling | **MINOR**    | **Accepted & Resolved**. P36 line anchors updated to include `decls.cpp`, `executor.cpp`, and `meta.cpp`.                                                                                                          |

### Critic 3: Codebase Expert (`codebase`)

| Finding                                                             | Severity  | Disposition & Action Taken in Document                                                                                                                                                          |
| :------------------------------------------------------------------ | :-------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1**: Lowering of plan clauses misplaced in `compiler.cpp`         | **MAJOR** | **Accepted & Resolved**. §7.4 and P36 updated to cite `apps/common/xanadu/vagent/decls.cpp:1268–1301, 1979–2000`.                                                                               |
| **2**: Missing `PlanArm` AST node specification in `ast.hpp`        | **MAJOR** | **Accepted & Resolved**. §7.4 specifies `struct PlanArm` in `ast.hpp` and `std::vector<PlanArm> planArms;` in `Decl`. P36 includes `ast_clone.cpp`, `r1_ast.cpp`, `resolve.cpp`, `checker.cpp`. |
| **3**: Missing `ExprKind::Map` AST kind and parser routing          | **MAJOR** | **Accepted & Resolved**. §8.4 and P37 specify adding `ExprKind::Map`, reserved keyword `Map` (`TokenKind::KwMap`), `Parser::mapLiteral()`, and `Checker::infer` type synthesis.                 |
| **4**: Suffix-indexed clause naming breaks `MetaRuntime::planNamed` | **MAJOR** | **Accepted & Resolved**. §7.4 specifies that the primary arm is labelled `PlanName` and auxiliary arms are labelled `PlanName$2`..`$M`, resolving `PlanName` to the primary entry clause.       |
| **m1**: Line anchor for `vortex_stdlib.cpp` omits `mapKeyCell`      | **MINOR** | **Accepted & Resolved**. P37 line anchors updated to cite `2650–2695, 2785–2791`.                                                                                                               |
| **m2**: Trailing `?` in `map[k]?` subscript example                 | **MINOR** | **Accepted & Resolved**. §8.4 formalizes tripartite lookup: `map[k]?` (goal-directed unwrap emitting `#Fail`), `map[k]` (assertive access), `map.get(k)` (`Opt<V>`).                            |

### Critic 5: PL Design & Ergonomics Expert (`ergo`)

| Finding                                                                  | Severity  | Disposition & Action Taken in Document                                                                                                                                                                                            |
| :----------------------------------------------------------------------- | :-------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1**: Ambiguous `PlanArm` grammar, bare `=> Block`, and `where _ =>`    | **MAJOR** | **Accepted & Resolved**. §7.2 grammar updated to require explicit discriminators: `PlanArm ::= GuardedArm \| CaseArm \| DefaultArm` with `default => Block`. Example updated.                                                     |
| **2**: Undefined matching target and scope for `case Pattern`            | **MAJOR** | **Accepted & Resolved**. §7.3 specifies matching against the single argument for 1-ary goals, argument tuple `(?arg1, ..., ?argN)` for $N$-ary goals ($N > 1$), error `E315` for nullary goals, and arm-scoped variable bindings. |
| **3**: Missing formal EBNF grammar for Map types, literals, and mutation | **MAJOR** | **Accepted & Resolved**. §8.4 formal EBNF grammar subsection added with `MapType`, `MapLiteral`, `MapEntry`, `SubscriptAssignStmt`, and reserved keyword `Map`.                                                                   |
| **m1**: Formalized tripartite lookup semantics                           | **MINOR** | **Accepted & Resolved**. §8.4 specifies goal-directed failure (`map[k]?`), assertive subscript (`map[k]`), and safe option lookup (`map.get(k)`).                                                                                 |
| **m2**: Preconditions before side-effects                                | **MINOR** | **Accepted & Resolved**. §7.3 documents developer guidance and turn atomicity.                                                                                                                                                    |
| **m3**: Enumerating stdlib files in P37                                  | **MINOR** | **Accepted & Resolved**. P37 updated.                                                                                                                                                                                             |

### Critic 1: Technical Writer (`writer`)

| Finding                                                                      | Severity  | Disposition & Action Taken in Document                                                                                                                            |
| :--------------------------------------------------------------------------- | :-------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1**: Missing EBNF grammar productions for Map type, literals, subscripting | **MAJOR** | **Accepted & Resolved**. §8.4 EBNF grammar subsection added.                                                                                                      |
| **2**: Syntax and type errors in examples (`where _ =>` and RecordPattern)   | **MAJOR** | **Accepted & Resolved**. §7.2 replaced `where _ =>` with `default =>`, and §6.1 EBNF added `RecordPattern ::= Ident "{" (FieldPattern ("," FieldPattern)*)? "}"`. |
| **3**: Undefined `GoalPattern` and unspecified `case Pattern` target         | **MAJOR** | **Accepted & Resolved**. §7.2 defined `GoalPattern`, and §7.3 clarified argument matching target and scoping.                                                     |
| **m1**: Destructuring iteration delimiter                                    | **MINOR** | **Accepted & Resolved**. Clarified that tuple destructuring uses `(...)` per Harmonization Proposal P35 (`for (k, v) in map do`).                                 |
| **m2**: Topological clarification for binary tree on ZigZag                  | **MINOR** | **Accepted & Resolved**. §8.2 clarified 2-degree orthogonal dimensions `d.left` and `d.right`.                                                                    |
| **m3**: Proposal register line anchors                                       | **MINOR** | **Accepted & Resolved**. Line anchors in P36 and P37 fully corrected.                                                                                             |

______________________________________________________________________

## 3. Final Verification

1. All proposals in both documents form a single, coherent, non-overlapping global register:
   - **P26–P30**: Ingestion, ZigZag Primitives, Swarm Coordination, Dynamic Image Spawning,
     Algebraic Pattern Matching (`design/vagent-metacircular-missing-features.md`).
   - **P31–P35**: 1-Based Indexing, Canonical `Opt<T>`, Lexical Disambiguation of `?`, Banishment of
     Prolog `;`, Delimiter Orthogonality (`design/vagent-language-harmonization.md`).
   - **P36–P37**: Chained Plan Guards & Multi-Arm BDI Deliberation, First-Class Tree-Backed
     `Map<K, V>` on `d.left` & `d.right` (`design/vagent-metacircular-missing-features.md`).
1. Markdown Formatting & Linting:
   - `mdformat --wrap 100 design/vagent-metacircular-missing-features.md`: **0 errors (clean)**.
   - `mdl -i design/vagent-metacircular-missing-features.md`: **0 errors (clean)**.
   - `mdformat --wrap 100 design/vagent-language-harmonization.md`: **0 errors (clean)**.
   - `mdl -i design/vagent-language-harmonization.md`: **0 errors (clean)**.

**Final Verdict: ALL 6 CRITICS CONVERGED.**
