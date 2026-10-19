# OR- and AND-parallelism: a survey against a per-worker-arena, copy-on-write substrate

This is the concurrency researcher's grounding for `design/vicon-andor-parallelism.md`. It surveys
the classic parallel-logic-programming literature with one filter throughout: **what transfers to a
share-nothing per-worker `ArenaManifold` over one immutable base `Manifold`, where a binding is a
`d.clone` splice, undo is `mark()`/`release()`, and persistence is a single serialized
`promote()`.** Everything is checked against the real engine (`zigzag/vlog.hpp`,
`vortex/vortex_vm.{hpp,cpp}`, `zigzag/arena_manifold.hpp`) and the general study
(`design/vortex-multithreading-study.md`, §6.4/§6.6). It is deliberately mechanism-first: a citation
without a named mechanism is not useful to the design.

The headline finding is stated once up front and defended below: **a copy-on-write arena overlay
over an immutable base is the environment-copying (Muse) model with the copy made lazy and
cell-granular.** The design doc should build on that identification, not reinvent it.

## 0. What the code actually gives us (cite file:line)

- A **binding is a splice of two `d.clone` ranks**. `Vlog::bind(v, t)` splices `v`'s clone rank onto
  the posward tail of `t`'s so the combined master is `t`'s; `deref` is `cloneMaster`
  (`zigzag/vlog.hpp`, struct `Vlog`, `bind`/`deref`). **This is the multiple-binding problem in one
  sentence**: two OR-branches that both want to bind `v` would both splice `v`'s single `d.clone`
  rank, and one arena cannot hold both splices.
- `unify()` returns `std::expected<void, UnifyFailure>` and **leaves partial bindings on failure**
  for the caller's `release()` to truncate — "which is what Prolog does too, and for the same
  reason" (`zigzag/vlog.hpp` doc comment; `UnifyResult`). Rational trees by default; `cloneMaster`
  guards the cycle.
- The VM's choice machinery is C++:
  `ChoicePoint{cursor, altOp, zigzag::Mark mark, stackFrame, cutBarrier}` and a **single global
  `std::vector<ChoicePoint> choiceStack_`** (`vortex/vortex_vm.hpp`). `pushChoicePoint` captures
  `core_.arena().mark()` and sets `cutBarrier = choiceStack_.size()` (`vortex/vortex_vm.cpp:164`).
  `backtrack` pops, calls `core_.arena().release(cp.mark)`, resets the cursor to `altOp` (`:178`).
  `cut(barrier)` pops down to `barrier`, calling `arena().discard(cp.mark)` on each (`:197`) — cut
  keeps bindings, drops the retry. Opcodes `Unify`/`Choice`/`Fail`/`Cut` are at
  `vortex_vm.cpp:789/878/887/899`.
- The arena is the undo engine. `Mark` is twelve `std::uint32_t` high-water lengths and allocates
  nothing; `release(m)` replays the trail tail in reverse then truncates; `discard(m)` keeps
  bindings and drops the retry; `compact()` is **refused while a mark is outstanding**
  (`zigzag/arena_manifold.hpp`, `Mark`, `release`, `discard`, `compact`, `ArenaRefusal`).
- The **overlay is already copy-on-write at cell granularity**: an arena over a base `Manifold`
  answers reads of a cell it does not hold by falling through to the base, and the first *write* to
  one `shadow()`s it — the whole slot, its link run and content run copied into the arena, keeping
  the base's `CellRef` as its name (`arena_manifold.hpp`, `shadow()` and the class doc comment). A
  `Mark::shadowCount` records how many base cells had been shadowed so `release()` drops the shadows
  a failed branch took.
- `promote(store, parent, from, root, budget)` is a reachability walk from the answer root —
  "promote the answer, not the search" — mapping arena refs to persistent refs
  (`arena_manifold.hpp`, `promote` doc). It is the one road across the `ephemeralBit` boundary.

Everything below is read against these facts.

## 1. The multiple-binding problem and its canonical solutions

### 1.1 Statement

