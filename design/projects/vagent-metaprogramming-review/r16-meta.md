# Round 16 critique — `design/vagent-metaprogramming.md` v1.15 — Metaprogramming Expert lane

Scope: verify my r15 items (R15-B's uniform marked-int64 tuple keyspace, :1144-1155; the carried
program-side/dim writeback minor, now :2241-2264; the alias-scoping nit, :1776-1778), judge whether
the recipe/ref-capture endpoint model is now closed, and hunt new defects in the v1.15 deltas only
(full diff `git diff 406d079b..b7e2a655` reviewed; code citations re-verified against
`apps/common/xanadu/`).

## 1. Major verification — R15-B: **FIXED** for the reported hole; one residual scope clause remains (Finding 1)

The stale-P hole is closed. v1.15 routes every `+d.recipe` field naming a source cell — tuple
`from`/`dim`/`to` endpoints and record subjects — through the P16 marked-carrier: an emitter-minted
int64 ref cell carrying the endpoint's position as `int64` content, marked
`d.mesh.relocation→"cell"`, with *the field cell's own `+d.recipe` record* as the ceremony's
inventory entry beside `+d.refs` (:1145-1149, :1055-1071, :2051-2055, P16 row :2907). The mechanics
check out end-to-end against code:

- **Relocation.** `isCellRelocationHandle` cells enqueue their `handleTarget` in promote's frontier
  (`arena_manifold.cpp:1759-1764`), so every tuple endpoint is pulled into the fold; OpHandle-kind
  marked cells promote to `real.at(target)` (`arena_manifold.cpp:1947-1951`), ephemeral endpoints
  included — verified `discover(edge.dim, false)` at `:1781` also folds each edge's dim cell.
- **Re-join.** At (d) the field reads `*handleTarget` — the endpoint's promoted position — one
  position keyspace for the old→new map; two tuples sharing an endpoint both read the same promoted
  position and resolve to the same copy cell (:1149-1151). In-session the field reads the live
  position; post-ceremony the `Int64(*handleTarget)` writeback is the identity write on the
  continuing arena, so the dual-read ("live position in-session, `*handleTarget` at (d)",
  :1148-1149, mirrored at :2276-2278) is exact.
- **No spurious records.** The field's own `+d.recipe` record is the inventory — the
  `+d.refs`-only pairing ("one ref-capture record per `+d.refs` ref-record") is undisturbed, and
  the mixed link/`int64` carrier is abolished: the record's `+d.values` is decorative,
  never load-bearing, replay resolving the content channel alone (:1151-1155). This is the
  normalization I asked for, achieved without dragging tuple fields into `closed`'s ref frontier.

**Residual (Finding 1, MINOR):** the uniform sweep is one subclass too broad — tuple `dim` fields.
A dim's durable identity is *name*, not position — the doc's own dim-referent arm exists precisely
because a session-minted dim folds as an orphan off the `d.dims` rank (frontier skips `d.dims`
edges, `arena_manifold.cpp:1766-1768`; `dimensionNamed` walks base `d.dims` + `arenaDims_`,
`manifold.cpp:781-815`, `arena_manifold.cpp:60-76`). A tuple `dim` field under the position rule
binds the *orphan*: `discover(edge.dim,false)` folds it, `*handleTarget` names its promoted
position, and the replayed `Link` lands on a dim `ensureDimension(name)` never finds — the same
silent split (e) just fixed for slots. Today this is unreachable — graph wiring rides genesis dims
(`initGenesis`, `vortex_core.cpp:50-63`, image band, folding verbatim), descriptor links are
regenerated not tuple-replayed, and `+d.entry`/`d.refFor`/`d.recipe` are prelude-data dims — so it
is a scope-clause gap against an unspecified emission shape, not a live misroute. (If `+d.entry`
head links are tuple-recorded *and* adapter-minted dims can land session-side, it upgrades.)
**Fix:** scope :1145-1147 — "every field naming a source cell" → "every field naming a source
*graph* cell; a tuple `dim` field names an image dim, or takes the `+d.name` dim-referent arm" —
one clause, same vocabulary.

## 2. Carried minor verification — program-side/dim writeback: **FIXED**

