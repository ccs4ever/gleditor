# Round 3 critique: Xanadu / ZigZag Purist (`vagent-shared-program-and-live-upgrade.md`)

Paths below are relative to the repo root. Tally: 0 BLOCKING, 1 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.B1 (Skill master-to-clone chain in base manifold): RESOLVED — Skill and behavior instances now
  reside exclusively in the ephemeral overlay on `d.vars`/`d.values`, linked to `$agent` on
  `d.skill` (§2.2, §2.3 item 1). The base manifold holds only immutable capacity and method
  declarations, preventing multi-agent fan-in and preserving Rule V12.
- R2.M1 (Inward overlay-to-base links violate Rule V12): OPEN — §2.3 item 2 was accepted in R2
  disposition but retained verbatim in the text. It permits overlay-to-base links and misstates
  `ArenaManifold` reflection semantics.
- R2.M2 (Dimension conflation in program root diagram): RESOLVED — The §2.2 Mermaid diagram was
  fully updated to use §5.7 canonical ranks (`d.vars`, `d.values`, `d.clause`, `d.spin`, `d.step`),
  and `d.plan` has been completely eliminated.

## BLOCKING

None.

## MAJOR

### 1. (§2.3, line 236) Inward overlay-to-base links violate Rule V12 and trigger COW

Full title: Permitting inward overlay-to-base links contradicts Rule V12, misstates `ArenaManifold`
reflection semantics, triggers copy-on-write shadowing of base cells, and pollutes state
checkpoints.

**Problem:** §2.3 item 2 states:

> "2. **Overlay-to-Base Links Are Inward-Directed Only:** An overlay cell may link to a base cell
> along any dimension (`overlay.pos(d) = base`). In `ArenaManifold`, reading `neg(d)` from `base`
> through the overlay reflects the overlay cell via `overlay_` lookup without mutating the frozen
> base."

This statement is factually incorrect regarding `ArenaManifold` and directly violates the
repository's Nelsonian invariants:

1. **`ArenaManifold` Implementation Triggers COW Shadowing:** In
   `ArenaManifold::link(from, dim, dir, to)`
   (`apps/common/xanadu/zigzag/arena_manifold.cpp:1338–1340`), targeting a base cell unconditionally
   executes:

   ```cpp
   const auto target =
       (to.has_value() && *to != noCell) ? shadow(*to) : std::nullopt;
   ```

   `shadow(*to)` (`arena_manifold.cpp:1464–1490`) allocates a new dense slot in the overlay's
   `slots_`, deep-copies the base cell, and records an entry in `overlay_` and `shadowOrder_`. It
   does *not* perform a lightweight, zero-copy reflection. If multiple agents link to the same base
   declaration, each agent duplicates that base cell into its private overlay memory, defeating the
   memory savings of the shared base manifold.

1. **Direct Violation of Rule V12:** Rule V12 (`design/vagent-agent-language.md:226–233`)
   categorically mandates: *"No state cell links into the program."*

1. **Checkpoint Contamination:** In §3.1, state checkpoints serialize all cells reachable from the
   agent's ephemeral state roots (`stateH_`, active frames, etc.). If an overlay state cell links to
   a base program cell along any dimension, traversal from root will reach into the base program
   graph and drag immutable bytecode and schema declarations into the delta checkpoint store,
   breaking the state isolation contract of §3.1.

1. **Contradiction with §2.2 Architecture:** In §2.2, the relationship between overlay and base is
   modelled correctly as an out-of-band C++ pointer (`base_`) and coordinate arguments (`DimRef`),
   indicated in the diagram as `Overlay -.->|"Base pointer"| Base`. No manifold edges cross between
   overlay cells and base cells.

**Evidence:**

- `design/vagent-shared-program-and-live-upgrade.md:236–238`
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1338–1340, 1464–1490`
- `design/vagent-agent-language.md:226–233` (Rule V12)
- `design/vagent-shared-program-and-live-upgrade.md:188` (§2.2 diagram)

**Fix:** Replace §2.3 item 2 with an explicit reaffirmation of Rule V12:

```markdown
2. **Rule V12 Enforced Across Boundaries (No State-to-Program Links):** In accordance with Rule
   V12, no state cell in the overlay links directly to a cell in the base program manifold
   (`overlay.pos(d) != base`). All cross-boundary references are coordinate arguments (canonical
   `DimRef`s passed to `link()`/`linked()`) or VM instruction pointers held in `WorkerContext`
   (`actor->cursor`), never manifold edges between cells. Because `ArenaManifold::link`
   unconditionally executes `shadow(*to)` when targeting a base cell
   (`arena_manifold.cpp:1338–1340`), prohibiting cross-boundary manifold edges prevents
   copy-on-write shadowing of base cells and guarantees that state promotion during checkpoints
   never traverses into the program graph.
```

## MINOR

### 1. (§2.2, line 223) Suspension point linkage to resume opcode lacks explicit dimension name

**Problem:** §2.2 states: "and a suspension rank along `d.step` linking step ordinals ($0, 1, \dots,
K$) to their resume labels." It omits the dimension connecting the step ordinal cell to the target
opcode cell.

**Fix:** Specify that each suspension point cell $S_k$ on the plan's `+d.step` rank links posward on
canonical `+d.point` to the target opcode cell ($S_k \xrightarrow{+d.\text{point}}
\text{resumeOpcodeCell}$).

### 2. (§2.1, line 142; §5, line 452) Sealing should explicitly cite `xanadu::publish`

**Problem:** §2.1 and Proposal P17.2 describe sealing the program store but do not explicitly cite
`xanadu::publish`, which is the canonical mechanism that globalizes deployment permascroll spans
into the package descriptor.

**Fix:** Explicitly state in §2.1 and P17.2 that sealing the program store invokes `xanadu::publish`
to globalize primedia spans.

______________________________________________________________________

VERDICT: NOT CONVERGED
