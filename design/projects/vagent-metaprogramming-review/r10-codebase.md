# r10-codebase: Round-10 codebase verification of `design/vagent-metaprogramming.md` v1.9

**Orchestrator summary (≤200 words):** Every v1.9 citation fix verifies — `publication.cpp:1048-1086`
(remap loop ends `:1086`), `:1030`/`:1033`/`:1093-1099`/`:1127-1128`; `arena_manifold.cpp:1864-1867`
(guard; emplace at `:1868`), `:2010-2017` ("Links last"), `:1934-1954`, `:1755-1757`;
`mesh_jobs.cpp:304-318` (now exact); `arena_manifold.hpp:269-271`; `store.hpp:133`,`:1435`,
`:677-681`,`:450-457`; `mesh_value.cpp:519-523`; `mesh_host.cpp:507`/`:509-510`;
`mesh_actor.{cpp:119-122,156,199,215-216,260-269,270-271,462,467-468,473-477,659-662,668-694}`,
`mesh_actor.hpp:132-135`; `worker_context.hpp:52-55`,`:63`; `vortex_core.cpp:14`,`:29-70`,
`:119-127`,`:196-229`; `vortex_core.hpp:268`,`:282`,`:287`; `store.cpp:3836-3940`;
`provenance.hpp:286`; `Makefile:643`,`:688-690`,`:695`. The two-landmark model is mechanically
sound: image ops form a dependency down-set, so `adoptOpRecords`' deterministic fold reproduces the
seeded band's relative order index-for-index, and `rebuildManifold`'s `birthOp = opIndex` makes
`≤ E_ops` a true classifier. New defects: the `patch:` refusal contradicts §9.3's own
`learn_plan("Feed", "patch:near_water", …)` and leaves `patch:<n>`'s assigner unspecified (MAJOR);
the comparand's operand is ambiguous between segment order and fold order (MINOR); `label_of`'s
`cells"##/+d.code"` anchors at `home`, not the agent cell (MINOR). **0 BLOCKING, 1 MAJOR, 2 MINOR,
4 NITS. VERDICT: NOT CONVERGED.**

## Citation verification — v1.9 delta cites

All checked against the file at the cited lines:

- `publication.cpp:1048-1086` — scroll-table `remap.emplace(localScroll, …)` at `:1049`, the
  per-record rewrite `record.op.span.scroll = at->second` at `:1086`. Range is exact.
- `publication.cpp:1030` — `applyOpsSegment` signature, anonymous-namespace (file-local). `:1033`
  — `sealed.starts_with(sealedOpsMagic)` gate; `sealedOpsMagic = "\x7fXSO\x01"` defined `:971`.
  `:1093-1099` — `historyFromSeal` signature through the `for` head; its `applyOpsSegment` call is
  `:1100` (see NIT-2). `:1127-1128` — `restorePublication`'s
  `std::make_unique<Store>(std::move(readerPermascroll), pub.storeId)` at `:1128`.
- `arena_manifold.cpp:1864-1867` — the identity-map arm is `if (!isEphemeral(arena))
  { real.emplace(arena, arena); continue; }` at `:1867-1869`; the `real.emplace` itself is `:1868`
  — the cited range covers comment + guard, one line short of the emplace (NIT-1).
- `arena_manifold.cpp:2035` — **the `setLink` call is at `:2041`**, not `:2035`; `:2035` is the
  base-dedup `if (nullptr != from.base() && !isEphemeral(arena) …)` guard (NIT-1). Substantive
  claim still correct: `setLink` writes `real.at(arena)`/`to->second`, and persistent ends pass
  verbatim through the `:1868` identity map.
- `arena_manifold.cpp:2010-2017` — the `// Links last:` comment block; the link emission loop
  follows at `:2019-2044`. "promote emits links last" verified.
