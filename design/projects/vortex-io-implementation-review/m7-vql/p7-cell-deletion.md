# P7: one cell-deletion algorithm

Proposal P7 of the Vortex I/O review, approved by the owner and applied in the M7-VQL gate round 1
follow-up. It started as `ArenaManifold::detachAll` for vortex M3 and became the general ZigZag
operation, because deleting a cell was already done three times.

## Owner's specification

Cell deletion is the basic ZigZag algorithm and exists once, shared by xuzz deletions and engine
reclamation: break every connection of the cell, remove the cell, then either leave the connections
broken or repair them by joining each dimension's negward and posward neighbours. Both modes are
valid and the caller chooses. A cursor on the deleted cell moves to the cell along the first bound
dimension vector in sequence; a cell with no connections sends it to the home cell.

## What existed

| where                                                                   | what it did                                                                                                                                                           |
| ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ZigzagVisualizer::deleteFocusCell` (xuzz, keys `Delete` and Backspace) | persistent: one `SetLink` per dimension joining the neighbours (Repair), chose the next focus by a private walk of x, y, z then any non-metadata dimension, then home |
| `ZigzagVisualizer::unlinkFocusAlong`                                    | one-sided unlink of the focus along x (separate command; kept, it is not a deletion)                                                                                  |
| `io_cells.cpp detachFromRank`                                           | ephemeral: unlink both sides along one rank and join the neighbours (Repair on one dimension), used by the request reaper and `freeOwnedGraph`                        |
| `ArenaManifold::freeCell` (P4)                                          | refuses `CellLinked`; "the caller detaches it first"                                                                                                                  |
| `IoService::reapRequest` / `ArenaIoCellStore::reapRequest`              | call `reapIoRequest`, which uses `detachFromRank`; no unlink code of its own                                                                                          |
| `FileReadHost::stageRead`                                               | frees a refused stage's cell; cells it frees were never linked                                                                                                        |

So there were two hand-written splices (xuzz, io_cells) and a primitive that refuses unless a third
did the work. xuzz does have a delete command, with default chords already in `system://keymap`
(`kKeymapDeleteFocusCell`, `kKeymapDeleteFocusCellBksp`), so no UI command was built.

## The algorithm

`apps/common/xanadu/zigzag/cell_deletion.{hpp,cpp}` (engine side, device-free):

- `Neighbourhood::of(cell, dimensionsOf(cell))` reads, per dimension, the negward and posward
  neighbours as `std::optional`. It is also the value a deletion returns: the neighbourhood it
  broke.
- `planDeletion(cell, hood, mode, emit)` emits the fewest `LinkEdit` writes, and `planRankDeletion`
  does the same for one dimension.
  - `Repair`: one write per dimension, `neg.posward := pos`. A link is one edge, so this displaces
    the cell from both its sides. A rank of exactly two is cleared instead of joined to itself (the
    survivor ends with no link), which differs from the old xuzz delete that left a self-link.
  - `LeaveBroken`: clear each of the cell's links, one write per side.
  - A dimension with one neighbour needs only the cell's link cleared in either mode. No write is
    emitted that changes nothing, because an append-only spool records every write.
- Applying the edits is the only part that differs by where the cell lives.

### What "remove" means

| space                       | removal                                                                                                                                                                                                                                                                        |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `ArenaManifold` (ephemeral) | `freeCell`: the slot is reused under a new incarnation. `ArenaManifold::deleteCell(ref, incarnation, mode)` detaches and frees, returning `Deletion{broken, freed}`.                                                                                                           |
| cell read through a base    | detached, not freed: the writes shadow the cell, the base is never written, releasing the enclosing mark undoes it. `freed` is false.                                                                                                                                          |
| persistent store (xuzz)     | detached. A minted cell is never un-minted in an append-only spool (store-slice-convergence R7: the `MakeCell` index is the `CellRef`; R8: view movement adds nothing). It keeps its place in hypertime with no links, as the code already said: DELETE is REARRANGE TO LIMBO. |

