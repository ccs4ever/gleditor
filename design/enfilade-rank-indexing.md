# Enfilade Rank Indexing: A Proposal for U1

**Document Version:** 1.0 — One Tree, Three Wids **Status:** Proposal. Nothing here is built; U1 is
still open and this is a candidate answer to it, not a ruling **Answers:**
[`store-slice-convergence.md`](store-slice-convergence.md) U1 — random access along a rank

U1 is the convergence's oldest unresolved question, and it is stated there as a limit rather than a
bug: **a rank answers successor and predecessor, and nothing else.** `linked(c, d, negward)` is 4.96
ns and a walk of $n$ cells is $n$ of them, so "the 5000th cell on this rank" and "the cell on this
rank whose key is `foo`" are both $O(n)$, and there is nowhere to put an answer that survives an
edit.

U1's own text names the shape of the fix and then declines to build it:

> If any query is not, every rank needs an order-statistic index — which is the enfilade, rebuilt,
> and a real cost the convergence must own.

This document takes that sentence literally. It proposes the enfilade, owns the cost, and argues
that the structure Xanadu already invented for the permascroll is the same structure both of U1's
consumers are asking for — with the same tree and a different **Wid**.

______________________________________________________________________

## Contents

- [1. What U1 is actually being asked for, twice](#1-what-u1-is-actually-being-asked-for-twice)
- [2. An enfilade, in this codebase's vocabulary](#2-an-enfilade-in-this-codebases-vocabulary)
- [3. The ruling this needs: an enfilade is a replay product, not cells](#3-the-ruling-this-needs-an-enfilade-is-a-replay-product-not-cells)
- [4. One tree, three Wids](#4-one-tree-three-wids)
- [5. Why Vlog cannot sort its rank, and what that forces](#5-why-vlog-cannot-sort-its-rank-and-what-that-forces)
- [6. The cost, owned](#6-the-cost-owned)
- [7. The arena variant, and what a Mark does to a tree](#7-the-arena-variant-and-what-a-mark-does-to-a-tree)
- [8. What this does not solve](#8-what-this-does-not-solve)
- [9. Reconciliation with the rulings](#9-reconciliation-with-the-rulings)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. What U1 is actually being asked for, twice

Two designs reached U1 independently, from opposite ends:

| Consumer                                    | Asks for                        | Query                            |
| ------------------------------------------- | ------------------------------- | -------------------------------- |
| [VPL](vpl-array-language.md) §5             | an **ordinal**                  | `A[5000]`, `A[⍋A]`               |
| [Vlog](vlog-logic-extension.md) §6.3        | a **key**                       | clauses whose first arg is `foo` |
| `apps/zigzag`'s scrollbar (U1's experiment) | an ordinal, **and its inverse** | "where am I on this rank?"       |

The third row is the one that settles the design, and it was already in U1 without being read this
way. A scrollbar needs *both* directions: position → cell to jump, and cell → position to draw the
thumb. A cached array of cell refs answers the first and not the second, and an ordinal stored on
each cell answers both until someone inserts a cell, at which point $O(n)$ stored ordinals are
wrong. **The requirement is not "an index" but "an index whose cost per edit is not proportional to
the rank's length"** — which is precisely the problem enfilades were invented for, because the
permascroll has the same one.

So the three asks are one structure with three summaries. §4 is that claim.

______________________________________________________________________

## 2. An enfilade, in this codebase's vocabulary

An enfilade is a balanced tree over a sequence in which **every position is relative**. Each entry
in an interior node — historically a *crum* — carries two things about the subtree beneath it:

- a **Dsp** (displacement): how to get from this node's coordinate frame into the child's;
- a **Wid** (width): a summary of everything in that child, in that child's own frame.

Descending composes Dsps; ascending combines Wids. The reason this is not merely a B-tree is the
relativity: an insert in the middle of the sequence changes the Dsp on $O(\log n)$ crums along one
path and **renumbers nothing**, because no descendant ever stored its absolute position. A B-tree
with absolute keys has to rewrite every key after the insertion point.

The structure is parameterised, and the parameterisation is the whole trick. Dsp and Wid must
satisfy three algebraic conditions, and any pair that does yields a working enfilade:

```math
\begin{aligned}
&\text{Dsp composes associatively:} && d_1 \circ (d_2 \circ d_3) = (d_1 \circ d_2) \circ d_3 \\
&\text{Wid combines associatively:} && w_1 \oplus (w_2 \oplus w_3) = (w_1 \oplus w_2) \oplus w_3 \\
&\text{Dsp acts distributively on Wid:} && d \cdot (w_1 \oplus w_2) = (d \cdot w_1) \oplus (d \cdot w_2)
\end{aligned}
```

That is a monoid, a monoid, and an action of the first on the second. Swap the pair and you get a
different index over the same tree code, which is how Xanadu got a family from one idea:

| Historical filade | Dsp            | Wid               | Answers                        |
| ----------------- | -------------- | ----------------- | ------------------------------ |
| grandfilade       | address offset | byte extent       | where a scroll's bytes live    |
| poomfilade        | ordinal offset | count             | the $n$-th thing in a sequence |
| spanfilade        | address offset | bounding span set | who quotes this passage        |

**A rank index is a poomfilade for a rank.** Saying so is not decoration: it means the hard part was
solved in 1979 and what remains is choosing Wids and paying for nodes.

______________________________________________________________________

## 3. The ruling this needs: an enfilade is a replay product, not cells

The tempting move in this codebase is to make the index out of cells — a `d.enfilade` dimension,
crums as cells, because everything else here is cells and R12 made a dimension free to mint.

**Refuse it, for the reason §4.3 of the Vlog note refuses path compression.** Rebalancing is a
mutation. Crums-as-cells would mint a `SetLink` per rotation, so *reading* the 5000th cell of a rank
could append operations to the author's spool, and scrubbing to an earlier microversion would show
the rebalance as an edit to the document. R8 exists to forbid exactly that:

> **Only a user-generated update persists. Navigation never does.**

So the proposal is the other shape the convergence already has a name for:

> **An enfilade is a third replay product of the ops spool**, folded beside `Version` (text, from
> Insert/Delete/Rearrange) and `Manifold` (structure, from `OpKind::Structure`). It is derived, it
> earns no operations, it is thrown away and rebuilt like any index, and **nothing on disk changes
> to accommodate it.**

That last clause is what makes this proposal cheap enough to consider at all. No `OpKind`, no
`StructureVerb`, no flag bit, no format version bump, no fixture regeneration — the same bill Vlog
§8 came in under, and for the same reason: a fold is a function of operations that already exist.

The natural place is inside the existing fold. `Manifold::applyStructure()` already sees every
`SetLink`, which means it already knows the instant a rank changes shape — it is maintaining both
ends of the edge at that moment. An enfilade update is $O(\log n)$ work at a site that is already
doing $O(1)$ work with the relevant cells in registers.

______________________________________________________________________

## 4. One tree, three Wids

### 4.1 Wid = count. VPL's ordinal, and the scrollbar's inverse

The poomfilade proper. Each crum's Wid is the number of cells beneath it; each Dsp is an ordinal
offset.

- **Ordinal → cell**: descend, subtracting counts. $O(\log n)$.
- **Cell → ordinal**: *ascend*, summing the Wids of left siblings. Also $O(\log n)$, and this is the
  direction a cached array cannot answer at all.
- **Insert**: increment the count on one path. $O(\log n)$, no renumbering.

For the ascent to be possible each indexed cell needs a pointer to its leaf crum, so a cell's slot
gains one word *when the rank it is on is indexed* — see §6 for where that word lives, because it is
not in `CellSlot`.

### 4.2 Wid = key summary. Vlog's clause selection

Here the rank is **not sorted by the key**, and cannot be (§5). So min/max bounds prune nothing and
the Wid has to be a *set* summary instead: a small Bloom filter over the keys beneath the crum, or
for small subtrees an exact sorted list of them.

- **Key → candidate cells**: descend into every child whose Wid says *maybe*, skip the rest.
  Sublinear whenever the key is selective; exactly $O(n)$ when every crum says maybe, which is the
  honest worst case and is what a false-positive rate buys down.
- **Combine** is bitwise OR, which is associative; **Dsp** is the identity, since a key does not
  move when the sequence shifts. The algebra of §2 is satisfied trivially, which is a hint that this
  is the *easy* Wid and the ordinal is the hard one.

The key itself is whatever the caller keys on. For Vlog it is the principal functor of a clause's
first argument, which is either a scalar cell's canonical bits (R6, free — 64 bits already in the
slot) or a content hash of the functor's text. **Note that R6's refusal to give equal numbers equal
addresses is what makes the bits the right key**: they are a function of the value and not of where
it was typed, so two clauses about `42` land in the same bucket without sharing an address.

### 4.3 Wid = bounding span set. The one the codebase already wants

A spanfilade over `PrimediaSpan`s answers "which cells quote this passage", which is what
`link_discovery.cpp` and the transclusion-prism renderer do by other means today. It is listed here
not to propose building it but because it is evidence the structure pays for itself more than once:
if the tree exists for §4.1 and §4.2, this is a third Wid and no new tree.

Worth one warning, already recorded as a convergence non-goal: a span Wid must be a bounding *set*
and not a bounding *interval*, because `PrimediaSpan::intersect()` is pure range arithmetic and
`1.0` at `0x3FF0…0` overlaps its successor by seven bytes. An interval Wid over scalar renderings
would report transclusions between unrelated numbers.

______________________________________________________________________

## 5. Why Vlog cannot sort its rank, and what that forces

A tempting simplification: keep the clause rank sorted by key and use a min/max Wid, which is
smaller and exact.

**It is not available, and the reason is semantic rather than technical.** Prolog's clause order is
part of the program's meaning — `assertz` appends and `asserta` prepends *because* the order
determines which solution comes first, and ISO's logical update view (Vlog §6.2) is a statement
about that order. Sorting the rank would change what the program computes.

This is exactly the situation enfilades were built for. The permascroll cannot be sorted either: it
is append-only and its order is its history. An enfilade indexes **an order that must be
preserved**, which is why the Wid is a summary of a subtree's contents rather than a bound implied
by sortedness. The requirement Vlog derived from ISO and the requirement Xanadu derived from
append-only-ness are the same requirement.

______________________________________________________________________

## 6. The cost, owned

U1 asks for the cost to be owned rather than waved at. Here it is, and the conclusion is that **not
every rank gets one.**

**Space.** At a 32-way branching factor there is roughly one crum per cell plus one interior node
per 31, so the node overhead is about $\tfrac{1}{31}$ of the leaf count — call it 3% in nodes, but
the crums themselves are the real cost. A count-Wid crum is a child reference plus a count: 8 bytes.
A Bloom-Wid crum is that plus the filter: 16 bytes at a 64-bit filter. So an indexed rank costs
roughly **8–16 bytes per cell per Wid**, against `CellSlot`'s 32 bytes and R12's quoted 108 bytes
per cell all-in. An index on every rank of every cell would be a fraction of the manifold; an index
on every *Wid* on every rank would not.

**Time.** Bulk-loading during a cold fold is $O(n)$ — the cells arrive in rank order, which is the
one case a tree build is linear. Incremental maintenance in `applyStructure()` is $O(\log n)$ per
`SetLink` on an indexed rank, and zero on any other.

**So the trigger matters more than the structure.** Three candidate policies, in increasing order of
how much they assume:

1. **Explicit.** A caller asks for an index on a named (cell, dimension) rank and holds it. Honest,
   and puts the cost where the query is.
1. **On second ask.** The first random-access query on a rank walks it $O(n)$ *and* builds the
   index; subsequent ones are $O(\log n)$. Amortises to nothing if a rank is queried more than
   twice, which is the profile a scrollbar and a clause database both have.
1. **On size.** Any rank over some length gets one when it is folded. Simplest, and wrong — a
   million-cell rank nobody ever subscripts pays for an index nobody reads.

**(2) is the recommendation**, because it needs no measurement to be safe: it cannot be worse than
today by more than a constant, and U1's experiment has not been run.

**Where the per-cell leaf pointer lives.** Not in `CellSlot`. That struct is 32 bytes by
`static_assert` and R12's arithmetic turned on those bytes; spending four of them on an index most
cells are not in would repeat the mistake the fixed dimension array made. It goes in a side array on
the index itself, keyed by dense id — the same shape as `SegmentedOpsSpool::tree`, which lives
beside the nodes for the same reason `CompactOpNode` could not grow a field.

______________________________________________________________________

## 7. The arena variant, and what a Mark does to a tree

Step 21's `ArenaManifold` has choice points that undo by truncation, and a tree is the one structure
that does not survive that treatment: rebalancing moves nodes that were allocated before the mark.

Two ways out, and the second is better:

- **Trail the tree.** Every rebalance saves the nodes it touches, the way §5.3's trail saves a
  `CellSlot`. Correct, and it makes a read-side index pay a write-side cost on every binding.
- **Rebuild rather than maintain.** An arena index is *ephemeral even by arena standards*: it is
  built on demand, invalidated wholesale by any write to its rank, and never trailed. A resolution
  engine's clause ranks are read constantly and written almost never — `assertz` during a query is
  legal and rare — so wholesale invalidation is close to free and a tree that is never mutated under
  a mark needs no undo at all.

The second is also what the pinned-ephemeral-island mitigation in Vlog §4.3 and §6.3 already
described, now with a structure in it rather than "an index". An arena enfilade sits in a pinned
island, carries `ephemeralBit` refs if it references cells at all, and `applyStructure()` already
refuses to persist those — so the R8 boundary needs no new enforcement, exactly as step 21 found.

______________________________________________________________________

## 8. What this does not solve

1. **It does not make U1 free.** It makes it $O(\log n)$ with a 3–16 byte per-cell price on ranks
   that opt in. U1 asked whether the manifold can answer random access at all; the answer this
   proposes is "yes, for ranks that pay", which is a different and smaller claim than "a rank
   answers random access".
1. **It does not run U1's experiment.** The instrumentation U1 describes — the distribution of
   distance between a requested index and the cursor — still decides whether `apps/zigzag` needs any
   of this. If every editor query is a bounded walk from a cursor, the editor needs nothing and only
   VPL and Vlog do. **That experiment is still the next thing to do, and this document does not
   substitute for it.**
1. **Nothing here is written.** No code, no measurement, and the branching factor, Bloom width and
   trigger policy are all guesses that want a benchmark. The 4.96 ns/hop number that makes a walk
   affordable came from a measurement; none of these did.
1. **The ordinal Wid and the incremental fold interact in a way not worked out here.** R9's
   `verifyAgainstFullRebuild()` is what keeps a materialised view honest, and an enfilade folded
   incrementally is a second thing that can drift. It would need the same treatment, and that
   probably means `verifyAgainstFullRebuild()` grows to cover it rather than a parallel mechanism.
1. **VQL's `A[⍋A]` case is worse than a subscript.** An index-of-sorted permutation applied by
   subscript is $n$ random accesses, so $O(n \log n)$ with this and $O(n^2)$ without. Better, and
   still not what an array language's user expects from a sort.

______________________________________________________________________

## 9. Reconciliation with the rulings

- **R6 (scalar cells)**: the canonical bits are the key for §4.2, already in the slot and already a
  function of the value rather than of the address. The ruling that refused a shared address for
  equal numbers is what makes them a *sound* key.
- **R7 (micro-history chain)**: unaffected. An enfilade indexes cells, not operations.
- **R8 (only user-generated updates persist)**: the governing constraint, and the reason §3 refuses
  crums-as-cells. An enfilade is derived and therefore allowed to exist; it is a mutation-free read
  of the document by construction.
- **R9 (a materialised view can drift, so make it askable)**: applies to this too, per §8.4.
- **R12 (a dimension is a cell; runs beat fixed arrays)**: the reason the leaf pointer goes in a
  side array rather than into `CellSlot`'s 32 bytes, and the reason a `d.enfilade` dimension would
  be a trap rather than a shortcut.
- **V3 (CSR arena, compaction folded into the sweep)**: an enfilade over a rank is a second
  structure with the same lifecycle question, and §7's answer — rebuild, do not maintain, under a
  mark — is V3's answer in a different register.
- **U1**: this document is a candidate answer. It does not close it.
- **U3 (a cell's content is a run of spans)**: §4.3's spanfilade is over those spans, and is the
  reason the run's addresses being stable through an edit matters to more than transclusion.

______________________________________________________________________

## Appendix: Versioning and Change History

The rule is [VQL](vql-query-language.md)'s and [Vortex](vortex-hyperstructural-runtime.md)'s:

- **Major** — a change a conforming implementation could not ignore.
- **Minor** — an addition that breaks nothing, a refinement, a clarification, a correction to prose
  in a normative section, punctuation.

| version | commit    | date       | change                                                                  |
| ------- | --------- | ---------- | ----------------------------------------------------------------------- |
| 1.0     | `PENDING` | 2026-09-11 | Initial proposal: one tree, three Wids, and a replay product not cells. |
