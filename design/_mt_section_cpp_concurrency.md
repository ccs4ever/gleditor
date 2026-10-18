# The C++ concurrency reality

This section is the engineering ground truth for making the Vortex VM run on real OS threads. It is
deliberately pessimistic. Everything below is cited against the code in the
`study/vortex-multithreading` worktree. The headline finding first: **nothing in `VortexCore`,
`VortexVM`, or `ArenaManifold` is thread-safe, and several of the data structures are not merely
unsynchronized but are actively hostile to concurrent access because a write by one thread
invalidates pointers, iterators, and indices that another thread is mid-read on.** The green-thread
scheduler hides all of it today because `stepScheduler()` runs every cursor on the calling thread in
a plain `for` loop (`vortex/vortex_vm.cpp:1078`).

The owner's fear — a Python-GIL-style bottleneck around the one shared `ArenaManifold` — is
well-founded, and worse than the GIL in one respect: CPython's GIL at least makes the interpreter
memory-safe. Here there is no lock at all, so the naive "just run cursors on threads" change is not
slow, it is undefined behaviour.

## 1. Concrete data-race and invalidation hazards in this code

These are not hypotheticals. Each is a specific field or container that two cursors touch on
ordinary opcodes, with the specific C++ mechanism by which concurrent access breaks.

### 1.1 The unsynchronized `unordered_map`s in `VortexVM`

`VortexVM` holds four mutable members with no lock (`vortex/vortex_vm.hpp:148-151`):

```text
std::unordered_map<CellRef, OpcodeKind> opcodeMap_;
std::unordered_map<CellRef, std::string>  memoizedOps_;
std::unordered_map<CellRef, CellRef>      cursorPC_;
std::vector<ChoicePoint>                  choiceStack_;
```

`cursorPC_` is written on **every cursor step**: `setCursorOpcode()` does `cursorPC_[cursor] = op`
(`vortex/vortex_vm.cpp:120`) and `getCursorOpcode()` reads it (`vortex/vortex_vm.cpp:108-109`). If
two OS threads advance two cursors concurrently, this is a textbook data race on a
`std::unordered_map`:

- **Rehash invalidation.** `operator[]` on a new key can trigger a rehash. A rehash reallocates the
  bucket array and relinks every node. A concurrent `find()` on another thread is then walking a
  bucket chain that is being rewritten under it — a use-after-free, not a stale read. The standard
  gives `unordered_map` no internal synchronization; concurrent `insert`/`find` is a data race and
  therefore undefined behaviour, full stop.
- **Iterator/reference invalidation.** The standard guarantees references to elements survive
  rehash, but *iterators do not*, and pointers into the bucket array do not. Any code holding an
  iterator across an insert on another thread is broken.
- **Torn reads of the mapped value.** `memoizedOps_`'s value is a `std::string`
  (`vortex/vortex_vm.hpp:149`); `getMemoKey()` returns it by value (`vortex/vortex_vm.cpp:157-158`).
  A concurrent `enableMemoization()` writing `memoizedOps_[opcode] = std::string(memoKey)`
  (`vortex/vortex_vm.cpp:149`) can reallocate that string's buffer while the reader is copying it —
  a classic small-string/SSO vs heap-buffer race, see §1.5.

`opcodeMap_` is comparatively read-heavy after assembly, but assembly and execution are not phased:
`mintOpcode()` and `setOpcodeKind()` write it (`vortex/vortex_vm.cpp:69,92`), and VQL/VPL compilers
assemble opcodes and `spawnCursor` in the same call path (`vql/compiler.cpp:866`,
`vpl/compiler.cpp:1121`). A design that spawns real threads during or after compilation while the
compiler is still minting opcodes races `opcodeMap_`.

### 1.2 `choiceStack_` reallocation under concurrent push

`choiceStack_` is a **single global `std::vector<ChoicePoint>`**, not per-cursor
(`vortex/vortex_vm.hpp:151`). `pushChoicePoint()` does `choiceStack_.push_back(...)`
(`vortex/vortex_vm.cpp:169`), `backtrack()` does `choiceStack_.back()` then `pop_back()`
(`vortex/vortex_vm.cpp:180-181`), and `cut()` pops in a loop (`vortex/vortex_vm.cpp:198-200`).

Two independent problems:

1. **Reallocation.** `push_back` past capacity reallocates and moves every element. A thread reading
   `choiceStack_.back()` concurrently has a dangling reference the instant the other thread grows
   the vector. `size()`/`empty()` reads (`vortex/vortex_vm.cpp:179,198,206`) are torn against a
   concurrent `push_back`/`pop_back`.
2. **Semantic entanglement, which is worse than the race.** `cutBarrier` is set to
   `choiceStack_.size()` at push time (`vortex/vortex_vm.cpp:174`), and `cut()` truncates the stack
   back to a `cutBarrier` index. The barrier is an *index into the shared stack*. If two cursors
   push choice points interleaved, cursor A's cut barrier points into the middle of cursor B's
   frames. Even with a perfect lock making every operation atomic, the **meaning** is wrong: a
   global choice stack cannot express two cursors backtracking independently. This is the single
   deepest obstacle to OR-parallel vprolog (see §5).

### 1.3 `CellRef` index stability vs. dense-vector reallocation

`CellRef` is a bare `std::uint32_t` (`zigzag/dim_vector.hpp:21`). That is a saving grace and a trap
at once.

- **The ref value is stable.** It is `ephemeralBit | dense` for an arena cell
  (`zigzag/arena_manifold.hpp` `refOf`), pure arithmetic. Passing a `CellRef` between threads does
  not dangle. Good.
- **Everything the ref *points into* is a dense `std::vector` that reallocates.** `ArenaManifold`
  stores `std::vector<CellSlot> slots_`, `std::vector<DimLink> links_`,
  `std::vector<PrimediaSpan> content_`, `std::string scratch_`, `std::vector<TrailEntry> trail_`
  (`zigzag/arena_manifold.hpp`, private members). Any `makeCell()` / `link()` / `setContent()` /
  `intern()` can `push_back` past capacity and **reallocate the whole backing array**. A second
  thread that resolved a `CellRef` to a `CellSlot*`, a `SlotRef`, or a `std::span` before that
  reallocation now holds a pointer into freed memory.

The code tells you this itself. `SlotRef` is documented as "A reference into the manifold's slot
array, so valid until the next fold or write grows it" (`zigzag/manifold.hpp:311-313`). `slot()`
returns one, `contains()` calls `slot(...).has_value()` (`zigzag/arena_manifold.hpp`), and
`cells()` hands out a `std::span<const CellSlot>` over `slots_` directly
(`zigzag/arena_manifold.hpp`). Any of these, held across a concurrent write, is a dangling view. The
single-owner contract that makes this safe today — "dies with its owner," the class comment at
`zigzag/arena_manifold.hpp:12` — is exactly the contract true threading breaks.

### 1.4 Span invalidation from arena growth

`contentOf()` returns `std::span<const PrimediaSpan>` into `content_`, and `dimensionsOf()` returns
`std::span<const DimLink>` into `links_` (`zigzag/arena_manifold.hpp`). `scratchTextOf()` returns a
`std::string_view` into `scratch_` (`zigzag/arena_manifold.hpp`). All three are borrows into dense
vectors with no copy. The CoW `shadow()` path makes this sharper: the first write to a base cell
"copies the whole cell, its link run and its content run into the arena" (class comment,
`zigzag/arena_manifold.hpp:36-41`), which means a *read-looking* operation on an overlay arena
(`linkedMinting()`, `shadow()`) can append to `links_`/`content_`/`slots_` and invalidate spans a
sibling thread is iterating. "Read falls through to base, first write shadows" is a mutation hiding
inside a traversal — the worst kind of surprise for a reader thread.

### 1.5 `std::string` in `CellValue` and small-string sharing

`CellValue = std::variant<std::string, double, std::int64_t, bool>`
(`vortex/vortex_core.hpp:*`, the `using` at the top of the file). Passing a `CellValue` by const
reference across threads looks innocent, but:

- A `std::string` copy is not atomic. Copying the `std::string` alternative while another thread
  mutates the source races the pointer/size/capacity triple. There is no COW in libstdc++/libc++
  strings anymore (C++11 forbade it), so every copy touches the source's three words; a torn read
  of capacity vs. a heap pointer being swapped produces a buffer overrun.
