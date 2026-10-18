# AND/OR parallel search — the runtime machinery

Section input for [`vicon-andor-parallelism.md`](vicon-andor-parallelism.md). Scope: the mechanics
that *execute* an AND/OR parallel search once the share-nothing per-worker-`ArenaManifold` substrate
of [`vortex-multithreading-study.md`](vortex-multithreading-study.md) (§3.4, §6.5) is in place. It
does not re-derive the substrate; it answers "given a tree of speculative choice-point branches,
each owning a private copy-on-write arena, what actually runs them, suspends them, ships them,
bounds them, and cancels them." Grounded in [`_andor_briefing.md`](_andor_briefing.md),
`vortex/vortex_vm.{hpp,cpp}`, `zigzag/vlog.hpp`, and `zigzag/arena_manifold.hpp`.

The through-line: **a speculative branch is a `(suspended choice point, private arena)` pair, and
the runtime's whole job is to create those pairs lazily, run each on exactly one worker for its
whole life, and tear them down — binding store and all — in one truncation when they are pruned.**
The arena model does the hard isolation work for free; the runtime only has to schedule and cancel.

## 1. Task and work models — expressing the AND/OR task graph

### 1.1 The shape of the graph

The AND/OR tree is already latent in the VM. An OR-node is a `ChoicePoint` (`vortex_vm.hpp:87`):
`{cursor, altOp, zigzag::Mark mark, stackFrame, cutBarrier}`. Today `pushChoicePoint`
(`vortex_vm.cpp:164`) records *one* alternative (`altOp`) and `backtrack` (`:178`) tries it only
after the current path fails — serial OR by cursor rewind. An AND-node is the ordinary sequencing of
opcodes along a cursor's program: conjuncts `a, b` are `b` reached after `a` succeeds. Parallelising
means turning the implicit "try `altOp` later" into "run `altOp` now on another worker," and the
implicit "run `b` after `a`" into "run `a` and `b` on two workers when they are independent."

Four task substrates are candidates. The briefing names all four; this section ranks them for *this*
graph.

### 1.2 Work-stealing deques (Cilk / TBB / Lace)

