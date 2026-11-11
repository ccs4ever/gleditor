# r13-codebase: Round-13 codebase verification of `design/vagent-metaprogramming.md` v1.12

**Orchestrator summary (≤200 words):** All four r12 rulings verified against the tree. R12-C is
fully adopted — the twin segment-apply surface is registered twice in §10.2 (:2787-2791 adapter
surface (iii), :2805-2810 post-register paragraph) and §6.6's "named under §10.2" (:1594) now
resolves. R12-B's mechanism is sound: the `alias` field *is* sufficient as the recorded
superseded target because dedupe is verbatim `alias == +d.name` and rank order at replay is
deterministic (grafts append at `rankTail`, never precede declared clauses; learned `patch:<m>`
labels are unreachable by alias since `patch:` is a refused `name`). `exportSymbol`
(`vortex_core.cpp:119-127`: `makeCell(name)` — name is *content*, no `+d.name`) and the
`d.vars` first-match walk (`vortex_stdlib.cpp:1951-1954`) confirm the `learn_method` asymmetry;
`plan_named` has no C++ implementation yet (a `std:meta` builtin this doc specifies) and its
clause-`+d.name` matching is consistent with spec §5.7/V15. Every new line cite checks out —
including `store.cpp:124`, where **my r12 report was itself off by one** (`:124` is
`userPermascroll_`, `:125` `chronofilade_`; the doc's v1.12 fix is right and my earlier
verification was wrong). New defects in the deltas: **0 BLOCKING, 1 MAJOR, 5 MINOR, 3 NITS.
VERDICT: NOT CONVERGED.**

## Citation verification — v1.12 delta cites

- `vortex_core.cpp:366-369` — `wingOf` enters via `arena_.linked(opcode, dims_.grab, side)` at
  :366, `rank(...d.step)` at :368 — "the op loses its whole input wing" cite exact.
- `vortex_core.cpp:384-390` — `bindInput`: `makeScalarCell(int64(operand))` :384, `link(opcode,
  dims_.grab, POS, slot)` :389-390 — "writes `opcode +d.grab → slot`" exact.
- `vortex_core.cpp:119-127` — `exportSymbol`: `makeCell(symbolName)` :122, `+d.values→entryOp`
  :123, `rankTail +d.vars→symCell` :124-126 — exact; symbol name is *content*.
- `arena_manifold.cpp:1347-1377` — `displacedUs`/`displacedIt` read + `setOneSide(..., noCell)`
  eviction — the single-occupancy mechanism, exact.
- `arena_manifold.cpp:1867-1868` — `if (!isEphemeral(arena)) { real.emplace(arena, arena); }` —
  the verbatim persistent-ref identity map; `real.emplace` now correctly named.
- `store.cpp:124` — `userPermascroll_(std::make_shared<UserPermascroll>())` — **exact**; `:125`
  is `chronofilade_`. Correction of my r12 error, not the doc's.
- `store.cpp:3840` — `if (record.produces.isZero() || opsSpool.contains(record.produces))` —
  the `opsSpool.contains` line, fixed from `:3841`.
- `store.cpp:1461-1469` — `Store::scroll(ScrollId)` returns
  `cpp26::optional<const Scroll&>`; disengages on `localScroll == id || id > externals.size()`
  — exact (see NIT-1 for the second arm).
- `scroll.cpp:13-22` — `scrollKey(const Scroll&)`: named → `scrollKeyFor(publisher, salt)`;
  empty segments → `nullopt`; else `file:{torrent}:{fileIndex}` — exact.
- `spool.hpp:19` — `inline constexpr ScrollId localScroll = 0;` — exact.
- `publication.cpp:1097-1102` — `historyFromSeal` mints `std::make_unique<Store>()` :1097,
  `addScroll(from)` :1098, loops `applyOpsSegment(segment, *history, ...)` :1100 — "mints its
  own `Store` internally", nothing applies a segment into a caller-held store; exact.
- `mesh_actor.cpp:188` — `!checkpoint.store` inside the resume validity guard — exact. `:261` —
  `checkpoint.store->userPermascrollPtr()` read inside `receiveValue` — "read only for
  `userPermascrollPtr()`" exact. `:260-269`,`:271-273`,`:119-122`,`:156` — re-verified.
- `vortex_stdlib.cpp:4238-4241` — `addClause`'s same-dim `d.clause` `rankTail` append — exact;
  `arena_manifold.cpp:1423-1439` `insertIntoRank`, `cell_views.hpp:258` `rankTail` — exact.
