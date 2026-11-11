# Critique Report — Round 1 — `zigzag` (ZigZag / Xanadu Data-Model Purist)

- **Target Document:** `design/vagent-metacircular-compiler-implementation.md`
- **Companion Documents:** `design/vagent-metacircular-compiler.md`,
  `design/vagent-metaprogramming.md`, `design/vagent-metaprogramming-implementation.md`,
  `apps/common/xanadu/vagent/lineage.hpp`, `apps/common/xanadu/vagent/p16.cpp`
- **Reviewer:** Critic 6 (`zigzag` — ZigZag / Xanadu Data-Model Purist)
- **Date:** 2026-11-07

______________________________________________________________________

## Findings

### [ZIGZAG-1] BLOCKING: join_batch Operands Evict Links via 2-Degree Fan-In

- **Location**: `design/vagent-metacircular-compiler-implementation.md:558-571`

- **Evidence**: The specification for the batched join host verb (`join_batch`) states:

  > "Consumes a rank of operation tuples anchored on `ops +d.step`:
  >
  > - **Write tuple (`tuple` text == `"write"`):** reads target cell via `tuple +d.grab`, value cell
  >   via `tuple +d.values`...
  > - **Link tuple (`tuple` text == `"link"`):** reads `from` via `tuple +d.grab`, `to` via
  >   `tuple +d.values`, `dim` via `tuple +d.ref`, and `dir` (`+1` or `-1`) via `tuple +d.name`..."

  In a strict ZigZag 2-degree manifold (`ArenaManifold`), every cell has at most one posward link
  and at most one negward link per dimension (`arena_manifold.cpp:1359-1378`). When a tuple links
  `tuple +d.grab → from`, the reciprocal edge `from -d.grab → tuple` is asserted by the manifold.

  In any non-trivial compiler batch assembling a fragment DAG (such as the cited 25-cell, 60-link
  example), multiple operations necessarily reference the same target, source, or operand cells
  (e.g., an op cell referenced as `from` across multiple wing connections and control-flow jumps).

  When tuple 1 links `tuple1 +d.grab → cellA`, and tuple 2 subsequently links
  `tuple2 +d.grab → cellA`, `ArenaManifold::link` executes:

  ```cpp
  if (displacedIt && displacedIt != from) {
    if (const auto other = shadow(*displacedIt); other.has_value()) {
      setOneSide(*other, dim, dir, noCell);
    }
  }
  ```

  (`arena_manifold.cpp:1369-1373`). `cellA`'s existing `-d.grab` link to `tuple1` is displaced,
  which immediately and silently clears `tuple1 +d.grab` to `noCell`.

  Consequently, building a batch containing multiple references to the same operand cell silently
  destroys earlier tuples in the batch before `join_batch` is ever invoked!

- **Fix**: Tuples must not link directly to shared operand cells along functional dimensions
  (`d.grab`, `d.values`). Instead, adhere to the established Nelsonian carrier/wrapper convention in
  `vagent` (`meta.cpp:981-987`, `:1348-1353`):

  1. Store `from`, `to`, `dim`, and `target` as scalar `Int64` cell references inside dedicated
     position slots on the tuple (e.g. `tuple` carries a sub-rank of positional argument cells on
     `+d.step`, or dedicated fields), treating cell references as values rather than reciprocal
     manifold graph links.
  1. Alternatively, if links are used, each reference must ride an isolated per-tuple carrier slot
     cell holding `Int64(ref)` marked with `d.mesh.relocation → "cell"` if ephemeral cells span
     across turns.

______________________________________________________________________

### [ZIGZAG-2] BLOCKING: P16 Checkpoint Relocation Blind Spot for `+d.lineage` Carrier Cells

- **Location**: `design/vagent-metacircular-compiler-implementation.md:393-402, 474-477, 764` (and
  `apps/common/xanadu/vagent/p16.cpp:35-43, 155-184`)

