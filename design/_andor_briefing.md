# AND/OR parallelism and vicon — focused design briefing

This is the shared, code-verified basis for a dedicated design doc on **AND/OR parallelism** in the
threaded Vortex model and **vicon's unique concurrency challenges**. It narrows the general
multithreading study to the single hardest problem that study deferred. Every subagent treats this
as authoritative. Paths are relative to the `study/vortex-multithreading` worktree root.

## What this doc must solve

The general study (`design/vortex-multithreading-study.md`) recommends share-nothing per-worker
`ArenaManifold`s over one immutable base, an M:N scheduler, per-worker `choiceStack_`/trail, and a
single serialized `promote()` merge. It explicitly **defers** the hardest case to "Stage 5, opt-in":
parallel search in the goal-directed languages. §6.4 (vprolog) and §6.6 (vicon) name it but do not
design it. This doc designs it.

Two intertwined problems:

1. **OR-parallelism** — explore alternative choices (clauses in vprolog/Vlog; alternation arms and
   generator resumptions in vicon) on different workers simultaneously. The classic hazard is the
   **multiple-binding problem**: two OR-branches bind the same logic variable to different terms at
   once, and a single shared binding environment cannot represent both.
2. **AND-parallelism** — solve conjoined subgoals (`a, b` in Prolog; sequenced generators in vicon)
   on different workers. The classic hazards are **independent vs. dependent** AND-parallelism
   (dependent conjuncts share variables → a shared mutable trail) and solution **join/cross-product**
   ordering.

And **vicon's unique challenges** on top of both, because in vicon *every expression is a
generator*, not just clause resolution.

## Verified ground truth (cite file:line in the doc)

### The logic engine today (`zigzag/vlog.hpp`, `vortex/vortex_vm.{hpp,cpp}`)

- A **term is a cell**; functor is content, arguments on the Inputs Wing (`+d.grab`, then `+d.step`).
  A **variable is a cell on the `d.vars` rank** with an empty clone master. (`vlog.hpp` struct
  `Vlog`.)
- **A binding is a splice of two `d.clone` ranks** (`Vlog::bind`): `bind(v,t)` splices `v`'s clone
  rank onto the posward tail of `t`'s so the combined master is `t`'s. `deref` is `cloneMaster`
  (`vlog.hpp`). **This is the crux of the multiple-binding problem**: two OR-branches that both bind
  `v` would both splice `v`'s one `d.clone` rank — they cannot coexist in one arena.
- `unify()` returns `std::expected<void, UnifyFailure>` and **leaves partial bindings on failure**
  for the caller's `release()` to truncate — "which is what Prolog does too, and for the same
  reason." Rational trees by default (`X = f(X)` builds a cycle; `cloneMaster` guards the walk).
- The VM logic opcodes are C++: `Unify` (`vortex_vm.cpp:789`), `Choice` (`:878`), `Fail` (`:887`),
  `Cut` (`:899`). `ChoicePoint{cursor, altOp, zigzag::Mark mark, stackFrame, cutBarrier}`
  (`vortex_vm.hpp`). The choice stack is **a single global `std::vector<ChoicePoint> choiceStack_`**
  and `cut()`/`cutBarrier` index into it. `pushChoicePoint` captures `core_.arena().mark()`.
- `backtrack()` pops a `ChoicePoint`, calls `core_.arena().release(cp.mark)`, resets the cursor to
  `altOp`. The clause database is the `d.clause` rank off a predicate cell. vprolog runs on these C++
  opcodes; clause-selection-in-Vortex and a Vortex-native solver are gated on U1 (`vlog` doc §6–§7).

### The arena undo model (`zigzag/arena_manifold.hpp`)

- `ArenaManifold` is dense vectors, not thread-safe, single-owner, copy-on-write overlay on a base
  `Manifold`. `mark()` captures twelve `std::uint32_t` high-water lengths and allocates nothing;
  `release(mark)` truncates back; `discard` keeps bindings and drops the retry; `compact()` is
  refused while a mark is outstanding. `promote()` is the only road to persistence and maps refs.
- **The mark/release model is inherently serial within one arena** — a `Mark` names a point in *that
  arena's* dense vectors. This is why OR-branches on different workers need *different arenas*, and
  why a binding made in one worker's arena is invisible to another by construction (the share-nothing
  property the general study relies on).

### vicon's unique challenges (`design/vicon-goal-directed-language.md`)

- Every expression is a generator = a resumable `ChoicePoint`. `1 to 3`, `i < j`, `s ? e`, `every`
  all push choice points. So OR-parallelism is not an opt-in corner — it is the evaluation model.
