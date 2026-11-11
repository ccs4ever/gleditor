# R8 — System Architect critique (v1.7)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene; verdicts on my
r7 findings (the locus major, the empty-set fail-open minor, the fold-narration minor, nits) plus new
defects in the v1.7 deltas only (`:583-598` lineage, `:1332-1375` fail-closed, `:1574-1700`
genesis-as-bind + reorder, `:2187-2196` adapter inventory/seeding, `:2217-2218` P15/P16 rows).
Re-verified against the tree (`mesh_actor.cpp`, `mesh_jobs.cpp`, `mesh_value.cpp`,
`publication.{hpp,cpp}`, `arena_manifold.cpp`, `manifold.cpp`, `vortex_vm.cpp`,
`vagent-agent-language.md`).

v1.7 does what the disposition ruled, and the re-rooting is real: the ordering claim has moved off
genesis and onto the store lineage where it always belonged, genesis is narrated as bind-not-build,
the fail-closed arm is written, and every adopted minor landed. The defect that survives is not in
what the document now says about the fold — it is in what the runtime will let the adapter touch:
the store whose lineage the whole model rides on is a private member of `Actor`, and neither the
spawn path, the resume path, `checkpointView`, nor the `Executor` signature ever hands it out. The
major moved one layer down and is now an API boundary question, which is where it should have been
all along — but it is still open.

## Verdicts on my r7 findings

### 1. MAJOR (image-first at an unsequenceable locus) — RESOLVED in locus and narration; the reach question reopens one layer down (see new finding 1)

Everything r7 asked for is present. The lineage is named at §4.5 (`:591-598`): the agent's store is
anchored on the artifact's ops as low prefix at load, "the `job.base` shared-store pattern,
`mesh_jobs.cpp:303-318`", checkpoints append above it, `sealLocalSpool` seals the whole spool — and
the mechanism is real code: `publishValue` calls `sealLocalSpool` with `opsAlreadySealed` left at its
default `0` (`mesh_value.cpp:226-228`; header default `publication.hpp:340`), so
`sealableOps(store, 0)` exports ops `1..opCount` (`publication.cpp:975-1019`) — every checkpoint's
ops segment is a full-history seal, and `receiveValue`'s `restorePublication` +
`rebuildManifold` reproduce positions `1..E` verbatim (`mesh_value.cpp:345-351`,
`manifold.cpp:427-442` — `birthOp = opIndex` at `:434,442`). §6.6 restates it against the
discriminator (`:1340-1357`) including the "flat append after state would invert the test" clause at
`:1356-1357`. Genesis is now honestly bind-shaped (`:1650-1655`: "arrives — it is not constructed";
step (a) asserts rather than constructs; the arena overlay "adds nothing persistent" `:1665-1666`).
The corollary edits landed: M8's cells are "cells of the persistent base manifold — the program
image folded from the artifact's low ops prefix … which the arena reads through and shadows"
(`:280-283`); M11 and §4.5 say "never *republished*" / "not republished — rides the shared base
lineage" (`:355-356`, `:588`); the `job.base` cite verifies (`mesh_jobs.cpp:304-320`: `fetchValue`
the base, `store = base->store.get()`, evaluate over `base->manifold`, `publishValue` appends).
`promote` does what "not republished" needs — non-ephemeral, non-`holdsOwn` base cells are skipped
(`arena_manifold.cpp:1755`; `holdsOwn` = shadowed-in-overlay, `:270-272`). The stated model is now
mechanically coherent. What it does not yet have is a reachable store — that is finding 1 below,
not a restatement of this verdict.

### 2. MINOR (empty-set arm fails open) — RESOLVED

