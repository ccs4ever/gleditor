# The multi-core problem as a distributed system

The owner's fear is precise and correct: `VortexCore` holds exactly one `ArenaManifold &arena_`
(`vortex/vortex_core.hpp`), and nearly every opcode — `link()`, `value()`, `splice()`, `newCell()`,
`breakLink()`, `pin()`, `collectGarbage()`, every memo record and lookup — mutates it. A naive "wrap
the arena in a mutex" design serialises on that lock on almost every instruction, which is a Python
GIL in all but name. This section does not argue about locks. It argues that the *shape* of the
Vortex state — an append-only immutable op log underneath a copy-on-write ephemeral arena, with
`promote()` as the one write back — is already the shape of a well-behaved distributed system, and
that the way out of the GIL is to stop treating the arena as one shared mutable heap and start
treating per-worker arenas as share-nothing nodes that exchange immutable messages.

## 1. Share-nothing arenas as nodes exchanging messages

### Why the arena is the node boundary, not the lock boundary

`ArenaManifold` is documented as "dense vectors only, no ops, dies with its owner" and is explicitly
single-owner scratch space. That is not a limitation to be patched; it is a node. The distributed
reframing is:

- A **node** is one `ArenaManifold` plus the `VortexCore`/`VortexVM` bound to it. It owns its dense
  vectors (`slots_`, `links_`, `content_`, `scratch_`, `trail_`), its choice-point `floors_`, and —
  critically — nothing anyone else can see.
- The **shared immutable substrate** is the base `Manifold` and the `Store`'s `ops.nodes` the arena
  overlays. Reads of a cell the arena has not shadowed "fall through to the base" (the overlay
  comment in `arena_manifold.hpp`), and the base "is never touched." Many nodes can read one base
  concurrently with no coordination because the base is immutable by construction.
- A **message** is an immutable, content-addressed value: a `CellRef` into the shared base, a
  `PrimediaSpan` (which names bytes already spooled), a `CellValue`
  (`variant<string,double,int64,bool>`), or — for results that must cross back to persistence — the
  `Promoted{version, cells}` that `promote()` returns.

The move from "one arena, N green cursors" to "N arenas, each with its own cursors" turns the GIL
contention point into a fan-out/fan-in boundary. Today's cooperative scheduler,
`VortexVM::stepScheduler()` (`vortex_vm.cpp`), is a plain `for` loop over `activeCursors()` calling
`step(cursor)` on one thread. The share-nothing design keeps that loop *inside* each node and makes
the parallelism be between nodes, where there is no shared mutable state to contend on.

### Mapping the classic models onto the three frontends

The point of naming the models is that each frontend has a different natural shape, and forcing one
model onto all three is how a threading design acquires accidental global state.

- **Fork-join / MapReduce — VPL, and the stdlib `map`/`filter`/`fold`.** This is the cleanest fit
  and the one to build first. `VortexStdLib::map`/`filter`/`fold` (`vortex_stdlib.cpp:2212+`)
  already take a `function_ref` and run as pure ranges pipelines over a rank: `map` transforms each
  `render(cell)`, `filter` renders once then predicates, `fold` is a `std::ranges::fold_left`. The
  element function is `render` + an optional `call(fnOp, ...)`. The fork is "partition the rank into
  K sub-ranges"; the map is "each worker renders and transforms its sub-range against a read-only
  base"; the join is "concatenate the result cells in rank order." Determinism is preserved because
  the reduction order is fixed by rank position, not by completion order. The one hazard is that the
  per-element `fnOp` today runs through `call()`, which `spawnCursor`s into the *shared* arena
  (below); a parallel `map` requires the element function to run in the worker's own node.
- **Dataflow — VQL.** A query is a pipeline of scans, filters, joins and projections over the
  manifold or the `MultiStoreCoordinator`'s composite arena. Each stage is a transform over a stream
  of `CellRef`s; stages are independent as long as each reads immutable base cells. This maps onto a
  dataflow graph where edges carry batches of `CellRef`s and nodes are stateless w.r.t. each other.
  Parallel scans and the probe side of a hash join are embarrassingly parallel reads; the build side
  of a join is a fork-join barrier.
- **Actors / CSP — the cursor scheduler and `call`.** A cursor is already an actor in spirit: it has
  a private program counter (`cursorPC_`) and advances by `step`. The distributed reframing makes
  each worker thread an actor owning a mailbox of cursors and its own arena; cursors do not share
  memory across actors, they exchange messages. `std:functional`'s `call` and the system GC cursor
  (`vortex_host.cpp:86`, `"sys:gc"`) become actors with clear ownership rather than co-tenants of
  one heap.
