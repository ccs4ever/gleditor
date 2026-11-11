# Round 13 critique (vortex role): vagent-metaprogramming.md v1.12

Lane: VortexCore/VM execution semantics, store/spool positions, trail/marks,
handle relocation, promote, MeshActorRuntime invariants. Convergence check on
the v1.12 deltas: R12-A (`d.refFor`/`d.captured` correlation dims, :1024-1025,
:1052-1057, :1137-1156, :2164-2167), R12-B (dedupe scoped to the install rank,
`alias` as superseded-target record, genesis (d) verb re-enactment,
:1183-1233, :2138-2154, per-verb label/serial split :1738-1750), R12-C
(segment-apply surface, :2787-2791, :2805-2810), R12-D (canonical comparand,
:2055-2064, §6.6 cite :1585-1587); plus the applied minors (`d.patch`-record
`unlearn` refusal, re-mint `+d.name` transport, `≤E_ops` walk scoping, agent
cell `d.vars` pin, `d.weight` de-ranked).

## Verification of the r12 dispositions

### R12-A — dedicated correlation dims: mechanism adopted, but the shared-referent indirection re-instantiates the same collision (finding 1)

**Code citations — all exact.** `wingOf` enters the input wing through
`linked(opcode, dims_.grab, POS)` at `vortex_core.cpp:366` (`side` parameter),
returns `{}` when `!first` at :367 — the doc's claim that severing the head
edge makes `inputsOf` read empty (:1142-1143) is verbatim correct.
`bindInput` writes `opcode +d.grab → slot` for the first-position slot at
`vortex_core.cpp:387-390` (fresh `makeScalarCell(int64(operand))` :384).
`ArenaManifold::link`'s displacement reads `displacedIt =
theirs->neighbor(-dir)` at `arena_manifold.cpp:1353-1356` and clears the
displaced side with `setOneSide(*other, dim, dir, noCell)` at :1369-1373 —
both inside the cited :1347-1377 window (:1043, :1984). `ensureDimension`
resolves `dimensionNamed`/`arenaDims_` before minting
(`arena_manifold.cpp:78-105`) — the name arm the `+d.name →` dim-name replay
rides (:1154-1156, :2181).

**Design application — correct for the primary target.** `d.refFor`/`d.captured`
are listed among the data-minted dims at :95, :1024-1025 and the no-marks
minting set :533-534 (`ensureDimension`, V10). `+d.grab`/`+d.values` are
barred with the right occupancy argument at :1137-1149 (the `-d.grab` wing
edge for slot correlation; a `d.vars` referent's live `-d.values` backlink
for the referent edge). `-d.refFor` on the slot is "free by construction" —
true: a fresh dim, one ref-record per marked ref cell, and the slot *is* the
ref cell, so one `+d.refFor` in-edge per slot. The `+d.refs`/`+d.recipe` join
through the shared slot cell is O(1) via the slot's `-d.refFor` backlink.
Genesis (e) restates the pairing correctly (:2164-2167). The `+d.name` program
-side arm is unchanged from v1.11 and still consistent.

**The residual hole is finding 1 below** — the shared-referent "one hop"
indirection moves the single-occupancy collision onto records that are
themselves single-occupancy targets.

### R12-B — verb re-enactment: verified, composes with `plan_named`'s verbatim walk

The dedupe is scoped to "the clause `+d.name`s **of the install goal's own
`d.clause` rank** — the rank the patch records as the attach parent"
(:1201-1204); the descriptor's parent field exists (:1768-1769 "the
`goal`/`event` name replay needs to re-run the attach"). The match is "any
verbatim hit: declared or grafted, qualified or unqualified" (:1204-1206) and
appends only on no live hit (:1211-1212) — the r12 self-contradiction is
gone. A learned clause's `patch:<n>` label is unreachable because `patch:` is
refused as a `name` argument (:1206, :1195-1197), so the dedupe can never
mis-hit a learned clause. The `alias` field is the recorded superseded-target
name (:1213-1215) and genesis (d) "re-enacts each patch's verb": re-match
verbatim, re-unlink the hit (:2150-2153), `learn_method` riding
`graft_method`'s unlink-first, `learn_handler` re-appending (:2153-2154).

Composition with `plan_named`'s verbatim walk is **sound**, and the append
-only rank discipline is what makes it sound: grafts/learns append to the
`d.clause` rank (`graft_plan` row :1105; `rankTail` append
`vortex_stdlib.cpp:4238-4241`; `insertIntoRank`
`arena_manifold.cpp:1423-1439`), so declared clauses always rank before any
grafted or learned twin. Session first-match therefore hits the earliest
declared speller; a grafted clause is displaced only when no declared clause
carries the name. At replay the folded rank contains exactly the declareds
(installs re-append as `patch:`-labeled copies the alias cannot match), so
re-matching displaces the same declared clause the session displaced — or
nothing, when the session hit a session-only grafted clause, which is gone
post-resume anyway. Durable outcomes converge in every interleaving I could
construct, including `learn_where` interposed on the displaced clause
(both eras end with it unlinked) and `unlearn` of the displacing patch
(session-dead, folds back next genesis — :1221-1223 states exactly this).
`unlearn`'s `-d.patch` record refusal (:1794-1796) is a sound discriminator —
every `d.patch` member's backlink is occupied on a same-dim `insertIntoRank`
chain, and no clause cell carries one.

### R12-C — segment-apply surface: registered

§10.2 names it adapter-side surface (iii) at :2787-2791 and registers it
standalone at :2805-2810, conditioned on the kept-in-step twin — the
`base()->store()` verify arm stands alone. Both sites describe
`adoptOpRecords` (decoded records, `store.cpp:3836`) and `historyFromSeal`
(own `Store`, `publication.cpp:1093-1103`) accurately.

### R12-D — canonical comparand: harmonized, with one under-specified arm (finding 2)

(a) :2055-2064 now states the single canonical form — order-vs-order decoded
`OpRecord`/`CompactOpNode`, "op kind plus every non-scroll field verbatim,
plus scroll *identity* by key" — and §6.6's verify arm cites it verbatim
(:1585-1587). `scrollKey` is `scroll.cpp:13-22` (optional return; `btpk:` for
named, `file:` for unnamed-with-segments, `nullopt` for anonymous-empty);
`Store::scroll` disengages on `localScroll` **and** `id > externals.size()`
(`store.cpp:1461-1464`, doc cites :1461-1469 — accurate); `localScroll == 0`
at `spool.hpp:19`; the remap (`publication.cpp:1048-1086`, `applyOpsSegment`
at :1030) rewrites `op.span.scroll` per record at :1079-1086 while leaving
everything else as written. `CompactOpNode.scrollId` *is* `op.span.scroll`
(`compact_op.hpp:54`, `span()` :66-69). The `localScroll` arm is mostly
defensive — `applyOpsSegment` remaps `localScroll → selfScrollInHistory`
(:1049), so band records from either sealed derivation carry real external
ids and the `scrollKey` leg does the work. The remaining gap is the
disengage arm's coverage — finding 2.

### Minors applied — verified

- **Re-mint `+d.name` transport** (:2143-2149): fresh label cell verbatim off
  the record, never a re-link — the "drop `patch:<n>` at the *second* genesis"
  reasoning is mechanically right (a re-link would evict the record's own
  `-d.name`-occupied label edge, leaving the record nameless for the next
  genesis). Two equal-text cells per clause-installing patch is correctly
  counted per-incarnation.
- **Per-verb label/serial split** (:1738-1750): every `learn_*` consumes
  `<n>` and carries `+d.name → patch:<n>` on the record; clause-installing
  verbs mint a second equal-text label on the installed clause; consistent
  with :1176-1179 and (d).
- **`(d)` no-serial scoping** (:2138-2140): scoped to `patch:<n>`; the install
  copy's `reify:<src>:<serial>` mints off the same counter — consistent.
- **`≤E_ops` agent-rooted walk** (:1491-1497): the claim is now scoped —
  "nothing an agent-rooted walk reaches *through a link beyond its root* can
  classify program-side" plus the exempt-`agent_cell()` carve-out. Sound
  under promote's either-direction walk: a state-side agent cell cannot have
  image edges pointing at it (it postdates the image), and the next hop out
  of any reached state cell would need a V12-barred state→program edge.
- **Agent-cell `d.vars` pin** (:1179, :1719, :1808-1809, :2262, :2352-2354,
  :2616): consistent everywhere; module `d.vars` named program-side.
- **`d.weight` de-ranked** (:1494-1495): now the §8.1 scalar edge dim.
- **Cite nits**: `real.emplace` :1868 (:1583, :1650); `store.cpp:3840` for
  `opsSpool.contains` (:2105 — the `contains` call is at :3840 exactly);
  `store.cpp:124` for the `userPermascroll_` mint (:3386 — the initializer
  is at `store.cpp:124`, `:125` is `chronofilade_`); `real.at` sites
  `arena_manifold.cpp:1950`,`:1996` inside the cited `:1947-1954`,`:1992-1996`;
  `setLink` `real.at(arena)` at `:2041` (:1649).
- **`compiler.cpp:769-772`**: the `dimConstant` lambda mints
  `literal(int64(findOrMintDimension(name)))` — an int64 ref cell naming a
  folded dim, matching the `+d.name` program-side arm's `ensureDimension`
  re-resolution. `mesh_evaluator.cpp:735` per-reference marker mint verified.

## New findings (v1.12 deltas only)

### 1. MAJOR — the `+d.captured →` "earlier record" indirection collides on the record's own `-d.captured` backlink for ≥3 sharers, and cross-head sharing is unencodable

**Location:** :1149-1153 (the "named once / later records / one hop" rule),
restated :2166-2167 ("one hop on a shared referent") and :3466 (changelog).

**Problem.** The indirection exists because a referent's `-d.captured`
backlink is single-occupancy ("two records naming one referent collide on the
same single-occupancy slot", :1148-1149) — but the fix re-creates the identical
collision one level down. Read under the only interpretation consistent with
the stated "one hop" bound — every later record `+d.captured →` the unique
earlier record that names the referent — a third sharer's link evicts the
second's: `link(record3, d.captured, POS, record1)` reads record1's
`-d.captured` neighbour (record2) as `displacedIt` and clears record2's
`+d.captured` (`arena_manifold.cpp:1352-1356`,`:1369-1373`). Only the last
spoke survives; every earlier spoke's slot has no endpoint. The charitable
reading — each later record chains to the *immediately prior* record for that
referent (record3 → record2 → record1 → referent) — is M14-safe but
contradicts the stated bound: the k-th sharer needs k−1 hops, not "one hop",
and the text never says which earlier record is meant. As specified, one
branch silently corrupts and the other violates its own invariant.

Second, and worse, **cross-head sharing is unencodable**: the discriminator
for "is this `+d.captured` target an indirection" is membership of *this*
head's `+d.recipe` chain (:1150-1152). Two registered heads capturing the
same state-side referent X — two learned clauses both binding, say,
`cells"$a/+d.position"` — give head_B's record no spec-conforming edge at
all: `record_B +d.captured → X` evicts head_A's record_A edge outright
(X's `-d.captured` is already occupied); `record_B +d.captured → record_A`
fails the discriminator (record_A is not on head_B's chain → it is read as
the *endpoint*, minting `int64(record_A)` into the copy's slot — a recipe
record as operand).

Blast radius: donor slots survive (`*handleTarget` rehydrates them), but every
`reify` and `learn_*` derives the copy's ref slots "from the capture-record
endpoints" (:2155-2157, :2183-2188) — severed or misresolved endpoints mint
wrong or absent operand values in every installed copy. Reachability is
routine, not exotic: `compiler.cpp` binds shared canonical cells
(`cells_->zero`/`minusOne`/`fail`/`unit`, :531-532, :781, :785, :796, :805-806,
:826-827) and one `*subject` across every step's input wing (:779-795), each
binding a fresh ref slot on the same referent — a three-op fragment with a
shared constant or subject hits the ≥3 case immediately.

**Fix.** Either chain to the tail — "each later record `+d.captured →` the
current tail of that referent's record chain (the cell reached by walking
`-d.captured` backlinks from the referent — free by construction, since only
the tail's backlink is unwritten); the discriminator is *shape*, not chain
membership: a `+d.captured` target carrying its own `+d.captured`/`+d.refFor`
is a record, anything else is the endpoint; replay unwinds to the endpoint,
k−1 hops for the k-th sharer" — or spend a cell per referent (house style):
a capture-hub cell `H` per shared referent (`H +d.captured → referent` — the
single writer of the referent's `-d.captured`), every ref-capture record
chained `+d.step` off `H +d.share`; the hub is found in O(1) via the
referent's `-d.captured` backlink, works across heads, and keeps "one hop"
true per record. Either way, restate (e)'s "(one hop on a shared referent)"
and the §6.1 enumeration, and drop the chain-membership discriminator for a
shape test.

### 2. MINOR — the comparand's disengage arm names only `localScroll`; `breakMarkerScroll` misfires

**Location:** :2061-2063 — "`Store::scroll` ... disengages on `localScroll`
..., so local-scroll ids compare as ids (both `localScroll`, else unequal)".

**Problem.** `Store::scroll` also disengages on `id > externals.size()`
(`store.cpp:1463`), and `breakMarkerScroll` — a legitimate sealed `op.span.scroll`
value, identity-remapped precisely because ops carry it (`publication.cpp:1050`;
nodes holding it are skipped, not rejected, by `sealableOps`' table builder at
`:985-986`) — disengages too. Under the stated rule, two identical records
carrying `breakMarkerScroll` compare "else unequal" and fail the band compare —
a false refusal of an honest resume. In-domain reachability is thin (image-band
ops rarely write break markers) but the rule is wrong as written; the
anonymous-empty `scrollKey` nullopt (`scroll.cpp:17-19`) is *un*reachable —
`sealableOps` refuses to seal keyless scrolls (`publication.cpp:1000-1006`).

**Fix.** "Disengaged ids compare as ids — equal iff identical (`localScroll`,
the identity-remapped `breakMarkerScroll`); engaged ids compare `scrollKey`" —
or adopt `globalKeyOf`'s arm set (`publication.cpp:853-869`) outright.

### 3. MINOR — "the alias is write-only audit metadata" is contradicted by its own sentence

**Location:** :1186-1190.

**Problem.** The clause now says the alias "is write-only audit metadata ...
its only two uses are the `d.patch` rediscovery join below and `learn_plan`'s
replay dedupe" — a join key and a replay key are reads, so the alias is
read twice.

**Fix.** "the alias is *session-write-only* — never a session lookup key; its
two reads are the replay dedupe and the `d.patch` rediscovery join".

### 4. MINOR — "share the `patch:<n>` `+d.name`" phrasing persists beside its own correction

**Location:** :1721-1723.

**Problem.** "Patch and installed clause share the `patch:<n>` `+d.name`
intentionally" still invites the structurally impossible shared-label-cell
reading (the label cell's `-d.name` backlink is single-occupancy — the r12
finding-4 point); the correction exists at :1741-1743 ("two cells, never one
shared") twenty lines later.

**Fix.** "...carry equal-text `patch:<n>` labels on their own `+d.name` label
cells intentionally — the patch's label names the clause it installed".

### 5. MINOR — `learn_method`'s label-omission justification does not parse

**Location:** :1746-1748 — "`learn_method`'s installed symbol keeps its
functional `owner.name` content — a `patch:` label under it would shadow the
`d.vars` name walk it is found by".

**Problem.** The `d.vars` name walk matches symbol *content*; a `+d.name`
label edge cannot shadow it, and `plan_named`'s space is clause-rank cells
(:1723) so no lookup collision exists. The choice (label on the record alone)
is right; the stated reason is wrong — the honest reason is that the symbol's
durable handle is its `owner.name` content and a `patch:` label would mint a
second, misleading name with no resolver to consume it.

**Fix.** "...keeps its functional `owner.name` content — its `patch:<n>` lives
on the record alone, since the symbol resolves by content, not label".

## Nits (non-blocking)

- :1050-1057 — the head diagram shows `+d.refFor`/`+d.captured`/`+d.name` but
  omits the shared-referent indirection arm entirely; whatever finding 1's fix
  is, the diagram should carry it.
- :3466 — changelog repeats "one `+d.captured` hop"; needs restating with
  finding 1's fix.
- :2059 — "`scrollKey(*store->scroll(id))`" — fine as written since the
  dereference only applies to engaged ids, but the guard ordering (engage
  check → key → id fallback) is worth making explicit given `scroll()`'s
  `cpp26::optional` return.

## Summary for the orchestrator

R12-A's dedicated dims are the right mechanism and correctly argued against
the code, but the shared-referent indirection re-instantiates the same M14
single-occupancy collision on the record's `-d.captured` backlink — broken at
≥3 sharers under the stated "one hop" fan-in reading, and unencodable across
heads under any reading. R12-B's verb re-enactment composes with `plan_named`'s
verbatim walk — the append-only rank discipline makes session and replay
first-match converge. R12-C is registered in §10.2; R12-D's canonical
comparand is harmonized with one under-specified disengage arm
(`breakMarkerScroll` misfires). Every code cite I checked is exact.

VERDICT: NOT CONVERGED
