# True multithreading in the Vortex VM: a design study

This study evaluates introducing true (OS/hardware) multithreading into the Vortex virtual machine
and its downstream effect on the three frontends (VQL, VPL, vprolog) and the Vortex standard
library, and specifies how each frontend would expose real thread primitives to its language. It is
grounded in the shared briefing
([`vortex-multithreading-briefing.md`](vortex-multithreading-briefing.md)) and in the code under
`apps/common/xanadu/vortex/`, `vql/`, `vpl/`, `vprolog/`, and `zigzag/arena_manifold.*`. The driving
concern is the owner's: Vortex cursors are green threads today, scheduled cooperatively, and every
one of them touches a single shared `ArenaManifold` on nearly every opcode — the exact shape that
forced CPython to keep a Global Interpreter Lock for thirty years. The finding of this study is that
the fear is correct about the bottleneck but wrong about the remedy: the arena is not a lock to be
made faster, it is a node boundary. The arena's existing copy-on-write overlay over an immutable
base, its `ephemeralBit` isolation guard, and its single `promote()` persistence road already
compose into a share-nothing, snapshot-isolated, content-addressed substrate. Giving each OS worker
its own arena over one shared immutable base, scheduling green cursors M:N across those workers, and
merging answers at the single existing writer removes the GIL for exactly the read-heavy parallel
workloads where it would otherwise bite.

## Executive summary

**Bottom line on the GIL concern: do not build a GIL, and do not try to make a shared arena
thread-safe with locks. Adopt per-worker share-nothing arenas over one immutable base `Manifold`,
with an M:N scheduler and a single serialized `promote()` merge.** The owner's instinct is right
that wrapping the one `ArenaManifold` in a mutex reproduces the Python GIL — it serializes nearly
every opcode, because nearly every opcode mutates that arena. But the way out is not a cleverer
lock. It is to stop sharing the mutable arena at all.

The decision rests on four properties that already exist in the code, not on new infrastructure:

- The arena's **copy-on-write overlay** lets any number of workers read one immutable base
  `Manifold` with no lock, each shadowing privately on first write. Read-sharing is free;
  write-sharing is impossible by construction.
- The arena's **undo model is whole-arena vector truncation** (`mark()`/`release()`), which is only
  coherent with a single writer. This positively *forbids* a shared arena with concurrent writers,
  independent of any lock, and so dictates the per-worker design rather than merely suggesting it.
- The **`ephemeralBit`** on every arena ref is refused by `Store::setLink()` and
  `Manifold::applyStructure()`, so a leaked per-worker ref cannot silently corrupt a persistent
  store — the isolation boundary is enforced by pre-existing R8 machinery.
- **`promote()` is the single, ref-mapping road to persistence**, so the cross-thread merge has
  exactly one chokepoint to serialize, and it already refuses untranslated refs.

The migration is staged so that each step ships green under `make check` and `make test`, orders by
payoff and safety (VPL data-parallel array operations first, vprolog OR-parallel search last and
opt-in), and never touches the fixed on-disk invariants (`sizeof(CompactOpNode) == 64`, 64 KiB
Merkle pieces, append-only). The hardest obstacle — the single global `choiceStack_` and the global
arena mark/release that make concurrent backtracking inexpressible today — is resolved for free by
the same move that removes the GIL: once each worker owns its arena and VM, its choice stack and
marks are per-worker and serial within that worker. True thread safety is verified under
ThreadSanitizer and lifetime bugs under AddressSanitizer — using a new Make target that instruments
the engine objects and the gtest binaries, because the stock `sanitize/thread`/`sanitize/address`
targets build only the plain `gleditor` editor and touch no Vortex code — with looping multi-thread
stress runs, because these races are schedule-dependent and a one-shot test proves nothing.

## 1. The current green-thread model and the bottleneck

Vortex today has only green threads. A cursor is not an OS thread: `spawnCursor()`
(`vortex/vortex_vm.cpp:127`) mints a cell, records its program counter, and links the cell onto the
tail of `home`'s `+d.cursors` rank (the `link(...)` call at `vortex/vortex_vm.cpp:137-139`).
`stepScheduler()` (`vortex/vortex_vm.cpp:1076`) reads `activeCursors()` off that rank
(`vortex/vortex_vm.cpp:1078`) and calls `step(cursor)` on each in a plain serial `for` loop
(`vortex/vortex_vm.cpp:1079-1087`) on the calling thread. There is no preemption, no OS thread, and
no lock — and no lock is needed, because everything runs on one thread.

