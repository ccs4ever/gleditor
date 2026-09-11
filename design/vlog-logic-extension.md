# Vlog: Unification and Backtracking in Vortex

**Document Version:** 3.0 — Trailing Is Copy-on-Write **Extension To:** Vortex Hyperstructural
Runtime Core — `link` and `value`, plus §8's bill **Status:** The *machinery* of §5 is built and
tested — `ArenaManifold`, `mark`/`release`/`discard`, the conditional trail, `scratchScroll`,
`promote()`. The resolution engine over it is not: no unification, no clause database, no solver.
**Core changes required:** none outstanding; §8 is now a record of what was paid

**Vlog** — the Vortex Logic Extension — is resolution as a program over Vortex's own two primitives.
The original Vortex draft listed "Prolog-style unification and backtracking" as a deferred idea and
said nothing more about it; the fossil of that decision is the words "Logic Engine" in the spec's
`Target Environment:` line. This document is the elaboration, written now because the architecture
changed underneath it in a way that makes the idea much cheaper than it was when it was deferred.

Two things happened. `d.entangle` was deleted and replaced by **`d.clone` rank traversal**, where
one master holds the content and every cell on the rank reads through it
([vortex-hyperstructural-runtime.md](vortex-hyperstructural-runtime.md) §5.2). And the store became
an **append-only operations spool folded into a manifold**, where every state has a name and two
operations naming one parent are two futures that do not destroy each other
([store-slice-convergence.md](store-slice-convergence.md)). The first of those is a variable
binding. The second is a choice point. Neither was built with logic programming in mind, and both
are exactly the mechanism a resolution engine needs.

**Vlog is Vortex gaining elements of Prolog, not a Prolog that compiles to Vortex.** The distinction
matters and is the whole reason this is worth writing down: there is no separate term
representation, no heap, and no binding environment. A term is a cell. A variable is a cell. A
binding is a link. Resolution is a program written in Vortex's two primitives, in the same sense
that [VQL](vql-query-language.md) and [VPL](vpl-array-language.md) are — and it is held to the same
acceptance test they are (§2). Vlog adds two data structures and no more: §5.3's trail, which is not
there to undo destructive assignment but because an arena that truncates can leave a dangling
reference behind, and §5.5's scratch buffer, for the one operation in the language that constructs
bytes nobody has written down before.

**Why "extension" and not "front end".** VQL and VPL are front ends: surface syntaxes over the
manifold that compile to `link` and `value` and ask the runtime for nothing it does not already do.
Vlog is not one of those. It has no syntax of its own — write it in VQL's paths, in VPL's glyphs, or
against the primitives directly, because what Vlog contributes is a *mechanism*, not a notation. And
it does ask the runtime for something: a way to snapshot and discard an **ephemeral** manifold
cheaply enough to do it a million times in a query (§8). That single ask is what makes this an
extension rather than a third language, and it is the reason the material lives in its own document
instead of as a section of the Vortex spec.

______________________________________________________________________

## Contents