- **Evidence**: In `P5` (lines 393-402), the implementation specifies:

  > "store the `sourceHead` reference via a ref-carrier cell on `twinHead +d.lineage` (i.e.
  > `twinHead +d.lineage → carrier`, where `carrier` holds `Int64(sourceHead)` marked with
  > `d.mesh.relocation → "cell"`)"

  However, in `apps/common/xanadu/vagent/p16.cpp:35-43` and lines 155-184,
  `EngineP16Ceremony::markedCells` constructs its relocation inventory exclusively by walking
  `head +d.refs → refRecord +d.step → refRecord` and extracting `refRecord +d.ref`. As
  `p16.cpp:41-43` explicitly notes:

  > "the records are the enumeration the ceremony walks because a lone marker is undiscoverable
  > (U-M7 — no per-cell dimension enumeration)."

  In Stage 0 / Stage 2 (lines 474-477), the implementation updates `EngineP16Ceremony` to include
  heads on `d.kind-registered` in the `heads` list, but does NOT update `markedCells` or
  `genesis.cpp` step `(e)` to traverse `twinHead +d.lineage → carrier`.

  Because `carrier` is never harvested into `markedCells()`, it is never re-kinded to
  `ValueKind::OpHandle` during `onFlagged()`. When `promote()` runs during a P16 checkpoint or
  promote cycle, `carrier`'s `Int64` content is copied verbatim without relocation
  (`arena_manifold.cpp:1810-1823`). After promote/checkpoint, `carrier` contains a stale, dangling
  ephemeral `CellRef` to the prior manifold's slot, corrupting provenance across agent checkpoints.

- **Fix**: EITHER:

  1. Eliminate the `Int64` carrier cell entirely by modeling lineage as a pure native ZigZag rank:
     `sourceHead +d.lineage → twin1 +d.lineage → twin2`. In a native ZigZag rank, each cell has
     degree $\le 2$ on `d.lineage`, no fan-in link eviction occurs, and native ZigZag edges are
     promoted and relocated automatically by `ArenaManifold::promote` without needing `Int64` P16
     re-kinding. Furthermore, this restores `design/vagent-metacircular-compiler.md:555`'s contract
     that `d.lineage` walks enumerate all-time twins.
  1. OR, if the carrier cell holding `Int64(sourceHead)` is retained, `carrier` MUST be chained into
     `twinHead +d.refs` as a standard ref-record, OR `p16.cpp::markedCells` and `genesis.cpp` step
     `(e)` must be explicitly modified to traverse `twinHead +d.lineage → carrier` and include
     `carrier` in the re-kinding and rebind sweeps.

______________________________________________________________________

### [ZIGZAG-3] BLOCKING: Silent Link Eviction on R1 AST Nodes via Shared `d.ast.kinds` Links

- **Location**: `design/vagent-metacircular-compiler-implementation.md:578`

- **Evidence**: Line 578 states:

  > "Each R1 AST node is a cell whose text (or `+d.verb` link to the filed `d.ast.kinds` cell) names
  > its kind..."

  The proposed parenthetical alternative — linking `+d.verb` from the AST node to the filed
  `d.ast.kinds` cell — directly violates the 2-degree ZigZag invariant.

  The vocabulary cell on `d.ast.kinds` (e.g., the cell representing `"ident"` or `"lit.int"`) has
  exactly ONE `-d.verb` link slot. If AST node 1 links `node1 +d.verb → kindIdent`, and AST node 2
  later links `node2 +d.verb → kindIdent`, `node2`'s link will silently overwrite `-d.verb` on
  `kindIdent` and disown `node1 +d.verb` (`arena_manifold.cpp:1369-1373`).

  The first AST node's kind link is severed. Any AST containing more than one node of the same kind
  (i.e. Every non-trivial program AST) becomes silently corrupted during construction.

- **Fix**: Strike the `(or +d.verb link to the filed d.ast.kinds cell)` option entirely. Mandate
  that kind tags on R1 AST cells are stored strictly as text content on the AST cell itself
  (`node.text == "ident"`). Clarify that the `d.ast.kinds` rank on `programRoot` is purely an
  immutable schema/vocabulary catalog, never the target of inbound `-d.verb` edges from AST
  instances.

______________________________________________________________________

### [ZIGZAG-4] MAJOR: Missing Parent-to-Child Link Dimension in R1 AST Tree Invariant

- **Location**: `design/vagent-metacircular-compiler-implementation.md:579-580`

- **Evidence**: Line 579 states:

  > "`+d.name` holds its identifier/operator/literal token, `+d.values` holds its annotated type
  > cell (populated by Stage 6), and `+d.step` chains its child AST nodes in order."

  If `+d.step` is used both to connect parent to children and to chain siblings, a ZigZag cell
  degree collision occurs immediately for any interior child node: A child node $C_1$ that has both
  siblings ($C_2$ via `+d.step`) and children of its own ($D_1$) would require two outbound
  `+d.step` links, which is impossible in a 2-degree manifold. Furthermore, if parent $P$ links to
  $C_1$ via `+d.step`, parent and child boundaries become conflated with sibling sequences on the
  same rank.

