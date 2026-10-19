# Interpreter / language-runtime section: true multithreading in the Vortex VM

This section is the language-runtime expert's contribution to
[`vortex-multithreading-study.md`](vortex-multithreading-study.md). It is grounded in the briefing
([`vortex-multithreading-briefing.md`](vortex-multithreading-briefing.md)) and in the code under
`apps/common/xanadu/vortex/`, `vql/`, `vpl/`, `vprolog/`, and `zigzag/arena_manifold.*`. It argues
from precedent: every claim about a comparable runtime names the mechanism that runtime actually
uses, and then maps it onto Vortex's concrete types (`VortexCore`, `VortexVM`, `ArenaManifold`,
`ChoicePoint`, `choiceStack_`).

The one-sentence framing: Vortex's `ArenaManifold &arena_` is a shared, non-thread-safe,
mutate-on-nearly-every-opcode heap with global checkpoint/rollback, which is structurally the same
object that forced CPython to keep a GIL for thirty years. The question is not whether a naive
shared-heap thread model would bottleneck — it would, exactly as the owner fears — but which of the
escape routes other runtimes took fits an arena whose undo is vector truncation.

## 1. Survey: how comparable runtimes solved exactly this

### 1.1 CPython — the GIL, PEP 684 per-interpreter GIL, PEP 703 free-threading

