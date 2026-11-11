# r9-codebase: Round-9 codebase verification of `design/vagent-metaprogramming.md` v1.8

**Orchestrator summary (≤200 words):** Every citation the v1.8 delta touched checks out —
`mesh_actor.cpp:156`,`:199`,`:215-216`,`:260-269`,`:467-468`,`:659-662`; `mesh_actor.hpp:102-106`,
`:113-116`; `mesh_jobs.cpp:303-318`; `mesh_value.cpp:227-228`,`:241`,`:512-545`;
`mesh_host.cpp:484-530`; `publication.cpp:611-617`,`:664-672`,`:981-1008`,`:1030+`;
`manifold.cpp:427-442`; `arena_manifold.cpp:274-287`,`:1347-1377`,`:1934-1954`;
`vortex_vm.cpp:98-100`,`:102-114`; `mesh_evaluator.cpp:723-735`;
`mesh_actor_resume_test.cpp:125`. The adapter-store mechanism is consistent with today's
ownership: `Actor` is private (`mesh_actor.hpp:194`), `resume` keeps its internally built
`ReceivedValue` (`mesh_actor.cpp:260-269` → `unique_ptr<Store>` at `mesh_value.hpp:102`),
`checkpointView`/`Executor` expose no store, and `attach(Space)` installs `base_`/`store_` when
unset (`arena_manifold.cpp:274-287`). "Position-stable" is mechanically exact:
`applyOpsSegment` rewrites only `record.op.span.scroll` (`publication.cpp:1074-1087`) and
`adoptOpRecords`' topo-adoption preserves the sealed order on a linear parent chain
(`store.cpp:3836-3940`). One new defect: genesis (a)'s `walk-max = E` conflates the op-band
boundary with the last cell-bearing position. Counts: **0 BLOCKING, 1 MAJOR, 3 MINOR, 4 NITS**.
**VERDICT: NOT CONVERGED.**

## Citation verification — v1.8 delta cites

All verified against the file at the cited lines:

- `mesh_actor.cpp:156` — `actor->context = std::make_unique<WorkerContext>()` inside `spawn`
  (fresh arena, no store). Supports "creates no store"; the signature (`:119-122`,
  `mesh_actor.hpp:132-135`) is what supports "accepts no store". See NIT-1.
- `mesh_actor.cpp:199` — `continuation->programRef != request.programRef`. Exact.
- `mesh_actor.cpp:215-216` — `grant->programRef != continuation->programRef`,
  `grant->baseRef != checkpoint.contentHash`. Exact; the programRef and lineage pins.
- `mesh_actor.cpp:260-269` — `receiveValue` at `:260-261` builds `verified` (owning
  `unique_ptr<Store>`/`Manifold`, `mesh_value.hpp:98-105`); `actor->checkpoint` takes it at
  `:269`. `Actor` is private (`mesh_actor.hpp:194`). "builds internally, keeps private" exact.
- `mesh_actor.cpp:467-468` — `mailbox.front()` `:467`, `pop_front()` `:468`. Exact; my r8 nit
  applied verbatim.
- `mesh_actor.cpp:659-662` — `pending()` returns `mailbox.size()`. Exact. `:672` carries the
  `!actor->mailbox.empty()` refusal inside `checkpointView`. Exact.
- `mesh_actor.hpp:102-106` — `ActorCheckpointView{continuation, const ArenaManifold &arena,
  root}` — no store member. Exact. `:113-116` — `Authorizer`/`Executor` typedefs; `Executor`
  takes `WorkerContext &`, which holds `arena` and no store. Exact.
- `mesh_jobs.cpp:303-318` — `job.base` → `fetchValue` → `store = base->store.get()` (`:314`),
  `evaluate(…, base->manifold.get(), store)` (`:319-320`). The `job.base` shared-store pattern.
  `:303` is the preceding `return` — same harmless off-by-one r8 accepted. See NIT-4.
- `mesh_value.cpp:227-228` — `sealLocalSpool(store, keys, …)` called with default
  `opsAlreadySealed = 0` (no fourth-positional arg present). Exact. `:241` —
  `{*seed.history.opsSegment}` handed to `publish`. Exact.
