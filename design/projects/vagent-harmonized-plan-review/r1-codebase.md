# Round 1 Critique: Codebase Expert (`codebase`)

**Review Target**: `design/vagent-harmonization-and-missing-features-implementation-plan.md`\
**Companion Documents**:

- `design/vagent-metacircular-missing-features.md` (Proposals P26–P30, P36–P37)
- `design/vagent-language-harmonization.md` (Proposals P31–P35)
- `design/vagent-metacircular-compiler-implementation.md` (Metacircular Pipeline Architecture)
- `design/projects/vagent-harmonized-plan-review/brief-common.md`

______________________________________________________________________

## 1. Executive Summary & Codebase Ground-Truth Assessment

The *VAgent Harmonization & Missing Features Implementation Plan* defines a comprehensive,
architecturally principled path for integrating all twelve accepted proposals (**P26 through P37**)
into the C++23 engine substrate and self-hosting `.vagent` standard library. The core technical
decisions—such as lowering multi-arm plans into chained clauses along the goal host's `+d.clause`
rank, executing $O(\log N)$ 2-degree BST traversal over orthogonal `d.left` and `d.right`
dimensions, and speculative intra-turn arena marks `m_arm`—align with the Nelsonian invariants and
the existing execution engine in `apps/common/xanadu/`.

A line-by-line verification against the live C++23 repository reveals **0 BLOCKING**, **4 MAJOR**,
and **5 MINOR** codebase discrepancies. While the operational intent is sound, several file anchors,
line ranges, type names, and member signatures do not match the actual code in the repository. In
particular, the standard library inventory in Section 6.2 lists non-existent files and omits real
modules; dimension registration references fictitious types and methods; and line numbers in
`vortex_stdlib.hpp/.cpp` point into `std:collections` instead of new module extension points.

```
+-----------------------------------------------------------------------------------------+
|                               CODEBASE AUDIT SUMMARY                                    |
+-------------------+---------------------------------------------------------------------+
| Target Document   | design/vagent-harmonization-and-missing-features-implementation-plan.md |
| Audit Scope       | Exact file paths, line anchors, C++23 types, methods, AST structures|
| Proposals Audited | P26 through P37 (All 12 proposals)                                  |
| Status Summary    | 0 BLOCKING, 4 MAJOR, 5 MINOR                                        |
| Verdict           | VERDICT: REVISE                                                     |
+-------------------+---------------------------------------------------------------------+
```

### Key Findings Overview

1. **`MAJOR-CODE-01` (Section 2.1)**: Target file line numbers for P26 (`std:io`, `std:env`) in
   `apps/common/xanadu/vortex/vortex_stdlib.hpp:160–185` and `vortex_stdlib.cpp:2580–2645` are
   completely mispointed. In the actual repository, these lines are `std:collections` (`createList`,
   `listToVector`, `pushBack`, etc.). `std:io` and `std:env` must be added as new modules around
   lines 420–445 of `.hpp` and line 4760 of `.cpp`.
1. **`MAJOR-CODE-02` (Section 2.3)**: Target type and method names for dimension registration in
   `vortex_core.hpp/cpp` and `lineage.hpp` are fictitious. The dimensions struct is named
   `struct SystemDimensions` (not `Dims`); the dimension initialization method is
   `VortexCore::initGenesis()` (not `VortexCore::bootstrapStandardDimensions()`); and dimension
   vocabulary in `lineage.hpp` is registered in `kGenesisDimensionNames` (not
   `VagentLineage::standardDimensions()`).
1. **`MAJOR-CODE-03` (Section 6.2)**: Standard library file inventory lists non-existent files
   `token.vagent` and `bdi.vagent`, while omitting the two actual standard library modules present
   on disk: `arbitration.vagent` and `fragment.vagent`.
1. **`MAJOR-CODE-04` (Section 3.2 & 5.2)**: `decls.cpp:1984` contains an unhandled precondition
   check `if (!decl.body)` that unconditionally rejects multi-arm plans (which carry arms in
   `decl.planArms` rather than a single `decl.body`). Furthermore, `resolve.cpp:walk` has an
   exhaustive switch over `ExprKind` (lines 391–682) that must be updated for all new `ExprKind`
   variants, which the plan omits from Section 3.2 deliverables.

______________________________________________________________________

## 2. Ground-Truth Verification of File Anchors & Signatures by Proposal

### 2.1 Track 1A: Epistemic Ingestion (P26)

#### Target Files Cited in Plan

- `apps/common/xanadu/vortex/vortex_stdlib.hpp:160–185`
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2580–2645`
- `apps/vagent/main.cpp:56–76, 337–426`

#### Codebase Reality

In `vortex_stdlib.hpp`, lines 160–185 are inside `// -- Module 5: std:collections`:

