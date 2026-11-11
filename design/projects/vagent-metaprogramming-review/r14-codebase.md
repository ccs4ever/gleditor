# r14-codebase: Round-14 codebase verification of `design/vagent-metaprogramming.md` v1.13

**Orchestrator summary (≤200 words):** R13-A landed correctly — `d.captured` is struck from
every minted-dim list (:95, :533, :1018, :1025, :2788, :3028; it survives only in the
changelogs where it belongs), §6.1's enumeration (:1139-1165), the head diagram (:1052-1059)
and genesis (e) (:2200-2228) are restated coherently: `+d.refFor` correlates record→slot, the
referent rides slot content (`int64`/`handleTarget`/old→new map/`+d.name` for dims), and the
optional `+d.values`-may-name is correctly conditioned on a free `-d.values`. Every assigned
v1.13 citation verifies exact — `globalKeyOf` (`publication.cpp:853-869`), the remap pair
(`:1049-1050`), `applyOpsSegment` (`:1048-1086`), `breakMarkerKey` (`publication.hpp:447`),
`Store::scroll` (`store.cpp:1461-1469`), `adoptOpRecords` (`:3836-3940`), `store.cpp:124`,
`spool.hpp:19`, `vortex_stdlib.cpp:1953`, `vortex_core.cpp:366-369`/`:384-390`,
`arena_manifold.cpp:1347-1377`/`:1867-1868`/`:1947-1951`, `compiler.cpp:531-532`/`:781`/`:796`,
`mesh_actor.cpp:182`/`:188`/`:260-271`, `worker_context.hpp:52-54`, `manifold.hpp:486`,
`arena_manifold.hpp:269-271`/`:752`, `mesh_jobs.cpp:304-318`, `vortex_vm.cpp:50-67`/`:98-100`,
`store.hpp:133`/`:1435`/`:450-457` — and §10.2 (v)'s `documentId` claim checks out against
`historyFromSeal`'s real signature (`publication.cpp:1093-1103`, mints `Store()` at :1097).
**New defects in the v1.13 deltas: 0 BLOCKING, 1 MAJOR, 1 MINOR, 1 NIT.
VERDICT: NOT CONVERGED.**

## Citation verification — v1.13 delta cites (all pass)

- `publication.cpp:853-869` — `globalKeyOf`: `breakMarkerScroll → breakMarkerKey` (:860-861),
  `isLocal() → localSealedAs` else `store.scroll(span.scroll) → scrollKey` (:866-868). Exact;
  the comparand's "minus its `localScroll` case" is accurate — post-remap no span is local
  (`:1049` maps `localScroll → selfScrollInHistory`, an external id).
- `publication.cpp:985-987` — `sealableOps`'s table builder `continue`s on
  `localScroll || breakMarkerScroll` — "skipping rather than rejecting" exact.
- `publication.cpp:1049-1050` — `remap.emplace(localScroll, selfScrollInHistory)` +
  `remap.emplace(breakMarkerScroll, breakMarkerScroll)` — the identity remap, exact.
- `publication.cpp:1048-1086` — `applyOpsSegment` remap + scroll-table loop + per-record
  `op.span.scroll` rewrite (`:1079-1086`) — "carries ops, never identity" exact.
- `publication.hpp:447` — `inline constexpr std::string_view breakMarkerKey = "break:"` — exact.
- `publication.cpp:1093-1103`/`:1097` — `historyFromSeal(segments, from, scrolls)` returns
  `unique_ptr<Store>`, mints `std::make_unique<Store>()` — **no permascroll or `DocumentId`
  parameter**. §10.2 (v)'s unmechanized-`documentId` claim (:2833-2837) and §10.1's
  (:2762-2767) are accurate; `restorePublication`'s `Store(readerPermascroll, pub.storeId)`
  precedent is at `:1127-1128` as cited. (See NIT-1 for "Store-ctor state".)
- `store.cpp:1461-1469` — `Store::scroll` disengages on `localScroll == id ||
  id > externals.size()` — the doc's "*or an id beyond `externals`*" arm is now covered
  (:2092-2093); r13 NIT-1 adopted.
- `store.cpp:3836-3940` — `adoptOpRecords` topo-sort drained by min-`produces`
  `priority_queue` (:3909-3913) — exact.