Work stealing gives each worker a double-ended queue of ready tasks; a worker runs its own deque
LIFO and steals from the *other* end of a victim's deque when idle ([Work stealing][ws];
[Cilk-5, PLDI'98][cilk5]). This is the right *load-balancer* for an AND/OR tree because the tree is
irregular and its shape is unknown until it runs: one clause fails in one unification, another
spawns a thousand resumptions. Static partition cannot balance that; the general study already flags
"start static, measure imbalance, add stealing only if needed" (§7, open questions). The deque's
LIFO-local / FIFO-steal discipline also matches depth-first search: a worker drives its own branch
to depth (LIFO, like serial backtracking, so the *local* behaviour and therefore the determinism
story stay close to the serial engine), and only *older, higher* branches — the ones nearest the
root, carrying the most work — get stolen. Lace's non-blocking split deque ([Lace]) is the concrete
design to copy if the deque is built in-tree, because it avoids a memory fence on the common local
push/pop.

**Tie to the arena model.** A stolen task must carry its binding store with it. In a conventional
work-stealer the stolen frame shares the heap with its parent; here it must not — two OR-branches
sharing one arena is exactly the multiple-binding collision (`Vlog::bind` splices the one `d.clone`
rank, `vlog.hpp`). So a stealable OR-task is not a stack frame pointer; it is the
`(ChoicePoint, arena)` pair of §3, and stealing it means handing a worker a private arena. This is
heavier than a Cilk steal (which just moves a pointer) and is the reason granularity control (§4)
matters more here than in a numeric Cilk program.

### 1.3 Lazy task creation

Lazy task creation (LTC) is the decisive technique and should be the default. The idea: do **not**
eagerly build a task object at every choice point. Run the first alternative inline on the current
worker, as the serial engine would, and leave behind only enough state that an *idle* worker can
later "promote" the untried alternative into a real task and steal it (survey in
[Dynamic Multiple Work Stealing][dmws]). The payoff is that on one core, or when every worker is
busy, the program degrades to the serial choice-point walk with near-zero overhead — no arena is
copied, no task is allocated, `pushChoicePoint` stays as cheap as it is now. Overhead is paid only
at the moment a steal actually happens.

This is the same move Concurrent Cilk makes — "lazy promotion from tasks to threads," paying the
full cost only for work that actually blocks or is actually stolen
([Concurrent Cilk, LCPC'15][ccilk]). For us the promotion is: *materialise the untried `altOp` as a
branch with its own arena only when stolen.* Until then the alternative lives where it lives today —
as `altOp` + the enclosing `Mark` on the owner's `choiceStack_`.

### 1.4 Futures / continuations

A future is the natural handle for "the solutions of this subgoal, eventually." It fits AND-parallel
join (a conjunct's producer side is a future the consumer awaits) and fits the result-merge: each
branch is a future delivering a solution stream, merged by submission index (§5, and the general
study's determinism rule). But a bare `std::future`/`std::promise` is too coarse — one blocking
value, a mutex and condvar per future, no cancellation, no composition. It is the wrong *primitive*
even though it is the right *mental model*. The continuation half matters more than the future half:
the thing we actually ship between workers is a continuation (§3), and a solution is delivered by
*invoking* that continuation's consumer, not by fulfilling a one-shot promise.

### 1.5 Senders / receivers (P2300 `std::execution`)

P2300 (voted into C++26) models async work as **senders** (a lazy description of work) connected to
**receivers** (completion handlers with three channels: `set_value`, `set_error`, `set_stopped`),
with algorithms like `when_all` and a built-in, structured **stop-token cancellation** path
([P3090 intro][p3090]; [P2300 cancellation tutorial][stdexec];
[P3409, efficient stop-token cancellation][p3409]). Three features map onto our graph almost
one-to-one:

- **Three completion channels == three choice-point outcomes.** `set_value` is a solution;
  `set_error` is a hard fault (e.g. `UnifyFailure` escalated, or `ArenaRefusal`); `set_stopped` is
  *exactly* a pruned branch — a cut killed it, or first-solution-wins made it unwanted. vicon/vlog
  "fail and backtrack" is **not** `set_error`; it is ordinary control flow inside one branch and
  stays below the sender boundary. The sender boundary is drawn at the branch, not at the goal.
- **`when_all` == the AND-join** of independent conjuncts: run both, complete when both have, and —
  critically — if one errors, the structured-concurrency rule requests stop on the siblings. That is
  the AND-side "one conjunct's failure prunes its siblings" we want, delivered by the framework.
- **Structured stop tokens == cut and first-solution cancellation.** `get_stop_token` on the
  receiver threads a cancellation signal down the whole subtree; P3409 adds the "finite number of
  children" guarantee that lets the implementation make it cheap. This is the right spine for §5.

**Recommendation on the task layer.** Use senders/receivers as the *compositional vocabulary* for
the AND/OR graph (it gives cancellation and join for free and is a standard lingua franca), running
over a work-stealing scheduler with lazy task creation as the *execution engine* underneath. P2300
is a protocol, not a scheduler — it needs a `scheduler` to run on, and the work-stealing pool is
that scheduler. Senders express *what* the graph is and *how it cancels*; the deque decides *where
and when* each branch runs; LTC decides *whether a branch is even reified*. Availability: P2300 is
C++26 and not yet in the repo's standard library, so the facade must follow the repo's
native-or-fallback discipline (`cpp26_select.hpp`; a pinned fallback such as
[Beman `execution`][beman], the way `function_ref`/`optional` are pinned in `cpp26.hpp`) — never
`#if __cpp_lib_*`. If adding a sender library is judged too heavy for Stage 5's opt-in scope, the
fallback is the deque + LTC core plus a hand-rolled stop flag (`std::stop_source`/`std::stop_token`
are C++20, already available via `std::jthread`), implementing only `when_all` and stop propagation
by hand. The sender vocabulary is preferred but not load-bearing.

## 2. How real parallel logic/functional runtimes schedule and reclaim speculative work

Five systems, each contributing one mechanism we reuse.

**OR-parallel Prolog: Muse stack copying, and stack-splitting.** Muse gives each worker a private
copy of the WAM stacks and copies the shared prefix on a steal, so each worker has its own binding
environment — directly dissolving the multiple-binding problem by *not sharing* the store
([or-parallel splitting survey][orsplit]). Stack-splitting (PALS on Beowulf clusters) refines this
by *partitioning* the untried alternatives between giver and taker instead of copying the whole
choice stack, which is what makes it work across distributed memory with no shared state ([PALS]).
The mapping is immediate and is the central claim of our design: **our per-worker `ArenaManifold`
copy-on-write overlay is Muse's private stack, done better.** Muse must *copy* the shared prefix on
a steal; our arena *overlays* a frozen shared base and copies a cell only on first write
(`arena_manifold.hpp` header: "the first write to one shadows it"). So the Muse copy cost is paid
lazily, per-written-cell, instead of eagerly per-steal. Stack-splitting maps to how we partition
`altOp` alternatives between the owning worker and a stealer (§4.3).

**Manticore: nested, cancellable schedulers.** Manticore builds parallelism from first-class
continuations and a hierarchy of schedulers, and treats cancellation of speculative work as a
primitive the scheduler supports, not an afterthought ([Manticore scheduling, ICFP'08][manticore]).
The lesson: cancellation must be designed into the scheduler's task representation from the start
(§5), so that killing a subtree is O(subtree) signal delivery, not a scan of every worker.

**GHC sparks and `par`/`pseq`.** `par x y` creates a *spark* — a hint that `x` may be evaluated in
parallel — placed in a spark pool; an idle worker may convert it to real work, or it may **fizzle**
if `x` is already being evaluated (e.g. the main thread got there first), costing almost nothing
([Marlow via Edwards, "Lazy and Parallel"][sparks]; [GHC sparks fizzling, SO][sparkso]). This is the
model for speculative OR: an untried alternative is a *spark*, not a thread. If the owning worker
backtracks into it before any stealer claims it, the spark fizzles and we have paid nothing — this
is lazy task creation (§1.3) seen from the reclamation side, and it is why speculation here is safe
to be generous with: unclaimed speculation is free.

**Koka / effect handlers.** Algebraic effect handlers reify a suspended computation as a resumable
continuation that the handler decides what to do with — resume once, resume many times, or drop. The
"resume at most once" case is the **one-shot** continuation
([Yarrow, effect handlers + regions][yarrow]). This frames vicon's generator precisely (§3):
resuming a generator is invoking its captured continuation, and because a given suspension is
resumed by exactly one worker exactly once per resumption, it is one-shot per resumption even though
the *generator* yields many times.

**WAM-on-threads.** The clusters-of-multicores or-parallel work
([multicore or-parallel Prolog][moc]) confirms the substrate prerequisite the general study already
states: concurrent backtracking is only expressible once the choice stack and its bindings are
per-worker, not global. Today they are global (`std::vector<ChoicePoint> choiceStack_`,
`vortex_vm.hpp:151`; one `core_.arena()`), which is exactly why §6.5 of the study localizes both.
Nothing in this section is reachable before that localization lands.

**Synthesis.** Private binding store per branch (Muse → our arena overlay), speculation as cheap
hints that fizzle for free (GHC sparks → LTC), cancellation as a scheduler primitive (Manticore →
P2300 stop tokens), suspension as a one-shot resumable continuation (Koka → §3), all gated on
per-worker choice stacks (WAM-on-threads → study §6.5).

## 3. Continuation capture for a suspended generator

This is vicon's crux: *every expression is a generator* (briefing; study §6.6), so a suspended-and-
resumable branch is the unit of work, and we must be able to represent it **as data** that a worker
can hold and, when stolen, that another worker can run.

### 3.1 What a vicon generator already is, in the code

A generator's suspended state is a `ChoicePoint` (`vortex_vm.hpp:87`): the `cursor` (which carries
the program counter via `cursorPC_`/`getCursorOpcode`), the `altOp` to resume into, the arena `Mark`
naming the point the arena's dense vectors must truncate back to on failure, the `stackFrame`, and
the `cutBarrier`. That is the entire continuation — vicon does not need a captured native call
stack, because resolution/generation state is already reified on the choice stack and in the arena,
not on the C++ stack. `pushChoicePoint` even snapshots `core_.arena().mark()` into it
(`vortex_vm.cpp:172`). **The suspended generator is therefore already heap data, by construction** —
the hard part other runtimes solve by stack-copying or stack-switching, vicon gets for free because
its VM reifies control rather than riding the host stack.

### 3.2 One-shot vs multi-shot, and why vicon is one-shot-per-resumption

Dybvig et al. distinguish continuations invoked *once* (one-shot) from those invoked *many* times
(multi-shot); one-shot continuations can be implemented without the copy overhead inherent to
multi-shot, because the captured stack need not be preserved for a second invocation
([one-shot continuations][oneshot]). A vicon generator *yields* many values, which sounds
multi-shot, but the mechanism is: each resumption consumes the current suspension and produces the
*next* suspension. Any single `ChoicePoint` is resumed **once** — resuming it mutates the cursor to
`altOp` and releases the arena to its `Mark` (`backtrack`, `vortex_vm.cpp:178`), destroying the
suspension it resumed from. So each suspension is one-shot; the generator's multiplicity is a
*chain* of one-shot resumptions, not one multi-shot continuation. This is the single most important
representational fact for parallelism: **we never have to clone a live continuation.** A branch is
handed to a worker, resumed once, and either succeeds (yielding a new suspension) or fails (and is
released). No two workers ever hold the same live suspension — which is the control-flow analogue of
no two workers sharing one arena.

### 3.3 Representing a shippable branch

A branch shipped to a stealer is the pair:

```text
struct SpeculativeBranch {
  ChoicePoint    suspension;   // cursor, altOp, mark, stackFrame, cutBarrier (vortex_vm.hpp:87)
  ArenaManifold  arena;        // private COW overlay on the frozen shared base
  std::uint32_t  submissionIndex; // for deterministic merge (§5, study determinism rule)
  std::stop_token stop;        // cut / first-solution cancellation (§5)
};
```

Two subtleties.

- **The `Mark` is arena-relative.** `Mark` is twelve `std::uint32_t` high-water lengths into *one
  arena's* dense vectors (`arena_manifold.hpp:169`). A `Mark` from worker A's arena is meaningless
  in worker B's. So shipping a branch does **not** mean shipping a bare `ChoicePoint` and
  reattaching it to a foreign arena — it means the branch *carries its arena*. The arena and the
  choice stack travel together or not at all. This is why the briefing's "a generator is confined to
  one worker for its life" (study §6.6) holds: a *running* cursor cannot migrate (study §3.2), but a
  *suspended* branch can be shipped *before it is resumed*, as a whole `(suspension, arena)` unit,
  and then runs to completion on the taker. Migration-of-the-running is forbidden;
  stealing-the-suspended is the design.
- **Segmented/incremental copy is the fallback, not the default.** When the arena to ship is large,
  the Hieb/Dybvig stack strategy — bound the copy on reinstatement rather than copy everything
  ([first-class continuations][firstclass]) — maps to copying only the *shadowed* slots above the
  branch point (the arena already tracks `shadowCount` per `Mark`, `arena_manifold.hpp`), i.e.
  stack-splitting's "partition, don't copy the whole stack." But the default is better than either:
  because our arena is COW over a *shared frozen base*, a stealer that overlays the *same* base
  needs none of the base copied — only the stealing branch's own writes. The ship cost is the
  shadowed-slot set, not the arena. This is the one place the design beats classic Muse/PALS
  outright.

### 3.4 Segmented stacks, briefly

Heap-allocated continuation closures vs. segmented stacks vs. copied call stacks is the design axis
in Farvardin/Reppy ([From Folklore to Fact][folklore]). vicon sidesteps it: its continuation is not
a native stack at all but reified VM state, so we need neither segmented stacks nor stack copying
for the *VM-level* generator. Native segmented stacks would only re-enter the design if a branch
called into deep *C++* (host builtins) and had to suspend mid-call — which vicon's model does not
do, because builtins run to completion within one opcode step. Record this as a boundary condition,
not a mechanism we build.

## 4. Granularity control — when forking costs more than running inline

### 4.1 The problem, stated for this engine

Classic oracle scheduling: run small tasks in parallel and the task-creation overhead swamps the
work; run large tasks sequentially and cores idle ([oracle scheduling][oracle]). Our fork cost is
higher than a numeric Cilk program's, because forking an OR-branch means giving it a private arena
overlay and shipping its shadowed slots (§3.3), not just enqueuing a closure. So the break-even
grain is larger, and the default must bias toward *inline* execution.

### 4.2 Lazy task creation as the primary control

LTC (§1.3) is itself the main granularity mechanism: because an untried alternative is left as a
cheap spark and only reified-with-arena when actually stolen, the *common* case — enough work on
every worker — never pays the fork cost at all. A choice point that the owner backtracks into before
anyone steals it fizzles like a GHC spark (§2), costing one deque push/pop. So granularity control
is mostly "make not-forking free," and LTC does that. Leapfrogging / the deque discipline then
ensure a stealer takes the *oldest* (root-nearest, biggest-subtree) spark, maximising stolen grain
per steal — the leapfrogging intuition from the briefing and the deque's FIFO-steal end (§1.2).

### 4.3 Threshold heuristics, as config not magic numbers

Where LTC alone is not enough (pathologically shallow-and-wide searches that create many tiny
sparks), add admission thresholds, every one of them a `system://settings` key per the briefing's
"config in `system://settings`, not magic numbers" constraint — never a literal in the scheduler:

- **`andor.minSpeculationDepth`** — do not expose a choice point as stealable until the search is at
  least this deep, so trivial top-level alternations stay inline. (vicon's bounded-vs-unbounded
  distinction, below, is a *structural* version of this.)
- **`andor.maxLiveBranches`** — cap concurrently reified branches (each owns an arena); beyond it,
  new alternatives stay serial sparks. This bounds arena memory, the real resource, directly.
- **`andor.minArenaShipCost`** — if the shadowed-slot set a branch would have to ship exceeds a
  threshold, *keep it on the owner* (the ship would cost more than resuming inline). Measurable
  because the arena tracks `shadowCount`/trail size per `Mark` (`arena_manifold.hpp`).

Defaults live in `defaultSettingSpecs()` (study/briefing); the engine reads them at start, as the
study requires config to.

### 4.4 vicon's bounded/unbounded distinction is free granularity information

The briefing's sharpest vicon-specific lever: a bounded expression emits a `discard(mark)` at its
boundary, an unbounded one does not (study §6.6; `arena_manifold.hpp` `discard` keeps bindings and
drops the retry). That boundary marker *is* a granularity signal the compiler can hand the scheduler
for free: an expression known-bounded has a small, known number of resumptions and is a poor fork
candidate (run it inline — its whole yield set is cheap); an unbounded or large-bounded expression
(`i < j` over a wide range, `every` over a big generator) is where forking an OR-branch pays. So
`andor.minSpeculationDepth`'s structural counterpart is "only fork at unbounded choice points,"
derivable from the `discard` the compiler already emits — no runtime profiling needed for the common
case. This is a contribution point: Icon-family literature is thin here (briefing), and the
bounded/unbounded boundary gives a static grain oracle that general work-stealers lack and must
approximate dynamically.

## 5. Cancellation and pruning — cut and first-solution-wins

### 5.1 What cut must do across workers

Serial cut is `cut(barrier)` (`vortex_vm.cpp:197`): pop and `discard` every `ChoicePoint` above a
`cutBarrier` index into the single `choiceStack_`. Once the stack is per-worker (study §6.5),
`cutBarrier` indexes into the *owning* branch's stack — fine for choice points still on that stack.
The hard case is the branches that cut prunes which have already been **stolen** and are running on
other workers: those are no longer on the cutting worker's stack to `discard`. Cut must become
*cancellation* of those sibling subtrees.

This is where senders/receivers earn their place (§1.5). Each speculative branch carries a
`std::stop_token` (§3.3) wired to a `std::stop_source` owned by the parent choice frame. Executing a
cut requests stop on the sources for every alternative to the cut's left-plus-right that was spun
off as a branch — P2300's structured stop propagation delivers it down each stolen subtree
([P3409]); the Manticore lesson (§2) is that this must be a scheduler primitive so it is O(subtree),
not O(workers). A cancelled branch completes on its `set_stopped` channel. **The reclamation is
trivial and this is the arena model paying off again:** a cancelled branch simply drops its private
arena. Because the arena is a share-nothing COW overlay, dropping it reverts every `d.clone` splice,
every `d.scan` position move, every reversible-assignment write that branch made, in one destruction
— no trail-unwinding across workers, no shared binding to repair. Cut semantics are preserved not by
coordinating the prune but by *discarding isolated state*, which is exactly `discard(mark)`'s serial
meaning lifted to a whole worker's arena.

The honesty limit: a branch between a stop request and its next cancellation-check point keeps
running briefly (cooperative cancellation — P2300 and `stop_token` are both cooperative). It wastes
a little work but cannot corrupt anything, because its writes are confined to its own arena and will
be dropped. Cut therefore stays *semantically* exact (no pruned solution ever escapes) while being
only *eventually* efficient. The check point is the natural one: between opcode steps in `run`
(`vortex_vm.cpp` `run`/`stepScheduler`), test the token, as the backtrack loop already tests the
choice stack.

### 5.2 First-solution-wins

First-solution (the common REPL/`vquery` case, and vicon's bounded-context "is there any?") is cut's
cousin: the lowest-submission-index branch to produce a `set_value` wins, and every other branch is
cancelled by the same stop-token spine. Determinism is preserved by the general study's rule — merge
and "first" are by *submission index*, not wall-clock arrival (briefing hard-question 4). So a
higher-index branch that happens to finish first does **not** win: its solution is held, and if a
lower-index branch later produces one, the lower-index one is reported and the higher-index branch's
is discarded along with its arena. This makes first-solution-wins *deterministic across worker
counts* — the language-visible promise the study makes for the serial-vs-parallel equivalence. The
cost is that we cannot stop lower-index branches the instant any branch finishes; we can only stop
*higher*-index branches when a *lower*-index one finishes, and stop everything once the lowest-index
live branch finishes.

### 5.3 All-solutions and the AND-join

For all-solutions (`every`, `findall`-shaped queries), nothing is cancelled by success; branches run
to exhaustion and their solution streams merge by submission index into one ordered stream (stable
merge, `when_all`-shaped but streaming). The AND-join across independent conjuncts uses the same
`when_all` structure: a conjunct's hard failure requests stop on its siblings (§1.5), its success
feeds the next conjunct. Dependent AND-parallelism — conjuncts sharing an unbound variable — is
**out of scope for the runtime to parallelise** and must be left serial, because two workers binding
the shared variable is the multiple-binding problem reintroduced through the AND side and the arena
model forbids it (the branches would need to *share* a `d.clone` splice, not isolate it). That
boundary is the sibling of vicon's "a single goal-directed evaluation cannot span workers" (study
§6.6) and is stated as a honest limit, not solved here.

## 6. Recommended runtime shape

A single picture, bottom to top, each layer tied to the arena model:

1. **Worker pool with work-stealing deques** (Lace-style non-blocking split deque to avoid local
   fences), one deque per OS worker, M:N over the general study's scheduler. Each worker owns one
   private `ArenaManifold` overlaying the one frozen shared base — the share-nothing invariant that
   dissolves the multiple-binding problem (§2, Muse → arena overlay).
1. **Lazy task creation on top of the deque.** A choice point's untried `altOp` is left as a cheap
   spark on the owner's deque, not a reified task. The owner runs the first alternative inline
   (serial semantics, serial speed). A spark the owner backtracks into first *fizzles* for free
   (§4.2, GHC sparks).
1. **A stolen spark reifies into a `SpeculativeBranch`** =
   `(ChoicePoint suspension, private arena, submissionIndex, stop_token)` (§3.3). It carries its
   arena; it is resumed **once** (one-shot per resumption, §3.2); it runs to completion on the taker
   and never migrates while running (study §3.2). Reification cost = the shadowed-slot set only,
   because base is shared COW (§3.3 — the one place we beat Muse/PALS).
1. **Senders/receivers as the composition and cancellation vocabulary** (P2300, via a pinned
   `cpp26`-style fallback until the stdlib has it). `set_value`/`set_error`/`set_stopped` ==
   solution/fault/pruned; `when_all` == AND-join of provably-independent conjuncts; `get_stop_token`
   == the cut / first-solution cancellation spine (§1.5, §5). If P2300 is too heavy for Stage 5's
   opt-in scope, fall back to deque + LTC + hand-rolled `std::stop_source`/`when_all` (C++20,
   already present) — the vocabulary is preferred, not load-bearing.
1. **Cut and first-solution-wins are cancellation, not coordination** (§5). A prune requests stop
   down the stolen subtrees; each cancelled branch drops its private arena, reverting all its
   `d.clone` splices, `d.scan` moves and `<-` writes in one destruction. Determinism by submission
   index, so parallel first-solution equals serial first-solution across worker counts.
1. **Granularity is mostly "not-forking is free" (LTC)**, backed by `system://settings` thresholds
   (`andor.minSpeculationDepth`, `andor.maxLiveBranches`, `andor.minArenaShipCost`) and by vicon's
   static bounded/unbounded grain oracle — fork at unbounded choice points, run bounded ones inline,
   read straight off the compiler's `discard(mark)` boundary (§4.4).

Where cells and dimensions do the work (for the structure expert to extend): the binding store *is*
the `d.clone` overlay in the private arena; `d.scan` state is per-arena and therefore per-branch for
free; a suspended co-expression island on `d.pinning-cursors` is precisely a `SpeculativeBranch`
that is *not* placed on a deque — a pinned branch, activated by a message to its owning worker
rather than stolen (study §6.6, co-expressions worker-pinned). The OR-tree need not be a rank in the
store; reifying it as a rank would make speculation mint structure, which the arena model
specifically avoids (speculation is ephemeral; only `promote()` writes). Keep the OR-tree in the
deque/sender graph; promote only a chosen solution.

**One-line summary.** Speculative branch = one-shot suspended `ChoicePoint` + a private COW arena;
create them lazily (spark, fizzle-for-free), run each on one worker to completion, compose and
cancel them with senders/stop-tokens, and reclaim a pruned one by dropping its arena — so the arena
model does the isolation and the runtime only schedules and cancels.

## Sources

- Work stealing: [ws]; Cilk-5: [cilk5]; Concurrent Cilk: [ccilk]; Lace deque: [lace].
- Lazy task creation / granularity: [dmws]; oracle scheduling: [oracle].
- std::execution (P2300): intro [p3090]; cancellation tutorial [stdexec]; stop-token cancellation
  [p3409]; Beman impl [beman].
- Continuations: one-shot [oneshot]; first-class [firstclass]; stacks vs. continuations [folklore].
- Parallel runtimes: Manticore scheduling [manticore]; GHC sparks [sparks], [sparkso]; effect
  handlers [yarrow].
- OR-parallel Prolog: splitting strategies [orsplit]; PALS stack-splitting [pals]; multicore
  or-parallel [moc].

[beman]: https://github.com/bemanproject/execution
[ccilk]: https://www.cs.ox.ac.uk/people/timothy.zakian/lcpc15.pdf
[cilk5]: https://pages.cs.wisc.edu/~markhill/restricted/pldi98_cilk.pdf
[dmws]: https://www.researchgate.net/publication/258650225
[firstclass]: https://www.khoury.northeastern.edu/~shivers/cs6983/papers/dybvig-continuations.pdf
[folklore]: https://par.nsf.gov/servlets/purl/10201136
[lace]: https://ris.utwente.nl/ws/files/5346301/lace.pdf
[manticore]: http://manticore.cs.uchicago.edu/papers/icfp08-sched.pdf
[moc]: https://arxiv.org/html/1608.01499v1
[oneshot]: https://www.khoury.northeastern.edu/~shivers/cs6983/papers/dybvig-1shot-conts.pdf
[oracle]: https://chargueraud.org/research/2011/oracle/oracle_scheduling.pdf
[orsplit]: https://arxiv.org/html/1301.7690v1
[p3090]: https://open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3090r0.pdf
[p3409]: https://open-std.org/JTC1/SC22/WG21/docs/papers/2024/p3409r1.html
[pals]: https://arxiv.org/pdf/cs/0607040
[sparks]: http://www1.cs.columbia.edu/~sedwards/classes/2021/4995-fall/laziness.pdf
[sparkso]: https://stackoverflow.com/questions/4576734/
[stdexec]: https://github.com/maikel/tutorial_stdexec/blob/main/01_Sender_Factories.md
[ws]: https://en.wikipedia.org/wiki/Work_stealing
[yarrow]: https://arxiv.org/pdf/2607.15876v2
