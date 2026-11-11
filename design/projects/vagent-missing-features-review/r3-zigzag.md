# Round 3 critique: Xanadu / ZigZag Purist (vagent-metacircular-missing-features.md)

Paths below are relative to the repo root. Tally: 1 BLOCKING, 2 MAJOR, 2 MINOR.

## Status of previous BLOCKING/MAJOR findings (Rounds 2+ only)

- R1.1–R1.6: All Round 1 findings remain resolved in the core architecture.
- R2.1 (Event Name Alignment in Swarm Sequence): Non-blocking; addressed in §4.2.
- R2.2 (Primitive Mapping for CellValue in `set_value`): Non-blocking; addressed in §3.2.

______________________________________________________________________

## BLOCKING

### 1. (§8.2, §8.3, §9 P32, §10.7) Topological Impossibility of Direct Branching on `d.branch`

Full title: Attempting to Embed a Binary Tree Directly on One ZigZag Dimension Violates the
2-Degree Invariant and Displaces Existing Edges

**Problem:** Section 8.2 (§8.2, lines 641–663) specifies representing a balanced binary search tree
on a single orthogonal dimension `d.branch`, stating:
> "`d.branch`: Orthogonal dimension where `NEG` links to the left subtree ($< \text{key}$) and
> `POS` links to the right subtree ($> \text{key}$)."

It illustrates this with the topology:

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

In Ted Nelson's ZigZag model, and in this repository's implementation
(`apps/common/xanadu/zigzag/arena_manifold.cpp:1359–1378`), every dimension is strictly a 2-regular
graph. Each cell possesses at most one link in the `POS` direction and at most one link in the `NEG`
direction. Furthermore, ZigZag links are reciprocal edges:
`linked(A, dim, POS) == B` if and only if `linked(B, dim, NEG) == A`.

Because of this fundamental reciprocal invariant, a branching binary tree of depth $\ge 2$
**cannot be embedded directly between key cells along a single dimension**:

1. **Reciprocal Link Displacement & Subtree Severing**:
   When `Root` links `-d.branch` to `Left Child` ["closed"], `Left Child`'s `+d.branch` neighbor is
   reciprocally bound to `Root`. If a subsequent key $K$ satisfying
   `"closed" < K < "pure"` (e.g. `"make"`) is inserted, $K$ is the right child of `"closed"`.
   According to §8.2, a right child must be linked along `+d.branch`.
   Calling `arena.link(leftChild, core.dims().branch, DimVector::POS, makeCell)` sets `leftChild`'s
   `+d.branch` to `"make"`.
   In `ArenaManifold::link` (`arena_manifold.cpp:1364–1368`):

   ```cpp
   if (displacedUs && displacedUs != toCell) {
     if (const auto other = shadow(*displacedUs); other.has_value()) {
       setOneSide(*other, dim, -dir, noCell);
     }
   }
   ```

   This clears `Root`'s `-d.branch` link to `noCell`. The root is instantly severed from its left
   subtree!
2. **Symmetric Displacement on Right Subtree**:
   Symmetrically, `Right Child` ["result"] has its `-d.branch` link reciprocally bound to `Root`.
   Inserting a left child under `"result"` (e.g. `"queue"`) along `-d.branch` will overwrite its
   link to `Root`, severing `"result"` from `Root`.
3. **Internal Node Degree Violation**:
   An internal node in a binary tree has 3 incident tree edges (1 parent, 1 left child, 1 right
   child). A single ZigZag dimension provides degree at most 2 (1 POS half-edge, 1 NEG half-edge).
   Therefore, it is mathematically impossible for an internal node to connect to both children and
   its parent on a single dimension.

**Evidence:**
- `design/vagent-metacircular-missing-features.md:646–662`: Claims a single dimension `d.branch`
  models a binary search tree using `NEG` for left and `POS` for right.
- `apps/common/xanadu/zigzag/arena_manifold.cpp:1359–1378`:
  "A link is one edge two cells share, so setting it breaks whatever each end held: the invariant is
  `linked(a, d, dir) == b` exactly when `linked(b, d, -dir) == a`."
- `design/vagent-metacircular-missing-features.md:723`: Proposal P32 specifies "1 dimension
  allocation (`d.branch`)".

**Fix:**
Replace the single dimension `d.branch` with **two orthogonal branching dimensions**:
- `d.branch_l`: Left child branch ($< \text{key}$). Parent links `+d.branch_l` to left child; left
  child reciprocally links `-d.branch_l` to parent.
- `d.branch_r`: Right child branch ($> \text{key}$). Parent links `+d.branch_r` to right child;
  right child reciprocally links `-d.branch_r` to parent.

This topology is 100% compliant with ZigZag invariants:
1. Every cell has at most 1 link POS and 1 link NEG on `d.branch_l` (left child / parent).
2. Every cell has at most 1 link POS and 1 link NEG on `d.branch_r` (right child / parent).
3. Parent navigation is immediate: `step(N, -d.branch_l).value_or(step(N, -d.branch_r))`.
4. Absence of a child is cleanly represented by `noCell` without ambiguous sentinels.
5. Update Proposal P32 in §9 to allocate two dimensions (`d.branch_l` and `d.branch_r`) in
   `vortex_core.hpp:45–65` and `vortex_core.cpp:52–72`.

______________________________________________________________________

## MAJOR

### 1. (§8.2, §8.3, §8.4) Missing Map Container Anchor Cell and Ambiguous Root Invariants

