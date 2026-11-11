# r11-codebase: Round-11 codebase verification of `design/vagent-metaprogramming.md` v1.10

**Orchestrator summary (≤200 words):** Every v1.10 citation fix verifies against the tree —
`setLink` `arena_manifold.cpp:2041`, `real.emplace` `:1868`, `OpRecord`/`CompactOpNode` naming
(`binary_ops.hpp:86`, `compact_op.hpp:28`), `historyFromSeal` `publication.cpp:1093-1103` incl.
the `applyOpsSegment` call at `:1100`, `restorePublication`'s pair `setContentSource` `:1129` /
`bindPublishedLocalScroll` `:1131`, `Store(readerPermascroll, pub.storeId)` `:1127-1128`,
`Store()`'s fresh `UserPermascroll` `store.cpp:123-124`, `historyFromSeal`'s internal mint
`publication.cpp:1097` + decl `publication.hpp:605-612`, `ReceivedValue checkpoint`
`mesh_actor.cpp:182`/`checkpoint.store->userPermascrollPtr()` `:261`, `unique_ptr<Store>`
`mesh_value.hpp:102` (struct `:98-105`), `WorkerContext` two-arg ctor `worker_context.hpp:52-54`,
resume's `setDefaultSpanReader` `mesh_actor.cpp:273`, raw `base_` `arena_manifold.hpp:752`,
`store.hpp:133`,`:1435`, `mesh_evaluator.cpp:658-663` (`program.instructions` → `setOpcodeKind`
`:662`). The recorded-enumeration claim **is mechanizable**: `adoptOpRecords`' min-`produces`
heap over deduped records (`store.cpp:3837-3846`,`:3897-3928`) is input-order-independent, so
post-fold positions are computable at emission — but the doc never pins the enumeration's
*space* (emission-order vs post-fold positions diverge on a branched band). New defects: the
verb-minted `patch:<n>` leaves stale "same-named learned clause" mechanics (`learn_handler`'s
plan_named first-hit ambiguity is impossible under unique labels) and an unpinned, re-usable
`d.patch` ordinal (non-tail `unlearn` pops collide labels and mis-rebind descriptors) — **0
BLOCKING, 2 MAJOR, 3 MINOR, 3 NITS. VERDICT: NOT CONVERGED.**

## Citation verification — v1.10 delta cites

All checked against the file at the cited lines:

- `arena_manifold.cpp:2041` — `out.version = store.setLink(out.version, real.at(arena), …)` —
  exact. `:1868` — `real.emplace(arena, arena);` inside the `!isEphemeral` arm (`:1867-1869`) —
  exact; r10's NIT-1 is closed.
- `publication.cpp:1093-1103` — `historyFromSeal` signature through closing brace; its
  `applyOpsSegment` call sits at `:1100`, inside the widened range. `applyOpsSegment` is
  anonymous-namespace file-local (`:1030`, `} // namespace` before `:1093`), magic gate
  `:1033` (`sealedOpsMagic = "\x7fXSO\x01"`, `:971`). `:1097` — `auto history =
  std::make_unique<Store>();` — mints `Store()` internally with no permascroll/`DocumentId`
  parameter, exactly as claimed. Decl pair `publication.hpp:606-612` (cite `:605-612` covers
  both overloads).
- `publication.cpp:1129-1131` — `history->setContentSource(&source)` `:1129`,
  `history->addScroll(local->second)` `:1130`, `history->bindPublishedLocalScroll(self)`
  `:1131` — the pair, exact. `:1127-1128` — `std::make_unique<Store>(
  std::move(readerPermascroll), pub.storeId)`. `store.hpp:132-133` — the
  `(shared_ptr<UserPermascroll>, const DocumentId&)` ctor; `store.hpp:1435` —
  `DocumentId documentId_`. `store.cpp:123-124` — `Store::Store() :
  userPermascroll_(std::make_shared<UserPermascroll>()),` — the fresh mint, exact; and
  `DocumentId()` self-`generate()`s (`document_id.hpp:26`), so "each agent its own document
  lineage" is literal, not figurative.
