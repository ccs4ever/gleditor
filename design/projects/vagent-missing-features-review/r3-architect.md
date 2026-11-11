# Round 3 critique: System Architect (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 1 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R2.1 (Worker Actor Termination Lifecycle in CompilerCoordinator): RESOLVED — The lifecycle is
  clarified in §4.2, specifying cooperative termination via `stop` percept upon `tell UnitCompiled`.
- R2.2 (Capability Authorization Guard in promote): RESOLVED — §3.2 and Proposal P27 explicitly
  mandate verifying `grant.immutable == false` before initiating `std:zigzag:promote`.

## BLOCKING

### 1. (§8.2, §8.3, §9 P32, §10.7) Topological Impossibility: Binary Tree on Dimension `d.branch`

Full title: Embedding a binary search tree in a single ZigZag dimension `d.branch` violates the
2-degree rank invariant and severs ancestral links via RL1 reciprocal link displacement

**Problem:** Section 8.2 proposes embedding a binary search tree into a single dimension `d.branch`:
"- `d.branch`: Orthogonal dimension where NEG links to the left subtree ($< \text{key}$) and POS
links to the right subtree ($> \text{key}$)." Section 9 (Proposal P32) allocates a single dimension
`d.branch`.

In Nelsonian ZigZag, every dimension is a strictly 1-dimensional manifold. Each cell $C$ must adhere
to the fundamental 2-degree invariant: $\text{deg}^+(C, D) \le 1$ and $\text{deg}^-(C, D) \le 1$ for
any dimension $D$. Furthermore, all links in `ArenaManifold` and `Store` are strictly bidirectional
and reciprocal (RL1, `apps/common/xanadu/zigzag/arena_manifold.hpp:714` and
`apps/common/xanadu/zigzag/manifold.cpp:653–690`): linking cell $A$ to cell $B$ in direction `dir`
automatically sets cell $B$'s link to $A$ in direction `-dir`.

Now trace the topology of any binary tree of depth $\ge 2$:

1. Let the root cell be $R$, and let left child $L$ have key $K_L < K_R$.

1. To link $L$ as the left child of $R$, the manifold links $R \xrightarrow{-d.branch} L$.

1. By RL1 reciprocity (`manifold.cpp:690`), $L$'s reciprocal positive link along `d.branch` points
   back to $R$:

   $$
   L \xrightarrow{+d.branch} R
   $$

1. Now insert a key $K_{RL}$ such that $K_L < K_{RL} < K_R$. Node $K_{RL}$ must become the **right
   child of $L$**.

1. According to §8.2, a right child must be linked along `+d.branch`:

   $$
   L \xrightarrow{+d.branch} K_{RL}
   $$

1. **Catastrophic Failure:** $L$'s `+d.branch` link is ALREADY occupied by its parent $R$! When
   `ArenaManifold::link(L, d.branch, POS, K_{RL})` executes, `manifold.cpp:679–688` detects the
   displaced neighbor $R$ and executes:

   ```cpp
   setOneSide(*other, dim, -dir, noCell);
   ```

   This immediately sets $R$'s `-d.branch` neighbor to `noCell`, severing $R$'s link to $L$!

1. Symmetrically, if $R$ links `+d.branch` to right child $W$, $W$'s `-d.branch` neighbor is $R$. If
   $W$ gains a left child $L_W$, linking $W \xrightarrow{-d.branch} L_W$ severs $W$'s backlink to
   $R$.

In graph theory, a graph where every vertex has degree $\le 2$ consists solely of disjoint paths and
cycles. A binary tree node has 3 incident edges (parent, left child, right child). It is
topologically impossible to embed a binary tree of depth $\ge 2$ into a single ZigZag dimension. Any
attempt to do so either severs parent-child links or violates 2-degree bounds via fan-in branching.

**Evidence:**

- `design/vagent-metacircular-missing-features.md:646–648`: "- `d.branch`: Orthogonal dimension
  where NEG links to the left subtree ($< \text{key}$) and POS links to the right subtree
  ($> \text{key}$)."