The single sentence that defines the problem: **every opcode of every cursor mutates one
`ArenaManifold`, held by reference in exactly one place, with no lock anywhere in the VM, the core,
the stdlib, or the host.** `VortexCore` holds exactly one `ArenaManifold &arena_`
(`vortex/vortex_core.hpp:256`), and the host threads that one arena through every component in its
member init list (`vortex/vortex_host.cpp:77-78`, the constructor opening at `:76`), so `core_`,
`vm_`, `stdlib_`, and the VQL and VPL engines/compilers all share it. (vprolog has no separate
engine member in that list; it rides the VM's logic opcodes directly, per §6.4.) Every structural
primitive funnels into it: `link()` (`vortex/vortex_core.cpp:124`), `value()` (`:165`), `newCell()`
(`:220`), `breakLink()` (`:235`), `pin()` (`:454`), `collectGarbage()` (`:509`), and memo
record/lookup. The arena's own class comment is blunt (`zigzag/arena_manifold.hpp:15`): "dense
vectors only. No ops. Dies with its owner." It is single-owner scratch space, not thread-safe, by
design.

### 1.1 The shared mutable state, enumerated

The arena's mutable backing — a data race if two threads write any of it — is
`slots_`/`links_`/`content_`/`scratch_`/`trail_` (`zigzag/arena_manifold.hpp:661-666`) plus the
overlay/federation bookkeeping `overlay_`/`shadowOrder_`/`floors_`
(`zigzag/arena_manifold.hpp:675-696`). The mutators that two cursors would contend on run from
`makeCell()` (`zigzag/arena_manifold.hpp:518`) through `compact()` (`:624`). Crucially, even the
read path is not pure under copy-on-write: `linkedMinting()` (`zigzag/arena_manifold.hpp:460`) and
the private `shadow()` *write* on first access to a base cell, so a "reader" cursor mutates
`overlay_`, `shadowOrder_`, `slots_`, `links_`, and `content_` as a side effect of reading through
the base. A read is a write.

`VortexVM` adds four mutable members, none per-cursor-isolated and none synchronized
(`vortex/vortex_vm.hpp:148-151`):

```text
std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
std::unordered_map<CellRef, std::string> memoizedOps_;
std::unordered_map<CellRef, CellRef>     cursorPC_;
std::vector<ChoicePoint>                 choiceStack_;
```

`opcodeMap_` and `memoizedOps_` are effectively write-once-at-assembly, read-many-at-run.
`cursorPC_` is the per-cursor program counter — but it is one map keyed by cursor, not a per-cursor
object, written on every step (`vortex/vortex_vm.cpp:120`). `choiceStack_` is a **single global**
vector of `ChoicePoint`, not per-cursor, and is the hardest structure in the system for any threaded
design (§5).

The runnable set is itself shared arena state: `spawnCursor` links a cursor onto the `+d.cursors`
rank (an arena write) and `activeCursors()` reads it back (an arena read). Memoization lives
entirely in the arena on `d.cache` ranks: `getOrCreateMemoPin` (`vortex/vortex_core.cpp:515`),
`lookupMemo` (`vortex/vortex_core.cpp:524`), `recordMemo` (`vortex/vortex_core.cpp:537`). A system
GC cursor is spawned at host init (`vortex/vortex_host.cpp:86`, `"sys:gc"`); its sweep reaches
`core_.collectGarbage()` → `arena_.compact()`, which **moves every run** and is **refused while a
mark is outstanding** (`zigzag/arena_manifold.hpp:613-624`).

### 1.2 Why this is a GIL and not merely a hotspot

The decisive fact is the undo model. A `Mark` is a vector of high-water lengths into the arena's
dense vectors — twelve `std::uint32_t` fields: `cellCount`, `linkSize`, `contentSize`,
`scratchSize`, `trailSize`, `liveLinks`, `liveContent`, `shadowCount`, `proxyCount`,
`quoteOccurrenceCount`, `proxyShadowedEdgeCount`, `quoteSpaceCount`
(`zigzag/arena_manifold.hpp:169-184`). `release()` truncates those vectors back to the recorded
lengths. Truncation is coherent only with a single append stream: thread B's `makeCell` grows the
vectors that thread A's mark named, so A's `release` would delete B's cells. This is why the single
shared arena is a GIL rather than a tunable contention point — the data model itself assumes one
writer with a nested mark stack (`floors_`). A lock big enough to make this correct is a lock around
the whole VM, which is the GIL exactly.

### 1.3 The naming trap

The word "thread" already exists in the API and already means "green cursor." `compileAndAttachVQL`
takes `bool spawnThread` (`vortex/vortex_host.cpp:513`); `spawnThread == true` does nothing but call
`vm_.spawnCursor(...)` (`vortex/vortex_host.cpp:526-527`). The cursor rank is `d.cursors` and the
`SystemDimensions::name` comment calls `d.name` the "Thread, pin, and store naming rank"
(`vortex/vortex_core.hpp:48`). A real-threads design must reckon with this: shipping an OS-thread
primitive called `spawnThread` beside a green-thread flag of the same name is a trap for every
future reader. This study keeps "cursor/green thread" as the language-visible unit and names the new
OS-level unit a *worker*, reserving "thread" at the C++ layer for the OS worker only (§6 details the
per-language surface).

## 2. Prior art and model selection

Vortex's `ArenaManifold &arena_` is a shared, non-thread-safe, mutate-on-nearly-every-opcode heap
with global checkpoint/rollback — structurally the object that forced CPython to keep its GIL. The
useful question is which escape route, taken by a runtime that faced the same shape, fits an arena
whose undo is vector truncation.

### 2.1 Survey

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

The lessons that transfer:

- **CPython.** The GIL exists because refcounts are written on essentially every operation, so even
  read-mostly code writes shared memory — Vortex's arena-on-every-opcode situation. Making refcounts
  atomic cost ~2x single-thread throughput (the "Gilectomy"). PEP 684 (per-interpreter GIL) fixed it
  by isolating the mutable and sharing the immutable; PEP 703 (free-threading) removed the GIL with
  biased refcounting, object immortalization, and per-object locks — years of fine-grained work with
  a standing single-thread penalty. The lesson: fine-grained locking of a mutate-everywhere heap is
  the most expensive option per engineer-month; per-interpreter isolation is the cheaper structural
  fix and maps onto an arena.
- **Ruby Ractors / Erlang BEAM.** Share-nothing with an immutable/frozen shared tier, communicating
  by copy or ownership-move. BEAM runs N schedulers on N cores with per-process heaps and
  per-process GC, scaling near-linearly on parallel workloads at the cost of message copying.
  Vortex's cursor is already a cheap, independently scheduled unit; the only thing stopping it from
  being a BEAM process is that all cursors share one arena instead of each owning a heap.
- **JVM/CLR.** The shared-heap counterexample is viable but is a decade-scale memory-model-and-GC
  investment, and it pushes correctness onto the programmer. Vortex has no memory model and a
  single-threaded mark GC; this path means building both from scratch.
- **Go.** Shows the *scheduler* shape to copy — M:N, per-worker run queues, work stealing —
  independently of the heap decision. Vortex can adopt Go's scheduler while rejecting its shared
  heap.
- **WAM parallel Prolog (Aurora/Muse).** The most directly relevant precedent, because vprolog is a
  WAM-shaped engine over `choiceStack_` and the arena trail. OR-parallelism hits the
  multiple-binding problem; Aurora solved it with per-worker binding arrays (SRI model) and Muse
  with per-worker copied stacks. Both concluded the trail and choice stack cannot stay global and
  shared — which is precisely Vortex's `choiceStack_`.

### 2.2 Model selection

Four facts from `arena_manifold.hpp` decide it: the arena mutates on nearly every opcode
(CPython-refcount shape); undo is global vector truncation, which is coherent only with a single
writer; a copy-on-write overlay over an immutable base already exists (Ruby's frozen-shareable /
BEAM's immutable message, pre-built); and the `ephemeralBit` plus `promote()` already enforce the
isolation boundary and the ref-mapping channel across it. Vortex is therefore **not** a JVM-shaped
shared-heap problem and is **not** well served by PEP-703-shaped fine-grained locking (the
truncation semantics fight it). It is a BEAM/Ractor/Muse-shaped problem wearing a Go-shaped
scheduler. The model that fits: **share-nothing green-cursor pools, each pool owning one
`ArenaManifold`, all pools sharing one immutable base `Manifold`, scheduled M:N across N OS workers,
communicating by `promote()` and by copying `CellValue`s.** Each piece is chosen because Vortex
already has the mechanism it depends on.

The GIL question resolves accordingly: a coarse global lock is rejected as an endpoint (zero
parallel throughput on arena-bound workloads — the owner's exact fear) and kept only as a
transitional genesis-only safety net; fine-grained arena locking is rejected (defeated by
whole-arena mark/release and by dense-vector reallocation invalidating cross-thread refs); lock-free
is rejected as a research project that does not solve truncation-undo. Per-arena isolation is
adopted.

## 3. Proposed architecture and staged migration plan

### 3.1 The threading model

Each OS worker owns its own `ArenaManifold`, `VortexCore`, and `VortexVM` (and therefore its own
`choiceStack_`, `cursorPC_`, `opcodeMap_`). Workers never touch another worker's arena. They
coordinate by explicit message passing — a submit/complete handoff of immutable request and result
values — not through shared mutable manifold state. This fits three properties of the current
design:

- The copy-on-write overlay makes read-sharing free and write-sharing impossible: N worker arenas
  share one base `Manifold` as a read-only database, each shadowing privately, and the base is never
  mutated.
- `ephemeralBit` prevents a leaked ref from crossing a boundary silently; messages carry values or
  `promote()`d persistent refs, never live ephemeral refs.
- `promote()` is the single ref-mapping road to persistence, so the merge step has one chokepoint to
  make safe.

The shared thing becomes the *base* `Manifold` (immutable, read-only, needs no lock) plus a tiny
message channel. The mutable thing — arena, choice stack, trail — becomes per-worker, touched by
exactly one thread for its whole life. The GIL disappears because there is no shared mutable state
on the opcode hot path, not because a lock around it got faster.

### 3.2 The M:N scheduler

M green cursors are scheduled onto N OS workers. The worker pool is the engine's own, modeled on the
proven `render::WorkerPool` (`include/gleditor/render/worker_pool.hpp:36`): threads started once and
parked between batches, the caller taking a share rather than blocking, the first worker exception
rethrown after all finish, the closure borrowed as a `function_ref`. The Vortex worker pool is a
*sibling* in `apps/common/xanadu/vortex/`, not a modification of the render pool, whose blocking
indexed-batch contract is load-bearing for frame recording. A cursor is pinned to a worker at spawn
and stays there, because migrating a running cursor would mean moving arena state across threads —
the thing the model exists to avoid. `stepScheduler()` becomes: partition `activeCursors()` by
owning worker, hand each worker its sublist, join; the inner per-worker loop is the unchanged serial
`for`. Worker count is configuration, read from `system://settings` as `vortex.worker_threads`
(unset or 0 ⇒ probe `std::thread::hardware_concurrency()`; 1 ⇒ caller-only, no threads started),
joining the existing `vortex.cycle_budget` and `vortex.gc_interval_frames` knobs that
`VortexHostConfig::fromStore` already reads.

### 3.3 Arena lifecycle across threads

The host owns a vector of worker contexts, each
`{ArenaManifold, VortexCore, VortexVM, VortexStdLib}`, each arena constructed over the **same shared
base `Manifold*`**. The base is the read-only document; the overlay makes each worker's writes
private. The base must outlive every worker arena, and rebinding it (a new microversion) is a
barrier: quiesce all workers, swap the base, rebuild worker arenas — `bindManifold()` generalized to
N arenas with a join in the middle. Promotion is the only cross-thread write, and it is serialized:
a worker that finishes an answer ships the self-contained reachable subgraph back to the host, which
promotes on a single designated thread, preserving append-only ordering and the single-writer
invariant. Nothing about `CompactOpNode == 64`, 64 KiB Merkle pieces, or append-only changes — only
*which thread* calls the existing writer, and only one does. Ephemeral refs never leave their
worker; a result is a `CellValue`, a scroll-addressed span, or a promote request, and a leaked
`ephemeralBit` ref is refused by the existing R8 machinery.

### 3.4 Backtracking becomes per-worker for free

Once each worker owns its arena and VM, its `choiceStack_` holds only its own cursors' choice
points, each `ChoicePoint::mark` names *that worker's* arena, and `release()` truncates only that
worker's vectors — the serial semantics that exist today, confined to one thread.
`cut()`/`cutBarrier` keep their meaning as an index into the owning worker's stack. No new
synchronization is needed for backtracking; it becomes correct by the same move that removes the
GIL. OR-parallelism (alternative clauses on different workers) is expressible but explicitly
deferred to a late stage.

### 3.5 Deterministic merge-back

Determinism is enforced at the merge boundary, not inside the workers. Workers return results tagged
with a stable submission index (chunk position, scan partition rank order); the host concatenates in
index order regardless of completion order, the same discipline `render::WorkerPool` uses. Promotion
happens in submission order on one thread, yielding a byte-reproducible store testable with
`xudu-dump --section=ops STORE` before/after. Parallel `fold` computes per-chunk partials in
parallel but combines them left-to-right in chunk-index order, so a non-associative combiner matches
the serial fold. Memoization is per-worker (no contention, no cross-worker sharing); a shared
read-only memo table promoted into the base between barriers is a later optimization, not a stage-1
feature.

### 3.6 Staged migration

Each stage is independently shippable and leaves the tree green under `make -j$(nproc) check` and
`make -j$(nproc) test`.

- **Stage 0 — Instrument and prove the bottleneck.** Add `SPDLOG_LEVEL`-gated timing under a
  `vortex.sched` category and a headless benchmark driving many cursors over one arena. Gate: a
  committed measurement showing the single shared arena is the serialization point. Note a build
  caveat that shapes every later gate: the existing `sanitize/address` and `sanitize/thread` targets
  depend only on the `gleditor` program (`sanitize/address: gleditor`, `Makefile:848`;
  `sanitize/thread: gleditor`, `Makefile:855`; the `/run` variants execute `$(OBJDIR)/gleditor`),
  and `gleditor` links only the plain-editor objects and the library (`Makefile:823`) — a binary
  that by AGENTS.md design shares no Xanadu or Vortex code. So the stock targets instrument *zero*
  lines of `vortex_vm`, `arena_manifold`, or any frontend. Stage 0 therefore adds a new, no-CMake
  Make target that applies the `SANITIZE_ADDR_OPTS` flag macro (`Makefile:436`, defined under
  `ifdef DEBUG`) to the engine objects and the `xudu_test`/`zigzag_test` binaries, and runs the
  engine test suite under it single-threaded to shake out latent lifetime bugs.
- **Stage 1 — Make the VM movable.** Refactor so per-cursor VM state is addressable per cursor and
  the host can hold more than one worker context. Still single-threaded, one context on the caller.
  Gate: `make test` unchanged; `xudu-dump --section=ops` byte-identical on every sample (fixtures
  not regenerated, because nothing in `CompactOpNode` or the spool changed).
- **Stage 2 — Vortex worker pool, data-parallel VPL first.** Add the worker pool and wire VPL's
  `map`/`filter`/`fold` only to fan disjoint index ranges across workers. VPL is first because it is
  the natural data-parallel fit with no cross-element arena writes. Gate: scaling with worker count
  on a multi-core box; results bit-identical to serial across 1..N workers; `fold` combine order
  fixed and tested with a non-commutative combiner.
- **Stage 3 — Parallel independent cursors.** Place independent cursors on different workers, each
  with its own arena/VM/choice stack. Gate: a looping stress test spawning many independent cursors
  is clean under a new engine-and-test ThreadSanitizer target (the `SANITIZE_THR_OPTS` flag macro,
  `Makefile:438`, applied to the engine objects and `xudu_test`/`zigzag_test`, with
  `TSAN_OPTIONS=second_deadlock_stack=1` — not the stock `sanitize/thread` target, which builds and
  runs only `gleditor`); determinism holds; promotion single-threaded and `ops.nodes` reproducible.
- **Stage 4 — Parallel VQL.** Partition scans/joins over the (multi-store) base, run partitions on
  private overlays, merge in index order. Gate: identical result sets and ordering to the serial
  compiler across worker counts; `vquery`/`vqueryc` headless runs match; no persistent cross-store
  link silently created.
- **Stage 5 — vprolog OR/AND-parallel search, opt-in.** Only after 1–4: fork a worker overlay at a
  choice point to explore an alternative clause (OR-parallelism), merging solutions
  deterministically by submission index (lowest-index branch wins "first solution"), cut confined to
  its branch. Gated behind a `system://settings` flag, default off. Gate: same solution set and
  first-solution as the serial engine for a suite including cut and negation; no arena mark/release
  crosses a worker boundary (verified under the engine-and-test TSan target from Stage 3 and
  `zigzag.arena` leak logging).

Cross-cutting gates every stage: `make -j$(nproc) format-check lint analyze` clean; `make test`
green; every tunable a `defaultSettingSpecs()` entry read via `getSetting` (no magic numbers); no
change to `CompactOpNode`, the spool writer, Merkle piece size, or append-only ordering; determinism
is a test (`xudu-dump --section=ops` reproducibility and serial-vs-parallel equality), not a claim.

### 3.7 Component diagram

```text
                         VortexHost  (owns the shared, read-only base)
                         ----------------------------------------------
                              base Manifold*  (ops-backed, immutable
                              for a microversion; shared by all workers,
                              needs no lock)
                                      |
                        +-------------+--------------------------------+
                        |  config from system://settings:              |
                        |  vortex.worker_threads / cycle_budget / ...   |
                        +-----------------------+----------------------+
                                                |
                       Vortex worker pool (sibling of render::WorkerPool;
                       threads parked between batches; caller takes a share;
                       pool of 1 => caller-only, no threads)
                                                |
        -----------------------------------------------------------------------
        |                        |                        |                    |
   Worker 0 (OS thread)     Worker 1 (OS thread)    Worker N-1 ...        (caller
   -------------------      -------------------      ---------------       takes a
   ArenaManifold  (CoW      ArenaManifold  (CoW      ArenaManifold          share)
     overlay on base;         overlay on base;         overlay on base;
     private shadows)         private shadows)         private shadows)
   VortexCore (home_,       VortexCore               VortexCore
     dims_, this arena)
   VortexVM                 VortexVM                 VortexVM
     cursorPC_  (own)         cursorPC_  (own)         cursorPC_  (own)
     choiceStack_ (own) <--   choiceStack_ (own)       choiceStack_ (own)
     mark/release/cut           per-worker; no shared mutable arena,
     over THIS arena only       no cross-thread backtracking
   +d.cursors rank (own)
     green cursors here
        |                        |                        |
        | results tagged with submission index            |
        | (CellValue / scroll-addressed span / promote request;
        |  NEVER a live ephemeralBit CellRef)
        v                        v                        v
   -----------------------------------------------------------------------
   Deterministic merge  (host): order by submission index; fixed fold
   combine order; single promoting thread
                                      |
                              promote()  (the one ref-mapping road to
                              persistence; ephemeralBit refs refused by
                              Store::setLink / applyStructure if leaked)
                                      |
                              Store::save -> SegmentedOpsSpool::
                              writeSegmentFile  (single writer; ops.nodes
                              append-only; CompactOpNode==64; 64 KiB
                              Merkle pieces)  -- UNCHANGED
```

## 4. Distributed-systems framing, determinism, and merge

The architecture above is, viewed from a distance, a well-behaved distributed system, and the
distributed lens sharpens the determinism and failure story.

### 4.1 Arenas as share-nothing nodes

A **node** is one `ArenaManifold` plus the `VortexCore`/`VortexVM` bound to it; it owns its dense
vectors and its choice-point `floors_` and nothing anyone else can see. The **shared immutable
substrate** is the base `Manifold` and the `Store`'s `ops.nodes`; reads of an unshadowed cell fall
through to the base, which is never touched, so many nodes read one base concurrently with no
coordination. A **message** is an immutable, content-addressed value: a `CellRef` into the base, a
`PrimediaSpan`, a `CellValue`, or the `Promoted{version, cells}` that `promote()` returns. The
classic models map cleanly onto the frontends: fork-join/MapReduce onto VPL and the stdlib
`map`/`filter`/`fold`; dataflow onto VQL pipelines; actors/CSP onto the cursor scheduler and `call`;
AND/OR parallelism onto vprolog (the hardest, because it collides with the global `choiceStack_`).

### 4.2 Immutability is the determinism asset

The op log is append-only and immutable (`CompactOpNode` 64 bytes, sealed segments `PROT_READ`,
content addressed by span). Composed with the overlay contract that the base "is never touched,"
this gives snapshot isolation for free: each node evaluates against a fixed immutable snapshot keyed
by a `MicroversionId`. Content addressing strengthens it — two workers that independently construct
the same content converge on the same `PrimediaSpan` address, so equality and transclusion discovery
are stable across workers with no coordination. Determinism of a read-only parallel query then
reduces to one rule: fix the merge order by a deterministic key (rank position, `CellRef`, or
`GlobalOpRef`), not by completion order.

`promote(store, parent, from, root, budget)` (`zigzag/arena_manifold.hpp:759`) is exactly a
distributed commit: it is a reachability walk from the answer root ("promote the answer, not the
search"), it maps refs rather than trusting them (an untranslated ref is refused, the validation
half of a commit), and it takes an explicit `parent` `MicroversionId` (the version-vector edge).
When many workers promote, promotions linearize against the single-writer commit stage: workers
produce content-addressed write sets in parallel (the expensive part), and one thread assigns the
microversion and writes `ops.nodes` (cheap, bounded by `budget.maxOps`) — a clean prepare/commit
split. Because editing an earlier state *branches* rather than overwrites, two workers promoting
from the same parent produce two children, a merge-free CRDT-like "keep both" posture; the
application-level question "which branch is current" is a deliberate decision, not a race.

One determinism caveat the distributed and interpreter experts both raise, and which this study
adopts: with OR/AND parallelism the *set* of vprolog solutions is order-independent but the *order*
reported is not, unless imposed. A parallel search must either sort by a deterministic solution key
before returning or document the order as nondeterministic. The staged plan sorts by submission
index.

### 4.3 Partitioning, failure, and cancellation

Queries partition two ways that compose: **by store** (federation — one node per store,
fan-out/fan-in, join on `GlobalOpRef` via `bindSharedIdentities()`, `zigzag/arena_manifold.hpp:377`)
and **by rank** (range or hash partition over the doubly-linked rank, merged by `chainCells`
preserving order). The CSR layout means a reading worker touches only the immutable base, so there
is no false sharing on the hot structural vectors; the moment a worker writes, it writes into its
own arena. Memoization does not partition cleanly and starts per-node. Failure and cancellation are
clean because a worker owns its arena: local undo is `mark()`/`release()` truncation (O(trail), not
O(cells)); cancellation is dropping the node (the arena dies with its owner, having written nothing
shared); the `ephemeralBit` guard fires if a ref from a cancelled worker reaches a persistent write;
and budgets (`run` `maxCycles`, `PromotionBudget` `maxOps`, `allocationLimit_`) bound each worker
and compose. OR-parallelism comes before AND-parallelism because AND-parallel conjuncts that share
bindings reintroduce a shared mutable trail unless the conjuncts are provably independent.

### 4.4 Where it is overkill

The distributed machinery earns its complexity where the shared arena is the bottleneck and the work
is read-heavy against the immutable base: VPL data-parallel `map`/`filter`/`fold`, VQL parallel
scans and join probe sides, and multi-store queries. It is premature for a concurrent shared memo
cache (start per-node, measure reuse loss first), AND-parallel vprolog with shared bindings,
parallelising short interactive cursors (the GIL fear is batch throughput, not keystroke latency —
keep the cooperative scheduler for the interactive path), and parallelising the commit (the win is
parallel *evaluation* feeding a serial *commit*).

## 5. C++ concurrency hazards and mitigations

This is the engineering ground truth, and it is deliberately pessimistic. **Nothing in `VortexCore`,
`VortexVM`, or `ArenaManifold` is thread-safe, and several structures are actively hostile to
concurrent access because a write by one thread invalidates pointers, iterators, and indices another
thread is mid-read on.** Worse than CPython's GIL in one respect: there is no lock at all, so the
naive "run cursors on threads" change is not slow, it is undefined behaviour.

### 5.1 Concrete hazards

- **Unsynchronized `unordered_map`s** (`vortex/vortex_vm.hpp:148-151`). `cursorPC_` is written on
  every step (`vortex/vortex_vm.cpp:120`). Concurrent `operator[]`/`find` is a data race: a rehash
  reallocates the bucket array and relinks nodes under a concurrent reader (use-after-free, not a
  stale read); iterators do not survive rehash; and `memoizedOps_`'s `std::string` value can have
  its buffer reallocated mid-copy. `opcodeMap_` is read-heavy after assembly, but assembly and
  execution are not phased — VQL/VPL compilers mint opcodes and `spawnCursor` in the same path, so
  threading during compilation races it.
- **`choiceStack_` reallocation and semantic entanglement** (`vortex/vortex_vm.hpp:151`).
  `push_back` past capacity (`vortex/vortex_vm.cpp:169`) dangles a concurrent `.back()` (`:180`).
  Worse than the race: `cutBarrier` is `choiceStack_.size()` at push (`:174`), an index into the
  shared stack, so cursor A's cut barrier points into cursor B's frames. Even with a perfect lock
  the *meaning* is wrong — a global choice stack cannot express two cursors backtracking
  independently. This is the deepest obstacle to OR-parallel vprolog.
- **`CellRef` index stability vs. dense-vector reallocation.** `CellRef` is a bare `std::uint32_t`
  (`zigzag/dim_vector.hpp:21`), so the ref value is stable across threads — but everything it points
  into is a dense `std::vector` that reallocates on `push_back`. A `CellSlot*`, `SlotRef`, or
  `std::span` resolved before another thread's `makeCell`/`link`/`intern` dangles. The code says so:
  `SlotRef` is "valid until the next fold or write grows it" (`zigzag/manifold.hpp:312`). The
  single-owner contract that makes this safe is exactly what threading breaks.
- **Span invalidation sharpened by CoW.** `contentOf`/`dimensionsOf`/`scratchTextOf` return borrows
  into dense vectors; the first write to a base cell shadows it, appending to those vectors — so a
  *read-looking* op (`linkedMinting`, `shadow`) invalidates spans a sibling thread is iterating.
- **`std::string` in `CellValue` and SSO.**
  `CellValue = std::variant<std::string, double, std::int64_t, bool>`. A string copy is not atomic
  (no COW strings since C++11); SSO means short-string bytes live inside the object and are
  rewritten on reassignment; and a `variant` alternative switch runs a destructor then constructor,
  so a concurrent `std::get` can read a `double` out of storage being reinterpreted as a
  `std::string`. The stdlib `map`/`filter`/`fold` `function_ref` surfaces
  (`vortex/vortex_stdlib.cpp:2214,2234,2259`) are the most tempting to parallelize and exactly where
  these payloads are shared.
- **False sharing.** `sizeof(CellSlot) == 32` (`zigzag/manifold.hpp:309`) and `TrailEntry == 40`
  (`zigzag/arena_manifold.hpp:154`); the hot `+d.cursors` rank tail
  (`vortex/vortex_vm.cpp:137-139`), the GC cursor cell (`vortex/vortex_host.cpp:86`), and the trail
  tail are cache lines that ping-pong across cores even under a correct lock.

### 5.2 Memory model, stated for this codebase

A data race is undefined behaviour — not a stale value, UB. `std::atomic` gives atomicity and an
ordering; `acquire`/`release` is the pairing for "I filled an arena, then flipped a ready flag"
(immutable-snapshot handoff), but an atomic flag orders the publication of a whole object, it does
not make `push_back` indivisible. A mutex is mandatory whenever an invariant spans more than one
memory location (`choiceStack_` push, `unordered_map::insert`, a multi-field `CellSlot` write).
`volatile` is not a concurrency tool — no atomicity, no ordering, no fence; reject any change that
reaches for it. x86 is strongly ordered and hides bugs that surface on ARM (Android and macOS-arm64
are packaging targets), which is the argument for ThreadSanitizer, which models the abstract
machine, not the chip.

### 5.3 Mitigation menu and the recommendation

A coarse mutex around the VM step is correct, trivial, and *is the GIL* — useful only as a first
TSan-clean baseline, not as the answer. Sharded locks help disjoint cursors but reintroduce the GIL
at the global `home`/`d.cursors` write targets and need lock ordering to avoid deadlock.
`std::shared_mutex` is unsafe here because "reads" mutate (CoW shadow), so a `shared_lock` taken for
a read that then shadows is a correctness bug. RCU/hazard pointers have no C++23 standard support
(vendoring collides with the no-new-dependency instinct) and do not compose with in-place
truncation. The recommended core is **thread-local arenas over an immutable published base with a
serialized `promote()` boundary** — it reuses the CoW isolation the arena already implements instead
of fighting it, each arena keeping exactly one owner (now a thread). The cost is memory (one arena
per worker), the serial base-freeze before fan-out, and `promote()` as the serial Amdahl throat.

### 5.4 Candid gotchas

"Read" paths mutate — audit every `const`-looking method before classifying it as a read.
`mark()`/`release()` is inherently serial and un-shareable; the global `choiceStack_` defeats
OR-parallel vprolog by construction and no incremental lock rescues it — each search branch needs
its own stack and arena. `compact()` is refused under outstanding marks and moves every run, so no
thread may compact an arena another is using (another argument for one arena per thread).
`promote()` is the serial write-back; plan for it as the Amdahl ceiling and keep it off the hot
path. `ephemeralBit` is a persistence net, not thread safety. And `stepScheduler()`
(`vortex/vortex_vm.cpp:1079`) is a two-line parallel-for trap: replacing the `for` with a
parallel-for is instant UB because every `step` writes `cursorPC_`, touches the shared arena, and
may push the global choice stack. The parallelization must happen one layer up — partition cursors
into disjoint-arena groups first, then run groups on threads.

### 5.5 Tooling

Use the hooks the Makefile already has, but understand what they cover. **ThreadSanitizer is
non-negotiable** for this work, and the flag macro to build on is `SANITIZE_THR_OPTS`
(`-fsanitize=thread,undefined,integer -fno-omit-frame-pointer`, `Makefile:438`, defined under
`ifdef DEBUG`). The catch: the stock `sanitize/thread` and `sanitize/address` targets depend only on
`gleditor` (`Makefile:855` and `:848`; the `/run` variants execute `$(OBJDIR)/gleditor` at `:858`
and the ASan `/run` likewise), and `gleditor` links only the plain-editor objects and the library
(`Makefile:823`) — code that by AGENTS.md design shares nothing with Vortex. Run as written, those
targets instrument none of the engine. Covering `vortex_vm`, `arena_manifold`, and the frontends
therefore requires a *new* sanitizer-instrumented build target (a Make target, no CMake) that
applies the `SANITIZE_THR_OPTS` and `SANITIZE_ADDR_OPTS` (`Makefile:436`) macros to the engine
objects and the `xudu_test`/`zigzag_test` gtest binaries. Instrument the whole engine and test
binary together — partial instrumentation gives false confidence — and run TSan and ASan as separate
configs, since they are mutually exclusive in one binary. AddressSanitizer (the `SANITIZE_ADDR_OPTS`
macro) catches the §5.1 use-after-reallocation and dangling-span bugs directly; note that a plain
`DEBUG=1` build sets only `-O0 -g` and enables no sanitizer, so lifetime shakeout must use a
sanitizer-flagged target, not `DEBUG=1` alone. Extend the fuzz targets with a multi-threaded stress
harness run under TSan over thousands of randomized interleavings (a single run proves nothing;
races are schedule-dependent). Stress tests must loop with high iteration counts and multiple cores;
the pass criterion is "TSan-clean over K iterations," not "right answer once." Establish a total
lock order the moment there is more than one lock, prefer `std::scoped_lock`, and treat any
`second_deadlock_stack` report (`TSAN_OPTIONS=second_deadlock_stack=1`, as the stock target already
sets at `Makefile:858`) as a release blocker.

## 6. Downstream impact and how each frontend exposes thread primitives

The common substrate every frontend relies on is "one arena per worker, one immutable base shared,
choice state localized." On top of it, each language exposes real parallelism in the shape that fits
it, and in every case the language-visible unit stays the green cursor — "thread" at the C++ layer
means the OS worker only, resolving the `spawnThread` naming trap by making the OS unit a *worker*
and keeping `spawnCursor`/`spawnThread` meaning "green cursor."

### 6.1 The stdlib

The functional surface is the shared foundation. `map` (`vortex/vortex_stdlib.cpp:2212`), `filter`
(`:2232`), and `fold` (`:2257`) each take a `function_ref` and today run synchronously on the
calling thread over a `rank(...)` range; their `CellRef fnOp` overloads (`:2224`, `:2248`, `:2269`)
call `call`, which spawns a cursor and runs it to completion inline (`:2075`/`:2093`). The
per-element function is already isolated behind `function_ref` and the ranges are materialized, so
the element function is the natural parallel unit — the obstacle is only that it currently runs
through `call` → `spawnCursor` → arena writes on the one shared arena. Under the new substrate, each
element function runs in the calling worker's own arena; `map`/`filter` preserve rank order by
construction and `fold` fixes a left-to-right chunk-index combine order so a non-commutative
combiner matches the serial result. Memoization (`getOrCreateMemoPin`/`recordMemo`/`lookupMemo`,
`vortex/vortex_core.cpp:515`/`:537`/`:524`) becomes per-worker; a shared cache is a later
promote-between-barriers optimization, not a stage-1 feature, because a live shared `d.cache` rank
would reintroduce the GIL. The primitive these surfaces expose to every language is a *parallel-for
over a rank with a deterministic merge*, not a raw thread.

### 6.2 VPL — data-parallel arrays (first and easiest)

VPL's array operations are the most natural fit: an operation over disjoint index ranges is
embarrassingly parallel with no cross-element arena writes. VPL (`vpl/compiler.cpp:1121` spawns
`"main"`) exposes parallelism *implicitly* through its existing array verbs — `map`/`filter`/`fold`
over a lattice grid simply fan disjoint chunks across workers when the array exceeds a configured
threshold (`vortex.parallel_threshold` in `system://settings`), each chunk on a worker arena over
the shared base. The language gains no new surface syntax in the first instance; the array verbs it
already has become parallel. This is the right default for an APL/J-style language where the
programmer thinks in whole-array operations and expects the runtime to find the parallelism. An
explicit opt-out (force-serial) is the only new knob a VPL author needs, for debugging determinism.

### 6.3 VQL — query parallelism

VQL (`vql/compiler.cpp:866` spawns `"main"`; `compileAndAttachVQL`, `vortex/vortex_host.cpp:513`) is
a dataflow pipeline of scans, filters, joins, and projections. It exposes parallelism
*declaratively*: the compiler partitions a scan or the probe side of a hash join across workers over
private overlays and merges in index order, with no language-visible thread primitive — a query says
*what* to compute and the planner decides the partitioning, exactly as a SQL engine parallelizes
without the query mentioning threads. The `spawnThread` boolean on `compileAndAttachVQL` is
repurposed as the honest place to request "run this query on its own worker" (its current meaning,
"mint a green cursor," is preserved and generalized). The contract VQL promises its users is
deterministic result sets and ordering across worker counts: identical to the serial compiler,
verified by `vquery`/`vqueryc` headless runs. Multi-store queries parallelize by federation — one
node per store, join on `GlobalOpRef` — without silently creating persistent cross-store links.

### 6.4 vprolog — parallel search (last and opt-in)

vprolog is the hardest because it spawns no cursor at all: its compiler lowers clauses into the
logic database and resolution runs on the VM's C++ logic opcodes — `Unify`
(`vortex/vortex_vm.cpp:789`), `Choice` (`:878`), `Fail` (`:887`), `Cut` (`:899`) — over the single
global `choiceStack_` and the single arena trail. Concurrent backtracking is inexpressible today
precisely because both the stack and the mark it holds are global (the Aurora/Muse multiple-binding
problem in C++). The substrate change — per-worker arenas with per-worker choice stacks — is the
prerequisite the WAM precedent predicts. vprolog exposes parallelism as *opt-in OR-parallel search*:
a directive or a `system://settings` flag (default off) lets the engine fork a worker overlay at a
choice point to explore an alternative clause (SRI/Muse per-worker binding store, spelled as
copy-on-write), merging solution sets deterministically by submission index (lowest-index branch is
"first solution"), with cut confined to its originating branch. AND-parallelism (independent
conjuncts) comes later and only where independence is provable, because dependent conjuncts
reintroduce a shared mutable trail. The language-visible promise: the same solution set and
first-solution as the serial engine, for programs including cut and negation. This is the one
frontend where a true thread primitive is explicit in the language, and it is deliberately gated
behind the proof that Stages 1–4 work.

### 6.5 Summary of VM-core changes the frontends ride on

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

## 7. Risks, open questions, and non-goals

**Risks.** The schedule-dependence of data races is the operational risk: a passing test proves
nothing, so the whole plan is gated on TSan over looping stress runs, not correctness tests.
`promote()` is the serial Amdahl throat — if a workload's purely in-arena fraction is small, the
parallel ceiling is low, and Stage 0's measurement exists to find that out before building further.
The base-freeze barrier serializes microversion rebinding, so a workload that rebinds the base
frequently loses parallelism; the interactive path is intentionally left on the cooperative
scheduler for this reason. False sharing on the cursor rank tail and the trail can eat the parallel
win even under correct locking, so cursor spawning and the trail must have single-threaded owners.

**Open questions carried forward from the experts.**

- *Static partition vs. work stealing.* Static cursor partitioning avoids cursor migration entirely;
  Go-style stealing needs Muse-style incremental cursor/stack copy across arenas. Recommendation:
  start static, measure imbalance, add stealing only if needed.
- *Base-freeze protocol.* The exact point at which the shared base is frozen after genesis and
  stdlib minting, and whether later stdlib additions force a controlled re-freeze, is a
  host-lifecycle question to pin down before Stage 2.
- *Cross-worker promotion ordering.* The determinism rule VQL needs depends on an ordering
  discipline over `promote()`; this study fixes it as submission-index order on a single promoting
  thread, but the exact key for OR-parallel vprolog first-solution semantics should be validated
  against a program suite in Stage 5.
- *Shared memo cache.* Deferred to a measured need; per-worker memoization is correct and lock-free
  first.

These are matters of staging and emphasis, not contradictions. The experts converge on the
architecture (share-nothing per-worker arenas, immutable base, serialized merge) and on the ordering
(VPL first, vprolog last). The only genuine divergence worth flagging is *degree of eagerness*: the
distributed and interpreter sections are comfortable describing OR-parallel vprolog as a natural
extension, while the C++ concurrency section is emphatic that the global `choiceStack_` makes it the
single hardest and most dangerous change. These are reconciled, not left open, by making vprolog
parallelism the last stage, opt-in, default-off, and gated on logic-equivalence tests under TSan —
the cautious reading governs the plan.

**Non-goals.** This study does not propose a shared mutable heap with a memory model and concurrent
GC (the JVM path); does not propose fine-grained arena locking or lock-free dense vectors; does not
change `CompactOpNode`, the spool writer, Merkle piece size, or append-only ordering; does not
parallelize the `promote()` commit itself; and does not parallelize short interactive
single-document edits, which stay on the proven cooperative scheduler. It does not introduce
AND-parallel vprolog with shared bindings. It does not add any new C++ dependency (ruling out
vendored RCU/hazard-pointer libraries for C++23).

## 8. Recommendation and next steps

**Recommendation.** Adopt per-worker share-nothing `ArenaManifold`s over one immutable base
`Manifold`, scheduled M:N across OS workers (a Vortex worker pool modeled as a sibling of
`render::WorkerPool`), with `choiceStack_`/`cursorPC_`/mark/release localized to each worker and a
single serialized `promote()` merge. Reject the coarse global lock as an endpoint (it is the GIL),
reject fine-grained arena locking (whole-arena truncation and dense-vector reallocation defeat it),
and reject lock-free (research project, no undo story). This is the only option that delivers real
parallel throughput on arena-bound workloads, keeps truncation-undo correct, and reuses the overlay,
`ephemeralBit`, and `promote()` the code already has. The owner's GIL fear is answered not by making
a shared arena faster but by never sharing a mutable arena.

**Next steps, in order.**

1. **Stage 0 — measure.** Add `vortex.sched` timing and a headless many-cursor benchmark; add a new
   Make target that applies the `SANITIZE_ADDR_OPTS` macro (`Makefile:436`) to the engine objects
   and `xudu_test`/`zigzag_test` (the stock `sanitize/address` target builds only `gleditor`), and
   run the engine suite under it single-threaded to clear latent lifetime bugs. Commit a baseline
   showing the shared arena is the serialization point.
1. **Stage 1 — make the VM movable** with no behavior change; verify `xudu-dump --section=ops` is
   byte-identical on every sample store.
1. **Stage 2 — Vortex worker pool and data-parallel VPL**; verify scaling and bit-identical
   serial-vs-parallel results, `fold` combine order tested with a non-commutative combiner.
1. **Stage 3 — parallel independent cursors**; verify clean under the engine-and-test
   ThreadSanitizer target (`SANITIZE_THR_OPTS`, `Makefile:438`, applied to the engine and gtest
   binaries — not the stock `sanitize/thread`, which covers only `gleditor`) over looping stress
   runs.
1. **Stage 4 — parallel VQL**; verify identical result sets and ordering across worker counts.
1. **Stage 5 — opt-in vprolog OR-parallel search**, default off, gated on logic equivalence (cut and
   negation) under TSan.

Every stage keeps `make -j$(nproc) check` and `make -j$(nproc) test` green, routes every tunable
(`vortex.worker_threads`, `vortex.parallel_threshold`, the vprolog parallel-search flag) through
`defaultSettingSpecs()`/`getSetting` in `system://settings`, and leaves the on-disk invariants
untouched. Determinism and thread safety are tests in CI-runnable form, not claims.

______________________________________________________________________

Change history: 2026-10-16 — initial synthesis of the five expert sections
(`_mt_section_codebase.md`, `_mt_section_interpreter.md`, `_mt_section_architecture.md`,
`_mt_section_distributed.md`, `_mt_section_cpp_concurrency.md`) into this study; incorporated the
critique panel's corrections (`_mt_section_*` citations reconciled: `vortex_vm.hpp:148-151` for the
four VM members; `make sanitize/address` rather than `DEBUG=1` for AddressSanitizer; `Mark` has
twelve high-water fields; memo/line-citation fixes). 2026-10-16 (rev 2) — corrected the sanitizer
verification plan: the stock `sanitize/thread`/`sanitize/address` targets build only `gleditor`
(`Makefile:848`/`:855`/`:823`) and instrument no Vortex code, so §exec/§3.6/§5.5/§8 now call for a
new Make target reusing the `SANITIZE_THR_OPTS`/`SANITIZE_ADDR_OPTS` macros (`Makefile:438`/`:436`)
against the engine objects and `xudu_test`/`zigzag_test`; fixed the host init-list citation and
dropped the "three frontend compilers" overclaim (vprolog has no engine member); aligned the
`filter` signature citations. 2026-10-16 (rev 3) — final citation reconciliation against the
re-verified source: host constructor at `vortex_host.cpp:76` with the member init list at `:77-78`
(the rev-2 entry had asserted `:78-79`/ctor `:77`, one line high); the `+d.cursors` `link(...)` call
at `vortex_vm.cpp:137-139` (comment at `:136`) in §1 and §5.1; and the stdlib `CellRef fnOp`
overloads at `:2224` (map), `:2248` (filter), `:2269` (fold) with the `filter` definition at `:2232`
(§6.1). The `stepScheduler` serial `for` loop body is `:1079-1087` (closing brace at `:1087`).

Reconciliation note: this study is grounded in and must stay consistent with
[`vortex-multithreading-briefing.md`](vortex-multithreading-briefing.md), the code-verified factual
basis. Where any claim here diverges from the briefing or from the code it cites, the briefing and
the code govern, and this study should be corrected to match.