- `store.cpp:124` — `userPermascroll_(std::make_shared<UserPermascroll>())` in `Store()`
  (:123) — exact (the fresh-permascroll mint backing "each agent its own document lineage").
- `spool.hpp:19` — `localScroll = 0` — exact; `:21-22` gives `breakMarkerScroll = max`.
- `vortex_stdlib.cpp:1953` — `arena().textOf(sym) == symName` — the `d.vars` name walk
  matches *content* — exact; the corrected §6.1 sentence (:1771-1773) is right.
- `vortex_core.cpp:366-369`,`:384-390` — `wingOf`'s `linked(opcode, dims_.grab, side)` entry
  and `bindInput`'s `opcode +d.grab → slot` write — exact.
- `arena_manifold.cpp:1347-1377` — `displacedUs`/`displacedIt` + `setOneSide(..., noCell)` —
  M14's mechanism, exact.
- `arena_manifold.cpp:1867-1868` — `if (!isEphemeral(arena)) { real.emplace(arena, arena); }`
  — the verbatim persistent-ref identity map, exact.
- `arena_manifold.cpp:1947-1951` — `OpHandle` promote: `isEphemeral(target) ?
  real.at(target) : target` — exact; `makeOpHandle` writes the translated target.
- `compiler.cpp:531-532`,`:781`,`:796` — `bindInput(op, cells_->zero)`/`minusOne` —
  `cells_` is re-minted per `emitStatement` (:850-852), so `cells_->zero` is one cell shared
  by *every op in the emission* — the canonical shared `int64` referent; the R13-A rationale
  "routine in practice" is accurate.
- `compiler.cpp:769-772` — `dimConstant` = `literal(int64(findOrMintDimension(name)))` —
  exact (program-side ref-capture arm).
- `mesh_actor.cpp:119-122`,`:156` — `spawn(identity, request, wallNow)` + bare
  `WorkerContext()` — no store. `:182` — `resume(…, ReceivedValue checkpoint, …)`;
  `ReceivedValue.store` is `unique_ptr<Store>` (`mesh_value.hpp:102`). `:188` —
  `!checkpoint.store` in the validity guard. `:260-271` —
  `checkpoint.store->userPermascrollPtr()` at `:261` (read only for that), `WorkerContext(
  …->manifold.get())` at `:270-271`, `setDefaultSpanReader` at `:273` — all exact.
- `worker_context.hpp:52-54` — two-arg ctor `(const Manifold*, const SpanReader*)` — exact;
  `:63` `ArenaManifold arena` mutable member, exact.
- `manifold.hpp:486` — `xanadu::Store *store() const noexcept { return store_; }` — a
  `const` accessor returning *mutable* `Store*` — "the mutable reach" is accurate.
- `arena_manifold.hpp:269-271` — `store()` returns `const xanadu::Store*` falling through to
  `base_->store()` — exact. `:752` — `const Manifold *base_{nullptr}` raw pointer — exact.
- `mesh_jobs.cpp:304-318` — `job.base` arm fetches the base `Store` vs fresh
  `Store(options_.permascroll)` — exact.
- `vortex_vm.cpp:50-67` — `resolveCell` returns the operand cell itself (inCells then
  `CellRef` from `inputs[idx]`, `arena.contains` range check only) — exact.
- `vortex_vm.cpp:98-100` — `setOpcodeKind` writes `opcodeMap_[opCell] = op` — exact.
- `store.hpp:133` — `Store(userPermascroll, const DocumentId &)` ctor param; `:1435` —
  `DocumentId documentId_` member; `:450-457` — `rebuildManifold` returns `Manifold` *by
  value* — all exact.
- `store.cpp:1388-1405` — `addScroll`'s `sameContentAs` dedup returns the existing id —
  exact.
- `mesh_actor_resume_test.cpp:125` — `xanadu::Store store;` fresh-store `publishValue` —
  exact.
- `scroll.cpp:13-22` — `scrollKey` three-arm shape — exact.

## Ruling-adoption verification