- `arena_manifold.cpp:1934-1954` — Int64 verbatim arm (`:1934-1936`), OpHandle →
  `makeOpHandle(isEphemeral(target) ? real.at(target) : target)` (`:1947-1951`), ForeignOpHandle →
  `makeForeignOpHandle` (`:1952-1954`). "Int64 verbatim, handle kinds translated" exact.
  `:1992-1996` — the cyclic-handle `setValue` OpHandle arm. `:1759-1764` — the
  `isCellRelocationHandle` discover arm in the BFS. `:1797-1813` — the unmarked-OpHandle gate.
- `mesh_jobs.cpp:304-318` — now exact: `std::optional<ReceivedValue> base;` (`:304`) through the
  no-base arm's `}` (`:318`); `store = base->store.get()` at `:314`, `evaluate` at `:319`. The r8
  off-by-one (the stray `:303` return) is gone.
- `mesh_host.cpp:507`,`:509-510` — `store = base->store.get()` (`:507`), `empty =
  std::make_unique<xanadu::Store>` (`:509`), `store = empty.get()` (`:510`). The two `commit`
  destination arms, exact.
- `mesh_value.cpp:519-523` — `std::ranges::sort(ordered, …)` with
  `std::tie(contentHash, origin.authorKey, parent)` — the `(contentHash, authorKey, parent)`
  order, exact. `:227-228` — `sealLocalSpool` called with default `opsAlreadySealed = 0`
  (`publication.hpp:340`). `:241` — `{*seed.history.opsSegment}` to `publish`. `:485-501` —
  `importValue` demotes unmarked `OpHandle`→`None` (`:485-490`), re-kinds marked via
  `local.at(*target)` (`:494-500`).
- `publication.cpp:611-617`,`:664-672` — `opsTotal`/`opsAlreadySealed` guard, `sealableOps`;
  segment `at=opsAlreadySealed`, `length=opsTotal-opsAlreadySealed`, `path=sealedOpsName` — with
  `opsAlreadySealed=0` covers `1..opsTotal`. Exact.
- `provenance.hpp:286` — `inline constexpr auto sealedOpsName = "ops";` — the sealed `ops`
  segment name. Exact.
- `arena_manifold.hpp:269-271` — `store()` at `:269-272`, returns `store_` else
  `base_->store()` — public, falls through as claimed. `Manifold::store()` at `manifold.hpp:486`
  is `const noexcept` returning non-const `Store*` — the "de-facto supply shape" claim holds.
- `store.hpp:133` — `Store(shared_ptr<UserPermascroll>, const DocumentId&)` ctor param.
  `:1435` — `DocumentId documentId_;`. `:677-681` — `cellRefOf` returns
  `opsSpool.indexOf(version)` (`index != 0` guard): the persistent ref *is* the op position, and
  `segmented_ops_spool.hpp:277-280` documents index 0 as the op-less root state — so `1..E_ops`
  is the right band spelling. `:450-457` — `rebuildManifold` returns `zigzag::Manifold` by value
  and `setStore`s it (`store.cpp:511`,`:532`), so the `arena.store()` fall-through on a rebuilt
  base works as claimed.
- `store.cpp:3836-3940` — `adoptOpRecords`: dedup (`:3837-3846`), dep collection incl. SetLink
  dim/target and OpHandle value-target, Kahn order with a min-`produces` heap (`:3897-3928`).
  "topologically re-sorts adopted records with a `produces`-hash tie-break" exact.
- `mesh_actor.cpp:119-122`/`mesh_actor.hpp:132-135` — `spawn(identity, request, wallNow)` — no
  store param. `:156` — `actor->context = std::make_unique<WorkerContext>()` (fresh arena).
  `:199` — `continuation->programRef != request.programRef`. `:215-216` — grant↔continuation
  `programRef` and `baseRef`↔`contentHash` pins. `:260-269` — `receiveValue` → private
  `actor->checkpoint`. `:270-271` —
  `std::make_unique<WorkerContext>(actor->checkpoint->manifold.get())` — the base-injection shape
  (one-arg here; the ctor's reader slot exists at `worker_context.hpp:52-54`, and resume pairs it
  with `setDefaultSpanReader` at `:273`). `:462` — `mailbox.empty()` skip; `:467-468` —
  `front()`/`pop_front()`; `:473-477` — `executor_(…, *actor->context)`; `:659-662` — `pending()`
  → `mailbox.size()`; `:668-694` — `checkpointView` incl. the `!mailbox.empty()` refusal
  (`:672-676`).