(e) now states the write per arm (:2254-2264): per ref-record `Int64(*handleTarget)` for
internal/external endpoints, and "a `+d.name`-bearing dim record instead writes `Int64` of the
`DimRef` `ensureDimension`'s name arm re-resolves (`arena_manifold.cpp:78-105`) — `Linked` binds a
`DimRef`, never a promoted position", the arm keyed on the record's `+d.name`, never the sideness
test. `ensureDimension`'s name arm verified (`arena_manifold.cpp:78-104`: `dimensionNamed` →
`arenaDims_` → mint+`d.dims`-link). The orphan mechanism is correctly named — a session-minted dim
reaches the fold only through a `handleTarget` enqueue, so `*handleTarget` binds it while
`ensureDimension` re-mints the canonical twin; for image dims the two arms agree (verbatim fold,
`real[x]=x` at `arena_manifold.cpp:1853-1855`), so keying on `+d.name` is the right unification.
`Linked` now binds a live, name-findable DimRef — the dim-split is closed.

(d) is likewise corrected (:2241-2248): the record supplies "the pairing (`+d.refFor`) and the
program-side mark (`+d.name`) — … and never the endpoint value, the `+d.name`-bearing dim record
excepted: there the record's `+d.name` *is* the endpoint source and the slot takes `Int64` of the
resolved `DimRef`, not the donor's promoted twin". The excepted arm is exactly what the r15 minor
required — the false universal is scoped. One leftover nit: "the program-side mark" is
pre-rename vocabulary (Finding 2).

## 3. Nit verification — alias scoping lead: **FIXED**

:1776-1778 now reads "the alias present only where the verb takes a *user-alias* name
(`learn_plan`, `learn_handler`; `learn_where`/`learn_body` take none and `learn_method`'s `name` is
never an alias)" — the accurate lead I asked for.

## 4. New findings (v1.15 deltas only)

### 1. MINOR — §6.1 :1145-1151 (echoed :1055-1057, :2051-2053, changelog :3707-3712): tuple `dim` fields swept under the position keyspace while dim identity is name-keyed

Described in §1 above — "every field naming a source cell" + "tuple `from`/`dim`/`to` endpoints
and record subjects alike" makes no name arm and no image-dim premise for `dim` fields, against
§6.1's own dim-referent arm and the (e) fix it just landed. Latent orphan-split; no emitted tuple
produces it today. One-clause scope fix as above.

### Nits

- :2243 + :3672 — "the program-side mark (`+d.name`)" survives the arm-(c) rename everywhere else;
  the mark is the *dim-referent* discriminator, and a session-minted dim is precisely not
  program-side (ephemeral/state-side). Read "the dim-referent mark (`+d.name`)".
- :1162 — the corrected `wingOf` cite `vortex_core.cpp:373` names `inputsOf`'s
  `return wingOf(opcode, POS)` line; the `linked(opcode, dims_.grab, side)` the sentence describes
  is `wingOf`'s own at `vortex_core.cpp:366` (the disposition's `:366-369→:373` was applied
  literally but lands on the caller).

## 5. Staging-discipline assessment — the endpoint model is closed *as emitted*

Every admitted endpoint class now has a durable path and a named inventory: internal/external slot
referents ride marked content → `*handleTarget` → map-or-verbatim at replay; dim referents ride
`+d.name` → `ensureDimension` on both the (d) replay and (e) rehydration arms; call/run bindings
ride `+d.refs` name-records with the role split; tuple endpoints ride marked field positions
inventoried by their own `+d.recipe` records; `cap$N` captures re-clone the same definition-site
cells via marked record subjects (:1192-1193 — the `+d.clone` append at `vortex_vm.cpp:816-825`
re-established per copy, direction state→program so R15-A's unreachability stands). Every refused
class is still named (frame-bound by class, session handles by type, unnamed program-side links by
arm, smuggled non-op-graph edges as a stated §11-door limitation), and `closed`'s frontier stays
enumerable through slot content + `masterOf` decode. The two r15 write-path residuals are both
resolved; what remains is a one-clause scope on the `dim` subclass of tuple endpoints (Finding 1)
and two wording nits. The mark-inventory machinery — marker per cell, record per marked cell —
holds uniformly across `+d.refs` and `+d.recipe`.

VERDICT: CONVERGED — 0 blocking, 0 major, 1 minor (tuple `dim` fields need the image-dim premise
or the `+d.name` arm stated — same orphan class as the fixed (e) bug, latent-only today), 2 nits
("program-side mark" leftover; `wingOf` cite lands on the caller at :373 not the `linked` call at
:366).