- **AND/OR parallelism (a specialisation of fork-join) — vprolog.** This is the hardest and is
  treated in §2 and §4, because it collides with the single global `choiceStack_` and the global
  arena `mark()`/`release()`.

## 2. Consistency and determinism: immutability is the asset

### Why parallel reads are already safe

The op log is append-only and immutable: `CompactOpNode` is 64 bytes, sealed segments are
`PROT_READ`, and content is addressed by span, not copied. The arena overlay's contract
(`arena_manifold.hpp`) is that the base `Manifold` "is never touched" and "a shadow copies a cell,
not a promise about one." These two facts compose into the central guarantee: **any number of worker
nodes may read the same base concurrently without a lock, because nothing they can do mutates it.**
A worker that needs to change a cell shadows it into its own arena (copy-on-write at cell
granularity), leaving the base — and therefore every other worker's view — untouched. This is
snapshot isolation handed to us for free: each node evaluates against a fixed, immutable snapshot
identified by a `MicroversionId`.

Content addressing strengthens this. "Shared addresses, rather than equal strings, identify shared
content" (README). Two workers that independently arrive at the same content converge on the same
`PrimediaSpan` address, so equality and transclusion-discovery (the Spanfilade) are stable across
workers with no coordination. Determinism of a *read-only* parallel query therefore reduces to one
rule: fix the reduction/merge order by a deterministic key (rank position, `CellRef`, or published
`GlobalOpRef`) rather than by worker completion order. Everything upstream of the merge is pure.

### `promote()` is the commit/merge point

`promote(store, parent, from, root, budget)` (`arena_manifold.hpp`) is the only road from the
ephemeral side to persistence, and it is exactly a distributed commit:

- **It is a graph walk, not a bookkeeping problem.** It promotes only what is reachable from `root`
  along any dimension — "promote the answer, not the search." A failed branch's cells are
  unreachable from an answer and are never written. This is the commit of a transaction's write set,
  computed by reachability.
- **It maps refs rather than trusting them.** Every arena ref is `ephemeralBit`-tagged;
  `Store::setLink()` and `Manifold::applyStructure()` refuse an ephemeral ref. A promotion that
  forgot to translate a ref is refused, not silently persisted. This is the validation half of a
  commit: the write set is rewritten into the authoritative namespace, and anything that failed to
  rebase is rejected by pre-existing R8 machinery.
- **It takes an explicit `parent` `MicroversionId`.** The result `Promoted{version, cells}` names a
  new microversion whose parent is the snapshot the work was done against. This is the
  version-vector edge of a commit.

So the system already has the two primitives a distributed design needs: an immutable snapshot to
fork from (the base at a `MicroversionId`) and an atomic, validated merge back (`promote()`
producing a child microversion).

### Ordering, idempotency, and conflict when many workers promote

The honest news is that `promote()` is *per worker to its own `root`* and the merge back to a single
store is not yet concurrent. Here is what the model implies when it becomes so:

- **Ordering.** Promotions must be linearised against the store's op log, because `ops.nodes` is
  append-only and a child microversion names a specific parent. The natural design is a
  single-writer commit stage (one thread owns `Store::save()` /
  `SegmentedOpsSpool::writeSegmentFile` — the briefing notes "one writer"), with workers queueing
  `Promoted` write sets. The parallelism is in the *evaluation*, which is the expensive part;
  serialising the *commit* costs little because a promotion writes at most `budget.maxOps`
  operations and the arena has already done the search.
- **Idempotency.** Content addressing makes re-promoting the same answer cheap and safe: identical
  constructed bytes spool to the same permascroll address, and an unchanged cell promotes to an
  equal operation. A retried promotion (after a lost commit) is therefore close to idempotent at the
  content layer; what is not idempotent is the *microversion name*, which must be minted once by the
  commit stage, not by the worker. This argues for workers producing content-addressed write sets
  and the commit stage assigning the version — classic separation of "prepare" from "commit."
- **Conflict handling.** Because editing an earlier state *branches* rather than overwrites (README:
  "Editing an earlier state branches rather than discarding its descendants"), two workers promoting
  from the same parent do not destructively conflict — they produce two children, i.e. a fork in
  hypertime. This is a merge-free CRDT-like posture for the *structure*: concurrent writes never
  lose data, they create branches. A true semantic merge (reconciling two children into one) is a
  higher-level operation the owner may or may not want; the substrate's default is "keep both,"
  which is the safe default for a hypertext whose whole point is that nothing is destroyed. The
  conflict that *does* need policy is the application-level question "which branch is current," and
  that is a deliberate decision, not a race.

