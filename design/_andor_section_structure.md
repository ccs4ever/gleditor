# AND/OR parallelism, told as cells and dimensions

This is the ZigZag/Vortex expert's contribution to `design/vicon-andor-parallelism.md`: the part of
the design that insists the answer live in cells and dimensions rather than in a C++ data structure
wherever that is honestly possible, and that says plainly where C++ is genuinely required. Every
claim is grounded in the code of this worktree. Paths are relative to the
`study/vortex-multithreading` worktree root.

The spine of the argument is one observation already made for us by the arena:

> The mark/release model is inherently serial within one arena — a `Mark` names a point in *that
> arena's* dense vectors. OR-branches on different workers therefore need *different arenas*, and a
> binding made in one worker's arena is invisible to another by construction.
> (`design/_andor_briefing.md`, "The arena undo model")

Everything below is a consequence of that, read structurally.

## 1. How a binding works today, and why two OR-branches collide

A binding is not an assignment and not a trail entry you look up. It is **one link on the `d.clone`
rank**:

```cpp
void Vlog::bind(const CellRef v, const CellRef t) {
  // One link ... a rank tail has no posward neighbour to evict ...
  zigzag::expectWritten(m.link(endOfRank(t, clone, DimVector::POS), clone,
                               DimVector::POS,
                               endOfRank(v, clone, DimVector::NEG)));
}
```

(`apps/common/xanadu/zigzag/vlog.cpp:86`). `bind(v, t)` splices `v`'s whole `d.clone` rank onto the
posward tail of `t`'s rank, so the combined rank's negward-most master is `t`'s master. Dereference
is nothing but walking that rank to its master:

```cpp
[[nodiscard]] CellRef deref(CellRef ref) const noexcept {
  return m.cloneMaster(ref, clone).value_or(ref);
}
```

(`apps/common/xanadu/zigzag/vlog.hpp:80`, over `ArenaManifold::cloneMaster`,
`apps/common/xanadu/zigzag/arena_manifold.hpp` — the cycle-guarded negward walk). A variable is just
a bare cell on the `d.vars` rank with empty content and no value (`Vlog::makeVar`, `vlog.cpp:23`;
`Vlog::isUnbound`, `vlog.cpp:78`), and unification is the ordinary recursive descent that calls
`bind` on the first unbound side it meets (`Vlog::unify`, `vlog.cpp:96`).

**The multiple-binding problem, in this codebase's exact terms.** A variable cell `v` has exactly
one `d.clone` rank: one posward neighbour and one negward neighbour on dimension `d.clone`, because
a ZigZag cell has at most one neighbour per direction per dimension — that is the rank invariant the
manifold is built on. `bind(v, t)` *consumes* that one slot. If OR-branch A wants `v = t₁` and
OR-branch B wants `v = t₂`, both must splice `v`'s single `d.clone` rank, and the second splice
would have to evict the first. The two bindings cannot both be present on one rank in one arena:
there is no "copy of `v`" to bind differently, because a cell's identity *is* its slot and its slot
holds one `d.clone` edge each way. This is the Aurora/Muse multiple-binding problem expressed not as
"two values in one environment cell" but as "two splices contending for one `DimLink`."

The serial engine never sees this because it only ever has one binding live at a time: `#Choice`
takes a `mark()` (`VortexVM::pushChoicePoint`, `apps/common/xanadu/vortex/vortex_vm.cpp:164`),
`#Fail` calls `release(cp.mark)` which truncates the splice away (`backtrack`, `vortex_vm.cpp:178`),
and only then does the alternative run and make *its* splice into the now-free slot. The single
global `std::vector<ChoicePoint> choiceStack_` (`vortex_vm.hpp:151`) and the single arena enforce
"one branch at a time" as a side effect of being single. Parallelism removes that accident, which is
why the collision becomes visible precisely when we want to go fast.

## 2. The copy-on-write overlay gives each OR-branch a private binding store for free

The arena header states the overlay directly:

> An arena may be given a base `Manifold`, and then it is a copy-on-write view ... reads of a cell
> the arena does not hold fall through to the base, and the first *write* to one shadows it — the
> whole cell, its link run and its content run copied into the arena, keeping the base's `CellRef`
> as its name. The base is never touched. (`arena_manifold.hpp`, "The overlay")

So the mechanism needs no new type. Give each OR-branch its own `ArenaManifold` over the **same
shared frozen base** — the exact share-nothing shape the general study already recommends
(`design/vortex-multithreading-study.md` §3.3). Then:

- The clause database, the term being solved, and every variable the goal mentions live in the base,
  readable by all branches with no copy.