`detachCell(ref, mode)` and `detachFromRank(ref, dim, mode)` are the same plan without the removal
and for one rank. `deleteCell` refuses before writing anything: `UnknownCell` for a stale
incarnation, `NotReleasable` for the home cell and for cells other arena state names (dimensions,
proxies, quote and provenance cells).

### Clone ranks, marks, shadows

- A clone master heads its rank (clones are posward), so deleting a master with `Repair` promotes
  its first view; deleting a view repairs the rank and the master still answers. The algorithm does
  not protect a master other cells view: that is policy (`io_cells heldByViews` keeps one).
- Under a mark every write, and the free, is journalled; `release(mark)` restores links and cell.
- `link` may throw (shadow allocation, link-run limit) after some writes; under a mark `release`
  restores. `io_cells` catches and logs as before.

## The cursor rule

`cursorMoveOff(broken, boundDimensions, home, eligible)`, a pure function beside the plan:

1. For each bound dimension in binding order, the posward neighbour, then the negward one.
1. If no bound dimension yields an eligible cell, the cell's other connections in neighbourhood
   order, again posward before negward (the xuzz delete's existing fallback).
1. Otherwise `home`.

The posward-then-negward order is the one the xuzz delete already used over x, y, z. A neighbour
that is the deleted cell itself is never chosen. `eligible(dim, cell)` lets a UI refuse protected or
derived cells and metadata dimensions. Step 2 goes beyond the owner's wording (which names only home
for an unconnected cell) for a cell connected only on unbound dimensions; flagged in the report.

## Wiring

- xuzz: `deleteFocusCell` computes the neighbourhood, calls `cursorMoveOff` over (x, y, z) with its
  protected/ephemeral/metadata predicate, and emits `planDeletion(..., Repair)` through
  `UnifiedTransclusionEngine::linkCells`.
- engine: `io_cells detachFromRank` is now `ArenaManifold::detachFromRank(..., Repair)`.

## Tests

`tests/xuzz/cell_deletion_test.cpp`: `CellDeletionPlanTest` (write counts, rank of two),
`ArenaDeleteCellTest` (both modes, no links, rank of two, clone master and view, under a mark then
release, shadowed base cell, refusals, per-rank detach, stale handle), `CursorMoveOffTest` (first
bound dimension, posward over negward, falls through unbound dimensions, home when isolated or none
eligible, self-neighbour).

## M3: freeing an effect script's compiled program

`ProgramRun` (vql/execution.cpp) records the allocation incarnations of the program's own execution
(`ArenaManifold::newestIncarnation()`, `liveCellsAllocatedAfter()`): one window from before compile
to the first park, one per resume. When the run closes (completion, failure or an abandoned wake) it
`deleteCell`s each cell of those windows in Repair mode, so ranks shared with cells outside the
program (spin, grab, step, cursors) stay walkable. Left alone:

- cells a host hook allocated inside a window: `ScriptRunner::admit` reports its stretch through
  `QueryExecutor::excludeHostAllocations`, covering the staged spec, the request graph and the
  submission sum, which the I/O service reaps itself;
- the rows, and what they reach inside the program's region;
- a script that published session bindings (it keeps its program, as before);
- anything `deleteCell` refuses (home, dimensions, proxies).

Cells minted between windows (the outcome the pump publishes) are in no window. Measured: 40 cells
per run retained before, 0 after. `ManyEffectScriptRunsLeaveTheArenaBounded` runs 10^4 scripts and
asserts the slot count and live cells are unchanged from the warm state;
`AnAbandonedScriptFreesItsProgramToo` and `AFinishedScriptsRowsAndOutcomeSurviveItsProgram` cover
the other exits and the rows. The test harness stages a fresh spec cell per read that nothing owns
(the file host owns it in production), so the harness frees old ones.