- SSO (small-string optimization) means the bytes live *inside* the `std::string` object for short
  strings. A `CellValue` moved or reassigned on one thread rewrites those inline bytes while a
  reader copies them. No external buffer is involved, so sanitizers key on the object storage, but
  it is still a race.
- `std::variant` assignment that changes the active alternative runs a destructor then a
  constructor. A concurrent `std::get`/`std::holds_alternative` can observe the variant mid-switch,
  reading a `double` out of storage that is being reinterpreted as a `std::string`.

The stdlib `map`/`filter`/`fold` surfaces take `function_ref<CellValue(const CellValue &)>` and
friends (`vortex/vortex_stdlib.cpp:2214,2234,2259`). These are the most tempting to parallelize
(§5 of the briefing says VPL array ops are "the most natural fit"), and they are exactly where
`CellValue` string payloads would be shared read-mostly across worker threads. Read-mostly is still
not read-only unless you *prove* the callback is pure and the inputs are frozen.

### 1.6 False sharing on hot cells

`sizeof(CellSlot) == 32` (`zigzag/manifold.hpp:309`). Two `CellSlot`s share a 64-byte cache line.
The hottest cells in the system are the ones every cursor touches: `home_` (`vortex_core.hpp:*`),
the `+d.cursors` rank tail that `spawnCursor()` links onto (`vortex/vortex_vm.cpp:135-137`), and the
system GC cursor cell (`vortex_host.cpp:86`). If those live in adjacent slots — and the rank tail
plus freshly minted cursors are allocated near each other by construction — then even a *correctly
locked* design suffers false sharing: a write to the cursor rank tail invalidates the cache line
holding `home`'s neighbour on every other core. The `DimLink` run and `TrailEntry` (`sizeof ==
40`, `zigzag/arena_manifold.hpp` `static_assert`) vectors have the same hazard: the trail is
appended on nearly every binding (`trail()` in the WAM-style conditional path), so the trail tail is
a false-sharing magnet.

## 2. The C++ memory model, stated precisely for this codebase

The team must internalize these before writing any `std::atomic` or picking a lock granularity.

- **A data race is undefined behaviour.** Two accesses to the same non-atomic object, at least one a
  write, not ordered by a happens-before relationship, is UB — not "a stale value," UB. The compiler
  is entitled to assume it never happens and optimize accordingly. Every hazard in §1 is this.
- **`std::atomic` gives you two things: atomicity and an ordering.** The default
  `memory_order_seq_cst` is correct and slow. `acquire`/`release` is the pairing that matters:
  a `release` store on thread A *publishes* every write A did before it; an `acquire` load on
  thread B that reads that value *sees* all of them. This is the tool for "I filled an arena, then
  flipped a ready flag" (immutable-snapshot handoff, §3.5). It is **not** a substitute for a mutex
  around a multi-word structure like a `std::vector` or `unordered_map`: an atomic flag orders the
  publication of a whole object, it does not make `push_back` indivisible.
- **A mutex is required whenever an invariant spans more than one memory location** and both must be
  seen consistently. `choiceStack_` push (grow array + write element + update size) is three
  locations; `unordered_map::insert` (allocate node + relink bucket + bump count) is several; a
  `CellSlot` write (`linkOffset`, `linkCount`, `valueBits`) is multiple fields that must agree.
  None of these can be made correct with atomics on the individual words; they need a critical
  section, or a redesign that makes the structure immutable while shared (§3.5).
- **`volatile` is not a concurrency tool and never was.** `volatile` suppresses compiler
  optimizations around a single access (it was designed for memory-mapped device registers). It
  gives you **no** atomicity, **no** happens-before ordering, and **no** cache-coherence fence.
  A `volatile bool ready` flag is a bug: the writes it was supposed to publish can be reordered
  across it by the CPU, and torn/duplicated by the compiler on non-atomic-width types. If anyone
  reaches for `volatile` to "fix" a race in this code, reject the change. Use `std::atomic` with an
  explicit order, or a mutex.
- **Compiler and hardware both reorder.** x86 is strongly ordered and will hide many bugs that then
  surface on ARM (relevant: Android and macOS-arm64 are packaging targets). TSan catches these
  regardless of host ISA because it models the C++ abstract machine, not the chip — which is the
  argument for §4.

## 3. Mitigation menu, with honest tradeoffs

No option is free. Ordered roughly from least-invasive and least-scalable to the opposite.

### 3.1 One coarse mutex around the whole VM step

Wrap `step(cursor)` (`vortex/vortex_vm.cpp`) and every `VortexCore` structural primitive in a single
`std::mutex`. Correct, trivial, and **this is the GIL** — it reproduces exactly the bottleneck the
owner fears. Throughput is capped at one core regardless of how many OS threads you spawn; you pay
thread-switch and contention cost on top for a net loss on anything but I/O-bound workloads. Useful
only as a *first correctness milestone* to get a TSan-clean baseline before sharding. Do not ship
it as the answer.

### 3.2 Sharded locks

Partition the arena (or the cursor set) into N shards, each with its own mutex; a cursor touching
only its shard contends only with its shard. Good speedup when cursors are *disjoint* in the cells
they touch. Two hard problems specific here: (a) the CoW overlay and `home`/`d.cursors` rank are
**global** write targets every cursor hits (`spawnCursor` links onto the shared rank tail,
`vortex/vortex_vm.cpp:135-137`), so a hot shard reintroduces the GIL for cursor lifecycle; (b)
cross-shard links (`link(from, dim, to)` where `from` and `to` are in different shards) need a
lock-ordering protocol or they deadlock (§4). Sharding the choice stack per-cursor is separately
necessary and valuable regardless (§5).

### 3.3 Reader-writer lock (`std::shared_mutex`)

Fits the read-mostly phases: many cursors reading the manifold, occasional structural write. Cheap
when writes are rare. But this codebase's "reads" are not pure — `linkedMinting()` mints,
`shadow()`/CoW writes on first touch (§1.4) — so a `shared_lock` taken for a "read" that then
shadows a base cell is a correctness bug (upgrade-under-shared-lock is not supported and deadlocks
if two readers both try). Viable only after the read path is audited and genuinely-mutating
"readers" are reclassified as writers. Writer starvation under heavy read load is the usual
operational gotcha.

### 3.4 RCU / hazard pointers for the arena

Read-copy-update or hazard pointers let readers run fully lock-free while a writer swaps in a new
version and defers reclamation until no reader references the old one. This is the high-performance
answer and the highest-complexity one. The dense-vector + stable-`uint32_t`-index design is
*almost* RCU-friendly: indices don't dangle, only the backing array does, so an RCU scheme that
publishes a new immutable `slots_`/`links_`/`content_` snapshot via an `atomic<shared_ptr>` and
retires the old one once quiescent would work. Costs: you must never hand out a raw `CellSlot*`/span
that outlives the read-side critical section (every API in §1.3–1.4 violates this today); you need a
quiescence mechanism (epoch counters or `std::hazard_pointer` once it lands — not in C++23, so
you'd vendor a library, which collides with the no-new-dependency instinct in AGENTS.md); and the
mark/release trail (`zigzag/arena_manifold.hpp` `mark()`/`release()`) assumes in-place truncation,
which is fundamentally mutating and does not compose with copy-on-publish. RCU is plausible for a
*read-only published base* with per-thread scratch arenas layered on top (§3.6), not for the mutable
trail itself.

### 3.5 Immutable snapshot + copy-on-write

Freeze a `Manifold` as an immutable base, give each worker an `ArenaManifold` overlay over it
(`ArenaManifold(const Manifold *base)`, `zigzag/arena_manifold.hpp`). Reads fall through to the
shared immutable base with no lock; writes shadow into the thread's *own* arena and never touch the
base ("The base is never touched," class comment `zigzag/arena_manifold.hpp:40`). This is the single
best fit for the existing architecture, because **the CoW machinery already exists and was built for
exactly this isolation**. The base must be genuinely immutable for the duration (the comment warns
it "must not be mutated while it is being read through," `zigzag/arena_manifold.hpp` ctor doc). The
cost is memory (one arena per worker) and the merge problem: combining per-worker answers back into
one store goes through `promote()`, which is serial and budgeted (`PromotionBudget`,
`zigzag/arena_manifold.hpp`). Determinism of merged results is a VQL/vprolog semantics question, not
a C++ one, but the C++ reality is that per-arena isolation makes the parallel *section* race-free.

### 3.6 Thread-local arenas (the recommended core)

The strongest version of 3.5: a shared, immutable, published base manifold, plus a strictly
thread-local `ArenaManifold` per OS thread, with `promote()` as the only, serialized, write-back
boundary. This maps the existing single-owner `ArenaManifold` contract onto threads *without
breaking it* — each arena still has exactly one owner, that owner is now a thread. The GIL
dissolves because the shared state (the base) is read-only during parallel work, and the mutable
state (each arena) is unshared. What you still pay: the base must be built and frozen before fan-out
(a serial phase), `promote()` and any cross-thread result reconciliation are serial (Amdahl's law
sets your ceiling by how much work is purely in-arena), and anything that must mutate the *shared*
store mid-flight (the GC cursor, `vortex_host.cpp:86`; the `d.cursors` rank) needs its own
single-threaded owner or a lock. This is the design the rest of the study should build on.

## 4. Tooling the repo must run before claiming any of this works

The Makefile already has the hooks; use them, do not reinvent them.

- **ThreadSanitizer.** The `sanitize/thread` target exists:
  `SANITIZE_THR_OPTS := -fsanitize=thread,undefined,integer -fno-omit-frame-pointer`
  (`Makefile:438`), wired to `sanitize/thread` and `sanitize/thread/run` with
  `TSAN_OPTIONS=second_deadlock_stack=1:detect_leaks=1:strict_string_checks=1`
  (`Makefile:853-858`). **TSan is non-negotiable for this project** — it is the only tool that
  reliably finds the §1 races, and it finds them on x86 even though the bug would only *manifest* on
  ARM. Caveat: TSan instruments only code it compiles, so the whole engine and the test binary must
  be built with it (a partially-instrumented build gives false confidence). TSan and ASan are
  mutually exclusive in one binary; run them as separate build configs.
- **AddressSanitizer.** `DEBUG=1` turns on `SANITIZE_ADDR_OPTS` with
  `-fsanitize=address,undefined,integer`, `-fsanitize-address-use-after-return=runtime`, and
  `-fsanitize-address-use-after-scope` (`Makefile:435-437,841`). ASan is what catches the §1.3–1.4
  use-after-reallocation and dangling-span bugs directly: a span into a `std::vector` that got
  `push_back`'d is a heap-use-after-free ASan flags on the next read. Run the existing test suite
  under `DEBUG=1` *first*, single-threaded, to shake out the latent lifetime bugs that threading
  will only make more frequent.
- **The fuzz targets, extended for concurrency.** `make fuzz` builds `fuzz_binary_ops`,
  `fuzz_link_package`, `fuzz_identity_wire` against a sanitizer copy of the engine
  (`FUZZ_FLAGS := -fsanitize=fuzzer-no-link,address,undefined`, `Makefile:906`;
  `fuzz: fuzz_binary_ops fuzz_link_package fuzz_identity_wire`, `Makefile:936`). Add a
  **concurrency stress harness** — not strictly a libFuzzer target, but a multi-threaded driver that
  spawns N real threads each advancing cursors over a shared arena, run under TSan, with randomized
  interleavings and schedules. A single run proves nothing (races are schedule-dependent); run it
  thousands of iterations in CI with TSan, and consider a deterministic scheduler shim to force
  pathological interleavings.
- **Stress tests with contention, not just correctness tests.** A test that spawns two threads and
  passes once is worthless for concurrency. Tests must loop with high iteration counts, randomize
  work distribution, and pin to multiple cores; the pass criterion is "TSan-clean over K
  iterations," not "produced the right answer once."
- **Lock-ordering discipline to prevent deadlock.** The moment there is more than one lock (§3.2
  sharded, or a per-arena lock plus a store lock), establish a **total order** on locks and acquire
  them only in that order, everywhere, no exceptions. Cross-shard `link()` (`vortex_core.hpp` link
  primitive) touching two shards must lock the lower-indexed shard first. `TSAN_OPTIONS=second_
  deadlock_stack=1` (`Makefile:858`) makes TSan report both stacks of a lock-order inversion; treat
  any such report as a release blocker. Prefer `std::scoped_lock` (deadlock-avoiding) over
  nested `lock_guard`s wherever two locks are genuinely needed at once.

## 5. Gotchas specific to the zigzag CSR arenas and the Vortex opcode loop

The list that will actually bite, beyond the generic C++ ones:

- **"Read" paths mutate.** CoW shadowing (`shadow()`, `linkedMinting()`,
  `zigzag/arena_manifold.hpp`) means an operation that looks like a lookup appends to the dense
  vectors on first touch of a base cell. Any locking scheme that classifies these as reads
  (`shared_lock`) is wrong. Audit every `const`-looking method for a hidden write before trusting
  it.
- **`mark()`/`release()` is inherently serial and global to one arena.** A `Mark` is a tuple of
  high-water lengths into the dense vectors (`struct Mark`, `zigzag/arena_manifold.hpp`), and
  `release()` truncates back to them (`vortex/vortex_vm.cpp:183`). Truncation is only meaningful if
  nothing allocated above the mark on *another* thread in the meantime. Mark/release cannot be
  shared across threads at all; each thread needs its own arena with its own trail. The briefing is
  right that "concurrent backtracking by two OS threads over one arena is not expressible today" —
  and it cannot be made expressible by locking, only by giving each searcher its own arena.
- **The global choice stack defeats OR-parallelism by construction.** As in §1.2, `cutBarrier` is an
  index into one shared `choiceStack_` (`vortex/vortex_vm.cpp:174,198`). vprolog's AND/OR search is
  the workload that most wants parallelism and is the one the current data model most forbids. Any
  parallel-search design must give each search branch its own choice stack *and* its own arena, then
  reconcile solutions — there is no incremental lock that rescues the shared stack.
- **`compact()` is refused while a mark is outstanding** (`compact()` returns
  `ArenaRefusal::MarksOutstanding`, `zigzag/arena_manifold.hpp`), and compaction moves every run.
  Under threading this means no thread may compact an arena any other thread (or choice point) is
  using — another argument for one-arena-per-thread, where compaction is a private decision.
- **`promote()` is the only write-back and it maps refs, serially.** `promote()` walks the reachable
  subgraph and mints real operations (`zigzag/arena_manifold.hpp` `promote()` doc), with a budget.
  It is the serial throat every parallel result passes through. Plan for it to be the Amdahl ceiling
  and keep it off the hot path.
- **`ephemeralBit` is a safety net, not a lock.** A leaked arena ref is caught by `Store::setLink()`
  and `applyStructure()` refusing an ephemeral ref (class comment,
  `zigzag/arena_manifold.hpp:26-33`).
  That catches *persistence* of a stray ref; it does nothing about a *data race* on the arena the
  ref lives in. Do not mistake the R8 refusal machinery for thread safety.
- **The scheduler loop is the obvious parallelization point and the obvious trap.**
  `stepScheduler()` iterates `activeCursors()` and calls `step(cursor)` on each
  (`vortex/vortex_vm.cpp:1078-1086`). Replacing the `for` with a parallel-for is a two-line change
  that produces instant UB: every `step` writes `cursorPC_` (§1.1), touches the shared arena
  (§1.3), and may push the global choice stack (§1.2). The parallelization must happen one layer up
  — partition cursors into disjoint-arena groups first, *then* run groups on threads — not by
  threading the existing loop.
- **False sharing on the `d.cursors` rank tail and the trail.** `spawnCursor()` links every new
  cursor onto the shared rank tail (`vortex/vortex_vm.cpp:135-137`) and the trail is appended on
  nearly every binding; with 32-byte `CellSlot`s and 40-byte `TrailEntry`s packed in dense vectors,
  these tails are cache lines that ping-pong across cores even when the logic is correct. Cursor
  spawning and the trail belong to single-threaded owners, or you pay coherence traffic that eats
  the parallel win.

## Bottom line

The current code is single-owner by design and says so (`zigzag/arena_manifold.hpp:12`, "dies with
its owner"). True multithreading cannot be bolted onto the shared `ArenaManifold`; the honest paths
are (a) a coarse lock as a throwaway correctness baseline, then (b) **an immutable published base
with thread-local arenas and a serialized `promote()` boundary** (§3.5–3.6), which is the one
option that reuses the CoW isolation the arena already implements instead of fighting it. The global
`choiceStack_` and the global `mark()`/`release()` trail must become per-thread; no locking strategy
makes a *shared* choice stack correct for parallel search. Gate everything on `sanitize/thread`
(`Makefile:853`) and `DEBUG=1` ASan (`Makefile:435`), with looping multi-thread stress runs, because
these races are schedule-dependent and a one-shot test proves nothing.
