# Round 3 critique: Vortex & Spool-Mesh Expert (design/vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 3 MAJOR, 3 MINOR.

## BLOCKING

### 1. (§8.2, lines 641–663; §9, P32) Infeasible 2-Degree BST on Single Dimension `d.branch`

Section 8.2 proposes embedding a binary search tree in ZigZag by assigning left children to
`-d.branch` and right children to `+d.branch`:

```text
                     Root Cell [Key: "pure"]
                    /                       \
              (-d.branch)               (+d.branch)
                 /                             \
       Left Child ["closed"]          Right Child ["result"]
          (+d.values)                      (+d.values)
               |                                |
          Value Cell                       Value Cell
```

This topology fundamentally violates the Nelsonian 2-degree rank limit and the reciprocity invariant
of ZigZag manifolds (`ArenaManifold::link`, `arena_manifold.hpp:714`).

In ZigZag, any link along a dimension $D$ connects cell $A$ in direction `POS` to cell $B$ in
direction `NEG` ($A \xrightarrow{+D} B \iff B \xrightarrow{-D} A$). Every cell has strictly at most
one `POS` neighbor and at most one `NEG` neighbor on dimension $D$. Therefore:

1. When `Root` links `-d.branch` to `Left Child`, reciprocity dictates that `Left Child`'s
   `+d.branch` neighbor is `Root` (`Left Child` $\xrightarrow{+d.branch}$ `Root`).
1. If `Left Child` attempts to attach its own right child along `+d.branch`, its `+d.branch` link is
   already occupied by its parent `Root`. Overwriting it breaks the link to `Root` and orphans the
   subtree.
1. Similarly, `Right Child`'s `-d.branch` link is occupied by `Root` (`Right Child`
   $\xrightarrow{-d.branch}$ `Root`), leaving it unable to attach a left child along `-d.branch`.
1. A 1D manifold with degree $\le 1$ in `POS` and $\le 1$ in `NEG` can only form linear paths and
   cycles (ranks). Vertices in a branching binary tree require degree 3 (1 parent, 2 children).
   Attempting to represent branching on a single dimension collapses the tree into two degenerate
   linear spines (left-only off root, right-only off root); any non-trivial binary tree is
   topologically impossible.

**Resolution**: Allocate **two** orthogonal dimensions in `vortex_core.hpp:45–65` instead of one:
`d.left` and `d.right` (or `d.branch_left` and `d.branch_right`):

- `+d.left`: Connects parent to left child ($< \text{key}$). Reciprocally, `-d.left` connects left
  child back to parent.
- `+d.right`: Connects parent to right child ($> \text{key}$). Reciprocally, `-d.right` connects
  right child back to parent.
- `+d.values`: Connects key node cell to value cell.
- `+d.step`: Connects all key node cells in symmetric in-order sequence for $O(N)$ iteration.

Every node now has degree $\le 1$ in POS and $\le 1$ in NEG on each dimension, strictly adhering to
the 2-degree invariant while providing bidirectional parent-child navigation for $O(1)$ rebalancing
rotations. Update §8.2, §8.3, and P32 accordingly.

______________________________________________________________________

## MAJOR

### 2. (§8.3, lines 665–674; §8.4; §9, P32) Misconception of `#Find` / `#Step` for Map Subscripting

The document and review brief suggest leveraging `#Find` and `#Step` lowering for `Map` lookups.
However, in the Vortex VM (`apps/common/xanadu/vortex/vortex_vm.cpp:876–903`), `OpcodeKind::Find` is
specifically tailored for record field lookups (`record.field`):

1. `#Find` performs an $O(N)$ linear scan over `rankAfter(core_.arena(), referenced, dims.step)`.
1. It tests member cells for label equality against `core_.arena().linked(member, dims.name, POS)`.
1. It renders the member cell directly.

In contrast, the tree-backed `Map`:

1. Requires $O(\log N)$ descent along `d.left` / `d.right`.
1. Stores the string key payload directly in `CompactOpNode::text` of the node cell.
1. Attaches the value cell along `+d.values`.

Lowering `map[k]` via `#Find` would execute a linear walk along the in-order `+d.step` rank,
degrading all map subscript lookups to $O(N)$ linear scans and completely destroying the logarithmic
performance guarantee.

**Resolution**: Explicitly specify the lowering path for `Map` subscripting in §8.3, §8.4, and P32:

- Subscript access `map[k]` and `map[k]?` lowers either to a call to standard library function
  `std:map:get(map, key)` (`VortexStdLib::mapGet`), or to a dedicated VM opcode `OpcodeKind::MapGet`
  that accepts container ref and key string operands and descends `d.left`/`d.right`.
- When key lookup fails, `map[k]?` branches to `cells_->fail` (`#Fail`), whereas `.get(k)` wraps the
  result in an `Opt<V>` (`Some(val)` vs. `none`).

### 3. (§8.2–8.3; `vortex_stdlib.cpp:2650–2695`) Missing `consumeWork` Fuel in Map Operations

Existing map routines in `apps/common/xanadu/vortex/vortex_stdlib.cpp:2650–2695` and `mapKeyCell`
(lines 2785–2791) execute without calling `core_.arena().consumeWork()`. In an actor mesh, any loop
or recursive hop across manifold cells must expend fuel via `consumeWork()` to prevent unbounded
execution and enforce Spool-Mesh turn limits:

- Each node descent step along `d.left` or `d.right` during search (`mapGet`, `mapHas`,
  `mapKeyCell`) and insertion (`mapSet`) must call `core_.arena().consumeWork()`.
- Each step along `+d.step` during sequential iteration (`mapKeys`, `for (k, v) in map`) must call
  `core_.arena().consumeWork()`.

