# Convergence record — vagent-metaprogramming design review

**Status: CONVERGED at v1.18** (design-draft scope), declared after the round-18
freeze round and the targeted v1.18 close-out. The document remains explicitly a
design draft — nothing in it is implemented.

## What the freeze round proved

The bounded r18 round (verify r17 rulings landed *correctly*; hunt majors only in
pre-existing mechanisms) returned:

- 3 lanes converged outright (vortex, meta, writer — writer at 0/0/0).
- 4 narrow majors, all in-scope and all fixed in v1.18:
  - The R17-B dim-filing enumeration was verified *mechanized wrong* (zigzag +
    codebase independently): the `d.dims`-rank walk is circular
    (`ArenaManifold::dimensions()` never reads the arena-local rank;
    `ensureDimension` rank-appends only under a base-supplied `home`), and the
    cited `vortex_core.cpp:50-69` range mints the nineteen system dims, not V10's
    thirteen `std:agent` dims. Unfiled V10 dims would orphan every
    `+d.belief`/`+d.intention` rank at first checkpoint. v1.18 re-enumerates by
    the arena **name table** plus the explicit V10 thirteen with
    sweep-before-filing ordering.
  - Declared-image positional int64 refs had no stated finalization (architect):
    v1.18 states they ride the positional-preservation invariant the canonical
    adopted-band comparand verifies — no writeback exists or is needed.
  - §8.1's "synapse junction's own label" endpoint member was contradicted by
    `sprout_synapse` minting no label (ergo, pre-existing since v1.2): dropped.
  - The `+d.step` positional schema carrier lacked a decode rule (three-lane
    minor cluster): v1.18 states the `dim`-member anchor (`from` predecessor,
    `dir`/`to` successors).

## Round trajectory

| round | blocking | majors | converged lanes | dominant theme |
| ----- | -------- | ------ | --------------- | -------------- |
| r1–r8 | ~12 | ~60 | → 6/7 by r8 | architecture: lineage, genesis, E_ops, P15/P16 |
| r9    | 1 | 4 | 0 | E_ops/E_cell landmark conflation |
| r10–r15 | 0 | 2–6/round | up to 6/7 | durability layer: patch serials, dedupe, recipe records |
| r16–r17 | 0 | 4–5 | 2–3 | ceremony reachability, dim orphaning, schema carrier |
| r18 (freeze) | 0 | 4 narrow | 3 (+4 one-major) | enumeration correctness, stated finalization |

## Verified-core mechanisms (no open majors)

- Base-lineage store model: adapter-owned `Store`, spawn-seed injection before
  `WorkerContext` freeze, `getOrCreate` rebuilt fold as injected base.
- Genesis: `E_ops`/cell-ref distinction, mandatory canonical adopted-band
  comparand (scroll-identity-aware), recorded instruction enumeration for
  opcode registration, two-root reachability, `d.clone`/`masterOf` backlink
  decoding.
- P15 kind immutability via `setOpcodeKind`-registered-cell write refusal.
- P16 relocation ceremony: single `+d.refs` inventory covering marked slots and
  `+d.recipe` fields, `d.refFor`-based correlation (no `+d.grab` occupancy
  collision), positional `+d.step` schema carrier with `dim`-anchor decode,
  `+d.name` reserved to the dim discriminator.
- Dim filing: name-table enumeration + explicit vocabulary (19 system dims,
  13 V10 `std:agent` dims, `d.store-refs`, `dimConstant` mints), file-before-pin
  and file-before-first-resolution ordering.
- Durable mutation: monotone `patch:<n>` serials from agent-cell `d.vars`,
  verb-replayed dedupe, no-ABA after `unlearn`, separate user aliases.
- Staging taxonomy: `code"…"` quasiquote → `Code<T>` → `splice`/`static` →
  `learn_*`/`graft_*` → checkpoint → genesis replay; staging model verified
  mechanism-closed.

## Known open items (parked — wording/nit level, not defects)

- Recipe literal/capture record member *shapes* remain unstated (only the
  ref-capture record is specified); an implementer defines them.
- `d.op-label`/or_parallel machinery dims are mid-session mints — covered by the
  honest-residual clause, not filed.
- `set_weight` dispatch non-totality for `+d.name`-less targets (state/op cells)
  — implementation may add a refuse arm.
- §8.3 `if [w]` guard is dead for junctions; `graft_*`-named clauses absent from
  a named-clause enumeration.
- Changelog lockstep restates some pins up to 5× (house style).
- :1200-1203 and glossary `closed fragment` row are dense.

## Process verdict

Unrestricted critique was finding ~1 new major per ~30 lines of new text — the
spec was growing faster than verification. The freeze round bounded verification
to ruling-correctness + pre-existing majors and found one genuinely mismechanized
ruling (R17-B), which v1.18 closed. Remaining risk is concentrated in
unspecified record shapes — the right surface to expose them is implementation,
not another specification round. **Further critique rounds are parked.**