In sequential WAM Prolog a logic variable is a single mutable cell on a shared heap, conditionally
bound and trailed so backtracking can unbind it. Under OR-parallelism, two branches explore
different clauses for the same call *at the same time*; each may bind a shared ancestor variable to
a different term. One mutable cell cannot represent both bindings simultaneously. This is the
**multiple-binding problem** (Gupta & Jayaraman 1990; Lusk et al., *The Aurora or-parallel Prolog
system*, 1990). Every OR-parallel model is, at bottom, a different answer to "where does a branch's
private binding of a shared variable live?"

The deep result the design must respect is Gupta & Jayaraman's **three-criteria impossibility**: no
OR-parallel model implementing full Prolog semantics can make all three of these constant-time at
once (confirmed verbatim, Pontelli, *Overheads in Or-Parallelism*, citing Gupta & Jayaraman):

1. **Task creation** — building the environment/goal-list for a new alternative at a choice point.
1. **Task switching** — moving a worker from one or-tree node to another.
1. **Variable access** — reading or binding a variable.

Any model pushes its non-constant cost into at least one phase. The practical winners (Muse, Aurora)
pay it in **task switching**, because the scheduler controls task-switch frequency whereas
variable-access and task-creation frequency are set by the program. **This is the single most
important design constraint to carry forward**: the Vortex design should also push its unavoidable
non-constant cost into task switching / fork, which it can control, not into deref or bind, which it
cannot.

### 1.2 The five canonical solutions, with tradeoffs

