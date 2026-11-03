# Round 18 critique (vortex role): vagent-metaprogramming.md v1.17

Lane: VortexCore/VM execution semantics, store/spool positions, handle
relocation, promote, genesis pass order. Convergence-freeze round —
narrow mandate: (1) verify the r17 rulings landed *correctly* (R17-B
filing coverage/ordering, R17-A positional `+d.step` carrier);
(2) hunt a MAJOR only in **pre-existing mechanisms** — text predating
v1.17 that an earlier ruling may have left mechanically broken.

## Verification of the r17 dispositions

### R17-B — `d.dims` filing coverage + ordering: verified, landed correctly

**(i) Whole minted-dim vocabulary enumerated.** All three sites carry the
same closed enumeration — no name class left out:

- §1 :113-121 — "the `d.dims`-rank member set `dimensions()` walks
  (`manifold.cpp:771-775`), every mint path appending there:
  `initGenesis`'s `mintNamedDimension` V10 names
  (`vortex_core.cpp:50-69`), the `ensureDimension` apparatus names, and
  `dimConstant`/`findOrMintDimension`'s mints (`compiler.cpp:769-772`)".
  Verified: `Manifold::dimensions()` walks `rankAfter(*this, *home_,
  *dimsDim_)` at `manifold.cpp:767-779` (the `std::ranges::copy` is at
  :774-775); the V10 genesis block mints 19 names at
  `vortex_core.cpp:50-68`; `dimConstant` emits
  `literal(int64(core_.findOrMintDimension(name)))` at
  `compiler.cpp:769-772`; and `mintNamedDimension`
  (`vortex_core.cpp:17-26`), `mintDimension`/`findOrMintDimension`
  (:72-98) and `ensureDimension` (`arena_manifold.cpp:77-105`) all
  append to the `d.dims` rank tail on mint — "every mint path appending
  there" is literally true.
- genesis (b) :2277-2281 — same enumeration, same cites.
- §10.1 sketch :2977-2991 — adds the one set the other two sites leave
  implicit: `attach()`'s `d.store-refs` (`arena_manifold.cpp:289-290` —
  verified: `ensureDimension("d.stores")` :289 is a V10 name already
  covered; `ensureDimension("d.store-refs")` :290 is the genuine
  attach-only name) and renames the predecessor's overclaim — the
  "every other `ensureDimension`-minted name" phrasing is gone
  (grepped: zero hits outside changelog retro-confessions).

**(ii) File-before-pin at spawn-seed.** :119-120, :2281-2283, :2993-2999
all state it with the mechanism: "`getOrCreate`'s rebuilt fold is the
injected base (`dimension_registry.cpp:194-217`)". Verified
`getOrCreate` at `dimension_registry.cpp:161-184`: on miss it calls
`store.makeDimension(head, name)`, rebuilds the fold via
`store.rebuildManifold(head)`, then **`manifold = std::move
(newManifold)`** at :207-214 — the caller's `Manifold&` *is* the fold
returned, so filing on the to-be-injected manifold before `attach` pins
it is exactly the seam the doc describes. The sketch's added warning is
mechanically accurate: "a filing after the pin is invisible to
`dimensionNamed`'s base arm and the arena mints twins regardless" —
`dimensionNamed` reads `base_->dimensionNamed(name)` before
`arenaDims_` (`arena_manifold.cpp:57-67`), so a post-pin filing is
invisible to the session. My r17 MINOR #2's one-clause fix landed as
a full paragraph.

