# AND/OR parallelism and vicon's concurrency challenges

This is a focused, code-grounded design for parallelising AND/OR search in the threaded Vortex
model, and for vicon's unique concurrency traits, as the depth-first child of the general
multithreading study's deferred Stage 5
([`vortex-multithreading-study.md`](vortex-multithreading-study.md) §6.4 vprolog, §6.6 vicon). It
inherits that study's architecture without re-deriving it: share-nothing per-worker `ArenaManifold`s
over one immutable base `Manifold`, an M:N scheduler modeled on `render::WorkerPool`, per-worker
`choiceStack_`/trail, and a single serialized `promote()` merge. This doc designs the one thing that
study explicitly left for last — parallel search in the goal-directed languages — and no more.

It is the synthesis of four code-verified section inputs, each of which it cites and does not
repeat: [research](_andor_section_research.md) (the parallel-logic-programming literature filtered
through the arena substrate), [runtime](_andor_section_runtime.md) (what executes, suspends, ships,
and cancels a branch), [structure](_andor_section_structure.md) (the answer told as cells and
dimensions, and where C++ is unavoidable), and [C++](_andor_section_cpp.md) (which standard-library
primitive expresses each piece and whether the toolchain ships it). The shared, code-verified basis
is [`_andor_briefing.md`](_andor_briefing.md). Every code claim cites `file:line` against
`apps/common/xanadu/zigzag/vlog.hpp`, `apps/common/xanadu/vortex/vortex_vm.{hpp,cpp}`, and
`apps/common/xanadu/zigzag/arena_manifold.hpp`.

## 0. The problem and the one-line answer

Two intertwined problems the general study named but did not design (briefing):

1. **OR-parallelism** — explore alternative choices (clauses in vprolog/Vlog; alternation arms and
   generator resumptions in vicon) on different workers at once. The classic hazard is the
   **multiple-binding problem**: two OR-branches bind the same variable to different terms
   simultaneously, and one shared binding environment cannot hold both.
1. **AND-parallelism** — solve conjoined subgoals (`a, b`; sequenced generators) on different
   workers. The classic hazards are **independent vs. dependent** (dependent conjuncts share a
   mutable trail) and solution **cross-product** ordering.

And vicon's traits on top of both, because *every expression is a generator*, not just clause
resolution: `d.scan` scanning state, reversible assignment `<-`, and co-expressions as pinned
islands.

**The one-line architecture.** A speculative OR-branch is a
`(suspended ChoicePoint, private copy-on-write ArenaManifold, submission index, stop_token)`; the
OR-tree that relates branches is ephemeral store structure on four coordination dimensions; branches
are created lazily and run each on exactly one worker for life; cut and first-solution-wins are
cancellation (stop a stolen subtree, drop its arena); AND-parallelism is independent-only (disjoint
shadow-sets); and vicon runs a single generator chain on one worker, forking only coarse alternation
arms. The arena model does the isolation; the runtime only schedules and cancels; the search is
structure and the leaves are state.

The four experts do not disagree on this model; they describe it from four sides, and the few places
they offer alternatives are decided below.

## 1. The OR-parallel mechanism: a private arena per branch

### 1.1 The decision

**Each OR-branch gets its own `ArenaManifold`, a copy-on-write overlay over the one immutable base
`Manifold`, and that private arena is its entire binding store.** This is not a new type and not a
new mechanism; it is the arena the engine already has, instantiated once per live branch instead of
once per worker. The general study localizes the arena to the worker (§3.1); this design subdivides
further — a worker that forks a speculative branch hands it its own overlay, because two branches
sharing one arena is exactly the collision of §1.2.

Why this and not binding arrays (Aurora/SRI) or hashing windows (AKL): both reintroduce a shared
mutable per-branch store that must be reconciled at task switch, and the share-nothing invariant
plus the `ephemeralBit` guard forbid shared mutable manifold state on the hot path (research §1.3;
study §2.2). The arena overlay pays the multiple-binding cost the one way the substrate allows —
lazily, per written cell — and the study's no-cursor-migration rule (§3.2) removes the task-switch
reconciliation that is the reason Aurora pays at all. The substrate already chose environment
copying; there is nothing to re-decide, only to instantiate per branch.

### 1.2 Why two branches collide in one arena, exactly

A binding is one link on the `d.clone` rank. `Vlog::bind(v, t)` splices `v`'s whole `d.clone` rank
onto the posward tail of `t`'s so the combined master is `t`'s (`Vlog::bind`, `vlog.cpp:86`, the
splice at `:91-93`), and `deref` is nothing but `cloneMaster` walking that rank (`vlog.hpp:81`). A
ZigZag cell has at most one neighbour per direction per dimension, so `v` has exactly one `d.clone`
slot each way. If branch A wants `v = t1` and branch B wants `v = t2`, both must consume `v`'s
single `d.clone` slot and the second would evict the first. There is no "copy of `v`" to bind
differently, because a cell's identity *is* its slot. That is the Aurora/Muse multiple-binding
problem stated in this codebase (structure §1).

