# R11 — System Architect critique (v1.10)

Scope: layering, ownership, adapter-vs-core boundaries; verdict on my r10 major + minor (R10-C —
seed ceremony hydration, adopt-`documentId` arm, "expose neither" narrowing) plus new defects in
the v1.10 deltas only. Re-verified against the tree (`publication.{hpp,cpp}`, `store.{hpp,cpp}`,
`mesh_actor.cpp`, `mesh_value.{hpp,cpp}`, `mesh_jobs.cpp`, `worker_context.hpp`,
`manifold.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `resolver.cpp`,
`tests/xuzz/mesh_actor_resume_test.cpp`).

## R10-C verification — FIXED (major and both adopted nits)

- **Seed ceremony is now `restorePublication`'s full shape.** §4.5 (`:605-613`) and §6.6
  (`:1415-1420`) both state it verbatim: "`historyFromSeal` is the bare fold and installs no span
  source", so the seeded store is "**hydrated like any restored history** — `setContentSource` on
  the adapter's content source (the lease pinned for the actor's life, the role
  `actor->sourceLease` plays at `mesh_actor.cpp:268`) plus `bindPublishedLocalScroll(self)` —
  `restorePublication`'s own pair at `publication.cpp:1129-1131`", with the failure consequence
  named ("every artifact-local span resolves to `\"\"` … `initGenesis` mints ephemeral twin
  dims"). The cited mechanics are exact: `restorePublication` performs
  `setContentSource(&source)` at `publication.cpp:1129` and `bindPublishedLocalScroll(self)` at
  `:1131`; `historyFromSeal` (`:1093-1103`) performs neither; `applyOpsSegment`'s remap leaves no
  span local (`:1048-1049`,`:1079-1086`). Order also works: `setContentSource` hydrates *existing*
  externals eagerly (`store.cpp:1425-1433` iterates `externals`) and `scroll()` lazy-hydrates on
  read (`:1466-1467`), so calling it after `historyFromSeal` covers every scroll the fold added.
- **Adopt-`documentId` named unmechanized.** `:621-631` and `:1425-1435` state it exactly as
  ruled: "`historyFromSeal` mints `Store()` internally with no permascroll or `DocumentId`
  parameter (`publication.cpp:1097`, decls `publication.hpp:606-612`), `applyOpsSegment` is
  file-local so the apply cannot wrap a pre-minted store, and `Store()` mints a fresh
  `UserPermascroll`
  (`store.cpp:123-124`)" — the fresh-mint arm mechanized, adoption owed as a `historyFromSeal`
  overload or `restorePublication` seed "alongside the supply-side seam". Verified:
  `Store(permascroll, documentId)` exists (`store.hpp:130-133`) but nothing reachable calls it
  with the artifact id except `restorePublication` (`publication.cpp:1127-1128`).
- **"Expose neither" narrowed.** `:1436-1445` and `:2550-2557` now carry the transport
  distinction: "`resume` already transports a caller `Store` inside `ReceivedValue checkpoint`
  (`mesh_actor.cpp:182`, consumed at `:186`,`:261`, read only for `userPermascrollPtr()` and
  dropped before `receiveValue`'s rebuild), so the owed change is the fold *trusting* the
  supplied store or comparing it, not new transport — and `checkpoint.store` is a `unique_ptr`
  (`mesh_value.hpp:98-105`), so 'supply' of the adapter's *kept* store implies shared ownership
  or a second seeded product." Verified verbatim: `resume(…, ReceivedValue checkpoint, …)`
  (`mesh_actor.cpp:182-186`), `!checkpoint.store` gate `:188`, `userPermascrollPtr()` read `:261`,
  `receiveValue` rebuilds a *new* store via `restorePublication` (`mesh_value.cpp:345-346`) and
  `actor->checkpoint` keeps that one (`mesh_actor.cpp:269`) — the caller's store is dropped.
  `ReceivedValue.store`/`manifold` are `unique_ptr`s (`mesh_value.hpp:102-103`).

## Is the unreachable-object seam chain closed? — almost; one object still hidden under verify

Walking every boundary the adapter must name:

| Boundary | Object needed | Reachable? |
| --- | --- | --- |
| load | sealed `ops` segment **bytes** | `pub.opsSegments` are torrent-coord descriptors (`publication.hpp:196-203`) — the adapter must run `restorePublication`'s resolve loop shape (`:1139-1160`) to get bytes; unstated (minor 4) |
| load | `from` scroll + `scrolls` map | `pub.scrolls.at(pub.historyScroll)` + `pub.scrolls` — `publish()` sets both when `opsSegments` is non-empty (`publication.cpp:1301-1302`), `publishValue` carries them (`mesh_value.cpp:239-247`) ✓ |
| load | the `Store` itself | `historyFromSeal` → `unique_ptr<Store>` ✓ |
| hydrate | `self` for `bindPublishedLocalScroll` | not returned by `historyFromSeal` — recovered by the idempotent re-add `store->addScroll(from)` (`store.cpp:1388-1405`, `sameContentAs` identity); unstated (minor 3) |
| bind | `Manifold` base + `SpanReader` | `rebuildManifold`/`rebuildManifoldFromIndex` by value (`store.hpp:450-457`), pinned `unique_ptr`-stable for raw `const Manifold *base_` (`arena_manifold.hpp:752`); `Store : public SpanReader` (`store.hpp:128`) through `WorkerContext`'s two-arg ctor (`worker_context.hpp:52-55`) ✓ — `initGenesis` dims read through `base->store()` (`manifold.cpp:824-829`), the `defaultReader_` arg serves the arena's own reads; both are the same hydrated store |
| genesis (a) | both `1..E_ops` bands | adapter's own store + `arena.store()` → `const Store*` (`arena_manifold.hpp:269-271`) → `getCompactOp`/`opCount` public const (`store.hpp:179`,`:985`) ✓ |
| genesis (b) | recorded instruction enumeration | positions on the `programRef` bind record, recorded at emission (`:1924-1928`) — the `program.instructions` precedent `mesh_evaluator.cpp:658-663` verified ✓ |
| genesis (d)/(e) | mutable arena | the adapter's `Executor` callback gets `*actor->context` inside `pumpOne` (`mesh_actor.cpp:473-477`) ✓ |
| **publish** | **the store holding the live band** | **under supply: the adapter's ✓; under verify: `arena.base()->store()` = the runtime's rebuilt store, reachable only through `Manifold::store()`'s mutable const-hole (`manifold.hpp:486`) — finding 1** |
| resume | supply-or-verify surface | honestly owed, named like P4's wake ✓ |

So the chain is closed *under the supply arm*; under verify the last store in the chain — the
publish destination — is still the runtime's private object, and the doc's own sentence points at
the wrong one. Finding 1.

## New findings (v1.10 deltas only)

### 1. MAJOR — under the verify arm, the publish destination is unnamed and the doc's literal answer corrupts: `publishValue` must run on the store holding the received band (`arena.base()->store()` = the runtime's rebuilt store), not the seeded store (§6.6 `:1504-1507`; §10.1 `:2517-2519`)

**Problem.** v1.10 says: "`publishValue` runs against the adapter's own lineage store — the
`Store` it seeded at spawn and supplies, or verifies the rebuilt counterpart of, at `resume`"
(`:1504-1507`). Under the **verify** arm those are two different stores, and only one can serve:
the adapter's seeded store holds `[image 1..E_ops]` and nothing else, while the live arena's
persistent state cells sit at positions `E_ops+1..R` in the *received* fold's numbering.

The mechanism that makes the seeded store the corrupt destination is `promote`'s identity map:
persistent cells `real.emplace(arena, arena)` verbatim (`arena_manifold.cpp:1867-1868`), so the
emitted ops *name the received positions* — and promote even gates handle targets on
`store.getCompactOp(*target)` existence (`:1809`), proving the destination must already hold
every referenced position. Publish into the image-only seeded store and the emitted ops name
positions `E_ops+1..R` that were never written in it — **the exact failure the doc itself names
twenty lines later** ("a fresh-store publish over a persistent base writes ops naming positions
that never existed in it", `:1522-1527`) — and the (a) comparand cannot catch it downstream,
because it compares only `1..E_ops`; the corruption lives above the band.

The two coherent arms, neither currently written down:

- **(i) publish into the rebuilt store** — the store the arena's base actually folded from,
  `arena.base()->store()`. This is the in-tree precedent the doc already cites:
  `mesh_jobs.cpp:314` sets `store = base->store.get()` and `:322` publishes into it — the
  `job.base` shared-store pattern *is* "publish into the store behind the base". Reachable today
  but only through the mutable const-hole: `ArenaManifold::store()` returns `const Store*`
  (`arena_manifold.hpp:269-271`), so the mutable reach is `arena.base()->store()` —
  `Manifold::store()`'s `const noexcept → Store*` leak (`manifold.hpp:486`) the doc wants to
  sanction, not lean on. Mutating `actor->checkpoint`'s private store through it is exactly what
  "keeps its internally built one private" exists to prevent.
- **(ii) keep the adapter's store in step** — fold each received checkpoint's ops segments into
  the kept store so it holds the full band, then publish into it. **Unmechanized today**:
  `applyOpsSegment` is file-local (`publication.cpp:1023-1091`), `historyFromSeal` and
  `restorePublication` both mint fresh stores, and `adopt()` transcludes `pieces` only, never
  `opsSegments` (`:1194-1278`). A segment-apply-into-existing surface would have to join the owed
  seam alongside the `documentId` overload.

**Fix.** One clause pinning the destination per arm — e.g.: "under supply the publish store is
the adapter's; under verify it is `arena.base()->store()` — the rebuilt store behind the base,
published into through `Manifold::store()`'s mutable return pending the sanctioned seam (the
`job.base` pattern, `mesh_jobs.cpp:314`,`:322`) — or the adapter keeps its store in step, which
owes a segment-apply-into-existing surface no public function performs today (`applyOpsSegment`
file-local, `adopt` transcludes pieces only)." As written, an implementer taking "the adapter's
own lineage store" literally under verify builds the corrupt path and no stated test detects it.

### 2. MAJOR — `patch:<n>`'s `<n>` is positional while `d.patch` compacts on `unlearn`/replay-drop: the verb's mint reuses freed ordinals, colliding with live `patch:<m>` labels — durable-label ABA (§6.1 `:1130-1132`; §6.6 `:1590-1591`)

**Problem.** v1.10 mints the installed clause's `+d.name` as `patch:<n>` where "`<n>` = the
`d.patch` ordinal the call creates" (`:1130-1131`; "the new patch's ordinal" `:1590`) — a rank
*position*. But the rank does not keep its dead: `unlearn` "undoes" the latest matching patch
(`:1089`,`:1643-1646`) and "dead patches are dropped" (`:1584-1585` — their cells "stay allocated
until reclamation" `:2711-2713`, i.e., unlinked from the live rank), and genesis replay drops
dead patches "after recording a `ConditionFault`-style fault — **compaction**, not a ceiling —
so dead patches are not re-skipped forever" (`:1995-1998`; "compaction" is required by the
rationale — a dead record still in the rank *would* be re-walked and re-dropped every genesis).

So positions renumber: drop patch 2 of `[p1..p4]` (whose clauses are labeled
`patch:1,patch:3,patch:4`), append a new learn at position 4, and the verb mints `patch:4` — a
**second live clause carrying the same durable label**. The consequences land exactly where the
doc's own invariant says they must — "labels are the re-resolution key and a duplicate would
misbind" (`:2034-2035`, the same sentence that justifies refusing duplicate `#label`s at
emission):

- `plan_named("patch:4")` first-hits one twin arbitrarily;
- a patch-target descriptor recorded verbatim as `patch:4` (learned-clause targets store "the
  clause's verbatim `+d.name`", `:1618-1621`) resolves to the wrong clause at replay — a durable
  cross-session misbind, not just session ambiguity;
- `unlearn` by recorded name pops the wrong twin (`:1142-1145` — LIFO cannot pick a named
  sibling).

Reachable both mid-incarnation (`unlearn` then `learn_*`) and across resume (replay drop then
`learn_*`). The doc already owns a collision-free mint — `reify`'s `<serial>` "comes from a
durable per-agent counter on `d.vars`" (`:2048-2050`) — and chose a positional source here
anyway. If the intended reading is instead that `d.patch` never compacts, that contradicts the
doc's own "compaction" wording and reintroduces re-skip-forever.

**Fix.** One clause giving `<n>` a collision-free source: "`<n>` is the patch's serial from the
durable counter `reify`'s `<serial>` rides (`d.vars`), never the rank position — `d.patch`
compacts on drops, so positions are reused while labels are durable keys" — or state that dead
patches are tombstoned in-rank (and replay skips them), which also fixes it but forfeits
compaction. (Adjacent to the zigzag lane — flagging because the misbind lands at genesis replay,
the durability boundary this report audits.)

### 3. MINOR — `bindPublishedLocalScroll(self)`'s `self` is not in the adapter's scope: name the idempotent re-add (§4.5 `:609`; §6.6 `:1417`; §10.1 `:2494`)

**Problem.** The hydration pair is prescribed as "`bindPublishedLocalScroll(self)` —
`restorePublication`'s own pair" — but `self` is a local inside `restorePublication`
(`history->addScroll(local->second)`, `publication.cpp:1130`); `historyFromSeal` returns only the
store, and its `addScroll(from)` result at `:1098` is discarded. The recover path is real and
free: `addScroll` is content-identified — "Recording the same scroll twice gives the same id"
(`store.hpp:1000-1009`; `sameContentAs` match returns the existing id, `store.cpp:1388-1405`) —
so `store->addScroll(from)` re-asks for `self` (it was the first external added), and
`bindPublishedLocalScroll` throws on a bad id (`store.cpp:1418-1422`) so a wrong answer is loud.
**Fix:** write the idiom — "`bindPublishedLocalScroll(store->addScroll(from))` — `addScroll` is
idempotent on content identity (`store.cpp:1388-1405`), so the re-add returns `from`'s
fold-internal id".

### 4. MINOR — `historyFromSeal` wants segment *bytes* but the artifact's `opsSegments` are torrent descriptors, and the seed silently skips `restorePublication`'s verification (§4.5 `:602-605`; §10.1 `:2491-2493`)

**Problem.** `historyFromSeal` takes `span<const string_view>` — sealed byte strings
(`publication.hpp:605-612`). `Publication::opsSegments` are `ScrollSegment` descriptors —
`torrent`/`fileIndex`/`path`/`streamOffset` coordinates, `at`/`length` in *op counts*
(`publication.hpp:196-203`). The adapter must first run `restorePublication`'s resolve loop
shape (`:1139-1160`: `metainfo` → `Scroll::ofTorrentFile` → `resolver.resolve` →
`isVerified`/length check) over its own content source — mechanical, but the recipe currently
reads as if segments arrive as bytes. The same step is where `restorePublication` performs its
gap/overlap, coordinate, dependency and inventory checks (`:1140-1179`) — which `historyFromSeal`
skips entirely — so the seed's trust basis (manifest signature via `decodePublication`, plus the
(a)-comparand downstream) is worth one clause rather than silence. **Fix:** name it — "the
segment bytes are resolved like `restorePublication`'s loop (`publication.cpp:1149-1160`) over
the adapter's source; the signed-inventory checks it forgoes are covered by signature-on-decode
plus the genesis (a) band comparand".

## Nits

- **`ArenaManifold::store()` is const-only** — `const Store*` (`arena_manifold.hpp:269-271`), so
  the *mutable* publish reach under verify is specifically `arena.base()->store()`
  (`manifold.hpp:486`'s hole), a distinction worth the two extra tokens when the doc gestures at
  the leak.
- **Comparand scroll fields** — "op kind plus non-scroll fields" (`:1884-1885`) drops scroll
  identity entirely; comparing `scrollKey(*store->scroll(span.scroll))` keeps it at equal cost
  (a lineage differing only in *which* scroll a span names passes today — exotic, but the
  canonical form is the place to say whether scroll identity is in or out).
- **`resume` consumption cite** — `checkpoint.store` is first read at `mesh_actor.cpp:188`, not
  `:186` (the signature's close); the doc's `consumed at :186,:261` is two lines early.
- **The user-`name` alias is the patch record's only field without a named carrier** — `:136`/
  `:1591-1593` give `ordinal`, `+d.patch-head`, `+d.name` and `priority` their dims; the alias's
  carrier is implicit. One word alongside it.
- **The comparand's `(a)` band and the instruction enumeration are both "recorded at load"** —
  worth stating both are *positions in the sealed fold* (emission positions == sealed positions
  == folded positions under `adoptOpRecords`' deterministic fold, `store.cpp:3836-3940`), so a
  reader doesn't go looking for descriptor-index→position mapping that was never specified.

## Verified v1.10 deltas (no finding)

- **R10-A shape**: `patch:` refused as user `name` while the verb mints the label (`:1130-1135`);
  dedupe matches the *alias* against `+d.name`s so learned `patch:<n>`s never self-replace
  (`:1136-1139`); `learn_handler` additive + `unlearn` LIFO limit stated (`:1140-1145`); §9.3's
  example passes a plain alias `"near_water"` (`:2383-2385`,`:2399-2402`); glossary row updated
  (`:136`). Layering coherent — `patch:` is clause-label space, `code_named`'s head-labels-only
  row keeps the spaces disjoint (`:1740-1741`,`:2053-2057`), and `plan_named`'s verbatim match is
  the consumer (`:2075-2078`). The one defect is finding 2's positional `<n>`.
- **R10-B**: "exactly the recorded instruction enumeration … `program.instructions` precedent,
  `mesh_evaluator.cpp:658-663`" — verified: `instructions`/`setOpcodeKind` per recorded
  instruction (`mesh_evaluator.cpp:658-666`); the value-scan counterexample is right —
  `dimConstant` operand cells hold `int64` dim refs and an `asInt64 ≤ Linked` sweep would
  register data cells into P15's refusal (`:1929-1933`); `setOpcodeKind` is public
  (`vortex_vm.hpp:118`) and positions-as-refs hold (`manifold.hpp:295`).
- **R10-D**: capture endpoint on the recipe record's `+d.values` — consistent with §6.1's recipe
  spec (endpoints encoded as *links*, `+d.values →` endpoint cells, `:1109-1112`), folds
  positionally, internal arm rebinds via the old→new map, `*handleTarget` equality scoped to
  external/capture referents (`:1967-1979`). Coherent.
- **R10-E**: the comparand is the adapter's adopted store's decoded `1..E_ops` vs the received
  store's, order-vs-order post-`adoptOpRecords` (`:1881-1897`); `getCompactOp`/`opCount` public
  const cover both reads; `adoptOpRecords` topo-sort cite `store.cpp:3836-3940` verified.
- **`label_of` path**: `cells"$a/+d.code"` off `agent_cell()` — `d.code` correctly moved off
  arena `home` (`:1075`); `[Cell]`→`Cell` indexing named.
- **Two-root walk vocabulary**: agent-cell legs (`+d.code`, `d.contract`, `d.belief`, `d.source`)
  enumerated only by a second `agent_cell()`-rooted walk (`:1401-1405`,`:1904-1908`) — correct;
  `promote` walks from the agent cell and prunes at non-`holdsOwn` persistent cells
  (`arena_manifold.cpp:1755-1757`), so nothing cross-root leaks.
- **Fail-until-resume vs fail-forever split** and the `None`-content emission-fixed slot
  (`:1687-1698`): consistent with no-sentinel and the rebind sweep; the headless-`run` refusal
  stays a drop at genesis.

## Structural notes

- `resume` re-checked end to end: `ReceivedValue` by value (`mesh_actor.cpp:182-186`),
  `!checkpoint.store || !checkpoint.manifold` gate `:188`, `receiveValue(manifest, *sourceLease,
  checkpoint.store->userPermascrollPtr(), …)` `:260-261`, `actor->sourceLease` `:268`,
  `actor->checkpoint` `:269`, one-arg `WorkerContext(manifold.get())` `:270-271`,
  `setDefaultSpanReader` `:273`. `WorkerContext` two-arg ctor `worker_context.hpp:52-55`;
  `ArenaManifold(const Manifold *base)` one-arg `:254` delegates `store_ = nullptr` → `store()`
  falls through to `base_->store()` `:269-271`; `Manifold::store()` `const noexcept → Store*`
  `manifold.hpp:486`; `Manifold::dimensionNamed(name)` reads through `*store_`
  `manifold.cpp:824-829`; `DimensionRegistry` fast path cold then registered `:787-791`,`:816-817`.
- Store surface: `Store : public SpanReader` `store.hpp:128`; `Store()` mints `UserPermascroll`
  `store.cpp:123-125`; `Store(permascroll, documentId)` `store.hpp:130-133`/`store.cpp:132-136`;
  `documentId_` `store.hpp:143-144`; `addScroll` idempotent `store.cpp:1388-1405`;
  `bindPublishedLocalScroll` guard `:1418-1422`; `setContentSource` eager-hydrates `externals`
  `:1425-1433` + lazy `scroll()` `:1461-1469`; `read` chain `:1562-1588`; `publishedLocalScroll_`
  consumed by store-tables import `:4473-4493`; `rebuildManifold`/`FromIndex` `store.hpp:450-457`.
- Publication path: `publish()` sets `historyScroll`/`scrolls` only when `opsSegments` non-empty
  `publication.cpp:1297-1304`; `opsSegments` are descriptors `publication.hpp:196-203`;
  `restorePublication` hydration+verification `publication.cpp:1112-1191`; `adopt` transcludes
  pieces, no ops `:1194-1278`; `historyFromSeal` `:1093-1110`; `applyOpsSegment` file-local
  `:1023-1091`; `SealedScroll::opsSegment` "assembled by `historyFromSeal()` rather than by
  `Resolver::read()`" `publication.hpp:307-315`.
- `promote`: identity map `:1867-1868`; OpHandle target existence gate `:1809`; links-last
  emission and `real`/translate machinery as previously verified; `job.base` shared store
  `mesh_jobs.cpp:304-322` confirmed — `store = base->store.get()` is the publish destination,
  the precedent finding 1's fix names.

## Verdict

R10-C's fix is real and correctly shaped — the hydration pair, the unmechanized adopt arm, and
the narrowed transport claim all landed verbatim with honest evidence. The seam chain is closed
at every boundary **except one**: under the verify arm the publish destination is still the
runtime's private store, and the doc's literal answer (the seeded store) is the corrupt one —
that plus `patch:<n>`'s positional label reuse are the two majors. Everything else is
minors-and-nits on an otherwise consistent delta.

VERDICT: NOT CONVERGED