- `mesh_value.cpp:512-545` — `mergeValues` signature through `return out`. Exact range; the
  claim *about* it is partly wrong — see MINOR-2.
- `mesh_host.cpp:484-530` — `commit`: `store = base->store.get()` (`:507`) or fresh `Store`
  (`:509-510`), `mergeValues` (`:518-519`), `store->save` (`:529`). Exact range.
- `publication.cpp:611-617` — `opsTotal`, `opsAlreadySealed` guard, `hasNewOps`,
  `sealableOps(store, opsAlreadySealed)`. Exact. `:664-672` — ops segment `at=opsAlreadySealed`
  (`:666`), `length=opsTotal-opsAlreadySealed` (`:667`), `sealed.opsSegment=segment` (`:672`);
  with `opsAlreadySealed=0` this is `at=0,length=opsTotal` — the `[0..opsTotal)` claim. Exact.
- `publication.cpp:981-1008`,`:1030+` — `:981-1008` is `sealableOps`'s scroll-table collection
  loop (`named` map + `scrollKey`); serialization at `:1010-1017` sits just below the range;
  `applyOpsSegment` begins `:1030` and the `ScrollId` remap lives at `:1048-1087`
  (`record.op.span.scroll = at->second` at `:1086`). The claim "a segment's scroll table remaps
  into the receiving store's `ScrollId` space" pairs both ends correctly. See NIT-2.
- `manifold.cpp:427-442` — `dense` (`:427`), `slots.push_back{…, .birthOp = opIndex, …}`
  (`:430-441`), `byRef.emplace(opIndex, dense)` (`:442`) — the `applyStructure` MakeCell arm the
  rebuild drives. Exact.
- `arena_manifold.cpp:274-287` — `attach(Space)`: `base_ = space.manifold` when `base_ ==
  nullptr` (`:279-281`), `store_ = space.store` when `store_ == nullptr` (`:282-284`),
  `space.reader = space.store` default (`:285-287`). "installs `base_`/`store_` when unset"
  exact — and the when-unset guard is load-bearing: `WorkerContext`'s arena ctor already sets
  `base_` (`arena_manifold.cpp:23-28`), so at `resume` an adapter `attach` lands only `store_`,
  matching the doc's "supplies … or verifies" pair.
- `arena_manifold.cpp:1347-1377` — `displacedUs`/`displacedIt` cleared via `setOneSide(…,
  noCell)` (`:1364-1373`) before the new edge (`:1374-1377`). `link` eviction exact.
- `arena_manifold.cpp:1934-1954` — Int64 verbatim copy (`:1934-1936`), deferred `OpHandle`
  (`:1937-1946`), `OpHandle` → `makeOpHandle(isEphemeral(target) ? real.at(target) : target)`
  (`:1947-1951`), `ForeignOpHandle` → `makeForeignOpHandle` (`:1952-1954`). "Int64 verbatim,
  handle kinds translated" exact.
- `vortex_vm.cpp:98-100` — `setOpcodeKind` exact signature. `:102-114` — `getOpcodeKind`:
  map-membership check first (`:103-105`), `asInt64` decode fallback (`:107-113`). Supports the
  moved rationale: membership is what P15 needs; decode already answers kind.
- `mesh_evaluator.cpp:723-735` — `d.mesh.relocation` dim mint (`:723`), `setValueBits(cell,
  OpHandle, target)` (`:733-734`), `builder.link(cell, relocation, makeCell("cell"))` (`:735`).
  Both halves of the relocation-handle test. Exact.
- `mesh_actor_resume_test.cpp:125` — `xanadu::Store store;` fresh default store at exactly
  `:125`, `publishValue(store, {}, snapshot->arena, …)` at `:128`. Exact — and for the
  actor-checkpoint ceremony it is the only publish call site; `mesh_jobs.cpp:322` publishes in
  the *job* flow (into the lineage store when `job.base` exists). See NIT-3.