- The moment branch A calls `bind(v, t₁)`, the first write to `v`'s slot triggers `shadow(v)`
  (`arena_manifold.hpp:656`): A's arena takes a private copy of `v`'s slot — *keeping the base's
  `CellRef`* — and splices the `d.clone` edge **in A's copy**. Branch B does the same in B's arena.
  Each branch now has its own `v` with its own one `d.clone` rank. The contention of §1 is
  dissolved: there are two slots named `v`, one per arena, each with room for one splice.

This is the SRI binding-array / Muse stack-copying answer, but we pay neither cost in full. We do
not pre-size a binding array over all variables (SRI), and we do not copy the whole stack at a fork
(Muse). The overlay is **lazy stack copying at cell granularity**: a branch shadows only the cells
it actually binds.

**The precise cost.** One `shadow()` per base cell a branch *writes* — nothing for cells it only
reads (`arena_manifold.hpp`, `shadow()` doc: "An evaluation pays one copy per cell it writes to and
nothing for the ones it only reads"). A shadow copies one `CellSlot` plus its link run and content
run. `Mark::shadowCount` (`arena_manifold.hpp:180`) counts them, and `release()` drops exactly the
shadows taken under the mark, so a cell a failed branch wrote "reverts to reading through the base —
which is the undo, for an overlaid cell" (same header). So the per-branch binding store is literally
free in the common case (variables the branch itself minted via `makeVar` are arena-local already,
not shadows) and costs one slot-copy per *shared* variable the branch binds. For a clause with `k`
head variables unifying against a shared goal term, that is at most `k` shadows per branch — the
smallest possible binding store, created on demand.

C++ is required for exactly one thing here: **one arena per worker, pinned, never migrated.** A
`Mark` names dense-vector lengths in one arena (`struct Mark`, `arena_manifold.hpp:169`), so a mark
from arena A is meaningless in arena B; `release`/`discard`/`compact` are not thread-safe and the
class is single-owner by design. The branch-to-worker-to-arena pinning is scheduler state and
belongs in C++ (the worker pool, modeled on `render::WorkerPool`, per study §3.2). The *bindings*
stay structural; only the ownership of each arena is C++.

## 3. Should the OR-frontier itself be cells on a dimension? Yes — mostly

The choice-point frontier today is a C++ `std::vector<ChoicePoint> choiceStack_`
(`vortex_vm.hpp:151`) whose elements are `{cursor, altOp, Mark, stackFrame, cutBarrier}`
(`vortex_vm.hpp:87`). The `Mark` and the raw cursor PC are genuinely C++/arena-local and cannot be
anything else (see below). But the *shape of the search* — who is a child of whom, which branches
are to the left of a cut, what order solutions were submitted in — is relational, and relations are
what dimensions are for. Making it structural lets scheduling, cut-barriers, and solution collection
be rank walks instead of parallel C++ bookkeeping, which is the whole point of steering to cells and
dimensions.

Proposed dimensions, all **minted in a dedicated coordination arena owned by the host** (the one
thread that already serializes `promote`, study §3.3/§3.5), all **ephemeral** — they carry
`ephemeralBit` and are never promoted, because the search tree is not a document, it is the scaffold
a document answer falls out of:

- **`d.or`** — the parent/child frontier. A choice-point cell's posward `d.or` neighbours are its
  alternative branches, in the order the alternatives were created (clause order; alternation-arm
  order; generator-resumption order). This *is* the OR-tree: a branch's siblings are its `d.or`
  rank, its parent is the cell this rank hangs under. Scheduling "take the next untried alternative"
  is "step posward on `d.or`."
- **`d.branch`** — binds a frontier branch cell to the worker arena executing it. One link from the
  branch cell to a per-worker *arena-handle cell* (an ephemeral cell that stands for "worker N's
  arena"). This replaces the implicit `ChoicePoint::cursor`→worker mapping with a readable edge:
  "which worker owns this branch" is `linked(branch, d.branch)`.
- **`d.frontier`** — the live/ready/running/done lifecycle, modeled as a rank of state cells (one
  cell per state, à la the briefing's "variablehood is membership of a rank, not a flag bit",
  `vlog.cpp:23` comment). A branch's membership of the `running` cell's `d.frontier` rank says it is
  in flight; moving it to the `done` cell's rank is a single `insertIntoRank`
  (`arena_manifold.hpp:insertIntoRank`). The scheduler reads ready branches by walking one rank.
- **`d.submission`** — the deterministic merge order. When a branch is forked it is linked, in
  creation order, onto the tail of a `d.submission` rank. "First solution = lowest submission index"
  (study §3.5) becomes "the negward-most solved branch on `d.submission`," a rank walk, not a sort
  of completion timestamps. This is the structural form of the study's submission-index discipline.

What is minted vs. ephemeral: **all four are ephemeral.** They describe the search, which dies with
the query. `promote()` is the only road to persistence (`arena_manifold.hpp`, `promote` doc), and we
deliberately do not take it for frontier cells — only the *answer* subgraph reachable from the
result root is promoted ("promote the answer, not the search," same doc). The ephemeral bit is
load-bearing here: if a frontier cell ever leaked toward a store, `Store::setLink` and
`Manifold::applyStructure` refuse an `ephemeralBit` ref with the existing R8 machinery
(`arena_manifold.hpp`, class-doc), so a scheduler bug that tried to persist the OR-tree is caught
for free.

Where C++ stays. Each branch's actual binding state is its own worker arena's dense vectors plus its
`Mark`; those are not frontier structure and must not be. The coordination dimensions name branches
and order them; the branch *contents* (bindings, trail, PC) are per-worker arena state the frontier
cells merely point at via `d.branch`. In other words: **the tree is cells; the leaves' interiors are
C++.** That split matches the hardware — the tree is read by the host scheduler on one thread, the
leaves are mutated by workers on many, and crossing that line is exactly where `ephemeralBit` and
the serialized `promote` already sit.

## 4. Cut as a structural operation over the branch rank

Cut today is a pop-loop over the global C++ stack:

```cpp
void VortexVM::cut(std::size_t cutBarrier) {
  while (choiceStack_.size() > cutBarrier) {
    ChoicePoint cp = choiceStack_.back();
    choiceStack_.pop_back();
    core_.arena().discard(cp.mark);   // keep bindings, drop the retry
  }
}
```

(`vortex_vm.cpp:197`). `cutBarrier` is an index into the one stack (`vortex_vm.hpp:92`), set at push
time to `choiceStack_.size()` (`vortex_vm.cpp:164`). The semantics: discard — not release — every
choice point younger than the barrier, keeping their bindings but destroying the ability to retry
them. `discard(mark)` is the arena's "give up a choice point without undoing it ... only the ability
to retry is discarded" (`arena_manifold.hpp:601`).

Structurally, with the `d.or` frontier of §3, **cut is a prune of a sub-rank.** The cut's barrier is
not an integer index but a *cell*: the choice-point cell that is the cut's parent frontier node. Cut
means "remove every `d.or` sibling of me that is posward of the barrier, and every subtree hanging
off them." Concretely:

1. From the cutting branch, walk negward on `d.or` to the barrier cell (the predicate-entry choice
   point — in vprolog, the clause-selection node; in vicon, the generator whose scope the cut, or
   the implicit cut in `if/then`, closes).
1. For each branch cell posward of the barrier on that `d.or` rank: move it from the
   `running`/`ready` cell's `d.frontier` rank to a `cut` cell's rank (one `insertIntoRank`), and
   emit a cancellation to the worker named by its `d.branch` edge.
1. The cut branch itself survives; its already-made bindings stand because the *arena* discards
   (keeps bindings), never releases.

This makes "cut prunes branches to its left/right" a reachability statement over `d.or` instead of
an index comparison, and it makes the cross-worker part honest: a pruned branch may be *running on
another worker*, so the structural prune must be paired with a C++ cancellation. That cancellation
is `std::jthread`/`std::stop_token`: the worker executing a pruned branch holds a `stop_token`, the
host requests stop when it moves the branch to the `cut` rank, and the worker checks the token at
its `stepScheduler` boundary (the same granularity at which cursors are already stepped) and
abandons the branch, releasing its arena mark. The study's non-migration rule (§3.2) is preserved:
we never move a running cursor, we only *cancel* it from the outside and let it unwind locally.

So: the barrier and the prune are structural (`d.or` + `d.frontier`); the "stop a branch that is
physically executing elsewhere" is irreducibly C++ (`stop_token`), because stopping a thread is an
OS primitive and the "new C++ must justify why it is not Vortex" rule (AGENTS.md) names scheduler/OS
primitives as exactly the allowed case.

## 5. vicon: `d.scan`, reversible assignment, and co-expressions

vicon makes every expression a generator, so a `#Choice` is pushed for `1 to 3`, `i < j`, `s ? e`,
and every `every` (study §6.6). Three vicon-specific states, read structurally:

**`d.scan` — per-worker scanning state, already in the right place.** String scanning keeps the
subject and position (`&subject`, `&pos`) on a `d.scan` rank in the arena (briefing, "vicon's unique
challenges"). Because the recommended model gives each worker its own `ArenaManifold`, each worker's
`d.scan` rank is private automatically — "two scans on different workers [cannot] corrupt each
other's position" (study §6.6). `&pos` moves are ordinary `link` edits on the `d.scan` rank under a
live mark, so they are trailed and `release`d on backtrack by the same `TrailEntry` mechanism that
undoes a binding (`arena_manifold.hpp:TrailEntry`, `trail()` doc). No new machinery: a scan cursor
is a cell with a `d.scan` edge, and backtracking a scan is `release(mark)` truncating that edit. The
structural statement is "scanning position is a `d.scan` link, not a C++ field," and it ships
between workers only in the sense that it never needs to — it is born in the worker that scans.

**Reversible assignment `<-` — a trailed `value()` write, confined to one worker.** `<-` is "an
ordinary `value()` write under a live mark, undone by the trail" (briefing). That is already a
structural operation: `setValueBits`/`setContent` on an arena cell, trailed if the cell predates the
mark (`arena_manifold.hpp`, `TrailEntry` + `trail()`), reverted by `release`. The parallelism
consequence is a *limit*, not a mechanism: because the undo is a per-arena trail and
`ChoicePoint::mark` names one worker's arena (study §3.4), **a single goal-directed evaluation that
uses `<-` cannot be split across workers.** It is the dependent-conjunct hazard (shared mutable
trail, study §4.3) and the honest answer is the same as vprolog's: do not split one search across
workers; split *between* independent searches (alternation arms, `e1 | e2`) that touch disjoint
variable sets. That independence is itself checkable structurally — two arms are independent iff the
sets of base cells they would shadow are disjoint, and `shadowOrder_`/`Mark::shadowCount` make that
set observable (`arena_manifold.hpp:180`).

**Co-expressions — pinned islands, shipped as a message not a thread.** `create e` / `activate`
(`@`) capture a suspended choice stack plus an arena mark in a pinned island on a
`d.pinning-cursors` rank (briefing). Represent the suspended co-expression as a **cell on
`d.pinning-cursors`** whose edges name: the entry opcode cell, the `d.branch` worker-arena handle it
was created in, and a frontier sub-rank (`d.or`) holding its suspended choice points. That cell is a
legitimate, movable *name* for the suspension — it can be shipped between workers as a message,
because it is just a `CellRef` and a handful of ephemeral edges. What **cannot** be shipped is the
live arena state the island points at: the choice stack and the mark live in the creating worker's
arena, and the migration rule (study §3.2) forbids moving a running cursor's arena across threads.
So activation on a *different* worker is not "move the island there"; it is "send a message to the
creating worker to resume it," exactly the Ractor-style option the study names (§3.3, §6.6). The
co-expression cell is the message's payload; the worker-pinning is enforced by its `d.branch` edge.
The spec already marks co-expressions optional (study §6.6), so this defers cleanly: the structural
island makes activation *expressible* as a message without ever moving arena state.

**How much of vicon stays serial.** Any single generator chain that threads `&pos`, `<-`, or a
shared variable through sequenced sub-expressions runs on one worker, for the dependent-trail reason
above. vicon's realistic parallelism is OR-parallel alternation only — `e1 | e2` arms whose
shadow-sets are disjoint — gated behind the same `system://settings` flag as vprolog (study §6.6).
The bounded/unbounded distinction tells the scheduler how much speculation is worth: an expression
is unbounded exactly when no `discard(mark)` is emitted at its boundary (briefing), so a bounded arm
is cheap to fork-and-cancel and an unbounded one commits the worker for the duration — a structural
read (presence or absence of the boundary `discard`) driving the C++ granularity decision.

## Summary: the cells-and-dimensions scorecard

| Concern                  | Structural (cells/dimensions)                                | C++ required, and why                                        |
| ------------------------ | ------------------------------------------------------------ | ------------------------------------------------------------ |
| Binding (`v = t`)        | `d.clone` splice (`vlog.cpp:86`); deref = `cloneMaster`      | —                                                            |
| Per-branch binding store | COW shadow of `v`'s slot per worker arena                    | one `ArenaManifold` per worker, pinned (Mark is arena-local) |
| OR-tree / frontier       | `d.or`, `d.branch`, `d.frontier`, `d.submission` (ephemeral) | branch *interiors* (bindings/trail/PC) are arena state       |
| Cut                      | prune a `d.or` sub-rank; `discard` keeps bindings            | `stop_token` to cancel a branch running on another worker    |
| Deterministic merge      | negward-most on `d.submission`                               | serialized `promote` on one thread (study §3.5)              |
| vicon `d.scan`           | `d.scan` rank, private per arena; `&pos` trailed             | —                                                            |
| vicon `<-`               | trailed `value()` write, reverted by `release`               | confines a search to one worker (shared-trail hazard)        |
| vicon co-expression      | cell on `d.pinning-cursors` as a shippable message           | arena state stays worker-pinned; activation is a message     |

The through-line: **the search is structure, the leaves are state.** Bindings, cuts, the OR-tree,
the scan position, and the merge order are all cells and dimensions and can be reasoned about with
rank walks and `ephemeralBit`. The irreducible C++ is the per-worker arena ownership, the thread
cancellation for cut, and the single serialized `promote` — each one an OS/scheduler primitive that
the architecture already isolates and that AGENTS.md's governance rule explicitly admits.
