# Architecture: true multithreading for the Vortex VM

This section proposes the concrete threading architecture. It is grounded in the current code read
during the study: `VortexCore` holds exactly one `ArenaManifold &arena_` (`vortex/vortex_core.hpp`);
`VortexVM` owns the single global `std::vector<ChoicePoint> choiceStack_` and the unsynchronized
`cursorPC_`/`opcodeMap_`/`memoizedOps_` maps (`vortex/vortex_vm.hpp`); the scheduler is a plain
serial `for` loop over `activeCursors()` (`vortex_vm.cpp:1076`); and `ArenaManifold` is explicitly
"dense vectors only, no ops, dies with its owner," single-owner, not thread-safe
(`zigzag/arena_manifold.hpp`). The owner's fear is exact: that single shared arena, read or written
on nearly every opcode, is a genuine GIL-analogue. The design below removes the shared mutable arena
from the hot path rather than guarding it with a lock.

## 1. The threading model to adopt, and why

### 1.1 Rejected: one shared arena behind a lock

The obvious move — wrap `VortexCore::arena_` in a mutex — reproduces the GIL exactly. Every
structural primitive (`link`, `value`, `splice`, `newCell`, `breakLink`, `pin`, `collectGarbage`,
memo record/lookup) mutates that one arena, so a lock around it serializes nearly every opcode.
Worse, `ArenaManifold`'s undo model forbids it on correctness grounds, not just throughput: `mark()`
reads seven high-water lengths off the arena's dense vectors and `release()` truncates them, and
`compact()` is *refused while a mark is outstanding* because compaction relocates every run and
invalidates offsets a `Mark` recorded. Two OS threads marking and releasing against one set of dense
vectors would corrupt each other's undo. A lock big enough to prevent that is a lock around the
whole VM.

### 1.2 Rejected: fine-grained locking inside `ArenaManifold`

Per-cell or per-rank locks on the arena's CSR vectors (`slots_`, `links_`, `content_`, `trail_`) do
not survive the growth operations. A write that shadows a base cell can *relocate a cell's whole
link run to the arena's tail* (documented on `TrailEntry` in `arena_manifold.hpp`), reallocating
`links_`; `mark()`/`release()` reason about absolute lengths of those shared vectors. Fine-grained
locking here means locking the vectors themselves on every append, which is the GIL again with more
overhead and a much larger surface for ABA and offset-staleness bugs.

### 1.3 Recommended: share-nothing arena-per-worker + message passing, data-parallel VPL

The staged target is **share-nothing**: each OS worker owns its own `ArenaManifold`, its own
`VortexCore`, its own `VortexVM` (and therefore its own `choiceStack_`, `cursorPC_`, `opcodeMap_`).
Workers never touch another worker's arena. They coordinate through explicit message passing — a
submit/complete handoff of immutable request and result values — not through shared mutable manifold
state.

This fits the code we already have, for three reasons that are properties of the current design, not
wishes:

- **The copy-on-write overlay already makes read-sharing free and write-sharing impossible by
  construction.** An `ArenaManifold` constructed over a base `Manifold` falls through to the base
  for reads and shadows a cell on first write, keeping the base `CellRef` as identity, and *the base
  is never mutated* (class comment, `arena_manifold.hpp`). A base `Manifold` is ops-backed and, once
  built for a microversion, is logically immutable. So N worker arenas can share one base `Manifold`
  as a read-only clause/data database with zero synchronization, each shadowing privately. The
  share-nothing model is the overlay used as intended, N-up.

- **`ephemeralBit` already prevents a leaked ref from crossing a boundary silently.** Arena refs are
  `ephemeralBit | dense`; `Store::setLink()` throws on an ephemeral ref and
  `Manifold::applyStructure()` counts one as refused. A worker that accidentally shipped a raw arena
  ref to another worker or to the store is caught by machinery that predates this project. Message
  passing between workers carries *values* (numbers, interned text, span addresses in a named
  scroll) or `promote()`d persistent refs, never live ephemeral refs.