- Also re-verified in the delta's wake: `vortex_core.cpp:30-36` (`initGenesis` adopts
  `*base->home()`), `manifold.cpp:450-451` (`home_` = first MakeCell's opIndex),
  `vortex_vm.cpp:157-160` (`rankTail(home,d.cursors) → cursor`), `vortex_vm.cpp:206`
  (cursor→frame `d.stack` read), `vortex_core.cpp:113-126` (rank membership `rankTail +d →
  member`), `vortex_core.cpp:119-127` (`exportSymbol` mints `makeCell(name)` + `+d.values →
  entryOp`, no `+d.name`), `vortex_core.cpp:196-229` (`writeValue` covers
  Double/Int64/Bool/None — no handle kind), `vortex_core.cpp:354-414` (`wingOf`/`bindInput`/
  `bindOutput` — `d.grab`/`d.step` wings; `d.spin` is the op-sequencing dim,
  `vortex_vm.cpp:92`), `masterOf` exists (`vortex_core.cpp:344`), `mesh_value.cpp:485-501`
  (importValue demotes unmarked `OpHandle`→`None` `:485-490`, re-kinds marked via
  `local.at(*target)` `:494-500`), `arena_manifold.cpp:1864-1867` (`real.emplace(arena, arena)`
  — persistent refs identity-map through promote), `arena_manifold.cpp:1489-1490`,
  `manifold.hpp:295` (`birthOp == this CellRef`), `dim_vector.hpp:23` (`noCell==0`),
  `arena_manifold.hpp:468-470` (`refOf = ephemeralBit|dense`), spec
  `vagent-agent-language.md:425-428` (§4.4 per-arena-load wording quoted correctly; U-A14 is
  the shared-base alternative) and `:1670` (P4 `wake(pid)` row).

## R8-A verification — the adapter-store ownership is consistent with today's code

- `spawn()`'s signature takes identity/request/wallNow — no store (`mesh_actor.cpp:119-122`);
  its body mints a fresh `WorkerContext` (`:156`) whose `ArenaManifold` starts with
  `base_`/`store_` null (`arena_manifold.cpp:23-28`). The adapter *can* already
  `attach(Space{.manifold, .store})` at spawn — `multi_store.cpp:225` and
  `quoted_structure.cpp:525` are the precedent — so the genuinely owed seam is the `resume`
  arm, which is exactly what the doc names.
- `resume()` builds the `ReceivedValue` internally (`receiveValue` → `restorePublication`'s
  fresh `Store`, `publication.cpp:1127-1131`) and keeps it in the private `Actor`
  (`mesh_actor.cpp:269`, `mesh_actor.hpp:194`); `setDefaultSpanReader` installs the store only
  as a const `SpanReader` (`:273`) — the adapter cannot reach it mutably. `checkpointView`
  yields `{continuation, const ArenaManifold&, root}` and `Executor` gets `WorkerContext&` —
  neither hands out a `Store`. "The platform holds no store accessor" is exact: fresh-store
  publication is unpreventable today and `mesh_actor_resume_test.cpp:125` is the only publish
  the ceremony exercises — fail-closed *by detection* is the honest framing.
- "Position-stable, not byte-identical" verified mechanically: `applyOpsSegment` rewrites only
  `record.op.span.scroll` through the per-segment table (`publication.cpp:1048-1087`);
  parent/source/produces "travel as they were written" (`:1075-1078`). `adoptOpRecords`
  dedupes then Kahn-orders on min-`produces` (`store.cpp:3836-3940`), which on a linear
  parent-chained spool emits the sealed sequence verbatim — positions `1..E` identical, bytes
  not. The `storeId` caveat (`:607-609`) is also real: `restorePublication` rebuilds
  `Store(readerPermascroll, pub.storeId)` (`publication.cpp:1127-1128`), so restored stores do
  publish under one store identity.
- `ActorContinuation`/`grant` pins verified: `:199` (request↔continuation programRef), `:215`
  (grant↔continuation programRef), `:216` (baseRef↔contentHash).

## New findings (v1.8 deltas only)

### Finding 1 — MAJOR

- **Location:** :1732-1736 (genesis step (a)); the `E` definition at :1389 and :616-617
  ("the image occupies persistent positions `1..E`").
- **Problem:** "every cell the `program()` structural walk reaches must hold a persistent ref
  `≤ E`, and the walk's maximum must equal `E`" conflates the op-band boundary with the last
  cell-bearing position. `E` is "the image prefix length" — the count of image ops, since
  positions in the `[image 1..E][state >E]` band are op indices. A program cell's persistent
  ref is its *birth op* index (`manifold.hpp:295`, `manifold.cpp:434,442`), so the walk's
  maximum is the index of the last **cell-producing** op — strictly less than `E` whenever the
  image's tail ops are links or values, which is the normal emission shape (`exportSymbol`
  ends with two link ops, `vortex_core.cpp:119-126`; `bindInput`/`bindOutput` append link ops,
  `vortex_core.cpp:380-414`; `ensureDimension` ends with a rank link). Read literally —
  `walk-max == store.opCount()`, the natural implementation — the positional assertion refuses
  the honest artifact on every resume: genesis fails closed unconditionally and `learn` can
  never admit. If the toolchain happens to mint a walked anchor cell last today, the check
  stands on an emission invariant the document never states.
