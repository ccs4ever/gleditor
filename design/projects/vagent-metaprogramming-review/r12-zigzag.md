# R12 — ZigZag topology/promotion critique (v1.11)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.11 deltas in my lane: the ref-capture
record shape (`+d.grab`→slot correlation, `+d.values`/`+d.name` endpoint arms — schema :1046-1054,
mechanism :1129-1138, rehydration consumer :2091-2097), the `patch:<n>` durable `d.vars` serial +
replay-verbatim label transport (:136, :390, :1155-1169, :1670-1747, :2068-2081, :2176-2179,
:2266, :2530-2531), the verify-arm publish destination `arena.base()->store()` (:1525-1547,
:1579-1600), the `d.contract` op-anchored correction + completed agent-leg list + named second-walk
consumer (:1430-1457, :2006-2016), the seed-pair stake split + `addScroll(from)` self-recovery
(:601-633, :1468-1477), and the (b) position-space pin (:2030-2065).

Verified against `vortex_core.cpp:354-414` (`wingOf` reads `linked(opcode, dims_.grab, side)` first
— `:366-367` returns `{}` when the wing head is absent; `bindInput` first slot takes
`opcode +d.grab → slot` at `:385-390`, so a first input slot's `-d.grab` backlink is the opcode;
`bindOutput` first slot takes `opcode -d.grab → slot` `:398-410`, leaving the slot's `-d.grab` free),
`arena_manifold.cpp:1349-1372` (`link` clears the displaced occupant's side — the eviction the doc
invokes as M14), `arena_manifold.cpp:77-105` (`ensureDimension` name arm: `dimensionNamed`
base/cache resolution before any mint, `:55-75` + `:80-88`), `vortex_stdlib.cpp:603`,`:628`
(`preconditionsOf`/`postconditionsOf` read `-d.contract`/`+d.contract` chains off `*curOp`, an op
cell — also the wing read shape at `vortex_core.cpp:445`,`:453`), `manifold.hpp:486`
(`Manifold::store()` `const noexcept` returning non-const `Store*`), `arena_manifold.hpp:269-272`
(`ArenaManifold::store()` const-only, falls through to `base_->store()`), `mesh_value.hpp:69-74`
(`publishValue(xanadu::Store &store, …)` — any Store&), `mesh_value.hpp:98-105`
(`checkpoint.store` `unique_ptr`), `mesh_actor.cpp:181-189` (`resume` signature, `!checkpoint.store`
guard at `:188`), `:199` (`continuation->programRef != request.programRef`), `:215-216`
(`grant->programRef != continuation->programRef`), `:260-273` (base-only `WorkerContext` +
post-construction `setDefaultSpanReader`), `worker_context.hpp:51-54` (reader installed before
`core` in member-init order), `store.cpp:1388-1405` (`addScroll` `sameContentAs` dedup → `i+1`),
`publication.cpp:1097-1102` (`historyFromSeal` keeps `addScroll(from)`'s return internal),
`publication.cpp:1048-1050` (`remap.emplace(localScroll, selfScrollInHistory)` — no folded span
keeps `localScroll`), `publication.cpp:1138-1162` (`restorePublication`'s per-segment resolve loop:
metainfo → file-coords → `Scroll::ofTorrentFile` → `resolver.resolve` → `isVerified`+length),
`store.cpp:4081-4089` (`sealPendingMetadataAsCells`' `publishedLocalScroll_ == localScroll` arm
mints `registerScroll`), `manifold.cpp:1502-1503` (`scrollRegistry`'s resolved-scroll set),
`store.cpp:4176`,`:4454` (storeTables write/read of `publishedLocalScroll_`), `store.cpp:3836-3940`
(`adoptOpRecords` Kahn topo-sort, `inDegree`/`ready`; `opsSpool.contains(record.produces)` `:3841`),
`store.hpp:677-681` + `opsSpool.indexOf/idOf` (`store.cpp:175`,`:357`,`:364`,`:429`), `manifold.hpp:295`
(`birthOp` == the CellRef), `arena_manifold.cpp:2010-2017` (promote emits links last),
`arena_manifold.cpp:1867-1868` (`real.emplace(arena, arena)` persistent identity-map), `:1992-1997`,
`:2041` (`real.at` outputs shipped verbatim), `mesh_value.cpp:227-228`,`:241` + `publication.hpp:340`
(`opsAlreadySealed = 0` default → `1..opsTotal`, `publication.cpp:611-617`), `mesh_value.cpp:519-523`
(mergeValues `(contentHash, authorKey, parent)` order), `mesh_host.cpp:507`,`:509-510` (job.base
store vs fresh destination), `mesh_actor_resume_test.cpp:125` (fresh `Store` publish fixture),
`vagent/compiler.cpp:484-486` (`cells"…%"` mutating-path refusal), `:768-772`
(`dimConstant` = `literal(int64(findOrMintDimension(name)))`), `vortex_core.cpp:99-127`
(`findModule`/`exportSymbol` — `makeCell(symbolName)`, `sym +d.values → entryOp`, rankTail same-dim
`d.vars` chain), `vortex_vm.cpp:98-100` (`setOpcodeKind` writes `opcodeMap_`), `:102-114`
(`getOpcodeKind` `asInt64` fallback), `mesh_evaluator.cpp:658-663` (`program.instructions` drives
`setOpcodeKind` registration), `:730-737` (per-ref `d.mesh.relocation→"cell"` mint inside loop),
`foreign_op_handle.cpp:98-117` (`isCellRelocationHandle` = OpHandle kind + marker edge),
`result_slice.cpp:35` (`d.source` = the provenance dim the spec means).

## Verdicts on the r11 items

### R11-C — ref-capture record shape: **enumerated and the program-side arm is sound, but the correlation edge is occupancy-unsafe — residual MAJOR (Finding 1)**

§6.1's recipe enumeration now names the fourth record kind — "link tuples + literals + capture
records + ref-capture records — one per `+d.refs` ref-record" (:1046-1050) — the mechanism states
the pairing (:1129-1138), and (e) names it as the rehydration source (:2091-2097). The enumeration
gap from r11 Finding 1 is closed.

The **program-side arm is the right construction**: `+d.name → the referent dim's name string`,
re-resolved through `ensureDimension`'s name arm (`arena_manifold.cpp:78-105`, verified — resolves
base dims via `dimensionNamed`'s reader/`store_`/`base_->store()` cascade then the `arenaDims_`
cache before any mint). The recorded edge lands on a fresh *label cell*, never on the dim — so no
edge runs from a `d.code`-registered head's record into a program-skeleton cell: the
`d.code`-rank invariant (:1417-1419) and `closed`'s refused class are both avoided. It is also the
*complete* program-side arm: `closed` already refuses every program-side reference that lacks a
name-record (:1749-1757), so the only program-side referents a ref-record can legally carry are
folded dims — the class `d.dims`' named exclusion (:1443-1446) puts outside both walks — and
`ensureDimension` covers exactly that class. Cross-check passes: a dim referent ships through
`promote`'s OpHandle path verbatim (`isEphemeral(target) ? real.at(target) : target`,
`arena_manifold.cpp:1947-1951` — a persistent dim folds back at its own position), so the name-arm
re-resolution and the promoted handle converge on identical bits.

What remains wrong is the *correlation* encoding — Finding 1.

### R11-A — `patch:<n>` durable serial + verbatim replay: **resolved**

`<n>` is now the durable monotone serial off the `d.vars` counter `reify`'s `<serial>` rides (:136,
:390, :1158-1162, :1671-1672, :2176-2179), never the rank position, never reclaimed across
`unlearn`/replay-drop/resume (:1745-1747). Replay transports the `+d.name` verbatim off the patch
record — no mint, no re-derived `<n>` (:2068-2073). The `learn_*` dedupe consequence is stated and
correct: a *qualified* alias matching a declared clause's stored `+d.name` unlinks it and installs
`patch:<n>`, the user name going dead; name-preserving replace routes through
`learn_where`/`learn_body` (:1173-1181, :3319-3321). `plan_named`'s space stays clause-rank cells,
so patch records sharing the `patch:<n>` spelling cannot collide (:1674-1676); `patch:` is refused
as a user `name` at the verbs and reserved at module declaration (:1167-1169, :2171-2175), so the
first-`:` shape test can never reroute a `patch:<n>` lookup down the qualified path. Consistent end
to end. One residue — the "no serial is consumed" sentence overclaims against §6.7's blanket
`reify` label mint (Finding 2), and two stale-phrasing nits (Findings 3-4).