- `publication.cpp:1049`,`:1086` — `remap.emplace(localScroll, selfScrollInHistory)` and the
  span rewrite; `selfScrollInHistory = history->addScroll(from)` is an *external* id (≥1), so
  "the remap makes *no* span local" is exact — every folded span resolves through externals and
  dead-reads `""` without a source (`hydrateExternalScroll` early-returns on no source,
  `store.cpp:1433-1436`; `setContentSource` wires the fallback at `:1425-1432`).
  `bindPublishedLocalScroll`'s contract (`store.cpp:1418-1423`, throws unless `1 ≤ id ≤
  externals.size()`) is satisfied by the `from`-scroll's id — see NIT-3 for how `self` is
  recovered.
- `mesh_actor.cpp:182` — `MeshActorRuntime::resume(` signature; `ReceivedValue checkpoint`
  param `:184`; `checkpoint.store->userPermascrollPtr()` `:261`; the store is otherwise read
  only for `!checkpoint.store` validity `:188` and dropped when `actor->checkpoint` is rebuilt
  `:269`. "Consumed at `:186`" lands on `wallNow) {` — see NIT-1.
- `mesh_value.hpp:98-105` — `struct ReceivedValue` `:98-105`; `std::unique_ptr<xanadu::Store>
  store` `:102` — the `unique_ptr` claim exact, so "supply implies shared ownership or a second
  seeded product" follows: `resume` takes `ReceivedValue` *by value* but the adapter must
  *keep* its lineage store, so a `unique_ptr` move-in cannot serve the kept store.
- `worker_context.hpp:52-54` — `explicit WorkerContext(const Manifold *baseManifold = nullptr,
  const xanadu::SpanReader *reader = nullptr) : arena(baseManifold),
  core(*arena.setDefaultSpanReader(reader)), …` — the two-arg form *does* install the reader at
  construction, so "the spawn arm supplies both at construction" is mechanizable today.
  `mesh_actor.cpp:271-273` — resume passes `actor->checkpoint->manifold.get()` alone (`:271`)
  then `context.arena.setDefaultSpanReader(actor->checkpoint->store.get())` (`:273`) — the
  stated real call shape, exact.
- `arena_manifold.hpp:752` — `const Manifold *base_{nullptr};` — raw pointer, exact; the
  pin-for-life requirement follows from `rebuildManifold` returning `Manifold` by value
  (`store.hpp:450-457`).
- `mesh_evaluator.cpp:658-663` — `std::unordered_set<CellRef> instructions` `:657`, the
  `program.instructions` loop `:658-664`, `context.vm.setOpcodeKind(cell, instruction.opcode)`
  `:662` — the recorded-list precedent, exact.
- `mesh_actor.cpp:268` — `actor->sourceLease = std::move(sourceLease);` — the actor-pinned
  `shared_ptr<const ContentSource>` lease, exact; `setContentSource(const ContentSource*)`
  (`store.hpp:1196`) takes `.get()` of it.
- `compiler.cpp:769-772` — `dimConstant` mints `literal(int64(dim))` cells — the honest data
  cells an `asInt64 ≤ Linked` sweep would wrongly register (`getOpcodeKind`'s decode is `*val
  >= 0 && *val <= Linked`, `vortex_vm.cpp:107-111`; the doc's "≤ Linked" shorthand omits the
  `≥0` bound, harmless).
- `store.cpp:3836-3940` — `adoptOpRecords`: dedup by `produces` (`:3837-3846`), dep collection
  (`:3848-3872`), Kahn with min-`produces` heap (`:3897-3928`). Public (`store.hpp:1369`).
- `vortex_core.cpp:196-254` — `writeValue` `:196`, `splice` `:251-254` — funnel cite exact.
  `vortex_vm.hpp:186-188` — `opcodeMap_`/`memoizedOps_`/`cursorPC_` private VM state.
  `vortex_stdlib.cpp:1436-1439` — `VortexStdLib` ctor → `bootstrap()` → `mintOpcode`s
  (`:1504`+) during `WorkerContext` member construction — "populated by `VortexStdLib`'s
  `mintOpcode`s during `WorkerContext` construction" exact; those cells are ephemeral stdlib
  twins, so "holds none of the folded image's op cells" is exact too.
- `vortex_core.cpp:14`,`:29-70` — ctor → `initGenesis` → `mintNamedDimension` →
  `baseDimensionNamed` (`:18`,`:349-352` → `arena_.base()->dimensionNamed(name)`) — the folded
  image's dim names are primedia spans, so the unhydrated-fold claim ("`initGenesis` mints
  ephemeral twin dims") is mechanically exact, not loose.
- `mesh_value.hpp:98-105` re-checked; `mesh_actor.hpp:102-106` (`ActorCheckpointView`
  `:102-106`, no store member); `mesh_value.cpp:227-228` (`sealLocalSpool` with default
  `opsAlreadySealed`),`:241`; `publication.cpp:611-617`; `manifold.hpp:295`;
  `store.hpp:677-681`; `mesh_evaluator.cpp:735` (`builder.link(cell, relocation,
  arena.makeCell("cell"))` — per-ref marker precedent); `mesh_actor.cpp:119-122`,`:156`,
  `:260-269`; `vortex_vm.cpp:98-100`,`:102-114`; `arena_manifold.cpp:78-105`,`:1766`,
  `:1947-1954`,`:2010-2017`,`:274-287`; `mesh_jobs.cpp:304-318`; `mesh_host.cpp:507`,
  `:509-510`; `mesh_value.cpp:519-523`; `provenance.hpp:286` — all re-verified or unchanged
  from r10 verification.

## Recorded instruction enumeration — mechanizability

The task's core question: can the emitter know op positions post-`adoptOpRecords` reordering?
**Yes, with one unstated constraint.**

- `adoptOpRecords`' output is a deterministic function of the adopted *set*, not its input
  order: dedup on unique `produces` (`store.cpp:3837-3846`), backward-only deps
  (`:3848-3872`), Kahn drained by a min-`produces` heap (`:3909-3928`). Input permutations
  yield identical schedules, so the fold is idempotent and the receiving store's `1..E_ops`
  order is fixed by the segment's record set alone.
- Therefore the emitter *can* know post-fold positions at emission: `adoptOpRecords` is public
  (`store.hpp:1369`) — the emitter can fold its own record list (or read back `opRecords()`,
  `store.hpp:1363`) — and under the normal artifact shape the work is free anyway: sequential
  promote appends give the image ops a linear `parent` chain, the unique topological order, so
  sealed order == fold order verbatim.
- The residual spec gap (Finding 3): "records which **band positions** are its op cells" never
  says *which* band — the sealed/append order (S) or the canonical post-fold order (T). On a
  branched image band — "a reachable shape" per (a)'s own text — S≠T, recorded positions point
  at the wrong cells in the folded band, and (a)'s comparand cannot catch it: it compares
  fold-vs-fold of the same segment, which is equal regardless of S. A `produces`-id
  enumeration sidesteps positions entirely.

## New findings (v1.10 deltas only)

### Finding 1 — MAJOR

- **Location:** :1140-1144 (`learn_handler` additivity) and :1142-1144 (the `unlearn` gloss);
  minting rule :1130-1133, :1590-1591; glossary :136.
- **Problem:** the verb-minted `patch:<n>` makes every learned clause's `+d.name` unique, but
  the additive-handler and unlearn text still speaks v1.9's shared-label language. "`learn_handler`
  is deliberately additive instead — a second **same-named** handler clause *appends* beside the
  first, so `plan_named(name)`'s first-hit stays ambiguous between them" is unreachable: two
  learned handler clauses carry `patch:1`/`patch:2` — never the same `+d.name` — and the user
  alias lives only on the patch record, which `plan_named` (a clause-`+d.name` verbatim match,
  :1071) cannot see. `plan_named("patch:1")` hits *uniquely*; `plan_named("near_water")` misses
  *both* — there is no `name` under which first-hit ambiguity can occur. Either `learn_handler`
  is an unstated exception to the minting rule (which then also breaks the descriptor
  convention at :1618-1620, "the `patch:<n>` the learning verb minted — for learned-clause
  targets") or the sentence is stale. The `unlearn` gloss compounds it: "among same-named
  learned clauses it always pops the latest patch recording that name" — the real LIFO subject
  is same-*target* patch records (two `learn_where` on clause `patch:5`, or two `learn_handler`
  on event `Evt` whose patches record the same target name), since same-named learned clauses
  cannot exist.
- **Evidence:** :1130-1133 — "under `learn_*` the clause's `+d.name` label is the verb's own
  mint, `patch:<n>`"; :1071 — `plan_named` "matches every clause `+d.name` label verbatim";
  :1643-1645 — `unlearn` "matches by the patch target's resolved name … the latest patch whose
  recorded target name equals it".
- **Concrete fix:** rewrite the handler sentence to name where additivity lives now — "a second
  `learn_handler` on the same event *appends* beside the first — distinct `patch:<n>` labels,
  `priority` the differentiator at selection" — and the `unlearn` limit to its true subject:
  "among patches recording the *same target name* it always pops the latest; the argument
  cannot pick an earlier patch on that target out of order."

### Finding 2 — MAJOR

- **Location:** :1130-1132 ("`<n>` = the `d.patch` ordinal the call creates"), :1590-1591
  ("`<n>` the new patch's ordinal"), glossary :136 ("ordinal"); interplay with `unlearn` "pops"
  :1143, "dead patches drop" :1089,`:1585`, replay "in ordinal order" :1947,`:1995`.
- **Problem:** `patch:<n>`'s uniqueness is load-bearing — `plan_named` first-match, target
  descriptors, and the R10-A "only the verb can mint it" invariant all ride on it — but the
  doc never says how the ordinal is *assigned*, and every assignment derivable from the schema
  breaks under the `unlearn` pops the same round adds. Rank-position or rank-length minting:
  pop a non-tail patch (`learn` on `x` → `patch:1`, `learn` on `y` → `patch:2`,
  `unlearn(x)` pops `patch:1`), append `learn` on `z` → the new patch lands where ordinal
  arithmetic (`length+1` or tail index) produces `patch:2` — **a duplicate live label**, so
  `plan_named("patch:2")` and any `patch:2` target descriptor are ambiguous. Even tail-only
  pops re-issue freed numbers: pop `patch:3`, mint a new `patch:3` — a *different* clause now
  carries the label while other patches' recorded target descriptors still name the old
  `patch:3`, so genesis replay and `unlearn` mis-rebind to the new clause. A monotonic-ever
  counter would fix it, but nothing in the schema supplies one: popped patch records leave the
  rank ("pops", "dropped"), so the verb cannot recover a count-ever from the live `d.patch`.
- **Evidence:** the doc's only ordinal statement is "the `d.patch` ordinal the call creates"
  (:1131) / "the new patch's ordinal" (:1590) — assignment unspecified; "pops" (:1143) and
  "dead patches are dropped" (:1585) make vacated ordinals reachable, and :1947/`:1995`
  require ordinals comparable for replay order.
- **Concrete fix:** pin the mint to a never-reused token — e.g., "`<n>` = the patch cell's own
  persistent ref / a mint-order ordinal recorded as one more than the greatest ordinal ever
  recorded on the rank, retained even after `unlearn` drops records (tombstone ordinals)" — or
  state that popped patches leave ordinal tombstones so position-derived minting cannot
  re-issue a live or descriptor-named number.

### Finding 3 — MINOR

- **Location:** :1924-1932 (genesis (b)).
- **Problem:** as analyzed above — "the artifact records which band positions are its op cells
  at emission" leaves the position space unpinned: sealed/append order (S, what the emitter's
  store literally holds) vs canonical post-fold order (T, what every receiver folds). For
  honest linear-chain artifacts S=T; on the branched band (a) calls reachable they diverge and
  the recorded positions mis-register cells — silently, since (a)'s fold-vs-fold comparand
  cannot see S.
- **Evidence:** `store.cpp:3897-3928` (deterministic min-`produces` fold); `adoptOpRecords`
  public (`store.hpp:1369`), so post-fold positions are computable at emission; the
  `program.instructions` precedent the doc cites actually records *emission-side identities*
  (`instruction.cell` indexes `program.cells`, `mesh_evaluator.cpp:659`), not positions.
- **Concrete fix:** "records which **post-fold** band positions are its op cells (equivalently,
  the op records' `produces` ids — order-free identities resolvable by `indexOf` after the
  fold)".

### Finding 4 — MINOR

- **Location:** :1390-1406 (the two-roots vocabulary).
- **Problem:** the program-side walk vocabulary still lists `d.contract`, `d.belief`,
  `d.source` among "every dim the program skeleton hangs on", while the next paragraph groups
  them with `+d.code` as "legs hung on the agent cell … enumerated only by a second walk".
  `+d.code` alone was moved out of the list; the other three agent-cell ranks were left in —
  internally inconsistent about which dims live on which root (harmless to the walk, since the
  dims are unreachable from `program()`, but the enumeration is now muddy about its own
  vocabulary).
- **Evidence:** :1390-1392 lists `d.contract`, `d.belief`, `d.source` in the program vocab;
  :1401-1405 names all four as agent-cell legs enumerated only by the second walk.
- **Concrete fix:** move `d.contract`, `d.belief`, `d.source` into the agent-cell group with
  `+d.code`, or state the big list is the *union* both walks share.

### Finding 5 — MINOR

- **Location:** :1132-1133, :1591-1592, glossary :136 (the user `name` alias).
- **Problem:** the patch record carries "the user `name` alias", but no dim is named for it
  (the patch's stated fields are `{ordinal, target descriptor record, the user `name` alias,
  +d.patch-head→record chain, +d.name→label, priority}` — the alias's carrier is unnamed) and
  no reader consumes it: `learn_plan`'s dedupe matches the *argument* alias against clause
  `+d.name`s, `unlearn` matches recorded *target* names, `plan_named` reads clause labels —
  the recorded alias is provenance-only, unstated.
- **Concrete fix:** name the alias's dim (e.g., `+d.alias`) and state it is
  provenance/dedupe-key only — no lookup resolves it — or give it a consumer.

### Nits

- **NIT-1** (:1441, :1793, :2552, :2564, :3145): "consumed at `:186`" — `mesh_actor.cpp:186`
  is `wallNow) {`, the signature's close. The `ReceivedValue checkpoint` param is `:184`, the
  first store deref `!checkpoint.store` `:188`, the `userPermascrollPtr()` use `:261`. Correct
  to `:184`/`:188`,`:261`.
- **NIT-2** (:1418-1419): "`initGenesis` mints ephemeral twin dims" is mechanically right via
  `mintNamedDimension`→`baseDimensionNamed` (`vortex_core.cpp:18`,`:349`), but the earlier
  phrasing "`initGenesis`'s `baseDimensionNamed`" (:606-607 area) compresses the call chain —
  initGenesis adopts `home_`/`dims_` by *ref* (`:30-43`) and only the per-dim mints name-resolve.
  Substance holds.
- **NIT-3** (:614, :1415, :2496): `bindPublishedLocalScroll(self)` borrows `restorePublication`'s
  local name, but `self` is internal to `historyFromSeal` (its own `addScroll(from)`, `:1098`)
  and never returned; the adapter recovers it via `addScroll`'s `sameContentAs` dedup
  (`store.cpp:1388-1402` — re-adding `from` returns the same id, always `externals[0]`/id 1 on a
  fresh store). Mechanizable, worth a clause.

## Non-findings verified (delta claims that hold)

- **Hydration seam is real and the pair is right:** `historyFromSeal` (:1093-1103) calls only
  `make_unique<Store>`, `addScroll`, `applyOpsSegment` — no `setContentSource`, no
  `bindPublishedLocalScroll` — while `restorePublication` runs both (`:1129`,`:1131`); without a
  source, `hydrateExternalScroll` early-returns (`store.cpp:1433-1436`) so every external span
  reads `""` and `baseDimensionNamed` (`vortex_core.cpp:349-352`) misses every folded dim name.
- **Adopt-`documentId` arm correctly named unmechanized:** `applyOpsSegment` is
  anonymous-namespace file-local (`:1030`) and `historyFromSeal` mints `Store()` itself
  (`:1097`) — no public path applies segments into a pre-minted `Store(permascroll, id)`.
- **`resume` store-transport claim is accurate:** `checkpoint` by value (`:184`), `store` read
  for validity (`:188`) and `userPermascrollPtr()` (`:261`), replaced by `receiveValue`'s
  product (`:269`); `unique_ptr` (`mesh_value.hpp:102`) ⇒ shared ownership or a second seeded
  product for a kept store.
- **`opcodeMap_` unscoped correctly:** populated by `VortexStdLib` bootstrap `mintOpcode`s
  during construction (`vortex_stdlib.cpp:1436-1439`); `initGenesis` adopts `home_`/dims only —
  the narrowed "holds none of the folded image's op cells" phrasing is exact.
- **`code_named(L)` slot claims hold:** `Manifold::valueKindOf` defaults `None` on unwritten
  cells (`manifold.cpp:1078-1083`) — "observable content is `None`; no sentinel" exact; the
  emission-fixed / fail-until-resume vs fail-forever split is internally consistent.
- **Two-roots walk is consistent modulo wording:** `d.code` does hang on the agent cell
  (`:1597`), cells under agent-cell legs are all `>E_ops` state-side, and the second walk is
  admitted optional ("or they simply classify nothing").
- **`learn_*` minting + alias dedupe is coherent:** `learn_plan` matching the argument alias
  against clause `+d.name`s hits grafted clauses (verbatim user names) and never learned ones
  (`patch:<n>` labels) — "successive learns append rather than self-replace" follows; §9.3's
  corrected example (`"near_water"` as alias) is consistent with the rule.
- **Capture-endpoint restatement matches the schema:** `+d.ref`→the slot cell (`:1024`),
  recipe tuple `+d.values`→endpoint cells never census ints (`:1111-1113`), internal referents
  through the old→new reify map (`:1112-1116`), `*handleTarget` equality external-only.
- **`opRecords()`/`ArenaManifold::store()` give the comparand its surface:** `Store::opRecords`
  public (`store.hpp:1363`), `ArenaManifold::store()` falls through to `base_->store()`
  (`arena_manifold.hpp:272-274`), and `rebuildManifold` `setStore`s the fold — the adopted
  store's `1..E_ops` band is decodable on both sides.

VERDICT: **NOT CONVERGED** — 0 blocking, 2 major, 3 minor, 3 nits. All v1.10 citation fixes
verify; the recorded-enumeration claim is mechanizable via the deterministic public
`adoptOpRecords` fold (position space unpinned — Finding 3). The `patch:<n>` naming delta is
the live front: stale "same-named" mechanics (Finding 1) and an unpinned, re-usable ordinal
(Finding 2).