- `worker_context.hpp:52-55` — `WorkerContext(const Manifold *baseManifold = nullptr, const
  SpanReader *reader = nullptr)`; member-init order `arena(baseManifold), core(…)` puts the base
  in place before `VortexCore`'s ctor. `:63` — `ArenaManifold arena;`.
- `vortex_core.cpp:14` — ctor runs `initGenesis()`. `:29-70` — the `initGenesis` body;
  `:30-43` adopts `*base->home()` and `*base->dimsDimension()` only when a base exists.
  `vortex_core.hpp:268`,`:282`,`:287` — `private:`, `initGenesis()`, `home_{noCell}` — no
  re-genesis door. Exact. `:119-127` — `exportSymbol` ends with the `+d.values` link and the
  `d.vars` rank link (links close the image arm). `:196-229` — `writeValue` covers
  Double/Int64/Bool/None — no handle kind. `:100-127` — `findModule`'s `firstOf` first-match.
- `vortex_vm.cpp:98-100` — `setOpcodeKind`. `:102-114` — `getOpcodeKind` map-then-`asInt64`
  fallback. `vortex_vm.hpp:186-188` — `opcodeMap_`/`memoizedOps_`/`cursorPC_` private VM state.
  `:73-82` — `mintOpcode` writes `int64(op)` + registers. `:50-67` — `resolveCell`
  `arena.contains` gate. `:157-160` — `home +d.cursors` rank append. `:206` — cursor→frame
  `d.stack`. `:337`,`:343-346`,`:771-806`,`:1269-1273`,`:1287-1294`,`:139-143`,`:854-893`,
  `:854-856` (`toInt(inputs[0])`),`:904`,`:616-836`,`:654-723`,`:807-836` — the P15 funnel sites
  and op ranges all land where cited.
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` → `arenaDims_` cache → mint.
  `:274-287` — `attach(Space)` installs `base_`/`store_` when unset. `:1347-1377`/`:1352-1373` —
  the `displacedUs`/`displacedIt` eviction. `:1423-1439` + `arena_manifold.hpp:624-628` —
  `insertIntoRank`, idempotent on existing membership. `:1489-1490` — "the shadow's name is the
  base cell's ref". `:1744-1788` — the reachable-subgraph BFS; `:1755-1757` — the
  non-`holdsOwn`-persistent prune; `:1766` — the `d.dims` skip; `:1782-1783` —
  `discover(edge.pos/true) + discover(edge.neg/true)` — both directions. `:256-267` — `denseOf`;
  `slot()` falls through to `base_->slot(ref)` (`:791-797`), so `contains(persistentRef)`
  resolves base cells. `arena_manifold.hpp:468-470` — `refOf = ephemeralBit|dense`;
  `manifold.hpp:154-158` — `ephemeralBit`; `dim_vector.hpp:23` — `noCell = 0`.
- `manifold.cpp:427-442` — `.birthOp = opIndex` (`:434`), `byRef.emplace(opIndex, dense)`
  (`:442`). `:450-451` — `home_` = first MakeCell. `manifold.hpp:295` — "the MakeCell index; ==
  this CellRef".
- `foreign_op_handle.cpp:98-117` — `isCellRelocationHandle` requires `OpHandle` kind *and* the
  `d.mesh.relocation`→`"cell"` edge. `ops.hpp:154-163` — `OpHandle`/`ForeignOpHandle` kinds.
  `mesh_evaluator.cpp:723-735` — the relocation-dim mint + `setValueBits` + `link` precedent.
- `compiler.cpp:769-772` — `dimConstant` mints `int64(ref)` cells; `:484-486` — the `$`-anchor
  mutation refusal; `:108-110` — the no-marks/no-choices emission invariant; `:749-751` —
  `Anchor::Kind::Home` resolves to `core_.home()`.
- `vagent/compiler.hpp:28-34` (`CompiledStatement`),`:23-26` (`ScalarCompilerLimits`),
  `checker.hpp:12-27` (`CheckLimits`), `cell_path.cpp:147-183` (`parseCellsPath`),
  `vortex_stdlib.cpp:4238-4241` (`addClause` rankTail append), `cell_views.hpp:258`
  (`rankTail`), `Makefile:643` (`COMMON_XANADU_SRCS` glob),`:688-690` (`VAGENT_FRONTEND_SRCS`),
  `:695` (`VAGENT_SYNTAX_OBJS`), `mesh_actor_resume_test.cpp:125` (fresh `Store`),
  `vagent-agent-language.md:1670` (P4 `wake` row),`:425-428` (per-arena-load wording) — all
  verified.

## Two-landmark model vs `rebuildManifold`/`adoptOpRecords` — mechanically consistent

- `rebuildManifold` assigns `birthOp = opIndex` verbatim (`manifold.cpp:434`), so a cell's
  persistent `CellRef` is its birth op's spool position; `cellRefOf` confirms ref==position
  (`store.hpp:677-681`) and index 0 is the op-less root state, making `1..E_ops` the correct
  band spelling and `noCell == 0` "the only ref below them".
- `adoptOpRecords`' Kahn pass uses only backward-pointing deps (`parent`, `source`, `context`,
  structure dim/target, OpHandle value-target — `store.cpp:3848-3872`). Image ops' deps therefore
  lie wholly inside the image set, so the image records form a down-set: their *relative* fold
  order is the same whether the segment holds the image alone or image+state (the ready-queue
  min-`produces` tie-break pops the same image op whenever an image op is popped). The adapter's
  seeded band and the resumed band carry identical relative order — the comparand premise holds.
- Under a *linear parent chain* (every appended op's `parent` = predecessor — the normal spool
  shape), the fold reproduces the sealed order verbatim, so `1..E_ops` is a strict prefix and
  positions coincide exactly. Under a *branched* segment a state op can interleave below `E_ops`
  — but then the band content already differs from the image sequence and the comparand refuses,
  which is the stated purpose. The "permuted within `1..E_ops`" sub-case (all image ops inside,
  reordered) is the coincidence the comparand exists for. Consistent.
- (d)→(e) capture-endpoint restatement verified mechanically: post-resume a donor ref cell is
  `OpHandle`-kind carrying `real.at(target)` (`arena_manifold.cpp:1949-1951`) — the promoted
  position; a replayed `Value` writes `int64(endpoint)` where the endpoint is the fold cell at
  that same position, so the bits are identical and the copy arrives pre-rehydrated. `slot()`
  falls through to `base_->slot(persistentRef)` (`arena_manifold.cpp:791-797`), so the written
  int resolves session-side. Consistent.
- Spawn-side base-injection verified: `WorkerContext`'s init list constructs `arena(base)` before
  `core(…)` (`worker_context.hpp:52-55`), and `initGenesis` runs inside the `VortexCore` ctor
  (`vortex_core.cpp:14`) adopting `*base->home()`/dims only if a base exists (`:30-43`) — so a
  post-construction `attach(Space)` can never re-deliver them, and the `WorkerContext(base,
  reader)` shape is the only working seam. `rebuildManifold` `setStore`s the fold
  (`store.cpp:511`,`:532`), so `arena.store()` → `base_->store()` gives the verify arm its band
  today. Consistent.

## New findings (v1.9 deltas only)

### Finding 1 — MAJOR

- **Location:** :1112-1122 (the `patch:` reservation, new this round) vs :2286 (§9.3's worked
  example); convention statement at :1558-1561 and :1692.
- **Problem:** the new rule "``patch:` is refused as a user-supplied `name` at
  `graft_*`/`learn_*` emission" contradicts the document's own flagship example, which calls
  `learn_plan("Feed", "patch:near_water", …)` — a `patch:`-prefixed user `name`, refused as
  written. Worse, the convention "learned ones are conventionally named `patch:<n>`, `<n>` the
  patch's `d.patch` ordinal" is now unassignable by the caller: the ordinal belongs to the patch
  the same `learn_*` call is creating, so `patch:<n>` can only be *verb-minted* — yet
  `learn_plan(goal, name, test, body)` still takes a user `name`, and the doc never says what
  that argument then feeds (the clause has one `+d.name` edge — link eviction allows exactly
  one). If the user `name` *is* the clause `+d.name`, the `patch:<n>` convention is unreachable
  by rule; if `learn_*` mints `patch:<n>` itself, the `name` parameter's disposition is
  unspecified.