The determinism caveat vprolog forces: with AND/OR parallelism, the *set* of solutions is
order-independent, but the *order* solutions are reported in is not, unless imposed. `solveLogic`
(`vortex_host.hpp`) returns a `vector<LogicSolution>` with a `maxSolutions` bound; a parallel search
must either sort by a deterministic solution key before returning or document that solution order is
nondeterministic under parallelism. The global `choiceStack_` and the global arena `mark()`/
`release()` (a mark names a point in the arena's dense vectors — inherently serial) mean the current
engine cannot express concurrent backtracking at all; per-node arenas with per-node choice stacks
are the prerequisite, and that is precisely the share-nothing move.

## 3. Data partitioning for parallel query

Two axes present themselves, and they compose.

### Partition by store (coarse, already structural)

`MultiStoreCoordinator` composes many stores "in one arena" via a `d.stores` rank
(`multi_store.hpp`), attaching foreign cells as proxies. Distributed thinking says: don't compose
them in one arena for a parallel query — give each store its own node and treat the composite as a
*federation*. The arena's own federation vocabulary already models this: `attach(Space)`,
`proxyFor(space, foreignIndex)`, `resolveForeign()`, `bindSharedIdentities()`/
`bindDimensionsByNameMatch()` (`arena_manifold.hpp`, §5.7). A cross-store query becomes:

1. fan out the per-store predicate to each store's node (each reads its own immutable base);
1. each node returns matches as content-addressed `PrimediaSpan`s / `GlobalOpRef`s;
1. a coordinator joins on shared identity (`bindSharedIdentities()` groups dimensions sharing a
   published `GlobalOpRef`) and projects proxies into a result arena.

This is a MapReduce over stores where the map keys are global identities. It parallelises almost
perfectly because the only shared thing read is each store's own immutable `ops.nodes`, and the join
key is content-addressed, so it needs no distributed lock. The README's own caveat holds: this does
*not* create persistent cross-store cell links automatically; the composition is an ephemeral,
per-query view, which is exactly why it is safe to shard.

### Partition by rank (fine, within one store)

A rank is a doubly-linked chain along one dimension (a cell has at most one posward and one negward
neighbour per dimension). The stdlib already walks ranks as C++ ranges (`rank(arena, head, dim)`).
To parallelise a scan/`map`/`filter` over a long rank:

- **Range partition** the rank into K contiguous slices by walking it once to find K boundary cells
  (O(N) prelude, or O(log N + K) if a rank index exists), then hand each worker a `[begin, end)`
  sub-range. Each worker reads base cells and writes only into its own arena. Results are chained
  back in rank order (`chainCells`), preserving determinism.
- **Hash/round-robin partition** when per-element cost is skewed (e.g. `fnOp` is a heavy `call`):
  assign cells to workers by `CellRef mod K`. This loses rank locality but balances load; the merge
  re-sorts by original rank position.

The CSR layout matters here: a worker that only *reads* a sub-range touches `slots_`/`links_`/
`content_` of the shared base, which is immutable, so no false sharing on writes occurs. The moment
a worker *writes* (shadows a cell), it writes into its own arena's vectors, so there is no
cross-worker cache-line contention on the hot structural vectors. This is why the share-nothing
arena is not just a correctness story but a performance one: it eliminates the write-write and most
read-write sharing that a single shared `ArenaManifold` would thrash on.

### What does not partition cleanly

Memoization (`getOrCreateMemoPin`/`recordMemo`/`lookupMemo`, `vortex_core.hpp`) is global mutable
state keyed by op and inputs. Under partitioning it must become either per-node (each worker
memoises privately, losing cross-worker reuse but staying lock-free) or a concurrent read-mostly
cache behind the base (shared reads, serialised writes at a commit-like point). The
pin-and-cache-rank implementation lives *in the arena* today, so a shared memo cache cannot live in
a per-worker arena; it would need to be promoted to a shared immutable-ish structure. The cheap and
honest first answer is per-node memoisation, measured, before building a concurrent cache.

## 4. Failure and cancellation of a cursor/worker

The arena's choice-point machinery gives failure and cancellation a clean, already-implemented local
semantics, and the share-nothing design extends it across workers.

- **Local failure/undo is truncation.** `mark()` reads seven lengths and allocates nothing;
  `release(mark)` "replay its trail tail in reverse, then truncate" — cells minted under the mark
  are "gone, not garbage." `discard(mark)` keeps the bindings but drops the ability to retry (a
  cut). `backtrack()` (`vortex_vm.cpp`) is `release`; `cut()` is `discard`. So a worker that fails a
  branch does not corrupt anything shared — it truncates its own dense vectors back to a mark. This
  is a node-local transaction abort, and it is O(trail), not O(cells).
- **Cancellation is dropping the node.** Because a worker owns its arena and the arena "dies with
  its owner," cancelling a worker is destroying its `ArenaManifold`: no cleanup of shared state is
  needed, because it never wrote any. A cancelled parallel `map` worker simply discards its partial
  result arena; the base is untouched and the other workers are unaffected. This is the single
  biggest safety dividend of share-nothing: cancellation cannot leave shared structure half-written.
- **The leak guard still fires across the boundary.** `ephemeralBit` means a ref that escaped a
  cancelled worker into a persistent write is refused by `Store::setLink()` / `applyStructure()`. A
  partially-promoted answer cannot slip through: `promote()` is all-or-nothing against its reachable
  subgraph and returns "nothing, having written nothing" when the subgraph exceeds `budget` or
  `root` is not a cell of the arena.
- **Timeouts and budgets are first-class.** `VortexVM::run(cursor, maxCycles)` already bounds work;
  `PromotionBudget{maxOps}` bounds a commit; `allocationLimit_` bounds an arena. A worker that
  exceeds its budget fails locally and is cancelled by dropping its node — no partial global effect.
  The owner's "runaway evaluation cannot write an unbounded number of operations" guarantee
  (`arena_manifold.hpp`) is preserved per worker and composes.

The hard case, again, is vprolog AND/OR parallelism. OR-parallel branches each want their own
`mark()`/`release()` over their own arena — fine once arenas are per-node. AND-parallel conjuncts
that share bindings need those bindings to be either immutable (bind-once, then read) or reconciled
at a join; the WAM-style conditional trailing the arena already does (`trail()` only saves cells
older than the innermost mark) is the right local primitive, but cross-worker binding sharing is
where a naive design reintroduces a shared mutable trail. The safe staging is: OR-parallelism first
(independent branches, independent arenas, merge the solution sets by a deterministic key),
AND-parallelism later and only where conjuncts are provably independent.

## 5. Where distributed thinking is overkill vs. where it de-risks the GIL

Distributed thinking earns its complexity exactly where the shared arena is the bottleneck and the
work is read-heavy against the immutable base. It is overkill where the work is small, inherently
sequential, or dominated by the serial commit.

Genuinely de-risks the GIL (do this):

- **VPL data-parallel `map`/`filter`/`fold` over large ranks.** Pure, read-mostly, deterministic
  merge, `function_ref` already in place. Highest ratio of speedup to risk. This is where "stop
  sharing one arena" directly removes the GIL for the common heavy workload.
- **VQL parallel scans and the probe side of joins over long ranks or many stores.** Reads immutable
  bases; partition by rank or by store; join on content-addressed identity. The federation
  vocabulary is already there.
- **Multi-store queries.** One node per store, fan-out/fan-in, join by `GlobalOpRef`. The stores are
  separate immutable substrates, so this is near-linear and low-risk.

Overkill or premature (don't do this first):

- **A distributed/concurrent shared memo cache.** Start per-node; measure cross-worker reuse loss
  before paying for a concurrent cache. Global mutable memo state is the second-easiest way to
  reintroduce a GIL after the arena itself.
- **AND-parallel vprolog with shared bindings.** High coordination cost, nondeterministic solution
  order, collides with the global `choiceStack_` and global arena mark. OR-parallelism captures most
  of the benefit at a fraction of the risk; defer AND-parallelism until a measured need and provable
  conjunct independence.
- **Parallelising short cursors / interactive single-document edits.** The GIL fear is about
  throughput on big batch queries, not latency on a keystroke. A single short `step` sequence over
  one small arena is faster sequentially than it is to fork, fan-in, and merge. Keep the cooperative
  single-thread scheduler for the interactive path; add thread pools only for explicitly parallel
  frontend operators.
- **Treating the commit as parallel.** The op log is append-only with one writer; the win is
  parallel *evaluation* feeding a serial *commit*. Trying to parallelise `promote()`'s write to one
  store buys little (a promotion is bounded by `budget.maxOps`) and risks the append-only and
  Merkle-piece invariants.

The through-line: the arena's immutability-plus-overlay-plus-`promote()` design is already a
share-nothing, snapshot-isolated, content-addressed distributed substrate. The GIL is an artifact of
*one* arena being shared, not of the model. Give each worker its own arena over the same immutable
base, keep the merge at the one place that is already the single writer, and the GIL-analogue
disappears for exactly the read-heavy parallel workloads (VPL arrays, VQL scans, multi-store
queries) where the owner fears it most — while the interactive and logic-backtracking paths, which
do not benefit, are left on the proven cooperative scheduler until there is measured reason to
change them.