```cpp
[[nodiscard]] std::vector<CellValue>
listToVector(CellRef head, std::optional<DimRef> dim = std::nullopt) const;
void pushBack(CellRef head, const CellValue &val, ...);
CellRef pushFront(...);
std::optional<CellValue> popBack(...);
std::optional<CellRef> popFront(...);
[[nodiscard]] std::size_t listLength(...) const;
CellRef createMap();
```

In `vortex_stdlib.cpp`, lines 2580–2645 implement `createList`, `listToVector`, `pushBack`,
`pushFront`, `popBack`, `popFront`, and `listLength`.

Adding `std:io` and `std:env` to `std:collections` is an architectural error. In
`vortex_stdlib.hpp`, modules 1 through 15 are defined up to line 422. New module declarations belong
after `std:goal` (around line 440), and their registration in `init()` occurs around line 1479
(`build...Module`). In `vortex_stdlib.cpp`, implementations belong after line 4760
(`buildZigzagModule`).

`apps/vagent/main.cpp:56–76` is `bool readSource(std::string_view path, std::string &source)`, which
is an exact match.

`apps/vagent/main.cpp:380–426` is `int main(int argc, char *argv[])`, where CLI arguments are parsed
into `std::vector<std::pair<std::string, std::vector<SendSpec>>> units`. Forwarding positional
arguments into `stdlib.setArgs(argv)` correctly touches lines 398–420.

In `VortexStdLib`, `setArgs(std::vector<std::string>)` requires a member field
`std::vector<std::string> cliArgs_;`, which belongs in the private members section of
`vortex_stdlib.hpp` (around line 510).

### 2.2 Track 1B: ZigZag Primitives (P27)

#### Target Files Cited in Plan

- `apps/common/xanadu/vortex/vortex_stdlib.hpp:317–330, 505`
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:4634–4760`
- `apps/common/xanadu/vortex/vortex_core.hpp:50–55`

#### Codebase Reality

`vortex_stdlib.hpp:505` is exact:

```cpp
const xanadu::Store *boundStore_{nullptr};
```

Changing this to mutable `Store *boundStore_{nullptr};` is verified and valid.

`vortex_stdlib.hpp:317–330` is `// -- Module 10: std:zigzag` (`zzStep`, `zzInsert`, `zzUnlink`,
`zzLink`, `zzDelete`, `zzCloneToChain`, `zzDuplicate`). Adding `make_cell`, `text_of`, `set_text`,
`set_value`, `resolve_dim`, and `promote` here is verified.

`vortex_stdlib.cpp:4634–4760` is `VortexStdLib::buildZigzagModule(CellRef mod)`, where exported
routine bindings are constructed.

In `apps/common/xanadu/vortex/vortex_stdlib.cpp:5087–5099`, `zzCloneToChain` is defined:

```cpp
std::optional<CellRef> VortexStdLib::zzCloneToChain(CellRef symbolOp,
                                                    CellRef targetCell) {
  if (symbolOp == noCell || !core_.arena().contains(symbolOp) ||
      targetCell == noCell || !core_.arena().contains(targetCell)) {
    return std::nullopt;
  }
  CellRef clone = core_.arena().makeCell();
  zigzag::expectWritten(
      core_.arena().link(symbolOp, core_.dims().clone, DimVector::POS, clone));
  zigzag::expectWritten(
      core_.arena().link(targetCell, core_.dims().spin, DimVector::POS, clone));
  return clone;
}
```

Linking directly to `targetCell` on `dims().spin` without finding the rank tail violates 2-degree
reciprocity when `targetCell` already has a positive neighbor. Appending to
`rankTail(core_.arena(), targetCell, core_.dims().spin)` is the exact required fix.

`vortex_core.hpp:50`: `DimRef dims{noCell};` is correctly positioned in `SystemDimensions`.

### 2.3 Track 1C: Orthogonal Tree Dimensions & 2-Degree BST (P37 Substrate)

#### Target Files Cited in Plan

