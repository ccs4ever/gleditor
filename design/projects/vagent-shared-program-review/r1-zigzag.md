# Round 1 critique: Xanadu / ZigZag Purist (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 3 BLOCKING, 3 MAJOR, 2 MINOR.

## BLOCKING

### 1. (§2.3, line 236) Base-to-overlay linkage & shadowing violates Rule V12

Full title: Base-to-overlay linkage and opcode output slot shadowing violate the 2-degree rank
invariant, cause unbounded overlay growth, and violate Rule V12

**Problem:** The specification proposes that per-agent mutable state overlays
(`zigzag::ArenaManifold`) directly link to or shadow base cells in the frozen `programManifold`:

1. "Opcode `-d.grab` output slots: Base Manifold cell ID, shadowed on write in agent
   ArenaManifold... reset/discarded per turn" (§2.3 Table, line 240).
1. "Per-agent handle roots (`stateH_`, `beliefH_`, ...): Base OpHandle cell IDs, shadowed once in
   agent ArenaManifold" (§2.3 Table, line 241).
1. "Any reference to an unshadowed base cell $b \in [1, N_{\text{prog}}]$ (such as ... a suspended
   plan step's base CellRef)" (§3.2, lines 335–337).

In ZigZag, every link is reciprocal: $A.\text{pos}(d) = B \iff B.\text{neg}(d) = A$. In
`ArenaManifold::link(from, dim, dir, to)`, setting an edge between an overlay cell and a base cell
invokes `shadow(*to)` (`apps/common/xanadu/zigzag/arena_manifold.cpp:1339`). Shadowing copies the
base cell into the agent's private `overlay_` so its reciprocal link `to.neg(d) = from` can be
recorded.

This proposal breaks four fundamental invariants:

1. **Unbounded Overlay Growth:** The design claims opcode output slots are "reset/discarded per
   turn" (§2.3, §3.2). But `ArenaManifold` has no selective unshadowing API. `shadowOrder_` is
   truncated only by `ArenaManifold::release(m)` on turn failure (`arena_manifold.cpp:1668–1671`).
   On turn commit, `discard(m)` is called (`design/vagent-agent-language.md:323`), which releases
   the mark while leaving all shadowed base cells in `overlay_`. Every opcode executed across the
   agent's lifetime remains permanently shadowed in `overlay_`, accumulating an unbounded memory
   leak.
1. **Promoting Program into Checkpoints:** In `arena_manifold.cpp:1757`, `zigzag::promote` skips
   only unshadowed base cells (`!isEphemeral(current) && !from.holdsOwn(current)`). Because shadowed
   base cells have `from.holdsOwn(current) == true`, `promote` traverses and mints new persistent
   `CompactOpNode` operations in the checkpoint store for every shadowed opcode output slot and base
   handle.
1. **Violation of Standing Rule V12:** `design/vagent-agent-language.md:226–233` explicitly
   commands:
   > "V12. No state cell links into the program. A checkpoint promotes everything reachable from the
   > agent cell along any dimension (the comment above `zigzag::promote`), so a link from state into
   > a program cell would drag the compiled program and `home` into every checkpoint. Capacities,
   > plans, skills and behaviors are therefore referred to by qualified name. The program's own
   > output cells are written at run time as scratch, which is how every Vortex opcode works
   > (`VortexVM::step`); their values never outlive a statement unless spilled to an activation
   > frame (§5.8)."
1. **Base-to-Overlay 2-Degree Fan-in:** If multiple agents or multiple intentions link to an
   unshadowed base code cell (e.g. "a suspended plan step's base `CellRef`"), each link attempts to
   write a reciprocal link to that base cell. A base cell cannot have multiple reciprocal links on a
   single dimension/direction without eviction or shadowing.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:236–246` (Table)

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1338–1340`:

  ```cpp
  const auto target =
      (to.has_value() && *to != noCell) ? shadow(*to) : std::nullopt;
  ```

- `apps/common/xanadu/zigzag/arena_manifold.cpp:1757`:

  ```cpp
  if ((current != root && !isEphemeral(current) && !from.holdsOwn(current)) ||
      from.isProxy(current)) {
    continue;
  }
  ```

- `design/vagent-agent-language.md:226–233` (Rule V12)

**Fix:** Enforce strict separation between code and state:

1. Base `programManifold` must hold strictly pure, read-only code, canonical dimensions, and static
   constants. It must not allocate per-agent mutable execution registers (`a0..a4`, `r0..r1`,
   `scratch_`, `pool_`) or handle cells (`stateH_`, `beliefH_`, etc.) in the base.
1. Per-agent handles and registers must be minted in the agent's private `ArenaManifold` overlay at
   instantiation (attached to the agent's execution context or activation frame `d.stack`), leaving
   base cells untouched.
1. In accordance with Rule V12, no state cell (such as an active intention) may link to a base code
   cell. Control flow resume points must be identified symbolically by qualified plan name and
   numeric suspension ordinal $s$, resolved at execution time via the program manifest, never via
   direct manifold links into compiled code.

______________________________________________________________________

### 2. (§3.3, line 359) Dimension overloading & uncloned fan-in in root table

Full title: Dimension overloading of `d.vagent.type` and uncloned fan-in in field representations
violate ZigZag orthogonality and the 2-degree invariant

**Problem:**

1. §3.3 overloads `d.vagent.type` for three mutually contradictory roles:
   - Agent type descriptor: `$agent (d.vagent.type = "agent:<AgentTypeName>@<programRef>")`
   - Field names: field cells, "each carrying its field name on `d.vagent.type` (`Bytes`)"
   - Plan structural hashes: `d.vagent.plan` carrying qualified plan identifier, "plus
     `d.vagent.type` holding the `UInt64` `planGraphHash`"
1. In ZigZag, dimension names are orthogonal coordinate axes with invariant semantics. Overloading
   `d.vagent.type` violates House Style Rule 2 ("Prefer solving with more cells and dimensions over
   bit-packing, C++ side tables, or overloaded dimensions").
1. Worse, the proposed field topology creates uncloned fan-in: if a field cell links along
   `+d.vagent.type` to a name cell $N$ (e.g. `"speed"`), then `F_1.pos(d.vagent.type) = N` and
   `F_2.pos(d.vagent.type) = N`. Because `A.pos(d) = B <=> B.neg(d) = A`, setting
   `F_2.pos(d.vagent.type) = N` forces `N.neg(d.vagent.type) = F_2`, which evicts `F_1`! Two fields
   with the same name (or fields across multiple records/agents) cannot point to the same name cell
   without cloning.
1. The proposal discards the canonical, battle-tested ZigZag field topology specified in
   `design/vagent-agent-language.md` §5.1, §5.2 and `VortexCore::exportSymbol`:
   `$agent ─(+d.vars)─► field1_name ─(+d.vars)─► field2_name`
   `field1_name ─(+d.values)─► field1_val` `field2_name ─(+d.values)─► field2_val` where `d.vars`
   forms the 1D rank of variable/field names, and `d.values` forms the orthogonal 1D rank of values.
   This 2D grid has zero fan-in, zero eviction, and uses existing genesis dimensions.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:359–370`:

  ```markdown
  - `$agent` (`d.vagent.type = "agent:<AgentTypeName>@<programRef>"`)
    - `d.vagent.field` rank: field cells, each carrying its field name on `d.vagent.type` (`Bytes`)
      and its value on the field cell itself (or `d.vagent.entry` for composite values).
    ...
    - `d.vagent.plan`: `Bytes` qualified plan identifier plus `d.vagent.type` holding the `UInt64`
      `planGraphHash`
  ```

- `design/vagent-agent-language.md:477–480, 495–497`:

  ```text
  [agent cell "PatrolDrone"]
    ├─(+d.vars)──► [targets_seen] ─(+d.values)─► [3]
    │                └─(+d.vars)─► [$peers] ─(+d.vars)─► [$timers] …
  ```

**Fix:** Eliminate `d.vagent.field` and cease overloading `d.vagent.type`. Represent agent, skill,
and behavior fields using the standard `d.vars` and `d.values` genesis grid. Store `planGraphHash`
on a dedicated dimension (e.g. `d.hash`) or as an attribute of the suspension table, maintaining
dimension orthogonality.

______________________________________________________________________

### 3. (§2.4, line 278) Discarding `d.clone` and `d.capacity` breaks Nelsonian clone purity

Full title: Discarding the `d.clone` master/instance topology for skills breaks capacity dispatch
and Nelsonian multiplicity rules

**Problem:**

1. In `design/vagent-agent-language.md` §5.1, §5.2, §5.3, installed skills are structured using
   Nelsonian clone topology:
   - A single skill frame cell holds the skill's state fields on `d.vars` and acts as the clone
     master (`findCloneMaster`, `ArenaManifold::cloneMaster`).
   - For every capacity the skill implements (including super-capacities), a distinct instance cell
     is appended to the agent's `d.skill` rank, linked along `+d.capacity` to the capacity's
     qualified name, and linked posward on the skill frame's `+d.clone` rank.
1. In §2.4 and §3.3, `vagent-shared-program-and-live-upgrade.md` discards `d.clone`, `d.capacity`,
   and the clone-master topology entirely. It invents `d.vagent.skill` and links a single cell $S_i$
   per skill directly to `$agent`.
1. This creates a severe structural defect:
   - In `vagent`, a skill can implement multiple capacities (`skill S implements C1, C2`). Under
     §2.4, an agent cannot link $S$ to both $C_1$ and $C_2$ without multi-linking or losing capacity
     dispatch.
   - Dispatching a capacity method requires walking `d.skill`, matching `d.capacity`, and navigating
     along `-d.clone` to the master frame cell to access fields. By flattening this into a single
     cell $S_i$, §2.4 breaks capacity-based dispatch and destroys the Nelsonian principle of clone
     views.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:278–285`:

  ```markdown
  - Each mounted skill is a state cell S_i linked on the d.vagent.skill rank from $agent, with:
    - S_i content (ValueKind::Bytes): qualified skill type name (e.g. "Navigation").
    - d.vagent.field rank from S_i: the skill instance's own field cells...
  ```

- `design/vagent-agent-language.md:518–525`:

  > "Installing a skill mints one skill frame cell, whose content is the skill's qualified name and
  > which holds the skill's fields on `d.vars`, and one instance cell per capacity the skill
  > implements, including capacities those extend. Each instance is appended to the agent's
  > `d.skill` rank, appended posward to the frame's `d.clone` rank, and linked along `+d.capacity`
  > to its own cell holding the capacity's qualified name. The frame is the clone master
  > (`ArenaManifold::cloneMaster` returns the rank's negward end) and is never removed while an
  > instance exists."

**Fix:** Revert the skill topology in §2.4, §3.3, and §4.3.3 to the canonical clone-master structure
defined in `vagent-agent-language.md` §5.3 using `d.skill`, `d.capacity`, `d.clone`, and
`d.vars`/`d.values`.

______________________________________________________________________

## MAJOR

### 1. (§2.1, line 141) Sealed program store omits permascroll transclusion model

Full title: The sealed program store specification omits permascroll span allocation, publication,
and swarm distribution requirements

**Problem:**

1. Under `design/store-slice-convergence.md` R6:
   > "Every persistent cell has a real `PrimediaSpan` in a real scroll. A programmatically created
   > scalar cell's span holds its shortest-round-trip rendering... A Store holds NO primedia of its
   > own. Permascroll is under `$XDG_DATA_HOME` (`user_permascroll.hpp`). A store is handed one,
   > never makes its own."
1. In the xanalogical architecture, an OSMIC `xanadu::Store` is an edit decision list (EDL)
   referencing primedia scrolls (`AUTHORSHIP.tsv`, `GlobalSpan`).
1. §2.1 and Proposal P17.2 specify that `CompiledProgram` compiles a program into a sealed `Store`
   whose segments are hashed to produce `programRef`. However, the design completely omits how
   primedia spans (routine identifiers, string constants, dimension names, reflection text) are
   allocated and sealed:
   - Does `CompiledProgram` allocate a dedicated programmatic scroll, or spool into the author's
     local permascroll?
   - If spans point to the author's local permascroll without being bundled into a distributed
     torrent package (via `publication.hpp` `GlobalSpan` / `historyFromSeal`), remote swarm nodes
     loading `programRef` will have an invalid scroll registry, making text unresolvable
     (`OpsSegmentUnreadable`).

**Evidence:**

- `design/store-slice-convergence.md:238–246` (R6)
- `design/store-slice-convergence.md:264–270` (Ruling on permascroll transclusion)
- `AGENTS.md` ("every store is an edit decision list whose spans address that one shared
  permascroll, so without it every document renders empty")

**Fix:** Explicitly specify the scroll and publication lifecycle in §2.1 and P17.2:
`CompiledProgram` compiles into a store with an associated standalone programmatic scroll; upon
sealing, spans are globalized via `xanadu::publish` and sealed into the package descriptor so that
remote nodes can mount `programManifold` with complete primedia resolution.

______________________________________________________________________

### 2. (§2.2, line 214) Suspension table tuples expressed as unmodeled C++ notation

Full title: Suspension table entries `(resumeEntryCellRef, liveVarLayoutHash)` use informal C++
tuple notation instead of explicit ZigZag cells and ranks

**Problem:**

1. §2.2 specifies: "suspensionPoints: ordered rank on `d.vagent.step` mapping each suspension
   ordinal $s \in \{0, 1, \dots, K-1\}$ ... to `(resumeEntryCellRef, liveVarLayoutHash)`"
1. In ZigZag, every cell has a single value payload (`CompactOpNode::value` / `valueBits`) and a
   `ValueKind`. A cell cannot hold a C++ pair `(resumeEntryCellRef, liveVarLayoutHash)`.
1. Leaving this as a vague C++ tuple notation violates House Style Rule 2 ("Cells and Dimensions
   Over Side Tables or Bit-Packing: State, structure, and metadata live on orthogonal ZigZag ranks
   (`d.*`), never in ad-hoc C++ side tables, JSON/bencode blobs stuffed into scalar cell text...").

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:217–218`
- `design/vagent-shared-program-and-live-upgrade.md:511–513`

**Fix:** Explicitly model the suspension point topology in the program manifold: Each suspension
point is a cell $S$ on the plan's `d.step` rank with value $s$ (`ValueKind::Int64`).

- `S ─(+d.entry)─► resumeEntryOpcodeCell` (or linked on `d.point`)
- `S ─(+d.hash)─► hashCell` holding `liveVarLayoutHash` as `ValueKind::UInt64`
- `S ─(+d.vars)─► liveVarsRankHead` describing the live variable name rank for verification.

______________________________________________________________________

### 3. (§3.3, line 367) Qualified plan delimited strings replace structural ZigZag navigation

Full title: Encoding qualified plan identifiers as delimited strings abandons hyperstructural
navigation in favor of string parsing

**Problem:**

1. In §3.3, an intention's plan attribute is stored as a formatted, delimited string literal:
   `d.vagent.plan`: `Bytes` qualified plan identifier (`"<Owner>::<GoalType>#<Index>/<PlanName>"`).
1. Stuffing compound identifiers into delimited strings forces ad-hoc runtime string splitting and
   regex matching (`::`, `#`, `/`), abandoning hyperstructural navigation.
1. In `vagent-agent-language.md` §5.7, plans are arranged structurally in the program module symbol
   table: `Module ─(+d.vars)─► GoalType ─(+d.clause)─► Plan`.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:367–369`
- `design/vagent-agent-language.md:616–623` (§5.7)

**Fix:** Store the plan reference using structural cells (e.g. goal type cell on `d.goal` and plan
name cell on `d.plan`), or document that the qualified string is strictly an external
display/logging identifier while internal lookup uses the module's `d.vars`/`d.clause` symbol rank.

______________________________________________________________________

## MINOR

### 1. (§2.2, lines 163–176) Mermaid diagram depicts unlabelled fan-out from program root

**Problem:** The diagram in §2.2 shows `PR --> DIMS`, `PR --> MANIFEST`, `PR --> CODE`,
`PR --> PLANS` as four unlabelled branching arrows. In a ZigZag manifold, every link is a
dimensioned edge with degree $\le 1$ in each direction.

**Fix:** Label the edges with their respective dimensions or depict them as an ordered rank along
`+d.First` and `+d.Next`.

______________________________________________________________________

### 2. (§1.1, line 31) Dimension naming disparity (`d.vagent.*` vs canonical `runtime_abi.hpp`)

**Problem:** The document invents prefixed names (`d.vagent.skill`, `d.vagent.behavior`,
`d.vagent.space`, `d.vagent.entry`, `d.vagent.child`) that collide with the canonical 13 dimensions
already implemented in `apps/common/xanadu/vagent/runtime_abi.hpp:16–35` and
`design/vagent-agent-language.md` §5.1 (`d.skill`, `d.behavior`, `d.space`, `d.member`, `d.holon`,
etc.).

**Fix:** Harmonize all dimension names in the specification with
`apps/common/xanadu/vagent/runtime_abi.hpp`.

______________________________________________________________________

VERDICT: NOT CONVERGED