Without explicit `consumeWork()` accounting, an actor manipulating large maps or traversing cyclic
malformed graphs could evade CPU cycle quotas, violating actor turn isolation.

**Resolution**: Add an explicit requirement in §8.3 and P32 mandating that all `VortexStdLib` map
operations (`mapGet`, `mapSet`, `mapHas`, `mapKeys`, in-order iteration) call
`core_.arena().consumeWork()` on each cell hop.

### 4. (§7.3, lines 606–609; §9, P31) Missing Line Anchors for BDI Reconsideration

Section 7.3 states that on `#Fail`, "the BDI intention scheduler reconsiders the intention and
advances to the next applicable untried arm." However, Proposal P31 only lists:
`apps/common/xanadu/vagent/parser.cpp:570–615`, `ast.hpp`, `compiler.cpp:1498–1530`,
`stdlib/checker.vagent`.

This anchor list omits the core runtime subsystems that actually implement plan selection, intention
state, and failure ladders:

1. `apps/common/xanadu/vagent/executor.cpp:3030–3090`: The current intention failure ladder retries
   the *same* member up to `vagent.max_intention_faults`, then immediately raises `GoalFailed` with
   `cause = "no_untried_plans"`. It never records failed plans in the intention's `tried` set, nor
   does it invoke `selectClause` to find the next untried plan arm!
1. `apps/common/xanadu/vagent/meta.cpp:1270–1350`: `MetaRuntime::selectClause` takes a `tried` set,
   and `MetaRuntime::adoptGoal` initializes `field("tried", arena.makeCell())` (line 1446), but the
   reconsideration loop connecting intention failure to `selectClause` with the updated `tried` set
   is missing.
1. `apps/common/xanadu/vagent/decls.cpp:1979–2000`: Plan lowering (`DeclKind::Plan`) is executed in
   `decls.cpp` via `lowerClauseBody`, not in `compiler.cpp` (which compiles scalar expressions and
   statements). Multi-arm plans must be lowered in `decls.cpp` into $M$ sequential clause cells
   along `host`'s `+d.clause` rank.

**Resolution**: Update Proposal P31's "Files Touched & Line Anchors" to include
`executor.cpp:3030–3090`, `meta.cpp:1270–1350`, and `decls.cpp:1979–2000`. Detail the deliberation
transition in §7.3: When an arm body terminates with `#Fail` (or subgoal failure):

- Link the failed clause cell onto the intention's `tried` rank along `+d.step`.
- Invoke `meta.selectClause(goal, tried, whereFuel, args)`.
- If an untried arm matches, update the intention's `plan` field, reset `step` to 0, clear
  `faulted`, and set `ready = 1`.
- If `selectClause` returns `nullopt`, escalate to the goal failure ladder (`GoalFailed`).

______________________________________________________________________

## MINOR

### 5. (§7.3, line 612; §10.6, line 775) `#CutChoiceDepth` Shorthand vs. `ChoiceDepth` + `Cut` Flow

Sections 7.3 and 10.6 refer to `#CutChoiceDepth` as if it were a single VM opcode. In the Vortex VM
(`vortex_vm.hpp:92–93`, `vortex_vm.cpp:2114–2126`), choice point bounding is realized via two
separate opcodes:

1. `OpcodeKind::ChoiceDepth`: Reads current choice stack depth into a scalar register.
1. `OpcodeKind::Cut`: Cuts choice stack back to the specified barrier depth.

In `apps/common/xanadu/vagent/compiler.cpp:1552–1562`, this idiom is compiled cleanly by the
`bounded()` helper. The text should clarify that `#CutChoiceDepth` is conceptual shorthand for the
`ChoiceDepth` + `Cut` sequence compiled via `bounded()`.

### 6. (§7.2, §7.4) Decoupled Pattern Bindings between Guard and Body Fragments

In multi-arm plans with pattern specialization (`case Token { kind: Ident, ... } where ... =>`),
Section 7.4 states that `+d.grab` points to the guard graph and `+d.spin` points to the body graph.
Because guard and body are separate compiled fragments:

- Variables extracted during guard pattern matching (`?t`) live in the guard fragment's local frame
  and do not automatically carry over into the body fragment.
- The body fragment receives only the raw goal arguments (`args`). The compiler lowering in
  `decls.cpp` / `compiler.cpp` must ensure that pattern destructuring and variable bindings are
  emitted into the body fragment's prologue as well as the guard preflight, guaranteeing pattern
  variables are in scope within the body block.

### 7. (§8.2, §8.4) Map Container Header Cell vs. Mutating Tree Root Cell

Section 8.2 illustrates the tree rooted at `Root Cell [Key: "pure"]`. If the `Map` value handle is
directly the root key cell, any insertion that rotates the tree or prepends a new root (or creates
an empty map) would alter the root `CellRef`, leaving variable bindings (`let m := Map {...}`)
pointing to a non-root or stale cell. The specification should state that a `Map` consists of a
stable container cell (e.g. minted by `createMap()`), which links to the current tree root node via
`+d.values`. Mutations update the container's root pointer, preserving handle identity.

______________________________________________________________________

## Summary & Convergence Status

Because finding 1 identifies a fatal topological impossibility in the proposed 1D `d.branch` layout
that directly violates ZigZag 2-degree rank axioms, and findings 2–4 identify critical operational
and fuel gaps in lowering and BDI scheduling, this review cannot converge in its current state.
Adopting two orthogonal dimensions (`d.left`, `d.right`), wiring `consumeWork` into `VortexStdLib`,
and updating the BDI scheduler line anchors in P31 will achieve full architectural alignment.

VERDICT: NOT CONVERGED (1 BLOCKING, 3 MAJOR, 3 MINOR)