The serial engine never sees it because it holds one binding live at a time: `#Choice` takes a
`mark()` (`pushChoicePoint`, `vortex_vm.cpp:164`, capturing `core_.arena().mark()` at `:172`),
`#Fail` calls `release(cp.mark)` to truncate the splice away (`backtrack`, `vortex_vm.cpp:183`), and
only then does the alternative splice into the now-free slot. The single global
`std::vector<ChoicePoint> choiceStack_` (`vortex_vm.hpp:151`) and the single arena enforce
one-branch-at-a-time as an accident of being single. Parallelism removes the accident, which is why
the collision becomes visible precisely when we go fast.

### 1.3 How a branch is spawned and its arena constructed

The sequence, decided:

1. **Reach a parallel choice point.** The VM reaches a `#Choice` with an untried `altOp` (opcode
   handler `vortex_vm.cpp:878`, which calls `pushChoicePoint` at `:164`). Serially this is one
   `pushChoicePoint`. Under this design the alternative is left as a cheap *spark* on the owning
   worker's deque — reified into a real branch only if an idle worker steals it (lazy task creation,
   runtime §1.3; the common case forks nothing).
1. **A steal reifies the branch.** On a steal, construct a new `ArenaManifold` over the *same frozen
   base* the parent overlays — the base is read-only, shared by all branches with no copy and no
   lock (study §3.3). Construction allocates an arena header and a base pointer; it copies no cells.
1. **First write shadows.** The instant the branch calls `bind(v, t)`, the first write to `v`'s slot
   triggers `shadow(v)`: the branch's arena takes a private copy of `v`'s whole slot — its link run
   and content run — *keeping the base's `CellRef` as its name* — and splices the `d.clone` edge in
   its own copy (`arena_manifold.hpp`, the overlay contract; `Mark::shadowCount` counts the shadows
   taken). The base's `v` is never mutated. Each branch now has its own `v` with its own one
   `d.clone` slot. The collision of §1.2 is dissolved: there are two slots named `v`, one per arena.
1. **The branch carries its arena, never migrates while running.** A `Mark` is twelve `uint32_t`
   high-water lengths into *one arena's* dense vectors (the `Mark` struct in `arena_manifold.hpp`),
   so a mark from arena A is meaningless in arena B. A *suspended* branch (a `ChoicePoint` plus its
   arena) can be stolen before it is resumed; a *running* cursor's arena cannot cross threads (study
   §3.2). Steal-the-suspended is the design; migrate-the-running is forbidden (runtime §3.3).

### 1.4 The cost, stated honestly

One `shadow()` per *shared* base cell a branch writes; nothing for cells it only reads; nothing for
variables the branch itself mints via `makeVar` (those are arena-local, not shadows). For a clause
with `k` head variables unifying a shared goal term, at most `k` shadows per branch — the smallest
possible binding store, created on demand (structure §2). `release()` drops exactly the shadows
taken under a mark, so a failed branch's writes revert to reading through the base at no scan cost.
This is Muse environment copying with the copy made lazy and cell-granular, strictly fewer copies
than Muse's eager incremental segment copy (research §1.3, transfer-table rows "environment copying"
/ "incremental copying"). The irreducible C++ here is exactly one thing: **one arena per branch,
single-owner, pinned, never migrated** — the arena is dense vectors with no atomics or locks, so it
is not thread-safe (`arena_manifold.hpp`; the general study §3.2 states the single-owner rule), and
ownership is scheduler state (§6).

### 1.5 Prior art and model selection

This section discharges the task's requirement for an explicit survey, condensing the research
section ([`_andor_section_research.md`](_andor_section_research.md) §1) and the runtime section
([`_andor_section_runtime.md`](_andor_section_runtime.md) §1) so the model choice above is defended
against the named alternatives rather than asserted. The researcher's headline finding governs: **a
copy-on-write arena overlay over an immutable base is the environment-copying (Muse) model with the
copy made lazy and cell-granular**; the design builds on that identification instead of reinventing
it.

The constraint that chooses the model is Gupta & Jayaraman's **three-criteria impossibility**: no
OR-parallel model implementing full Prolog can make all three of *task creation*, *task switching*,
and *variable access* constant-time at once. Every model pushes its unavoidable non-constant cost
into one phase. The winners (Muse, Aurora) pay it in **task switching**, the phase the scheduler
controls, rather than in deref or bind, whose frequency the program sets. The arena design must do
the same: push cost onto fork, which it controls, never onto `deref`/`bind`, which it cannot.

The five canonical answers to "where does a branch's private binding of a shared variable live,"
each scored against the substrate:

- **Binding arrays — the SRI model (Aurora).** A per-worker array indexed by a dense variable
  counter; deref is one indirection (near-constant), the non-constant cost is reconciling bindings
  at task switch. **Rejected:** it is a *shared mutable* per-worker store that must be installed and
  deinstalled on relocation, which share-nothing forbids and the no-cursor-migration rule (study
  §3.2) makes unnecessary — there is no task switch to reconcile.
- **Environment / stack copying (Muse).** No shared mutable store; a worker copies the differing
  stack segment and runs an ordinary sequential engine, so each binds in its own copy and the
  multiple-binding problem dissolves by construction. O(1) deref; non-constant cost at the copy.
  **Adopted, and improved** (§1.1–§1.4): the arena copies *nothing* at fork and one cell on first
  write (`shadow()`), which is strictly fewer copies than Muse's eager incremental segment copy.
- **Versions-vectors / hashing windows (AKL).** A per-variable structure recording each branch's
  binding; deref hashes the current branch. **Rejected:** it reintroduces non-constant *variable
  access*, the phase one least wants to pay, where the arena keeps `deref` = `cloneMaster` with no
  per-branch indirection.