- `apps/common/xanadu/vortex/vortex_core.hpp:45–65`
- `apps/common/xanadu/vortex/vortex_core.cpp:52–72`
- `apps/common/xanadu/vagent/lineage.hpp:45–80`
- `apps/common/xanadu/vortex/vortex_stdlib.hpp:170–190`
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2650–2695, 2785–2791`

#### Codebase Reality

The plan specifies: "Add orthogonal dimensions `DimRef left{noCell};` and `DimRef right{noCell};` to
`Dims` in `vortex_core.hpp`. Register `d.left` and `d.right` in
`VortexCore::bootstrapStandardDimensions()` and `VagentLineage::standardDimensions()`."

In `apps/common/xanadu/vortex/vortex_core.hpp:34–57`, the struct is named `struct SystemDimensions`,
NOT `Dims`:

```cpp
struct SystemDimensions {
  DimRef grab{noCell};
  DimRef step{noCell};
  DimRef spin{noCell};
  DimRef stack{noCell};
  DimRef contract{noCell};
  DimRef clone{noCell};
  DimRef cursors{noCell};
  DimRef cache{noCell};
  DimRef vars{noCell};
  DimRef values{noCell};
  DimRef pinningCursors{noCell};
  DimRef name{noCell};
  DimRef role{noCell};
  DimRef dims{noCell};
  DimRef stdlib{noCell};
  DimRef clause{noCell};
  DimRef scan{noCell};
  DimRef stores{noCell};
};
```

In `apps/common/xanadu/vortex/vortex_core.cpp:31–72`, the dimension minting method is
`void VortexCore::initGenesis()`, NOT `bootstrapStandardDimensions()`. Lines 52–72 are the exact
lines where `dims_.grab` through `dims_.role` are minted off `dims_.dims`.

In `apps/common/xanadu/vagent/lineage.hpp:46–52`, there is NO method
`VagentLineage::standardDimensions()`. The system dimensions are declared as:

```cpp
inline constexpr std::string_view kGenesisDimensionNames[] = {
    "d.dims",   "d.grab",     "d.step",       "d.spin",
    "d.stack",  "d.contract", "d.clone",      "d.cursors",
    "d.cache",  "d.vars",     "d.values",     "d.pinning-cursors",
    "d.name",   "d.stdlib",   "d.clause",     "d.scan",
    "d.stores", "d.role",     "d.store-refs",
};
```

Dimensions `"d.left"` and `"d.right"` must be appended to `kGenesisDimensionNames` in
`lineage.hpp:51`.

In `vortex_stdlib.cpp:2650–2695`, `createMap`, `mapSet`, `mapGet`, `mapHas`, and `mapKeys` are
implemented. Currently, `mapSet` and `mapKeyCell` perform an $O(N)$ linear search on
`core_.dims().vars`. Replacing this with $O(\log N)$ descent along `dims_.left` and `dims_.right`
touches these exact lines.

In `vortex_stdlib.cpp:2785–2791`, `VortexStdLib::mapKeyCell` is defined:

```cpp
std::optional<CellRef>
VortexStdLib::mapKeyCell(const CellRef mapRoot,
                         const std::string_view key) const {
  return firstOf(rankAfter(core_.arena(), mapRoot, core_.dims().vars) |
                 std::views::filter([&](const CellRef cell) {
                   return core_.arena().textOf(cell) == key;
                 }));
}
```

This linear scan is replaced by BST descent. The line reference `2785–2791` is exact.

### 2.4 Track 2A: Lexer Tokens & Operator Disambiguation (P33, P34, P35, P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/token.hpp:15–80`
- `apps/common/xanadu/vagent/token.cpp:20–95`
- `apps/common/xanadu/vagent/lexer.cpp:60–180`

#### Codebase Reality

`token.hpp:15–53` defines `enum class TokenKind : std::uint8_t`.

In `token.cpp:20–72`, keywords are not distinct `TokenKind` variants; they are recognized by
`bool isKeyword(std::string_view word)` and emitted as `TokenKind::Keyword`. If distinct tokens
(`KwMap`, `KwMatch`, `KwWith`, etc.) are introduced, `token.hpp:TokenKind`,
`token.cpp:tokenKindName`, and `lexer.cpp:identifier()` must all be updated.

`lexer.cpp:60–180` covers `peek()`, `advance()`, `token()`, `newline()`, `identifier()`, and
`number()`. Citing `60–180` completely omits `Lexer::punctuation()` at
`apps/common/xanadu/vagent/lexer.cpp:267–339`.

Punctuation scanning for `?` (line 309), `;` (line 293), `+` / `++` (line 299), and `: / :=` (line
297\) is located in `Lexer::punctuation()`.

Disambiguating `?` into prefix logic variables vs postfix question mark, as well as `...` and `=>`,
belongs in `lexer.cpp:267–339`.

### 2.5 Track 2B: AST Definitions & Pipeline Infrastructure (P30, P35, P36, P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/ast.hpp:35–110, 208–245`
- `apps/common/xanadu/vagent/ast_clone.cpp:45–85`
- `apps/common/xanadu/vagent/r1_ast.hpp:25–65`
- `apps/common/xanadu/vagent/r1_ast.cpp:480–560`

#### Codebase Reality

In `ast.hpp:38–92`, `enum class ExprKind : std::uint8_t` contains 53 enumerators. Adding the 8 new
variants (`Match`, `MatchArm`, `Map`, `Tuple`, `TuplePattern`, `ListPattern`, `RecordPattern`,
`DefaultArm`) fits safely within `std::uint8_t`.

In `ast.hpp:208–231`, `struct Decl` is defined. Adding `std::vector<PlanArm> planArms;` is clean and
valid.

In `ast.hpp:177–182`, `struct GoalClause` is defined. `struct PlanArm` should be defined right after
`GoalClause` (around line 183), where `ExprPtr` and `SourceLocation` are complete.

