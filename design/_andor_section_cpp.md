# AND/OR parallelism — the C++23/26 substrate

This section grounds the AND/OR runtime proposed in
[`vicon-andor-parallelism.md`](vicon-andor-parallelism.md) in concretely available C++. It is a
contribution to that doc, written by the C++ expert; it does not re-argue the model (per-worker
`ArenaManifold`s over one immutable base, M:N scheduling, serialized `promote()`, determinism by
submission index — all from [`vortex-multithreading-study.md`](vortex-multithreading-study.md)). It
answers a narrower question: *which standard-library primitive expresses each piece of the runtime,
is it actually available in this toolchain, and what does the code look like?*

Every primitive below is tagged one of three ways:

- **C++23 native** — in the compiler's standard library today, used directly.
- **cpp26 polyfill** — reached only through `gleditor::cpp26::*` from `<gleditor/cpp26*.hpp>`, which
  picks native-or-fallback once in `cpp26_select.hpp`. Per
  [`cpp26-compatibility.md`](cpp26-compatibility.md), **never test `__cpp_lib_*` directly**; the
  facade already did.
- **aspirational** — not in the toolchain today; a design target with a named fallback that is used
  instead now.

The hard invariant underneath all of it: an `ArenaManifold` "is dense vectors, not thread-safe,
single-owner" (`zigzag/arena_manifold.hpp` class comment and `mark()`/`release()` contract). Every
primitive here exists to keep that single-owner arena touched by exactly one thread at a time, and
to move only *immutable, published* results between threads.

## 1. Cancelling speculative and abandoned branches

Two events abandon a running branch: a **cut** prunes every OR-alternative to its left
(`VortexVM::cut`, `vortex_vm.hpp:cut(std::size_t cutBarrier)`), and **first-solution-wins** retires
every sibling once the lowest-submission-index branch has an answer. Both must stop work already
dispatched to other workers. The branch cannot be killed from outside — it owns an `ArenaManifold`
whose `release()`/`discard()` must run on its own thread — so cancellation is **cooperative**: the
branch polls a token at its natural yield points (the same points where a cursor would
`backtrack()`, `vortex_vm.hpp:backtrack`).

### `std::jthread` + `std::stop_token` / `std::stop_source` — C++23 native

`std::jthread` and the stop-token machinery are C++20, shipping in the toolchain; no polyfill is
needed. A branch worker receives a `std::stop_token`; the OR-group owns the `std::stop_source`. A
cut, or a first winner, calls `request_stop()`, and every sibling observes it at its next poll.

```cpp
// A branch is a resumable search over its OWN arena. It never touches a
// sibling's arena; cancellation only sets a flag it polls.
struct Branch {
  std::uint32_t submissionIndex;        // deterministic merge key (§2, §5)
  zigzag::ArenaManifold arena;          // single-owner, over the shared base
  gleditor::cpp26::function_ref<        // the per-branch closure (§4)
      BranchOutcome(zigzag::ArenaManifold &, std::stop_token)> run;
};

// Observed at every backtrack()/choice boundary, i.e. where the cursor would
// pop a ChoicePoint (vortex_vm.hpp ChoicePoint{cursor,altOp,mark,...}).
bool shouldAbandon(const std::stop_token &st) noexcept {
  return st.stop_requested();
}
```

A cut maps onto `request_stop()` on the sources of exactly the branches the `cutBarrier` names.
Today `cut(cutBarrier)` indexes one global `std::vector<ChoicePoint> choiceStack_`
(`vortex_vm.cpp:choiceStack_`); under the per-worker model that index becomes a *submission-index
range*, and the OR-group holds one `std::stop_source` per outstanding branch:

```cpp
class OrGroup {
public:
  // Cut: prune every alternative strictly left of the committing branch.
  // The classic cutBarrier (an index into the single choiceStack_) becomes a
  // submission-index threshold once the stack is per-worker.
  void cutTo(std::uint32_t cutBarrier) {
    for (auto &b : branches_)
      if (b.submissionIndex > cutBarrier)
        b.source.request_stop();     // cooperative; branch unwinds its own arena
  }

  // First-solution-wins: the lowest live index won, retire the rest.
  void retireSiblingsOf(std::uint32_t winner) {
    for (auto &b : branches_)
      if (b.submissionIndex != winner)
        b.source.request_stop();
  }

private:
  struct Live {
    std::uint32_t submissionIndex;
    std::stop_source source;
  };
  std::vector<Live> branches_;
};
```