- **Binding directory trees.** A tree mirroring the OR-tree; deref walks up to the nearest binding,
  O(tree-depth). **Rejected** for the same non-constant-deref reason; the overlay's O(1)
  read-through is strictly better.
- **Composition tree / paged binding arrays (Gupta & Jayaraman, combined AND-OR).** Extends binding
  arrays to the AND-OR cross-product with first-class cut and side-effects. **Not adopted** as a
  binding store (it inherits the binding-array rejection) but its cross-product-without-recompute
  idea is adopted for the solution join (§4.2).

The researcher's transfer table rates the arena substrate **yes** on multiple-binding isolation,
environment copying (a strictly better `yes+` on incremental copying), O(1) choice-point marking,
and parallel GC of abandoned branches (a non-problem — an abandoned branch *is* its arena, dropped
with no scan); **no** on every shared-mutable-store model (binding arrays, hash windows, directory
trees) and on dependent AND-parallelism; and **partial** on the scheduler work this design specifies
(lazy task creation, work-stealing deque, cut-as-cancellation, the submission-index merge). The
"no"s are deliberate refusals with the reason stated, not gaps. On the AND side, DeGroot's RAP and
Hermenegildo's &-Prolog decide independence with compile-time conditional-dependency-graph guards;
the arena sharpens "independent" to *disjoint `shadow()` write-sets* (§4.1). For vicon the
literature is thin and reported honestly: canonical Icon is resolutely sequential (Griswold's
Arizona TRs; Proebsting's Jcon), and the one concurrency precedent is **Junicon** (HIPS 2016), which
makes co-expressions actor-like message-passing islands — exactly the worker-pinned treatment of
§5.4, cited as the known-good shape, not an invention. Full sources are in
[`_andor_section_research.md`](_andor_section_research.md) §7.

## 2. The OR-frontier: ephemeral structure for the tree, C++ for the leaves and the deque

### 2.1 The decision, and the line

The structure expert proposes making the OR-frontier structural (four ephemeral dimensions); the
runtime expert argues the OR-tree should stay in the deque/sender graph and not be minted as
structure. **Both are right about different objects, and the reconciliation is a clean line: the
_relationships_ among branches are ephemeral store structure; the _ready-to-run queue_ and each
branch's _interior_ are C++.** Neither expert is rejected; the apparent conflict is a scope
confusion described from opposite ends.

- **Structural (adopt the structure proposal), ephemeral, host-arena-owned:** the parent/child shape
  of the search, the branch→worker binding, the lifecycle state, and the deterministic submission
  order. These are *relations*, and relations are what dimensions are for. Making them cells lets
  cut be a rank prune and the merge be a rank walk instead of parallel C++ bookkeeping, and the
  `ephemeralBit` guard catches a scheduler bug that tried to persist the search for free (R8 refusal
  in `Store::setLink`/`applyStructure`).
- **C++ (adopt the runtime proposal):** the per-worker *work-stealing deque* of ready sparks, and
  each branch's *interior* — its bindings, trail, program counter, and `Mark`. A `Mark` is
  arena-local and cannot be structure; the deque is a hot scheduler structure touched on every steal
  and must not mint cells (speculation would then write structure, which only `promote()` is allowed
  to do).

The rule that resolves it: **promote the answer, not the search.** The OR-tree *shape* is minted as
ephemeral coordination cells so the host can reason over it on one thread; the OR-tree *as runnable
work* lives in the deque; and no frontier cell is ever promoted — only the answer subgraph reachable
from a solved root crosses the `ephemeralBit` boundary (`arena_manifold.hpp`, `promote` contract;
structure §3).

### 2.2 The four coordination dimensions

All four are minted in a dedicated **coordination arena owned by the host** — the one thread that
already serializes `promote` (study §3.3/§3.5) — and all four carry `ephemeralBit` and are never
promoted:

- **`d.or`** — parent/child frontier. A choice-point cell's posward `d.or` neighbours are its
  alternative branches in creation order (clause order; alternation-arm order; generator-resumption
  order). "Take the next untried alternative" is "step posward on `d.or`"; "the branches to a cut's
  left" is a reachability walk, not an index comparison.
- **`d.branch`** — binds a frontier branch cell to a per-worker arena-handle cell. "Which worker
  owns this branch" is `linked(branch, d.branch)`, replacing an implicit cursor→worker map with a
  readable edge.
- **`d.frontier`** — the ready/running/done/cut lifecycle as a rank of state cells (variablehood as
  rank membership, not a flag bit, `isUnbound`, `vlog.cpp:78-82`). Moving a branch between states is
  one `insertIntoRank`; the scheduler reads ready branches by walking one rank.
- **`d.submission`** — the deterministic merge order. A forked branch is linked, in creation order,
  onto the tail of a `d.submission` rank. "First solution = lowest submission index" (study §3.5)
  becomes "the negward-most solved branch on `d.submission`" — a rank walk, not a sort of wall-clock
  completion times.

The branch *contents* (bindings, trail, PC) are the owning worker's arena state the frontier cells
merely point at via `d.branch`. The tree is cells; the leaves' interiors are C++; and that split is
the hardware boundary — the tree is read by the host scheduler on one thread, the leaves are mutated
by workers on many (structure §3).

## 3. Cut as cancellation: structural prune plus stop-token

### 3.1 What serial cut does, and the cross-worker hard case

Serial `cut(cutBarrier)` pops the single global stack down to the barrier, calling
`arena().discard(cp.mark)` on each popped frame (`vortex_vm.cpp`, `cut` body — the
`while (choiceStack_.size() > cutBarrier)` loop). `discard` **keeps bindings and drops the retry** —
the arena's "give up a choice point without undoing it." `cutBarrier` is set at push to
`choiceStack_.size()` (`vortex_vm.cpp:174`), an index into the one stack. Once the stack is
per-worker (study §6.5), `cutBarrier` indexes the owning branch's stack — fine for choice points
still on it. The hard case is the branches the cut prunes that have already been **stolen** and are
running on other workers: they are no longer on the cutting worker's stack to `discard`.

### 3.2 The decision: prune the rank, cancel the thread

Cut becomes two coordinated actions, structural where it can be and C++ where it must be:

1. **Structural prune (the barrier is a cell).** From the cutting branch, walk negward on `d.or` to
   the barrier cell (in vprolog the clause-selection node; in vicon the generator scope the cut, or
   the implicit cut in `if/then`, closes). For each branch posward of the barrier on that `d.or`
   rank, move it from the `running`/`ready` `d.frontier` rank to a `cut` rank (one
   `insertIntoRank`). The cut branch itself survives and its already-made bindings stand, because
   the arena `discard`s (keeps bindings), never `release`s — the serial `discard(cp.mark)` meaning
   lifted to a whole branch.
1. **C++ cancellation (stop a thread that is physically elsewhere).** Each speculative branch
   carries a `std::stop_token` wired to a `std::stop_source` owned by the parent choice frame (C++20
   native, in the toolchain today — *not* the cpp26 facade, which covers only
   `function_ref`/`optional`/`inplace_vector`/`concat`/`span_at`, confirmed in `cpp26_select.hpp`).
   The host requests stop on the pruned branches' sources as it moves them to the `cut` rank. Each
   worker polls `stop_requested()` at its `stepScheduler` boundary — the same granularity at which
   cursors are already stepped and the backtrack loop already tests the choice stack — and, on
   seeing it, `release()`s its arena and exits.

### 3.3 Why reclamation is free, and the honesty limit

A cancelled branch simply drops its private arena. Because the arena is a share-nothing
copy-on-write overlay, dropping it reverts every `d.clone` splice, every `d.scan` move, and every
reversible-`<-` write that branch made in one destruction — no trail-unwinding across workers, no
shared binding to repair (research §4.1/§4.3; runtime §5.1). This is the arena's "cells minted under
the mark are gone, not garbage" property made cross-worker: per-worker reclamation, no cross-worker
GC, no stop-the-world — a structural advantage over Aurora/Muse, which both needed custom parallel
GC.

The honest limit: cancellation is **cooperative** (both `stop_token` and senders are). A branch
between a stop request and its next poll keeps running briefly; it wastes a little work but cannot
corrupt anything, because its writes are confined to its own arena and will be dropped. Cut stays
*semantically exact* (no pruned solution ever escapes, because a solution is suppressed unless its
branch is confirmed leftmost by submission index) while being only *eventually* efficient. For an
unbounded vicon generator that may never reach a natural yield point, poll on a bounded step counter
in the resume loop (C++ §1) so an abandoned unbounded branch cannot run forever. Non-migration
(study §3.2) is preserved throughout: we never move a running cursor, we cancel it from outside and
let it unwind locally.

## 4. AND-parallelism: independent-only, join by submission index

### 4.1 Independent is adopted; dependent is provably out of reach

**AND-parallelism in this design is independent-only.** Two conjoined subgoals `a, b` may run on two
workers iff they do not compete to bind the same variable. In the arena model that independence has
a sharp, checkable form the WAM lacked: **two subgoals are independent iff the sets of base cells
each would `shadow()` are disjoint** (research §3.1; structure §5). If the shadow-sets are disjoint,
the two subgoals run in two child arenas over the same frozen base and their results merge with no
binding conflict — independent AND-parallelism expressed as non-overlapping copy-on-write
write-sets. `shadowOrder_`/`Mark::shadowCount` make the write-set observable, so the independence
check is a runtime property, not only a compile-time CGE guard (DeGroot RAP / &-Prolog). Determinacy
analysis is the companion: a subgoal known to produce one solution needs no cross-product join.

**Dependent AND-parallelism is provably out of reach of the share-nothing arena model**, and the
design states it as a hard limit rather than attempting it. Dependent conjuncts share an unbound
variable: one binds it (producer), the other reads it (consumer), which requires a binding made on
one worker to become visible on another — the shared-mutable-trail hazard the arena forbids by
construction. The two escapes are both bad fits: reintroduce a shared mutable cell for the shared
variable (breaks share-nothing, needs locking on a `d.clone` splice, defeats the reason the arena
dissolves the multiple-binding problem) or message-pass the producer's binding to the consumer's
arena (serializes the dependency, i.e. sequential execution with extra copying). This is the same
ruling the study gives vicon's generators (§4.3/§6.4): do not split one search across workers
(research §3.2; runtime §5.3).

### 4.2 The solution join / cross-product

Even for independent conjuncts, if `a` has solutions {a1, a2} and `b` has {b1, b2}, `a, b` must
yield the cross-product in Prolog's observable left-to-right order. The join is **two solution
streams, each a `d.solutions` rank of promotable answer roots tagged by submission index, and the
cross-product is their rank-product** — structure doing the bookkeeping (research §3.3). The join
pairs roots; it does not recompute either side, because each solution is already an arena subgraph
reachable from a root (avoiding the redundant-computation trap Gupta & Jayaraman's combined AND-OR
tree was built around). Ordering is the general study's submission-index rule: fork assigns each
branch the index it would have in serial depth-first order, the host joins in that order, and the
parallel cross-product reproduces the serial order deterministically (study §3.5). The AND-join
across independent conjuncts is a `when_all`-shaped rendezvous (`std::barrier`, C++20 native, for
the stage boundary before the join; C++ §2); a conjunct's hard failure requests stop on its siblings
via the same stop-token spine as cut.

## 5. vicon specifics

vicon makes *every expression a generator*, so a `#Choice` is pushed for `1 to 3`, `i < j`, `s ? e`,
and every `every` (study §6.6). That makes the per-worker localization of `choiceStack_`,
`ChoicePoint::mark`, and the trail a **correctness precondition** for running vicon on any worker,
not an optimization. The design's five vicon decisions:

### 5.1 Generators single-worker by default; alternation is the opt-in

A single generator chain that threads `&pos`, `<-`, or a shared variable through sequenced
sub-expressions runs on **one worker**, for the dependent-trail reason of §4.1. The granularity of
`1 to 3` or `i < j` is far below any sensible fork threshold anyway (the Icon-family literature is
thin precisely because generators are *more* pervasive than clause calls, making naive parallelism
harder, not easier — research §5). vicon's realistic parallelism is therefore **OR-parallel
alternation only**: `e1 | e2` arms whose shadow-sets are disjoint, gated behind the same
`system://settings` flag as vprolog's OR-parallel search (default off, study §6.6). A `#Choice` is a
`#Choice`, so the alternation arm rides the identical branch mechanism of §1–§3; vicon adds no new
opcode.

### 5.2 `d.scan` is per-worker for free

String scanning holds `&subject`/`&pos` on a `d.scan` rank in the arena. Because each worker (and
each branch) owns its own `ArenaManifold`, its `d.scan` rank is private automatically — two scans on
two workers cannot corrupt each other's `&pos` because the ranks are in different arenas. A scan
cursor is a cell with a `d.scan` edge; `&pos` moves are ordinary `link` edits under a live mark, so
they are trailed and `release`d on backtrack by the same `TrailEntry` mechanism that undoes a
binding (`arena_manifold.hpp`, `TrailEntry`/`trail()`). No new machinery: scanning position is a
`d.scan` link, not a C++ field, and it is born in the worker that scans (structure §5).

### 5.3 Reversible assignment `<-` confined to its arena

`<-` is an ordinary `value()`/`setValueBits` write under a live mark, trailed if the cell predates
the mark and reverted by `release` (`arena_manifold.hpp`, `TrailEntry`). It is already structural;
the parallelism consequence is a *limit*, not a mechanism. Because the undo is a per-arena trail and
`ChoicePoint::mark` names one worker's arena, a goal-directed evaluation that uses `<-` cannot be
split across workers — the dependent-conjunct hazard of §4.1. A cancelled branch's `<-` writes
vanish with its arena at no cost (§3.3), which is why reversible assignment is cheap and safe
*within* a branch and forbidden *across* a fork.

### 5.4 Co-expressions: worker-pinned, activated by message

`create e` / `activate` (`@`) capture a suspended choice stack plus an arena mark in a pinned
island. Represent the suspended co-expression as a **cell on a `d.pinning-cursors` rank** whose
edges name the entry opcode cell, the `d.branch` worker-arena handle it was created in, and a `d.or`
sub-rank holding its suspended choice points. That cell is a movable *name* for the suspension — a
`CellRef` and a few ephemeral edges — and can be **shipped between workers as a message**. What
cannot be shipped is the live arena state it points at: the choice stack and mark live in the
creating worker's arena, and non-migration (study §3.2) forbids moving a running cursor's arena
across threads. So activation on a different worker is not "move the island there" but "send a
message to the creating worker to resume it" — the Junicon actor-island shape, the one known-good
precedent (research §5; structure §5). The co-expression cell is the message payload; its `d.branch`
edge enforces the pinning. The spec marks co-expressions optional (study §6.6), so this defers
cleanly: the structural island makes activation *expressible* as a message without ever moving arena
state. A co-expression is thus a `SpeculativeBranch` that is deliberately *not* placed on a deque —
pinned, not stolen (runtime §6).

### 5.5 Bounded/unbounded as a free static grain oracle

A bounded expression emits a `discard(mark)` at its boundary; an unbounded one does not (study §6.6;
`discard` keeps bindings and drops the retry). That boundary marker — which the compiler already
emits — is a granularity signal handed to the scheduler for free: a known-bounded expression has a
small, known yield set and is a poor fork candidate (run it inline), while an unbounded or
large-bounded one (`i < j` over a wide range, `every` over a big generator) is where forking an
OR-branch pays. "Only fork at unbounded choice points" is readable straight off the `discard` the
compiler emits — a static grain oracle general work-stealers must approximate dynamically. This is a
genuine contribution point of the vicon mapping (runtime §4.4).

## 6. The runtime that executes it

Bottom to top, each layer tied to the arena model and to an available primitive (C++ availability
table):

1. **Worker pool with per-worker work-stealing deques** (Lace-style non-blocking split deque to
   avoid a fence on the common local push/pop), one deque per OS worker, M:N over the general
   study's scheduler (sibling of `render::WorkerPool`, study §3.2). Each worker owns one private
   `ArenaManifold` over the one frozen shared base. The deque holds `std::deque` today; `std::hive`
   is aspirational and only if branch-record pointer stability becomes a measured need (C++ §4).
1. **Lazy task creation on top of the deque.** A choice point's untried `altOp` is left as a cheap
   spark, not a reified task; the owner runs the first alternative inline at serial speed. A spark
   the owner backtracks into before anyone steals it *fizzles* for free (GHC-spark shape) — so
   speculation is safe to be generous with, because unclaimed speculation costs one deque push/pop
   (runtime §1.3/§4.2).
1. **A stolen spark reifies into a `SpeculativeBranch`** =
   `(ChoicePoint suspension, private arena, submissionIndex, stop_token)` (runtime §3.3). It carries
   its arena; it is resumed **once per resumption** (a vicon generator's multiplicity is a *chain*
   of one-shot resumptions, so we never clone a live continuation); it runs to completion on the
   taker and never migrates while running. Reification cost is the shadowed-slot set only, because
   the base is shared copy-on-write — the one place this beats Muse/PALS outright.
1. **Composition and cancellation vocabulary.** `std::execution` (P2300) senders/receivers map
   almost one-to-one — three completion channels == solution / fault / pruned, `when_all` == the
   independent AND-join, `get_stop_token` == the cut and first-solution spine. But P2300 is
   **aspirational**: `cpp26-compatibility.md` rates it 10–20 engineer-days and "Defer", stdexec is
   experimental with no pin, and the design must not write against it or probe its macro (C++ §3).
   **So the buildable Stage 5 shape is the deque + lazy task creation core plus a hand-rolled
   `SearchPool` using C++20 `std::stop_source`/`stop_token` and `std::barrier`, implementing only
   `when_all` and stop propagation by hand.** The sender vocabulary is the migration target behind
   the `SearchPool` facade, preferred but explicitly not load-bearing (runtime §1.5; C++ §3).
1. **Cut and first-solution-wins are cancellation, not coordination** (§3). A prune requests stop
   down the stolen subtrees; each cancelled branch drops its arena. First-solution-wins is cut's
   cousin: the lowest-submission-index branch to produce a result wins and the rest are cancelled by
   the same spine, so a higher-index branch that finishes first in wall-clock does **not** win — its
   result is held and discarded if a lower-index branch later succeeds. This makes first-solution
   deterministic *across worker counts*, the language-visible serial-equivalence promise (runtime
   §5.2).
1. **The merge is a serial, index-ordered drain by one thread** — the single `promote` writer (study
   §3.5). Atomics (`std::atomic<std::shared_ptr<const Solution>>` for the blackhole/one-forcer,
   `std::atomic_ref` on hot merge counters, all C++20 native) let branch threads announce completion
   cheaply; determinism is enforced by the serial drain in submission order, not by the atomics. A
   solved branch publishes an **immutable** `Solution` with `store(release)`; the merge thread
   consumes with `load(acquire)`; the `Solution` does not alias the arena, so there is no data race
   even though many branches publish into many slots (C++ §2/§5). The arena stays single-threaded
   with no atomics: the `submit` handoff is the synchronization edge, so
   `mark`/`release`/`unify`/every `d.clone` splice runs on one thread by construction — which is
   *why* the multiple-binding problem dissolves (C++ §5a).
1. **Granularity is mostly "not-forking is free" (lazy task creation)**, backed by
   `system://settings` keys — `andor.minSpeculationDepth`, `andor.maxLiveBranches`,
   `andor.minArenaShipCost` — read via `getSetting` from `defaultSettingSpecs()` (no magic numbers,
   AGENTS.md governance), and by vicon's static bounded/unbounded oracle (§5.5).
   `andor.maxLiveBranches` bounds arena memory, the real resource, directly;
   `andor.minArenaShipCost` keeps a branch on its owner when the shadowed-slot set it would ship
   exceeds the resume-inline cost, measurable because the arena tracks `shadowCount` per `Mark`
   (runtime §4.3).

Fallible fork is `std::expected<Ticket, ForkRefusal>` (C++23 native, matching `unify()`'s
`std::expected<void, UnifyFailure>` and `ArenaResult`), never a sentinel — pool saturation,
speculation budget, or `ArenaRefusal` are refusals, not search failures (C++ §4). A branch's
synchronous body uses `gleditor::cpp26::function_ref` (the `WorkerPool::Work` pattern); the
asynchronous `submit` body must own its closure via `std::move_only_function` (C++23 native),
because `function_ref` may not be stored past the caller's lifetime — the one place the `WorkerPool`
borrow cannot be copied verbatim (C++ §3).

## 7. OR-tree across workers, with per-branch arenas and the cut path

```text
                        HOST  (one thread: coordination arena + the single promote writer)
                        -----------------------------------------------------------------
                        base Manifold*  (immutable for the microversion; shared, no lock)
                        d.or / d.branch / d.frontier / d.submission  (ephemeral, never promoted;
                          ephemeralBit refused by Store::setLink if a scheduler bug leaks one)
                                                   |
                       choice point  C0            |  "prove p(X)"  -- clause order = d.submission
                       (barrier for cut in p)      |
                                                   |
                 d.or posward -->  B1 ........... B2 ........... B3 ........... B4
                 submissionIndex:   0              1              2              3
                   (lowest-index solution is "first"; wall-clock arrival is ignored)
                                                   |
         --------------------------------------------------------------------------------------
         |                       |                        |                        |
    Worker 0 deque          Worker 1 deque           Worker 2 deque           (spark: not yet
    runs B1 INLINE          stole B2 (reified)       stole B3 (reified)        stolen -> fizzles
    (serial speed)          SpeculativeBranch        SpeculativeBranch         for free if the
         |                  {CP, arena, idx=1,        {CP, arena, idx=2,       owner backtracks
    Arena_B1 (CoW on         stop_token}              stop_token}             into it first)
     base; shadows v         |                        |
     on first bind)     Arena_B2 (CoW; its       Arena_B3 (CoW; its own
         |               own shadow of v,         shadow of v, d.scan,
    d.clone splice       d.scan, <- writes)       <- writes)
    v = t1 PRIVATE           |                        |
                        d.clone splice v=t2      B3 finds a solution first (wall-clock),
                                                 but idx=2 > idx=0 -> result HELD, not emitted
                                                   |
         ======================  CUT fires in B1 (idx 0, barrier = C0)  =====================
         |                                                                                    |
    1. structural prune: move B2,B3,B4 from d.frontier[running] to d.frontier[cut]            |
       (one insertIntoRank each); keep B1's own bindings (discard, not release)               |
    2. C++ cancellation: request_stop() on the stop_source of each pruned branch              |
                                   |                        |                                 |
                            Worker 1 polls           Worker 2 polls                           |
                            stop_requested() at      stop_requested() at                      |
                            stepScheduler boundary   stepScheduler boundary                   |
                                   |                        |                                 |
                            DROP Arena_B2  --------> DROP Arena_B3  (reverts every d.clone     |
                            (one destruction          splice, d.scan move, <- write in ONE    |
                             reverts all writes)       destruction; no cross-worker trail)    |
                                   |                        |                                 |
         ----------------------------------------------------------------------------------  |
         |                                                                                    |
    B1 survives, its bindings stand. Its solution (idx=0) drains to the HOST in               |
    submission-index order and is promoted on the single writer thread.  <--------------------+
    (B3's held idx=2 result is discarded with its arena -- a pruned solution never escapes.)
```

The diagram shows the three load-bearing facts together: each stolen branch holds a *different*
arena over the *same* immutable base (so no `d.clone` splice is ever cross-thread, dissolving the
multiple-binding problem); cut is a structural `d.or`/`d.frontier` prune paired with a C++
stop-token for branches physically running elsewhere; and a dropped arena reclaims a cancelled
branch's entire binding store in one destruction, with determinism enforced by the submission-index
drain at the host, not by wall-clock arrival.

## 8. Staged plan, nested under the general study's Stage 5

The general study ends at **Stage 5 — vprolog OR/AND-parallel search, opt-in, default off, gated on
logic equivalence under TSan** (§3.6/§6.4). This design subdivides that one stage into five
sub-stages, each independently shippable, leaving the tree green under `make -j$(nproc) check` and
`make -j$(nproc) test`, every tunable a `defaultSettingSpecs()` entry, and the on-disk invariants
(`sizeof(CompactOpNode) == 64`, 64 KiB Merkle pieces, append-only, single promote writer) untouched.
Sub-stages are prerequisite-ordered: the mechanism before cut, cut before the structural frontier,
OR before AND, vprolog before vicon.

- **Stage 5.0 — Per-branch arena + the `SearchPool` core.** Give an OR-branch its own
  `ArenaManifold` over the shared base; build the deque + lazy-task-creation `SearchPool` with
  hand-rolled `std::stop_source`/`when_all` (no P2300). Still single speculative branch at a time
  behind the default-off `system://settings` flag. **Gate:** first-solution and full solution *set*
  bit-identical to the serial engine on a deterministic suite; `xudu-dump --section=ops`
  byte-identical for any promoted answer (the search mints nothing); the engine-and-test
  ThreadSanitizer target (`SANITIZE_THR_OPTS` applied to the engine objects and
  `xudu_test`/`zigzag_test`, *not* the stock `sanitize/thread`, which builds only `gleditor`) clean
  over looping stress runs.
- **Stage 5.1 — Cut as cancellation.** Add the per-branch `stop_token`, the poll at the
  `stepScheduler` boundary, and `OrGroup::cutTo(cutBarrier)` mapping the barrier to a
  submission-index threshold. **Gate:** a cut/negation suite (green cut, `once`, `\+`, if-then-else)
  matches the serial engine's solution *sequence*; no pruned solution escapes (a held higher-index
  result is discarded); TSan clean; `zigzag.arena` leak logging shows every cancelled branch's arena
  dropped.