In `ast_clone.cpp:45–85`, `cloneDecl` clones contracts, goalClauses, members, body, and guard.
Deep-cloning `decl.planArms` via `cloneExpr` on each arm's `pattern`, `guard`, and `body` fits
directly into lines 45–73.

In `r1_ast.cpp:480–560`, `marshalDeclToR1` (480–505), `marshalProgramToR1` (508–541), and
`unmarshalR1ToDecl` (543–565) are defined. However, serialization and deserialization of the new
`ExprKind` variants requires editing `marshalAstToR1` (lines 78–270) and `unmarshalR1ToAst` (lines
272–425). The plan's anchor `480–560` omitted expression serialization.

### 2.6 Track 2C: Recursive Descent Parser Extensions (P30, P35, P36, P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/parser.cpp:560–620, 840–920, 1010–1090`

#### Codebase Reality

`parser.cpp:560–575` is `Decl Parser::plan()`.

`parser.cpp:577–625` is `Decl Parser::declaration(Context context)`.

`parser.cpp:844–854` parses `ExprKind::Index` in `Parser::postfix()`.

`parser.cpp:872–887` is `Parser::list()`.

`parser.cpp:889–915` is `Parser::record(Token name)`.

`parser.cpp:917–925` is `Parser::termArgument()`.

`parser.cpp:1010–1090` is in `Parser::primary()`, handling `for`, `believes`, `findall`, `splice`,
`static`, actions, and PascalCase records.

These line anchors are exceptionally accurate and capture the exact recursive descent parsing
boundaries.

### 2.7 Track 3A: Universal 1-Based Indexing Harmonization (P31)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/checker.cpp:1380–1405, 1960–1985`
- `apps/common/xanadu/vagent/compiler.cpp:1730–1765`

#### Codebase Reality

`checker.cpp:1383–1395` is `case ExprKind::Index:` in `Checker::place` (validating indexing in
lvalue assignment positions).

`checker.cpp:1964–1982` is `case ExprKind::Index:` in `Checker::infer` (type checking `List`, `Str`,
and `Map` indexing).

`compiler.cpp:1730–1765` covers `Block`, `Field`, `Record`, `Cells`, and `Let`. Notice:
`compiler.cpp` does NOT currently contain any lowering for `ExprKind::Index`! Citing lines 1730–1765
points to the insertion point in `emitValue`, but the plan should explicitly state that
`case ExprKind::Index:` must be introduced into both `preflight()` (around line 770) and
`emitValue()` (around line 1739).

### 2.8 Track 3B: Canonical Option (`Opt<T>`) & Tripartite Lookup (P32, P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/checker.cpp:700–740, 1850–1890`
- `apps/common/xanadu/vagent/compiler.cpp:1600–1635`

#### Codebase Reality

`checker.cpp:700–718` is `Checker::validateType`, where `Opt<T>` arity (1) is checked.

`checker.cpp:1844–1855` is unary `!` checking (legacy generation). Modifying this for hard unwrap
`opt!` and adding postfix `?` soft unwrap fits here.

`compiler.cpp:1600–1619` is `case ExprKind::Unary:`.

`compiler.cpp:1620–1640` is `case ExprKind::Binary:` with `op == "?"` (legacy Icon scanning).
Repurposing `?` for soft unwrap to jump to `cells_->fail` is verified.

### 2.9 Track 3C: Pattern Matching Typing & Static Exhaustiveness (P30)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/checker.cpp:2050–2120, 2740–2790`

#### Codebase Reality

`checker.cpp:2050–2120` is inside `Checker::infer` (`List`, `Cells`, `Record`). Inserting
`case ExprKind::Match:` and `case ExprKind::Map:` here is verified.

`checker.cpp:2740–2790` is `Checker::declaration` for `Goal`, `Plan`, and `Handler`. This is the
home of multi-arm plan checking (Track 3D), not pattern matching typing or exhaustiveness. Citing
`2740–2790` in Track 3C is a copy-paste error from Track 3D.

### 2.10 Track 3D: Multi-Arm Plan Checking & Variable Scoping (P36)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/resolve.cpp:763–785`
- `apps/common/xanadu/vagent/checker.cpp:2747–2785`

#### Codebase Reality

`resolve.cpp:763–782` is `case DeclKind::Plan:` in `Resolver::declaration`. Walking each arm in
`decl.planArms` belongs here.

`checker.cpp:2747–2783` is `case DeclKind::Plan:` in `Checker::declaration`. Validating
`GoalPattern` arity and checking arm guards/bodies belongs here. Line references are exact.

### 2.11 Track 3E: Map Type Synthesis & Subscript Validation (P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/checker.cpp:1385–1395, 2060–2085`

#### Codebase Reality

`checker.cpp:1385–1395` is `Checker::place` for `ExprKind::Index`.

`checker.cpp:2060–2085` is `case ExprKind::List:` and `case ExprKind::Cells:` in `Checker::infer`.
Inserting `case ExprKind::Map:` here is exact.