- **Fix**: Explicitly specify the two-dimension tree hierarchy convention standard in this codebase
  (matching `fragment.cpp:55-64` and `MetaRuntime::clausesOf`):

  - Parent $P$ enters its child sequence via `P +d.grab → FirstChild`.
  - Siblings chain sequentially via `Child_i +d.step → Child_{i+1}`. This ensures every node has at
    most degree 1 on `d.grab` (as parent) and degree 2 on `d.step` (as sibling in its parent's child
    rank), maintaining strict 2-degree invariants throughout nested expression trees.

______________________________________________________________________

### [ZIGZAG-5] MAJOR: Truncation of `kMetaDimensionNames` and Omission of `d.ast.kinds`

- **Location**: `design/vagent-metacircular-compiler-implementation.md:244-255` (and
  `apps/common/xanadu/vagent/lineage.hpp:58-62`)

- **Evidence**: The document states:

  > "Extend `kMetaDimensionNames` (`apps/common/xanadu/vagent/lineage.hpp:58-62`) from 6 entries to
  > 8 entries by filing `"d.lineage"` (`P5`) and `"d.kind-registered"` (`P4`):
  >
  > ````cpp
  > inline constexpr std::array<std::string_view, 8> kMetaDimensionNames{
  >     "d.code",   "d.refs",    "d.ref",          "d.ref.for",
  >     "d.patch",  "d.patch.head", "d.lineage",  "d.kind-registered",
  > };
  > ```"
  > ````

  This citation and definition are factually inaccurate on the codebase baseline:

  1. `kMetaDimensionNames` in `apps/common/xanadu/vagent/lineage.hpp:58-62` already contains **12**
     entries: `"d.code"`, `"d.patch"`, `"d.weight"`, `"d.synapse"`, `"d.entry"`, `"d.patch-head"`,
     `"d.validate"`, `"d.refs"`, `"d.ref"`, `"d.recipe"`, `"d.refFor"`, `"d.mesh.relocation"`.
  1. Replacing it with the proposed 8-element array drops 6 existing, vital system dimensions:
     `"d.weight"`, `"d.synapse"`, `"d.entry"`, `"d.recipe"`, `"d.validate"`, and
     `"d.mesh.relocation"`. Dropping `"d.mesh.relocation"` directly breaks P16 relocation across
     checkpoints.
  1. The proposed array misnames existing dimensions (`"d.patch.head"` instead of `"d.patch-head"`,
     and `"d.ref.for"` instead of `"d.refFor"`).
  1. Furthermore, Stage 4 introduces `"d.ast.kinds"` (line 573: *"under a filed dimension
     d.ast.kinds"*), but fails to add `"d.ast.kinds"` to `kMetaDimensionNames` anywhere in Stage 0
     or Stage 4. Because `dimensionVocabulary()` (`lineage.cpp:426-446`) and
     `fileDimensionVocabulary()` (`lineage.cpp:448-465`) sweep `kMetaDimensionNames`, any omitted or
     dropped dimension will be absent from the lineage store's persistent dimension rank, forcing
     unsafe ad-hoc session-minting.

- **Fix**: Correct `kMetaDimensionNames` in `apps/common/xanadu/vagent/lineage.hpp` to retain all 12
  existing entries, preserve their exact spellings (`"d.patch-head"`, `"d.refFor"`,
  `"d.mesh.relocation"`), and append all 3 required metacircular dimensions: `"d.lineage"`,
  `"d.kind-registered"`, and `"d.ast.kinds"` (bringing the total to 15 entries). Update lines
  244-255 and Proposal Register `P5` accordingly.

______________________________________________________________________

### [ZIGZAG-6] MAJOR: Ambiguous Pending Registration State Pollutes Kind-Registered Carrier

- **Location**: `design/vagent-metacircular-compiler-implementation.md:428-435, 448-453` (and
  `design/vagent-metacircular-compiler.md:388, 416-418`)

- **Evidence**: Line 432 states:

  > "Link a trailed `"pending"` status marker on `head`'s `d.vars` rank (`$reg_state → "pending"`)
  > and append `head` to a trailed pending rank (or `d.kind-registered` with
  > `$reg_state == "pending"`)." And line 451 states: "unlink $h_i$ from `d.kind-registered`,
  > register none of its ops, and write a `fault:<label>` tombstone on `agentCell`'s `d.vars` rank
  > with the refusal reason"

  This directly contradicts the companion architectural document
  `design/vagent-metacircular-compiler.md:388`:

  > *"The carrier write is the commit act — a drain-faulted head's entry is never appended, so the
  > carrier contains committed heads and nothing else."*

  If pending heads were appended to `d.kind-registered` under live marks:

  1. `d.kind-registered` would be polluted with speculative, unvalidated heads during turn
     execution.
  1. If an actor checkpoint (`p16.cpp`) runs or genesis rebinds, it would sweep uncommitted heads.
  1. Conversely, if a "trailed pending rank" is used instead, the implementation fails to name its
     dimension or root anchor. It cannot use `"d.pending"` because `agentCell +d.pending` is already
     occupied by the event/percept trigger queue (`executor.cpp:125`, `:3236`, `meta.cpp:1153`).
  1. If pending heads are instead held in a C++ `std::vector<CellRef>` on `MetaRuntime`, that
     creates an un-trailed C++ side-table that will NOT be rolled back if the turn or choice point
     backtracks via `failTurn` (`executor.cpp:702`) / `arena.release()`, leading to dangling pointer
     corruption during `drainPendingRegisters()`.

- **Fix**:

  1. Re-affirm the strict invariant from `design/vagent-metacircular-compiler.md`:
     `d.kind-registered` contains ONLY committed heads; pending heads are NEVER linked into
     `d.kind-registered`. Strike "(or `d.kind-registered` with `$reg_state == "pending"`)" and the
     drain-time unlinking of faulted heads from `d.kind-registered`.
  1. For the pending window within a turn, define a clean, trailed manifold rank anchored on
     `agentCell`: file dimension `"d.pending-registration"` (or link a dedicated pending container
     cell to `agentCell +d.vars` under key `"$pending_registers"`), chaining pending heads along
     `+d.step`. Because links in the arena are trailed by `ArenaManifold::trail`, this rank
     automatically vanishes on `release()` if the turn aborts, and provides an authoritative
     manifold-resident queue for `drainPendingRegisters()` to drain without any out-of-band C++
     side-tables.

______________________________________________________________________

### [ZIGZAG-7] MINOR: Silent Disowning on Repeated `registerHead` Call via 2-Degree Fan-In

- **Location**: `design/vagent-metacircular-compiler-implementation.md:415, 425`
- **Evidence**: Line 415 states:
  `unless head itself is already the registered holder, in which case registerHead is idempotent`.
  Line 425 states:
  `append head to agentCell's +d.kind-registered rank (binds_.dims.kindRegistered)`. If `head` is
  already present in `d.kind-registered` (or if `head` already has an inbound `-d.kind-registered`
  link from an earlier registration), attempting to append `head` to `rankTail` will call
  `arena.link(tail, kindRegistered, Posward, head)`. Because `head` already has a predecessor on
  `d.kind-registered`, `arena.link` will displace `head`'s existing predecessor, breaking the rank
  into a disconnected cycle or dangling fragment.