`:1367-1374` inverts the arm exactly as ruled: a prefix-less lineage (publish off a foreign or fresh
store, `importValue` ingress of ephemeral twins, sibling manifold) reports an empty program-side
set "under which the discriminator **fails closed**: `learn` is refused outright" — with the reason
named ("an admitted `learn` would link the skeleton into every checkpoint … the V12 failure with the
roles reversed") — plus the second refusal class: "a `programRef` that names no cell of the
received manifold is a genesis bind refusal". Mirrored at genesis (a) `:1663-1664` and changelog
`:2645-2647`. Closed.

### 3. MINOR (step (b) narrated the `receiveValue` fold as a genesis overlay) — RESOLVED

The (a)/(b) construction is gone. Genesis opens with "the persistent manifold **arrives — it is not
constructed**" (`:1650-1652`), (a) is now "lineage bind" — assert + inherit, then "the arena then
overlays the base with ephemeral session cells; the overlay adds nothing persistent" (`:1665-1666`)
— the arena overlay claim now attaches to session cells only, which is what the code does
(`WorkerContext(actor->checkpoint->manifold.get())`, `mesh_actor.cpp:270-271`; `refOf` mints
`ephemeralBit|dense`, `arena_manifold.hpp:468-470`). Closed.

### 4. Nits — four resolved, one carried

- **Pump-ordering attribution** — RESOLVED: `:1610-1617` now says the `publishValue` call "is issued
  from the same `Executor` callback that ended the flagged turn, synchronously, before the pump's
  next `pumpOne` decision" and names it "*proposed adapter discipline*, not an attribution to the
  pump" — mechanically true, since `pumpOne` invokes `executor_` inline (`mesh_actor.cpp:473-476`).
- **"Re-`import` the prelude"** — RESOLVED: the genesis section now says "the artifact's ops
  segment, loaded verbatim" (`:1660`); `importValue` appears only as the excluded contrast case
  (`:1686-1690`).
- **Keyless memo record chain growth** — RESOLVED: "no memo record is appended for it either, since
  a keyless record could never hit" (`:297-299`).
- **Carrier residuals** — the (c)-reads-(d)'s-record ordering is resolved by "recorded once at load
  on the adapter's `programRef` bind record" (`:1350-1351`): the record is a load-time product, (a)
  reads it, (c) performs the arena bind — no longer a write before the record exists. "Adapter-side
  durable" is now honestly sourced: "chain-stable because the continuation pins the artifact
  (`grant->programRef == continuation->programRef`, `mesh_actor.cpp:199,215-216`)" (`:1351-1353`) —
  verified at `mesh_actor.cpp:215`. Carried forward as a nit: the assert's *procedure* is still
  implicit (see nits).

## New findings (v1.7 deltas only)

### 1. MAJOR — the lineage store is named but unreachable: nothing in the runtime hands the adapter "the store backing the arena's base manifold" (§4.5 `:593-595`; §6.6 `:1341-1345,1368-1370`; genesis (a) `:1659-1664`; adapter row `:2187-2189`)

**Problem.** The whole re-rooted model stands on one object: the agent's *continuing* store —
seeded with the artifact ops at load, appended by `promote` per checkpoint, re-sealed whole each
time. v1.7 states the invariant on it twice: "`publishValue` runs against the store lineage that
carries the prefix — **the store backing the arena's base manifold**" (`:1368-1369`) and "the
adapter seeds the agent's store on the artifact's ops segment at load" (`:2187`). Under
`MeshActorRuntime`'s current API that object does not exist for the adapter:

- **Spawn creates no store and accepts none.** `spawn()` builds `actor->context =
  std::make_unique<WorkerContext>()` — base-less, store-less (`mesh_actor.cpp:156`); `actor->checkpoint`
  is unset. There is no parameter, no `attach` hook invocation, no moment where the artifact's ops
  could enter anything. "Seeds the agent's store at load" names an object that is not yet allocated.
- **Resume builds the store inside and keeps it private.** `resume()` re-verifies the manifest and
  constructs `verified = receiveValue(manifest, …)` *internally* (`mesh_actor.cpp:260-269`), then
  `actor->checkpoint = std::make_unique<ReceivedValue>(std::move(*verified))` — the caller's
  `ReceivedValue` contributes only the manifest bytes and `checkpoint.store->userPermascrollPtr()`
  as a reader argument; its store is dropped. The store that actually backs the arena's base —
  `actor->checkpoint->store` — is a private member of `Actor` (`mesh_actor.hpp:194`: `struct Actor;`
  opaque). `checkpointView` yields `{continuation, const ArenaManifold&, root}` — no store
  (`mesh_actor.hpp:102-106`). The `Executor` signature hands the callback `WorkerContext&` — no
  store (`mesh_actor.hpp:115-116`, invoked `:474-476`). There is no accessor, no publish-through-
  runtime call, no way to inject one.
- **The only publish path exercised today is the doc's own fail-closed arm.** The resume test does
  `xanadu::Store store; publishValue(store, {}, snapshot->arena, …)` — a fresh empty store per
  checkpoint (`mesh_actor_resume_test.cpp:125-129`). That works in the test only because a
  spawned actor's state is all ephemeral, so `promote` emits everything and the fresh spool is
  self-contained. For a resumed actor with a persistent base, the same call is precisely "a publish
  off a foreign or fresh store" — the arm v1.7 correctly refuses. And it is worse than refused: link
  ops emit non-ephemeral targets verbatim (`isEphemeral(target) ? real.at(target) : target`,
  `arena_manifold.cpp:1950,1996`), so a fresh-store publish over a persistent base writes ops naming
  op indices that do not exist in that store. The continuing store is not a nicety — `promote`'s
  encoding presumes it.

So the invariant is stated against an object the platform's actor API structurally withholds, and
the seam that would fix it is unregistered: the doc still claims "no new mechanism beyond P4's
already-approved wake" (`:1578-1579`, `:2218`). The mechanisms available are real but unwritten —
either (i) the adapter retains its own **content-identical** store (fetch the same manifest it hands
`resume`; positions coincide because a persistent `CellRef` is a spool position, not an object
identity) and "the store backing the arena's base manifold" means *that* copy — then say so,
because as written it denotes the private member; or (ii) a registered runtime seam —
`checkpointView` carrying the `Store&`, or spawn/resume accepting an adapter store. The spawn side
additionally needs the injection spelled: `ArenaManifold::attach(Space)` installs `base_`/`store_`
when unset (`arena_manifold.cpp:274-287`), so a first-turn executor `attach` of the seeded
store+fold is the existing shape — also never mentioned. This is the r7 major's honest residue: the
invariant found its locus, and the locus has no door. Clause-or-seam fixable: state the
adapter-held-copy model (and the `attach` injection at spawn) or register the seam.

### 2. MINOR — the artifact's *seedable form* is unnamed: who emits the ops segment, and "byte-identical" is the wrong invariant (`:1341-1344`, `:1659-1661`, `:2187-2189`)

**Problem.** "Seeds the agent's store with the artifact's ops segment — loaded verbatim
(byte-identical, or no position is stable)" presumes the compiled artifact exists as a `sealableOps`-
shaped byte stream applied into a store — `applyOpsSegment`/`historyFromSeal`
(`publication.cpp:1030+`) is the only code that does that, and nothing in §10.1 says the compiler
emits such a segment, who seals it, or under whose keys/scrolls. The `job.base` cite also elides a
shape difference: `job.base`'s base is a *received published value* (`fetchValue` → `ReceivedValue`
with manifest, version and `job.parent` check, `mesh_jobs.cpp:304-314`); the artifact analog is a
build artifact, and fetch-and-adopt vs apply-into-fresh-store decide store identity
(`historySalt(store.documentId())` names the history scroll salt, `mesh_value.cpp:227`). And the
stated invariant overshoots: `sealableOps`'s per-segment scroll table is remapped into the
receiving store's `ScrollId` space (`publication.cpp:981-1008`, `:1030+`), so the stored bytes
*legitimately differ* per store — what must be identical is the **op sequence and count from op 1**
(positions), plus resolvable scroll names.