- `design/vagent-metacircular-missing-features.md:653–663`: ASCII diagram showing only depth 1,
  omitting the impossible reciprocal backlink from children to parent.
- `apps/common/xanadu/zigzag/manifold.cpp:679–690`: Reciprocal edge maintenance
  (`setOneSide(*target, dim, -dir, self)` and displacement of existing link).
- `design/vagent-metacircular-missing-features.md:723`: Proposal P32 specifying "1 dimension
  allocation (`d.branch`)".

**Fix:** Replace the single-dimension model with two orthogonal branch dimensions: `d.left` and
`d.right` (or `d.branch_l` and `d.branch_r`).

- Left child: `Parent` $\xrightarrow{+d.left}$ `LeftChild` (reciprocal: `LeftChild`
  $\xrightarrow{-d.left}$ `Parent`).
- Right child: `Parent` $\xrightarrow{+d.right}$ `RightChild` (reciprocal: `RightChild`
  $\xrightarrow{-d.right}$ `Parent`).

Topological validation of this fix:

- On `d.left`, a node has at most one left child (POS) and at most one parent (NEG).
- On `d.right`, a node has at most one right child (POS) and at most one parent (NEG).
- Every cell strictly satisfies $\text{deg}^+(C, D) \le 1$ and $\text{deg}^-(C, D) \le 1$.
- Parent ascent is trivial and $O(1)$: check `-d.left` first; if non-empty, parent is found; else
  check `-d.right`.
- Sequential in-order iteration continues to ride `+d.step` independently.
- Update §8.2, §8.3, and Proposal P32 to allocate 2 dimensions (`d.left`, `d.right`) instead of 1.

______________________________________________________________________

## MAJOR

### 1. (§7.3, §10.6, §9 P31) Turn Boundary Leak: Unisolated Intra-Turn Arm Reconsideration

Full title: Failure during plan arm execution leaks staged `+d.outbox` entries and in-turn mutations
if the BDI intention scheduler reconsiders untried arms within the same actor turn

**Problem:** Section 7.3 states: "Untried Arm Reconsideration on `#Fail`: If the active arm body
fails during execution (e.g. subgoal failure, expired deadline, or explicit `#Fail`), the failure
does not fault the actor. Instead, the BDI intention scheduler reconsiders the intention and
advances to the **next applicable untried arm**." In §10.6, the document correctly observes that
procedural side-effects cannot backtrack across statements via `#CutChoiceDepth`.

However, within the actor execution loop (`apps/common/xanadu/vagent/executor.cpp:206–273`), an
actor turn runs under a single base mark `m0 = arena.mark()`. Outbox records emitted by verbs
(`emit`, `tell`, `spawn`) are appended as live cells to the agent's `+d.outbox` rank
(`apps/common/xanadu/vagent/adapter.hpp:88–109`, `executor.cpp:368–374`). At turn commit, the turn
calls `arena.discard(m0)` (`executor.cpp:3348`), and the adapter drains `+d.outbox` to the transport
wire (`executor.cpp:3365–3375`).

Consider the following execution sequence:

1. Arm 1 begins execution and runs Statement 1: `emit DroneDispatched(id)`. This stages an outbox
   cell onto `+d.outbox`.
1. Statement 1 completes; `#CutChoiceDepth` cuts choice points.
1. Arm 1 runs Statement 2: `achieve FlyTo(dest)`, which fails (subgoal failure or `#Fail`).
1. Under §7.3, the BDI deliberation engine reconsiders and immediately advances to Arm 2:
   `where truck_available() => { emit TruckDispatched(id) ... }`.
1. Arm 2 executes and succeeds.
1. The actor turn completes with success: `arena.discard(m0)`.
1. `drainOutbox` drains **BOTH** `DroneDispatched` (from the failed arm!) and `TruckDispatched`
   (from the fallback arm!) onto the Spool-Mesh network.

This breaks actor turn atomicity and causes phantom message delivery from abandoned speculative plan
branches.

**Evidence:**