### 2.12 Track 4A: Deterministic Pattern Lowering (P30)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/compiler.cpp:1540–1620, 1850–1920`

#### Codebase Reality

`compiler.cpp:1552–1562` is `bounded()`.

`compiler.cpp:1571–1809` is `emitValue()`.

`compiler.cpp:1863–1911` is `emitStaticCell()`.

Lowering deterministic decision trees via `#Equal`, `#Branch`, and `#ExtractField` without `#Choice`
points integrates smoothly into `emitValue()`.

### 2.13 Track 4B: Multi-Arm Plan Lowering & BDI Scheduler (P36)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/decls.cpp:1268–1301, 1979–2000`
- `apps/common/xanadu/vagent/executor.cpp:206–273, 3030–3090`
- `apps/common/xanadu/vagent/meta.cpp:1270–1360`

#### Codebase Reality

Verified in detail in Section 4 of this report.

### 2.14 Track 4C: Logarithmic Map Subscript Lowering (P37)

#### Target Files Cited in Plan

- `apps/common/xanadu/vagent/compiler.cpp:1730–1760`
- `apps/common/xanadu/vortex/vortex_vm.hpp:55–65`
- `apps/common/xanadu/vortex/vortex_vm.cpp:880–920`

#### Codebase Reality

In `vortex_vm.hpp`, lines 55–65 are control flow opcodes (`Clone`, `Jump`, `Branch`, `Call`, etc.).
`OpcodeKind::Find` is defined at line 105, not lines 55–65.

In `vortex_vm.cpp:876–904`, `case OpcodeKind::Find:` executes
`rankAfter(core_.arena(), referenced, dims.step)`, which is the $O(N)$ linear scan on records.

In `vortex_vm.cpp:905–925`, `case OpcodeKind::FieldSet:` executes the matching $O(N)$ write scan.

In `compiler.cpp`, lowering `map[k]` to `std:map:get` instead of `#FIND` prevents linear scans.

### 2.15 Track 4D: Dynamic Image Spawning (P29)

#### Target Files Cited in Plan

- `apps/vagent/main.cpp:275–296`
- `apps/common/xanadu/vagent/adapter.hpp:85–115`
- `apps/common/xanadu/vagent/adapter.cpp:140–185`
- `apps/common/xanadu/vagent/executor.cpp:3350–3380`

#### Codebase Reality

In `main.cpp:275–296`, `adapter.setSpawnSender` binds `runtime.spawn`. Exact match.

In `adapter.hpp:80–120` and `adapter.cpp:144–175`, `setSpawnSender` and `stageSend` are implemented.

In `executor.cpp:3350–3380`, this is `drainOutbox`. The actual host step execution for `spawn` is in
`runHostStep` at `apps/common/xanadu/vagent/executor.cpp:1433–1477`, and outbox drain dispatch is at
lines 3423–3440. Citing `3350–3380` points to outbox helpers rather than the spawn performative
execution logic.

______________________________________________________________________

## 3. Deep-Dive: AST Additions & Structural Integrity

### 3.1 Verification of `struct PlanArm` and `Decl::planArms`

The proposed `struct PlanArm` is:

```cpp
struct PlanArm {
  ExprPtr pattern;
  ExprPtr guard;
  ExprPtr body;
  SourceLocation loc{};
};
```

In `apps/common/xanadu/vagent/ast.hpp`:

1. `ExprPtr` is declared as an alias at line 95 (`using ExprPtr = std::unique_ptr<Expr>;`), but
   `struct Expr` is an incomplete type until line 148.
1. Therefore, `struct PlanArm` cannot be defined prior to line 148. It fits naturally at line 183,
   adjacent to `struct GoalClause` (lines 177–182).
1. In `struct Decl` (lines 208–231), adding `std::vector<PlanArm> planArms;` is clean and does not
   disrupt existing fields.

### 3.2 Deep-Cloning in `ast_clone.cpp`

`ExprPtr` is a move-only `std::unique_ptr<Expr>`. In
`apps/common/xanadu/vagent/ast_clone.cpp:43–73`, every struct member with `ExprPtr` must be
deep-cloned. The cloning loop for `planArms` must be:

```cpp
copy.planArms.reserve(decl.planArms.size());
for (const auto &arm : decl.planArms) {
  PlanArm cloned;
  cloned.pattern = cloneExpr(arm.pattern);
  cloned.guard   = cloneExpr(arm.guard);
  cloned.body    = cloneExpr(arm.body);
  cloned.loc     = arm.loc;
  copy.planArms.push_back(std::move(cloned));
}
```

### 3.3 New `ExprKind` Variants & Codebase Cascade Effects

The plan proposes 8 new `ExprKind` variants:

```
Match, MatchArm, Map, Tuple, TuplePattern, ListPattern, RecordPattern, DefaultArm
```