**(iii) File-before-first-resolution at emit.** :118-119 ("at emit
before `initGenesis`"), :2283-2285 ("before its compile arena's
`initGenesis` (`vortex_core.cpp:14,:29-70`)"), :2999-3006. Verified the
ordering claim end to end: file on the artifact's lineage store →
rebuild fold (includes filings) → `ArenaManifold(base=fold)` →
`VortexCore` ctor `initGenesis()` (`vortex_core.cpp:14`) →
`mintNamedDimension` hits `baseDimensionNamed` for every filed name →
returns the persistent cell and appends **nothing** to the arena rank
→ `promote`'s `d.dims` skip (`arena_manifold.cpp:1766`, `:2029-2033`)
has no session filing edges to drop → the sealed image's dims are all
persistent. The closure is indeed by pre-emption — and now *both*
lineages state it.

**(iv) The residual is honest, not overclaimed.** §10.1 :3005-3006 and
(e) :2377-2380 now scope it correctly: "a name first minted mid-session
stays outside the filed set — its orphan accretion the honest residual";
(e) reads "an *unfiled* session-minted dim in use accreting one such
orphan per checkpoint … re-bound off the filed rank for a filed name,
re-minted only for an unfiled one". The v1.16 overclaim ("every other
ensureDimension-minted name") is gone and the "dead weight" sentence now
covers the re-bound/re-mint split — my r17 MAJOR's "scope the claim
honestly" arm landed.

### R17-A — positional `+d.step` schema carrier: verified, landed correctly

- **Carrier named.** :1186-1190 — "the member's own `+d.step` position
  in the chain carrying the field identity — a link tuple's fields four
  `+d.step`-consecutive members in `(from, dim, dir, to)` order"; echoed
  at §6.6 :2131-2133, the diagram :1078-1080, and §4.5 :390-395.
  Position-keying removes the M14 collision my r17 MINOR #3 diagnosed:
  no `+d.name` is spent on field identity.
- **Dim-name reservation pinned.** "`+d.name` on a `+d.recipe` member is
  that dim-name channel alone" (:1190-1192); "`+d.name`'s presence **on
  a `+d.refs` record** is the discriminator the pre-pass reads; on a
  `+d.recipe` member `+d.name` is the `dim` field's dim-name channel
  alone, the member still marked like every field" (:1116-1121). This
  matches the re-kind's read — it checks `+d.name` on the `+d.refs`
  *record*, while the `dim` member's `+d.name` sits on the `+d.recipe`
  member (a different cell) → the member's record stays a REF-record →
  still marked → still re-kinded. No over-fire of (e)'s exemption.
- **Ref-capture pairing scoped.** "one per `+d.refs` *slot* ref-record
  (a `+d.recipe` field's ref-record pairs with nothing, the member its
  own endpoint record — no `+d.refFor`)" (:1090-1093, :1206-1208,
  :2368-2370) — the zigzag minor landed verbatim in all three places.
- **`closed` arm (c) gained the member form.** :1946-1948 — "or, for a
  `dim` tuple field, on the field's own `+d.recipe` member's `+d.name`"
  — consistent with the glossary row (:146) and the re-verify.
- **`reify` name-cell re-mint.** :1257-1261 — "every `+d.name` name cell
  re-minted per copy … never re-linked off the donor's name cell".
  Verified the justification against `link`'s displacement:
  `arena_manifold.cpp:1348-1373` — setting `record +d.name→ nameCell`
  where `nameCell`'s `-d.name` is occupied clears the displaced pair via
  `setOneSide`, i.e. re-linking off a shared name cell would sever the
  donor's edge. Re-mint is required, not cosmetic.
- **(e) write targets correct.** :2374-2377 — "into the record's
  `+d.refFor`-paired slot or the member cell itself respectively" — the
  dim-field member being its own field cell, the write lands on the
  marked cell, not on a nonexistent pair.
- Changelog confesses the pin (:4025-4028) and annotates the 1.16 entry
  (:3943-3946) — per convention.

## Part 2 — pre-existing mechanism hunt

Re-walked the load-bearing machinery that predates v1.17, specifically
looking for a mechanism an *earlier* ruling re-broke or that was always
broken:

- **The (a) ops-record order comparand** (:2203-2237): pairwise decoded
  `OpRecord` fields + scroll identity by key — `adoptOpRecords`
  deterministic produces-order fold (`store.cpp:3836-3940`),
  `mergeValues`'s `(contentHash, authorKey, parent)` import order
  (`mesh_value.cpp:519-523`), `opsSpool` id↔index (`store.cpp:175+`),
  `sealLocalSpool` whole-spool seal (`publication.cpp:585`,
  `mesh_value.cpp:227-228`). Coherent.
- **The re-kind → publish → restore ceremony** (:2144-2191): `pending
  (pid)==1` gate + P4 wake (`mesh_actor.cpp:659-672`), `OpHandle`-kind
  cells never execute, `isCellRelocationHandle` requires kind *and*
  `d.mesh.relocation→"cell"` marker (`foreign_op_handle.cpp:98-117`),
  marker-precedent cite `mesh_evaluator.cpp:733-735` exact
  (`setValueBits(OpHandle,target)` + `link(cell,relocation,makeCell
  ("cell"))` in the per-reference loop), kind-guarded idempotent
  restore. Coherent.