- [1. What is being claimed](#1-what-is-being-claimed)
- [2. The acceptance test: `link` and `value`, and nothing else](#2-the-acceptance-test-link-and-value-and-nothing-else)
- [3. Terms](#3-terms)
  - [3.1 A term is a cell; its arguments are the Inputs Wing](#31-a-term-is-a-cell-its-arguments-are-the-inputs-wing)
  - [3.2 A variable is a cell on a `d.vars` rank](#32-a-variable-is-a-cell-on-a-dvars-rank)
  - [3.3 `deref` is `cloneMaster`, and it already exists](#33-deref-is-clonemaster-and-it-already-exists)
- [4. Unification is one link along `d.clone`](#4-unification-is-one-link-along-dclone)
  - [4.1 The four cases](#41-the-four-cases)
  - [4.2 Variable-to-variable aliasing is why the clone rank is the right mechanism](#42-variable-to-variable-aliasing-is-why-the-clone-rank-is-the-right-mechanism)
  - [4.3 Union is splicing two ranks; find is deliberately not compressed](#43-union-is-splicing-two-ranks-find-is-deliberately-not-compressed)
  - [4.4 Rational trees are the default, and the occurs check is the opt-in](#44-rational-trees-are-the-default-and-the-occurs-check-is-the-opt-in)
- [5. Backtracking is already implemented, twice, at two scales](#5-backtracking-is-already-implemented-twice-at-two-scales)
  - [5.1 Persistent: a choice point is a microversion](#51-persistent-a-choice-point-is-a-microversion)
  - [5.2 Arena: a choice point is a pair of high-water marks](#52-arena-a-choice-point-is-a-pair-of-high-water-marks)
  - [5.3 The WAM's trail condition falls out of the arena rather than being imposed on it](#53-the-wams-trail-condition-falls-out-of-the-arena-rather-than-being-imposed-on-it)
  - [5.4 Cut is one comparison against a barrier](#54-cut-is-one-comparison-against-a-barrier)
  - [5.5 What the arena is not: no in-memory store, and no in-memory permascroll](#55-what-the-arena-is-not-no-in-memory-store-and-no-in-memory-permascroll)
- [6. The database](#6-the-database)
  - [6.1 Clauses are a rank on `d.clause`](#61-clauses-are-a-rank-on-dclause)
  - [6.2 `assertz`, `retract`, and the logical update view for free](#62-assertz-retract-and-the-logical-update-view-for-free)
  - [6.3 First-argument indexing wants random access, which is U1](#63-first-argument-indexing-wants-random-access-which-is-u1)
- [7. Worked example: `append/3`](#7-worked-example-append3)
- [8. What the C++ core actually has to gain](#8-what-the-c-core-actually-has-to-gain)
- [9. What this buys that a conventional Prolog does not have](#9-what-this-buys-that-a-conventional-prolog-does-not-have)
- [10. Breakages and costs](#10-breakages-and-costs)
- [11. Reconciliation with the convergence](#11-reconciliation-with-the-convergence)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. What is being claimed

| Prolog concept               | Vortex mechanism                                       | New machinery   |
| ---------------------------- | ------------------------------------------------------ | --------------- |
| Compound term `f(A, B)`      | A cell holding `"f"`; arguments on `+d.grab`/`+d.step` | none            |
| Atom, integer, float         | A cell with content; a scalar cell for numbers (R6)    | none            |
| Unbound variable             | A cell on a `d.vars` rank whose clone master is bare   | none            |
| `deref`                      | `Manifold::cloneMaster()`                              | none            |
| Binding `X = T`              | One `SetLink` joining `X`'s clone rank onto `T`'s      | none            |
| Variable aliasing `X = Y`    | The same operation, in the same direction              | none            |
| The binding environment      | The clone ranks themselves; there is no other store    | none            |
| Choice point (persistent)    | A `MicroversionId`                                     | none            |
| Choice point (arena)         | Arena high-water marks                                 | `mark()`        |
| Undo on failure (persistent) | `Store::rebuildManifold(M)`                            | none            |
| Undo on failure (arena)      | `resize()` plus a conditional trail                    | `release()`     |
| The trail                    | Only writes to cells older than the mark               | one vector      |
| Constructed text             | A run in the arena's scratch buffer (§5.5)             | `scratchScroll` |
| Retry as a new branch        | `apply(M, op)` where `M` already has a successor       | none            |
| Cut                          | A recorded barrier, compared against before undoing    | none            |
| The clause database          | A rank on `d.clause` off a predicate cell              | none            |
| `assertz` / `retract`        | Link at the rank tail / rearrange to limbo             | none            |
| The logical update view      | Resolve against the microversion of call entry         | none            |
| Tabling, memo tables         | A pinned ephemeral island (§5.6 of the Vortex spec)    | none            |

The right-hand column is the argument. Everything structural is already there; what is missing is a
way to take a cheap snapshot of an *ephemeral* manifold, because the persistent one's snapshots are
already free and are too expensive to use a million times per query (§10).

______________________________________________________________________

## 2. The acceptance test: `link` and `value`, and nothing else

VQL and VPL are each held to the rule that every construct must lower to a finite sequence of

```text
link(cell, ±dim, [target])   ->  cell_id or nothing
value(cell, [offset], [length], [replacement])  ->  cell_id
```

and that an expression which cannot be written that way is a defect in the document rather than a
feature of the language. The same rule binds Vlog, and it is a sharper test here than it was there,
because resolution is the first thing proposed for Vortex that has a *control* structure of its own.
The temptation is to give the engine a `unify` primitive and a `choice_point` primitive and declare
victory. The rest of this document is the claim that neither is needed:

- **Unification is a program.** §4's pseudocode calls `link`, `value` and `cloneMaster`, recursing
  over an argument rank. It is longer than a primitive would be and that is all.
- **A choice point is not an operation at all.** It is a *reading* of state the runtime already
  keeps — a microversion id, or two vector lengths. Nothing is written to take one.
- **Cut writes nothing either.** It compares.

So Vlog adds no primitive, and the Single-Primitive Invariant survives a feature that looks, from
the outside, like it ought to break it. §8's two methods are not a counterexample: `mark()` and
`release()` manage the *lifetime* of an ephemeral manifold and cannot express anything about cells
or links that `link` and `value` could not already say. An extension that needed a third primitive
would be a different and much worse document.

______________________________________________________________________

## 3. Terms

### 3.1 A term is a cell; its arguments are the Inputs Wing

A compound term is a cell whose content is the functor name, with its arguments hanging off the
Inputs Wing of §3 of the Vortex spec: the first argument posward on `+d.grab`, the rest chained
posward on `+d.step`.

```mermaid
graph LR
    F["f"] -->|"+d.grab"| A1["arg 1: X"]
    A1 -->|"+d.step"| A2["arg 2: g"]
    A2 -->|"+d.step"| A3["arg 3: 42"]
    A2 -->|"+d.grab"| B1["arg 1 of g: Y"]
```

This is not a term representation invented for the occasion — it is *literally* the dual-wing
calling convention, which is what makes a goal and a term the same object in Vortex as they are in
Prolog. `call/N` needs no conversion step because there is nothing to convert, and `=..` (univ) is a
walk along `+d.step` counting cells.

Arity is the length of that rank, which means:

- An **atom** is a cell with content and no `+d.grab` link.
- A **number** is a scalar cell — content holding its shortest round-trip rendering, plus canonical
  bits in the same cell (convergence R6). Numeric unification is therefore a 64-bit comparison and
  never parses text, which is exactly why R6 put the bits there.
- A **list** is `'.'/2` in the ordinary way, or — better, and available here — a rank on a dimension
  of its own, which costs one cell per element instead of two and gives `length/2` a walk instead of
  a recursion. Both are expressible; the second is not expressible in Prolog at all.

### 3.2 A variable is a cell on a `d.vars` rank

Variablehood is **membership of a rank**, not a flag bit. This follows the precedent convergence R12
set for dimensions: a dimension is a cell on the `d.dims` rank rather than a cell with a
`isDimension` bit, because a rank is already a thing the manifold can answer questions about and a
bit is not.

So a variable is minted onto the `d.vars` rank of the scope cursor that introduced it (§4 of the
Vortex spec), and:

```text
is_unbound(C)  ==  on_vars_rank(deref(C))  and  value_is_empty(deref(C))
```

The second conjunct is what separates an unbound variable from a bound one, and the first is what
separates an unbound variable from the atom `''`. Once `C` is bound, `deref(C)` is the *term*, which
was not minted onto a `d.vars` rank, so the first conjunct fails and no separate "bound" marker is
ever written.

Note what is *not* here: §4's `d.values` axis. The Vortex spec uses `d.vars`/`d.values` as an
associative scope — a name cell on one axis, its binding on the other. A logic variable does not
want that, because a `d.values` link is a pointer to a value and a clone link *is* the value. Using
`d.values` would give two mechanisms for one idea and would lose the aliasing property §4.2 is
about. `d.vars` keeps its role as the rank that says "these cells are the variables of this scope";
`d.values` stays for ordinary imperative bindings, which are assignable and therefore genuinely a
different thing.

### 3.3 `deref` is `cloneMaster`, and it already exists

```cpp
CellRef Manifold::cloneMaster(CellRef ref, DimRef cloneDim) const noexcept;
```

It walks negward along the given dimension to the end of the rank, is bounded by the slot count, and
answers the cell it started from if the rank loops. That is `deref` — including the cycle guard,
which §4.4 needs and which was written for a completely different reason.

______________________________________________________________________

## 4. Unification is one link along `d.clone`

A clone rank has one master at its negward end; every cell on the rank reads the master's content,
and a write through any of them lands on the master and is instantly visible from all of them. That
is the semantics of a logic variable binding, stated without reference to logic programming, in a
mechanism that was designed to let a spreadsheet cell appear in two places.

**To bind `X` to `T`, splice `X`'s clone rank onto the posward tail of `T`'s.** The master of the
combined rank is `T`'s master, so `X` now reads `T`.

### 4.1 The four cases

```text
unify(A, B):
    a = deref(A);  b = deref(B)
    if a == b:                        succeed          # already the same rank
    if unbound(a):                    splice(a, b);  succeed
    if unbound(b):                    splice(b, a);  succeed
    # both bound: compare, then recurse
    if arity(a) == 0 and arity(b) == 0:
        succeed if scalar_bits(a) == scalar_bits(b)    # numbers: 64-bit compare
               or render(a) == render(b)               # atoms
        else fail
    if render(a) != render(b):        fail             # functor mismatch
    x = link(a, +d.grab);  y = link(b, +d.grab)
    while x and y:
        if not unify(x, y):           fail
        x = link(x, +d.step);  y = link(y, +d.step)
    fail if x or y                                     # arity mismatch
    succeed

splice(v, t):
    vh = walk(v, -d.clone)            # head of v's rank
    tt = walk(t, +d.clone)            # tail of t's rank
    link(tt, +d.clone, vh)            # one call
```

`splice` is **one** `link` call rather than a pair, because `Manifold::applyStructure()` writes the
reciprocal end of a dimensional link in the same operation and evicts whatever was previously linked
there. Since `tt` is a rank tail it has no posward neighbour to evict, so the single call is also
the whole of the edit. This is worth stating precisely because it is the one place where
"unification is a link" could have turned out to mean "unification is three links and a repair."

### 4.2 Variable-to-variable aliasing is why the clone rank is the right mechanism

The var–var case is the one that decides whether a binding mechanism is correct, and it is where
most naive representations fail. After `X = Y` with both unbound, and then `Y = foo`, `X` must be
`foo`. A representation that binds by copying a value has nothing to copy at the first step and no
record of the obligation; a representation that binds by pointer needs the first step to install a
pointer whose target is later overwritten, which is the destructive assignment a trail exists to
undo.

Here the first step splices two ranks whose master holds no content, and the second step writes
content to that master. The Vortex spec's own headline property for clone ranks — *creating a new
head instantly changes the value of every cell along `d.clone`* — **is** the propagation that makes
aliasing correct, and no step of it is a mutation of anything but the master's content.

```mermaid
graph LR
    subgraph after["after X = Y, then Y = foo"]
        M["master: 'foo'"] -->|"+d.clone"| Y2["Y"]
        Y2 -->|"+d.clone"| X2["X"]
    end
    subgraph before["after X = Y"]
        MB["master: bare"] -->|"+d.clone"| YB["Y"]
        YB -->|"+d.clone"| XB["X"]
    end
```

### 4.3 Union is splicing two ranks; find is deliberately not compressed

What §4.1 describes is union-find. `splice` is union, in one operation and independent of rank
length. `deref` is find, and it is a walk.

A conventional union-find would now apply **path compression**: after finding the master, relink
every cell visited directly to it, making the next find O(1). Vortex will not do that, and the
reason is a principle rather than an oversight:

> **Path compression is a mutation, and every mutation of persistent structure has a name in
> hypertime.** Compressing a path would mint a `SetLink` per cell visited, which means a *read*
> would append to the operations spool, and scrubbing to an earlier microversion would show the
> compression as an edit to the document. R8 exists to forbid exactly this: navigation never
> persists.

So find is $O(k)$ for a rank of $k$ cells, against union-find's near-constant. The mitigation is
already specified and is not a compromise: **compression belongs in an ephemeral pinned island.**
§5.6 of the Vortex spec describes a `d.cache` structure held up by a cursor on `d.pinning-cursors`,
whose cells carry `ephemeralBit` and which the fold *refuses* to write into an operations spool even
if asked. A memoised `deref` lives there. It is path compression with no name in hypertime, released
by breaking one link, and it cannot leak into the document by construction.

That is the general shape of the answer whenever Vlog wants a data structure Prolog would put in the
heap: it goes in a pinned ephemeral island, and the reason it is allowed to exist is that it is
provably not part of the document.

### 4.4 Rational trees are the default, and the occurs check is the opt-in

`X = f(X)` must either be refused (ISO Prolog, with `unify_with_occurs_check/2`), or build a cyclic
term (rational trees, what most implementations actually do by default for `=/2` because the check
costs a traversal per unification).

Vortex builds the cyclic term, and this is a deliberate divergence rather than an accident of
implementation: **a zzstructure is happy with a cycle.** `cloneMaster()` already answers the cell it
started from when the rank loops; a rank that closes on itself is a legal structure the visualizer
will draw. The occurs check is available — it is a traversal of the argument ranks looking for `a` —
and it is opt-in, on the grounds that a manifold which can represent the cyclic term has no reason
to refuse to build it.

The one thing that is *not* optional is that every traversal written for this engine must be
cycle-bounded, the way `cloneMaster()` is. A resolution engine over rational trees will meet cycles
in ordinary operation, not only in pathological programs.

______________________________________________________________________

## 5. Backtracking is already implemented, twice, at two scales

### 5.1 Persistent: a choice point is a microversion

`ops.hpp` says it, about operations rather than about search:

> Every op also names the state it applies to. That is what makes time a graph rather than a line:
> two ops naming the same parent are two futures of the same document, and neither destroys the
> other.

Read that as a description of a choice point and it is complete. Taking a choice point is *reading
the current microversion id* — no write, no allocation. Failing is `Store::rebuildManifold(M)`,
which folds the spool back to that state. Retrying is `apply(M, op)` with a different op, which
branches automatically **because the parent already has a successor**, and the first attempt remains
in the spool untouched.

There is no trail, because nothing was destroyed. There is no undo log, because the log is the whole
history and it only ever grew. The search tree is the hypertime DAG, and `d.ops_dag` — already
declared in the convergence — is the dimension along which you walk it.

### 5.2 Arena: a choice point is a pair of high-water marks

The persistent regime is semantically perfect and operationally ruinous: a binding is a 64-byte
`CompactOpNode`, and a query performing $10^6$ inferences would append tens of megabytes to the
author's operations spool, all of it search scaffolding that is not a document. R8's answer applies
directly — resolution runs in an `ArenaManifold` and only the *answer* is promoted.

An `ArenaManifold` (convergence step 21, **built**: `apps/common/xanadu/zigzag/arena_manifold.hpp`)
holds dense vectors and only ever appends. So:

```cpp
struct Mark {                    // seven lengths, no allocation
  std::uint32_t cellCount, linkSize, contentSize, scratchSize, trailSize;
  std::uint32_t liveLinks, liveContent;
};
Mark mark()               ->  read them
void release(const Mark&) ->  replay the trail tail in reverse, then truncate
void discard(const Mark&) ->  give up the retry, keep the bindings (§5.4)
```

The last two lengths are what this document originally got wrong by omission. `liveLinks` and
`liveContent` are derived counters, and recomputing them on release would make it $O(cells)$ — which
would defeat the whole point, since the reason an arena choice point beats a microversion is that it
is $O(1)$. Carrying them in the mark is exact: every slot above it is discarded and every slot below
it is restored, so the live counts return to precisely what they were and can be assigned back.

**Undo is truncation.** A choice point is four integers and costs no allocation; discarding one on
success costs nothing at all. This is the WAM's cost model reached from the other direction: the WAM
allocates a choice-point frame and pushes trail entries; the arena's structure means the frame *is*
the lengths it would have recorded.

What that `contentArenaSize` is — and what the arena is therefore *not* — is §5.5, because two
plausible readings of it are both wrong and one of them would be an architectural violation.

The symmetry with §5.1 is the point worth keeping: **persistent backtracking scrubs to a state that
has a name; arena backtracking truncates to one that does not.** The difference between the two
regimes is precisely whether the intermediate state deserves a name — and a failed branch of a
search does not.

### 5.3 The WAM's trail condition falls out of the arena rather than being imposed on it

Truncation alone is not enough, and the gap is instructive.

If a cell minted *before* the mark is linked to a cell minted *after* it, truncating leaves the old
cell holding a dangling reference. Binding an old variable is not an edge case — it is what head
unification does on every call. So the arena needs a trail after all, recording the previous
contents of slots it overwrites, and `release()` replays it in reverse.

But it only needs to record writes to cells **older than the mark**, because a write to a younger
cell is undone by the truncation that removes the cell entirely. The condition is one comparison:

```math
\text{trail if } \quad \mathit{cellRef} < \mathit{mark}.\mathit{cellCount}
```

This is the WAM's conditional-trailing rule, verbatim — "trail a binding only if the variable is
older than the most recent choice point" — and it arrives here as a *consequence of the arena being
an arena* rather than as an optimisation someone thought of. The common case in head unification is
binding a variable minted for this clause activation, which is younger than the mark, so it is not
trailed and the trail stays empty through most of a deterministic call.

**A trail entry is a saved `CellSlot`, not a link's previous value.** This document first proposed
`(CellRef cell, DimRef dim, bool negward, CellRef old)` at 16 bytes, and implementing it showed that
is not enough — for a reason only the CSR arena makes visible. A cell that gains a dimension may
have its whole link *run relocated* to the arena's tail, which changes the slot header and no
`DimLink` at all; replaying link values would restore a `linkOffset` pointing above the mark. Saving
the slot covers that, and covers content runs, `valueBits` and `lastOp` in one mechanism:

```cpp
struct TrailEntry {
  std::uint32_t dense;   ///< which slot
  CellSlot saved;        ///< what it held before the write
};
static_assert(sizeof(TrailEntry) == 40);
```

**And trailing a cell copies its runs above the mark.** The header alone is still not sufficient,
which is the second thing the implementation caught: a `DimLink` lives in a shared arena, so
overwriting one *in place* in a run that sits below the mark survives truncation. `release()` would
restore an offset pointing at a run whose contents had already been edited. So trailing relocates
the cell's link and content runs to the arenas' tails — the originals below the mark are left
pristine, every later write lands in territory truncation reclaims, and restoring the header is
enough because the header is the only thing pointing at the copy.

That is copy-on-write per cell per choice point, and §5.3's condition is exactly what bounds how
often it happens: a variable minted for this clause activation is never copied. It also gives a
precise test for "already copied out under this mark" — both runs sitting above the mark's arena
lengths — which is what keeps this at one entry per old cell rather than one per write.

**The cost of a binding is one entry per old cell the edit touches, and an eviction touches one.**
Setting a link displaces whatever the far end held, and the displaced occupant is a cell too. So
binding an old variable whose previous target is also old costs two entries, not one. Easy to forget
when counting what an undo must restore, which is why there is a test named for it.

**Compaction is forbidden while a mark is outstanding.** Not caution — compaction moves every run,
which is precisely what makes an offset recorded in a `Mark` meaningless, so an arena that compacted
inside a choice point would corrupt undo silently. Nothing is lost by waiting: `release()`'s
truncation *is* the reclamation, so the dead runs a failed branch left behind cost nothing. Deferred
compaction and a search that abandons work turn out to want the same thing.

### 5.4 Cut is one comparison against a barrier

Cut discards the choice points created since entry to the current predicate, **without undoing the
bindings made under them**. That distinction is what makes cut awkward in implementations that
conflate the choice-point stack with the undo mechanism.

Here they are already separate. In the arena regime, a cut pops the *mark stack* down to the mark
recorded at predicate entry, and merges each discarded mark's trail segment into the enclosing
mark's rather than replaying it — the trail entries still matter to any outer choice point, they
just no longer have an inner one to serve. No cell is touched and no binding is undone.

In the persistent regime a cut records the entry microversion as a barrier, and the failure handler
refuses to scrub past it. One comparison on a `MicroversionId`.

Both spellings are a change to a stack the engine keeps for itself, so cut — like a choice point —
writes nothing to the manifold.

### 5.5 What the arena is not: no in-memory store, and no in-memory permascroll

"A cheap snapshot of an ephemeral manifold" invites two readings, and both are wrong in ways worth
recording, because the second one would break an invariant rather than merely cost too much.

**Not an in-memory `Store`.** The arena's saving is not disk against RAM; it is *one op per
mutation* against *no ops at all*. R8's ruling is two concrete types:

```cpp
class Manifold;      // ops-backed. Every mutation appends a CompactOpNode.
class ArenaManifold; // dense vectors only. No ops. Dies with its owner.
```

A `Store` held in memory still mints a 64-byte named node per binding and still gives every binding
a microversion. That is §5.1, and §5.1 is *correct* — it is where the inspectable search tree comes
from. Moving it into RAM would keep the whole cost and throw away the only thing the cost buys.

**Not an in-memory permascroll**, and here the reason is structural. A manifold holds no content at
all. Its content arena is

```cpp
std::vector<xanadu::PrimediaSpan> content;   // manifold.hpp
```

— *addresses*, not bytes. So §5.2's `contentArenaSize` is a span arena, which `ArenaManifold` has by
construction and which costs this design nothing. And `PrimediaSpan`'s one-line brief is the whole
argument against the alternative:

> **A run of content at a permanent address.**

An address a backtrack truncates was never permanent. Worse, one span is structurally
indistinguishable from another, so a promoted answer could quote a scratch address — a transclusion
into a scroll that no longer exists. That is not an expensive mistake, it is a silent one, which is
the failure mode the "refuse loudly, by number" habit exists to prevent.

**So how many new bytes does resolution actually need?** Almost none, and the accounting is the
useful part:

| What Vlog does                               | Bytes allocated                                                     |
| -------------------------------------------- | ------------------------------------------------------------------- |
| Mint a variable cell                         | zero — a variable is *bare*; that is §3.2's second conjunct         |
| Unify against a clause in the database       | zero — the program's text is already at a real address              |
| Compare or compute numbers                   | zero — §4.1 compares `scalar_bits`, which live in `CompactOpNode`   |
| Bind to a substring (§9)                     | zero — `value(cell, offset, length)` quotes an address it was given |
| `atom_concat/3`, `number_codes/2`, term→text | **the only case that allocates**                                    |

The third row is the one that had to be checked rather than assumed. R6 gives a scalar cell two
halves — canonical bits in the node, and the shortest round-trip rendering as ordinary spooled
primedia — and insists that neither is a lossy view of the other. But unification and `is/2` read
**only the bits**. So in the arena the rendering is simply *deferred*: mint the bits, and spool the
text at promotion, when the value has earned an address. An arithmetic-heavy Vlog program allocates
no content bytes whatsoever. R6 was not designed with this in mind; it falls out of the two halves
being independent.

**For the row that does allocate: a scratch scroll, refused by the encoding.** Constructed text goes
in a byte buffer belonging to the `ArenaManifold`, and its spans carry a reserved `ScrollId`.
Reserving from the top of the range is the convention already in use:

```cpp
inline constexpr ScrollId breakMarkerScroll = std::numeric_limits<ScrollId>::max();
inline constexpr ScrollId vocabularyScroll  = breakMarkerScroll - 1;
inline constexpr ScrollId scratchScroll     = breakMarkerScroll - 2;   // new
```

`Manifold::applyStructure()` then refuses to persist a span naming it, exactly as it already refuses
an ephemeral cell as a link target:

```cpp
if (isEphemeral(to) || (noCell != to && noDense == denseOf(to))) {   // manifold.cpp, today
```

This is the same trick `ephemeralBit` plays for a `CellRef`, applied to the address side: the
invariant is enforced by the encoding rather than trusted to the caller, which is the only kind of
enforcement that survives a second call site.

And it gives promotion a job it did not have a name for. **`promote()` is where an ephemeral byte
acquires a permanent address**: spool the scratch run into the author's permascroll, rewrite the
span to name it, and the answer leaves the arena addressed like anything else. That is the same pass
the convergence's V3 already describes — *promoting an `ArenaManifold` into a `Manifold` is a CSR
compaction that happens to write ops as it goes* — with the deferred renderings of §5.5's third row
written out along the way.

______________________________________________________________________

## 6. The database

### 6.1 Clauses are a rank on `d.clause`

A predicate is a cell; its clauses are a rank posward from it along **`d.clause`**, the one new
dimension Vlog introduces. Introducing it costs no code: R12 made a dimension a cell on the `d.dims`
rank, so minting one is `MakeCell` plus `SetLink` and nothing in C++ learns its name.

A clause cell's content is unread; its head hangs off `+d.grab` and its body off `+d.spin`, which
means a clause body is an instruction stream in exactly the sense §1 of the Vortex spec already
means — `d.spin` is the process instruction stream, and a conjunction of goals is a rank along it.
Trying the clauses in order is a walk posward. There is no "next clause" pointer to save in a choice
point, because the choice point is a state and the position in the rank is part of that state.

### 6.2 `assertz`, `retract`, and the logical update view for free

`assertz` links a clause cell at the posward tail of the rank. `asserta` links it at the head.
`retract` is `link(clause, ±d.clause, 0)` — which is REARRANGE TO LIMBO, OSMIC's actual name for
deletion, so **the retracted clause is still in the spool** and the microversion before the retract
still has it.

That last fact disposes of the single fiddliest requirement in ISO Prolog. The **logical update
view** says a call must see the clauses as they were when the call was made, regardless of what
subsequent `assert`s and `retract`s do during backtracking into it. Implementations satisfy it with
generation counters on clauses, death stamps, and a reclamation scheme for clauses that are dead but
still visible to an active call.

Here it is: *resolve against the microversion the call entered with.* A generation counter is a
microversion id that the store was already keeping, clause death is a link the spool already
remembers, and reclamation is unnecessary because nothing is reclaimed. The hardest part of the
specification is a consequence of append-only-ness.

### 6.3 First-argument indexing wants random access, which is U1

Selecting candidate clauses by the principal functor of the first argument is what makes Prolog fast
on large fact tables. It wants a hash from a key to a set of clauses.

A rank cannot answer that. Convergence **U1** is exactly this open question: a rank answers
successor and predecessor, not "the *n*-th thing" or "the one matching this key," so clause
selection over a rank of $n$ facts is $O(n)$ per call. VPL hit the same wall from the array side
(`A[5000]` walks five thousand cells) and this is the same workload from the logic side.

The mitigation is §4.3's again — an index is a pinned ephemeral island keyed by the canonical scalar
bits or the functor's content hash, rebuilt lazily and released by breaking one link — and the
honest statement is that **two unrelated consumers now want the same missing primitive from U1**,
approaching it from opposite ends: VPL wants an ordinal and Vlog wants a key. That is the strongest
argument yet for resolving U1 rather than deferring it further, and it is worth more than either
document's own case, because a gap two designs reach independently is a gap in the manifold rather
than a gap in a language.

______________________________________________________________________

## 7. Worked example: `append/3`

```prolog
append([], L, L).
append([H|T], L, [H|R]) :- append(T, L, R).
```

The database, as cells:

```mermaid
graph TD
    P["predicate: 'append/3'"]
    C1["clause 1"]
    C2["clause 2"]
    P -->|"+d.clause"| C1
    C1 -->|"+d.clause"| C2
    C1 -->|"+d.grab"| H1["head: append"]
    C2 -->|"+d.grab"| H2["head: append"]
    C2 -->|"+d.spin"| B2["body: append(T, L, R)"]
```

Solving `append(X, Y, [a, b])`:

1. `mark()` — four integers read, nothing written. Try clause 1.
1. Head unification: `X = []`, `Y = L`, `L = [a, b]`. Three `splice` calls, one `link` each. The
   first binds a caller variable — older than the mark — so it is **trailed**; `L` was minted for
   this activation and is younger, so it is not.
1. Clause 1 succeeds. First solution: `X = []`, `Y = [a, b]`, read off by `render(deref(X))`.
1. The caller asks for another. `release(mark)` — three vector truncations and one trail entry
   replayed. `X` and `Y` are unbound again, and the cells minted for clause 1's variables are gone
   from the arena entirely rather than garbage.
1. Walk `+d.clause` to clause 2, `mark()` again, and recurse. The recursive call's mark sits above
   this one; its own bindings to `T`, `L`, `R` are untrailed for the same reason as step 2.
1. Three more solutions, then the clause rank is exhausted, the last `release` returns the arena to
   its state before the query, and the engine reports failure.

Total persistent writes across the whole query: **zero**, until the caller promotes an answer.

Now the part a conventional Prolog cannot do. Run the same query in the *persistent* regime and
every step above has a microversion id. The failed attempt at clause 1 for a non-empty first
argument is still in the spool; `xudu-dump --section=ops` will render it; `d.ops_dag` walks from the
query's root to each of the four solutions and to each dead end. **The search tree is a document.**
That is an interactive debugger for a logic program, and nobody wrote it — it is what the store does
with every operation it is given.

______________________________________________________________________

## 8. What the C++ core actually has to gain

The brief for Vlog was "as few changes to the C++ core as possible." The answer is:

**Nothing in `ops.hpp`, `compact_op.hpp`, `binary_ops.cpp`, `Store`, the wire format, or any on-disk
format changes at all.** No new `OpKind`, no new `StructureVerb`, no flag bit, no format version
bump, no fixture regeneration.

What was added, almost entirely inside `ArenaManifold` — convergence step 21, now written:

1. `Mark mark()` — reads seven vector lengths (§5.2).
1. `void release(const Mark &)` — replays the trail tail in reverse, then truncates.
1. `void discard(const Mark &)` — cut and success: the retry goes, the bindings stay (§5.4).
1. `std::vector<TrailEntry> trail_` — 40 bytes per entry, not the 16 this document first claimed,
   appended only when the written cell is older than the innermost mark (§5.3).
1. A scratch byte buffer and `scratchScroll`, for the one thing that allocates content (§5.5).

**Version 1.0 claimed the last item was not needed, and that nothing in `Manifold` changed at all.
That was too strong.** `scratchScroll` is a constant in `spool.hpp`, which is free; but a scratch
span must be *refused* on the way into the spool, and the places that refusal belongs are
`Manifold::applyStructure()`, beside the `isEphemeral()` check it already performs, and the three
span-taking entry points on `Store`, beside `setLink()`'s ephemeral throw. Small — a few lines in
functions that already do exactly this for cell refs — but not zero, and a bill revised downward by
leaving items off it is worth nothing.

**What the bill did not have to include is the interesting part.** An arena cell's ref carries
`ephemeralBit`, so `Store::setLink()` throws on one and `applyStructure()` counts one as refused,
which means an arena ref escaping into a document is stopped by machinery written before this class
existed and for an unrelated reason (R12's derived cells). `promote()` works by *mapping* refs
rather than by being trusted with them, and the test for that asserts the refusal comes from a
`Store` that has never heard of `ArenaManifold`.

And the timing argument held. Adding two methods to a class before it is written is free; adding
them to `Manifold` after seventeen fixtures depend on its layout is not. **A speculative feature
that lands its requirements on an unwritten class has found the cheapest moment it will ever have**
— and in the event, the class was written *because* three documents had landed requirements on it,
which is the same observation from the other side.

Everything else — unification, term shape, clause selection, cut, negation as failure, the occurs
check, tabling — is a program over `link` and `value`, per §2.

______________________________________________________________________

## 9. What this buys that a conventional Prolog does not have

- **The search tree is inspectable after the fact.** §7. In the persistent regime every attempted
  derivation has a name and still exists, so "why did this query fail" is a navigation problem
  rather than a re-run-under-a-tracer problem.
- **The logical update view is free.** §6.2, and it is the requirement implementations find hardest.
- **Bindings can be transclusions.** `value(cell, offset, length)` yields an ephemeral cell quoting
  a *range* of another cell's content, so unifying against a substring shares addresses instead of
  copying characters. Prolog copies terms into the heap; here a term is an address, and two terms
  that share a passage are visibly the same passage.
- **Terms have structure Prolog cannot express.** A list can be a rank rather than nested `'.'/2`
  cells; a term can carry links on dimensions the resolution engine never looks at, which is how a
  clause can also be a paragraph in a document.
- **Multiple solutions are a rank.** Enumerating them is walking `d.ops_dag`, so "all solutions" is
  a traversal rather than a control-flow trick, and a solution set can be handed to
  [VQL](vql-query-language.md) or [VPL](vpl-array-language.md) without leaving the manifold.
- **Memo tables have a lifetime that is neither "this query" nor "forever."** §5.6's pinned islands,
  which is what SLG resolution's tables want and what a heap cannot offer.

______________________________________________________________________

## 10. Breakages and costs

Stated in the same spirit as VPL §5 — these are real, and two of them are gating.

1. **`deref` is $O(k)$ in the clone rank's length, not near-constant.** §4.3. Path compression is a
   mutation and therefore refused on persistent structure; the ephemeral-island workaround is
   specified but unbuilt. A program that aliases a long chain of variables pays for it.
1. **Clause indexing needs random access by key, which U1 has not resolved.** §6.3. Without it,
   resolution over a large fact base is linear in the number of facts per call, which is the
   difference between a toy and a usable engine.
1. **Persistent-regime resolution is unaffordable at scale.** §5.2. $10^6$ inferences is $\approx$
   64 MB of operations, and it is *correct* — this is the regime with the inspectable search tree —
   so the interesting question is not how to avoid it but how to bound it. A plausible answer is to
   run in the arena and promote only the derivation of a *found* answer, giving a proof term without
   the dead ends; that is not specified here.
1. **~~Vlog is gated on step 21.~~ No longer: `ArenaManifold`, its choice points and `promote()` are
   built and tested.** What is *not* built is anything above them — no unification, no clause
   database, no solver, so every claim in §4, §6 and §7 is still a claim about a program nobody has
   written. And the arena is not yet an **overlay** over a persistent `Manifold`: it starts empty
   and its cells are its own, so a clause database living in a document cannot be resolved against
   in place. That is the next increment, and VQL's read path wants the same thing.
1. **Rational trees diverge from ISO.** §4.4, deliberately, and it means a program ported from a
   conforming Prolog can loop where it would have failed.
1. **Nothing here addresses the standard library, exceptions, or arithmetic evaluation.** `is/2`
   over scalar cells is straightforward given R6's canonical bits and is simply not written up;
   `catch/3` interacts with the mark stack in ways that want their own section.

______________________________________________________________________

## 11. Reconciliation with the convergence

- **R5 (`noCell == 0`)**: `link(..., 0)` is `retract`, and "there is no cell zero" is why absence
  needs no sentinel in a clause rank.
- **R6 (scalar cells)**: numeric unification is a 64-bit comparison, never a parse, because the
  canonical bits sit in the cell beside the rendering. §5.5 leans harder on the two halves being
  independent than R6 had reason to: in the arena the *rendering* is deferred to promotion, so
  arithmetic allocates no content at all.
- **R7 (micro-history chain)**: a cell's `lastOp` chain means "how did this variable come to be
  bound" is answerable per cell, not only per query.
- **R8 (only user-generated updates persist)**: the governing constraint of §5.2 and §4.3. It is why
  resolution belongs in the arena and why path compression may not touch the document.
- **R11 (bump the version, delete the old reader)**: not invoked. This design bumps no format, which
  is the strongest thing §8 has to say for itself.
- **R12 (a dimension is a cell on `d.dims`)**: `d.clause` costs no code, and §3.2 follows the same
  precedent for variablehood.
- **U1 (random access along a rank)**: §6.3. Now wanted by both VPL and Vlog, for an ordinal and a
  key respectively.
- **U3 (a cell's content is a run of spans)**: `Splice` is what lets a term's content be edited
  without severing the addresses a transclusion-binding shares, so §9's third bullet depends on U3
  having been resolved the way it was.
- **Step 21 (`ArenaManifold` + `promote()`)**: **built**, and §5.2–§5.5 are the record of what it
  had to be. Its remaining half is the overlay over a persistent `Manifold`, per §10.4.

______________________________________________________________________

## Appendix: Versioning and Change History

The rule is the one [VQL](vql-query-language.md) and [Vortex](vortex-hyperstructural-runtime.md)
use:

- **Major** — a change a conforming implementation could not ignore: a mechanism gains, loses or
  changes meaning; a core requirement is added or withdrawn.
- **Minor** — an addition that breaks nothing, a refinement, a clarification, a correction to prose
  in a normative section, punctuation.

Editorial changes that alter no normative text bump neither component.

| version | commit    | date       | change                                                                                                                                                                                                                                  |
| ------- | --------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 3.0     | `c28240c` | 2026-09-11 | `ArenaManifold` implemented, and §5.2/§5.3 corrected by it: a `Mark` is seven lengths, a trail entry is a saved `CellSlot` at 40 bytes, trailing copies a cell's runs above the mark, and compaction is forbidden while a mark is held. |
| 2.0     | `f519ab9` | 2026-09-11 | §5.5: the arena is neither an in-memory store nor an in-memory permascroll — it holds addresses, not bytes. `scratchScroll` added to §8's bill, correcting 1.0's claim that `Manifold` was untouched.                                   |
| 1.1     | `88c9336` | 2026-09-11 | Named **Vlog**, and restyled as an extension rather than a front end (intro, §2). The intro's "no trail" reconciled with §5.3; U1's two consumers separated into ordinal and key.                                                       |
| 1.0     | `734513a` | 2026-09-11 | Initial specification: binding as a clone link, backtracking as truncation.                                                                                                                                                             |