In the codebase, `ExprKind` is inspected in exhaustive switches across 9 distinct source files:

1. `apps/common/xanadu/vagent/ast.hpp:38–92`: Enumeration definition.
1. `apps/common/xanadu/vagent/resolve.cpp:391–682`: Exhaustive switch in `Resolver::walk`. **Every
   new `ExprKind` MUST be added here**, or resolution will omit variable scoping for pattern arms
   and match bodies! The implementation plan omitted this file from Track 2B.
1. `apps/common/xanadu/vagent/checker.cpp:1640–2130`: Type inference switch in `Checker::infer`.
1. `apps/common/xanadu/vagent/compiler.cpp:450–862`: `preflight` switch.
1. `apps/common/xanadu/vagent/compiler.cpp:1571–1809`: `emitValue` switch.
1. `apps/common/xanadu/vagent/r1_ast.cpp:78–270`: `marshalAstToR1` (AST to ZigZag cell graph).
1. `apps/common/xanadu/vagent/r1_ast.cpp:272–425`: `unmarshalR1ToAst` (ZigZag cells to AST).
1. `apps/common/xanadu/vagent/code_path.cpp:20–95`: Code path analysis.
1. `apps/common/xanadu/vagent/host_compile.cpp:50–120`: Host compile pipeline.

Notice that `DefaultArm` is redundant as an `ExprKind`: a default arm in a multi-arm plan has no
pattern and no guard; it is simply a `PlanArm` where `pattern == nullptr` and `guard == nullptr`.
Treating it as a separate expression kind complicates AST representations unnecessarily.

______________________________________________________________________

## 4. Deep-Dive: Plan Lowering in `decls.cpp` & Resolution in `meta.cpp`

### 4.1 Lowering Multi-Arm Plans to `+d.clause` Ranks

In `apps/common/xanadu/vagent/decls.cpp:188–209`, `mintClause` constructs a clause cell:

```cpp
CellRef Lowerer::mintClause(CellRef host, std::string_view name, CellRef testHead,
                            CellRef bodyHead,
                            const std::optional<std::int64_t> &priority) {
  const auto clause = arena.makeCell();
  zigzag::expectWritten(arena.link(clause, dims().name, DimVector::POS,
                                   arena.makeCell(std::string{name})));
  zigzag::expectWritten(
      arena.link(clause, dims().grab, DimVector::POS, testHead));
  zigzag::expectWritten(
      arena.link(clause, dims().spin, DimVector::POS, bodyHead));
  ...
  zigzag::expectWritten(
      arena.link(zigzag::rankTail(arena, host, dims().clause), dims().clause,
                 DimVector::POS, clause));
  image.clauses.push_back(clause);
  return clause;
}
```

Notice that `mintClause` appends each clause to `rankTail(..., dims().clause)` along
`dims().clause`. Therefore, sequentially invoking `lowerClauseBody` for each arm $1 \dots M$ links
all $M$ clause cells along `+d.clause` of the host symbol.

- Arm 1: labelled `name` (`PlanName`)
- Arm 2: labelled `std::format("{}$2", name)`
- Arm $k$: labelled `std::format("{}${}", name, k)`

### 4.2 The `decls.cpp:1984` Blocking Precondition Bug

In `apps/common/xanadu/vagent/decls.cpp:1979–2000`:

```cpp
case DeclKind::Plan: {
  if (!decl.target) {
    refuse(decl, std::format("plan '{}' names no goal", name));
    return;
  }
  if (!decl.body) {
    refuse(decl, std::format("plan '{}' declares no body", name));
    return;
  }
  ...
```

If a plan is declared with multiple arms, the parser populates `decl.planArms`, leaving
`decl.body == nullptr`. Line 1984 will immediately trigger `refuse(decl, "declares no body")`!

**Remediation**: Line 1984 must be updated to:

```cpp
if (!decl.body && decl.planArms.empty()) {
  refuse(decl, std::format("plan '{}' declares no body or arms", name));
  return;
}
```

And lines 1998–1999 must iterate over `decl.planArms`:

```cpp
if (!decl.planArms.empty()) {
  for (std::size_t i = 0; i < decl.planArms.size(); ++i) {
    const auto &arm = decl.planArms[i];
    const std::string armName =
        i == 0 ? std::string(name) : std::format("{}${}", name, i + 1);
    lowerPlanArm(arm, host, paramsOf(decl, *decl.target), armName);
  }
} else {
  lowerClauseBody(decl, host, paramsOf(decl, *decl.target), name);
}
```

### 4.3 Clause Resolution in `meta.cpp`

In `apps/common/xanadu/vagent/meta.cpp:266–287`:

```cpp
std::optional<CellRef>
MetaRuntime::planNamed(std::string_view qualified) const {
  ...
  const auto match = [&](CellRef host) -> std::optional<CellRef> {
    for (const auto symbol :
         zigzag::rankAfter(arena(), host, core_.dims().vars))
      for (const auto clause : clausesOf(symbol))
        if (labelOf(clause).value_or("") == qualified) return clause;
    return std::nullopt;
  };
  ...
}
```