- **Promote internals**: `!isEphemeral` self-map `real[c]=c`
  (`arena_manifold.cpp:1865-1867`), `d.dims` skip before `discover`
  (:1766), `real.find(edge.dim)` gate (:2029-2033), deferred-mint
  ordering (:1797-1828), handle enqueue (:1759-1764). Filed dims fold
  `x -(D_p)→ y` canonical; session edges on unfiled dims still
  orphan-fold — the confessed residual.
- **`ensureDimension` order** (`arena_manifold.cpp:77-105`): base →
  `arenaDims_` cache → mint + `d.dims`-rank tail append (the tail walk
  traverses base edges too, so session mints append after filed dims).
- **`attach()` dims** (:282-290), **`ArenaManifold::dimensions()`**
  base+arena merge (:34-46), **`wingOf` operand lambda**
  (`asInt64`→`contains`→`masterOf`, `vortex_core.cpp:358-366`),
  **`cloneMaster`** `rankEnd` NEG (`arena_manifold.cpp:1009-1012`),
  **`setOpcodeKind`** map write (`vortex_vm.cpp:98-100`), **`resolveCell`
  returns the operand cell** (`vortex_vm.cpp:50-67`), **cells-mutating
  refusal** (`compiler.cpp:484-486`), **`link` displacement**
  (`arena_manifold.cpp:1348-1373`). All as the doc claims.

**Result: no mechanically-broken pre-existing mechanism found.** The
dim-field member's own mark → re-kind → `OpHandle(dim)` → twin-fold →
(e)-name-arm overwrite chain tolerates the filed/unfiled split exactly
as specified; the `+d.name` discriminator refinement is consistent
across re-kind, `closed`, and (e).

## Findings

### 1. MINOR — the recipe diagram keeps one stale "(name → tuple position)" echo without the `+d.step` carrier

**Location:** :1083 (head diagram, recipe block).

The block correctly pins "the member's chain position its field
identity" at :1079-1080 — but :1083 still reads "the member record
carrying the field's schema (name → tuple position)" with no carrier.
An implementer pattern-matching on the parenthetical alone could miss
that the carrier is positional. **Fix:** "(name → tuple position, the
member's `+d.step` position its field identity)" — the same suffix
:1187 and :2132 already carry.

### Nits (non-blocking, informational)

- The `+d.step` field-identity rule implies a tuple base offset in the
  mixed chain (literals/capture/ref-capture records occupy positions
  too) — position mod-4 needs the tuple region's start; implied by
  emission order, not pinned. v1.17-new wording — outside this round's
  hunt mandate; noted for the record.
- `vortex_core.cpp:50-69` cite: the V10 mint block actually spans
  :50-68; trivial.

## Summary for the orchestrator

R17-B landed correctly on all three sites: the whole minted-dim
vocabulary is enumerated (`dimensions()` rank walk, V10 names,
apparatus names, `dimConstant` mints, `attach()`'s `d.store-refs` —
verified `manifold.cpp:767-779`, `vortex_core.cpp:50-68`,
`compiler.cpp:769-772`, `arena_manifold.cpp:289-290`), file-before-pin
is mechanically true (`getOrCreate` move-assigns the rebuilt fold into
the caller's `Manifold&`, `dimension_registry.cpp:194-217`), and
file-before-first-resolution is stated for the emit lineage (before
`initGenesis`, `vortex_core.cpp:14,:29-70`) with the mid-session
residual honestly scoped. R17-A's positional `+d.step` carrier is
consistent across diagram/§6.1/§6.6/(e)/`closed`, the `+d.name`
discriminator stays on `+d.refs` records, and the `reify` name-cell
re-mint is justified by `link`'s displacement semantics
(`arena_manifold.cpp:1348-1373`). Pre-existing-mechanism hunt: the
comparand, ceremony, promote, and dimension paths all verify — nothing
mechanically broken found. One MINOR (a stale "(name → tuple position)"
echo at :1083 lacking the carrier suffix) plus two informational nits.

VERDICT: CONVERGED
