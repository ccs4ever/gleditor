# Round 14 critique (vortex role): vagent-metaprogramming.md v1.13

Lane: VortexCore/VM execution semantics, store/spool positions, handle
relocation, promote, genesis pass order. Convergence check on the v1.13 deltas:
R13-A (`d.captured` dropped; endpoints ride content), the comparand
`globalKeyOf` arm, the comparand pronoun, and the `alias` "no session resolver"
family — plus a fresh defect hunt over the whole `git diff` (25 hunks, ~425
lines).

## Verification of the r13 dispositions

### R13-A — `d.captured` dropped everywhere in live text: verified, and the content-carried endpoint closes all three cases

**Every live-text occurrence is converted.** `d.captured` survives only in the
changelog (:3510-3513, :3549-3559 — correct history). Minted-dim lists all
carry `d.refFor` and no `d.captured`: :95, :533-534, :1017-1018, :1024-1025,
:2788 (§10.1 — `d.refFor` restored), :3027-3028 (§14 R12 — restored). The head
diagram (:1050-1059), §6.1's enumeration (:1146-1169) and genesis (e)
(:2199-2230) all state the same three-arm content-carried rule.

**Code mechanics re-verified.** `bindInput` mints `makeScalarCell(int64(operand))`
(`vortex_core.cpp:384`) — a ref-box slot's int64 *is* a `CellRef`; `dimConstant`
mints `literal(int64(findOrMintDimension(name)))` (`compiler.cpp:769-772`) — the
literal cell's content is a `DimRef` (dims are cells, `ensureDimension` returns
`makeCell(span)`, `arena_manifold.cpp:87-89`). The three-arm split maps onto
this exactly: a wing slot `S` holding `int64(L)` has an *internal* referent `L`;
the literal `L` itself, holding `int64(dimRef)`, is the *program-side* "slot"
whose endpoint is the dim — consistent with :1161 calling `L` "a `dimConstant`
slot". `S` carries a ref-record + internal-arm recipe twin; `L` carries a
ref-record (its `+d.name` lives on the *recipe* record, so the refs-record is a
REF-record → marker → re-kind → `OpHandle(dimRef)` → promote drags the dim cell
in through `isCellRelocationHandle`'s target walk, `arena_manifold.cpp:1759-1764`)
+ program-side recipe twin (`+d.name → dim name`).

**The three failing cases, closed:**

- **≥3 sharers** (`compiler.cpp:531-532,781,796` — `cells_->zero`/`minusOne`
  per-emission shared cells, `*subject` bound across every step's wing): N ref
  slots each carry `int64(same-ref)`; there is no edge into the referent at all,
  so no single-occupancy backlink exists to collide on. `+d.refFor` is
  per-record → own-slot; the slot's `-d.refFor` sees exactly one edge.
- **Chains**: the indirection is gone — there is no record→record hop to
  bound. "one hop" is trivially true: slot content *is* the endpoint.
- **Cross-head copies**: a `reify` copy `Value`-replays `int64(endpoint)` —
  for external, the identical bits `*handleTarget` names (:2222-2225, stated
  honestly: "an equality that holds for the external arm only"); for internal,
  the map-translated twin. Two heads writing `int64(P_ext)` name the same
  referent — correct shared-external semantics, nothing severed.

**The one attack I ran and could not break.** In-session, post-checkpoint, the
donor's slots are restored — I initially suspected a map-key fork (recipe
links stay ephemeral `E_d` while slot content canonicalizes to persistent `P`).
It does not exist: `setValueBits(OpHandle, target)` writes the *untranslated*
endpoint ref — `promote` reads `handleTarget`=`E_d` and emits `real.at(E_d)`→`P`
into the *op* (`arena_manifold.cpp:1947-1951`), while `publishValue` holds the
arena `const&` — so the arena cell keeps `OpHandle(E_d)` and restore writes
`Int64(*handleTarget)`=`Int64(E_d)`, the original. Slot content and map keys
(the tuple links' ephemeral refs) stay in one ref-space per context: ephemeral
in-session, positional at genesis (all fold edges/refs are positions;
`handleTarget`=`P` hits the position-keyed map). Internal/external/program-side
arms are each consistent in both contexts. The `+d.values` "may" edge for
internal endpoints is occupancy-guarded at emission ("only where the endpoint's
`-d.values` is unclaimed") and cosmetic — the content channel never disagrees
with it, since both name the same cell in every context.

**Program-side end-to-end**: `L` marked → `OpHandle(dimRef)` → promote
(`isEphemeral(dimRef)`-dependent: persistent dims verbatim via
`store.getCompactOp` :1809; ephemeral session dims dragged through `seen` and
`real.at` :1762,:1802-1809) → genesis (e) pairs the refs-record to the recipe
record through `L`'s `-d.refFor` → `+d.name` present → `ensureDimension`'s name
arm (`arena_manifold.cpp:78-105` — `dimensionNamed`/cache before mint) writes
`int64(newDimRef)` — correct, and the only arm that must not take `*handleTarget`.
The discriminator ordering is implicit but consistent: recipe-record `+d.name`
present ⇒ program-side; else map membership ⇒ internal (:1153-1154); else
external verbatim.

### Minors — all verified applied and correct

- **`globalKeyOf` comparand arm** (:2083-2094): exact. `globalKeyOf`'s arm set
  (`publication.cpp:853-869`) minus the `localScroll` case — the remap never
  leaves one (`:1049` `remap.emplace(localScroll, selfScrollInHistory)`).
  `breakMarkerScroll` → `breakMarkerKey` (`publication.hpp:447`,
  `"break:"` verbatim); legitimately sealed (`:985-986` table-builder skip),
  identity-remapped (`:1050`). Engaged external → `scrollKey(*store->scroll(id))`;
  any other disengagement → fail. `Store::scroll` disengages on `localScroll` or
  `id > externals.size()` (`store.cpp:1461-1469`) — both named.
- **Comparand pronoun**: harmonized — :1596-1607 ("the comparand is the
  runtime's base store's own `1..E_ops` band ... the adapter's seeded store ...
  supplies the reference band"), :1987 ("verifies the rebuilt store's decoded
  `1..E_ops` band against the lineage's decoded reference band"), and (a)
  :2080-2082 (lineage decoded vs received decoded, pairwise) all agree:
  reference = adapter lineage, checked = received/runtime band.
- **`alias`**: :1195-1198 now reads "never a session lookup key: no session
  resolver consults the alias — the `d.patch` rediscovery join ... and
  `learn_plan`'s replay dedupe do"; conditional presence at :1225-1229 ("the
  field names a superseded clause exactly when the install-rank dedupe hit")
  and join multiplicity at :1202-1205 ("every match ... the caller picks by
  `ordinal`, latest-wins") — all sound.
- **Replay dedupe divergence** (:1228-1234, :2188-2190): covers both cases
  correctly — a session hit on a grafted twin replays as append (grafts never
  fold back); a session append replays as displace when an intervening
  displacer's patch replay-drops on a dead target (the folded-back declared
  clause is then live for the verbatim re-match). Deterministic both ways;
  the end rank agrees in the class that matters (the declared clause is dead
  in both histories).
- **`patch:<n>` equal-text split** (:1740-1744): "each label cell's `-d.name`
  backlink holding a single edge" — the direction slip is fixed.
- **`label_of`/`unlearn` handle** (:1761-1764, :1770): "the uniform
  `label_of`/genesis-transport bookkeeping handle (`unlearn` never matches
  it)" + "`unlearn` matches every verb by the recorded *target* name" — fixed.
- **`patch:`-shadows-`d.vars`** (:1766-1769): claim removed; `textOf` match
  verified at `vortex_stdlib.cpp:1953` (`arena().textOf(sym) == symName`).
- **Agent-cell legs** (:2114-2120): "reached only where a runtime-grown edge
  leads to them ... through capture edges" — mechanically right now: capture
  slots are genuine wing links (`:819-822`, `masterOf(slot)` decode), so the
  program() walk does reach captured `d.vars` cells; they classify `>E_ops`
  regardless.
- **`learn_plan` premise** (:1218-1219): `learn_plan("Patrol","demo:Guard.cautious",…)`
  against §9.2's declared `plan cautious for Patrol` (:2602) — valid.
- **§9.3 `on Meal`** (:2448-2449, :2648-2651): `set_weight` seeds the keyed
  `d.vars` record on first write; `else set_weight(c, 0.1)` covers the
  disengaged read. Consistent.
- **`documentId` arm (v)** (:2832-2836): `historyFromSeal` mints
  `std::make_unique<Store>()` at `publication.cpp:1097`, default ctor
  `store.cpp:123-125` — no id parameter; the `documentId_` field
  (`store.hpp:1435`) is only set by the extended ctor — accurate.
- **`store.cpp:124`**: `userPermascroll_` initializer is at :124 exactly.

## New findings (v1.13 deltas only)

### 1. MINOR — `closed`'s frontier enumeration still names "capture edges on the emission's ... record dims" after the edge was dropped

**Location:** :1863-1864 — "`+d.refs` records, capture edges on the emission's
wing/record dims, and the head's own structural dims".

**Problem.** Under the content-carried endpoint, the state-side endpoints
`closed` actually checks ride slot *content* — the only record-dim capture edge
left is the conditional `+d.values` on internal ref-capture records (absent for
every external endpoint). The enumeration as written implies record-dim edges
enumerate the captures, contradicting §6.1's own corrected statement
(:1166-1167: "`closed` reads each ref-capture endpoint through the paired
slot's own content").

**Fix.** "...— `+d.refs` records, capture edges on the emission's wing dims,
the `+d.recipe` ref-capture records whose endpoints are read off their paired
slots' content (§6.1), and the head's own structural dims".

### 2. MINOR — `closed`'s name-record arm does not textually admit the program-side (dim) endpoint

**Location:** :1839-1847 vs :1161-1165.

**Problem.** Strictly read, a `dimConstant` literal's referent is a
program-side cell (system dims sit `≤E_ops`), and its `+d.refs` record *must*
be a REF-record — if it carried `+d.name` it would lose its marker and its
genesis re-resolution, and the name-record arm would try resolving `"d.weight"`
as a symbol. So the dim endpoint is literally "a program-side reference
lacking a `+d.refs` name-record" — the refused class — making every fragment
with a `Linked`-op dim operand non-`closed`. The intended reading — the
*recipe* record's `+d.name` satisfies the name-recording requirement — is
nowhere stated. Inherited shape (the same gap existed under `d.captured`),
but §6.1's rewritten enumeration is where the exemption belongs.

**Fix.** Add to §6.1 or §6.6: "a program-side (dim) endpoint counts as
name-recorded through its ref-capture record's `+d.name`; the `+d.refs`
name-record requirement applies to call/run bindings, not dim referents."

### 3. MINOR — the (d)-time endpoint read is named `int64` where the donor slot is `OpHandle`-kind

**Location:** :2216-2219 — "the referent itself is read off the paired slot's
*content* (§6.1's rule: internal and state-side endpoints ride the slot's
`int64` ref position ...)".

**Problem.** At genesis (d) the donor's marked ref cells are still
`OpHandle`-kind — (e) rehydrates last — so `asInt64` fails on them; the read
is `*handleTarget`, which the same paragraph gets right at :2202 ("the slot's
own promoted target") and :2222-2223. The `int64` phrasing is right only
in-session.

**Fix.** "...ride the slot's `int64` ref position in-session and its
`*handleTarget` at (d) — a position, never a census integer".

### 4. MINOR — "carried as content, **never as an edge into the referent**" is contradicted two lines later

**Location:** :1147-1148 vs :1155-1156.

**Problem.** The bolded absolute ("never as an edge into the referent") is
qualified by the immediately following `+d.values` *may*-edge into internal
endpoints — the edge exists precisely where it is allowed. Rhetorically the
"never" invites a reader to strike the may-edge as non-conforming.

**Fix.** "...carried **as content wherever an edge into the referent would
collide** — a shared or already-occupied backlink, M14 on both counts — ... and
the record's `+d.values` *may* name its own endpoint cell as annotation, one
record one edge, only where the endpoint's `-d.values` is unclaimed".

## Nits (non-blocking)

- :2227-2228 — the stale-alias rule should name its full scope once: a marked
  ref cell's *recorded literal* is never the derivation source — the
  program-side literal `L` (whose recorded literal is itself a stale `dimRef`)
  is the sharpest case, not only donor wing slots.
- :3555-3558 — the changelog's "external/state-side *and program-side*
  endpoints ride the slot's own `int64` ... or `*handleTarget`" conflates the
  program-side carrier (which is re-resolved via `+d.name`, not carried
  verbatim into genesis). Defensible as written since the carrier is the same
  pre-resolution, but "external/state-side ride the slot; program-side rides
  `+d.name`; internal rebinds through the map" would be cleaner.

## Summary for the orchestrator

R13-A is genuinely closed — this is the first round where the mechanism holds
under every attack I could construct: content-carried endpoints remove the
single-occupancy collision for ≥3 sharers, eliminate the chain reading, and
make cross-head copies trivially encodable, while `promote`'s handle-target
walk (`arena_manifold.cpp:1759-1764`) still drags endpoints into the durable
set and the restore/genesis split keeps slot content and map keys in one
ref-space per context. The comparand `globalKeyOf` arm, the pronoun fix, and
the alias family all verify against code. No BLOCKING, no MAJOR: four MINORs
(stale frontier vocabulary, an inherited `closed`/dim name-record gap, an
`int64`-vs-`handleTarget` phrasing slip, and a rhetorical "never" the may-edge
qualifies) plus two nits. All code cites re-verified exact.

VERDICT: CONVERGED
