# R10 — System Architect critique (v1.9)

Scope: layering, ownership, adapter-vs-core boundaries; verdict on my r9 major (R9-B — `attach`
post-`WorkerContext` timing, frozen `home_`/dims/`opcodeMap_`) plus new defects in the v1.9 deltas
only. Re-verified against the tree (`worker_context.hpp`, `mesh_actor.{hpp,cpp}`,
`vortex_core.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `manifold.{hpp,cpp}`, `store.{hpp,cpp}`,
`publication.{hpp,cpp}`, `resolver.{hpp,cpp}`, `mesh_value.{hpp,cpp}`, `mesh_jobs.cpp`,
`mesh_host.cpp`, `mesh_evaluator.cpp`, `document_id.hpp`, `provenance.hpp`,
`tests/xuzz/provenance.cpp`, `tests/xuzz/mesh_actor_resume_test.cpp`).

## R9-B verification — FIXED, and the seam is *almost* closed

v1.9 does exactly what the disposition ruled:

- **Spawn adopts resume's ctor-injection shape.** §6.6 (`:1400-1413`): "the adapter's seeding is
  the §10.1 step that owns the store, bound **before the `WorkerContext`/`initGenesis` freeze** …
  The spawn arm therefore takes **resume's own injection shape**: the seeded store folded to a
  `Manifold` the adapter pins for the arena's life, constructing `WorkerContext(base, reader)`
  (`mesh_actor.cpp:270-271`, `worker_context.hpp:52-55`)". The cited mechanics are real:
  `WorkerContext(const Manifold *baseManifold, const SpanReader *reader)` at
  `worker_context.hpp:52-55` runs `arena(baseManifold)` before `core(…)` so the base exists when
  `VortexCore`'s ctor calls `initGenesis()` (`vortex_core.cpp:14`), which adopts `*base->home()`
  and `*baseDims` at `:30-43`. `initGenesis`/`home_`/`dims_` are private with no re-genesis door
  (`vortex_core.hpp:267`,`:281-282`,`:287`) — so the post-construction `attach(Space)` shape is
  now correctly *named as the refused alternative* (`:1404-1406`), and my r9 nits about `attach`'s
  side-effects (`spaces_[0]`, `d.stores`/`d.store-refs` mints, rank wiring) are moot — `attach`
  is no longer the mechanism, and resume never attaches anyway.
- **The `opcodeMap_` arm is covered.** Genesis (b) is now scoped to both moments: "`opcodeMap_` is
  per-`WorkerContext` and empty at the spawn-time image bind as after `resume`, so the pass runs
  at both" (`:1843-1844`), and §6.5 says the same (`:1310-1313`). `setOpcodeKind` is public
  (`vortex_vm.hpp:118`, impl `:98-100`), so the adapter can run the pass at the spawn bind.
- **Supply-vs-verify at `resume` is stated and reachable.** "supplies the same lineage store as
  the rebuild's backing store, or verifies the rebuilt one's `1..E_ops` band against it"
  (`:1411-1413`). `receiveValue` → `restorePublication` → `store->rebuildManifold(version)`
  (`mesh_value.cpp:345-351`) sets the `setStore` back-pointer (`store.cpp:511`,`:532`), so
  `checkpointView`'s `const ArenaManifold&` → `arena.store()` (`arena_manifold.hpp:269-271`,
  public, falls through to `base_->store()`) reaches the runtime's rebuilt store, and
  `Store::getCompactOp(uint32_t)` (`store.hpp:179`, public const, 1-based per
  `publication.cpp:1167-1168`) plus `opCount()` (`store.hpp:985`) implement the verify arm with
  **zero new surface today** — the doc's claim `:1458-1460` is exactly right.
- **`ArenaManifold::store()` public-surface correction landed** (`:1458-1463`), including the
  `Manifold::store()` `const noexcept`→non-const `Store*` const-hole acknowledgment
  (`manifold.hpp:486` — verified).
- **`documentId` is stated** (`:611-615`,`:1388-1394`): `documentId_` is ctor state
  (`store.hpp:133`,`:1435`; `DocumentId()` mints random, `document_id.hpp:27`), the apply carries
  ops not identity, `restorePublication`'s `Store(readerPermascroll, pub.storeId)` precedent is
  real (`publication.cpp:1127-1128`). But see finding 2 — one arm is unmechanized as cited.
- **The canonical comparand is feasible**: "op kind plus non-scroll fields" (`:1413-1415`,
  `:1809-1811`) — `applyOpsSegment` remaps exactly one field, `record.op.span.scroll`
  (`publication.cpp:1079-1086`), so the canonical form is `getCompactOp` minus the scroll id, and
  the `produces`/`context`/`structure*` microversion ids compare verbatim across stores.
- **R9-A's landmark split is sound.** `E_ops` as op count vs refs as birth-op positions
  (`manifold.hpp:295` — `birthOp` "the MakeCell index; == this CellRef"; `store.hpp:677-681`;
  `manifold.cpp:434` assigns `birthOp = opIndex`), links-last (`arena_manifold.cpp:2009-2016`,
  `exportSymbol` `vortex_core.cpp:119-127`), per-cell classification, mandatory comparand — all
  verified.
- **R9-C's capture-endpoint restatement is coherent**: record `+d.values` names the referent
  fold cell at its promoted position, `Value`-replaying `int64(endpoint)` writes the identical
  bits `*handleTarget` carries (`arena_manifold.cpp:1947-1954`'s `real.at`), no handle-kind read
  needed — name-record slots still take the rebind arm. Consistent.

### Structural check that nearly became a finding (resolved, recorded)

`adoptOpRecords` sorts records by `produces` then topo-schedules with a produces-heap
(`store.cpp:3830-3833`,`:3836-3940`). Naively this looks like it permutes positions on every
adoption — which would break `1..E_ops` across stores. It does not, for the case that matters:
`getDeps` includes `op.parent` (`:3850-3853`), and a linearly-appended history is a *total* parent
chain — every op's in-degree ≥1 until its predecessor files — so the schedule is forced to the
original order and positions survive verbatim (this is also why `historyFromSeal` round-trips a
linear artifact position-stably). The produces-heap only gets a choice among *siblings* — a
branched/merged record set — which is precisely the within-band permutation the new mandatory
comparand exists to refuse (`:1812-1815`). The doc's "position-stable" claim is thereby honest:
stable for the linear lineage it asserts, and the branched deviation is the intended fail-closed
arm, not an accident. Worth knowing: position-stability is a *property of the chain's shape*, not
of the serialization — the doc could say that in three words.

### What still stands in the seam — finding 1

The spawn arm delivers `base_`/`store_`/reader into the constructor, but the store the doc
produces cannot serve the *reader* role it is being injected for — one layer deeper than R9-B,
the same unreachable-object shape at the resource layer instead of the construction layer.
See finding 1.

## New findings (v1.9 deltas only)

### 1. MAJOR — the seeded store arrives un-hydrated: `historyFromSeal` never installs a `ContentSource`, so every artifact-local span resolves to `""` — silently twinning the session dims and dead-reading every name-resolution the ceremony and genesis depend on (§6.6 `:1380-1383`,`:1407-1409`; §10.1 `:2391-2398`)

**Problem.** The seeding step is `historyFromSeal` (`publication.cpp:1093-1103`). It does exactly
three things: `make_unique<Store>()`, `addScroll(from)`, `applyOpsSegment` per segment. It does
**not** call `setContentSource` (contrast `restorePublication` — the doc's own cited precedent —
which performs `history->setContentSource(&source)` at `publication.cpp:1129` and
`bindPublishedLocalScroll(self)` at `:1131`), and the `Store()` it mints carries a fresh empty
`UserPermascroll` (`store.cpp:123-124`).

`applyOpsSegment`'s remap is the mechanism that makes this bite: the sealed segment's
local-scroll spans (`span.scroll == localScroll == 0`, `spool.hpp:19`,`:61`) are rewritten to
`selfScrollInHistory` — the *external* id of `addScroll(from)` (`publication.cpp:1048-1049`,
`:1079-1086`), so after sealing **no span is local** (the test asserts this verbatim:
`EXPECT_FALSE(piece.isLocal())` for every travelled piece, `tests/xuzz/provenance.cpp:460-465`).
Every content read then lands on `Store::read`'s resolver path: `scroll(span.scroll)` →
`resolver.read(*which, span)` (`store.cpp:1575-1588`) → `Resolver::read` returns `{}` on any
non-`VerifiedBytes` result (`resolver.cpp:370-377`) — and with no `ContentSource` there is no
verified anything. `retainedContent_` is empty on a fresh store, `remoteAuthorBuffers_` is empty
without `setExternalLiveBytes` (`store.cpp:1591-1619`), `readVocabulary` covers only format
attributes (`format.hpp:63-87` — never dim names). **Read returns `""`, silently.**

Where the seam depends on those reads:

- `initGenesis`'s own adoption machinery: `mintNamedDimension` → `baseDimensionNamed`
  (`vortex_core.cpp:18-21`) → `arena_.base()->dimensionNamed(name)` (`vortex_core.cpp:349-352`) →
  `Manifold::dimensionNamed(name, *store_)` (`manifold.cpp:824-829` — note it uses the manifold's
  own store back-pointer, so no `defaultReader_` substitution can dodge this) → `textOf(dim,
  reader) == name` (`manifold.cpp:802-814`). Every dim name in the image — `d.grab`, `d.vars`,
  `d.stdlib`, `d.values`, `d.name`, all ~18 — was typed into the artifact's *local* scroll and is
  now an external-scroll span reading `""`. The `DimensionRegistry` fast path
  (`manifold.cpp:787-791`) is cold for a fresh store. So `baseDimensionNamed` returns `nullopt`
  for every system dim and `mintNamedDimension` **mints ephemeral twins of the entire dim
  vocabulary** — the identical twin-anchor defect R9-B fixed for `home_`, now arrived one level
  down on the dims it adopts. `findModule`/`getOrCreateModule`/`exportSymbol` walk
  `rankAfter(home_, dims_.stdlib)` (`vortex_core.cpp:101-116`) on the twin — the folded program
  image is unreachable by name, and nothing refuses: `textOf` returning `""` is indistinguishable
  from a legitimately absent dim.
- The same dead read underlies *every* name-resolution the document's machinery stands on:
  `ensureDimension`'s find-half (`arena_manifold.cpp:55-75`, also routed through `defaultReader_`
  = the same dead store, `:57-66`), `symbol_named`/`module_named`/`plan_named`/`code_named`'s
  `+d.name` label matches, the `src:`/`reify:` label→head lookup genesis (e)'s rebind uses
  (`:1688-1697`), and the V10 bind step's name lookups (§10.1 `:2405-2409`). Ops and int64 slots
  decode fine (canonical bits live in `valueBits`, not spans — which is why `getOpcodeKind`'s
  `asInt64` fallback works) — it is specifically the *text layer* that is dead, i.e., the layer
  that carries every identity the ceremony names.
- `bindPublishedLocalScroll` is equally unperformed: `publishedLocalScroll_` stays `localScroll`
  (`store.hpp:1440`), so the pick/metadata paths that consult it (`store.cpp:4473-4480`) see no
  published local scroll.

Why this is a doc defect, not an adapter detail: the doc names the reader role — "+ the store as
reader constructing `WorkerContext(base, reader)`" (`:2397-2398`) — and the `job.base` pattern it
claims as precedent (`mesh_jobs.cpp:304-318`) reaches its store through `fetchValue` →
`receiveValue` → `restorePublication`, i.e., a *fully hydrated* store (source set, storeId
adopted, local scroll bound, inventory verified — `publication.cpp:1127-1179`; `mesh_host.cpp:512`
likewise calls `setContentSource` on its merge destination). The doc picked the one function in
the family that performs *none* of that, and the failure is silent twinning rather than a thrown
error — the worst shape for a seam whose claim is "the asymmetry collapses to the resume arm."

**Fix.** One clause in §10.1's seeding step and in §6.6's paragraph, naming the hydration:
"the seeded store is then hydrated like any restored history — `setContentSource` on the
adapter's content source (`publication.cpp:1129` precedent; the lease pinned for the actor's
life, the `actor->sourceLease` role `mesh_actor.cpp:268` plays at resume) plus
`bindPublishedLocalScroll(self)` (`:1131`) — without it every artifact-local span resolves to
`""` and `initGenesis` twins the session dims." Naming `restorePublication` itself as the
seeding function is the alternative worth pricing: it performs the hydration *and* adopts
`pub.storeId` (settling the `documentId` arm below) but requires a signed `Publication` rather
than bare sealed segments — the local-trust case may genuinely want `historyFromSeal` + manual
hydration. Either way the step must be written down; it is currently invisible.

### 2. MINOR — the "adopt the artifact's `documentId`" arm is unmechanized as cited: `historyFromSeal` mints `Store()` internally and takes no id (§6.6 `:1388-1394`; §4.5 `:611-615`)

**Problem.** The doc offers a real construction choice — "`historyFromSeal`'s fresh mint makes
each agent its own document lineage, while adopting the artifact's id —
`Store(userPermascroll, artifactDocumentId)`, `restorePublication`'s precedent — puts every agent
off one artifact under a single document identity". But `historyFromSeal` constructs
`std::make_unique<Store>()` itself (`publication.cpp:1097`) — no permascroll or `DocumentId`
parameter exists on either overload (`publication.hpp:606-612`) — and `applyOpsSegment` is
file-local in an anonymous namespace (`publication.cpp:1023-1030`), so the adapter cannot
hand-roll the same apply around a `Store(permascroll, id)`. The adopt arm is reachable only via
`restorePublication` (which *does* adopt `pub.storeId`, `:1127-1128`, but consumes a signed
`Publication`, not bare segments — and does the inventory/dependency verification the doc's
`historyFromSeal` arm skips). As written, an implementer who chooses the documented adopt arm
finds no function that performs it. **Fix:** resolve the choice — "the adapter takes the
fresh-mint arm (`historyFromSeal`); adoption belongs to a future `historyFromSeal` overload or a
`restorePublication` seed" — or name the overload as part of the owed seam. One clause either
way; the choice should not be stated as available when only one side is mechanized.

### Nits

- **`WorkerContext(base, reader)` cites the signature, not the call shape.** `resume` literally
  calls `std::make_unique<WorkerContext>(actor->checkpoint->manifold.get())` — one arg — then
  `context.arena.setDefaultSpanReader(actor->checkpoint->store.get())` post-construction
  (`mesh_actor.cpp:270-273`). Functionally equivalent (the ctor's `setDefaultSpanReader(reader)`
  runs in `arena`'s init-list slot, `worker_context.hpp:54`, and `initGenesis` resolves dims via
  the manifold's `*store_`, not `defaultReader_`), but "the `WorkerContext(base, reader)` shape
  `resume` already uses" reads as a call-site cite. Say "(the one-arg call plus
  `setDefaultSpanReader`, `mesh_actor.cpp:270-273`)".
- **`resume` already transports a store — the supply arm's hole is narrower than stated.**
  `resume(…, ReceivedValue checkpoint, …)` requires `checkpoint.store`/`checkpoint.manifold`
  non-null (`mesh_actor.cpp:188`), uses the store only for `userPermascrollPtr()` (`:261`), then
  drops it — "rehydrate from authenticated history rather than accepting a caller's mutable
  `Manifold`" (`:256-257`). So "the runtime never hands the store object out" (`:1395-1399`) is
  true, but "spawn()/resume() expose neither today" undersells it: the caller-supplied store
  already crosses the signature; the owed change is `receiveValue` folding the supplied store
  (or `resume` trusting `checkpoint.manifold` after verification), not new plumbing. One clause
  strengthens §10.2's preamble. Related: `ReceivedValue.store` is `unique_ptr`
  (`mesh_value.hpp:102`) — "supply" of the adapter's *kept* store implies shared ownership or a
  second seeded product; the seam should say which.
- **The pin must be address-stable.** `WorkerContext` holds `const Manifold *base_` raw
  (`worker_context.hpp:52-54`); "a `Manifold` the adapter pins for the arena's life"
  (`:1408`,`:2395-2396`) must therefore be a `unique_ptr`/immovable holder — a by-value member of
  a movable bind record would dangle `base_` on move. One word ("pins it by `unique_ptr`") closes
  it.
- **`historyFromSeal`'s `Store()` also mints a fresh `UserPermascroll`** (`store.cpp:123-124`) —
  fine for the agent's own future typing (its primedia does not pollute the artifact scroll),
  but worth one clause since `userPermascrollPtr()` is what `resume` reads at `:261`.
- **Carried, still standing:** `Manifold::store()` is `const noexcept` returning mutable
  `Store*` (`manifold.hpp:486`) — the doc now acknowledges the const-hole honestly
  (`:1461-1463`), but the leak itself is a const-correctness bug worth reporting to the runtime
  owners; `rebuildManifold`'s `setStore(const_cast<Store*>(this))` (`store.cpp:511`,`:532`) is
  the same pattern one layer down.

## Structural notes (no finding)

- **Every new cite verified**: `historyFromSeal` body (`publication.cpp:1093-1103`),
  `applyOpsSegment` file-local + `\x7fXSO\x01` gate (`:1023-1033`), `sealedOpsName`
  (`provenance.hpp:286` = `"ops"`), `restorePublication`'s hydration (`:1127-1131`) and
  gap/overlap checks (`:1139-1166`), `adoptOpRecords` produces-sort + topo (`store.cpp:3830`,
  `:3836-3940`), `Store()`/`documentId_` (`store.cpp:123-135`, `store.hpp:1435`),
  `Store::read`'s resolution chain (`store.cpp:1562-1588`), `Resolver::read`→`{}`
  (`resolver.cpp:370-377`), `dimensionNamed` textOf-walk (`manifold.cpp:782-829`),
  `ArenaManifold::dimensionNamed` reader cascade (`arena_manifold.cpp:55-75`), `ArenaManifold`
  ctors (`arena_manifold.cpp:23-27`, `arena_manifold.hpp:254-261` — `arena(base)` ⇒ `store_`
  null ⇒ `store()` falls through to `base_->store()`), `cellCount` = overlay `slots_.size()`
  (`arena_manifold.hpp:478` — `initial` at `mesh_actor.cpp:161`/`:274` is base-agnostic),
  `holdsOwn` prune (`arena_manifold.cpp:1753-1757`), `real.emplace(arena,arena)` identity map
  (`:1866`), link-emission loop (`:2028-2040`), `promote` links-last (`:2009-2016`),
  `spawn`/`resume`/`checkpointView`/`pumpOne`/`pending` (`mesh_actor.cpp:119-122`,`:156`,
  `:260-273`,`:668-694`,`:460-505`,`:659-662`; decl `mesh_actor.hpp:132-144`),
  `ActorCheckpointView` fields (`mesh_actor.hpp:102-106`), `ReceivedValue` shape
  (`mesh_value.hpp:98-105`), `receiveValue`→`restorePublication`+`rebuildManifold`
  (`mesh_value.cpp:345-359`), `mergeValues` `(contentHash, authorKey, parent)` sort
  (`mesh_value.cpp:519-521`), `mesh_host` destination arms — `job.base` `:507`, fresh `:509-510`,
  `setContentSource` `:512` (doc's cites exact), `job.base` shared store
  (`mesh_jobs.cpp:303-318`), `mesh_evaluator` per-ref marker (`mesh_evaluator.cpp:723-735`),
  resume-test fresh `Store` (`tests/xuzz/mesh_actor_resume_test.cpp:125`),
  `DocumentId()` `generate()` (`document_id.hpp:27`), `noCell` (`dim_vector.hpp:23`),
  `ephemeralBit`/`isEphemeral` (`manifold.hpp:155-158`), `Manifold::home()`/`dimsDim_` adoption
  order (`manifold.cpp:448-454`, `manifold.hpp:597`).
- **Genesis (a) is now honestly stated**: mandatory ops-segment comparand (catches within-band
  permutation — the `adoptOpRecords` produces-heap makes that a reachable shape on *branched*
  record sets, `store.cpp:3836-3940`) + per-cell `≤E_ops` classification (non-image ingress reads
  as misclassification, `:1826-1828`). The walk-max assertion is gone; the two landmarks are
  distinct and both mechanized.
- **`(d)`→`(e)` hand-off and capture endpoints**: R9-C's restatement is mechanized — recipe
  records encode endpoints as cells, replay resolves them on the received fold, copies'
  ref slots arrive pre-rehydrated; name-record slots still take the rebind arm.
- **`E_ops` recording is adapter-side state** on the `programRef` bind record — no in-arena
  object needed; consumed at (a), re-bound at (c) (`:1821-1823`,`:1851-1853`).

## Verdict

R9-B's fix is real and correctly shaped: base-injection at spawn mirrors resume's ctor order,
`initGenesis` adoption, the `opcodeMap_` pass scope, the public `ArenaManifold::store()` verify
surface, `documentId` stated, canonical comparand stated, and `attach` demoted to the named
refused alternative. The verify arm needs nothing new; the supply arm is honestly registered as
owed surface. **But the seam is not yet architecturally complete**: one layer below the injected
base sits the store's own resolution chain, and the doc's chosen seeding function returns it
unwired — `historyFromSeal` performs no `setContentSource`/`bindPublishedLocalScroll`, so every
artifact-local span (the entire `d.*` dim vocabulary, all `+d.name`/`src:`/`reify:` labels and
symbol names) reads `""` inside `initGenesis` and every name-resolution path — silent twinning
and dead reads, not a refusal. That is the same unreachable-object shape R8-A and R9-B were, at
the resource layer instead of the construction layer — and it is a one-clause fix off the
`restorePublication` precedent the doc already cites. Plus one minor: the adopt-`documentId` arm
is stated but no cited function can produce it.

VERDICT: NOT CONVERGED — 0 blocking, 1 major, 1 minor, 5 nits