- **Fix**: In `registerHead`, check
  `arena.rankContains(agentCell, head, binds_.dims.kindRegistered)` (mirroring `codeRegistered` at
  `meta.cpp:1834`). If `head` is already in the rank, do not call `arena.link`; return idempotently
  immediately.

______________________________________________________________________

### [ZIGZAG-8] MINOR: Type Annotation Fan-In on Shared R1 AST Type Cells

- **Location**: `design/vagent-metacircular-compiler-implementation.md:579, 661-662`
- **Evidence**: Line 579 states: "`+d.values` holds its annotated type cell (populated by Stage 6)".
  If Stage 6 creates shared canonical type cells (e.g. A single cell representing `Int` or `Bool`)
  and links each AST node to that type cell via `+d.values`, the 2-degree invariant on `-d.values`
  of the type cell will be violated, causing silent link eviction of earlier AST node type
  annotations.
- **Fix**: Clarify that Stage 6 type annotation must either:
  1. Write the type name string directly into the node's own type slot cell (minted per AST node,
     `node +d.values → slotCell("Int")`), or
  1. Maintain type cells as dedicated unshared descriptors per AST node.

______________________________________________________________________

SUMMARY: BLOCKING=3 MAJOR=3 MINOR=2 NIT=0

VERDICT: NEEDS-REVISION