- **Evidence:** `manifold.cpp:427-442` (`birthOp = opIndex`, `byRef.emplace` only for
  cell-bearing ops — non-cell ops `emplace` an *alias* to the subject's dense, e.g.
  `manifold.cpp:525,541,562`, so a cell's canonical ref is its MakeCell index only);
  `publication.cpp:611` (`opsTotal = store.opCount()` — the natural "prefix length" source).
- **Concrete fix:** either record the extent as the last *cell-bearing* position — "the image
  prefix's last cell-bearing op index, recorded on the `programRef` bind record" — keeping the
  `≤ E` arm as the upper bound, or state the emission invariant the equality needs ("the
  image's final op is a MakeCell whose cell the `program()` walk reaches"). As written the
  comparand mixes units: op-position `E` vs cell-ref max.

### Finding 2 — MINOR

- **Location:** :1420-1422 (§6.6, the "presumes the continuing store" parenthesis).
- **Problem:** "link ops emit non-ephemeral targets verbatim, `arena_manifold.cpp:1950`,
  `:1996`" — the cited lines are `OpHandle`-kind *value* emission (`makeOpHandle` at
  `:1947-1951`, `setValue` at `:1992-1996`), not link ops. The actual link emission is
  `store.setLink` (~`:2035`), which translates both ends through `real` — where persistent
  refs pass through verbatim only because `real.emplace(arena, arena)` identity-maps them
  (`arena_manifold.cpp:1864-1867`). The claim is substantively true (persistent ends do ship
  verbatim, which is why a fresh-store publish writes ops naming positions that never
  existed) but labels and cites land on the wrong op class.
- **Concrete fix:** "link and handle-value ops emit non-ephemeral targets verbatim
  (`arena_manifold.cpp:1864-1867`,`:1947-1954`,`:2035`)" — or name the identity-map explicitly.

### Finding 3 — MINOR

- **Location:** :1737-1738 (genesis (a)'s merged-lineage arm).
- **Problem:** "`mergeValues` promotes imported twins into a fresh store in discovery order,
  `mesh_value.cpp:512-545` — `mesh_host.cpp:484-530` publishes it" has three imprecisions:
  (a) "discovery order" — `mergeValues` sorts by `(contentHash, authorKey, parent)`
  (`mesh_value.cpp:519-523`), a deterministic order, not arrival/discovery order;
  (b) "into a fresh store" — the destination is caller-supplied: `commit` uses `base->store`
  when `job.base` exists (`mesh_host.cpp:507`) and a fresh `Store` only in the no-base arm
  (`:509-510`); (c) "publishes it" — the cited block merges and `store->save()`s (`:529`),
  never calling `publishValue`. The fail-closed conclusion is right for the fresh arm; the
  mechanism description mislabels the ordering and the destination.
- **Concrete fix:** "`mergeValues` promotes imported twins into its destination store in
  `(contentHash, authorKey, parent)` order (`mesh_value.cpp:519-523`) — a fresh destination
  (`mesh_host.cpp:509-510`,`:518-529`) carries no prefix and fails closed".

### Finding 4 — MINOR

- **Location:** :1050 (`plan_named` builtin-table row, itself rewritten this round) vs
  :1520-1524 (the new declared-handler gap).
- **Problem:** the row says `plan_named` "matches every clause `+d.name` label verbatim,
  event-handler clauses included (§6.7)", while the new text at :1521-1522 states a
  kernel-emitted handler clause mints no `+d.name` under spec V15 (`on Event [guard] { }`) —
  so `plan_named` can never match a *declared* handler. "Event-handler clauses included" is
  only true of *learned* handler clauses (`learn_handler`/`graft_handler` mint named clauses,
  :1523-1524) and now reads as if declared handlers are nameable — the opposite of the gap
  the same delta introduces.
- **Concrete fix:** "matches every clause `+d.name` label verbatim — learned handler clauses
  included; declared handlers carry no `+d.name` (§6.6, spec V15)" or drop the parenthetical.

### Nits

- **NIT-1** (:1372-1373, :2298, :2327, :2336): `mesh_actor.cpp:156` — the fresh
  `WorkerContext` mint — supports "spawn *creates* no store"; "*accepts* no store" is the
  signature (`mesh_actor.cpp:119-122`, `mesh_actor.hpp:132-135`). Harmless; cite the
  signature if touched again.
- **NIT-2** (:604, :1369-1370, :2805): `publication.cpp:981-1008` cited for the remap is
  `sealableOps`'s scroll-table *collection* loop; the table serializes at `:1010-1017` and
  the remap proper is `:1048-1087` (covered by the paired `:1030+`). Paired cite is
  defensible; `:1010-1017` would be tighter for the table itself.
- **NIT-3** (:1418-1419): "the only publish path exercised today" is true for the
  actor-checkpoint ceremony but not codebase-wide — `mesh_jobs.cpp:322` publishes in the job
  flow (into the lineage store when `job.base` exists). Scope it: "the only publish path the
  actor ceremony exercises today".
- **NIT-4** (:606, :1371-1372, :1744-1745): `mesh_jobs.cpp:303-318` includes `:303`, the
  preceding `return`; the `job.base` block proper is `:304-318`. Accepted in r8 as harmless.

## Non-findings verified (delta claims that hold)

- The (d)→(e) hand-off is coherent: `reify` regenerates the copy's `+d.refs` (§6.1), the
  copies hang off program-side install anchors the publish-time walk prunes, so a head-only
  sweep cannot reach them — (d) handing (e) its install set is the honest dependency. The
  OpHandle→Int64 inheritance claim is consistent: the copy inherits the registered head's
  *current* slot contents; `reify`'s `New`/`Link`/`Value` replay cannot mint handle-kind
  cells (`writeValue` covers Int64/Double/Bool/None, `vortex_core.cpp:196-229`), and an
  `OpHandle`'s value bits *are* the target ref (`mesh_evaluator.cpp:733-734`), so the copied
  slot lands as `Int64(promotedTarget)` — pre-rehydrated — while `handleTarget` on an Int64
  cell yields nothing and the kind-guard (`:1719`) no-ops it.
- `code_named`'s `d.code`-miss → program-image-walk fallthrough (:1863-1868) is unambiguous
  by the doc's own disjointness argument (literal heads are never `d.code`-registered).
- The `patch:<n>` verbatim descriptor arm (:1503-1509) and the declared-handler gap are
  internally consistent: spec V15 gives handlers no name slot and spec §5.7 names only
  goals/events/methods/defs in `d.vars`; `unlearn`'s reverse read (clause `+d.name` label,
  symbol cell *content*) matches `exportSymbol` minting `makeCell(name)` with no `+d.name`
  (`vortex_core.cpp:119-127`).
- The `plan_named`/`symbol_named` `run`-arm removal is type-correct: both builtins are
  `Opt<Cell>` (`:1046`,`:1050`) where `run` demands `Code<T>`; `code_named(L)` with a
  qualified `L` is indeed the only remaining producer of a qualified `run`-role record, and
  the doc honestly notes it fails `code_named`'s in-session walk while the record resolves
  at genesis.

VERDICT: **NOT CONVERGED** — 0 blocking, 1 major, 3 minor, 4 nits. The lineage-store ownership
(R8-A) is now mechanically exact end to end — spawn/resume/checkpointView ownership, the
`attach(Space)` install guard, and the `applyOpsSegment`/`adoptOpRecords` position-stability
all verify against the code. The one major is genesis (a)'s `walk-max = E`: it compares a
cell-ref maximum against an op-count boundary and refuses every artifact whose tail ops are
links — i.e., every real image — unless the doc defines `E` as the last cell-bearing position
or states the closing-cell emission invariant.