- **Stage 5.2 — The structural OR-frontier.** Mint `d.or`/`d.branch`/`d.frontier`/`d.submission` in
  the host coordination arena; move scheduling, cut-prune, and first-solution to rank walks.
  **Gate:** `ephemeralBit` R8 refusal fires in a negative test that deliberately tries to persist a
  frontier cell; determinism unchanged; `xudu-dump` confirms no frontier cell reaches `ops.nodes`.
- **Stage 5.3 — Independent AND-parallelism.** Fork two conjuncts into two child arenas only when
  their shadow-sets are provably disjoint; join by submission-index rank-product over two
  `d.solutions` ranks. Dependent conjuncts stay serial (asserted by a test that a shared-variable
  conjunction never forks). **Gate:** cross-product order identical to serial for a non-trivial
  `a, b` with multiple solutions each; a non-commutative-sensitive case matches; TSan clean.
- **Stage 5.4 — vicon OR-parallel alternation.** Enable `e1 | e2` arm forking behind the flag;
  `d.scan`, `<-`, and co-expressions confined per §5 (a single generator chain stays one-worker;
  co-expression activation is a message). **Gate:** first-solution and solution-set equivalence to
  the serial vicon evaluator for a scanning + reversible-assignment suite; a co-expression activated
  from a foreign worker resolves via message to its creating worker (no arena state crosses a
  thread, verified under TSan); bounded expressions provably run inline (a counter asserts no fork
  below the `discard(mark)` boundary).