- **`promote()` is already the single, ref-mapping road to persistence.** It walks the subgraph
  reachable from an answer root, spools scratch bytes into the permascroll, writes deferred scalar
  renderings, and *maps* every ephemeral ref to a real one (`arena_manifold.hpp`, `promote()` doc).
  Nothing else writes persistent operations. That means the "merge back to the shared document" step
  has exactly one chokepoint to make safe, and it is already written to refuse untranslated refs.

The data-parallel path for VPL is a specialization, not a second model: VPL's `map`/`filter`/`fold`
already take `gleditor::cpp26::function_ref` (README; the stdlib surfaces), and an array operation
over disjoint index ranges is embarrassingly parallel with no cross-element arena writes. VPL feeds
the worker pool a per-chunk closure exactly as `render::WorkerPool::run(count, work)` does today.

### 1.4 Why this is not a lock on one shared arena

The shared thing becomes the *base* `Manifold` (immutable, read-only, needs no lock) plus a tiny
message channel. The mutable thing — the arena, the choice stack, the trail — becomes per-worker and
is touched by exactly one thread for its whole life. The GIL disappears because there is no shared
mutable state on the opcode hot path, not because a lock around it got faster.

## 2. How OS threads map to cursors: an M:N scheduler over a worker pool

Today a "cursor" is a green thread: `spawnCursor()` mints a cell, records its PC in `cursorPC_`, and
links it onto home's `+d.cursors` rank (`vortex_vm.cpp:127`); `stepScheduler()` walks that rank and
calls `step(cursor)` serially (`vortex_vm.cpp:1076`). The name "thread" in the code already means
"green cursor" — `compileAndAttachVQL(..., bool spawnThread)` with `spawnThread == true` only calls
`vm_.spawnCursor(...)` (`vortex_host.cpp:513`). A real-threads design must keep that meaning and add
OS threads *underneath* it, not rename it.

The model is **M green cursors scheduled onto N OS workers (M:N)**:

- **The worker pool is the engine's own, modeled on the proven `render::WorkerPool`.** That pool
  starts N threads once, parks them between batches, has the caller take a share rather than block,
  rethrows the first worker exception after all finish, and borrows the closure as a `function_ref`
  to avoid a per-batch heap allocation (`include/gleditor/render/worker_pool.hpp`). It is the
  codebase's established answer to "run the same function across threads," cited by Vulkan worker
  recording. The Vortex scheduler reuses this shape. Where Vortex needs long-lived workers that pull
  from a queue rather than a blocking indexed batch, the Vortex worker pool is a sibling in
  `apps/common/xanadu/vortex/`, not a modification of the render one (the render pool's blocking
  batch contract is load-bearing for frame recording and must not change).

- **A cursor is pinned to a worker for the duration of a step, and ideally for its life.** Because
  each worker owns its arena/VM, a cursor's `cursorPC_` entry, its slice of `choiceStack_`, and the
  cells it minted all live in one worker. Migrating a running cursor between workers would mean
  moving arena state across threads, which the share-nothing model exists to avoid. So cursors are
  **assigned to a worker at spawn** and stay there. `stepScheduler()` becomes: partition
  `activeCursors()` by owning worker, hand each worker its sublist, join. Within a worker the inner
  loop is still the serial `for` over its own cursors — unchanged logic, now one of N.

- **`+d.cursors` stays the registry of cursor identity; worker ownership is a second rank.** Cursor
  identity must remain a manifold fact (the scheduler reads `activeCursors()` off `+d.cursors`). We
  add a per-worker home: each worker's `VortexCore` has its own `home_` and its own `+d.cursors`
  rank in its own arena. The host-level scheduler owns the mapping from a logical cursor to its
  worker. This keeps "a cursor is a cell on a cursors rank" true per worker and keeps the
  green-thread abstraction intact for the frontends.