CPython's Global Interpreter Lock exists because `PyObject` reference counts
(`Py_INCREF`/`Py_DECREF`) are mutated on essentially every operation, including pure reads of shared
objects. A read of a global touches its refcount, so even read-mostly workloads write shared memory.
Making every refcount atomic was measured (Larry Hastings' "Gilectomy," 2016) to slow single-thread
execution by roughly 2x because atomic RMW defeats the CPU's store buffer and cache-line ownership
optimizations. The GIL was cheaper than correctness-by-atomics.

The two escape routes CPython actually shipped are the two that matter here:

- **PEP 684 (per-interpreter GIL, 3.12).** Multiple `PyInterpreterState` objects, each with its own
  GIL, its own object allocator, and (the hard part) its own copy of all previously process-global
  state. Threads in different interpreters run truly in parallel because they share no mutable
  objects; they communicate by copying or by a narrow channel. This is *share-nothing isolation
  retrofitted onto a shared-heap runtime*, and the cost was moving every `static` global into
  per-interpreter state.
- **PEP 703 (free-threading, 3.13+ experimental).** Remove the GIL outright. The mechanisms are
  specific and instructive: biased reference counting (the owning thread uses non-atomic increments,
  other threads use a separate atomic field), immortalization of permanent objects (interned
  strings, small ints, `None` — never refcounted at all), per-object locks on containers, and
  deferred reference counting for objects the interpreter touches constantly. It works, but it is
  years of fine-grained-locking work on a mature codebase and still carries a single-thread penalty.

The lesson for Vortex: fine-grained locking of a mutate-everywhere heap is the most expensive option
and buys the least per engineer-month. Per-interpreter isolation (PEP 684) was the cheaper and
sounder structural fix, and it is the one that maps cleanly onto an arena.

### 1.2 Ruby — Ractors (share-nothing with movable/immutable sharing)

Ruby kept its GVL (Global VM Lock) and added Ractors (Ruby 3.0) rather than remove it. A Ractor is
an actor with its own GVL; two Ractors run in parallel. The sharing rule is the whole design: only
*shareable* objects (deeply frozen/immutable, or classes/modules) may be referenced from more than
one Ractor; everything else is either **copied** or **moved** (ownership transfer, the sender loses
the reference) across the channel. This is Erlang's discipline bolted onto a mutable-object
language, with the type system (frozen-ness) enforcing the share-nothing boundary at send time.

The lesson for Vortex: if the arena is the unit of ownership, "shareable" maps onto "the base
`Manifold`, which is immutable while read through" and "move" maps onto `promote()` plus ref
remapping. Ruby had to invent frozen-ness; Vortex already has the immutable base and the ephemeral
bit.

### 1.3 JVM / CLR — shared heap, per-thread stacks, concurrent GC

The JVM and CLR are the counterexample: a genuinely shared mutable heap with real OS threads. They
make it work with (a) a per-thread stack and program counter — the *execution* state is never shared
— (b) a memory model (JMM, JSR-133) that defines exactly which writes become visible to which reads
across threads, enforced by the programmer with `synchronized`/`volatile`/`java.util.concurrent`,
and (c) a concurrent, parallel garbage collector (G1, ZGC) that is a major standing engineering
investment. The runtime does not protect you from a data race on a shared `HashMap`; it gives you
the primitives and a defined model, and incorrect programs get corrupted state.

The lesson for Vortex: the shared-heap model is viable but it is a decade-scale GC-and-memory-model
investment, and it pushes correctness onto the Vortex-language programmer. Vortex has no memory
model and its "GC" (`collectGarbage()`, `pin()`) is a single-threaded mark over the arena. Choosing
this path means building a memory model and a concurrent collector for the arena, from scratch.

### 1.4 Erlang / BEAM — share-nothing processes, per-process heaps, message copying

BEAM is the purest share-nothing design. Each Erlang process has its own small heap and its own GC;
processes never share mutable memory; all communication is by copying immutable messages into the
receiver's heap. Because no two processes touch the same memory, the scheduler runs N schedulers on
N cores with no global lock on the normal path, per-process GC pauses only that process, and the
system scales near-linearly on embarrassingly parallel workloads. The cost is message-copy overhead
and that you cannot cheaply share a large mutable structure — you restructure the problem instead.

The lesson for Vortex: this is the model the cursor abstraction already gestures at. A cursor is a
cheap, independently-scheduled unit (`spawnCursor`, `vortex_vm.cpp:127`); the single thing stopping
it from being a BEAM process is that all cursors share one `arena_` instead of each owning a heap.

### 1.5 Go — M:N scheduler, goroutines on OS threads, shared memory + channels

Go multiplexes M goroutines onto N OS threads (GOMAXPROCS) with a work-stealing scheduler: per-P
(processor) run queues, a global run queue, and stealing when a P goes idle. Goroutines are green
threads *scheduled across real OS threads*, which is exactly the step Vortex has not taken —
Vortex's `stepScheduler()` (`vortex_vm.cpp:1076`) is a single-threaded `for` loop over
`activeCursors()`, with no P, no OS thread, no stealing. Go's heap is shared and its GC is
concurrent; "share memory by communicating" is advice, not enforcement, and `go test -race` exists
precisely because the model permits data races.

The lesson for Vortex: Go shows the *scheduler* shape to copy (M:N, per-worker run queues, work
stealing) independently of the heap decision. Vortex can adopt Go's scheduler shape while rejecting
Go's shared heap in favor of BEAM's per-arena isolation — the two choices are orthogonal.

### 1.6 WAM-based parallel Prolog — Aurora/Muse OR-parallelism, AND-parallelism

This is the most directly relevant precedent because vprolog *is* a WAM-shaped engine over
`choiceStack_` and the arena trail. Classic parallel-Prolog systems split two ways:

- **OR-parallelism (Aurora, Muse).** Different alternatives of a choice point are explored in
  parallel. The central problem is the *multiple-binding problem*: two branches want to bind the
  same logic variable differently, so a single shared binding array/trail cannot hold both. Aurora
  solved it with the **SRI model** (binding arrays per worker, with a shared tree of choice points);
  Muse solved it by giving each worker **its own copy of the stacks** and copying the relevant stack
  segments on task hand-off (incremental copying). Both conclude the same thing: *the trail and the
  choice stack cannot stay global and shared.*
- **AND-parallelism (independent/dependent, &-Prolog, Andorra).** Conjoined goals that do not share
  unbound variables are solved in parallel. The analysis that proves two goals are *independent* (no
  shared unbound variable) is exactly the analysis that proves two branches can own separate arena
  regions without a lock.

The lesson for Vortex: parallel Prolog spent two decades proving that OR-parallel search over a
*shared* binding store is dominated by the binding-visibility problem, and that the implementations
that scaled either gave each worker its own binding array (SRI) or its own copied stacks (Muse).
Vortex's `choiceStack_` is the shared binding store, and the Muse answer — per-worker stacks, copy
on hand-off — maps directly onto per-worker `ArenaManifold` overlays plus `promote()`.

### 1.7 Summary table

| Runtime         | Heap model                         | Parallelism unit | Key mechanism                                       |
| --------------- | ---------------------------------- | ---------------- | --------------------------------------------------- |
| CPython (GIL)   | shared, refcounted                 | thread under GIL | one lock serializes the interpreter                 |
| CPython PEP 684 | per-interpreter, isolated          | interpreter      | per-interpreter GIL, no shared mutable objects      |
| CPython PEP 703 | shared, free-threaded              | thread           | biased refcount, immortal objects, per-object locks |
| Ruby Ractors    | per-Ractor; frozen objects shared  | Ractor (actor)   | copy or move across channel, frozen = shareable     |
| JVM / CLR       | one shared mutable heap            | OS thread        | memory model + concurrent/parallel GC               |
| Erlang / BEAM   | per-process heaps, share-nothing   | process          | message copy, per-process GC, N schedulers          |
| Go              | one shared heap                    | goroutine        | M:N work-stealing scheduler, concurrent GC          |
| Aurora/Muse     | per-worker binding arrays / stacks | OR-parallel task | SRI binding arrays or Muse incremental stack copy   |

## 2. Which model fits Vortex, given what the arena actually is

The decisive facts are structural, from `arena_manifold.hpp`:

1. **The arena mutates on nearly every opcode.** `link()`, `value()`, `splice()`, `newCell()`,
   `breakLink()`, `pin()`, `collectGarbage()`, and memo record/lookup all go through `VortexCore`,
   which holds exactly one `ArenaManifold &arena_` (`vortex_core.hpp`). This is the CPython-refcount
   situation: shared state written on the common path, so a shared-heap-plus-fine-locking approach
   pays lock traffic per opcode.
1. **Undo is global vector truncation.** A `Mark` is "the arena's high-water marks" — seven lengths
   — and `release()` truncates `slots_`, `links_`, `content_`, `scratch_`, `trail_` back to them
   (`arena_manifold.hpp`, `struct Mark`, `release()`). Truncation is only coherent if the thing
   being truncated has a single writer. Two OS threads minting cells into one `slots_` vector would
   interleave their dense indices, and a `release()` by one would delete the other's cells. **This
   alone rules out a shared arena with concurrent writers**, independent of any lock, because the
   semantics of the data structure assume a single append stream.
1. **Copy-on-write overlay over an immutable base already exists.** An
   `ArenaManifold(const Manifold *base)` reads fall through to the base and the first write shadows
   the cell (`shadow()`, `overlay_`), and "the base is never touched." This is precisely Ruby's
   shareable-frozen-object rule and BEAM's immutable-message rule, already implemented. The
   immutable base is the thing many workers can safely share; the arena on top is the thing each
   worker must own.
1. **The ephemeral bit already enforces the isolation boundary.** Arena refs carry `ephemeralBit`,
   and `Store::setLink()`/`Manifold::applyStructure()` refuse an ephemeral ref, so a leaked
   per-worker ref cannot silently corrupt a persistent store. `promote()` is "the only road across"
   and works by mapping refs. This is Ruby's move-semantics channel and BEAM's copy-on-send,
   pre-built.

Put together, Vortex is not a JVM-shaped problem (one mutable heap worth a concurrent GC) and it is
not well served by CPython-PEP-703-shaped fine-grained locking (the truncation semantics fight it).
It is a BEAM/Ractor/Muse-shaped problem wearing a Go-shaped scheduler. The arena is already a
per-owner scratch heap with an immutable shared base and a defined, ref-mapping promotion channel.
The model that fits is: **share-nothing green-cursor pools, each pool owning one `ArenaManifold`,
all pools sharing one immutable base `Manifold`, scheduled M:N across N OS threads, communicating by
`promote()` and by copying `CellValue`s.** This is Erlang's heap discipline, Ruby's shareable-base
rule, Muse's per-worker stacks, and Go's scheduler, each chosen because Vortex already has the
mechanism it depends on.

## 3. The GIL question — the concrete recommendation

The four options, judged against the arena facts above and the precedents:

### 3.1 Coarse global lock (the GIL)

One `std::mutex` around the VM; a worker holds it to run opcodes, drops it at a yield point. This is
the literal CPython GIL. It is correct, it is roughly a day of work, and it delivers *zero* parallel
throughput on arena-bound workloads — which is every Vortex workload, since the arena is touched on
nearly every opcode. The owner's fear is precisely this outcome. **Reject as an endpoint**; accept
only as a transitional safety net (see §3.3's note on a short-lived startup lock).

### 3.2 Fine-grained locking of the shared arena

Per-cell or per-rank locks over one shared `ArenaManifold`. This is the PEP 703 path. It is defeated
by fact (2): `mark()`/`release()` are whole-arena operations over dense vectors, so a backtracking
engine would have to lock the entire arena for every choice point, collapsing back to a GIL exactly
when logic programs need parallelism most. Even ignoring backtracking, `slots_`/`links_` are
`std::vector`s that reallocate on growth, so any cross-thread reference into them is invalidated by
another thread's `makeCell()`. Making this safe means replacing the dense-vector representation
wholesale and inventing a memory model. **Reject**: it is the most expensive option and it fights
the data structure's core invariant.

### 3.3 Per-arena / per-interpreter isolation — share-nothing green-cursor pools (recommended)

Give each OS worker its own `VortexCore` + `VortexVM` + `ArenaManifold`, all overlaying one shared,
immutable base `Manifold`. Cursors are partitioned across workers; a worker runs its own cursors
with its own `cursorPC_`, `choiceStack_`, and arena, with **no lock on the opcode path**. This is
PEP 684 (per-interpreter isolation) and BEAM (per-process heap) and Muse (per-worker stacks), all of
which Vortex's existing overlay + ephemeral-bit + `promote()` machinery were already building
toward.

Why it is the right call, by precedent and by mechanism:

- The overlay already makes the base safely shareable read-only (fact 3), so N workers reading a
  clause database or a document pay one copy per cell they *write* and nothing for reads — the exact
  property BEAM pays message-copy for and the arena gives for free on reads.
- Truncation-based undo stays correct because each arena has a single writer (fact 2 respected
  rather than fought).
- Results cross worker boundaries by `promote()` (ref-mapping, already the "only road across") or by
  copying `CellValue` (a `std::variant<std::string,double,int64_t,bool>`, trivially copyable across
  threads) — Ractor move/copy semantics with the channel already built.
- The ephemeral bit (fact 4) is a pre-existing guard that catches a worker ref escaping into a
  persistent store, so the isolation boundary is enforced by machinery that predates this design.

The transitional note: a single process-wide startup lock (or simply keeping genesis
single-threaded) is appropriate while system genesis and stdlib module minting run, because those
mutate `home` and the `d.stdlib`/`d.dims` ranks that every worker's base derives from. Hold it only
until the base is frozen, then drop it. This is PEP 684's "share the immutable, isolate the mutable"
with an explicit freeze point rather than a standing lock.

### 3.4 Lock-free

Lock-free dense vectors with hazard pointers or epoch reclamation. This is a research project, it
does not solve the truncation-undo problem (an epoch does not let you shrink a shared vector), and
it has no precedent among the surveyed runtimes for a backtracking heap. **Reject** for this study;
revisit only if a future shared read-only index (e.g. the Spanfilade) needs concurrent reads, where
a lock-free *read-only* structure is tractable and the undo problem does not arise.

### 3.5 Recommendation

Adopt **§3.3, per-arena share-nothing green-cursor pools on an M:N scheduler** (Go's scheduler
shape, BEAM/Ractor/PEP-684's heap discipline, Muse's per-worker stacks). This is the only option
that (a) delivers real parallel throughput on arena-bound workloads, (b) keeps truncation-undo
correct, and (c) reuses the overlay, ephemeral bit, and `promote()` that already exist. The GIL is a
transitional safety net around genesis only; fine-grained and lock-free are rejected for fighting
the arena's core invariant.

## 4. Consequences for the opcode lifecycle, the per-cursor PC, and `choiceStack_`

### 4.1 The per-cursor PC — already per-cursor, must become per-worker-owned

`cursorPC_` is `std::unordered_map<CellRef, CellRef>` on `VortexVM` and is logically already
per-cursor (`getCursorOpcode`/`setCursorOpcode`, `vortex_vm.cpp`). The only change the model forces
is *ownership*: in a per-arena design each worker owns a `VortexVM` and therefore its own
`cursorPC_`, so the map is partitioned by worker and never shared. No two workers index the same
map, so it needs no lock — the JVM lesson that execution state (stack + PC) is per-thread and never
shared applies directly, and Vortex is already most of the way there. The `setCursorOpcode`
write-through to `arena().setValueBits()` stays correct because the cursor cell lives in that
worker's own arena.

Note the existing dual encoding: `getCursorOpcode` falls back to `arena().asInt64(cursor)` when the
map misses. In a per-worker design this fallback is fine (the cursor cell is in the worker's arena),
but a cursor cannot be *migrated* to another worker by moving only the map entry — its backing cell
and PC-bits live in the source arena. Cursor migration (work stealing's requirement) must go through
the same `promote()`/copy channel as any other cross-arena transfer, or be forbidden (static
partitioning). Recommend static partitioning first, Muse-style incremental copy for migration later
if load imbalance demands it.

### 4.2 The global `choiceStack_` — must become per-cursor (or per-worker)

This is the load-bearing change and the one the WAM precedent predicts. Today:

- `choiceStack_` is a single `std::vector<ChoicePoint>` on `VortexVM`.
- `pushChoicePoint` captures `core_.arena().mark()` — a *global* arena checkpoint — into the shared
  stack (`vortex_vm.cpp`).
- `backtrack(cursor)` pops the stack and calls `core_.arena().release(cp.mark)` — a *global* arena
  rollback.
- `cut(cutBarrier)` truncates the shared stack and `discard()`s marks.
- `ChoicePoint::cutBarrier` is literally `choiceStack_.size()` at push time — an index into the
  global stack.

Every one of these is global, and every one is exactly what Aurora/Muse had to de-globalize. A
single shared `choiceStack_` with a single shared arena mark is the multiple-binding problem in C++:
two cursors backtracking concurrently would `release()` each other's marks and truncate each other's
cells. Concurrent backtracking "is not expressible today," as the briefing states, and the reason is
that both the stack and the mark it holds are global.

The fix the model forces, by Muse precedent:

- **`choiceStack_` becomes per-cursor state** (or per-worker, since a worker runs its cursors
  cooperatively). Each cursor's choice points reference marks in *that worker's* arena, so a
  `release()` truncates only that arena. This is Muse's "each worker owns its stacks."
- **`ChoicePoint::mark` stays a `zigzag::Mark`** but now names a point in the owning worker's arena,
  which is sound because that arena has one writer.
- **`cutBarrier` becomes an index into the per-cursor stack**, not the global one — a mechanical
  change once the stack is per-cursor.
- **OR-parallelism across workers** (exploring alternatives of one choice point in parallel)
  requires the SRI/Muse step: the alternative branches run in *separate* arenas (overlays of the
  same base), each with its own trail, and the winning branch's bindings are promoted; the losers
  are discarded by `release()` in their own arenas. This is why the overlay-over-immutable-base is
  the enabling mechanism — it is the binding-array-per-worker of the SRI model, spelled as
  copy-on-write.

### 4.3 The opcode lifecycle

The 10-step lifecycle itself (parameter binding along the wings, snapshot-before-write, contract
checks along `d.contract`, memoization along `d.cache`) does not change shape per worker, because it
only ever touches the worker's own `core_`/`arena_`. Three cross-cutting concerns need rules:

- **`spawnCursor` and the `+d.cursors` rank.** Today `spawnCursor` links the new cursor onto home's
  `+d.cursors` rank in the shared arena (`vortex_vm.cpp:127`). Under per-arena isolation, each
  worker has its own home-derived cursor rank; a cross-worker spawn (e.g. VPL fanning out a parallel
  `map`) must be an explicit hand-off to a target worker's queue, not a link into a shared rank. The
  `spawnThread` boolean in `compileAndAttachVQL` (`vortex_host.cpp:513`) — which today only means
  "mint a green cursor" — is the natural place to let a caller request "on its own worker."
- **Memoization under contention.** `getOrCreateMemoPin`/`recordMemo`/`lookupMemo` and
  `memoizedOps_` write the arena's `d.cache` rank. Per-worker arenas mean per-worker memo caches by
  default — correct but with duplicated work across workers (the BEAM tradeoff: no shared mutable
  cache). A *shared* memo cache would be a shared mutable structure and reintroduce the GIL problem;
  if cross-worker memo sharing is wanted, it should be a separate read-mostly structure populated by
  promotion at well-defined points, not the live arena rank. Recommend per-worker memo first.
- **The system GC cursor** (`vortex_host.cpp:86`, `"sys:gc"`) and `collectGarbage()` are a mark over
  one arena; per-worker arenas mean per-worker GC, which is the BEAM property (a GC pause stops one
  worker, not the world). No global GC pause is introduced, which is a direct win of the chosen
  model.

### 4.4 Net structural changes (interpreter-facing)

| Element                   | Today                            | After (per-arena isolation)                             |
| ------------------------- | -------------------------------- | ------------------------------------------------------- |
| `ArenaManifold &arena_`   | one, shared by all cursors       | one per worker, all overlaying a shared frozen base     |
| `cursorPC_`               | one map, logically per-cursor    | one map per worker, partitioned, lock-free              |
| `choiceStack_`            | one global `vector<ChoicePoint>` | per-cursor (or per-worker) stack                        |
| `ChoicePoint::mark`       | global arena checkpoint          | checkpoint in the owning worker's arena                 |
| `ChoicePoint::cutBarrier` | index into global stack          | index into per-cursor stack                             |
| cross-cursor results      | implicit via shared arena        | explicit `promote()` or `CellValue` copy (Ractor-style) |
| memo cache (`d.cache`)    | shared arena rank                | per-worker; shared only via explicit promotion          |
| GC / `collectGarbage()`   | single global mark               | per-worker, no stop-the-world                           |

These are the minimum changes the recommended model implies for the VM core. How each frontend (VQL
parallel scans/joins, VPL data-parallel `map`/`filter`/`fold`, vprolog AND/OR search) *exposes* true
thread primitives on top of this substrate is developed in the frontend sections; the common
substrate they all rely on is "one arena per worker, one immutable base shared, choice state
localized."

## 5. Open questions this section hands to the synthesis

1. **Static partition vs. work stealing.** Static cursor partitioning is simplest and avoids cursor
   migration entirely; Go-style work stealing needs Muse-style incremental cursor/stack copy across
   arenas. Recommend starting static, measuring imbalance, adding stealing only if needed.
1. **Base-freeze protocol.** The exact point at which the shared base `Manifold` is frozen after
   genesis and stdlib minting, and whether later stdlib additions force a controlled re-freeze, is a
   lifecycle question the host section must pin down.
1. **Cross-worker OR-parallel promotion ordering.** When parallel branches each promote an answer,
   the determinism rules (which VQL in particular needs) depend on an ordering discipline over
   `promote()` that the VQL section must specify.