Cross-cutting gates on every sub-stage, inherited verbatim from the study:
`make -j$(nproc) format-check lint analyze` clean; `make test` green; determinism is a *test*
(`xudu-dump --section=ops` reproducibility and serial-vs-parallel equality over 1..N workers), not a
claim; and because races are schedule-dependent, the TSan pass criterion is "clean over K looping
iterations on multiple cores," not "right answer once" (study §5.5).

## 9. Honest limits

- **Dependent AND-parallelism is out of scope and provably so** under share-nothing arenas (§4.1):
  it needs a producer→consumer shared mutable binding the arena forbids. Independent-only, by
  disjoint shadow-sets.
- **A single goal-directed evaluation cannot span workers** (§5.1/§5.3): `&pos`, `<-`, and shared
  bindings are per-arena trail effects. vicon's parallelism is coarse alternation arms, not every
  suspension.
- **Cancellation is cooperative** (§3.3): a branch runs briefly past a stop request. Semantically
  exact, only eventually efficient; the unbounded-generator step-counter poll bounds the waste.
- **Co-expression cross-worker activation is message-passing, not migration** (§5.4), and is marked
  optional in the vicon spec — it defers cleanly.
- **P2300 senders/receivers are aspirational** (§6): Stage 5 ships the hand-rolled `SearchPool`;
  senders are the migration target behind that facade, not written against today.
