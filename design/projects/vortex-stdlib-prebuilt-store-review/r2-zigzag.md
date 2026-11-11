# Round 2 critique: Xanadu / ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 1 BLOCKING, 3 MAJOR, 5 MINOR.

## BLOCKING

### 1. (P26) The shared `"cell"` marker violates strict 2-degree — `link()` severs the previous mark, so only the last-marked cell stays marked

`link(slot_i, relocDim, POS, marker)` computes `displacedIt` = the previously marked slot
then `setOneSide(prevSlot, relocDim, POS, noCell)` (`manifold.cpp:672–688`): **the previous
slot's forward mark is destroyed** — the eviction isn't confined to the marker's backlink.
After N marks exactly one cell carries the edge: the harvest sweep misses every earlier
referent (re-creating §2.3.2) and every earlier OpHandle wing slot trips promote's
`isCellRelocationHandle && seen.contains(target)` gate into `return std::nullopt`
(`arena_manifold.cpp:1812–1824`) — the export refuses outright. The codebase's convention
is **per-ref markers**: `refCell` mints fresh `arena.makeCell("cell")`
(`vagent/records.hpp:63–72`), `mesh_evaluator.cpp:755`, `executor.cpp:676–678`. The doc's
fallback trigger never fires — the structure cannot fan in.

**Fix:** per-ref markers as the rule — "`slot +d.mesh.relocation → <fresh "cell">`", one
marker cell per marked cell — for wing slots, ref scalars, manifest members, sig members.
~2 ops/slot stands.

## MAJOR

### 1. (§5.2.3, §6.4) Membership-append anchor unspecified — `insertIntoRank(anchor)` is a head-insert that silently shadows base members

`insertIntoRank(cell, anchor, dim, POS)` inserts *immediately posward of the anchor*
(`manifold.cpp:739–752`) — anchored at the module cell it's a **head-insert**: local
same-text symbols resolve first under `resolve()` first-match → silent override, the
repoint semantics §6.2 deferred. Anchored at `rankTail` it's a true tail append — same-text
member shadowed by base — but §6.4 never says either. Also "`link()`" over-broad:
`link(rankTail(anchor,dim), dim, POS, cell)` IS the builders' append and severs nothing —
what must be banned is `link()` onto an *occupied* mid-rank slot.

**Fix:** pin "append = `link(rankTail(anchor, dim), dim, POS, member)`, the
`exportSymbol`/`getOrCreateModule` pattern verbatim" and state the collision rule:
same-text members land posward of base and are shadowed under first-match (add, never
replace) — or refused; silence is not honest.

### 2. (P28, P29, §3.1) The `+d.owner` scope scalar is an unhandled Int64-ref class — the marked-Int64 sweep is not complete

`varsRecord` mints `record +d.owner → makeScalarCell(Int64(owner))`
(`vagent/records.hpp:113–118`) — an Int64 naming a cell. If the generator emits records
via `varsRecord`, each carries an unmarked Int64 ref: verbatim export → verbatim graft →
`owns()` compares across spaces → mis-scopes silently (or, worse, aliases a live cell).
If records emit unscoped, they "match any owner" (`records.hpp:103–108`) — acceptable for
singleton keys, but the choice must be stated.

**Fix:** manifest and signature records minted unscoped (no `+d.owner` arm) — preferred,
no new ref class — and say so; or mark the scope cell under P26's own letter.

### 3. (§4/§5.2) The single-owner invariant sentence is absent — and the graft depends on it twice over

`sym +d.values→entry`, `record +d.values→head`, `+d.step` chains, `±d.contract→first`,
`param +d.spin→head`, `sym +d.params→sigCell` all assume single-owned targets; an aliased
target silently evicts the other owner's edge at mint AND at verbatim graft replay
(`manifold.cpp:672–688`).

**Fix:** add to §4 and §5.2: "every `+d.values`, `+d.params`, `±d.contract`, `+d.spin`,
`+d.step`, `+d.clause` edge targets a cell owned by exactly that edge — an aliased target
is a silent eviction at mint and at graft-replay alike; the generator asserts it."

## MINOR

1. `zzResolveDim` cite stale AND the flagged bug is fixed — now walks the correct rank
   (`vortex_stdlib.cpp:5959–5974`). Related drift: `exportStandardLibraryToStore`
   `:5674–5691`, `importModuleFromStore` `:5694+`, `exportSymbol(fail)` `:4009`,
   `sql_lens.cpp` under `zigzag/`.
2. "The image's intended subgraph" is load-bearing but never operationally defined —
   say: intended = all image cells minus name-mapped dim cells and `home` (whole-space =
   everything).
3. Edge replay direction/dedup unspecified — replay each edge once pos-side, or state
   neg-side replay idempotent under the class rules.
4. Catch-all missing: an edge on a name-mapped cell on a *non-enumerated* dim is neither
   internal, listed-membership, nor map-miss — state skipped-or-refused by class
   (name-merge `"d."`-texted dims via `findOrMintDimension`).
5. A grafted local image's `"stdlib.image"` record leaves two same-text records on
   `home +d.vars` — harmless today (validation reads pre-graft); worth a clause in §6.4.

## Topological audits — resolved

- Membership anchors complete (home `d.stdlib`/`d.vars`, merged `+d.vars`, `d.dims`
  never replayed; NEW local module edge → membership by endpoint mapping).
- Ops manifest `+d.step` chain 2-degree clean.
- varsRecord sig shape conformant modulo `+d.owner`.
- No foreign serialization smuggled — marked-Int64 is the codebase's own convention;
  `imageEpoch` digest is text data, not structure encoding.
- Fold fiat verified: `home_`/`dimsDim_` = first two births (`manifold.cpp:447–454`);
  `promote` seeds roots in order (`arena_manifold.cpp:1745–1759`).
- Purist end-state honestly priced — the doc does not claim transclusion it cannot
  deliver.

VERDICT: NOT CONVERGED
