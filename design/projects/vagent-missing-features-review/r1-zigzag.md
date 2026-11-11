# Round 1 critique: Xanadu / ZigZag Purist (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 4 MINOR.

## BLOCKING

### 1. (§4.2, lines 314–366) Cross-Arena Ephemeral CellRef Leak in Swarm Coordinator

Full title: Cross-Arena Ephemeral CellRef Leak Violates Mesh Isolation and Promoted Subgraph Walks

**Problem:** Section 4.2 specifies `agent CompilerCoordinator` and its worker swarm exchanging naked
ephemeral `CellRef`s (`ast_root`, `image_root`, `tokens`) across actor boundaries via asynchronous
performatives (`achieve`, `tell`). In `MeshActorRuntime`, each actor executes in an isolated
`WorkerContext` owning a private `ArenaManifold`. Ephemeral `CellRef`s carry `ephemeralBit`
(`0x80000000`) and index that specific worker's arena slots. When `Coord` receives `image_root` from
`EmitterWorker` and executes `let version = std:zigzag:promote(image_root)`, `zigzag::promote` in
`arena_manifold.hpp:924` checks `from.isCell(root)` and aborts immediately ("return nothing, having
written nothing... when root is not a cell of from"). `Coord`'s arena does not contain `image_root`.
Furthermore, `CheckerWorker` and `EmitterWorker` reading `ast_root` from `ParserWorker`'s arena
dereference out-of-bounds or unrelated slots in their own arenas, causing silent data corruption or
engine traps. In Nelsonian hypermedia, ephemeral structures cannot be shared across sovereign actors
without either single-turn local execution or cryptographic microversion sealing and transclusion.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:318–324`:

  ```text
  Parse-->>Coord: tell AstConstructed(ast_root)
  Coord->>Check: achieve ValidateAst(ast_root) by CheckerAgent
  Emit-->>Coord: tell ImageLowered(image_root)
  Coord->>Coord: let ver = std:zigzag:promote(image_root)
  ```

- `design/vagent-metacircular-missing-features.md:361–365`:

  ```vagent
  plan for HandleImage(image_root, job_id) {
    // Commit the completed image to the store via Arena Promotion
    let version = std:zigzag:promote(image_root)
    std:io:print("Compilation succeeded. Sealed microversion: " ++ version)
  }
  ```

- `apps/common/xanadu/zigzag/arena_manifold.hpp:898–924`: "The one road from the ephemeral side to
  the persistent one, and it works by *mapping* refs rather than by being trusted with them: every
  arena ref is ephemeralBit-tagged... @return nothing, having written nothing, when the reachable
  subgraph exceeds @p budget, or when @p root is not a cell of @p from."

- `apps/common/xanadu/identity/identity_layout.hpp:315–330`: `struct ActorMessage` envelopes carry
  string values and hashes (`programRef`, `value`, `infoHash`), never raw in-memory `CellRef`
  pointers.

**Fix:** In distributed swarms, sovereign actors cannot pass ephemeral `CellRef`s across message
boundaries. Update §4.2, the sequence diagram, and `CompilerCoordinator` to follow either of two
valid architectures:

1. For batch compilation, mandate Pattern A (`behavior StagedCompiler`), where all passes execute
   synchronously within the same turn, `WorkerContext`, and `ArenaManifold`.
1. For distributed swarms (Pattern B), each phase that constructs structure (`ParserWorker`,
   `EmitterWorker`) must promote its own output into a local store microversion (`MicroversionId`)
   before replying with a message naming that immutable version: `tell AstConstructed(ast_version)`.
   Downstream workers adopt the microversion into their lineage. Alternatively, workers must stream
   source text / token strings across the wire rather than ephemeral cell handles.

______________________________________________________________________

### 2. (§3.1 & §3.3, lines 173, 237–239) Active 2-Degree Eviction Bug in Cited zzCloneToChain

Full title: Active 2-Degree Eviction Bug in Cited VortexStdLib::zzCloneToChain Implementation

**Problem:** Section 3.1 and 3.3 cite `clone_to_chain` (`VortexStdLib::zzCloneToChain`,
`vortex_stdlib.cpp:5087–5099`) as the established mechanism for upholding the strict 2-degree rank
invariant for many-to-one references: "- Clone ranks: Minting clone view cells along `+d.clone` via
`clone_to_chain`, where each reference cell has its own unique Crum identity while sharing the
master's semantic target." However, an audit of
`apps/common/xanadu/vortex/vortex_stdlib.cpp:5087–5099` reveals an active 2-degree link eviction
defect:

```cpp
CellRef clone = core_.arena().makeCell();
zigzag::expectWritten(
    core_.arena().link(symbolOp, core_.dims().clone, DimVector::POS, clone));
zigzag::expectWritten(
    core_.arena().link(targetCell, core_.dims().spin, DimVector::POS, clone));
return clone;
```

`zzCloneToChain` links `symbolOp` directly along `+d.clone` to `clone`. In ZigZag, a cell can have
at most one positive neighbor along any dimension. If a second clone is minted for `symbolOp`,
`core_.arena().link(symbolOp, ..., clone)` overwrites `symbolOp`'s `+d.clone` link, severing the
first clone from the master! Similarly, `targetCell.pos(d.spin)` is overwritten on subsequent calls,
evicting prior targets. In ZigZag (`zzcore.hpp:65–68`, `vlog.cpp:45, 93`), a clone family forms a
linear rank along `d.clone` where negward traversal leads to the master:
`cloneMaster(ref, d.clone) == rankEnd(ref, d.clone, NEG)`. New clones MUST be appended posward at
`rankEnd(symbolOp, cloneDim, POS)`. Relying on `zzCloneToChain` as-is causes silent topological
corruption and link loss whenever an AST identifier or type symbol has more than one reference.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:173`: Table 3.1: `clone_to_chain` via
  `#ZZ_CLONE_CHAIN` (`zzCloneToChain`, `L5087`).
- `design/vagent-metacircular-missing-features.md:237–239`: "- Clone ranks: Minting clone view cells
  along `+d.clone` via `clone_to_chain`, where each reference cell has its own unique Crum identity
  while sharing the master's semantic target."
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:5093–5098`:
  `core_.arena().link(symbolOp, core_.dims().clone, DimVector::POS, clone)` directly clobbers
  `symbolOp`'s existing positive link.
- `apps/common/xanadu/zigzag/zzcore.hpp:65–68`: "In ZigZag, a `d.clone` rank links a master cell to
  all its clones. Following negward links on `d.clone` leads to the head of the rank, which is the
  Master Cell."
- `apps/common/xanadu/zigzag/vlog.cpp:45, 93`: Appends to clone ranks using
  `endOfRank(..., clone, DimVector::POS)`.

**Fix:** Amend Proposal P27 to explicitly include fixing `VortexStdLib::zzCloneToChain` in
`apps/common/xanadu/vortex/vortex_stdlib.cpp:5087`. The implementation must find the posward tail of
`symbolOp`'s `d.clone` rank (`rankEnd(symbolOp, core_.dims().clone, DimVector::POS)`) and link the
new clone to that tail, and similarly ensure `targetCell`'s referrer rank along `d.spin` does not
overwrite prior links.

______________________________________________________________________

### 3. (§3.2 & §7, lines 215–219, 500) Under-Specified promote Skips d.dims and Relocations

Full title: Under-Specified std:zigzag:promote Primitive Skips d.dims and Relocation Restatement

**Problem:** Section 3.2 defines `promote(root: CellRef) -> string` as executing
`zigzag::promote(store, parentVersion, arena, root)` (`arena_manifold.hpp:930–934`). This naive call
will fail to produce a usable program image or valid store microversion:

1. `promote()` skips the `d.dims` rank by design (`arena_manifold.hpp:917–922`). Per R12 in
   `design/store-slice-convergence.md` and `lineage.cpp:322`, `extraRoots` must explicitly include
   `d.dims` (`if (dimsDim != noCell) extra.push_back(dimsDim);`). Without this, the promoted store
   has no filed dimension metadata, causing `Store::rebuildManifold()` to produce orphan twins.
1. Emitted opcode graphs store jump targets, term refs, and record refs as Int64 scalar cells with
   `d.mesh.relocation -> "cell"` markers (`records.hpp:54–63`). As documented in
   `lineage.cpp:327–349`, all relocation targets must be harvested into `extraRoots`, and
   post-promotion, `restateScopeArms` and `restateRefSlots` must rewrite the Int64 slots using
   `promoted->cells` and `promoted->arena`. Without this restatement, all jumps and op references
   point to stale ephemeral indices.
1. The design does not specify where `VortexStdLib` gets the mutable `xanadu::Store &` or
   `parentVersion`. `VortexCore` and `WorkerContext` own an `ArenaManifold`, not a `Store`.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:215–219`: `promote(root: CellRef) -> string`:
  Executes `zigzag::promote(store, parentVersion, arena, root) ...`
- `apps/common/xanadu/vagent/lineage.cpp:315–349`: "Root = the program root, once... extraRoots
  carries the d.dims dim before anything else discovered: R12's fiat reads home and dimsDim off the
  band's first two positions, and the d.dims rank is the one rank promote's discovery walk skips
  outright... The image record's 'ops' enumeration holds Int64 arena positions... They join
  extraRoots... restateScopeArms... restateRefSlots..."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:930–934`:
  `promote(Store &store, const MicroversionId &parent, const ArenaManifold &from, ...)`

**Fix:** In §3.2 and Proposal P27:

1. Specify how the ambient `Store &` is bound in `WorkerContext` / `VortexStdLib`.
1. Specify that `std:zigzag:promote` on a program image invokes the full image promotion pipeline
   (passing `d.dims` and harvested relocation targets via `extraRoots`, followed by restating scope
   arms and ref slots as implemented in `lineage.cpp:315–355`).
1. Update P27 to touch `apps/common/xanadu/vagent/lineage.hpp/.cpp` and `vortex_stdlib.cpp`.

______________________________________________________________________

## MAJOR

### 4. (§6.1 & §6.3, lines 430–446, 483–490) Undefined Dimension Topology for Pattern Matching

Full title: Undefined ZigZag Dimension Topology for Match Expressions and Constructor Destructuring

**Problem:** Section 6 introduces `match` expressions with constructor patterns (`CtorPattern`) and
tuple patterns (`TuplePattern`), but fails to define their ZigZag dimension topology. Section 6.3
states: "- Constructor tag checks compile to `VortexVM` `#Equal` comparisons against the Crum value.

- Tuple element access compiles to `#ZZ_STEP` hops along `+d.step` or array indexing." Casually
  offering "or array indexing" violates House Style Rule 2 ("Cells and Dimensions Over Side Tables
  or Bit-Packing") and contradicts Section 8.4 ("Refused: AST Encoding via JSON/Bencode Blobs in
  Scalar Cells"). Furthermore, for `CtorPattern`, the spec never defines how constructor tags link
  to child fields. In the 2-degree R1 AST manifold (`apps/common/xanadu/vagent/r1_ast.hpp:27–32`),
  child nodes link via `+d.grab` and sibling ranks on `+d.step`. The specification must explicitly
  map pattern destructuring onto these dimensions to prevent implementers from introducing ad-hoc
  C++ arrays or fan-in links.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:484–486`: "- Constructor tag checks compile to
  `VortexVM` `#Equal` comparisons against the Crum value.
  - Tuple element access compiles to `#ZZ_STEP` hops along `+d.step` or array indexing."
- `apps/common/xanadu/vagent/r1_ast.hpp:27–32`: "Marshals a parsed VAgent `Expr` into the
  2-degree-safe R1 AST cell schema: Node cell text = canonical kind tag... node +d.name -> text...
  node +d.grab -> first child, siblings chained on +d.step..."

**Fix:** Remove "or array indexing" from §6.3. Explicitly state the ZigZag dimension topology:

- Tuples are linear cell ranks along `+d.step` (or `+d.elts`).
- Constructor pattern matching on AST nodes inspects cell text (the kind tag) and destructures
  arguments by following `+d.grab` to the head child and traversing subsequent children along
  `+d.step`.

______________________________________________________________________

### 5. (§3.2 & §8.3, lines 200, 215, 528–532) Violation of No-Sentinels Rule in promote Signature

Full title: Violation of No-Sentinels Rule in Surface Signature of std:zigzag:promote

**Problem:** Section 3.2 declares the surface API signature as: `promote(root: CellRef) -> string`
In `arena_manifold.hpp:930`, `zigzag::promote` is fallible and returns `std::optional<Promoted>`.
Promotion fails if `root` is not an arena cell, if budget is exceeded, or if relocation cycles
occur. A return type of bare `string` forces failure to be represented as an empty string `""` (a
forbidden sentinel value) or an uncatchable runtime trap. This directly violates House Style Rule 3
("No Sentinels in APIs") and contradicts Section 8.3 ("Refused: Sentinel Integer Values in Surface
APIs").

**Evidence:**

- `design/vagent-metacircular-missing-features.md:200, 215`: `+promote(root) string`
  `promote(root: CellRef) -> string`
- `design/vagent-metacircular-missing-features.md:528–532`: "Refused: Sentinel Integer Values in
  Surface APIs... Strictly prohibited by AGENTS.md ('No sentinels outside the wire and on-disk
  formats'). All fallible operations return Result[T, E] or Option[T]."
- `apps/common/xanadu/zigzag/arena_manifold.hpp:930`:
  `[[nodiscard]] std::optional<Promoted> promote(...)`

**Fix:** Update the surface API signature in §3.2 and §8.3 to:
`def promote(root: CellRef, budget: Option[Int] = none) -> Result[string, PromoteError]` where
`PromoteError` provides typed diagnostic failure modes (`BudgetExceeded`, `InvalidRoot`,
`RelocationCycle`).

______________________________________________________________________

### 6. (§5.1 & §5.2, lines 382–415) Conflation of ActorGenesis and ActorLineage in spawn_image

Full title: Conflation of ActorGenesis and ActorLineage in spawn_image Operational Lifecycle

**Problem:** Section 5.2 Step 4 and diagram lines 397–398 state that `MeshActorRuntime::spawn()` is
invoked "with `ActorGenesis`". This contradicts `apps/common/xanadu/vortex/mesh_actor.hpp:211–225`,
where `MeshActorRuntime::spawn()` takes `const ActorLineage *lineage`. `ActorGenesis` is the
internal parameter passed to `GenesisHook` during context setup (`mesh_actor.hpp:128, 176`).
Moreover, the specification does not declare that `ActorGrant.programRef` must be sealed with the
promoted `MicroversionId`, leaving the actor's code unanchored cryptographically.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:397–398`:
  `Rebuild --> Genesis["4. MeshActorRuntime::spawn() with ActorGenesis"]`
- `apps/common/xanadu/vortex/mesh_actor.hpp:211–225`:
  `spawn(identity, request, wallNow, grant, const ActorLineage *lineage = nullptr)`
- `apps/common/xanadu/vortex/mesh_actor.hpp:119–122`:
  `struct ActorLineage { const Manifold *base{nullptr}; const SpanReader *reader{nullptr}; };`

**Fix:** Update §5.2 text and diagram to show that `MeshActorRuntime::spawn()` takes `ActorLineage`
constructed from `Store::rebuildManifold(version)` and the lineage store reader, and specify that
`ActorGrant::programRef` is set to `version`.

______________________________________________________________________

## MINOR

### 7. (§1, line 19) Missing Cross-Reference to R1 AST Schema Header

Mentioning "R1 AST manifolds" in §1 should reference `apps/common/xanadu/vagent/r1_ast.hpp:27–32`
where the canonical 2-degree R1 schema (`d.name`, `d.grab`, `d.step`, `d.values`, `d.vars`) is
defined.

### 8. (§4.2, lines 335–340) Syntax Inconsistency in Message Pattern Matching

In `CompilerCoordinator`, line 335 writes `on CompileRequest as req` with field access `req.path`,
whereas §6 introduces pattern matching. Align syntax with `on CompileRequest(path)`.

### 9. (§7, line 499) Fuel Accounting Specifics for Ingestion in P26

Proposal P26 mentions `consumeWork` but does not specify how `ActorGrant::maxOutputBytes` bounds are
checked inside `VortexStdLib::ioReadFile`. Clarify the host capability gate check.

### 10. (§9, line 548) Open Question #2 Budget Parameter Omitted in §3.2 Signature

Open Question #2 agrees that `promote` should accept an explicit `budget` parameter, but §3.2's
signature was not updated to accept it. Add `budget: Option[Int] = none` to `promote`.

______________________________________________________________________

VERDICT: NOT CONVERGED