- `vortex_vm.cpp:854-856` — `Call` executes `toInt(inputs[0])` — exact; `mesh_value.cpp:519-523`
  — `mergeValues` orders `(contentHash, authorKey, parent)` — exact; `mesh_host.cpp:507-510` —
  `job.base` vs fresh `Store` arms — exact; `store.cpp:3836-3940` — `adoptOpRecords` topo-sort
  drained by a min-`produces` `priority_queue` (:3909-3913) — exact.

## Ruling-adoption verification

- **R12-C adopted correctly.** §10.2 names the segment-apply surface twice: adapter surface
  (iii) at :2787-2791 ("`adoptOpRecords` takes already-decoded records, `historyFromSeal` mints
  its own `Store` internally (`publication.cpp:1097`), and nothing applies a received segment
  into a store the caller already holds — owed only if the kept-in-step adapter twin of §6.6's
  seam is kept") and the registered-outside-the-core-register paragraph at :2805-2810.
  §6.6's "named as owed surface under §10.2" (:1594) now resolves.
- **R12-B adopted correctly and the mechanism verifies.** The `alias` field as recorded
  superseded target is *sufficient*: dedupe matches `alias` verbatim against clause `+d.name`s
  and unlinks the first hit, and the rank at replay is deterministic — folded-back image
  clauses in image order plus earlier-replayed learned installs (whose `patch:<m>` labels are
  unreachable since `patch:` is refused as a `name`, :1195-1197). Grafted clauses can't precede
  declared ones (`rankTail` append), so a session hit on a grafted clause implies no declared
  clause matched — replay's no-hit append is equivalent since grafts never persist; a session
  hit on declared C re-hits C after the fold. `unlearn` of the displacer not re-linking C
  (:1221-1223) self-heals the unlearn-then-relearn interleavings. The `exportSymbol` /
  `plan_named` asymmetries are real: symbol name = cell *content* (`vortex_core.cpp:122`), the
  `d.vars` walk matches `textOf` first-hit (`vortex_stdlib.cpp:1951-1954`) — so
  `learn_method`'s replace genuinely replaces — while clauses carry `+d.name` labels
  (`addClause` itself mints none, `vortex_stdlib.cpp:4226-4241`; spec V15 confirms declared
  *handlers* get none; declared *plan* `+d.name` is this family's own consistent representation
  choice, spec §5.7 being silent on storage).
- **R12-A adopted in all three sites** (:1050-1057 head diagram, :1136-1158 enumeration,
  :2163-2167 genesis (e)) — `d.refFor`/`d.captured` replace the barred `+d.grab`/`+d.values`
  correlation edges — **but introduces Finding 1**.
- **R12-D adopted.** (a) holds the canonical comparand (:2052-2066: order-vs-order post-remap
  `OpRecord`/`CompactOpNode` fields, scroll *identity* by `scrollKey` under the `localScroll`
  guard); §6.6's verify arm cites it verbatim (:1585-1587).
- **Minors all applied:** serial pinned to the agent cell's `d.vars` (:1179, :1719, :1810,
  :2262-2264, :2353); replay `+d.name` transport is re-mint not re-link (:2143-2149); (d)'s
  "no serial" scoped to `patch:<n>` with `reify:<src>:<serial>` still minting (:2139-2140);
  per-verb label/serial split (:1738-1750); alias→clause join spelled (:1189-1194); `unlearn`
  `d.patch`-record refusal by `-d.patch` backlink (:1112, :1794-1797); `≤E_ops` agent-walk claim
  scoped for `agent_cell()` itself program-side (:2082-2086, :1495-1499); `d.weight` de-ranked
  to the §8.1 scalar edge dim (:1494-1496); `label_of` "all three paths" (:1098); citation nits
  applied.
- Pre-existing loose cite unchanged (not a new defect): `vortex_core.cpp:100-127` for the
  `d.vars` name walk at :1200 — the actual first-match `d.vars` walk is
  `vortex_stdlib.cpp:1951-1954`; :100-127 is `findModule`/`exportSymbol` (`d.stdlib`/`d.vars`
  append).

## New findings (v1.12 deltas only)

### Finding 1 — MAJOR

