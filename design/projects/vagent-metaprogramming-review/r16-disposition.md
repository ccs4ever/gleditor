# Round-16 disposition — orchestrator rulings on v1.15 findings

0 blocking, 2 mechanism-class majors (5 lanes), ~12 minors. Two lanes converged (ergo, meta).
R15-B's marking machinery verified correct; the majors are where its inventory and keyspace
end.

## Majors — all adopted

**R16-A — the `+d.recipe` marked fields are unreachable by the `+d.refs`-bounded ceremony**
(writer + zigzag majors; vortex minor agrees). Step 3's re-kind walk enumerates `+d.refs` only
(:2084-2086); `+d.recipe` field cells stay `Int64`-kind, fold verbatim
(`arena_manifold.cpp:1934-1936`) into dead ids — the R15-B defect relocated one level.
**Ruling:** the marking inventory is **one rank** — every marked int64 field cell (slot or
`+d.recipe` field) carries its `+d.ref` record on the head's `+d.refs` rank; the `+d.recipe`
record remains the *field's* schema record (name → tuple position), not a separate marking
rank. The re-kind/restore/(e) passes then stay `+d.refs`-bounded as stated, and (e) covers
both arms — state (e)'s `+d.recipe`-field writeback explicitly (vortex's adjacent minor).

**R16-B — the `dim` endpoint was swept into position-keyspace; all session-minted dims fold
to `dimensionNamed`-invisible orphans** (codebase + vortex majors; architect major — the
deepest). `promote` can never emit `d.dims` links (`:1766` skip + `real.find` gate
`:2030-2033`), so every session-minted dim (`d.patch`, `d.code`, `d.refs`, `d.recipe`,
`d.refFor`, `d.entry`, `d.weight`, `d.synapse`) folds to an orphan twin; (d)/(e) enumerate
empty inventories and the acceptance fixture dies at genesis-2. **Ruling:** dim identity is
name-keyed, never position-keyed — a `dim` endpoint field carries the dim **name** (`+d.name`
on its recipe record, `ensureDimension` re-resolution), exactly like the `dimConstant` arm;
and the minted-dim set must be **filed on the persistent `d.dims` rank at image-seal**
(`DimensionRegistry::getOrCreate`/`makeDimension`, the `vql/compiler.cpp:1128-1141`
precedent) so `ensureDimension`'s find-or-mint lands on the canonical cell, not the orphan —
name this as part of the spawn-seed ceremony in §10.1.

## Minors — all adopted

- **`masterOf`/`cloneMaster` "never a traversal" contradicts `rankEnd` NEG-ward** (ergo +
  zigzag): it *is* a `-d.clone` backlink traversal — the distinction is "on a non-vocabulary
  dim", restate; `closed`'s frontier relies on it.
- **`+d.recipe` record↔field pairing unnamed** (zigzag): name the edge/positional pairing
  (its `+d.ref`-analog).
- **`cap$N re-clone` names an op outside `New`/`Link`/`Value`** (zigzag): the replay palette
  is fixed — restate what the capture slot actually emits.
- **`set_weight` clause scoping** (ergo): junctions are `+d.name`-less targets of §8.3 —
  scope the refusal to clauses + name the dispatch rule.
- **"Program-side mark (`+d.name`)"** (writer + architect + meta): after arm (c) dropped
  sideness it reads "dim mark" — :2243, :3672, plus stale qualifiers in 1.12/1.13/1.15
  entries.
- **"Re-binds rather than re-mints"** (codebase + architect): twins re-mint, not re-bind —
  :1537-1538.
- **`Linked` "never a promoted position"** (architect): false for image-resident dims —
  mean "never the twin `*handleTarget`"; note the per-checkpoint twin accretion bound.
- **`+d.clone →` edge direction exact only for first capture** (vortex): chained captures
  run clone→clone (program→program for two declared captures) — scope the claim.
- **`wingOf` cite `:373`→`:366`** (meta + codebase): the `linked(opcode, dims_.grab, POS)`
  call is at :366.
- **Unannotated `+d.values`-carrier narrations in 1.9/1.10/1.11 entries** (writer): annotate
  as superseded per convention.

## Round status

v1.16 to follow. The `d.dims`-orphan find is the deepest yet — it reaches under the whole
minted-dim vocabulary, not just this round's delta. The fix (file at image-seal via the
registry) is architecturally simple and closes a hole every prior round missed.
