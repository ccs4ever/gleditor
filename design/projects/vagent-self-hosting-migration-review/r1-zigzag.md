# Round 1 critique: Xanadu & ZigZag Purist (design/vagent-self-hosting-migration.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 4 MAJOR, 4 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

*(Round 1: Initial review; no previous findings).*

## BLOCKING

### 1. (§4 Phase 1, line 158) R1 AST topology contradicts canonical 2-degree schema

**Problem:** The migration plan asserts that `parser.vagent` "Builds R1 AST cells connected via
`+d.step` and annotated with `+d.name`, `+d.kind`, `+d.vars`." This directly violates the canonical
2-degree ZigZag R1 AST schema established in `apps/common/xanadu/vagent/r1_ast.hpp:24–29`,
`r1_ast.cpp:141–179`, and `design/vagent-metacircular-compiler-implementation.md:658–677`
(`ZIGZAG-3`, `ZIGZAG-4`):

1. **Collapsing parent-child hierarchy on `+d.step`:** An AST is a hierarchical tree. In the
   canonical 2-degree ZigZag schema, a parent links along **`+d.grab`** to its first child
   (`node +d.grab → child_0`), and sibling children chain along **`+d.step`**
   (`child_0 +d.step → child_1 +d.step → ...`). Because `+d.grab` is parent-to-child and `+d.step`
   is sibling-to-sibling, interior AST nodes retain their sibling rank on `d.step` while anchoring
   their own child list on `d.grab` without dimensional collision. If parents connect to children
   via `+d.step`, then an interior node's `+d.step` would simultaneously point to its sibling and to
   its child. In `ArenaManifold::link()` (`arena_manifold.cpp:1360–1378`), the second link forcibly
   evicts the first (`setOneSide(d, dim, dir, toCell)`), collapsing the AST tree into an unparseable
   1D line or causing link loss.
1. **Inventing an unfiled `+d.kind` dimension:** In R1 AST (`r1_ast.cpp:141`), the node kind tag
   (`"lit.int"`, `"binary"`, `"let"`, etc.) is stored directly as the cell's own text payload
   (`arena.makeCell(kindTag)`). There is no `+d.kind` dimension (it is not filed in
   `kGenesisDimensionNames` or `kMetaDimensionNames` in `lineage.hpp:46–63`). In `ZIGZAG-3`, linking
   `+d.verb` or `+d.kind` to kind vocabulary cells was explicitly refused: sharing kind cells causes
   catastrophic fan-in eviction (two binary nodes linking posward to `"binary"` evict each other's
   links), while minting private kind cells wastes an unnecessary cell and link per AST node.
1. **Omitting `+d.values`:** Type annotations on AST nodes live on
   `node +d.values → arena.makeCell(typeString)` (`r1_ast.hpp:28`, `r1_ast.cpp:146–149`), which line
   158 completely overlooks.

**Evidence:**

`design/vagent-self-hosting-migration.md:158`:

> *"Builds R1 AST cells connected via `+d.step` and annotated with `+d.name`, `+d.kind`,
> `+d.vars`."*

`apps/common/xanadu/vagent/r1_ast.hpp:24–29`:

```cpp
/// Marshals a parsed VAgent `Expr` into the 2-degree-safe R1 AST cell schema:
/// - Node cell text = canonical kind tag (`"lit.int"`, `"binary"`, etc.)
/// - `node +d.name` -> token/operator/identifier text cell
/// - `node +d.grab` -> first child, siblings chained on `+d.step`
/// - `node +d.values` -> optional fresh type-annotation cell
/// - `node +d.vars` -> optional `"line"` and `"col"` source coordinates
```

`apps/common/xanadu/vagent/r1_ast.cpp:172–179`:

```cpp
if (i == 0) {
  zigzag::expectWritten(arena.link(node, core.dims().grab, Posward, child));
} else {
  zigzag::expectWritten(
      arena.link(prevChild, core.dims().step, Posward, child));
}
```

`apps/common/xanadu/zigzag/arena_manifold.cpp:1360–1378` (mutual link eviction on fan-in).

**Fix:** Replace line 158 with the exact 2-degree R1 AST specification:

```markdown
- Builds R1 AST cells strictly adhering to `apps/common/xanadu/vagent/r1_ast.hpp`:
  - Node cell text carries the canonical kind tag (`"lit.int"`, `"binary"`, `"let"`, etc.) with
    zero kind links.
  - Parent links to first child posward on `+d.grab`; sibling children chain posward on `+d.step`.
  - Operator, token, or identifier text is linked posward on `+d.name`.
  - Type annotations are linked posward on `+d.values` to a freshly minted type cell per node
    (`ZIGZAG-8`).
  - Source coordinates are linked as `"line"` and `"col"` records on `+d.vars`.
```

______________________________________________________________________

### 2. (§4 Phase 2, lines 169–170) Symbol resolution lacks clone views, causing fan-in

**Problem:** Track 2.1 and Track B1 state:

> *"Resolves symbol bindings: binds parameter names, local variables, and imported module symbols."*
> *"Operates strictly as an AST-to-AST manifold transformation."*

Neither the text nor the architecture defines how multiple AST identifier nodes referencing the same
declaration (for example, three occurrences of variable `x` in `x + x * x`) link to the symbol
definition without violating the strict 2-degree ZigZag rank invariant
($A.\text{pos}(d) = B \iff B.\text{neg}(d) = A$).

In ZigZag, two distinct cells cannot both link posward (or negward) to the same target cell along
the same dimension. If `resolve.vagent` links AST identifier cells directly to a shared declaration
cell `Decl` via `ident_1 +d.refs → Decl` and `ident_2 +d.refs → Decl`, `ArenaManifold::link()`
(`arena_manifold.cpp:1369–1373`) forcibly evicts `Decl -d.refs → ident_1` when linking `ident_2`. As
a result, `ident_1`'s resolution link is severed, corrupting the AST!

In Nelsonian ZigZag, any many-to-one relationship MUST use either:

1. **Clone view cells:** For every reference to `Decl`, a distinct clone view cell $C_i$ is minted
   on `Decl`'s `+d.clone` rank (`Decl +d.clone → C_1 +d.clone → C_2...`), and each clone cell links
   1-to-1 to its referencing AST node.
1. **Ref-box chains:** A dedicated reference chain on `Decl +d.refs → RefBox_1 +d.step → RefBox_2`,
   where each ref-box cell links to the AST use.

Attempting an "AST-to-AST manifold transformation" without specifying whether clone views
(`d.clone`) or ref-box chains (`d.refs` + `d.step`) are used guarantees that an implementer will
either cause catastrophic link eviction or reintroduce ad-hoc C++ side-tables
(`std::map<string, ScopeStamp>`), violating Standing Rule 2 (`brief-common.md:14–16`).

**Evidence:**

`design/vagent-self-hosting-migration.md:169–170`:

> *"Resolves symbol bindings: binds parameter names, local variables, and imported module symbols."*

`design/vagent-self-hosting-migration.md:285–286`:

> *"Track B1 (stdlib/expand.vagent + stdlib/resolve.vagent): Operates strictly as an AST-to-AST
> manifold transformation."*

`apps/common/xanadu/zigzag/arena_manifold.cpp:1369–1373`:

```cpp
if (displacedIt && displacedIt != from) {
  if (const auto other = shadow(*displacedIt); other.has_value()) {
    setOneSide(*other, dim, dir, noCell);
  }
}
```

`apps/common/xanadu/vagent/resolve.cpp:27–32` (current C++ side-table):

```cpp
struct Scope {
  std::uint64_t id{};
  std::map<std::string, ScopeStamp, std::less<>> bindings;
  std::uint64_t nextOrdinal{0};
};
```

`vortex-design-critique/SKILL.md:193–198`:

> *"Two cells **cannot** both link to the same target cell `T` along `+d` (or both along `-d`); the
> second `ArenaManifold::link` evicts the first! Any many-to-one relationship must either chain
> elements as a linear/circular rank along `d` or link each referrer to its own **clone view cell**
> on `T`'s `+d.clone` rank..."*

**Fix:** Explicitly specify the manifold representation of scopes and resolved symbol references in
`stdlib/resolve.vagent`:

1. **Lexical Scope Stack:** Scopes are represented as cells chained along `+d.stack` (genesis
   dimension, `lineage.hpp:48`), with nested child scopes pointing negward on `-d.stack` to their
   enclosing parent scope.
1. **Scope Symbol Bindings:** Within a scope, bound symbols are chained as records on
   `scope +d.vars` (`records.hpp:20–60`), with each symbol cell carrying its identifier on `+d.name`
   and its declaration node on `+d.entry`.