- `apps/common/xanadu/vagent/executor.cpp:262–273`: The base turn choice point and arena mark `m0`
  govern the entire turn.
- `apps/common/xanadu/vagent/executor.cpp:3346–3375`: Successful turn commits keep all staged outbox
  cells; `drainOutbox` transmits them without knowing which plan arm staged them.
- `design/vagent-metacircular-missing-features.md:606–614`: Lacks any specification of turn-level
  isolation or speculative rollback for failed arms.

**Fix:** Specify the exact turn isolation boundary for multi-arm plan execution in §7.3 and §10.6:

1. **Intra-Arm Speculative Boundary:** When executing an arm body within a turn, the executor must
   capture an arm-scoped mark: `m_arm = arena.mark()`. If the arm fails prior to irrevocable
   external commit, the executor must call `arena.release(m_arm)` to roll back speculative arena
   cells and un-stage any pending `+d.outbox` entries before advancing to the next untried arm.
1. **Post-Commit Arm Failure:** If an arm executes an irrevocable external operation (a committed
   action in a multi-turn intention), that arm has committed its turn. A subsequent failure in a
   later turn cannot rewind the previous turn; it must follow the standard BDI intention fault
   ladder (`faultIntention(intention, "body")`), recording the arm in
   `varsRecord(intention, "tried")` and rescheduling reconsideration for the next intention step
   with `step := 0`.

______________________________________________________________________

## MINOR

### 1. (§8.2, §9 P32) Dimension Registration and Link Retention in `std:zigzag:promote`

Full title: Ephemeral branch dimensions must be pre-filed in `VortexCore::dims()` to prevent silent
omission during store promotion

**Problem:** When a tree-backed `Map` is promoted to the Merkle ops spool via `std:zigzag:promote`
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1733–1802`), the link emission phase
(`arena_manifold.cpp:2097–2100`) verifies:

```cpp
const auto dim = real.find(edge.dim);
if (dim == real.end()) {
  continue; // Dimension cell not found in real map; link dropped!
}
```

If branch dimensions are minted ad-hoc in an arena without being registered in the destination
store's `d.dims` rank (the hazard resolved for `d.dims` in P27), all branch links will be silently
dropped, promoting a set of disconnected, unreachable cells.

**Fix:** Proposal P32 must specify that `d.left` and `d.right` are pre-filed in `VortexCore::dims()`
(`apps/common/xanadu/vortex/vortex_core.hpp:35–65`) and included in
`VagentLineage::standardDimensions()` (`apps/common/xanadu/vagent/lineage.hpp:45–80`), guaranteeing
they are recognized and preserved during `promote()`.

### 2. (§9 P31) Correct Line Anchors for Plan Lowering and Intention Scheduling

Full title: Update Proposal P31 line anchors to include `decls.cpp`, `executor.cpp`, and `meta.cpp`

**Problem:** Proposal P31 lists `apps/common/xanadu/vagent/parser.cpp:570–615`, `ast.hpp`,
`compiler.cpp:1498–1530`, `stdlib/checker.vagent`. In the codebase:

- Plan declaration and clause lowering are located in
  `apps/common/xanadu/vagent/decls.cpp:1268–1300` (`lowerClauseBody`) and `decls.cpp:1979–2000`
  (`case DeclKind::Plan`). `compiler.cpp:1498–1530` handles expression references
  (`referencesParam`), not plan clause lowering.
- Advancing to the next untried arm on `#Fail` requires updating the intention fault and
  reconsideration logic in `apps/common/xanadu/vagent/executor.cpp:3030–3090` and
  `apps/common/xanadu/vagent/meta.cpp:1270–1360` (`selectClause` with `tried` set).

**Fix:** Update P31's "Files Touched & Line Anchors" to include
`apps/common/xanadu/vagent/decls.cpp`, `apps/common/xanadu/vagent/executor.cpp`, and
`apps/common/xanadu/vagent/meta.cpp`.

______________________________________________________________________

VERDICT: UNCONVERGED (1 BLOCKING, 1 MAJOR, 2 MINOR)
