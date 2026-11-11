# Round-15 disposition — orchestrator rulings on v1.14 findings

0 blocking, 2 mechanism-classes of majors (4 lanes), ~12 minors. Three lanes converged
(vortex, writer, architect); all r14 fixes verified. The two majors are again precision holes
in v1.14's newest text — one reachability direction, one endpoint keyspace.

## Majors — all adopted

**R15-A — `cap$N`/`masterOf`/`d.clone` is a `-d.clone` backlink, not a walkable edge**
(codebase + ergo majors; architect minor agrees). The binding cell's `+d.clone →` slot is the
edge; the slot's `masterOf` is an operand decode (`vortex_vm.cpp:816-825`,
`arena_manifold.cpp:1009-1012`); `d.clone` is absent from the `+d`-walk vocabulary
(:1506-1511) — no agent-`d.vars` cell can enter the `program()` walk through it.
**Ruling:** the legs classify state-side by **unreachability** — restate :2134-2143 plainly:
agent-`d.vars` binding cells enter the walk only where a *runtime-grown outbound* edge leads
(and a declared fragment's `cap$N` edge is image-sealed, not runtime-grown — architect's
adjacent nit), so under the sealed image they are simply unreached and classify state-side.
Drop the `masterOf`-as-edge phrasing; `masterOf` is the slot's decode, not a traversal.
Apply the same correction at :3605, :3619-3621 (changelog).

**R15-B — the recipe-tuple's mixed `+d.values`-link/`int64` endpoint encoding splits the
keyspace under `promote`** (zigzag + meta majors). A tuple `int64` endpoint field is an
unmarked dense-id — outside P16's `+d.refs` inventory — and folds verbatim
(`arena_manifold.cpp:1934-1936`) into a dead id, while a `+d.values` *link* folds to the
promoted position: shared endpoints' claimed-`-d.values` namings can't re-join at replay.
**Ruling:** uniform content-keyspace — recipe-tuple endpoint fields carry **int64 ref
positions marked through the recipe-side ref-capture inventory** (the same P16 `+d.refs`
machinery, or opaque recipe-local keys that replay resolves through the old→new map); the
`+d.values` edge is decorative/conditional and never load-bearing. State which and restate
:1137-1142.

## Minors — all adopted

- **(e) program-side writeback arm** (architect + meta, convergent — promote to full ruling):
  `ensureDimension` re-resolution must name the slot write `Int64(resolved DimRef)` — `Linked`
  needs a DimRef, not the promoted dim-twin position; session-minted dims fold off the `d.dims`
  rank (`arena_manifold.cpp:1764-1768`) so `*handleTarget` binds an orphan. Also "(d)'s record
  supplies never the endpoint value" is literally false where `+d.name` *is* the endpoint —
  scope it.
- **Arm (c) "program-side dim referent" over-narrows** (vortex): session-minted dims are
  ephemeral — drop the qualifier to "a dim referent".
- **"The paired record's `+d.name`" pairing unstated** (vortex): name `+d.refFor`/`+d.recipe`
  vs a `+d.refs` record's `+d.ref` pairing.
- **Conditional `+d.values` misclassifies internal-with-claimed** (vortex): internal-vs-
  external is map membership (:1159); fields-only reading must defer to it.
- **`set_weight`/`weight_of` `+d.name`-less handler refusal** (architect): `weight_of`'s `Opt`
  disengage is named; the `()`-returning write arm's refusal isn't — state it.
- **"Capture slots carry positions"** (writer): `cap$N` slots carry no int64 (`asInt64` fails →
  `masterOf`); positions are the ref-box arm only — :1176-1178.
- **Changelog retro-edit confession incomplete** (writer): the program-side regroup
  (:3580-3585) and (v)-narration backfill (:3600-3603) go unconfessed — narrate.
- **(e) coverage `+d.ref`→`+d.refFor`** (codebase nit) + **alias scoping lead** (meta nit) +
  **`wingOf` cite** `:366-369`→`:373` + `set_weight` row gloss + serial sketch comment — all.

## Round status

v1.15 to follow. Trajectory: r13 1M → r14 1M → r15 2M (two classes). Still mechanism-bound —
each fix verified, the next constraint surfaces. The doc is ~3,645 lines; the review depth is
still paying for itself.