1. **Resolved AST References:** When an identifier node in the AST resolves to a declaration `Decl`,
   it links to a freshly minted clone view cell on `Decl`'s `+d.clone` rank
   (`Decl +d.clone → clone_1 +d.clone → clone_2...`), linking `clone_i +d.refs → ast_ident_node`.
   Direct many-to-one linking to `Decl` is strictly prohibited.

______________________________________________________________________

### 3. (§1 & §4, lines 49, 140–163) Permascroll provenance and transclusion omitted

**Problem:** Under Nelsonian hypermedia and the store-slice convergence model (`AGENTS.md:105–110`,
`design/store-slice-convergence.md` R6–R8, `user_permascroll.hpp`, `lineage.hpp`), source code is
**never** an untracked raw byte buffer read from POSIX flat files and discarded into RAM. Primedia
exists *only* in the author's permascroll. In `design/vagent-self-hosting-migration.md`:

- The compiler architecture diagram (§1 lines 37–68) depicts source code originating from
  `std:compiler:lexer (VIcon Scanning)` as if it were reading raw flat files.
- The terms "permascroll", "transclusion", "provenance", "hypertime", and "span" are completely
  absent from the migration plan.
- Phase 1 (§4 lines 143–150) specifies `Token{kind, text, line, col}` with integer coordinates
  rather than tracking character spans into permascroll memory or slice EDL (`PrimediaSpan` /
  `GlobalSpan`).

If the compiler does not bind source text to permascroll spans or `LineageStore` documents:

1. All Nelsonian bi-directional links (beams) and transclusion capabilities in `xuzz` are severed:
   an error diagnostic or a compiled fragment cannot be traced back to its origin in the author's
   scroll.
1. Re-genesis and versioning (`LineageStore::seedLineageStore`, `lineage.hpp:97–119`) lose the
   scroll binding (`bindPublishedLocalScroll`), causing artifact-local spans to dead-read `""`.
1. Self-hosting quine verification (Phase 5, lines 220–235) talks of "Binary Images" rather than
   sealed Merkle ops in a `Store`.

**Evidence:**

`design/vagent-self-hosting-migration.md:49`:

```
V_Lex["std:compiler:lexer (VIcon Scanning)"] --> V_Parse["std:compiler:parser (Recursive Descent)"]
```

`design/vagent-self-hosting-migration.md:149–150`:

> *"Returns a cell list of Token records (`Token{kind, text, line, col}`)."*

`AGENTS.md:105–110`:

> *"the permascroll, which is part of the fixture: every store is an edit decision list whose spans
> address that one shared permascroll, so without it every document renders empty."*

`apps/common/xanadu/vagent/lineage.hpp:97–102`:

> *"Construct and seed the lineage store: historyFromSeal over the artifact's sealed ops, then
> setContentSource + bindPublishedLocalScroll(addScroll(from)) ... without the pair, artifact-local
> span names dead-read ""."*

`design/store-slice-convergence.md` R6:

> *"A scalar cell carries both a real span and canonical bits."*

**Fix:** Ground the self-hosting compiler frontend in Nelsonian Store/Permascroll provenance:

1. Specify that source code compiled by `std:compiler:lexer` is accessed either as a character span
   over the author's `UserPermascroll` (`user_permascroll.hpp`) or from a document in a `Store`
   (`LineageStore` or `system://` store).
1. Augment `Token` and R1 AST records so that, in addition to human-readable `"line"` and `"col"`
   diagnostics, they carry the source `Span` (`offset`, `length`) on `node +d.refs` or
   `node +d.span` (`d.role → "source"`), preserving hypertime transclusion provenance.
1. In Phase 5 (§4 lines 220–235), define "Binary Image" as a `LineageStore` containing sealed Merkle
   `SegmentedOpsSpool` segments (`CompactOpNode` array, `seedProgramImage`), verified bit-for-bit
   against the store hash and Merkle root.

______________________________________________________________________

## MAJOR

### 1. (§4 Phase 4, line 208) Goal plan gathering confuses +d.step with clause rank

**Problem:** Line 208 states:

> *"Dynamic candidate plan gathering: hops goal symbol `+d.step`, collects plans matching the
> trigger."*

