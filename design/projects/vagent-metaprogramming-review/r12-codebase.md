# r12-codebase: Round-12 codebase verification of `design/vagent-metaprogramming.md` v1.11

**Orchestrator summary (≤200 words):** All four adopted rulings verified against the tree.
R11-A: the `d.vars` serial precedent exists — `reify`'s `<serial>` is the "durable per-agent
counter on `d.vars>`" (:2176-2178, also §7.2's validate-tag serial :2265-2267); scalar content
promotes verbatim and session-minted `d.vars` members classify state-side, so the counter is
durably reachable — sharing it across `reify:`/`patch:`/validate is collision-free (prefixes
differ). R11-B: `arena.base()->store()` reaches the received band exactly —
`ReceivedValue.manifold` is `store->rebuildManifold(pub.version)` over the runtime's own store
(`mesh_value.cpp:347-351`), bound as `context.arena.base_` at `mesh_actor.cpp:270-271`;
`Manifold::store()` `const noexcept → Store*` (`manifold.hpp:486`), `ArenaManifold::store()`
const-only (`arena_manifold.hpp:269-271`). The owed twin surface is real debt —
`adoptOpRecords` is public (`store.hpp:1369`), `applyOpsSegment` file-local. R11-C records,
R11-D stale-claim fixes, and every moved citation re-verify — except `real.at`/`store.cpp:3841`
line nits. New: **0 BLOCKING, 1 MAJOR, 3 MINOR, 3 NITS. VERDICT: NOT CONVERGED.**

## Citation verification — v1.11 delta cites

Code is byte-identical to the r11 tree (v1.11 commit touched only the doc); all r11-verified
cites stand. New/changed cites checked at the cited lines:

- `manifold.hpp:486` — `[[nodiscard]] xanadu::Store *store() const noexcept { return store_; }`
  — exact: const method returning non-const `Store*`.
- `arena_manifold.hpp:269-271` — `ArenaManifold::store()` returns `const xanadu::Store *`
  (falls through to `base_->store()`) — "const-only" exact; `base()` at `:268` returns
  `const Manifold*`, so `arena.base()->store()` yields the mutable `Store*` as claimed.
- `mesh_value.cpp:347-351` — `restorePublication` store → `rebuildManifold(pub.version)` →
  `ReceivedValue{.manifold}`; `mesh_value.hpp:103` `std::unique_ptr<Manifold> manifold`;
  `mesh_actor.cpp:270-273` binds it as the context arena's base — the received-band store is
  exactly `arena.base()->store()`. The R11-B reach claim is mechanically right.
