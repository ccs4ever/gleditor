# Round 1 Disposition: vagent Extended Capabilities Review

**Target Document**: `design/vagent-extended-capabilities-and-belief-vlog-unification.md`\
**Date**: 2026-10-25\
**Orchestrator**: Lead Architect & Antigravity

This document records the disposition of all findings raised across the six Round 1 critiques:

- `r1-writer.md` (1 BLOCKING, 4 MAJOR, 3 MINOR)
- `r1-vortex.md` (2 BLOCKING, 5 MAJOR, 2 MINOR)
- `r1-codebase.md` (3 BLOCKING, 5 MAJOR, 3 MINOR)
- `r1-architect.md` (2 BLOCKING, 3 MAJOR, 3 MINOR)
- `r1-ergo.md` (2 BLOCKING, 5 MAJOR, 3 MINOR)
- `r1-zigzag.md` (3 BLOCKING, 3 MAJOR, 2 MINOR)

______________________________________________________________________

## 1. Cross-Cutting Core Architectural Rulings

### Ruling 1.1: Rule V12 Partitioning of Facts and Horn Rules

- **Critics**: `zigzag` (B1), `architect` (B1), `codebase` (M1).
- **Finding**: Interleaving mutable overlay facts and immutable base Horn rules on a single
  `d.clause` rank violates Rule V12. Linking an overlay fact to an immutable base rule invokes
  `ArenaManifold::shadow(*to)`, causing COW shadowing on shared base cells and breaking P17 live
  upgrades.
- **Disposition**: **ACCEPTED**.
  - Base Horn rules declared in the program reside in the immutable `programManifold` on the
    predicate symbol's `+d.clause` rank.
  - Dynamic assertional facts asserted during actor execution reside in the actor's ephemeral
    `ArenaManifold` overlay, headed by an overlay predicate descriptor cell.
  - The solver (`solveQueryHelper` / query driver) evaluates overlay assertional facts first, and if
    unsatisfied, falls back to the base `programManifold`'s Horn rule rank. No mutable overlay cell
    ever links directly along `d.clause` to a base cell, eliminating COW shadowing.

### Ruling 1.2: SLD Resolution Location & Clause Freshening

- **Critics**: `architect` (B1), `codebase` (B1), `vortex` (B1), `zigzag` (B3).
- **Finding**: Proposal P18.2 attributes the SLD resolution engine to `zigzag/vlog.{hpp,cpp}`, which
  contains only low-level term unification (`zigzag::Vlog::unify`). The actual SLD resolution engine
  lives in `apps/common/xanadu/vortex/vortex_stdlib.cpp:289–1420` (`solveQueryHelper`). Furthermore,
  resolving Horn rules directly without clause freshening binds static variables in-place.
