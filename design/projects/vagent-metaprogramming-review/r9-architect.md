# R9 — System Architect critique (v1.8)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene; verdict on my
r8 major (R8-A — the publish-store invariant named an unreachable object) plus new defects in the
v1.8 deltas only. Re-verified against the tree (`mesh_actor.{hpp,cpp}`, `worker_context.hpp`,
`vortex_core.{hpp,cpp}`, `arena_manifold.{hpp,cpp}`, `publication.{hpp,cpp}`,
`store.{hpp,cpp}`, `manifold.{hpp,cpp}`, `mesh_value.cpp`, `mesh_jobs.cpp`,
`vagent-agent-language.md`).

**R8-A: the seam is now named and owned — and the fix's spawn arm exposes one more unnamed seam one
layer down.** v1.8 does what the disposition ruled: §6.6 (`:1363-1411`) names the store's owner —
"the adapter constructs and *keeps* the publish `Store` itself, seeded at spawn-time with the
artifact's sealed ops segment … installed into the arena by `attach(Space)`
(`arena_manifold.cpp:274-287` installs `base_`/`store_` when unset — verified: `:279-284`), and at
`resume` the adapter **supplies the same lineage store to the replay path, or verifies the rebuilt
one's `1..E` band against it** — owed adapter-API surface, a named seam change like P4's wake
(§10.2), not silently assumed." §10.1's adapter row owns construction/seeding (`:2295-2309`,
"CONSTRUCTS and KEEPS … this step owns that gap"); §10.2's preamble and P16's row register the seam
("the lineage-store seam §6.6 names — the adapter-owned publish `Store`, supplied or verified at
`resume`", `:2325-2336`); the unreachable-object sentences now say what the platform withholds and
where the gap is owned instead of pretending the object exists. Position-stability replaces
byte-identity correctly (`applyOpsSegment`'s scroll remap, `publication.cpp:975-1008`,`:1030`);
fresh-store publication is fail-closed **by detection** with the test path honestly cited
(`mesh_actor_resume_test.cpp:125`'s fresh `Store`, verified); per-agent store ownership and
upgrade→respawn are stated (`:606-612`, pin verified `mesh_actor.cpp:215`); the §4.4 supersession
is marked (`:357-360`, narrowing U-A14). The spawn-vs-resume asymmetry as *stated* is coherent:
`attach` installs only when `base_`/`store_` are unset, which is true at spawn and false at resume
— so the two arms genuinely differ. R8-A's ask is done. What it did not check — because r8's store
question did not reach it — is what else the resume path's *construction order* delivers that a
post-construction `attach` cannot: that is finding 1.

## New findings (v1.8 deltas only)

### 1. MAJOR — the spawn arm's seam stops at the arena: `attach` delivers `base_`/`store_` but not the base-derived anchors `initGenesis` already froze — `home_`, the session dims, and `opcodeMap_` (§6.6 `:1376-1379`; §10.1 `:2298-2299`; genesis (b) `:1752-1764`; home narrative `:1432-1435`)

**Problem.** The fix routes spawn-time seeding through `attach(Space)` inside the executor — the
only reachable moment, since `spawn()` owns the `WorkerContext` (`mesh_actor.cpp:156`) and the
callback gets `WorkerContext&` (`mesh_actor.hpp:115-116`). But `attach` runs *after* construction,
and everything the context derives from a base was frozen at construction:

- **`home_`/`dims_` anchored ephemerally, and there is no re-genesis door.** `WorkerContext()`
  constructs `arena(nullptr)` then `core(arena)` (`worker_context.hpp:52-55`), and `VortexCore`'s
  ctor calls `initGenesis()` immediately (`vortex_core.cpp:14`). With `arena_.base()==nullptr`,
  `home_ = arena_.makeCell("home")` and all ~18 named dims are *ephemeral* mints
  (`vortex_core.cpp:29-70`). `initGenesis` and `home_` are private (`vortex_core.hpp:268,282,287`)
  with no setter or re-run — the adapter cannot rebind. Post-`attach`, `arena.home()` returns the
  image's home (`ArenaManifold::home()` is `base_ ? base_->home() : nullopt`,
  `arena_manifold.cpp:30-32`) while `core.home()` keeps the ephemeral twin: `findModule` /
  `getOrCreateModule` walk `rankAfter(arena_, home_, dims_.stdlib)` on the twin
  (`vortex_core.cpp:101-116`), so any runtime module resolution anchors a rank the image's modules
  are not on. The resume arm escapes this because `WorkerContext(base)` ctor-injects the manifold
  *before* `initGenesis` runs (`mesh_actor.cpp:270-271`) — `initGenesis` adopts `*base->home()` and
  `*baseDims` (`vortex_core.cpp:30-43`). The doc itself claims the adoption unconditionally: "the
  checkpoint's own persistent `home` whenever the base has one — `initGenesis` adopts
  `*base->home()`" (`:1433-1434`) — true at resume, **falsified on the spawn arm it just
  introduced**: at spawn the base *has* a home after `attach`, but `home_` was frozen before it.
  Execution itself survives (ops carry their own dim refs; the adapter's binds are explicit image
  walks) — the falsified claim plus the twin-anchored core paths are the defect, not a crash.

- **`opcodeMap_` stays empty for incarnation 1.** Genesis (b)'s `setOpcodeKind` pass is scoped
  "Post-resume … at genesis" (`:1297-1300`, `:1752-1764`) — but the precondition it names ("the map
  is per-`WorkerContext` and empty") holds identically at spawn, where the image is equally folded
  and equally unregistered. `setOpcodeKind` is public (`vortex_vm.hpp:118`), so the adapter *can*
  run the pass post-`attach` — the doc simply never says to, and the consequence is the doc's own:
  "without this pass the folded skeleton's op cells are absent from the map and surgically
  writable" (`:1761-1763`) — P15's `opcodeMap_.contains(masterOf(cell))` refusal and `code`
  re-emission run uncovered for the agent's entire first incarnation.

So the named seam covers the arena's `base_`/`store_` and not the core/VM state a base feeds at
construction — the same unreachable-object shape as R8-A, one layer down: the object the spawn-side
genesis needs (`home_`, the registration pass's trigger) is on the far side of a private boundary
with no door. The clean fix also *simplifies* the seam: give `spawn` the base-injection resume
already has — an image parameter (folded `Manifold` + reader, or the seeded `Store` to fold)
constructing `WorkerContext(base, reader)`, the `mesh_actor.cpp:270-271` shape — after which
`initGenesis` adopts home/dims at both moments and the asymmetry collapses to "resume supplies or
verifies the lineage store". Re-running `initGenesis` post-`attach` is *not* a viable cheaper arm:
it already ran once, and `mintNamedDimension` is unconditional — a second pass double-mints
(`vortex_core.cpp:45-70`), so a re-genesis entry point would itself be a runtime change. Either way
the seam's spawn half must be widened beyond "`attach(Space)`", and (b)'s scope widened to "the
image bind — at the spawn-time injection/attach as at genesis". (If spawn base-injection lands,
note the P16-row wording "(`spawn` store parameter, …)" `:2336` gets the shape wrong too — the
parameter is a base/image, not a store.)

### 2. MINOR — the seeded store's *identity* is unstated: `applyOpsSegment` carries ops, not `documentId`, and the doc's own parenthetical presumes adoption (§4.5 `:606-609`)

**Problem.** "Artifact bytes are shared; **store objects never are** — two agents off one artifact
each hold their own `Store` (they do still share the artifact's `storeId`/`documentId` lineage
under `job.base`, publishing divergent histories under one store identity — a pre-existing
exposure)". Under the stated seeding mechanism the sharing is false: `documentId_` is `Store`-ctor
state (`store.hpp:1435`), a fresh `Store()` mints a random `DocumentId` (`document_id.hpp:27`), and
`applyOpsSegment` applies ops through a remapped scroll table — it does not carry identity. The
sharing holds only if the adapter constructs `Store(permascroll, artifactDocumentId)` —
`restorePublication`'s precedent (`publication.cpp:1127-1128` passes `pub.storeId`). The choice is
semantic, not cosmetic: `publishValue` salts the history scroll by `store.documentId()`
(`mesh_value.cpp:227`), so adopting the artifact's id produces divergent histories under one
document identity (the named exposure) and a fresh id makes each agent its own document lineage —
arguably the cleaner semantics. `job.base` shares identity only because the job uses the base's
store *object* (`store = base->store.get()`, `mesh_jobs.cpp:314`), which per-agent copies rule out
by definition. **Fix:** name the construction — "the adapter mints the seeded store with the
artifact's `documentId` (divergent-histories exposure, `publication.cpp:1128` precedent)" or "a
fresh `documentId`; each agent is its own document" — and make the parenthetical conditional on it.

### 3. MINOR — the verify arm's comparand is underspecified for the remap the doc itself cites (§6.6 `:1377-1379`, `:1409-1411`; genesis (a) `:1739-1741`)

**Problem.** "Verifies the rebuilt one's `1..E` band against it" is the zero-runtime-change arm —
`checkpointView`'s `const ArenaManifold&` exposes `store()` (`arena_manifold.hpp:269-272`,
falling back to `base_->store()`; `rebuildManifold` sets the back-pointer, `store.cpp:511,532`) —
but the comparison cannot be a node-byte compare: two stores that applied the same segment
legitimately store different `CompactOpNode` bytes (the segment's scroll table remaps into each
store's `ScrollId` space, `publication.cpp:1025-1030` — the very remap the doc cites at `:1369`).
A naive `getCompactOp(i)` byte-compare then fails closed on *every* legitimate resume — fail-closed
into liveness loss, the wrong direction for this seam. Genesis (a)'s optional comparand already
implies the right shape ("asserting the received ops `1..E` equal the recorded image", where the
record is the artifact's *segment*: decode both sides through their own scroll tables and compare
the op sequence semantically). **Fix:** say the comparand at the seam too — "position-wise op
equality through each store's scroll-name resolution against the recorded segment", or "the rebuilt
store's `1..E` decodes to the artifact's segment" — one clause.

### Nits

- **`attach`'s side-effects on the spawn path are unremarked.** It registers the image as
  `spaces_[0]` (ForeignRef space-id 1 then names program cells), mints `d.stores`/`d.store-refs`
  dims plus a `space_1` storeCell (`arena_manifold.cpp:289-299`), and its rank wiring links the
  storeCell under `homeCell()` — which post-install is the *image's* home, so `link(cur, dimStores,
  POS, storeCell)` (`:306-323`) shadows a `≤E` program-side cell in the overlay — the only overlay
  write the seam performs. Benign (`d.stores` is outside the sideness dim vocabulary and
  unreachable from the agent cell) but worth one sentence; it also means the storeCell re-mints at
  every spawn.
- **The fold's lifetime is unnamed.** `Space` wants `const Manifold *` /
  `shared_ptr<const Manifold> ownedManifold` (`arena_manifold.hpp:85-95`);
  `Store::rebuildManifold`/`rebuildManifoldFromIndex` return `Manifold` *by value*
  (`store.hpp:450-457`). §10.1 names keeping the *store* — the adapter must equally pin the folded
  manifold for the arena's life; one word in §10.1.
- **The "private store" is reachable through a const-hole** (`:1372-1374`, `:1418`, `:2327-2329`):
  `checkpointView.arena` (const&) → `.base()` (`arena_manifold.hpp:268`) → `->store()`
  (`manifold.hpp:486`) yields a **mutable `Store*`** — `store()` is `const noexcept` returning
  non-const. So `resume`'s store is not private-by-reachability, only private-by-member: the verify
  arm is implementable today via `arena.store()` (the `const Store*` path), and even the supply arm
  has a de-facto shape (`publishValue(*arena.base()->store(), …)` — `publishValue` takes
  `Store&`, `mesh_value.hpp:69`). Worth one sentence — the owed seam is *sanctioned*
  supply/verification, not raw reachability — and the `Store*`-from-const leak is worth reporting
  to the runtime owners as a const-correctness bug in `Manifold::store()`.
- **"Supply" arm's literal reading would double-apply.** `restorePublication` applies
  `pub.opsSegments` into a *fresh* store (`publication.cpp:1127-1165`); handing it the adapter's
  already-history-carrying store would duplicate ops and shift positions. The sane shape is "supply
  as backing store + rebuild/verify" (the runtime skips re-application) — or make the verify arm
  primary, since `arena.store()` covers it with no signature change at all. The seam text offers
  both arms without pricing the difference.
- **`mergeValues` "discovery order"** (`:1737`) is actually a deterministic sort by
  (contentHash, authorKey, parent) (`mesh_value.cpp:516-520`) — the fail-closed claim stands
  regardless; the phrase undersells it.

## Structural notes (no finding)

- **Every new cite verified**: `attach` install-when-unset (`arena_manifold.cpp:274-287`),
  `applyOpsSegment` (`publication.cpp:1030`) and the segment scroll-table remap (`:975-1008`,
  "carries its own table"), `sealLocalSpool` `opsAlreadySealed=0` default
  (`publication.hpp:340`, called at `mesh_value.cpp:227-228`; segment `at=0,length=opsTotal`
  `:664-672`), `restorePublication` gap/overlap checks and segment-order apply
  (`publication.cpp:1139-1165`), `rebuildManifold` → `setStore` back-pointer
  (`store.cpp:509-532`), `birthOp == CellRef` (`manifold.hpp:295`), `noCell==0`
  (`dim_vector.hpp:23`), `initGenesis` base adoption (`vortex_core.cpp:30-43`), `setOpcodeKind`/
  `getOpcodeKind` fallback (`vortex_vm.cpp:98-114`), `importValue` demote/remint arms
  (`mesh_value.cpp:485-501`), promote `OpHandle` verbatim non-ephemeral targets
  (`arena_manifold.cpp:1947-1954`,`:1996`), `d.dims` skip (`:1766`), `refOf`
  (`arena_manifold.hpp:468-470`), `insertIntoRank` idempotent (`:624-628`), shadow names base ref
  (`arena_manifold.cpp:1489-1490`), `resolveCell`/`contains` (`vortex_vm.cpp:50-67`,
  `arena_manifold.cpp:256-267`), `home_` = first MakeCell (`manifold.cpp:450-451`),
  `mergeValues` sort (`mesh_value.cpp:512-545`), `mesh_host` commit publishes the merge
  (`mesh_host.cpp:484-530`), P4's wake row (`vagent-agent-language.md:1670`),
  `checkpointView`/`pending`/`pumpOne` (`mesh_actor.cpp:460-476,659-694`),
  `d.mesh.relocation→"cell"` precedent (`mesh_evaluator.cpp:735`), `reify` cannot mint handle
  kinds (its `New`/`Link`/`Value` replay — consistent with `writeValue` covering
  Int64/Double/Bool/None only, `vortex_core.cpp:196-229`), `resume` pin
  (`mesh_actor.cpp:199,215`), `WorkerContext` ctor order (`worker_context.hpp:52-55`),
  `job.base` shared store (`mesh_jobs.cpp:303-318`), Makefile rows (`:643` kernel glob,
  `:688-690` `VAGENT_FRONTEND_SRCS`, `:695` `VAGENT_SYNTAX_OBJS`).
- **Genesis (a) is now honestly falsifiable**: positional (`every program()-walk cell ≤ E and
  walk-max = E`, `:1733-1736`) plus the stronger optional comparand — the adapter holds the
  segment it seeded (`:1739-1741`) — closing my r8 procedure-less-assert nit. The merged-lineage
  arm fails closed (`mergeValues`→fresh store, `:1737-1739`). My r8 `sealLocalSpool` cite nit is
  fixed — the call site and the machinery are both cited (`:1385-1387`). The (d)→(e) install-set
  hand-off is explicit and correctly motivated (`:1770-1774`, copy-inheritance named).
- **`code_named`'s fallthrough is disjointness-sound**: `d.code`-registered heads are state-side
  by the invariant, image heads are never registered, so miss→image-walk is unambiguous
  (`:1863-1869`); foreign-artifact heads take the fail-closed arm (`:1869-1873`), U-M11 honest.
- **Makefile/dependency hygiene unchanged and correct** (`:2310-2315`): five TUs join
  `VAGENT_FRONTEND_SRCS`, `fragment.cpp` rides the glob, `VAGENT_SYNTAX_OBJS` untouched.

## Verdict

R8-A's fix is real and end-to-end *as far as it goes*: the publish `Store` now has an owner, a
spawn-time install (`attach`), a resume-time supply/verify arm, position-stability instead of
byte-identity, and a named-seam registration beside P4's wake. But the chosen spawn arm —
post-construction `attach` inside the executor — inherits the one thing `resume`'s
ctor-injection delivers for free and `attach` cannot: the base-derived genesis state (`home_`,
session dims, `opcodeMap_`), all frozen on a base-less arena before the adapter can reach it, all
behind private members with no re-genesis door. The seam must be widened (spawn base-injection, the
symmetric shape) or the spawn arm under-delivers the very ceremony it names. Two minors: the seeded
store's `documentId` adoption, and the verify arm's comparand under scroll remap.

VERDICT: NOT CONVERGED — 0 blocking, 1 major, 2 minors, 5 nits