Why cooperative and not `jthread`'s destructor-join-and-stop alone: a `jthread` destructor requests
stop and joins, which is the right default lifetime, but the *semantic* stop (cut, winner) happens
long before the object dies, and must be a plain flag the arena-owning thread checks between
`release()` calls. A branch that observes the token mid-unification still finishes the in-flight
`unify()` (which "leaves partial bindings behind on failure", `vlog.hpp` `unify` contract) and then
`release()`s its mark on its own thread — never from the canceller's.

Granularity caveat: an unbounded vicon generator (`1 to *`, a repeat alternation) may never reach a
yield point on its own. Poll on a bounded step counter in the generator's resume loop so an
abandoned unbounded branch cannot run forever — the bounded/unbounded distinction in the briefing is
exactly "whether a `discard(mark)` is emitted at the boundary", and the poll rides the same loop.

## 2. Coordination primitives for blackhole / one-forcer and the merge

### The blackhole / one-forcer pattern

When several branches independently demand the same not-yet-computed shared result (a memoized
subgoal; a vicon co-expression's first value), exactly one thread must compute it and the rest must
block until it is published — the "blackhole" of lazy evaluation. Two sound encodings, both C++23
native:

**`std::atomic<std::shared_ptr<T>>` (C++20, native).** The result is published once; late arrivers
load it. `compare_exchange` elects the single forcer.

```cpp
// One forcer computes; others publish-nothing and wait on the shared_ptr.
struct Cell {
  std::atomic<std::shared_ptr<const Solution>> solved{nullptr};
  std::atomic_flag claimed = ATOMIC_FLAG_INIT;   // elects the one forcer
};

std::shared_ptr<const Solution> force(Cell &c, auto compute) {
  if (!c.claimed.test_and_set(std::memory_order_acquire)) {
    auto result = std::make_shared<const Solution>(compute());
    c.solved.store(result, std::memory_order_release);  // publish (§5)
    c.solved.notify_all();
    return result;
  }
  std::shared_ptr<const Solution> seen;
  while (!(seen = c.solved.load(std::memory_order_acquire)))  // acquire (§5)
    c.solved.wait(nullptr, std::memory_order_acquire);        // atomic wait
  return seen;
}
```

`std::atomic<std::shared_ptr<T>>` and the atomic `wait`/`notify_all` free-wait interface are C++20 —
native, no polyfill. The published `Solution` must be *immutable* (a promoted answer, or an
arena-independent value): a thread that loads it must not reach back into the forcer's arena.

**`std::latch` (C++20, native)** when the number of consumers is known up front and the value is a
plain object the group waits on once:

```cpp
std::latch ready{1};
// forcer:  compute(); ready.count_down();
// waiter:  ready.wait();   // then read the now-stable result
```

`std::latch` is single-use; `std::barrier` (also C++20 native) is the reusable version for a
generation-structured join — e.g. an AND-parallel stage where all conjuncts must reach a rendezvous
before the cross-product join of the next stage begins:

```cpp
std::barrier stage{static_cast<std::ptrdiff_t>(conjunctCount)};
// each conjunct: produce its solution set, then
stage.arrive_and_wait();     // all conjuncts settled -> join can run
```

### `std::atomic_ref` (C++20, native) for the submission-index merge counters

Branch results are merged in submission-index order (the general study's determinism rule). The
merge needs a few hot counters — "lowest completed index", "next index to emit" — updated by many
workers without giving each counter its own `std::atomic<>` member inside an otherwise plain,
cache-friendly struct. `std::atomic_ref` lets a non-atomic field be touched atomically only where it
must be:

```cpp
struct MergeState {
  alignas(64) std::uint32_t lowestComplete{UINT32_MAX};  // see §4 on alignment
  // ... plain fields read single-threaded elsewhere ...
};

void recordComplete(MergeState &m, std::uint32_t idx) noexcept {
  std::atomic_ref<std::uint32_t> lc{m.lowestComplete};
  std::uint32_t cur = lc.load(std::memory_order_relaxed);
  while (idx < cur &&
         !lc.compare_exchange_weak(cur, idx, std::memory_order_acq_rel)) {
  }
}
```

The actual *ordering* of emitted solutions is not done with atomics — it is a serial drain in index
order by the single merge/promote thread (one writer, as the invariants demand). The atomics only
let branch threads announce completion cheaply; `atomic_ref` keeps the announced-upon struct a plain
value everywhere else.

## 3. Is `std::execution` (P2300) the right shape? — honest status

The AND/OR search *is* a task graph: OR-alternatives are a `when_any`-shaped race (first solution
wins), AND-conjuncts are a `when_all`-shaped join (cross-product of solution sets), and cut is
cancellation propagating through it. [P2300R10] senders/receivers express exactly this — structured
concurrency with built-in cancellation (via `get_stop_token` on the receiver's environment) and
value/error/stopped channels that mirror success/fail/cut.

**Status: aspirational, not available.** [`cpp26-compatibility.md`](cpp26-compatibility.md) lists
`std::execution` senders/receivers at **10–20 engineer-days** and **"Defer pending a concrete
threading and cancellation design for swarm or rendering work"**, sourced from NVIDIA stdexec which
upstream "calls experimental and tracks changing wording". There is no `std::execution` in the
repo's standard library and no pin in `thirdparty/`. We must **not** write the runtime against it
today, and must not add a `__cpp_lib_*` probe for it (forbidden).

**Instead: a thin internal task interface, modeled on `render::WorkerPool`.** `WorkerPool`
(`include/gleditor/render/worker_pool.hpp`) already proves the shape we need under C++23: parked
threads, work passed as `gleditor::cpp26::function_ref<void(std::uint32_t)>` ("borrowed, not owned
... keeps only a pointer to it, and only until this call returns"), the caller taking a share rather
than blocking, and first-exception propagation after all siblings finish. The AND/OR runtime wants
three things `WorkerPool::run` does not have — asynchronous completion (not block-until-all),
per-task cancellation, and submission-index results — so the interface is a sibling, not a reuse:

```cpp
// A deliberately small async task interface. Not std::execution; shaped so a
// future migration to senders/receivers is a reimplementation behind this
// facade, not a rewrite of callers. function_ref per WorkerPool's contract.
class SearchPool {
public:
  explicit SearchPool(std::uint32_t threads);   // parked threads, like WorkerPool

  // Dispatch one OR/AND branch. The closure is COPIED into the pool (unlike
  // WorkerPool's borrow) because a branch outlives the submit() call -- it runs
  // asynchronously. The copy is of a small handle; the arena is moved in.
  struct Ticket { std::uint32_t submissionIndex; };

  Ticket submit(std::uint32_t submissionIndex, std::stop_token st,
                std::move_only_function<BranchOutcome(std::stop_token)> run);

  // Drain completed branches in submission-index order (deterministic merge).
  // Returns nullopt when every outstanding branch is done or cancelled.
  std::optional<std::pair<Ticket, BranchOutcome>> nextInOrder();
};
```

Two honest caveats on the sketch: it uses `std::move_only_function` (C++23 native) rather than
`function_ref` because an *asynchronous* task genuinely outlives the submit call — `function_ref`'s
"do not store beyond the caller's callable lifetime" rule (`cpp26-compatibility.md`) forbids the
borrow here, which is the one place the `WorkerPool` pattern cannot be copied verbatim. Where a task
is synchronous (the caller blocks on a `when_all`-style join), `function_ref` is correct and should
be preferred, exactly as `WorkerPool::run` does. The migration target is: when `std::execution` is a
toolchain reality, `SearchPool` becomes a facade over a `static_thread_pool` scheduler, `submit`
returns a sender, `nextInOrder` becomes a `when_any`/`when_all` composition, and cut is the
`stopped` channel — with callers unchanged.

## 4. Data layout, false sharing, alignment

### `inplace_vector` for fixed-arity branch sets — cpp26 polyfill (implemented)

A term's argument list and a clause's head arguments are fixed-arity; an OR-group over a predicate
of *k* clauses, or an alternation of *k* arms, is a bounded fan-out known at build time.
`gleditor::cpp26::inplace_vector<Branch, K>` stores that branch set inline with no heap allocation —
the same win measured for the tether curve (`cpp26-compatibility.md`: "zero" allocations vs. one).
It is **implemented** in the repo (`<gleditor/cpp26_inplace_vector.hpp>`, native when
`__cpp_lib_inplace_vector >= 202603L` else the Beman pin), used only through the facade. [P0843R14].

```cpp
#include <gleditor/cpp26_inplace_vector.hpp>
// A predicate with up to kMaxClauses alternatives forks inline; a larger
// clause count (rare) falls back to std::vector, mirroring the tether policy.
template <std::size_t K>
using BranchSet = gleditor::cpp26::inplace_vector<Branch, K>;
```

Caveat from `cpp26-compatibility.md`: the Beman fallback's API still differs in spots
(`try_push_back` return type), so keep use to `resize`/`data`/`size`/`push_back` which agree across
both branches, as the tether code does.

### Work queue — `std::deque` now, `std::hive` aspirational

The runnable-branch queue of an M:N scheduler is a work-stealing structure: owners push/pop one end,
thieves steal the other. `std::deque` (C++23 native) backs a per-worker Chase–Lev-style deque today
and is what should ship. `std::hive` (P0447, the colony lineage) is attractive *only* if we need
stable references to in-flight branch records across insertion/erase — `cpp26-compatibility.md`
rates it **"Defer pending a stable-iterator workload"**, there is no pin, and it is reached through
no facade. So: **aspirational**; the queue uses `std::deque` and a free list, and we revisit `hive`
only if branch-record pointer stability becomes a measured need.

### `std::expected` for fallible fork — C++23 native

Forking a branch can fail for reasons that are not search failure: the arena overlay cannot copy a
cell (`ArenaRefusal`, `arena_manifold.hpp`), the pool is saturated, a speculation budget is
exhausted. These are `std::expected` (C++23 native), never a sentinel — matching `ArenaManifold`'s
own `ArenaResult = std::expected<ArenaManifold *, ArenaRefusal>` and the AGENTS.md "no sentinels
outside the wire" rule. `unify()` already returns `std::expected<void, UnifyFailure>` (`vlog.hpp`),
so the fork path composes with it:

```cpp
enum class ForkRefusal : std::uint8_t { PoolSaturated, BudgetExhausted, ArenaCopy };
using ForkResult = std::expected<Branch::Ticket, ForkRefusal>;

ForkResult forkOrBranch(SearchPool &pool, const Branch &parent,
                        std::uint32_t submissionIndex);
```

### `function_ref` for the per-branch closure — cpp26 polyfill (implemented)

The *synchronous* per-branch body (join stages, inline evaluation that does not outlive the call) is
`gleditor::cpp26::function_ref` — the exact pattern `WorkerPool::Work` uses (`worker_pool.hpp`).
Implemented in the repo. The asynchronous `submit` path is the documented exception (§3): it must
own its closure (`std::move_only_function`), because `function_ref` may not be stored past the
caller's callable lifetime.

### False sharing and alignment

Each worker owns an arena and a scheduler slot that other workers touch only through atomics
(completion flags, steal pointers, the merge counters of §2). Those touched-across-thread fields
must not share a cache line with a neighbour's, or an unrelated store false-shares and stalls. Align
the hot per-worker record to the hardware destructive interference size:

```cpp
#include <new>  // std::hardware_destructive_interference_size (C++17, native)

constexpr std::size_t kLine =
#ifdef __cpp_lib_hardware_interference_size
    std::hardware_destructive_interference_size;   // allowed: core-lang/library size, not a cpp26 facade probe
#else
    64;  // fall back to the common line; AGENTS.md: no magic number in an algorithm, so name it
#endif

struct alignas(kLine) WorkerSlot {       // one per OS worker; never two per line
  std::deque<Branch> ready;              // owner end + steal end (§4)
  std::atomic<std::uint32_t> stealCursor;// touched by thieves
  // the arena lives here too, but is single-owner (§5) and never shared
};
static_assert(sizeof(WorkerSlot) % kLine == 0 || alignof(WorkerSlot) == kLine);
```

Note the one permitted `__cpp_lib_*` test: `hardware_interference_size` is a core/library size
query, not one of the four cpp26 *facade* facilities, and there is no `gleditor::cpp26` wrapper for
it; the `cpp26-compatibility.md` rule is specifically about the facade-selected types. The merge
counters of §2 are themselves `alignas(64)` for the same reason.

## 5. Memory-model rules: publishing a result, keeping the arena single-threaded

Two rules carry the whole safety argument, and both are plain C++23 memory model, no polyfill.

**(a) Per-worker arena stays single-threaded — no atomics at all.** An `ArenaManifold` is "not
thread-safe, single-owner" (class comment). The discipline that makes this sound without locks: an
arena is *moved* to a worker (ownership transfer, `std::move` into the branch), mutated only by that
worker, and never read by another worker while live. The move itself is the synchronization edge —
the thread that constructs the branch and the thread that runs it are ordered by the
`submit`/dispatch handoff (the pool's internal mutex/`condition_variable`, exactly as `WorkerPool`
serializes its batch handoff). So `mark()`, `release()`, `unify()`, every `d.clone` splice
(`Vlog::bind`), every `d.scan` cursor move, and reversible `<-` assignment all run on one thread by
construction. The multiple-binding problem dissolves because two OR-branches *physically cannot*
splice the same `d.clone` rank: each holds a different arena, and the base they overlay is immutable
(copy-on-write shadow, `arena_manifold.hpp` overlay comment). No `d.clone` splice is ever visible
across threads, so there is nothing to make atomic.

**(b) Publishing a solved branch's result — one release, matching acquire.** When a branch succeeds,
its answer must cross to the merge/promote thread. The answer is *not* the arena — the arena is torn
down on the branch's own thread after it `discard()`s its winning mark. The answer is an immutable
value: either a promoted `Promoted{version, cells}` (`promote()` ran on the serialized promote
thread) or, before promotion, a flat, arena-independent description of the solved bindings. Publish
it with release, consume with acquire:

```cpp
// Branch thread: finish, then PUBLISH. Nothing the branch wrote to its arena
// escapes; only this immutable Solution does.
void publish(std::atomic<std::shared_ptr<const Solution>> &slot,
             std::shared_ptr<const Solution> answer) {
  slot.store(std::move(answer), std::memory_order_release);   // (b): release
}

// Merge thread: the acquire pairs with that release; after it, every write the
// branch made to *answer* (not the arena) is visible and stable.
std::shared_ptr<const Solution> consume(
    const std::atomic<std::shared_ptr<const Solution>> &slot) {
  return slot.load(std::memory_order_acquire);                // (b): acquire
}
```

The release/acquire pair is the *only* cross-thread ordering the result path needs: it establishes
happens-before from "branch finished writing `*answer`" to "merge thread reads `*answer`". Because
`Solution` is immutable once published (and does not alias the arena), there is no data race even
though many branches publish into many slots. The merge thread then drains slots in submission-index
order (§2) — the serial drain is where determinism is enforced, not in the atomics.

Cut cancellation (§1) needs no stronger ordering: `stop_source::request_stop()` and
`stop_token::stop_requested()` carry their own `acq_rel` internally (C++20 guarantees), so a cut
observed by a sibling happens-after the cut that set it.

### Summary of availability

| Primitive                                | Standard | Status in this repo                          |
| ---------------------------------------- | -------- | -------------------------------------------- |
| `std::jthread`, `stop_token/stop_source` | C++20    | native                                       |
| `std::atomic<std::shared_ptr<T>>`        | C++20    | native                                       |
| atomic `wait`/`notify_all`               | C++20    | native                                       |
| `std::latch`, `std::barrier`             | C++20    | native                                       |
| `std::atomic_ref`                        | C++20    | native                                       |
| `std::expected`                          | C++23    | native (used by `unify()`, `ArenaResult`)    |
| `std::move_only_function`                | C++23    | native (async task body only)                |
| `std::deque` work queue                  | C++23    | native                                       |
| `hardware_destructive_interference_size` | C++17    | native (not a cpp26 facade)                  |
| `gleditor::cpp26::function_ref`          | C++26    | **cpp26 polyfill, implemented** (sync body)  |
| `gleditor::cpp26::inplace_vector`        | C++26    | **cpp26 polyfill, implemented** (branch set) |
| `std::hive`                              | C++26    | **aspirational** — `std::deque` for now      |
| `std::execution` (P2300)                 | C++26    | **aspirational** — `SearchPool` facade now   |

Papers: function_ref [P0792R14], inplace_vector [P0843R14], senders/receivers [P2300R10]; optional
references [P2988R9] and ranges [P3168R2] and `span::at` [P2821R5] are implemented facades available
if the merge path wants them, though the result path above does not require them.