Because Arm 1 is labelled `PlanName`, calling `MetaRuntime::planNamed("PlanName")` matches and
returns Arm 1's clause cell verbatim.

In `apps/common/xanadu/vagent/meta.cpp:1272–1348`, `MetaRuntime::selectClause` iterates over
`clausesOf(host)`. When Arm 1 fails, the executor appends Arm 1's clause to the intention's `tried`
set. `selectClause` skips Arm 1 (`if (tried.contains(clause)) continue;`) and evaluates Arm 2's
preflight guard on `+d.grab`. If Arm 2's guard produces, Arm 2 is selected. This mechanism is
completely sound.

______________________________________________________________________

## 5. Deep-Dive: Spool-Mesh & Actor Execution Mechanics in `executor.cpp`

### 5.1 Intra-Turn Speculative Rollback (`m_arm`)

In `apps/common/xanadu/vagent/executor.cpp:258–273`, the turn-level base mark `m0` is captured:

```cpp
const CellRef failOp = vm.mintOpcode(OpcodeKind::Fail, "fail_step");
vm.pushChoicePoint(exec.cursor, failOp);
const auto m0 = arena.mark();
```

When an arm body executes in the intention loop (around lines 3240–3275), executing arm side effects
speculatively requires an inner arm mark:

```cpp
const auto m_arm = arena.mark();
...
if (!ok) {
  // 1. Roll back uncommitted arena cells minted during this arm:
  arena.release(m_arm);
  // 2. Un-stage any outbox entries staged on d.outbox during this arm:
  unstageOutbox(arena, core, agent, outboxMark);
  // 3. Mark clause tried and reconsider untried arms:
  stageTriedClause(intention, planCell);
  ...
}
```

The plan correctly identifies the need for `m_arm = arena.mark()`, but cites lines `3030–3090`
(which is the fault ladder at the top of the turn loop). Arm body execution and failure handling
actually occur at `apps/common/xanadu/vagent/executor.cpp:3240–3275`.

______________________________________________________________________

## 6. Standard Library Inventory & Metacircular File Audit

Section 6.2 of the Implementation Plan lists eleven standard library files for migration:

```
apps/common/xanadu/vagent/stdlib/token.vagent
apps/common/xanadu/vagent/stdlib/lexer.vagent
apps/common/xanadu/vagent/stdlib/ast.vagent
apps/common/xanadu/vagent/stdlib/parser.vagent
apps/common/xanadu/vagent/stdlib/expand.vagent
apps/common/xanadu/vagent/stdlib/resolve.vagent
apps/common/xanadu/vagent/stdlib/checker.vagent
apps/common/xanadu/vagent/stdlib/emitter.vagent
apps/common/xanadu/vagent/stdlib/decls.vagent
apps/common/xanadu/vagent/stdlib/mind.vagent
apps/common/xanadu/vagent/stdlib/bdi.vagent
```

### 6.1 Codebase Reality Check: The Disk Inventory

An inspection of `apps/common/xanadu/vagent/stdlib/` on the live filesystem reveals the following
eleven `.vagent` files:

```sh
$ ls -1 apps/common/xanadu/vagent/stdlib/
arbitration.vagent
ast.vagent
checker.vagent
decls.vagent
emitter.vagent
expand.vagent
fragment.vagent
lexer.vagent
mind.vagent
parser.vagent
resolve.vagent
```

### 6.2 The Inventory Discrepancy

```
+-----------------------------------+-----------------------------------+
| Listed in Plan (Section 6.2)      | Actual File on Disk               |
+-----------------------------------+-----------------------------------+
| token.vagent                      | DOES NOT EXIST (Tokens in lexer)  |
| lexer.vagent                      | lexer.vagent                      |
| ast.vagent                        | ast.vagent                        |
| parser.vagent                     | parser.vagent                     |
| expand.vagent                     | expand.vagent                     |
| resolve.vagent                    | resolve.vagent                    |
| checker.vagent                    | checker.vagent                    |
| emitter.vagent                    | emitter.vagent                    |
| decls.vagent                      | decls.vagent                      |
| mind.vagent                       | mind.vagent                       |
| bdi.vagent                        | DOES NOT EXIST (In arbitration)   |
| [OMITTED FROM PLAN]               | arbitration.vagent                |
| [OMITTED FROM PLAN]               | fragment.vagent                   |
+-----------------------------------+-----------------------------------+
```

- `token.vagent` does NOT exist; token scanning is incorporated into `lexer.vagent`.
- `bdi.vagent` does NOT exist; intention scheduling and BDI deliberation are split between
  `mind.vagent` and `arbitration.vagent`.