- **R13-A adopted correctly and coherently.** `d.captured` is gone from all six minted-dim
  lists; `d.refFor` is restored at §10.1 (:2788) and §14 (:3028). The content-carried
  endpoint is stated consistently in all three sites — §6.1 (:1147-1164), head diagram
  (:1054-1059), genesis (e) (:2200-2228): internal → old→new map (map membership *is* the
  discriminator), external → slot `int64`/`*handleTarget` verbatim, program-side → `+d.name →
  dim` re-resolved through `ensureDimension` (`arena_manifold.cpp:78-105` verified). The
  optional `+d.values`-may-name is correctly conditioned ("only where the endpoint's
  `-d.values` is unclaimed") — it cannot re-invite M14 because a claimed backlink simply
  takes no edge. Genesis (d)→(e) hand-off and the donor-slot endpoint derivation
  (:2216-2228) are internally consistent; no functional hole found in the new model.
- **All adopted minors landed:** comparand `globalKeyOf` arm set (:2086-2094) — verified
  sound arm-for-arm (`localScroll` unreachable post-remap; `vocabularyScroll` is unsealable —
  `sealableOps` throws on the disengaged `store.scroll`, so the arm set is complete);
  comparand reference-side pinning (:2080-2082, :1596-1604) — lineage store supplies the
  reference band, `arena.base()->store()`'s received band is checked; alias
  restated/scoped/multiplicity-stated (:1196-1205, :1225-1228, :1745-1749); dedupe
  faithful-*and*-divergent victims (:1232-1237, :2185-2190 — the replay-dropped-displacer
  case verified mechanically: session append replays as displace exactly when an earlier
  same-alias patch was dropped on its dead target); equal-text `patch:<n>` own-label-cells
  (:1741-1744, :2181-2184); label-cell `-d.name` direction fix (:1767);
  `textOf`-matches-content fix (:1771-1773); `label_of`/genesis-transport handle scope
  (:1763-1764); §9.3 `on Meal` else-seed (:2651, consistent with `set_weight` seeding
  :2448-2449); `learn_plan("Patrol","demo:Guard.cautious",…)` premise (:1218-1219 against
  §9.2's declared clause :2602); §9.3 example re-targeted to declared `Feed` (:2641-2643).
- **§10.2 (v) `documentId`-adoption arm** (:2833-2837) — verified against
  `historyFromSeal`'s real signature; adoption is genuinely unmechanized.

## New findings (v1.13 deltas only)

### Finding 1 — MAJOR

- **Location:** :2114-2121 (§6.6, genesis (a) per-cell classification paragraph); echoed in
  the 1.13 changelog at :3575-3576.
- **Problem:** the adopted r13 minor fix asserts reachability through an edge class this
  same revision deleted. "the walk reaches them through capture edges — a fragment's slot
  link to an agent-`d.vars` binding cell" is impossible under v1.13 on three counts: (i) a
  ref-box slot's referent is `int64` *content*, never a link — that is the entire ref-box
  design (§6.1's focus-slot idiom); (ii) `d.captured` edges — the only edge that ever reached
  a capture referent — are deleted this revision; (iii) §6.1 itself states a `d.vars`
  referent's `-d.values` "is live" (:1149-1151), so not even the optional record
  `+d.values`-may-name edge can name a `d.vars` binding cell — the one edge class left is
  excluded for exactly the cells named. The claim was true in v1.12 (the zigzag lane's minor
  correctly noted `+d.captured` reached them); it was adopted into the same commit that
  abolished the mechanism, unreconciled.
- **Evidence:** §6.1's restated rule — "the referent itself is carried **as content, never
  as an edge into the referent**" (:1147-1148) and the `+d.values` exclusion
  (:1155-1157); `bindInput`/`reference` mint `makeScalarCell(int64(ref))`
  (`vortex_core.cpp:384`, `compiler.cpp:525`,`:531`) — no slot→referent link exists.
- **Concrete fix:** restate the reachability as unreachability-plus-classification — e.g.,
  "named `>E_ops` legs hanging off the agent cell are reached only where a runtime-grown
  `Link` edge leads to them — a `cells"…"` `Link` can mint one — never through capture,
  which rides slot *content* the link walk cannot follow; either way they classify
  `>E_ops`/state-side, so no agent-rooted second walk is owed" — the conclusion survives
  and is in fact stronger (unreachable rather than merely state-classified). Mirror the
  correction in the 1.13 changelog line (:3575-3576).

### Finding 2 — MINOR

- **Location:** :1170, :1678-1680, :1840-1847, :1863-1864 — the same stale "capture
  link/edge" vocabulary surviving R13-A elsewhere.
- **Problem:** four sites still describe captures as links/edges: ":1170 *the copy's capture
  links point at the same definition-site cells*" (slots name them by content — no links);
  :1678-1680 "*capture-linked* binding cells sit behind program-side anchors (… a
  program-side *capture wing*)" — defensible only via the optional record `+d.values` edge,
  which never exists for `d.vars` scalars, and "wing" misnames the recipe chain; :1840-1847
  is the sharpest — closedness arm (a) admits "*a link* to a *state-side* cell — a capture
  or `cells"…"` path" and "a capture *link* landing on a *program-side* cell … the refused
  class": under a literal reading a capture is no link at all, so every capture falls outside
  both arms and *all* capturing fragments become non-`closed` — contradicting the legal
  "captures of *declared fields and member vars*" two sentences later (:1851-1852);
  :1863-1864 "capture edges on the emission's wing/record dims" is loosest but salvageable
  as "the capture structure's edges" (wing links to slots, optional record `+d.values`).
- **Concrete fix:** substitute content-vocabulary — "capture slot"/"captured referent"/"a
  ref-box slot naming a state-side cell" — throughout; at :1840-1847 specifically, arm (a)
  should read "a ref-box slot whose `int64` names a state-side cell" so the admitted class
  actually covers captures.

### NIT-1 — MINOR-adjacent nit

- **Location:** :634-635 and :1550-1551 — "`documentId_` is `Store`-ctor state".
- **Problem:** strictly, `documentId_` has a second writer — `store.cpp:4450` assigns it from
  the store-tables on directory load, beside the three-arg ctor (`store.cpp:135`). The
  substantive claim is untouched (neither writer is reachable through `historyFromSeal`; the
  adoption arm is genuinely unmechanized) — the phrasing just overstates "ctor" exclusivity.
- **Fix:** "`documentId_` is `Store`-ctor-or-load state" or "set only at construction or
  directory load" — one word.

## Non-findings verified (delta claims that hold)

- **Comparand arm set is complete:** post-remap a record's `span.scroll` can only be
  `selfScrollInHistory`, `breakMarkerScroll`, or a table-derived external id
  (`applyOpsSegment` throws on any unmapped id, `publication.cpp:1079-1085`); `localScroll`
  and `vocabularyScroll` are unreachable (the former remapped at `:1049`, the latter refused
  at sealing via the disengaged-`scroll` throw `:992-999`). "Any other disengagement fails
  the compare" closes the `id > externals.size()` arm — and the `scrollKey == nullopt`
  degenerate case is unreachable because every external in a sealed lineage entered through
  the key-keyed scrolls map.
- **Dedupe divergence cases verify mechanically:** enumerated the session/replay matrices —
  declared-victim fold-back re-displaces, grafted-twin session hits reduce to append
  (grafts never fold back), and a session append replays as displace exactly when the
  intervening same-alias displacer's patch was replay-dropped on a dead target (renamed goal,
  pruned clause). Deterministic under the ordinal-ordered replay.
- **`+d.values`-may-name cannot re-invite M14:** the edge is taken only where the endpoint's
  `-d.values` is unclaimed; emission is sequential, so a second record sees the claimed
  backlink and takes none — content carries the endpoint regardless. Opportunistic
  self-description, never load-bearing at replay (:2217 reads the *slot's* content).
- **`documentId` arm is genuinely unmechanized:** `historyFromSeal` mints `Store()` with no
  id path; `documentId_`'s only writers are the three-arg ctor (`store.cpp:135`) and the
  tables load (`:4450`) — neither reachable through the seeding seam. The doc correctly names
  adoption as owed surface, not assumed.
- **Equal-text `patch:<n>` pair is sound:** `plan_named`'s space is clause-rank cells, never
  patch records (:1743-1744), and `unlearn` refuses `d.patch` record args by `-d.patch`
  backlink — the two equal-text label cells cannot be confused by any resolver.
- **dimConstant slots under P16 are consistent:** the slot's `int64` is a `DimRef`, promoted
  verbatim (non-ephemeral arm, `arena_manifold.cpp:1950`), and genesis (e) rebinds it through
  the `+d.name`/`ensureDimension` name arm — `handleTarget` is never needed for that class.

VERDICT: NOT CONVERGED