In the ZigZag 2-degree schema, `+d.step` is the sibling dimension. If a goal symbol is declared, its
`+d.step` link is already its sibling link on whatever declaration rank it was declared! If the goal
symbol uses `+d.step` to link to its plans, two fatal bugs occur:

1. A goal symbol cannot be part of any sibling declaration list without its `+d.step` link colliding
   with the plan link.
1. Even if isolated, the goal symbol itself would be the first element on the `d.step` rank, meaning
   the first hop from the goal symbol is along `+d.step` to `plan_1`, but subsequent plans are
   `plan_1 +d.step → plan_2`. This conflates the goal symbol with a plan on the same rank despite
   being different semantic entities.

In VAgent agent lowering (`lineage.hpp:50, 78`, `vagent-agent-language.md`), plans are attached to
goals via a distinct child/clause dimension (`+d.clause`, `+d.subgoal`, or `+d.grab`), with
candidate plans chaining along `+d.step` (`plan_1 +d.step → plan_2`). Claiming `+d.step` is hopped
from the goal symbol directly conflates rank navigation with parent-child descent.

**Evidence:**

`design/vagent-self-hosting-migration.md:208`:

> *"Dynamic candidate plan gathering: hops goal symbol `+d.step`, collects plans matching the
> trigger."*

`apps/common/xanadu/vagent/lineage.hpp:50, 78`: `d.clause` is in `kGenesisDimensionNames`;
`d.subgoal` is in `kAgentDimensionNames`.

`design/vagent-metacircular-compiler-implementation.md:670–674` (`ZIGZAG-4`): Parent-to-first-child
must use `+d.grab` or an orthogonal dimension; `+d.step` is reserved exclusively for
sibling-to-sibling chaining.

**Fix:** Clarify that the goal symbol anchors candidate plans via `+d.clause` (or `+d.subgoal`), and
candidate plans chain along `+d.step`:
`goalSymbol +d.clause → plan_1 +d.step → plan_2 +d.step → ...`. The mind engine hops `+d.clause` to
find the first candidate, then walks `+d.step`.

______________________________________________________________________

### 2. (§6, lines 307–309) Hallucinated assertRankInvariants API cited as verification

**Problem:** §6 Risk Mitigation Item 2 claims:

> *"All AST and opcode structures minted in `.vagent` are verified with
> `zigzag::assertRankInvariants()` to ensure strict 2-degree bounds (no fan-in collisions on
> `d.step`, `d.scan`, or `d.vars`)."*

The function `zigzag::assertRankInvariants()` does not exist anywhere in the repository (`src/`,
`include/`, `apps/`, `tests/`). The actual invariant verification function in the codebase is
`zigzag::crdt::CrdtEngine::verifyManifoldInvariants(const ArenaManifold &manifold)`
(`apps/common/xanadu/zigzag/crdt_zz.hpp:100–101`), alongside explicit assertions on
`ArenaManifold::link()` return expectations. Citing non-existent C++ verification functions gives a
false assurance of rank invariant safety for newly minted `.vagent` structures.

**Evidence:**

`design/vagent-self-hosting-migration.md:307–308`:

> *"All AST and opcode structures minted in `.vagent` are verified with
> `zigzag::assertRankInvariants()`..."*

`apps/common/xanadu/zigzag/crdt_zz.hpp:100–101`:

```cpp
[[nodiscard]] static bool
verifyManifoldInvariants(const ArenaManifold &manifold) noexcept;
```

Shell search across entire workspace: `grep -rn "assertRankInvariants"` yields 0 matches.

**Fix:** Update §6 Item 2 to cite the actual codebase verification machinery:
`zigzag::crdt::CrdtEngine::verifyManifoldInvariants(arena)` in `crdt_zz.hpp:100–101`, and specify an
in-language validation helper in `stdlib/ast.vagent` (`verify_ast_ranks(root: Cell): Bool`) that
walks the R1 AST checking that every node has degree $\le 1$ in each direction across `d.grab`,
`d.step`, `d.name`, `d.values`, and `d.vars`.

______________________________________________________________________

### 3. (§4 Phase 2, lines 171–177) Foreign serialization of compound types in strings

**Problem:** Track 2.2 specifies compound types (`List<T>`, `Opt<T>`, `Record<...>`, `Code<T>`) for
`stdlib/checker.vagent`. In the existing C++ codebase (`r1_ast.cpp:24–57`, `r1_ast.cpp:148`),
compound types are parsed from and serialized into flat strings (e.g. `"List<Int>"` or
`"Record<x: Int>"`). Standing Rule 2 (`brief-common.md:14–16`,
`vortex-design-critique/SKILL.md:57–59, 199–202`) explicitly mandates:

> *"No Foreign Serialization Inside Cells: Structured records, lists, ASTs, and activation frames
> must be represented as ZigZag cells and links (`d.vars`, `d.values`, `d.elts`, `d.stack`), never
> serialized into strings or hidden in C++ structs when the design claims manifold grounding."*

If the self-hosting type checker continues to serialize compound types as string blobs like
`"List<Record<a: Int, b: Str>>"` inside the text of the `+d.values` type annotation cell, it
violates ZigZag structural purity and forces string parsing during compilation and reflection.

**Evidence:**

`design/vagent-self-hosting-migration.md:173–174`:

> *"Compound types: `List<T>`, `Opt<T>`, `Record<...>`, `Code<T>`."*

`apps/common/xanadu/vagent/r1_ast.cpp:24–57`: `parseTypeString(std::string_view s)` (string parsing
of nested angle brackets and brackets).

`vortex-design-critique/SKILL.md:199–202`:

> *"Structured records, lists, ASTs, and activation frames must be represented as ZigZag cells and
> links (`d.vars`, `d.values`, `d.elts`, `d.stack`), never serialized into strings..."*

**Fix:** Specify that in `.vagent`, compound types are first-class ZigZag cell graphs:

- Base type constructor cell (text = `"List"`, `"Opt"`, `"Record"`, or `"Code"`).
- Type arguments linked posward on `+d.grab` (first type argument) and chained on `+d.step`.
- For `Record`, field names linked on `+d.name` and field types on `+d.values` (conforming to
  `records.hpp`).
- For backward compatibility during migration stages, provide a canonical stringifier only at the
  diagnostic reporting boundary, keeping the in-manifold representation purely structural.

______________________________________________________________________

### 4. (§4 Phase 3, lines 186–193) Fragment packaging omits d.mesh.relocation markers

**Problem:** Track 3.1 states:

> *"Packages fragment objects: creates head, descriptor with parameter list, produced flag cell,
> result cell, and +d.refs records."*

It omits the mandatory requirement from `MeshActorRuntime` and `lineage.hpp:61`
(`design/vagent-metacircular-compiler-implementation.md:695–696`): Every cell reference in `+d.refs`
MUST carry a `d.mesh.relocation → "cell"` marker! Without this marker, `ArenaManifold::promote()`
(`arena_manifold.cpp:1733–2116`) does not rewrite cell references when the fragment is promoted from
the compilation arena into the persistent `LineageStore`. The un-relocated raw cell references
become dangling pointers into dead ephemeral memory upon re-genesis!

**Evidence:**

`design/vagent-self-hosting-migration.md:193`:

> *"Packages fragment objects: creates head, descriptor with parameter list, produced flag cell,
> result cell, and +d.refs records."*

`design/vagent-metacircular-compiler-implementation.md:695–696`:

> *"and the complete `+d.recipe` and `+d.refs` chains (including `d.mesh.relocation → "cell"`
> markers on every ref-record cell) in one host step"*

`apps/common/xanadu/vagent/lineage.hpp:61`: `"d.mesh.relocation"` is in `kMetaDimensionNames`.

**Fix:** Explicitly state in Track 3.1 that each reference record linked on `head +d.refs` must have
`d.mesh.relocation` linked to a cell with text `"cell"` (or leverage `build_fragment` /
`boot_linker` which enforces this invariant), ensuring safe relocation across
`ArenaManifold::promote()`.

______________________________________________________________________

## MINOR

### 1. (§1 & §3, lines 50, 115) stdlib/ast.vagent missing from pipeline diagram

**Problem:** In §3 line 115, `ast.vagent` is listed as a separate stdlib module:
`ast.vagent # std:compiler:ast (AST node kinds, selectors, constructors)`. However, in the Target
Self-Hosting Architecture diagram (§1 lines 49–56), `ast.vagent` is omitted: `V_Parse` directly
outputs `V_AST["R1 AST Manifold"]` and passes to `V_Expand`. Furthermore, in §5 Track breakdown
(lines 278–297), there is no track for `ast.vagent`! It is subsumed into Track A2 without mentioning
who defines the constructor and selector functions.