- **Worker count is configuration, read from a system xanadoc, never a literal.** The existing
  `VortexHostConfig::fromStore()` already pulls `vortex.cycle_budget` and
  `vortex.gc_interval_frames` out of `system://settings` via `xanadu::getSetting`
  (`vortex_host.cpp`). Worker count joins them as `vortex.worker_threads` (0 or unset ⇒ probe
  `std::thread::hardware_concurrency()`, matching the render pool's "pool of one runs on the caller"
  degenerate case so single-threaded stays a first-class, allocation-free mode).

## 3. Ownership and lifecycle of arenas across threads

- **One arena per worker, created once, outliving individual cursors.** `VortexHost` today owns a
  single `arena_`, built in its constructor over an optional base `Manifold` and rebuilt by
  `bindManifold()`/`bindStore()` (`vortex_host.cpp`). The multithreaded host owns a *vector* of
  worker contexts, each `{ArenaManifold, VortexCore, VortexVM, VortexStdLib}`, each arena
  constructed over the **same shared base `Manifold*`**. The base is the read-only document; the
  overlay makes each worker's writes private copies. No worker's arena is ever handed to another
  thread.

- **The base `Manifold` is shared immutably and must outlive every worker arena.** The arena
  constructor already documents that the base "must outlive this, and must not be mutated while it
  is being read through." In the single-thread world that is a lifetime note; in the multithreaded
  world it becomes a hard invariant enforced at the host: the host holds the base, hands workers a
  `const Manifold*`, and tears every worker arena down before releasing the base. Rebinding the base
  (a new microversion) is a barrier: quiesce all workers, swap the base, rebuild worker arenas. This
  is `bindManifold()` generalized to N arenas with a join in the middle.

- **Promotion is the only cross-thread write, and it is serialized on the store.** `promote()` is
  the only path from an ephemeral arena to persistent operations, and `Store::save()` /
  `SegmentedOpsSpool::writeSegmentFile()` is the one writer of `ops.nodes`. A worker that finishes
  an answer does **not** promote on its own thread into the shared store. It ships the answer root
  (and the self-contained reachable subgraph) back to the host via the result channel; the host
  promotes on a single designated thread (or under a store-level lock), preserving append-only
  ordering and the single-writer invariant. This keeps `CompactOpNode == 64`, 64 KiB Merkle pieces,
  and append-only untouched: nothing about the node layout or the spool changes, only *which thread*
  calls the existing writer, and only one does.

- **Ephemeral refs never leave their worker.** A worker's result is either a `CellValue` (the
  `std::variant<std::string,double,std::int64_t,bool>` the VM already speaks), a scroll-addressed
  span (an address valid across threads because it names a scroll, not an arena slot), or a request
  to promote a described subgraph. A live `CellRef` with `ephemeralBit` set is meaningful only
  inside the arena that minted it and is never a message payload. If one leaks, `Store::setLink()` /
  `applyStructure()` refuse it — the existing R8 safety net now doubles as a cross-thread leak
  detector.

## 4. Backtracking / mark / release becomes per-worker

The current choice-point machinery is global and inherently serial: `pushChoicePoint()` captures
`core_.arena().mark()` and pushes onto the single `choiceStack_`; `backtrack()` pops and calls
`release(mark)`; `cut()` and `cutBarrier` index into that one stack (`vortex_vm.cpp`,
`vortex_vm.hpp`). A `ChoicePoint` carries `{cursor, altOp, zigzag::Mark, stackFrame, cutBarrier}`.
Because the mark is a checkpoint of one arena's dense vectors and the stack is global, "two OS
threads backtracking over one arena" is literally not expressible — the briefing states this and the
`Mark` doc confirms a mark names lengths in one arena's vectors.

Share-nothing resolves this cleanly: **each worker has its own arena and its own `choiceStack_`, so
mark/release/cut are already per-worker once the VM is per-worker.** No new synchronization is
needed for backtracking at all; it becomes correct by the same move that removes the GIL.
Concretely:

- Each worker's `VortexVM::choiceStack_` holds only choice points for cursors that worker owns. A
  `ChoicePoint::mark` is a `zigzag::Mark` of *that worker's* arena, so `release()` truncates only
  that worker's dense vectors — exactly the serial semantics that exist today, confined to one
  thread.

- `cut()` and `cutBarrier` keep their current meaning (an index into the owning worker's stack).
  Because a cursor never migrates workers, its cut barriers never cross a stack boundary.

- OR-parallelism in vprolog (trying alternative clauses on different workers) is expressible but is
  **explicitly deferred to a later stage** (see §6): it requires forking a worker arena at a choice
  point (a copy-on-write *of the overlay itself*, i.e. a cheap snapshot of
  `slots_`/`links_`/`content_` high-water state plus private continuation), and merging solutions
  deterministically. The architecture permits it because arenas are per-worker; the first milestones
  do not attempt it, to keep the global-choice-stack semantics intact while the share-nothing
  substrate is proven.

## 5. Deterministic result merge-back

Parallelism must not make a query's answer depend on scheduling. Determinism is enforced at the
merge boundary, not inside the workers:

- **Workers return results tagged with their submission index.** A VQL parallel scan or a VPL
  array-chunk fan-out assigns each unit of work a stable index (the chunk's position, the scan
  partition's rank order). The host collects results into an index-ordered buffer and concatenates
  in index order regardless of completion order. This is the same discipline `render::WorkerPool`
  already uses (indexed work `0..count-1`, caller sees all results on resume) applied to data
  instead of command buffers.

- **Promotion order is the submission order, on one thread.** When parallel work produces cells to
  persist, the host promotes them in deterministic submission order on the single promoting thread.
  Because `promote()` mints operations in the order it walks and `ops.nodes` is append-only, a fixed
  promotion order yields a byte-reproducible store. This is directly testable with the existing
  semantic-diff tool: `xudu-dump --section=ops STORE` before/after must match the single-threaded
  run (AGENTS.md names this as the format-change diff; here it doubles as the determinism gate).

- **Reductions (`fold`) use a fixed combine order.** A parallel `fold` computes per-chunk partials
  in parallel but combines them left-to-right in chunk-index order, so a non-associative or
  non-commutative combiner gives the same result as the serial fold. Associativity is not assumed.

- **Memoization under contention is made deterministic by confinement.** `getOrCreateMemoPin` /
  `recordMemo` / `lookupMemo` live on `VortexCore` and write the arena (`vortex_core.hpp`).
  Per-worker arenas mean per-worker memo tables: no contention, but also no cross-worker cache
  sharing. For the staged target that is the correct trade (correctness and determinism over a
  shared cache); a shared read-only memo table promoted into the base `Manifold` between barriers is
  a possible later optimization, not a stage-1 feature.

## 6. Staged migration plan

Each stage is independently shippable, leaves the tree green under `make -j$(nproc) check`
(format-check + lint + analyze) and `make -j$(nproc) test`, and respects the invariants: C++23, GNU
Make (no CMake), `sizeof(CompactOpNode) == 64` / 64 KiB Merkle pieces / append-only untouched,
config in `system://settings` not literals, and "new C++ must justify why it is not Vortex."
Threading primitives *are* such a justification — a worker pool, OS threads, and arena ownership are
allocator/raw-OS concerns, exactly the carve-out the rule names; the per-language *surface* that
exposes them stays in Vortex/VQL/VPL/vprolog, not in new C++.

### Stage 0 — Instrument and prove the bottleneck (gate: numbers, not opinion)

Add `SPDLOG_LEVEL`-gated timing on `stepScheduler` and the arena hot primitives under a
`vortex.sched` category. Produce a headless benchmark (reusing the `tests/` harness style) that
drives many cursors over one arena and reports where wall-clock goes. **Gate:** a committed
measurement showing the single shared arena is the serialization point, so later stages are judged
against a baseline rather than a belief.

### Stage 1 — Make the VM movable: extract a per-cursor execution context (gate: no behavior change)

Refactor so `VortexVM` state that is logically per-cursor (`cursorPC_`, the owning slice of
`choiceStack_`) is addressable per cursor, and `VortexHost` can hold more than one
`{ArenaManifold, VortexCore, VortexVM}` context. **Still single-threaded**: one worker context, run
on the caller, identical results. **Gate:** `make test` unchanged; `xudu-dump --section=ops` on
every sample store is byte-identical to before (fixtures not regenerated because nothing in
`CompactOpNode` or the spool changed — if a fixture moves, Stage 1 did something it must not).

### Stage 2 — Vortex worker pool, data-parallel VPL first (gate: speedup + determinism)

Add the Vortex worker pool (sibling of `render::WorkerPool`, in `apps/common/xanadu/vortex/`). Wire
**VPL's `map`/`filter`/`fold` only** to fan disjoint index ranges across workers, each with its own
arena over the shared base. VPL is first because it is the natural data-parallel fit and has no
cross-element arena writes. Worker count from `vortex.worker_threads` in `system://settings`; unset
⇒ probe hardware; 1 ⇒ caller-only (no threads started). **Gate:** a VPL array benchmark shows
scaling with worker count on a multi-core box; results are bit-identical to the serial path across
1..N workers (determinism harness from §5); `make check`/`make test` green; `fold`'s combine order
fixed and tested with a non-commutative combiner.

### Stage 3 — Parallel independent cursors (gate: isolation proven)

Let the scheduler place independent cursors (e.g. the `sys:gc` daemon cursor and a user query
cursor, or multiple independent VQL executions) on different workers, each with its own
arena/VM/choice stack (§4). No shared mutable arena remains on the hot path. **Gate:** a stress test
spawning many independent cursors shows no data races under the sanitizer build (`DEBUG=1` sanitizer
flags; the tree already builds a sanitizer config for fuzzers per AGENTS.md); determinism of
per-cursor results holds; promotion still single-threaded and `ops.nodes` reproducible.

### Stage 4 — Parallel VQL query execution (gate: deterministic results)

Parallelize VQL scans/joins over the (multi-store) base manifold: partition the scan, run partitions
on workers over private overlays, merge in index order (§5). **Gate:** representative VQL queries
return identical result sets and identical ordering to the serial compiler across worker counts;
`vquery`/`vqueryc` headless runs match; no persistent cross-store link is silently created
(federation still goes through the documented single-arena multi-store path, now per-worker for
reads).

### Stage 5 — vprolog OR/AND-parallel search (gate: logic equivalence, opt-in)

Only after Stages 1–4 are proven: allow a worker to fork its overlay at a choice point to explore an
alternative clause in parallel (OR-parallelism), merging solutions in a deterministic order keyed by
submission index (the lowest-index branch wins "first solution"), with cut semantics preserved by
confining a cut to its originating branch. This is the hardest stage because it collides with the
choice-stack/mark model; it is gated behind a `system://settings` flag and defaults off. **Gate:**
`vprolog` produces the same solution set and the same first-solution as the serial engine for a
suite of programs including cut and negation; no arena mark/release crosses a worker boundary
(verified by the sanitizer build and by `zigzag.arena` leak logging).

### Cross-cutting gates (every stage)

- `make -j$(nproc) format-check lint analyze` clean (clang-format 19; the config-harmony check).
- `make -j$(nproc) test` green, including the four gtest binaries; the swarm netns step's `veth`
  caveat is unrelated.
- No new magic numbers: every tunable (`vortex.worker_threads`, any fan-out threshold, the vprolog
  parallel-search flag) is a `defaultSettingSpecs()` entry read via `getSetting`, mirroring
  `vortex.cycle_budget`.
- No change to `CompactOpNode`, the spool writer, Merkle piece size, or append-only ordering; if a
  stage would touch them it is out of scope and must be redesigned.
- Determinism is a test, not a claim: `xudu-dump --section=ops` reproducibility and
  serial-vs-parallel result equality are checked in CI-runnable form.

## Component diagram

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