- **Disposition**: **ACCEPTED**.
  - P18.2 explicitly includes `apps/common/xanadu/vortex/vortex_stdlib.{hpp,cpp}` and
    `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`.
  - Clause freshening is formally specified: before unifying a rule head with a query goal, rule
    variable terms are instantiated into fresh variable cells on the query activation frame (or via
    `solveQueryHelper`'s term copying pipeline), preserving the immutability of base rule cells.
  - Body goals form an instruction stream chained along `+d.spin` (Vlog §6.1, `vprolog`), not
    `+d.step`.

### Ruling 1.3: String Scanning API Contract & Composability

- **Critics**: `writer` (B1), `ergo` (B1).
- **Finding**: `find` was specified as advancing `&pos` on produce, destroying composition with
  `tab(find(...))` where $|\text{oldPos} - i| = 0$, yielding empty string. `match`, `any`, and
  `many` were specified returning `Str` instead of `Int` positions, contradicting
  `compiler.cpp:2501` (`Ty::K::Int`) and Icon/vicon standards.
- **Disposition**: **ACCEPTED**.
  - In accordance with vicon §6 and Icon:
    - **Position Generators / Functions** (`find`, `upto`, `any`, `many`, `match`): return target
      **1-based integer position indices** (`Int`) and do **NOT** advance `&pos`.
    - **Movement Functions** (`tab`, `move`): take an integer target position or offset, advance
      `&pos`, and return the traversed **substring** (`Str`).
    - Thus, `tab(find("foo"))` evaluates `find("foo")` yielding integer index $k$, and `tab(k)`
      moves `&pos` to $k$ and returns the traversed text.
    - Fallible methods return `std::optional<Int>` / `std::optional<CellRef>`, strictly adhering to
      the sentinel ban.

### Ruling 1.4: Inverted Arena Trail Formula & Choice-Point Isolation

- **Critics**: `vortex` (B2), `codebase` (B3).
- **Finding**: §2.8 stated
  $\forall t \in \text{QueryTrail}, t.\mathit{dense} \ge m_0.\mathit{cellCount}$. This is
  mathematically inverted: in `ArenaManifold::trail` (`arena_manifold.cpp:1530`), writes to cells
  with `dense >= floor.cells` are omitted from the trail; trailed cells are precisely those with
  $\mathit{dense} < \mathit{floor.cells}$.
- **Disposition**: **ACCEPTED**.
  - Formula corrected to:
    $\forall t \in \text{QueryTrail}, t.\mathit{dense} < m_0.\mathit{cellCount}$.
  - The isolation barrier ensures that backtrack unwinding stops at $m_0$ and choice depth $d_0$,
    leaving actor variable mutations made before $m_0$ untouched.

### Ruling 1.5: 1D ZigZag Rank Excision & Limbo Compaction

- **Critics**: `zigzag` (B2), `architect` (M1).
- **Finding**: Retracting clauses without link repair severs 1D ZigZag ranks. Unlinked `CellSlot`
  records in `cells_` are not reclaimed by CSR compaction.
- **Disposition**: **ACCEPTED**.
  - Retraction of clause $C$ or source peer $S$ explicitly executes standard 1D rank excision:
    `prev = link(C, -d); next = link(C, +d); link(prev, +d, next); link(next, -d, prev); unlink(C);`
  - Reclaimed cell IDs are returned to the actor's ephemeral cell free-list or recycled on the turn
    boundary.

### Ruling 1.6: Inter-Agent Messaging vs. Local Assertions

- **Critics**: `ergo` (M1).
- **Finding**: `tell S P(...)` in §2.4 was specified as an internal belief mutation setting
  `source = S`, conflating outgoing message delivery with local belief assertion and allowing
  unilateral provenance forgery.
- **Disposition**: **ACCEPTED**.
  - `tell` is exclusively an outgoing performative staged to `d.outbox`.
  - Local assertions with provenance use `assert P(...) from S` (e.g., when recording sensor
    readings or handling an incoming `tell` percept).

### Ruling 1.7: Scanning Cursor Reentrancy & Nested Scans

- **Critics**: `zigzag` (M3), `architect` (B2).
- **Finding**: Anchoring `&pos` to global `home` on `+d.scan` breaks nested scans
  (`s1 ? (s2 ? ...)`) and multi-agent reentrancy in `programManifold`.
- **Disposition**: **ACCEPTED**.
  - The scanning cursor cell is allocated on the activation frame / expression stack, linked locally
    to the scan context node rather than global `home`. `std:goal` routines receive the cursor as an
    activation argument.

### Ruling 1.8: Turn Yielding & Native Skill Status Realities

- **Critics**: `writer` (M3), `codebase` (M2), `vortex` (M2).
- **Finding**: Turn yielding in plans was specified citing `#Return` instead of jumping to `haltOp_`
  (`#Halt` / `rtSettle_`). The document also claimed native skill execution was "unbuilt/stubbed",
  ignoring existing implementations in `compiler.cpp:3950–4052` and `agent_executor.cpp:2525–2731`.
- **Disposition**: **ACCEPTED**.
  - Corrected bytecode citation: plans suspend by jumping to `haltOp_` to trigger turn settlement.
  - Document status accurately acknowledges existing runtime dispatch in `agent_executor.cpp` and
    focuses P18.6 on missing AST lowering, compiler integration, and preemption binding.

______________________________________________________________________

## 2. Summary of Revisions by Section

- **§1**: Added Standing House Style sections (Refused Alternatives, Open Questions table
  `U-B1`–`U-B4`).
- **§2.1 & §2.4**: Partitioned `d.clause` ranks into base rules (`programManifold`) and overlay
  facts (`ArenaManifold`), eliminating Rule V12 COW shadowing. Specified 1D rank excision on
  retraction.
- **§2.2**: Replaced `zigzag::vlog::splice` with `zigzag::Vlog::bind`.
- **§2.5**: Relocated SLD resolution engine to `vortex_stdlib.cpp:289–1420` (`solveQueryHelper`),
  specified clause freshening, and corrected body goal rank to `+d.spin`.
- **§2.6**: Expanded EBNF grammar to support `from Postfix` in `for believes` loops and `findall`.
  Disentangled `tell` (messaging) from local assertion (`assert ... from S`).
- **§2.8**: Inverted trail safety formula to $\mathit{dense} < m_0.\mathit{cellCount}$.
- **§3.2–§3.5**: Fixed scanning function signatures (generators return `Int` positions; movement
  functions return `Str` and advance `&pos`). Scoped cursor cells to activation frames.
- **§4.3–§4.5**: Corrected turn yielding bytecode to `jump(haltOp_)` / settlement. Accurately
  reflected existing `agent_executor.cpp` effect machinery.
- **§5 (Proposal Register)**: Updated P18.1–P18.6 with accurate file impacts
  (`vortex_stdlib.{hpp,cpp}`, `runtime_abi.hpp` adding `d.scan`), prices, and invariants.
- **§6**: Updated Acceptance Verification Battery and Status Matrix.