**Evidence:**

`design/vagent-self-hosting-migration.md:50`:

```
V_Parse --> V_AST["R1 AST Manifold"]
```

`design/vagent-self-hosting-migration.md:115`:

```
├── ast.vagent # std:compiler:ast (AST node kinds, selectors, constructors)
```

**Fix:** Add `stdlib/ast.vagent` to Track A2 as the fundamental schema module defining constructors
(`make_lit_int`, `make_binary`, etc.) and rank walkers (`ast_first_child`, `ast_next_sibling`,
`ast_token`), and show it as a supporting library in the architecture diagram.

______________________________________________________________________

### 2. (§4 Phase 1, line 150) Unspecified manifold layout for Token records

**Problem:** Track 1.1 states:

> *"Returns a cell list of Token records (`Token{kind, text, line, col}`)."*

It does not specify how the cell list or the records are represented in ZigZag space. Under standard
VAgent record conventions (`records.hpp:20–60`), a record has fields on `d.vars` or direct
positional cells on `d.values`. If the token stream is a list of cells, it should be chained along
`+d.step` or `+d.scan`.

**Evidence:**

`design/vagent-self-hosting-migration.md:149–150`:

> *"Returns a cell list of Token records (`Token{kind, text, line, col}`)."*

`apps/common/xanadu/vagent/records.hpp:20–60` (canonical record layout in the manifold).

**Fix:** Specify that `Token` records are linked on `+d.step` (token stream), with each token cell
carrying its token kind text as cell text, `+d.name` pointing to the lexeme string, and source
coordinates as records on `+d.vars` (`"line"`, `"col"`).

______________________________________________________________________

### 3. (§4 Phase 5, lines 220–235) Quine verification omits Merkle canonicalization

**Problem:** Phase 5 describes bit-for-bit identity between Image 2 and Image 3:

> *"Image 2 and Image 3 must be bit-for-bit identical (modulo non-deterministic timestamp salts,
> which are canonicalized). This proves the metacircular compiler is a fixed point."*

In `segmented_ops_spool.hpp` and `store.hpp`, Merkle pieces and segment headers contain timestamps
and cryptographic seeds. To achieve exact bit-for-bit quine convergence in
`vagent_metacircular_quine_test.cpp`, the test harness uses normalized snapshots
(`snapshotFragment`) or canonical genesis timestamps (e.g. `unix_epoch = 0`). The migration plan
should reference the existing `snapshotFragment` pattern in
`tests/xuzz/vagent_metacircular_quine_test.cpp:44–53`.

**Evidence:**

`design/vagent-self-hosting-migration.md:233–234`:

> *"Image 2 and Image 3 must be bit-for-bit identical (modulo non-deterministic timestamp salts,
> which are canonicalized)."*

`tests/xuzz/vagent_metacircular_quine_test.cpp:44–53`: `FragmentStructure snapshotFragment(...)`.

**Fix:** Add an explicit reference to `snapshotFragment` and the normalized Merkle segment
serializer in `vagent_metacircular_quine_test.cpp` to formally define what "canonicalized" means.

______________________________________________________________________

### 4. (§2, lines 76–89) Subsystem feasibility table omits Proposal Register

**Problem:** Standing Rule 6 (`brief-common.md:21–22`, `vortex-design-critique/SKILL.md:67–71`)
mandates:

> *"Every change to existing code must be tracked in the numbered Proposal Register (`P<k>`)."*

The migration plan proposes replacing or modifying ~22,500 lines of existing C++ files in
`apps/common/xanadu/vagent/` without a single Proposal Register entry (`P<k>`). Even though this is
an implementation plan for self-hosting, retirement of C++ modules and any modifications to the
permanent substrate (`bootstrap.cpp`, `lineage.hpp`, `meta.cpp`) must be tracked in the numbered
Proposal Register.

**Evidence:**

`design/vagent-self-hosting-migration.md:76–89` (Subsystem Feasibility Table).

`brief-common.md:21–22`:

> *"Every change to existing code must be tracked in the numbered Proposal Register (`P<k>`)."*

**Fix:** Add a formal Proposal Register (§7) assigning `P<k>` numbers to substrate modifications
(e.g., `P17` for `stdlib/` packaging and C++ parser shims, `P18` for `decls.cpp` retirement, etc.).

______________________________________________________________________

VERDICT: NOT CONVERGED