Full title: Conflating Map Identity with the Root Key Cell Prevents Empty Maps, Rebalancing, and
Ordered Iteration

**Problem:** Section 8.2 and the diagram in §8.2 depict the root key cell ("pure") as the map
itself. Conflating the collection identity with an individual mutable key cell causes three major
failures:
1. **Empty Maps**: `createMap()` (`apps/common/xanadu/vortex/vortex_stdlib.cpp:2650`) creates an
   empty map before any entries exist. If the map handle is a key cell, an empty map cannot exist.
2. **Handle Invalidation on Rebalance or Deletion**: When keys are inserted or deleted, tree
   rebalancing (AVL / Red-Black rotations) changes which key cell occupies the tree root. If
   user-level variables (`let m: Map<K, V>`) directly hold the old root key cell, rebalancing or
   root deletion silently leaves user variables holding an interior descendant cell or a dangling
   cell.
3. **Ambiguous `+d.step` Iteration Anchor**: Iteration (`for (k, v) in map`) along `+d.step`
   requires a deterministic entry head. If the handle `m` is the tree root ("pure"), it sits in
   the middle of the sorted rank, preventing standard sequential iteration with
   `rankAfter(arena, mapRoot, dims.step)`.

**Evidence:**
- `design/vagent-metacircular-missing-features.md:653–662`: Root cell contains `Key: "pure"`.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2650`:
  `CellRef VortexStdLib::createMap() { return core_.arena().makeCell("map"); }`
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2690–2695`:
  `rankAfter(core_.arena(), mapRoot, core_.dims().vars)` relies on `mapRoot` acting as rank anchor.

**Fix:**
Specify that every map is anchored by a persistent container cell `mapRoot` (text `"map"`):
- `mapRoot` links along `+d.grab` to the current BST root key cell (or `noCell` if empty). Tree
  rotations update only `mapRoot`'s `+d.grab` link, ensuring external variable handles remain
  stable.
- `mapRoot` links along `+d.step` to the minimum key cell (head of the in-order rank).
- Sequential iteration `for (k, v) in map` and `mapKeys` simply walks
  `rankAfter(arena, mapRoot, core_.dims().step)`.

______________________________________________________________________

### 2. (§8.2, §8.3) Omission of In-Order Splice Specification during Insertion

Full title: Insertion Specification Omits In-Order `d.step` Rank Splicing and Does Not State
Rotation Invariance

**Problem:** Section 8.2 introduces `+d.step` for $O(N)$ sequential in-order iteration without
recursion. However, §8.3 defines insertion as:
> "Insertion (`map[key] := val`): $O(\log N)$ descent, followed by minting a key cell, linking
> `+d.values`, and attaching along `+d.branch` or `-d.branch`."

Section 8.3 completely omits splicing the newly minted key cell into the `+d.step` rank. If an
implementation attaches the cell to the tree branches but neglects the `d.step` rank, iteration
`for (k, v) in map` and `mapKeys` will skip newly added keys.

Furthermore, the document fails to explain how in-order `+d.step` threading behaves during tree
rebalancing (rotations):
In a binary search tree, tree rotations modify parent-child branch pointers, but **the in-order
sequence of keys is strictly invariant**. Therefore, tree rotations never touch `d.step` links.
Documenting this invariance is crucial for implementers: it guarantees that active sequential
iterators and spatial transclusion spans along `d.step` are never disturbed by tree balancing.

**Evidence:**
- `design/vagent-metacircular-missing-features.md:668–670`: Splicing into `+d.step` is omitted from
  the insertion steps.

**Fix:**
Update §8.3 to state:
1. During the $O(\log N)$ BST descent, the insertion routine tracks the in-order predecessor and
   successor.
2. After linking the new key cell $K$ to its parent along `+d.branch_l` or `+d.branch_r`, $K$ is
   spliced into the `+d.step` rank: predecessor links `+d.step` to $K$, and $K$ links `+d.step` to
   successor (or `mapRoot` links `+d.step` to $K$ if $K$ is the new minimum key).
3. Explicitly state the spatial invariance: Tree rotations (AVL / Red-Black) update only
   `d.branch_l` and `d.branch_r` edges; they **never modify `d.step` links**.

______________________________________________________________________

## MINOR

### 1. (§7.4, line 623; §9 P31) Guard Graph on `+d.clause`'s `+d.grab` Overly Restricted

Section 7.4 states:
> "- `+d.grab` pointing to the preflighted guard graph (`ExprKind::Binary` / `ExprKind::Call`)."

However, multi-arm plans (§7.2, lines 584–599) permit pattern specialization per arm:
`case Pattern where Expr => { ... }`.
Under §6.3, constructor pattern destructuring lowers to deterministic test graphs checking crum
string tags, `+d.grab` (first child), and `+d.step` (sibling arguments).
Clarify in §7.4 that the `+d.grab` guard head encompasses both the pattern test graph and the
boolean guard condition, failing the preflight if either fails.

### 2. (§10.7, line 785) Dimension Nomenclature Alignment in Refused Alternative 10.7

Section 10.7 lines 784–786 state:
> "Maps must be represented as spatial cell topologies over `d.branch` and `+d.values`."

Update to cite `(d.branch_l, d.branch_r)`, `+d.values`, and `+d.step` to match the corrected
2-degree compliant multi-dimensional tree topology.

______________________________________________________________________

VERDICT: REVISE (1 BLOCKING, 2 MAJOR, 2 MINOR)