- **Evidence:** :1114-1115 — "Grafted clauses take `name` parameters, held on a `d.name`
  label — learned ones are conventionally named `patch:<n>` … and `patch:` is refused as a
  user-supplied `name`"; :1559 — "a learned clause's name lives on its `+d.name` verbatim"
  (verbatim ⇒ user- or verb-written, one or the other); :2286 — the example's literal call.
- **Concrete fix:** pick the assigner. If `learn_*` mints the clause label: say so — "`learn_*`
  writes the clause's `+d.name` itself as `patch:<ordinal>`; the `name` argument is the patch
  record's lookup name / the registered head's label" — and fix the example to a non-`patch:`
  name (`"near_water"`). If users name clauses: narrow the refusal to the exact `patch:<digits>`
  shape that would collide with the ordinal convention, and keep the example.

### Finding 2 — MINOR

- **Location:** :1807-1811 (genesis (a) comparand); same phrase at :1413-1415, :1719, :2403.
- **Problem:** "the adapter holds the artifact's seeded segment, and asserts the received store's
  ops `1..E_ops` decode to the same op sequence" leaves the comparand's *operand* ambiguous: the
  artifact's **serialized segment order** (S) vs the adapter's **seeded-store fold order** (T =
  `adoptOpRecords`' deterministic topo order). For a linear parent chain S=T; but for the
  branched segment this paragraph itself invokes, S≠T is possible while the honest received band
  is still T — comparing against the segment refuses an honest resume, comparing against the
  seeded fold is correct for every shape (the checkpoint's image records are the same set, hence
  the same T). An implementer comparing "canonical post-remap form" against the retained segment
  bytes builds the refusing variant.
- **Evidence:** `applyOpsSegment`→`adoptOpRecords` re-sorts on adoption
  (`store.cpp:3836-3940`); sealing preserves stored order verbatim, so the artifact's S is an
  append order, not a canonical one.
- **Concrete fix:** name the operand as the fold — "asserts the received store's ops `1..E_ops`
  decode to the same op sequence **as the adapter's own seeded store's band** (the `historyFromSeal`
  fold of the artifact's segment — the serialized segment is not itself canonical)".

### Finding 3 — MINOR

- **Location:** :1060 (`label_of` row's new acquisition path).
- **Problem:** "`cells"##/+d.code"` for a registered head" anchors at `core_.home()` — `##` is
  `AnchorKind::Home` (`vql/ast.hpp:55`) resolved to `core_.home()` (`compiler.cpp:749-751`) — but
  the `d.code` rank hangs on the **agent cell**, not `home` (:1538-1539, "the top-level ranks …
  `d.code`, `d.patch`, `d.synapse` on the agent cell"). The example path reads `home`'s `+d.code`
  edge — by the doc's own layout there is none — so it cannot reach a registered head.
- **Evidence:** `compiler.cpp:749-751`; `vql/ast.hpp:55` (`Home ///< "##"`); the doc's own rank
  placement. The sibling arm (`cells"$clause/+d.spin"`, a `Cell`-variable anchor reaching the
  installed body head) is mechanically fine.
- **Concrete fix:** anchor by variable — "`cells"$agent/+d.code"`" (a `Cell` binding of
  `agent_cell()`'s result) — or name whatever spelling binds the agent cell.

### Nits

- **NIT-1** (:1467-1469, :2985-2986): `setLink` is at `arena_manifold.cpp:2041` (the `:2035` cite
  lands on the base-dedup guard); `real.emplace(arena, arena)` is at `:1868` (the `:1864-1867`
  cite covers the comment + `if` guard, one line short). Claims are substantively right; the
  ranges drifted.
- **NIT-2** (:603, :1381, :2392, :2982-2983): `historyFromSeal` spans `:1093-1103`; its
  `applyOpsSegment` call is at `:1100`, just outside the cited `:1093-1099`.
- **NIT-3** (:85): `arenaDims_` is an `ArenaManifold` member (`arena_manifold.hpp:756`), not
  "VM-side" — the row's grouping of it with `opcodeMap_`/`cursorPC_`/`memoizedOps_` under
  `vortex_vm.hpp:186-188` is loose (the "not mark-covered" claim itself is correct; the Mark
  fields at `arena_manifold.hpp:170-186` exclude it). The §6.5 text at :1340 already calls it
  correctly.
- **NIT-4** (:1127-1128 / :1391-1392, :2973): the doc's `Store(userPermascroll, …)` matches the
  ctor parameter name, but `restorePublication` passes `readerPermascroll` — harmless, noted for
  completeness.

## Non-findings verified (delta claims that hold)

- The `1..opsTotal` sealed-band correction is exact: `publishValue` defaults
  `opsAlreadySealed = 0` → segment `at=0, length=opsTotal` (`mesh_value.cpp:227-228`,`:241`;
  `publication.cpp:664-672`), covering positions `1..opsTotal` (index 0 is the op-less root,
  `segmented_ops_spool.hpp:277-280`).
- `plan_named`'s rewritten row is now consistent: "learned handler clauses included; declared
  handlers carry no `+d.name` to match (§6.6, spec V15)" — matches spec V15 and the :1573-1582
  gap text.
- "the only publish path the actor ceremony exercises today" (:1464-1465) is now correctly
  scoped to the ceremony (`mesh_actor_resume_test.cpp:125`), closing r9 NIT-3.
- `mergeValues` restated correctly: `(contentHash, authorKey, parent)` sort
  (`mesh_value.cpp:519-523`), fresh destination only in the no-`job.base` arm
  (`mesh_host.cpp:507` vs `:509-510`).
- The link-emission reframing is substantively right: `setLink` writes `real.at` output
  (`arena_manifold.cpp:2041`) and persistent refs identity-map (`:1868`) — only the cited line
  numbers drifted (NIT-1).
- The (e)-sweep "coverage first, ordering second" relabel and capture-endpoint mechanism are
  coherent: replayed copies' ref slots carry `int64(promotedEndpoint)` == `*handleTarget` bits;
  name-records still rebind post-install.
- `code_named(L)` unresolved-at-emission convention (:1626-1635 area) is internally consistent
  with the headless-`run` refusal class.

VERDICT: **NOT CONVERGED** — 0 blocking, 1 major, 2 minor, 4 nits. All r9 citation fixes verify;
the two-landmark model is mechanically consistent with `rebuildManifold`/`adoptOpRecords`
ordering (image ops are a dependency down-set, so the deterministic fold reproduces the seeded
band). The major is new this round: the `patch:` emission refusal contradicts §9.3's own
`learn_plan("Feed", "patch:near_water", …)` and leaves `patch:<n>`'s assigner (verb vs caller)
unspecified — an implementer cannot tell whether `learn_*` mints the ordinal name or refuses it.