- **`promote()` is the serial Amdahl throat** (study §4/§7): the win is parallel *evaluation*
  feeding a serial *commit*; a workload whose in-arena fraction is small has a low parallel ceiling,
  which Stage 0 of the parent study measures before any of this is built.

These are the same limits the four experts converge on, and they are features of the share-nothing
model, not gaps in it: the arena dissolves the multiple-binding problem precisely *because* it
refuses to share mutable state, and every limit above is the price and the proof of that refusal.

______________________________________________________________________

Change history: 2026-10-16 — initial synthesis reconciling the four code-verified section inputs
([`_andor_section_research.md`](_andor_section_research.md),
[`_andor_section_runtime.md`](_andor_section_runtime.md),
[`_andor_section_structure.md`](_andor_section_structure.md),
[`_andor_section_cpp.md`](_andor_section_cpp.md)) and [`_andor_briefing.md`](_andor_briefing.md)
into one design, consistent with [`vortex-multithreading-study.md`](vortex-multithreading-study.md)
§3 and §6.4/§6.6. Decisions recorded: per-branch copy-on-write arena as the OR binding store (§1),
defended against the five canonical OR-parallel models and Gupta & Jayaraman's three-criteria
impossibility (§1.5); the structure/C++ line for the OR-frontier — ephemeral
`d.or`/`d.branch`/`d.frontier`/`d.submission` cells for the tree, the deque and branch interiors in
C++ (§2); cut as a `d.or` prune plus `std::stop_token` cancellation (§3); independent-only
AND-parallelism with dependent provably out of reach (§4); vicon generators single-worker with
alternation the opt-in (§5); the five sub-stages 5.0–5.4 nested under the study's Stage 5 with
determinism/TSan/first-solution/cut-negation gates (§8). 2026-10-16 (rev 1) — citation-precision
pass against the critique panel: `Vlog::bind` splice `vlog.cpp:86` with the `m.link` at `:91-93`;
`deref` `vlog.hpp:81`; `backtrack`'s `release(cp.mark)` `vortex_vm.cpp:183`; the `#Choice` opcode
handler `vortex_vm.cpp:878` (which calls `pushChoicePoint` at `:164`); variablehood `isUnbound`
`vlog.cpp:78-82`; and the arena single-owner property attributed to its lock-free dense-vector
layout and study §3.2 rather than a non-existent class-comment statement.

Reconciliation note: this design is grounded in and must stay consistent with
[`_andor_briefing.md`](_andor_briefing.md) (the code-verified factual basis) and
[`vortex-multithreading-study.md`](vortex-multithreading-study.md). Where any claim here diverges
from the briefing or from the code it cites, the briefing and the code govern.
