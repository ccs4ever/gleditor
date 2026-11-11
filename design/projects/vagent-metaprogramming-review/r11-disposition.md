# Round-11 disposition — orchestrator rulings on v1.10 findings

0 blocking, 4 majors, ~14 minors. Three lanes converged (vortex, ergo, meta). All r10 mechanism
fixes verified end-to-end — the majors are specification gaps in v1.10's newest text
(`patch:<n>` minting, the verify arm), not reopened architecture.

## Majors — all adopted

**R11-A — `patch:<n>`'s ordinal is unpinned and recycles** (architect + codebase majors; meta,
vortex, zigzag minors — five lanes). The `d.patch` rank ordinal compacts on `unlearn`/replay-drop
(`:1584`,`:1997`), so freed ordinals re-mint onto live clauses → duplicate durable labels →
`plan_named`/descriptor/`unlearn` misbind — the ABA the doc itself names at :2034. Even tail pops
re-issue numbers that stale target descriptors mis-rebind. **Ruling:** `patch:<n>` draws from a
**durable monotone serial** — the `d.vars` serial `reify` already uses (:2048-2050) — never rank
position; ordinals are never recycled. Companion: at *replay* no patch is created, so the
replayed clause's label is transported verbatim on its record (zigzag), not re-derived.
Companion (vortex): state the `learn_*` dedupe consequence — a qualified alias unlinks the
named clause and installs `patch:<n>`, leaving the user name dead.

**R11-B — the verify arm's publish destination is still the runtime's private store**
(architect). "The adapter's own lineage store" (:1504-1507) literally names the image-only
*seeded* store — the received checkpoint's band lives in the runtime's rebuilt store
(`arena.base()->store()`, `manifold.hpp:486`). `promote`'s verbatim persistent-ref map
(`arena_manifold.cpp:1867-1868`) would write dangling positions undetectable by the `1..E_ops`
comparand. **Ruling:** under the verify arm the publish destination is the store holding the
*received* band — the runtime's base store, reachable via `base()->store()` — and a kept-in-step
adapter twin needs a segment-apply surface `adopt`/`historyFromSeal` don't provide (name it as
owed surface or drop the twin). The adapter's seeded store is only the spawn-side lineage
anchor.

**R11-C — no recipe record is ever minted to carry a ref cell's referent** (zigzag). §6.1
enumerates link tuples + literals + linked-capture records (:1033-1034,:1110-1112,:804-809) —
nothing carries a ref cell's referent; for program-side referents (`dimConstant` slots) the
`+d.values`→referent edge repeats the `d.code`-rank violation and hits `closed`'s refused
class. **Ruling:** state the ref-capture record shape explicitly; program-side dims resolve via
`ensureDimension` (name-arm), never a `+d.values` referent edge.

**R11-D — stale same-name claims under the unique mint** (writer + codebase majors;
ergo/meta/zigzag minors — the surviving text contradicts R10-A's mechanism). Learned clauses
carry unique `patch:<n>` `+d.name`s — same-named learned clauses can't exist; `unlearn` can pick
a specific twin in any order; `plan_named` zero-hits (the alias is invisible). **Ruling:**
correct the stale sentences — the surviving ambiguity is same-named *grafted* clauses only;
state the alias is write-only and name the rediscovery idiom (`clauses_of` + `patch:` prefix or
`plan_named("patch:<n>")`).

## Minors — all adopted

- **`bindPublishedLocalScroll` isn't on the read path** (vortex): `setContentSource` carries
  the dead-read stake; the bind's stake is scroll identity/`postLoad` (`store.cpp:4081-4089`,
  `manifold.cpp:1503`) — split the claims.
- **(b)'s recorded enumeration doesn't pin its position space** (vortex + codebase):
  `adoptOpRecords` permutes a non-linear op DAG — record `produces` MicroversionIds or state
  the parent-chained-emission invariant; the fold-vs-fold comparand can't catch permutation.
- **`bindPublishedLocalScroll(self)`'s `self` recovery** (architect): `historyFromSeal` returns
  no self — `store->addScroll(from)`'s idempotent re-add recovers it (`store.cpp:1388-1405`).
- **`unlearn` symbol-arm module-prefix composition unstated** (meta): arg resolves bare
  `Agent.member` but the recorded target is qualified `module:Agent.member`.
- **`d.contract` is op-anchored** (zigzag: `vortex_stdlib.cpp:603`,`:628`), not an agent-cell
  leg; the agent-leg list omits `d.patch`/`d.synapse`; the second walk has no consumer.
- **Citation nits** (writer): `:186`→`:188` (`checkpoint`'s first read is the `!checkpoint.store`
  guard); `store.cpp:123-124`→`:123` (`:124` is `chronofilade_`); "patch: convention"/
  `patch:<ordinal>` comment; hydrate/rehydrate stems.
- **Unapplied ergo nits now adopted**: `learn_method`-only qualified-success clause and
  qualified-verbatim-dedupe — take both; `patch:`-emission wording is cosmetic, take it.
- **`learn_method` replace-vs-`learn_plan` append asymmetry** — draw the structural contrast.

## Round status

v1.11 to follow. Trajectory: r9 1B+4M → r10 5M → r11 4M — the remaining majors are all
within two subsections (`patch:` naming, the verify arm) that arrived in the last two versions.
The machinery verified sound; its *specification details* are what keeps surfacing.