- **Location:** :1149-1153 (§6.1 ref-capture enumeration) and the echo at :2166 (genesis (e)).
- **Problem:** the shared-referent indirection is ambiguous between a star and a chain, and
  one of the two readings silently breaks. "a shared referent is named once, later records
  carrying `+d.captured → the earlier ref-capture record` … whose `+d.captured` the replay
  follows one hop". Reading (a) — "the earlier" = *the* record that named the referent — all
  later records link `+d.captured →` that one record, whose `-d.captured` backlink is
  single-occupancy: a **third** record's link evicts the second's `+d.captured` entirely
  (`arena_manifold.cpp:1347-1377`, the same M14 rule R12-A was fixing), and at replay that
  record has no endpoint — the slot rehydrates from its recorded literal, the stale-alias bug
  the endpoint derivation exists to preclude. Reading (b) — each later record points at the
  *immediately earlier* record in the sharing group — chains safely but makes "the replay
  follows one hop" false for the k-th sharer (k−1 hops). Three slots capturing one binding —
  `code"{ g(x, x, x) }"` — is a routine shape; the defect fires at exactly 3+ sharers.
- **Evidence:** the definite-article "the earlier ref-capture record" most naturally names the
  first (the one that "named it once"); `link`'s displacement of an occupied `-d` neighbour is
  mechanical and unconditional (`arena_manifold.cpp:1349-1372`).
- **Concrete fix:** pin the chain shape and the hop rule — e.g., "later records carry
  `+d.captured → the immediately-earlier record sharing the referent`, forming a chain whose
  tail is the referent cell; replay follows `+d.captured` until it lands on a non-record cell
  (one hop per extra share)" — and mirror it in (e) at :2166. (A per-referent hub cell can't
  fix it: the hub's `-d.captured` contends identically; the chain is the single-occupancy-safe
  shape.)

### Finding 2 — MINOR

- **Location:** :1743 — "two cells, never one shared, **the clause's `-d.name` holding a
  single edge** (genesis (d)'s re-mint rule)".
- **Problem:** wrong endpoint for the occupancy argument. The edge is
  `clause +d.name → label`; nothing writes `+d.name →` a clause, so the clause's `-d.name` is
  free and cannot justify two label cells. What forces two cells is the *label cell's*
  `-d.name` backlink: one label cannot hold backlinks from both record and clause. Genesis
  (d) itself states the mechanism correctly (:2144-2145: a re-link "single occupancy would
  evict off the record") — the §6.6 sentence names the wrong cell, and direction precision is
  load-bearing everywhere this vocabulary is used.
- **Concrete fix:** "the label's `-d.name` holding a single edge" (or "a `-d.name` backlink
  holding a single edge").

### Finding 3 — MINOR

- **Location:** :1746-1747 — "`learn_method`'s installed symbol keeps its functional
  `owner.name` content — a `patch:` label under it would shadow the `d.vars` name walk it is
  found by".
- **Problem:** mechanism misstated — a `+d.name` edge cannot shadow the `d.vars` name walk,
  which matches symbol *content* (`arena().textOf(sym) == symName`,
  `vortex_stdlib.cpp:1951-1954`; `exportSymbol` mints `makeCell(name)`,
  `vortex_core.cpp:122`), never `+d.name`. The true reason is the one the doc itself states at
  :2289-2294: the symbol's identity *is* its content — a `patch:` label edge would be dead
  weight (and would leak into `label_of`'s clause-arm read), while putting `patch:<n>` in the
  *content* would rename the symbol. The sentence conflates the two name representations the
  doc takes such care to split.
- **Concrete fix:** "— a `patch:` name as its *content* would lose the functional name the
  `d.vars` walk matches on `textOf`, and a `+d.name` label is a handle nothing on the symbol
  arm reads —".

### Finding 4 — MINOR

- **Location:** :1724-1726 — patch record field set lists "the user `name` alias"
  unconditionally.
- **Problem:** `learn_where`/`learn_body` take no `name` argument at all
  (`learn_where(plan_named("demo:Guard.cautious"), code"…")` :2567;
  `graft_where(clause, test)` :1107), and `learn_method`'s `name` is "never an alias" (:1714).
  Only `learn_plan`/`learn_handler` records can carry an `alias` key-record; the same list
  already conditions `priority` ("priority for learn_handler"), so the unconditional alias
  reads as every-verb.
- **Concrete fix:** "the user `name` alias where the verb takes one (`learn_plan`,
  `learn_handler`)".

### Finding 5 — MINOR

- **Location:** :1187-1190 — "the alias is **write-only audit metadata** … — its only two uses
  are the `d.patch` rediscovery join below and `learn_plan`'s replay dedupe".
- **Problem:** self-undermining — both named uses are reads: the join *reads* the `alias`
  key-record in-session via `cells"$a/+d.patch"`, and the replay dedupe reads it at genesis.
  The intended claim is the sentence's own "never a session lookup key" (no runtime resolves a
  clause *by* alias); "write-only" contradicts it.
- **Concrete fix:** "the alias is audit- and replay-keyed metadata, never a session lookup
  key — its only two uses are …".

### Finding 6 — MINOR

- **Location:** :1192-1194 — the rediscovery join "matching the `alias` key-record on each
  `d.patch` member" yields `<n>` "read off the patch *record's* `ordinal` field".
- **Problem:** join multiplicity unstated. Aliases are not deduped — `learn_plan` dedupe hits
  stored clause `+d.name`s only, and `patch:` labels are unreachable — so two `learn_plan`s (or
  a `learn_plan` and a `learn_handler`) can record the same alias; the join then matches two
  patch members and yields two ordinals, with no stated resolution (latest? all? error?).
- **Concrete fix:** one clause — "the join yields every match (same-alias reuse is possible;
  the caller picks by `ordinal`, latest-wins)" or "the alias is uniqueness-refused across live
  patches at learn time".

### Nits

- **NIT-1** (:2059-2063): the `localScroll` guard covers only half of `Store::scroll`'s
  disengagement — it also returns `nullopt` for `id > externals.size()` (`store.cpp:1463`), so
  `scrollKey(*store->scroll(id))` derefs a disengaged optional on an out-of-range scroll id;
  "an id beyond `externals` fails the compare outright" closes it. (`scrollKey` itself can
  also return `nullopt` for an unnamed segmentless scroll — two such compare equal under
  nullopt==nullopt; degenerate, worth half a sentence at most.)
- **NIT-2** (:1740): "the uniform `label_of`/`unlearn` bookkeeping handle" — `unlearn`
  explicitly does *not* use the record's own label (it matches by recorded target name and
  refuses a `d.patch` record argument by its `-d.patch` backlink, :1794-1797); the record's
  `+d.name` is `label_of`'s and the audit join's handle. Drop `/unlearn`.
