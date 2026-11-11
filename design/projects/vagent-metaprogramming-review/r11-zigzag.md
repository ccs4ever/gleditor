# R11 — ZigZag topology/promotion critique (v1.10)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the v1.10 deltas in my lane: the
capture-endpoint move onto the recipe tuple's `+d.values` via `+d.ref` slot identification
(:1966-1979, schema :1023-1038, changelog :3147-3148), the two-root walk vocabulary
(`+d.code`/`d.contract`/`d.belief`/`d.source` agent-cell legs, :1389-1405, :1899-1912), the
single-clause ingress restatement (:1908-1911), the `historyFromSeal` hydration pair
(`setContentSource` + `bindPublishedLocalScroll`, :603-610, :1411-1420, §10.1 comment
:2493-2500), the fresh-mint-vs-adopt `documentId` arms (:1425-1435), the `patch:<n>`
verb-mint + alias (:136, :1127-1142, :1587-1599, :1614-1623, §9.3 :2383-2400), the (b)
recorded-instruction-enumeration scope (:1921-1944), the order-vs-order comparand
(:1879-1890, :1467-1472), and the minor batch (`label_of` `cells"$a/+d.code"` :1075,
`code_named` head-labels-only :1074, `WorkerContext` real call shape :1461-1464,
`opcodeMap_` de-froze :1455-1457, emission-fixed `run` slot :1688-1698, `unlearn`
LIFO-by-name :1138-1142, `resume`'s transported store :1440-1445, cite nits).

Verified against `arena_manifold.cpp` (`dimensionNamed` reader/store_ fallback :55-66,
`ensureDimension` :77-105, `attach` when-unset :274-283), `arena_manifold.hpp:264-267`
(`setDefaultSpanReader`→`defaultReader_`), `:751` (`const Manifold *base_` raw pointer),
`manifold.cpp:824-828` (no-reader `dimensionNamed` → `*store_` as `SpanReader`),
`manifold.cpp:592` (`advance` binds `store_`), `store.cpp:509-511`
(`rebuildManifoldFromIndex` → `folded.setStore(this)`), `store.cpp:121-125` (`Store()`
mints its own `UserPermascroll`), `store.cpp:1418-1432`
(`bindPublishedLocalScroll`/`setContentSource`→`retainedContent_.setFallback`),
`publication.cpp:1094-1103` (`historyFromSeal` mints `Store()` :1097, `addScroll(from)`
:1098, `applyOpsSegment` call :1100), `publication.cpp:1030-1089` (`applyOpsSegment`
file-local, `\x7fXSO\x01` gate :1033, scroll-table remap :1046-1086),
`publication.cpp:1129-1131` (`restorePublication`'s hydration pair),
`vortex_core.cpp:14`,`:18`,`:29-70`,`:349-351` (`initGenesis` in ctor; `baseDimensionNamed`
→ `arena_.base()->dimensionNamed(name)` — the **Manifold** overload, bypassing the arena's
`defaultReader_`), `worker_context.hpp:52-55` (two-arg ctor installs reader before `core`),
`mesh_actor.cpp:182`,`:186-189`,`:261`,`:268`,`:270-273` (`ReceivedValue checkpoint`
transport, consumed for `userPermascrollPtr()` only, `sourceLease` pinned :268, base-only
ctor + post-construction `setDefaultSpanReader`), `mesh_value.hpp:100`
(`checkpoint.store` is `unique_ptr`), `compiler.cpp:768-772` (`dimConstant` =
`literal(int64(findOrMintDimension(name)))`), `vortex_stdlib.cpp:603`,`:627-628`
(`-d.contract`/`+d.contract` read off `*curOp` — op-anchored),
`vagent-agent-language.md:469-480` (the agent cell's full rank list).

## Verdicts on the r10 items

### R10-D — capture endpoint on the recipe tuple: **direction correct, carrier still unminted, and illegal for program-side referents — residual MAJOR (Finding 1)**

The rewording (:1966-1979) now matches the ruling: `+d.ref` identifies *which* cell's
referent is captured, the endpoint rides the recipe record's `+d.values`, internal
referents rebind through the old→new reify map, `*handleTarget`-equality scoped to the
external arm, and the `+d.refs` schema's `+d.ref`-never-`+d.values` occupancy rule
(:1024-1026) is no longer contradicted — the edge moved to a different rank. The schema
contradiction and the name-record overreach from my r10 Finding 1 are gone. What remains is
the *carrier*: §6.1 still enumerates the recipe as "link tuples + literals + capture
records" (:1033-1034, :1110-1112) where `+d.values` is stated only on *link-tuple* records
and captures are *linked* captures by design (:804-809) — no record type ever pairs an
int64 ref cell with its referent. And for the one marked class whose referent is
program-side — `dimConstant` slots — the asserted edge is the exact violation the section
exists to prevent. MAJOR, Finding 1.

### Two-root vocabulary / `>E_ops` legs: **resolved in substance** — residual nits, Finding 2

The `>E_ops` enumeration is now honest: `program()` and `agent_cell()` are distinct roots
never `+d`-reached (:1401-1405, matching the V12 discipline that state never links into the
skeleton), agent-side legs need a second walk or classify nothing, and the walk reaches
what it can actually reach (installed copies under `d.clause` attach anchors — reached
through `+d.grab`, which is in the vocab — and session `d.vars` members, :1902-1905).
Ingress is restated as classification with both arms in one clause (:1908-1911: "a
persistent ref `> E_ops` or any ephemeral ref is state-side… an image twin minted above
`E_ops` is admitted and reads as state") — my r10 Findings 2 and 3 are resolved. Two small
defects remain, Finding 2.

### R10-C — `setContentSource`/`bindPublishedLocalScroll`: **verified end-to-end; the span source does reach `initGenesis`'s dim-name resolution**

The full chain resolves in the code: `historyFromSeal` mints `Store()` internally
(`publication.cpp:1097`) — bare fold, no span source installed — then the stated hydration
is verbatim `restorePublication`'s pair (`setContentSource`+`bindPublishedLocalScroll`,
`publication.cpp:1129-1131`; `setContentSource` wires `retainedContent_.setFallback(source)`
and `resolver.setSource`, `store.cpp:1425-1432`). `initGenesis`'s dim-name resolution runs
`baseDimensionNamed` (`vortex_core.cpp:349-351`) → `arena_.base()->dimensionNamed(name)` —
the **Manifold** no-reader overload, which resolves `*store_` as the `SpanReader`
(`manifold.cpp:824-828`) → `textOf(dim, *store_)` → `reader.read(span)` → the seeded
store's scrolls plus the retained-content fallback the hydration installed. The folded
manifold's `store_` back-pointer is bound at `rebuildManifoldFromIndex`
(`store.cpp:509-511`). So the answer to the check is **yes** — with one precision worth
recording: the reader `initGenesis` actually uses is the folded manifold's `store_`
back-pointer, **not** the arena's `defaultReader_`. `baseDimensionNamed` bypasses the arena
entirely; `defaultReader_` feeds only the arena's *own* later `dimensionNamed` calls (the
adapter's `ensureDimension` minting of `d.code`/`d.patch`/…, `arena_manifold.cpp:55-66` —
where `reader != nullptr` → `base_->dimensionNamed(name, *reader)`). Passing the hydrated
store as the `WorkerContext` reader covers both paths, so the doc's "the store as reader"
holds — the mechanism is just one hop deeper than the sentence implies. Residual nit:
`bindPublishedLocalScroll(self)` cannot spell `self` — Finding 5.

### Other verifications

- R10-E comparand (:1879-1890, :1467-1472): "the adapter decodes its adopted lineage
  store's own `1..E_ops` band — the same op records `adoptOpRecords` folded it into" —
  `adoptOpRecords`' produces-ordered Kahn fold is deterministic, so decoding both stores'
  bands post-adoption gives a canonical order-vs-order comparison. Sound.
- R10-B (:1921-1944): registration keys on the recorded instruction enumeration rather
  than a value scan — the cited hazard (`dimConstant` cells holding small `int64` aliasing
  op kinds under an `asInt64 ≤ Linked` sweep) is real given `dimConstant` =
  `literal(int64(ref))` (`compiler.cpp:768-772`). The enumeration rides the `programRef`
  bind record `E_ops` rides; positions index the fold directly (position-stable).
- `patch:<n>` mint (R10-A): consistent — clause `+d.name` = verb-minted `patch:<n>`, user
  `name` = alias on the record (:136, :1129-1132, :1589-1593, :2396-2399); dedupe-by-alias
  correctly predicts learned clauses never re-hit (:1138-1140); target-descriptor arm
  stores the verbatim minted label (:1617-1622).
- `WorkerContext` call shape: `resume` passes `base` alone then calls
  `arena.setDefaultSpanReader` (`mesh_actor.cpp:270-273`); the two-arg ctor installs the
  reader *before* `core` (member-init order, `worker_context.hpp:52-55`) — spawn supplying
  both at construction is a valid shape, and the reader is on the arena before
  `initGenesis` runs.
- `resume` store transport: `ReceivedValue checkpoint` param :182, consumed in the validity
  check and for `userPermascrollPtr()` at :261, dropped in favour of `receiveValue`'s
  verified product (`actor->checkpoint` ← `*verified`) — "trusting or comparing it, not
  new transport" is accurate; `checkpoint.store` is `unique_ptr` (`mesh_value.hpp:100`).
- `opcodeMap_` de-froze (:1455-1457): accurate — `initGenesis` adopts `home_`/dims only;
  `opcodeMap_` is populated by `VortexStdLib`'s `mintOpcode`s in `stdlib(core, vm)` during
  `WorkerContext` construction — an image-free map, not an empty one.

## New findings (v1.10 deltas only)

### 1. MAJOR — the recipe-side endpoint record is never minted, and for program-side referents the asserted `+d.values` edge is the verbatim V12/`closed` violation

§6.6(e) now says "the referent endpoint itself rides the *recipe* record (the recipe
tuple's `+d.values`, §6.1 — a cell, never a census integer)" (:1968-1971), but no recipe
record in the stated schema carries an int64 ref cell's referent. §6.1's enumeration is
exhaustive: "link tuples + literals + capture records" (:1033-1034, :1110-1112) —

- **link tuples** record existing *edges*; a ref cell has no edge to its referent (that is
  the whole P16 problem), so no tuple's `+d.values` names it;
- **literal records** carry exactly the emission-time census int the derivation exists to
  avoid;
- **capture records** record *linked* captures — captured cells are deliberately
  link-referenced because "the link is the *checkable* form" (:804-809) — and a ref cell's
  referent is not a linked capture.

So for every ref-record'd ref cell the `+d.ref` slot identifies the slot but nothing pairs
it with an endpoint — the derivation reads a schema position that is never written. (For
internal referents the edge is *also* load-bearing: the recorded literal is an
emission-time dense id, meaningless in the fold — without the edge there is no way to find
the donor referent to feed the old→new map.)

Worse, where the referent is *program-side* — `dimConstant` slots, an explicitly marked
class (:1806-1808; `compiler.cpp:768-772` mints `literal(int64(findOrMintDimension(name)))`)
— minting `record +d.values → dimCell` is a link from a `+d.recipe`-hung record into a
program-skeleton cell:

- on a `d.code`-registered head it is verbatim §6.6's own invariant violation — "no cell on
  the agent's `d.code` rank may carry an edge in either direction into a program-skeleton
  cell" (:1374-1376) — the edge is emitted from a held source at `promote`, so it persists;
- at emission/`learn`-accept it is `closed`'s refused class — a program-side link with no
  name-record (:1648-1656). Recipe records sit on `+d.recipe`, a head structural dim inside
  the enumerable frontier (:1672-1674), and the `+d.values` edge is itself what makes the
  dim cell `program()`-reachable; even for a session-minted (ephemeral) dim the endpoint is
  neither state-side (dims hang on `home`'s `d.dims` chain and no path agent→`home` exists,
  :1542-1547) nor a name-record, so the positive `closed` condition fails either way —
  every `cells"…"`-bearing fragment, the canonical learnable class, becomes unlearnable.

An implementer either mints the violating edge or leaves `dimConstant`s (the commonest
marked ref class) with nothing to derive. Fix, mechanical: (a) enumerate the ref-capture
record in §6.1's recipe schema — a record pairing the slot with its referent, correlated
to the `+d.refs` record (a shared `+d.ref`→slot edge or positional key); (b) give
*program-side* referents a non-link encoding — record the dim **name** and re-resolve via
`ensureDimension`/`dimensionNamed` (name-stable dims are precisely what find-or-mint exists
for, :1397-1400, `arena_manifold.cpp:55-105`) — i.e., a `d.dims`-role name-arm in the
ref-record schema, since dims are position-stable inside `1..E_ops` and never anonymous;
(c) state whether recipe-record `+d.values` edges are inside or exempt from `closed`'s
enumerable frontier.

### 2. MINOR — `d.contract` is op-anchored, not an agent-cell leg; the second walk's leg list is incomplete and has no consumer

":1401-1403 `+d.code`, `d.contract`, `d.belief`, `d.source` — legs hung on the agent cell."
`d.contract` anchors on **op nodes** — `-d.contract`/`+d.contract` precondition and
postcondition chains read off `*curOp` (`vortex_stdlib.cpp:603`,`:627-628`; runtime doc
§3.3): program-side cells, already enumerated by the `program()` walk since the dim stays
in the main vocabulary (:1391). The agent-leg list also omits this document's own agent
ranks — `d.patch`, `d.synapse`, `d.weight` (:1002-1003, :1597) — and the spec's other ten
(`d.behavior`, `d.skill`, `d.space`, `d.holon`, `d.intention`, `d.outbox`, `d.pending`,
`d.member`, `d.capacity`, `d.belief`, `d.source`, `vagent-agent-language.md:469-480`). It
harms nothing positionally (agent-reachable cells classify state-side regardless), but the
"enumerated only by a second walk" phrasing implies a consumer the sideness test never
names, and "whose cells all classify state-side as `>E_ops`" covers neither the ephemeral
arm nor the doc's own program-side-`agent_cell()` contingency (:1529-1530). Fix: drop or
correct the `d.contract` attribution, complete or scope the rank list to what the test
consumes, and state what the second walk is *for* — `closed`-frontier enumeration, or
nothing.

### 3. MINOR — `unlearn`'s same-name limit describes a case the unique minted labels removed

":1138-1142 among same-named learned clauses it always pops the *latest* patch recording
that name — the clause-cell argument cannot pick an earlier twin out of order." Under the
verb mint, each learned clause's `+d.name` is a unique `patch:<n>` (:1589-1591) and
learned-clause targets record that verbatim label (:1617-1618) — two same-*alias* handlers
have *distinct* labels, so `unlearn(twin-cell)` resolves a unique name and **can** pick the
earlier twin out of order. The limit survives only as the re-point-chain LIFO (later
patches targeting the same `patch:<n>` undo youngest-first — correctly) and for
`learn_method` redefinitions sharing a qualified target name. Fix: restate the limit
against those two cases, not "same-named learned clauses".

### 4. MINOR — `patch:<n>`'s ordinal has no source at replay

"`<n>` = the `d.patch` ordinal the call creates — only the verb can mint it" (:1130-1132,
:1589-1591). But replay mints no patch cells (:1947-1948), so at genesis there is no
call-created ordinal: the replayed clause's `+d.name` must come from the replayed patch's
*own recorded* ordinal (or verbatim from the descriptor's recorded target name,
:1617-1618). If the mint re-derives `<n>` any other way — a fresh counter, a skipped label —
the clause mislabels and later patches targeting `patch:<k>` drop as unresolved at genesis
(:1995-1997). One clause naming the replay-time source fixes it.

### 5. MINOR — `bindPublishedLocalScroll(self)` cannot spell `self`

The hydration pair names `bindPublishedLocalScroll(self)` (:1416-1418, :603-606) —
`restorePublication`'s `self` is its own `addScroll` return (`publication.cpp:1130-1131`),
but in the `historyFromSeal` arm the `addScroll(from)` return stays inside the call
(`publication.cpp:1098-1100`) and only the `Store` comes back. The adapter must re-derive
the receiving `ScrollId` (the first added scroll) — or the signature must return it — to
bind it. One clause.

## Verified accurate (v1.10)

`historyFromSeal` mints `Store()` internally, no id/permascroll parameter
(`publication.cpp:1097`, decl `publication.hpp:606`); `Store()` mints a fresh
`UserPermascroll` (`store.cpp:121-125`); `applyOpsSegment` is file-local inside an anonymous
namespace ending :1090; `\x7fXSO\x01` gate :1033; scroll remap :1046-1086;
`restorePublication`'s pair :1129-1131; `Store(userPermascroll, storeId)` precedent
:1127-1128; `setContentSource`→`retainedContent_.setFallback`+`resolver.setSource`
(`store.cpp:1425-1432`); `bindPublishedLocalScroll` id check (`store.cpp:1418-1423`);
`Manifold::dimensionNamed` no-reader → `*store_` (`manifold.cpp:824-828`);
`rebuildManifoldFromIndex` `folded.setStore(this)` (`store.cpp:509-511`);
`Manifold::advance` `store_` bind (`manifold.cpp:592`); `baseDimensionNamed` →
`base()->dimensionNamed(name)` (`vortex_core.cpp:349-351`); `initGenesis` in ctor,
base home/dims adoption (`vortex_core.cpp:14`,`:29-70`); arena `dimensionNamed`
reader→store_→bare fallback (`arena_manifold.cpp:55-66`); `ensureDimension` :77-105;
`attach` installs `base_`/`store_` only when unset (`arena_manifold.cpp:274-283`); `base_`
raw `const Manifold*` (`arena_manifold.hpp:751`); `WorkerContext` two-arg ctor
(`worker_context.hpp:52-55`); `resume` base-only ctor + post-construction
`setDefaultSpanReader` (`mesh_actor.cpp:270-273`), `sourceLease` pin :268, transported
store :182,`:186-189`,`:261`; `checkpoint.store` `unique_ptr` (`mesh_value.hpp:100`);
`dimConstant` = `literal(int64(findOrMintDimension(name)))` (`compiler.cpp:768-772`);
`-d.contract`/`+d.contract` op-anchored (`vortex_stdlib.cpp:603`,`:627-628`); agent-cell
rank list (`vagent-agent-language.md:469-480`,`:126`); `attach(Space)` cannot re-run
genesis (`vortex_core.hpp:282`,`:287` private).

VERDICT: NOT CONVERGED