- `arbitration.vagent` and `fragment.vagent` exist on disk, contain active metacircular compiler
  code, but are omitted from the plan's migration register.

Section 6.2 must be corrected to list the actual 11 files present on disk.

______________________________________________________________________

## 7. Complete Findings Register

| Finding ID      | Severity  | File / Section Reference                                  | Description & Codebase Impact                                                                                    |
| :-------------- | :-------- | :-------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------- |
| `MAJOR-CODE-01` | **MAJOR** | `vortex_stdlib.hpp:160–185`, `vortex_stdlib.cpp:2580`     | P26 target lines point to `std:collections` instead of new `std:io`/`std:env` extension points (lines 440+).     |
| `MAJOR-CODE-02` | **MAJOR** | `vortex_core.hpp:45`, `vortex_core.cpp:52`, `lineage.hpp` | Fictitious names `Dims`, `bootstrapStandardDimensions()`, `VagentLineage::standardDimensions()`.                 |
| `MAJOR-CODE-03` | **MAJOR** | Plan Section 6.2                                          | Stdlib inventory lists non-existent `token.vagent`, `bdi.vagent`; omits `arbitration.vagent`, `fragment.vagent`. |
| `MAJOR-CODE-04` | **MAJOR** | `decls.cpp:1984`, `resolve.cpp:391–682`                   | Precondition `!decl.body` rejects multi-arm plans; `resolve.cpp:walk` switch missing new `ExprKind` variants.    |
| `MINOR-CODE-01` | **MINOR** | `lexer.cpp:60–180`                                        | Target range omits `Lexer::punctuation()` (`lexer.cpp:267–339`), where `?`, `;`, `=>`, `...` are scanned.        |
| `MINOR-CODE-02` | **MINOR** | `r1_ast.cpp:480–560`                                      | Omits expression serialization functions `marshalAstToR1` (78–270) and `unmarshalR1ToAst` (272–425).             |
| `MINOR-CODE-03` | **MINOR** | `compiler.cpp:1730–1765`, `vortex_vm.hpp:55–65`           | Subscript lowering (`Index`) is absent from `compiler.cpp`; `vortex_vm.hpp` opcode `Find` is at line 105.        |
| `MINOR-CODE-04` | **MINOR** | `checker.cpp:2740–2790` (Section 4.3)                     | Cites Goal/Plan/Handler declaration checking instead of expression pattern inference in `Checker::infer`.        |
| `MINOR-CODE-05` | **MINOR** | `executor.cpp:3350–3380` (Section 5.4)                    | Cites `drainOutbox` helpers instead of `spawn` verb execution (`1433–1477`) and drain dispatch (`3423–3440`).    |

______________________________________________________________________

## 8. Actionable Remediation Checklist

1. **Fix Section 2.1 File Anchors**:
   - Update `apps/common/xanadu/vortex/vortex_stdlib.hpp` anchor to lines `420–445` (module
     declarations) and line `510` (`std::vector<std::string> cliArgs_;`).
   - Update `apps/common/xanadu/vortex/vortex_stdlib.cpp` anchor to lines `1475–1498` (module setup
     in `init()`) and line `4765+` (module implementations).
1. **Correct Dimension Types and Initializers in Section 2.3**:
   - Replace `Dims` with `struct SystemDimensions` (`vortex_core.hpp:34–57`).
   - Replace `bootstrapStandardDimensions()` with `VortexCore::initGenesis()`
     (`vortex_core.cpp:31–72`).
   - Replace `VagentLineage::standardDimensions()` with `kGenesisDimensionNames` in
     `lineage.hpp:46–52`.
1. **Correct Standard Library Module Inventory in Section 6.2**:
   - Replace `token.vagent` and `bdi.vagent` with `arbitration.vagent` and `fragment.vagent`.
1. **Fix Multi-Arm Plan Lowering Precondition in Section 5.2**:
   - Explicitly document the update to `decls.cpp:1984` allowing empty `decl.body` when
     `!decl.planArms.empty()`.
   - Add `resolve.cpp:391–682` to Track 2B deliverables to handle new `ExprKind` variants in
     `Resolver::walk`.
1. **Adjust Supporting File Anchors**:
   - Add `lexer.cpp:267–339` (`punctuation()`) to Section 3.1.
   - Add `r1_ast.cpp:78–425` (`marshalAstToR1`, `unmarshalR1ToAst`) to Section 3.2.
   - Correct `vortex_vm.hpp` anchor to line `105` for `OpcodeKind::Find` in Section 5.3.
   - Point Section 5.4 to `executor.cpp:1433–1477` and `3423–3440` for spawn verb execution.

______________________________________________________________________

## 9. Verdict

`VERDICT: REVISE`

The implementation plan is architecturally grounded and technically well-conceived, but contains
four **MAJOR** ground-truth discrepancies against the live C++23 repository that must be remediated
before engineering execution begins. Applying the concrete remediations listed above will achieve
full codebase convergence.