- **`d.scan` cursor state**: string scanning holds `&subject`/`&pos` on a `d.scan` rank; `&pos` moves
  are trailed and `release`d on backtrack. New per-arena state the general study's §1.1 inventory did
  not have.
- **Reversible assignment `<-`**: an ordinary `value()` write under a live mark, undone by the trail.
  Dependent on per-worker trail semantics.
- **Co-expressions** (`create e`, `@`): a suspended choice stack + arena mark held in a pinned
  island on `d.pinning-cursors`. The doc says "not threads... no OS thread." Under true threads,
  activating one on a different worker moves choice state across threads — forbidden by the general
  study's §3.2 (no migrating a running cursor). Marked optional in the spec.
- Bounded vs. unbounded expressions differ only by whether a `discard(mark)` is emitted at the
  boundary — relevant to how much speculative OR work is worth forking.

## Prior art the researchers must engage (name mechanisms, not just cite)

- **OR-parallel Prolog**: Aurora (SRI model — binding arrays), Muse (stack copying / incremental
  copying), versions-vectors, the **multiple-binding problem** and its three classic solutions
  (binding arrays, hashing windows, directory trees). Map each onto "per-worker arena = per-branch
  binding store" and ask which the copy-on-write overlay already gives for free.
- **AND-parallel Prolog**: &-Prolog / independent AND-parallelism, CDG (conditional dependency
  graphs), determinacy analysis; dependent AND-parallelism (DASWAM), producer/consumer on shared
  variables.
- **Goal-directed / Icon parallelism**: generator suspension, bounded/unbounded expression
  speculation, co-expression scheduling. There is far less literature here — this is where the doc
  contributes.
- Modern substrate: work-stealing deques (Cilk/TBB/`std::execution`), futures/continuations, lazy
  task creation, speculation + cancellation, the cost of forking a branch vs. the expected work.
- **C++23/26**: `std::jthread`/`stop_token` for cancelling abandoned speculative branches,
  `std::atomic_ref`, `std::barrier`/`latch`, `std::execution` (P2300 senders/receivers) as a possible
  structure for the AND/OR task graph, `std::hive`/`inplace_vector`, `std::expected`, `function_ref`
  from `<gleditor/cpp26*.hpp>`. Ground every primitive in what the repo can actually use (C++23, the
  cpp26 polyfill facades — never test `__cpp_lib_*`).

## The hard questions the doc must answer

1. How does an OR-branch get its own binding store? (Answer should lean on per-worker `ArenaManifold`
   copy-on-write over the shared base so each branch's `d.clone` splices are private — show it
   dissolves the multiple-binding problem, and state the copy/overlay cost.)
2. When is forking an OR-branch worth it vs. running it inline? (Speculation cost, lazy task
   creation, granularity; the bounded/unbounded distinction in vicon.)
3. How is **cut** semantically preserved across workers? (Cut prunes branches to its left; those may
   be running on other workers → cancellation. `cutBarrier` is an index into a now-per-worker stack.)
4. How are solutions merged deterministically? (Submission-index order from the general study;
   first-solution = lowest-index branch; the AND-parallel cross-product join order.)
5. Dependent AND-parallelism: can it be done at all here, or is it provably-independent-only?
6. vicon specifics: `d.scan` per-worker; `<-` reversible assignment across a fork; co-expression
   worker-pinning or message-passing activation; how much of vicon stays single-worker.
7. Where do cells and dimensions do the work? (The zigzag/vortex expert must push solutions toward
   structure: is a per-branch binding store a `d.clone` overlay? Is the OR-tree itself a rank? Is a
   suspended co-expression a pinned island that can be shipped as a message?)

## Constraints (AGENTS.md / general study)

- C++23, GNU Make, no CMake. `make format`/`lint`/`check`; clang-format 19.
- New C++ must justify why it is not Vortex (hardware/OS/allocator/scheduler primitives only).
- No sentinels in APIs (`optional`/`expected`); setters chain. Config in `system://settings`, not
  magic numbers.
- Do not break `sizeof(CompactOpNode)==64`, 64 KiB Merkle pieces, append-only, single promote writer.
- Must stay consistent with `vortex-multithreading-study.md` (per-worker arenas, M:N scheduler,
  serialized promote, determinism by submission index). This doc is a depth-first child of §6.4/§6.6,
  not a competitor.

## Deliverable

`design/vicon-andor-parallelism.md` in this worktree: a focused, code-grounded design for AND/OR
parallelism and vicon's unique challenges, with a concrete mechanism (favoring cells/dimensions), a
staged plan consistent with the general study's Stage 5, honest limits (what stays serial, what is
provably-independent-only), and C++23/26-grounded primitives. Must pass `mdformat --wrap 100`
(gfm/dollarmath/frontmatter) and `mdl -c .mdlrc`.