- `store.cpp:4020` — `sealPendingMetadataAsCells() const` signature. `:4081-4089` — the
  `publishedLocalScroll_ == localScroll && homeCell_ && !externals.empty()` arm minting
  `registerScroll(curHead, *key)` per unregistered external — exact (it iterates *all*
  externals, `self` among them; the doc's "for `self`" reads slightly narrow).
- `manifold.cpp:1474-1475` — `ScrollRegistry Manifold::scrollRegistry(const SpanReader&)`;
  `:1503` — `resolvedScrollIds.insert(store_->publishedLocalScroll())` — the resolved-scroll
  set, exact.
- `store.cpp:4176` — `.publishedLocalScroll = publishedLocalScroll_` written into
  `StoreTables`; `:4454` — `publishedLocalScroll_ = tables.publishedLocalScroll` read back —
  the "storeTables round-trip" readers, exact.
- `store.cpp:1388-1405` — `addScroll`'s `sameContentAs` dedup returns `i+1` — idempotent
  re-add, exact; `externals[0]` = id 1 on a fresh store.
- `publication.cpp:1138-1162` — the ops-segment loop: `for (segment : pub.opsSegments)`
  `:1139`, `metainfo` `:1142`, `Scroll::ofTorrentFile` `:1149-1151`, `resolver.resolve` `:1153`,
  `isVerified`+length `:1154-1155`, `applyOpsSegment` `:1157` — exact shape.
- `scroll.hpp:333` — `std::optional<std::string> scrollKey(const Scroll&)` — exact line.
- `store.hpp:1020-1024` — `scroll(ScrollId)` returns `cpp26::optional<const Scroll&>`,
  disengaged for `localScroll` — see NIT-2.
- `segmented_ops_spool.hpp:272`,`:280`,`:287` — `contains`/`indexOf(MicroversionId)`/
  `idOf(index)`; `store.hpp:677-681` `cellRefOf` — "index … is its CellRef". `produces`-id →
  position translation mechanizes exactly as (b) claims. Doc cites `store.cpp:3841` for
  "opsSpool keys records by id" — see NIT-2 (the `contains` call is `:3840`).
- `vortex_core.cpp:16-18` — `mintNamedDimension` calls `baseDimensionNamed(name)` at `:18`;
  `:349-352` — `baseDimensionNamed` → `arena_.base()->dimensionNamed(name)` — the dead-read
  chain exact; `manifold.hpp:480-483` — `dimensionNamed` reads "using the associated Store as
  the reader; nullopt too when no Store is attached".
- `vortex_core.cpp:119-127` — `exportSymbol`: `makeCell(symbolName)` `:122`,
  `rankTail(moduleCell, dims_.vars) +d.vars→ symCell` `:124-126` — exact; symbol content is the
  bare name as the symbol-arm claim needs.
- `vortex_stdlib.cpp:603`,`:628` — "Preconditions along -d.contract" / "Postconditions along
  +d.contract" comments over `preconditionsOf`/`postconditionsOf(*curOp)` — `d.contract` is
  op-anchored, exact.
- `vortex_core.cpp:29-43` — `initGenesis` adopts `*baseHome`/`baseDims` under `arena_.base()`
  guards; ctor `:14`. `vortex_core.hpp:268`,`:282`,`:287` — `private:`, `initGenesis()`,
  `home_` — exact.
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` → `arenaDims_` cache →
  mint — the "name arm" exact. `:1934-1954` — Int64 verbatim arm + `OpHandle`/`ForeignOpHandle`
  translation arms; `:1950` `real.at(target)` — both cites right.
- `mesh_actor.cpp:188` — `!checkpoint.store` inside the validity guard — "first read at `:188`"
  exact. `:182`,`:261`,`:268`,`:271-273`,`:199,215-216` — re-verified.
- `mesh_actor.hpp:102-106` — `ActorCheckpointView` (continuation/arena/root, no store);
  `:113-116` — `Authorizer`/`Executor` typedefs (Executor's `(const ActorExecution &,
  WorkerContext &)` carries no store); `:132-135` — `spawn` signature — all exact.
- `store.cpp:123` — `userPermascroll_(std::make_shared<UserPermascroll>())` — corrected cite
  exact (`:124` is `chronofilade_`).
- `vortex_vm.cpp:73-82` `mintOpcode` mint+register; `:97-100` `setOpcodeKind`; `:102-114`
  `getOpcodeKind` `asInt64` fallback — exact.
- `compiler.cpp:769-772` — `dimConstant` → `literal(int64(findOrMintDimension(name)))` —
  honest-int64 cells, exact.
- `dim_vector.hpp:23` `noCell = 0`; `manifold.hpp:154-158` `ephemeralBit`/`isEphemeral`;
  `arena_manifold.hpp:468-470` `refOf = ephemeralBit | dense`; `manifold.cpp:427-442`
  `.birthOp = opIndex` (`:434`); `manifold.hpp:295` `birthOp == this CellRef`;
  `vortex_core.cpp:196-229` `writeValue` covers Int64/Double/Bool/None;
  `mesh_value.cpp:485-501` `importValue` OpHandle demote/re-mint;
  `publication.cpp:611-617` `opsAlreadySealed`/`sealableOps`; `vortex_core.cpp:61` `d.name`
  mint; `vortex_core.cpp:113-126` `rankTail +d→member` shape — all verified.
- `vagent-agent-language.md:480` — `d.source | clause → the peer cells that assert it` —
  clause-anchored as claimed; `:478` `d.belief | agent cell → predicate cells` — agent-anchored,
  correctly moved to the agent-leg list.

Pre-existing loose cites, unchanged text (not new defects): `vortex_core.cpp:100-127` for the
"`d.vars` name walk first-match-wins" — the only walk in that range is `findModule`'s
`firstOf`-over-`rankAfter` on `d.stdlib` (`:100-106`); the `d.vars` append is `exportSymbol`
`:124-126`. Mechanism right, cited walk is the sibling rank's.

## Ruling-adoption verification

- **R11-A adopted correctly.** `patch:<n>`'s `<n>` is pinned to "the same per-agent `d.vars`
  counter `reify`'s `<serial>` already rides" (:1158-1160, :1671-1673, :2176-2179) — the serial
  exists (:2176-2177; also the §7.2 validate-tag serial, :2265-2267, names the same counter).
  Durable reach: the counter is a session-minted scalar cell — state-side, Int64 content
  promotes verbatim (the P16 text's own promote claim), re-found via its rank edge across
  resume; a `programCell +d→stateCell` membership edge is the same shape install edges already
  take (`rankTail +d.clause→ stateClause`, :1572-1574). No-recycle semantics consistent:
  `unlearn` drops/replay-drops leave serials consumed (:1746-1747), replay transports
  `+d.name` verbatim and mints nothing (:2068-2073). Label spaces disjoint: `patch:` labels vs
  `reify:`/`src:` prefixes (:2171-2175), `patch:` refused as a `name` argument (:1167-1169) —
  no mint path but the verb.
- **R11-B adopted correctly.** Publish destination = the store holding the received band —
  `arena.base()->store()` (:1524-1535, :1579-1584, :2680-2683, :2703); `ReceivedValue.manifold`
  is built over the runtime's own `restorePublication` store (`mesh_value.cpp:347-351`) and
  bound as `context.arena`'s base (`mesh_actor.cpp:270-271`), so the reach is literal, not
  figurative. The adapter's seeded store is demoted to spawn-side lineage anchor consistently
  across §4.5/§6.6/§10.1/§10.2/P16 ("publish Store"→"lineage Store" everywhere; remaining
  "publish `Store`" hits are inside the 1.8/1.10 changelog entries — history, not stale text).
  Twin debt real: `adoptOpRecords` (`store.hpp:1369`) takes decoded records; `historyFromSeal`
  mints `Store()` internally (`publication.cpp:1097`) — nothing applies a segment into a
  caller-held store. See Finding 1 for where the debt was filed.
- **R11-C adopted correctly.** Ref-capture record shape stated (:1129-1142, :1044-1048,
  :2093-2097): `+d.grab→slot`, `+d.values→referent` (internal/state-side), `+d.name→dim name`
  (program-side, `ensureDimension` name arm `arena_manifold.cpp:78-105`). The `-d.ref`
  single-occupancy argument is the real ZigZag rule — one backlink per (cell,dim,dir), a later
  link evicts — matching M14's own statement (:2736). Program-side referent carried as a name
  string, never a `+d.values` edge — consistent with the state→program-skeleton bar.
- **R11-D adopted correctly.** `plan_named` row now scopes learned clauses to their minted
  `patch:<n>` spelling (:1089); `label_of` reads clause `+d.name` (:1090, :1166-1167); dedupe,
  additivity, zero-hit alias, and grafted-only ambiguity all corrected (:1171-1188,
  :1731-1747). Remaining "same-named learned" text is inside the 1.10 changelog — history.
- **Minors adopted:** seed-pair stakes split read-path vs scroll-identity (:607-621,
  :1468-1477); `self` recovered by `addScroll(from)` dedup (:1472-1474, :2628-2632); (b)'s
  enumeration pinned to `produces` ids / canonical post-fold order with the linear-chain
  invariant (:2033-2048); symbol-arm `module:` composition (:1737-1745); `d.contract` op-anchor
  + agent-leg list + `closed` consumer (:1434-1457); `learn_method` corollary (:1794-1800);
  byte-resolution step (:602-610); cite nits applied.

## New findings (v1.11 deltas only)

### Finding 1 — MAJOR

- **Location:** :1543-1547 ("named as owed surface under §10.2") vs §10.2 itself
  (:2680-2703).
- **Problem:** §6.6's new text rules the kept-in-step adapter twin "is **owed a segment-apply
  surface** `adopt`/`historyFromSeal` do not provide …, named as owed surface under §10.2
  rather than assumed" — but §10.2 never names it. The P16 row (:2703) covers the
  lineage-store seam (spawn base-injection + resume supply/verify) and nothing else;
  the register preamble (:2680-2698) names the lineage store and "owed adapter-API surface
  alongside P4's wake" but no segment-apply surface; P17 is the only deferred entry and is
  unrelated. The phrase "under §10.2" is a false cross-reference, and a new public `Store`/
  `publication` API is exactly the kind of C++ change the house rule requires in the numbered
  Proposal Register — it is unregistered. The disposition's "name it or drop the twin" was met
  in spirit at :1543-1547 but the register it points at lacks the item; an implementer costing
  §10.2 finds no such surface.
- **Evidence:** `grep -n "segment-apply\|twin" design/vagent-metaprogramming.md` — occurrences
  only at :1543-1547 and the changelog :3324-3325; the P16 row's "Where" cell lists
  "`resume` store supply/verify" and no apply surface.
- **Concrete fix:** add the surface to §10.2 — either a P16 "Where" bullet ("a
  `historyFromSeal`/`applyOpsSegment` overload applying a received segment into a caller-held
  `Store`, owed for a kept-in-step twin") or a deferred entry beside P17 — or strike "under
  §10.2" and drop the twin.

### Finding 2 — MINOR

- **Location:** :1179-1180 ("while an unqualified alias always *appends* under its own
  `patch:<n>`").
- **Problem:** over-broad absolute. `graft_*` holds the `name` argument verbatim on `+d.name`
  (:1155), and nothing requires it qualified — a clause grafted as `"travel"` stores `travel`.
  `learn_plan`'s dedupe matches the alias "verbatim against clause `+d.name`s" (:1173) and the
  same paragraph admits the alias "can only hit a **declared or grafted** clause" (:1174) — so
  an *unqualified* alias verbatim-matching a grafted clause's stored label unlink-replaces it,
  contradicting "always appends". The only truly always-append unqualified case is one that
  hits no stored `+d.name`.
- **Evidence:** :1155 (`graft_*` verbatim label), :1173-1175 (verbatim dedupe condition),
  :1185-1186 (same-named grafted clauses acknowledged possible).
- **Concrete fix:** "while an unqualified alias *appends* under its own `patch:<n>` unless it
  verbatim-matches a grafted clause's stored `+d.name`" — or constrain graft names to
  qualified spellings.

### Finding 3 — MINOR

- **Location:** :1677-1679 (patch field set lists `+d.name→label` unconditionally),
  :1666-1669 ("Patch and installed clause share the `patch:<n>` `+d.name` … the patch's label
  names the clause it installed"), :2069-2073 ("the copy's `+d.name` coming verbatim off the
  patch record").
- **Problem:** the `+d.name` story is specified only for clause-installing `learn_*`. For
  `learn_where`/`learn_body`/`learn_method` patches there is no installed clause: does the
  record's `+d.name` carry a fresh `patch:<m>` serial (every patch mints one — and then
  non-clause patches consume serials, worth stating), the *target's* resolved name (which the
  target descriptor already records), or nothing? The replay sentence likewise says every
  replayed install's copy takes `+d.name` verbatim off the record, but a `learn_where` install
  copy is a test graph with no `+d.name` to carry. The text reads unconditionally while the
  rule is arm-conditional.
- **Evidence:** :1156-1157 scopes the mint to "a clause-installing `learn_*`"; :1677 lists
  `+d.name→label` without an arm condition; :2116-2119 shows `learn_where` patches exist with
  no installed clause.
- **Concrete fix:** state the rule explicitly — e.g., "every patch consumes a serial; a
  clause-installing patch's `+d.name` is the shared `patch:<n>`, a non-clause patch's `+d.name`
  is its own `patch:<n>` (self-id)" — or scope the `+d.name` field to clause installs and say
  so.

### Finding 4 — MINOR

- **Location:** :1130-1133 / :1045-1047 — the ref-capture record's slot edge is `+d.grab`.
- **Problem:** `+d.grab` is one of M14's three install-reserved dims — `installed` tests the
  head's `-d.grab`/`-d.spin`/`-d.values` backlink occupancy (:133 glossary, :2736). A capture
  record's `+d.grab→slot` occupies the *slot's* `-d.grab` backlink; a slot cell that is itself
  a fragment head would read `installed` at emission and be refused by every `graft_*`. The
  doc minted `d.patch-head` for exactly this reason (:1679-1680) rather than reusing a
  reserved dim, and justified `+d.grab` only against the `-d.ref` eviction problem, not the
  install-dim overlap. Narrow, but the reserved-dim vocabulary is load-bearing elsewhere.
- **Evidence:** glossary `installed` row (:133) — "any of the head's `-d.grab`/`-d.spin`/
  `-d.values` install-dim backlinks is occupied"; `d.patch-head` precedent (:1679-1680).
- **Concrete fix:** name a dedicated dim for the slot edge (e.g., `+d.ref-slot` or reuse
  `+d.values`+`+d.step` correlation like other recipe fields), or state that ref slots are
  never heads by construction.

### Nits

- **NIT-1** (:1533-1534): "`promote`'s verbatim persistent-ref map (`real.at`,
  `arena_manifold.cpp:1867-1868`)" — the verbatim arm at :1868 writes
  `real.emplace(arena, arena)`; `real.at` is the map *read* (correctly cited at :2090 for
  :1950). Right lines, wrong accessor — `real.emplace`.
- **NIT-2** (:2037-2038): "opsSpool keys records by id, `store.cpp:3841`" — the
  `opsSpool.contains(record.produces)` call is `:3840`; `:3841` is `continue;`. Also
  (:1537-1538) `scrollKey(*store->scroll(span.scroll))` needs a `localScroll` guard:
  `scroll()` returns a disengaged optional for scroll 0 (`store.hpp:1020-1024`), so a
  local-scroll span would deref empty in the comparand.
- **NIT-3** (:1449): `d.weight` listed among the agent cell's "rank dims" — it is a property
  dim (`+d.weight→w` on synapse junctions, :1126), not an agent→members rank like
  `d.patch`/`d.synapse`/`d.belief`. Harmless to the walk (the edge dim is needed regardless)
  but mischaracterized; "its rank dims (`d.patch`, `d.synapse`, `d.belief`, …) plus property
  dims (`d.weight`)" reads cleaner.

## Non-findings verified (delta claims that hold)

- **Shared counter is safe:** `reify:<src>:<serial>` (:2158), `patch:<n>` (:1158), and the
  §7.2 validate-tag serial (:2265-2267) all draw the one `d.vars` counter — monotone draws can
  never collide across differently-prefixed label spaces, and tags are not labels at all. The
  counter's durability rides the same promotion path the doc already needs (session-minted
  scalar, Int64 verbatim promote, state-side classification :1570-1574); its anchor cell is
  unnamed but either anchor (agent cell or module `d.vars`) mints only a program→state
  membership edge, the shape install edges already take.
- **Replay transports, never re-derives:** (d) at :2068-2073 — the copy's `+d.name` comes
  verbatim off the patch record; no patch minted, no serial consumed. Consistent with
  `unlearn` never reclaiming `<n>` (:1746-1747) and ordinal-order replay (:2068, :2122).
- **Dedupe consequence stated right:** a qualified alias `demo:Forager.travel` hits a declared
  clause's stored qualified `+d.name` (clause labels are qualified per :2205-2206), unlinks it,
  installs `patch:<n>` — the qualified name then dead-resolves (:1176-1179); `learn_where`/
  `learn_body` named as the name-preserving path. Consistent.
- **Ref-capture `+d.values` bar holds:** a recipe record on a `d.code`-registered head
  `+d.values`-linking into a ≤E_ops dim cell would be a state→program-skeleton edge — the V12
  bar the doc names; `+d.name` carrying the dim's *name string*, re-resolved by
  `ensureDimension`'s find-or-mint (`arena_manifold.cpp:78-105`), is the consistent escape.
- **Seed-loop borrow is accurate:** `restorePublication`'s loop resolves bytes via
  `metainfo`→`ofTorrentFile`→`resolver.resolve`→`isVerified`+length
  (`publication.cpp:1142-1155`); skipping the signed-inventory checks at seeding is defensible
  — the descriptor list ships inside the signature-covered publication, and the (a) comparand
  guards the resume arm.
- **`checkpointView`/`Executor` carry no store:** `mesh_actor.hpp:102-106`,`:113-116` — exact.
- **`mesh_actor.hpp:132-135`** — `spawn` signature exact; accepts no store.
- **1.10 changelog entries retained** describing then-current `d.patch` ordinals and
  `same-named` claims (:3256-3267, :3302-3312) are version history, not live text — correctly
  left as written.

VERDICT: NOT CONVERGED