- **NIT-3** (:1045): the head diagram keeps `+d.grab→"call"/"run" role label` on `+d.refs`
  name-records — legitimate post-R12-A (the target is a fresh text label, not a slot; :1901
  does name "the same fresh-scalar convention the memo key uses"), but the diagram doesn't say
  fresh-per-record; an interned `"call"` cell would trip the same single-occupancy eviction
  between same-role records. Worth "(fresh per record, M8)" inline since the diagram is what
  an implementer copies.

## Non-findings verified (delta claims that hold)

- **Dedupe replay determinism:** grafted clauses can never precede declared ones on the rank
  (`rankTail` append, `addClause` precedent), so the session first-match victim and the replay
  first-match victim coincide whenever the victim is declared; grafted victims don't persist
  and reduce to the append case. `unlearn`'s no-re-link rule (:1221-1223) makes the
  unlearn/relearn interleavings converge — C dies or resurrects identically in session and at
  genesis in every ordering I enumerated.
- **Re-mint at second genesis:** "drop `patch:<n>` at the *second* genesis" (:2145) is
  compressed but correct — under a hypothetical re-link, genesis-1's copy would steal the
  record's label edge, and the *propagation failure* (copy gets no label) manifests at
  genesis-2.
- **`learn_method` replay for free** (:1219-1220): `graft_method`'s unlink-first re-runs at
  genesis, unlinks the folded-back image symbol, and installs the reified copy's symbol —
  stacked `learn_method` replaces replay in ordinal order correctly (later patch finds the
  earlier replay's installed symbol).
- **Per-verb serial rule consistency:** "every `learn_*` consumes its `<n>`" (:1738-1739) now
  pins the non-clause verbs' serials, resolving r12 Finding 3; consistent with "only the verb
  can mint it" (:1182) and no-reclaim (:1810-1811).
- **Role label freshness:** :1900-1901 does state "the same fresh-scalar convention the memo
  key uses (M8)" for the `+d.grab` role label — R12-A's bar (correlation edges) doesn't cover
  it and doesn't need to (see NIT-3 for the diagram nit only).
- **`store.cpp:124` is right and my r12 report was wrong:** the file was byte-identical then;
  `:123` is `Store::Store()`, `:124` `userPermascroll_`, `:125` `chronofilade_`.

VERDICT: NOT CONVERGED