**(a) Binding arrays — the SRI model (Aurora).** Each worker owns a private *binding array* indexed
by a dense counter assigned to each conditional variable. A variable cell holds its array offset;
its value for *this* worker is `BA[offset]`. Branches share the unbound structure but each reads its
own value through its own array. Deref is one extra indirection (near-constant variable access); the
non-constant cost is at **task switch**, where a worker moving to another node must install or
deinstall the differing bindings between the two nodes by walking the trail segment between them.
(Lusk, Warren, et al., *Aurora*, 1990; Warren's SRI model.)

- Space: one array slot per conditional variable per worker. Time: O(1) deref, O(trail-between-
  nodes) task switch.

**(b) Environment / stack copying (Muse).** No shared mutable binding store at all. A worker that
takes work from another node **copies the other's stacks** (or the differing portion) into its own
local address space, then runs an ordinary sequential engine. Each engine binds in its *own* copy,
so the multiple-binding problem is dissolved by construction — there is no shared cell to bind
twice. Variable access is exactly sequential (fully constant, no indirection); the non-constant cost
is the **copy at task creation/switch**. Muse copies only the segment above the shared node
("incremental copying"), and reports 8–22% overhead over sequential SICStus (Ali & Karlsson 1990;
Pontelli, *Overheads*). Environment copying is the model later systems (YapOr, Santos Costa et al.;
ACE, Gupta et al. 1994) standardized on, "as first implemented in Muse."

- Space: a private near-complete environment per active branch. Time: O(1) deref, O(copied-segment)
  fork.

**(c) Versions-vectors / hashing windows.** A middle road: a shared variable carries a small
structure (a vector indexed by branch, or a *hash window* — AKL's term, Montelius et al.) recording
each branch's binding of it. The AKL implementation separates a scheduled branch from its origin by
a **hash window in which bindings to shared variables are saved** (*A parallel implementation for
AKL*, 1993). Deref hashes/looks up the current branch; access is no longer constant and the window
grows with contention.

- Space: O(branches) per contended variable. Time: non-constant **variable access** (the phase you
  least want to pay in).

**(d) Binding directories / directory trees.** A tree mirroring the or-tree; a node records the
bindings local to that branch, and deref walks up to find the nearest binding. Shares structure
cheaply (cheap task creation) but deref is O(tree-depth). Pushes the non-constant cost onto
**variable access** again. Largely abandoned for exactly that reason.

**(e) Composition tree / paged binding arrays (combined AND-OR).** Gupta & Jayaraman's
Composition-tree with paged binding arrays extends (a) to represent the AND-OR cross-product and to
support cut and side-effects (*And-or parallelism in full Prolog with paged Binding Arrays*, 1992).
Relevant because it is the one classic model that treats cut/side-effects as first-class rather than
as an afterthought — see §4.

### 1.3 Which one a copy-on-write overlay most resembles

**The arena overlay is environment copying (Muse), with two refinements the 1990 systems did not
have:**

- **The copy is lazy and cell-granular, not eager and segment-granular.** Muse copies the differing
  stack segment at fork. The arena copies *nothing* at fork — a child arena over the same frozen
  base shares every cell by read-through — and copies one cell only on the first write to it
  (`shadow()` in `arena_manifold.hpp`). This is strictly better than Muse's incremental copying on
  the task-creation axis: fork is O(1) (a new arena header + base pointer), and the per-cell copy is
  paid only for cells a branch actually binds. In Gupta-Jayaraman terms, Vortex has moved even more
  of the cost off task creation and onto first-touch, which is a subset of variable access — but a
  *write*, not a read, so deref stays O(cloneMaster-walk) with no per-branch indirection.
- **The base is immutable, so there is no install/deinstall and no shared mutable binding array.**
  The reason Aurora pays at task switch is that its binding array is mutable and must be reconciled
  when a worker relocates. The general study forbids worker migration of a running cursor (§3.2), so
  there is no task-switch reconciliation to pay — a branch is pinned to the worker (and arena) that
  forked it. This is why the study can claim backtracking becomes "per-worker for free" (§3.4): each
  worker's `mark()`/`release()` names *its own* dense vectors, and a binding in one worker's arena
  is invisible to another **by construction**.

The honest cost statement for the design: **the model is Muse-shaped, so it inherits Muse's cost
profile — O(1) deref, non-constant cost at fork — but the arena makes the fork cost lazy (per
written cell) instead of eager (per segment).** The binding-array alternative is *not* available
without reintroducing a shared mutable store, which the share-nothing invariant and the
`ephemeralBit` guard both forbid. There is nothing to decide here; the substrate already chose
environment copying.

```text
multiple-binding problem      how the arena model answers it
---------------------------   ----------------------------------------------
two branches bind v           two child arenas over one frozen base
differently, at once          each shadows v's cell on first write; the
                              d.clone splice is private to that arena.
                              The base's v is never mutated. No shared
                              binding cell exists to be bound twice.
```

## 2. OR-parallel scheduling: granularity, speculation, voluntary vs. eager

### 2.1 Granularity and the cost of a fork

The three-criteria result says the model controls only task-switch/fork frequency, so **the whole
performance question is "when is a fork worth it?"** Classic answers:

- **Mark choice points, fork lazily.** Aurora/Muse mark parallel choice points at near-zero cost and
  only *materialize* a parallel task when an idle worker asks for it (Pontelli, *Overheads*: "as
  long as parallelism is not exploited, choice points are treated normally, without any overhead").
  This is **lazy task creation** (Mohr, Kranz & Halstead 1991, from Mul-T): run the alternative
  inline on the current worker, and only *promote* it to a stealable task if another worker is idle.
- **Work-stealing deques** (Cilk, Blumofe & Leiserson 1995; TBB; Go's M:N scheduler, which the
  general study §5 names as the scheduler shape to copy): each worker pushes/pops its own end of a
  deque LIFO; idle workers steal from the *other* end FIFO (oldest = largest subtree, cheapest to
  steal relative to work gained). This is the standard answer to granularity and the general study
  already recommends it.

For Vortex: a choice point is already a near-free `mark()` (`arena_manifold.hpp`: "allocates
nothing", lets "a resolution engine take a million of them"). So **marking parallel choice points is
free; the only question is forking a child arena**, and the fork cost is the lazy per-written-cell
shadow of §1.3. The granularity threshold ("fork only subtrees expected to do more work than the
fork + eventual `promote()` costs") belongs in `system://settings`, not as a magic number (AGENTS.md
governance).

### 2.2 Speculative vs. mandatory work

A branch is **mandatory** if the program's observable result needs it (e.g. `findall/3` collects
*all* solutions — every branch is wanted). A branch is **speculative** if it may be pruned before
its result is used — anything to the right of a possible cut, or any alternative after the first in
a `once`/first-solution context. The distinction is from the OR-parallel scheduling literature
(Beaumont & Warren; Ciepielewski) and matters because:

- Speculative work can be *wasted* (the branch is cut before it finishes) — so a scheduler should
  prefer mandatory work and only run speculative work on otherwise-idle workers ("speculative work
  is better than idling, worse than mandatory work").
- Speculative work with **side-effects** is dangerous: a `write/1` or an assert in a branch that is
  later cut must not have been observed. Classic Prolog handles this with *speculative-work
  suspension* — a side-effecting goal in a speculative branch suspends until the branch becomes
  leftmost (and therefore mandatory).

For Vortex this maps cleanly onto the existing `ephemeralBit`/`promote()` split: **a speculative
branch's work is confined to its arena and becomes observable only at `promote()`.** The engine
already "promotes the answer, not the search" — an abandoned branch's cells are simply never
reachable from an answer root and are never promoted (`arena_manifold.hpp`, `promote` doc). So the
side-effect hazard is dissolved for *structural* effects: a cut branch mints no operations because
it never promotes. The residual hazard is **non-structural** side-effects (I/O, a `write`), which
have no arena representation; those must follow the classic rule — suspend until leftmost/mandatory,
serialized at the merge point (exactly vlace's `IO`-serialized-at-merge rule, general study §6.6).

### 2.3 Voluntary vs. eager

- **Eager** (push model): a worker reaching a parallel choice point immediately spawns tasks for all
  alternatives. Simple, but over-forks fine-grained work.
- **Voluntary** (pull model, "scheduling on demand" — Muse, Aurora): the busy worker keeps the
  alternatives as ordinary choice points and runs them inline; an *idle* worker pulls one. This is
  the dominant design and composes with work-stealing deques. The general study's "fork a worker
  overlay at a choice point" (§6.4) should be read as voluntary: fork only when an idle worker
  pulls.

## 3. AND-parallelism

### 3.1 Independent AND-parallelism (&-Prolog, RAP, CDG)

Conjoined subgoals `a, b` can run in parallel iff they do not compete to bind the same variable —
**independence**. DeGroot's Restricted AND-Parallelism (RAP, 1984) and Hermenegildo's &-Prolog
(Hermenegildo & Greene 1991) decide independence with **Conditional Dependency/Graph Expressions
(CGE/CDG)**: compile-time analysis emits a guard like `(ground(X), indep(Y,Z) -> a & b ; a, b)` so
the parallel conjunction fires only when the runtime check confirms the shared variables are ground
or unaliased (NMSU LP notes, *And-parallelism*; Shen & Hermenegildo, 1996). **Determinacy analysis**
is the companion: a subgoal known to produce exactly one solution needs no cross-product join.

For Vortex: independence analysis is a *static* property of the compiled clause and does not touch
the substrate — it decides whether two subgoals *may* fork. The attractive case is the one the
general study already names (§6.4): "AND-parallelism (independent conjuncts) comes later and only
where independence is provable." The arena model makes "provably independent" sharper than in WAM:
two subgoals are independent iff the sets of cells each would `shadow()` are disjoint. If they are
disjoint, they can run in **two child arenas over the same frozen base** and their results merged
with no binding conflict — this is independent AND-parallelism expressed as non-overlapping
copy-on-write write-sets.

### 3.2 Dependent AND-parallelism (DASWAM, producer-consumer)

When conjuncts *share an unbound variable*, one may bind it (producer) while the other reads it
(consumer). DASWAM (Shen & Hermenegildo, *Flexible Scheduling for Non-Deterministic And-parallel
Execution*, EuroPar 1996; Shen's thesis) supports this with a **distributed stack** — each worker's
stack physically separate but logically one — plus extra data structures to link fork/join, and a
**shared mutable binding** that the producer writes and the consumer observes. This is precisely the
shared-mutable-trail hazard: a binding made on one worker must become visible on another, which the
share-nothing arena model forbids by construction.

**Honest conclusion for Vortex: dependent AND-parallelism is provably out of reach of the
share-nothing per-worker-arena model without a shared mutable binding channel.** The two options are
both bad fits:

- Reintroduce a shared mutable cell for the shared variable → breaks the share-nothing invariant,
  needs locking/atomics on a `d.clone` splice, and defeats the reason the arena dissolves the
  multiple-binding problem.
- Message-pass the producer's binding to the consumer's arena → serializes the dependency, which is
  just sequential execution with extra copying.

The design should state this as a hard limit: **AND-parallelism in Vortex is independent-only
(disjoint shadow-sets); dependent AND-parallelism stays serial.** This matches the general study's
§4.3/§6.4 and is the same ruling it gives vicon's generators ("do not split one search across
workers").

### 3.3 The solution-join / cross-product problem

Even for *independent* conjuncts, if `a` has solutions {a1, a2} and `b` has {b1, b2}, the
conjunction `a, b` must yield the **cross-product** {(a1,b1),(a1,b2),(a2,b1),(a2,b2)} — and in
Prolog's *observable left-to-right order*. Naively recomputing one side per solution of the other is
the redundant-computation trap Gupta & Jayaraman's combined AND-OR model (1988) was built to avoid:
their extended and-or tree "represents the cross-product of the solutions ... rather than
re-computing it."

For Vortex the cross-product is a **join over two solution streams, each a list of promoted (or
promotable) answer roots tagged by submission index.** The general study already fixes the ordering
rule (§3.5, §6.4): results carry a stable submission index and the host concatenates/joins in that
order, so the parallel cross-product reproduces the serial order deterministically. The join itself
is an ordinary nested iteration over two index-ordered vectors — and because each solution is an
arena subgraph reachable from a root, the join does not recompute either side; it pairs roots. Where
cells can carry this: the two solution streams are naturally two **ranks** (a `d.solutions` rank per
conjunct), and the cross-product is their rank-product — structure doing the bookkeeping.

## 4. Cross-cutting concerns

### 4.1 Cut under parallel branches = cancellation

Serial `cut(barrier)` pops the choice stack down to `barrier`, `discard()`ing each mark
(`vortex_vm.cpp:197`). Under OR-parallelism the branches to the left of the cut (the ones it prunes)
**may be running on other workers**, and `cutBarrier` is an index into what is now a per-worker
stack (general study §6.5). So cut becomes a **cancellation** problem: the cutting branch must stop
and reclaim sibling branches that the serial semantics would never have explored.

The classic treatment (Aurora/Muse cut; Hausman's speculative cut) is: a cut signals all speculative
branches to its right (and, for a hard cut, siblings to its left in the pruned subtree) to
**abort**; a branch checks the signal at choice points and unwinds. The modern C++ primitive for
exactly this is `std::stop_token` / `std::jthread` (C++23, available directly — not via the cpp26
facade, which only covers `function_ref`/`optional`/`inplace_vector`/`concat`/`span_at`, confirmed
in `include/gleditor/cpp26_select.hpp`): the cutting worker calls `request_stop()` on the pruned
branches' `std::stop_source`; each worker polls `stop_requested()` at its choice-point loop and, on
seeing it, `release()`s its arena marks and exits. Because an aborted branch's arena is thrown away
whole, **cancellation reclaims its bindings for free** — there is no partial state to repair, which
is the arena's "cells minted under the mark are *gone*, not garbage" property (`arena_manifold.hpp`,
`release`).

Subtlety the design must nail: **cut's prune set is defined by submission order, not wall-clock
order.** A worker that has already *found* a solution on a to-be-cut branch must have that solution
suppressed if a lower-submission-index cut fires. This is why speculative branches must not publish
(promote) until they are confirmed leftmost/mandatory (§2.2) — the same rule, seen from the cut
side.

### 4.2 Observable ordering vs. set-equality

Pure logic programs are **set-equal** under reordering: the *set* of solutions is independent of
exploration order. Full Prolog is **sequence-equal**: `findall` order, first-solution, cut, and
side-effects all observe the left-to-right *sequence*. The general study's promise is sequence-
equality ("the same solution set and first-solution as the serial engine, for programs including cut
and negation", §6.4). The mechanism is the **submission index**: fork assigns each branch the index
it would have in serial depth-first order; the host merges by that index; first-solution is the
lowest-index branch that succeeds (not the first in wall-clock time). This is a deterministic merge
over a nondeterministic schedule — the single most important property to preserve, and it is cheap
because the index is assigned at fork and never recomputed.

### 4.3 Failure and garbage of abandoned branches

In WAM systems an abandoned branch leaves trail and heap garbage that a parallel GC must reclaim
without stop-the-world (a hard problem; Aurora/Muse both needed custom GC). **The arena model makes
this a non-problem**: an abandoned branch *is* its arena, and discarding the arena (or `release()`
to a mark) frees everything at once with no scan — "release() truncates the dead runs away by
itself, so the runs a failed branch left behind cost nothing to reclaim" (`arena_manifold.hpp`,
`compact` doc). Per-worker arenas mean per-worker reclamation, no cross-worker GC, no stop-the-world
(general study §6.5, "per-worker, no stop-the-world"). This is a genuine, structural advantage over
the classic systems and the design should claim it explicitly.

## 5. Goal-directed / Icon-specific parallelism — and where it is thin

The literature here is **sparse and must be reported honestly.** Icon's goal-directed evaluation
(Griswold; Arizona TRs 80-21, 81-1a, 87-6) is the direct ancestor of vicon's "every expression is a
generator": a generator suspends and resumes to produce a sequence, and expression evaluation drives
*control backtracking* exactly as Prolog resolution does. But:

- **The canonical Icon implementations are resolutely sequential.** The Icon TRs describe a
  "straightforward and efficient implementation in **stack-based** languages" (TR 81-9) — a single
  expression-evaluation stack with suspension frames, no parallelism. Jcon (Proebsting et al., TR
  99-13) is a Java reimplementation whose novelty is a "simple mechanism for realizing Icon
  generators" object-by-object — again sequential. There is **no classic parallel Icon** in the
  Aurora/Muse sense.
- **The nearest thing is Junicon** (*Embedding Concurrent Generators*, HIPS 2016): it embeds Icon's
  goal-directed generators in a concurrent host (JVM/Scala) and makes *co-expressions* into
  concurrent threads with explicit synchronization. This is the one source that treats
  co-expressions as a concurrency unit, and it treats them as **actor-like islands with
  message-passing activation**, not as migratable stacks — which is exactly the general study's
  "worker-pinned or a message back to the creating worker" ruling for `create e` / `@` (§6.6). Cite
  Junicon as the precedent that the pinned-island treatment is the known-good one, not an invention.

**Why the literature is thin, and what vicon contributes.** Prolog's parallelism research had a
decade of funded systems (Gigalips, PEPSys, ACE) because resolution's OR/AND structure is uniform.
Icon's generators are *more* pervasive (every expression, not just clause calls) which makes naive
parallelism *harder*, not easier — the granularity of `1 to 3` or `i < j` is far below any sensible
fork threshold. So the honest position for the design is: **vicon's OR-parallelism is the same
mechanism as vprolog's (a `#Choice` is a `#Choice`, general study §6.6), restricted to the coarse
arms where it pays — alternation `e1 | e2`, not every suspension** — and the contribution is mapping
Icon's three special constructs onto the arena substrate:

- **`d.scan` scanning state** (`&subject`/`&pos`) is per-worker arena state, like `+d.cursors` — a
  rank in each worker's own arena, automatic under share-nothing (general study §6.6). Two scans on
  two workers cannot corrupt each other's `&pos` because the ranks are in different arenas.
- **Reversible assignment `<-`** is an ordinary `value()`/`setValueBits` write under a live mark,
  undone by the trail (`arena_manifold.hpp`, `TrailEntry`, `trail()`). It is a per-worker-trail
  effect and therefore confines a generator to one worker for its life — the same ruling as
  dependent conjuncts (§3.2).
- **Co-expressions `create e` / `@`** are a suspended choice stack + arena mark held as a **pinned
  island** on a `d.pinning-cursors` rank. Activating one on another worker would migrate live choice
  state (forbidden, §3.2), so activation is worker-pinned or a message to the creating worker — the
  Junicon shape. A co-expression is thus a natural **cell** (its island root) that can be *shipped
  as a message* (its arena subgraph), never run in place by a foreign worker.

The bounded/unbounded distinction (a generator differs only by whether a `discard(mark)` is emitted
at its boundary) tells the scheduler how much speculative OR work a vicon expression is worth
forking: an unbounded generator keeps its mark live (more speculation potential, more cancellation
risk); a bounded one `discard`s and commits.

## 6. Transfer table

For each classic technique: does the Vortex per-worker-arena / copy-on-write / cell-and-dimension
model already provide it (**yes**), partially (**partial**), or not (**no**)?

| Technique (source)                                | Provided? | Mechanism in the arena model / what is missing                                                                                                                              |
| ------------------------------------------------- | --------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Multiple-binding isolation (all OR models)        | yes       | Per-worker child arena over one frozen base; a `d.clone` splice is private to its arena; the base `v` is never mutated. Dissolved by construction.                          |
| Environment copying (Muse, Ali & Karlsson 1990)   | yes       | The CoW overlay **is** environment copying, made lazy + cell-granular: fork copies nothing, `shadow()` copies one cell on first write (`arena_manifold.hpp`).               |
| Incremental copying (Muse)                        | yes+      | Better than incremental: not "copy the differing segment at fork" but "copy a cell at first write." Strictly fewer copies.                                                  |
| Binding arrays / SRI model (Aurora 1990)          | no        | Needs a shared mutable per-worker array reconciled at task switch; share-nothing + no cursor migration (§3.2) removes both the array and the reconciliation. Not wanted.    |
| Versions-vectors / hashing windows (AKL 1993)     | no        | Would reintroduce non-constant **variable access**; the arena keeps deref = `cloneMaster` with no per-branch indirection. Deliberately not adopted.                         |
| Binding directory trees                           | no        | Same reason — O(tree-depth) deref. The overlay gives O(1) read-through instead.                                                                                             |
| O(1) choice-point creation (Muse/Aurora marking)  | yes       | `mark()` reads twelve lengths, allocates nothing (`arena_manifold.hpp`); "take a million of them."                                                                          |
| Lazy task creation (Mohr et al. 1991)             | partial   | Choice points are already free to mark; the *fork of a child arena* must be made lazy/voluntary by the scheduler (new work, consistent with §6.4 "fork at a choice point"). |
| Work-stealing deque (Cilk; Go)                    | partial   | The scheduler shape the general study recommends (§5); not yet built. The arena/promote substrate underneath is ready.                                                      |
| Speculative-vs-mandatory suspension               | partial   | Structural effects dissolved — a cut branch never promotes, so mints nothing. Non-structural I/O must still suspend-until-leftmost (serialize at merge), not yet built.     |
| Independent AND (RAP/CGE, &-Prolog)               | partial   | "Independent" = disjoint `shadow()` write-sets → two child arenas, merge without conflict. The static CGE/determinacy analysis to *prove* disjointness is not built.        |
| Dependent AND (DASWAM, Shen & Hermenegildo)       | no        | Needs a shared mutable binding (producer→consumer); forbidden by share-nothing. **Provably independent-only; dependent AND stays serial.**                                  |
| Solution cross-product without recompute (G&J)    | partial   | Each solution is an arena subgraph reachable from a root; the join pairs roots (two `d.solutions` ranks → rank-product), no recompute. The join driver is not built.        |
| Cut as cancellation across workers                | partial   | `std::stop_token`/`std::jthread` (C++23, direct); abort → `release()` the aborted arena, bindings gone for free. The signal/poll plumbing is not built.                     |
| Deterministic order / submission index            | partial   | The rule is fixed by the general study (§3.5/§6.4); fork assigns the index, host merges by it. The index-assigning fork and index-ordered merge are not built.              |
| Parallel GC of abandoned branches (Aurora/Muse)   | yes       | Non-problem: an abandoned branch is its arena; `release()`/discard frees it with no scan, per-worker, no stop-the-world (`arena_manifold.hpp`, §6.5).                       |
| `d.scan` scanning state isolation (vicon/Icon)    | yes       | A `d.scan` rank lives in each worker's own arena; share-nothing makes two scans non-interfering automatically (§6.6).                                                       |
| Reversible assignment `<-` across a fork (vicon)  | no        | A per-worker-trail effect → confines a generator to one worker; cannot cross a fork. Same ruling as dependent conjuncts. By design, not a gap.                              |
| Co-expression activation across workers (Junicon) | partial   | A pinned island (choice stack + mark) on `d.pinning-cursors`; activation worker-pinned or message-passed (Junicon shape). Message-passing activation is not built.          |

Legend: **yes** = the substrate provides it today; **yes+** = provides a strictly better variant;
**partial** = the substrate's hard part is done, named scheduler/analysis/plumbing work remains;
**no** = deliberately not adopted or provably out of reach, with the reason stated.

## 7. Sources

- Lusk, Warren, et al., *The Aurora or-parallel Prolog system*, New Generation Computing, 1990 (SRI
  binding arrays).
- Ali & Karlsson, *The Muse approach to OR-parallel Prolog* / *Full Prolog and scheduling
  or-parallelism in Muse*, Intl. J. Parallel Programming, 1990 (environment / incremental copying).
- Gupta & Jayaraman, *Combined And-Or Parallel Execution of Logic Programs*, UNC TR 88-012, 1988;
  and *And-or parallelism in full Prolog with paged Binding Arrays*, 1992 (three-criteria
  impossibility; composition tree; cut/side-effects).
- Pontelli, *Overheads in Or-Parallelism* (NMSU LP course notes) — confirms the Gupta-Jayaraman
  three-phase result and the Muse/Aurora 8–22% / 25% overheads.
- DeGroot, *Restricted And-Parallelism (RAP)*, 1984; Hermenegildo & Greene, *&-Prolog*, 1991; Shen &
  Hermenegildo, *Flexible Scheduling for Non-Deterministic And-parallel Execution* (EuroPar 1996)
  and the DASWAM dependent-AND model.
- Montelius et al., *A parallel implementation for AKL*, 1993 (hash windows).
- Santos Costa et al., *YapOr* (1999) and *ACE: And/or-parallel copying-based execution* (Gupta et
  al. 1994) — environment copying standardized.
- Blumofe & Leiserson, *Cilk* work-stealing, 1995; Mohr, Kranz & Halstead, *Lazy task creation*,
  1991; the Go runtime M:N scheduler (general study §5).
- Griswold et al., Icon project technical reports (Arizona TR 80-21, 81-1a, 81-9, 87-6); Proebsting
  et al., *Jcon*, TR 99-13; *Embedding Concurrent Generators* (Junicon), HIPS 2016 — goal-directed
  evaluation, sequential canonical implementations, co-expressions as concurrent islands.
- Code: `apps/common/xanadu/zigzag/vlog.hpp`, `apps/common/xanadu/zigzag/arena_manifold.hpp`,
  `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}` (lines cited inline);
  `include/gleditor/cpp26_select.hpp` (facade scope); `design/vortex-multithreading-study.md`
  §3.2/§3.4/§3.5/§5/§6.4/§6.5/§6.6; `design/_andor_briefing.md`.