### R11-B — verify destination `arena.base()->store()`: **resolved**

The comparand/publish store is now the one holding the *received* band — the runtime's base store
`arena.base()->store()` (:1528-1531, :1579-1584) — with the const-reachability spelled correctly:
`Manifold::store()` is `const noexcept` yielding non-const `Store*` (`manifold.hpp:486` — so the
const `base()` pointer suffices for the `publishValue(Store&…)` destination), while
`ArenaManifold::store()` is const-only (`arena_manifold.hpp:269-272`) and covers the *read* arm
(`arena.store()`'s band, :1593-1595). The seeded adapter store is demoted to its true role —
spawn-side lineage anchor and (a)'s reference comparand (:1524-1525, :1990-1992) — and the
kept-in-step twin is named as owed surface: no `adopt`/`historyFromSeal` applies a received segment
into a caller-held store (:1543-1547). The `promote`-writes-dangling-positions motivation is
stated accurately (`real.at` verbatim persistent map, `arena_manifold.cpp:1867-1868`,`:2041`,
:1533-1535). Verified — no residue.

### `d.contract` fix / agent-leg list / second-walk consumer: **resolved**

`d.contract` stays in the program-walk vocabulary with the correct justification — its
`-d.contract`/`+d.contract` precondition/postcondition chains anchor on *op cells*
(`vortex_stdlib.cpp:603`,`:628`; the wing read `dims_.contract` off the opcode,
`vortex_core.cpp:445`,`:453`) — and is explicitly excluded from the agent-leg list (:1449-1452).
`d.source` moved to the program list ("anchors on clause cells" — spec `clause → peers`,
`vagent-agent-language.md:480`, `result_slice.cpp:35`); the inclusion is safe even if it is
over-broad, since only persistent `≤E_ops` refs can classify program-side regardless of walk
reach. `d.belief` is *correctly retained* in the agent legs — it is a genuine agent-cell rank
(spec :478) — alongside the newly added `d.patch`, `d.synapse`, `d.weight`. The second walk's
consumer is now named: `closed`'s frontier enumeration, nothing else (:1455-1457, :2012-2016).
All three r11 Finding-2 asks are satisfied.

### Seed pair / self-recovery / (b) position space: **resolved**

The stakes are split correctly: `setContentSource` carries the read-path/dead-read stake
(artifact-local spans resolving `""` → `initGenesis` minting ephemeral twin dims, :612-618),
`bindPublishedLocalScroll` the scroll-identity stake (`sealPendingMetadataAsCells`'
`publishedLocalScroll_ == localScroll` arm, `store.cpp:4081-4089`; `scrollRegistry`,
`manifold.cpp:1503`; storeTables, `store.cpp:4176`,`:4454`). The `self` recovery is mechanized:
`historyFromSeal`'s internal `addScroll(from)` is the fresh store's `externals[0]`, so a second
`store->addScroll(from)` returns the fold-internal id via `sameContentAs` dedup
(`store.cpp:1388-1405`) — `bindPublishedLocalScroll(store->addScroll(from))` spells correctly
(:619-621, :1472-1474). The seed step's bytes-resolve through `restorePublication`'s loop shape
(`publication.cpp:1138-1162`) is accurate, signed-inventory checks named as skipped under
signature-on-decode + the (a) comparand (:603-608). (b)'s enumeration is pinned to `produces`
`MicroversionId`s resolved on the received fold (`opsSpool` keyed by id, `store.cpp:3841`;
`indexOf`/`idOf` translate, `store.cpp:357`,`:364`) — or positions in the canonical post-fold order
*under the named precondition* that the image appends formed a linear `parent` chain
(:2033-2043); `adoptOpRecords`' topo-sort (`store.cpp:3836-3940`) and why a value scan is the wrong
shape (`dimConstant` cells aliasing op kinds, `compiler.cpp:768-772`) are both stated.

## New findings (v1.11 deltas only)

### 1. MAJOR — the ref-capture record's `+d.grab`→slot correlation edge evicts the op's input-wing head on any first-position input slot; `+d.values`→referent has the same occupancy hole on shared referents

The record pairs itself to the ref-record by `+d.grab → the same slot cell` (:1047-1049,
:1130-1132), justified by "`+d.ref` … the slot's `-d.ref` backlink is single-occupancy" — the M14
reasoning is right about `-d.ref` but the chosen replacement dim is occupied too. A marked ref
cell is a *wing slot* — `bindInput` links the first operand `opcode +d.grab → slot`
(`vortex_core.cpp:385-390`), so a **first-position input slot's `-d.grab` backlink is the opcode
itself**. Writing `record +d.grab → slot` evicts it (`link` clears the displaced occupant,
`arena_manifold.cpp:1357-1368`), and the eviction is catastrophic, not local: `wingOf` reads
`linked(opcode, dims_.grab, POS)` *first* and returns `{}` when it is absent
(`vortex_core.cpp:366-367`) — the op loses its *entire* input wing, not one operand. This happens
at *emission* (the recipe is written onto the live emitted head) and again at *replay*, where the
copied op's `+d.grab → slot'` and the copied record's `+d.grab → slot'` race for the slot's single
`-d.grab` — whichever links second wins, either severing the copy's input wing or silently
dropping the correlation the (e) sweep needs. Only first-input-wing slots collide (chained slots'
`-d.grab` is free; output-wing slots' `-d.grab` is free — `bindOutput` occupies `+d.grab`,
`vortex_core.cpp:405-410`), but nothing in the schema bars a marked ref — a `dimConstant` operand,
a `cells`/`Value`-written focus slot — from first position, and first position is where every
op's first operand lands.

The same single-occupancy hole sits on the other endpoint: `+d.values → the referent endpoint`
(:1132) writes the referent's `-d.values` backlink — two ref-capture records on one head naming
the *same* internal/state-side referent (two slots capturing one binding is ordinary) evict each
other, and a referent already held as some record's or binding's `+d.values` target has that live
edge severed at emission.

Fix, mechanical: (a) correlate positionally — the pairing is already 1:1 ("one per `+d.refs`
ref-record"), so state that the *k*-th ref-capture record on `+d.recipe` pairs the *k*-th
ref-record on `+d.refs` (both chains are `+d.step`-ordered by one enumeration) and drop the
`+d.grab` edge entirely; or (b) mint a dedicated dim for the pairing edge (`d.refcap`-style — the
house preference is more dimensions over overloaded ones), whose `-d` backlink is free on wing
slots by construction; and (c) for the referent arm either keep `+d.values` behind a per-record
fresh indirection cell (the descriptor records' own `+d.vars`-key idiom, :1028-1035) or state the
one-referent-per-record uniqueness the bare edge requires.

### 2. MINOR — "(d) replay… no serial is consumed" contradicts §6.7's blanket `reify` label mint

":2068-2069 replay does not re-record (replayed grafts mint no patch cells, so no serial is
consumed)". But each replayed install `reify`s a registered head into the install copy
(:2069-2071), and §6.7's rule is unconditional — "`reify` labels `reify:<src>:<serial>`" (:2157)
— so every replayed install copy draws the shared `d.vars` serial for its own label unless the
label mint is registration-only. The consumption is harmless (monotone either way — patch
ordinals stay unique), but the flat claim is wrong under one reading and unstated under the
other. Fix: one clause — either install-copy reifies carry no `reify:` label (the label exists
for registration dedup; install copies are never registered), or the serial advances at replay
too, harmlessly.

### 3. MINOR — "Patch and installed clause share the `patch:<n>` `+d.name`" reads against M14's single-occupancy

":1674-1676 share the `patch:<n>` `+d.name` intentionally". One label cell cannot hold two
`+d.name` parents — the second `+d.name → label` write evicts the first (the memo-record
convention already states the fix: "a fresh text cell per record too, since an interned one would
be evicted between records", :1656-1657). "Share" must mean *equal text on two label cells*; as
written an implementer could link one. Fix: "carry the same `patch:<n>` text on their `+d.name`
labels — one fresh label cell each, per the single-occupancy convention".

### 4. MINOR — `unlearn`'s by-name match inherits the grafted-twin ambiguity the text scopes to `plan_named`

":1185-1187 the surviving `+d.name` ambiguity is same-named **grafted** clauses only — where
`plan_named(name)`'s first-hit can misbind". The same ambiguity reaches `unlearn`: two grafted
clauses carrying the verbatim name `X` resolve identically by the reverse read (:1731-1734), so
`unlearn(twin-cell)` pops the latest patch recording `X` regardless of which twin the caller
meant — name-matching is the stated contract, but the twin ambiguity is the same class the doc
names for `plan_named`. Fix: extend the sentence — "`plan_named`'s first-hit and `unlearn`'s
latest-name match alike cannot pick between same-named grafted twins".

### 5. NIT — "the ordinal read off the `d.patch` rank" phrasing invites the rank-position misreading the fix removes

":1166 `plan_named("patch:<n>")` with the ordinal read off the `d.patch` rank" — the ordinal is a
*recorded field* on the patch's descriptor (:136, :390), not the member's rank position; "off the
rank" is exactly the derivation this revision exists to forbid. Fix: "the ordinal read off the
patch record on the `d.patch` rank". (Adjacent nit: `mesh_actor.cpp:182` for the `ReceivedValue
checkpoint` parameter — the param is on `:184`; `:182` is the signature line.)

## Verified accurate (v1.11)

`ensureDimension` name arm resolves base dims + `arenaDims_` before minting
(`arena_manifold.cpp:55-75`,`:80-105`); `d.contract`/`d.source` op/clause-anchored
(`vortex_stdlib.cpp:603`,`:628`; `vortex_core.cpp:445`,`:453`; `result_slice.cpp:35`);
`Manifold::store()` `const noexcept` → non-const `Store*` (`manifold.hpp:486`);
`ArenaManifold::store()` const-only fall-through (`arena_manifold.hpp:269-272`); `publishValue`
takes any `Store&` (`mesh_value.hpp:69-74`); `checkpoint.store` `unique_ptr`
(`mesh_value.hpp:98-105`); `resume` first `checkpoint` read = `:188` `!checkpoint.store` guard,
consumption for `userPermascrollPtr()` `:261`, base-only `WorkerContext` `:271`, post-ctor
`setDefaultSpanReader` `:273`; `WorkerContext` two-arg ctor installs reader before `core`
(`worker_context.hpp:51-54`); `programRef` pins `mesh_actor.cpp:199`,`:215-216`; `addScroll`
dedup `store.cpp:1388-1405`; `historyFromSeal` `Store()` mint + internal `addScroll(from)`
`publication.cpp:1097-1102`, decl `publication.hpp:605-612`; scroll remap `localScroll → self`
`publication.cpp:1048-1050`; `restorePublication` resolve loop `publication.cpp:1138-1162`, pair
`:1129-1131`; `sealPendingMetadataAsCells` arm `store.cpp:4081-4089`; `scrollRegistry`
`manifold.cpp:1502-1503`; storeTables `store.cpp:4176`,`:4454`; `adoptOpRecords` topo-sort +
`opsSpool` id-keyed `store.cpp:3836-3940`,`:3841`; `indexOf`/`idOf` `store.cpp:357`,`:364`,`:429`;
`birthOp` == CellRef `manifold.hpp:295`; promote links-last `arena_manifold.cpp:2010-2017`;
`real.emplace(arena, arena)` `:1867-1868`; `real.at` shipped verbatim `:1992-1997`,`:2041`;
`sealLocalSpool` `opsAlreadySealed = 0` `publication.hpp:340`, `mesh_value.cpp:227-228`,`:241`;
`mergeValues` order `mesh_value.cpp:519-523`; `mesh_host.cpp:507`,`:509-510` job.base arms;
`mesh_jobs.cpp:304-318` shared-store pattern; `mesh_actor_resume_test.cpp:125` fresh-Store
fixture; `dimConstant` `vagent/compiler.cpp:768-772`; `cells"…%"` refusal `:484-486`;
`exportSymbol` `makeCell(symbolName)` + `+d.values`→head + same-dim `d.vars` rankTail
`vortex_core.cpp:117-127`; `insertIntoRank`/`rankTail` same-dim chains
`arena_manifold.cpp:1423-1439`, `cell_views.hpp:258`, `vortex_stdlib.cpp:4238-4241`;
`setOpcodeKind`/`getOpcodeKind` `vortex_vm.cpp:98-114`; `program.instructions` precedent
`mesh_evaluator.cpp:658-663`; per-ref marker mint `:730-737`; `isCellRelocationHandle`
`foreign_op_handle.cpp:98-117`; `sealedOpsName` `provenance.hpp:286`; `documentId_` `Store`-ctor
state `store.hpp:131-133`,`:1435`; `d.belief`/`d.source` agent-rank attribution
`vagent-agent-language.md:225`,`:478`,`:480`.

VERDICT: NOT CONVERGED