**Fix.** Name the carrier (artifact ships as a sealed ops segment / `ops.nodes`, produced once by
the toolchain), name the application (`applyOpsSegment` into a fresh `Store` or
`restorePublication` of the artifact's own publication), and restate the invariant as
position-identity ("the same op sequence at `1..E`"), not byte-identity of the seeded result.

### 3. MINOR — the model silently supersedes the parent spec's load model (cross-document)

`vagent-agent-language.md:425-428` still states "Each agent's arena loads its own copy of the
compiled program … after spawn and again after every `resume`" and defers a shared base manifold to
U-A14. v1.7 makes the program image a *persistent* store prefix folded into the base — the opposite
of per-arena ephemeral loading — and amends M8/M11 internally but never flags the supersession,
while it explicitly says "This document amends V12 narrowly" (`:356-357`) for the adjacent rule. A
reader holding both files cannot tell whether arena-load or store-prefix is the committed model.
One sentence owed: the lineage model supersedes §4.4's arena-load (or narrows U-A14 to "the prefix
is the shared base").

### Nits

- **Genesis (a)'s assert is still procedure-less** (`:1657-1659`): "the persistent high-water below
  which program cells sit equals `E`" is near-tautological — program-side is *defined* as ref ≤ E.
  The non-circular check is a fold-diff: the adapter holds the artifact's ops (it seeded them), so
  assert received-store ops `1..E` equal the recorded image — name the comparand representation or
  the "assert" is unfalsifiable.
- **`sealLocalSpool seals the whole spool` cites the wrong half** (`:595-596`, `:1347-1348`,
  `:2632-2633` → `publication.cpp:611-617`): the cited lines are the incremental machinery
  (`opsAlreadySealed` arithmetic); the load-bearing fact is the call site's default
  `opsAlreadySealed = 0` (`mesh_value.cpp:226-228`). Cite both — as cited, a reader sees a delta
  mechanism and cannot conclude the whole spool ships.
- **One store per agent, never a shared object**: the prefix is *copied* into each agent's own
  `Store` — job.base shares one received `Store` between job and base (`store = base->store.get()`);
  two agents of one artifact must each hold their own, or `promote` appends interleave into a
  shared spool. Say "artifact bytes shared, store objects never". Artifact upgrade is the same
  clause: a new artifact is a new `programRef`, so the continuation pin (`mesh_actor.cpp:215`)
  forces a new lineage — upgrades respawn, never migrate mid-chain; worth stating once.
- **`ActorCheckpointView` unchanged**: if the registered-seam arm of finding 1 is taken, the view's
  `{continuation, arena, root}` triple (`mesh_actor.hpp:102-106`) is where the `Store&` would ride —
  note it in §10.2.

## Structural notes (no finding)

- **The reorder dependency is named, not just swapped** (`:1692-1699`): copy-inheritance — a
  replayed install's slots mint with the registered head's *current* contents (OpHandle yields
  OpHandle) — is stated as the reason (e) must follow (d), on both arms (copies' regenerated records
  + post-installation symbol visibility). R7-B closed.
- **The `opcodeMap_` rationale moved correctly** (`:1671-1675`): the dependency is the empty
  per-`WorkerContext` map, not "stale indices"; `setOpcodeKind` exists as cited
  (`vortex_vm.cpp:98-99`) and the P15 row repeats the pass consistently (`:2217`).
- **`home` post-resume is now the persistent base home** (`:1386-1389`): `initGenesis` adopts
  `*base->home()` (`vortex_core.cpp:30-36`), `home_` = first MakeCell = op 1 — inside the image
  band, which is also what makes the "program-side `agent_cell()`" exemption coherent. The
  base-less arm is still named for genuine no-base genesis. Consistent.
- **Prefix lifecycle across agents is clean**: per-agent store copies of identical bytes →
  position agreement by content-identity, no shared-mutable owner, no cross-agent hazard — the
  residual is only the unstated seeding/upgrade story (finding 2, nit 3).
- **Makefile inventory unchanged and correct** (`:2197-2202`): `VAGENT_FRONTEND_SRCS` gains the five
  TUs, `VAGENT_SYNTAX_OBJS` correctly stays put; `fragment.cpp` rides the kernel glob.
- **Cites re-verified**: `pumpOne`/`pending`/`checkpointView` (`mesh_actor.cpp:460-505,659-694`),
  `ActorContinuation` pin fields (`mesh_actor.hpp:94-100`), `importValue` handle demotion claim vs
  resume (`:1686-1691`), `holdsOwn`/`isEphemeral` gates in `promote` (`arena_manifold.cpp:1755,
  1797-1809`).

## Verdict

The three-lane fix landed and landed honestly: lineage named at §4.5/§6.6, genesis re-narrated as
bind, fail-closed written, reorder justified, the minor batch applied without residue beyond nits.
The remaining major is narrower and better-placed than last round's — not "the invariant has no
mechanism" but "the mechanism's object is on the wrong side of the `Actor` API" — spawn has no
store to seed, resume keeps the store private, and the adapter's only real publish path is the
fail-closed arm. One honest paragraph (adapter-held content-identical store + `Space`/`attach` at
spawn) or one registered seam closes it.

VERDICT: NOT CONVERGED — 0 blocking, 1 major, 2 minors, 4 nits
