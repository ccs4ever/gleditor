# r17-codebase: Round-17 codebase verification of `design/vagent-metaprogramming.md` v1.16

**Orchestrator summary (≤200 words):** The R16-B fix lands and is mechanically
sound end-to-end: `Store::makeDimension` (`store.cpp:1057-1091`) mints + appends
at the `d.dims` rank tail + `registerDim`s; `getOrCreate`
(`dimension_registry.cpp:194-217`) is the find-or-mint wrapper;
`vql/compiler.cpp:1128-1141` is verbatim the "replicate dimensions" precedent.
Ordering is sound: spawn-seed filing on the lineage `Store` precedes the
overlay, so in-session `ensureDimension` (`arena_manifold.cpp:78-105`) binds the
filed canonical cell as a base ref, edges ride it through `promote` verbatim
(`:1867-1869`), no twin, no accretion — and `promote` provably cannot emit
`d.dims` links (`:1766` discovery skip + `:2030-2033` `real.find` gate +
`:2021` `!holdsOwn` on base tails). Name-keyed `dim` endpoints are consistent
with the `dimConstant` arm and (e)'s `Int64(C)` writeback. All adopted minors
landed; **every v1.16 citation verifies exact**. One new MAJOR: the `+d.recipe`
member's field-schema carrier is never pinned while `+d.name` gains a reserved
second role on the same record class — a `+d.name`-carried schema name collides
with the dim-name channel (M14) and mis-fires (e)'s dim arm on every non-dim
field. **1 MAJOR, 3 MINOR. VERDICT: NOT CONVERGED.**

## Citation verification — v1.16 delta cites (all pass)

- `vql/compiler.cpp:1128-1141` — `// 1. Replicate dimensions` (:1128), loop over
  arena cells, `name.starts_with("d.")` → `getOrCreate(store, ver, manifold,
  name)` → `dimMap` (:1130-1141). The cited precedent is exact (:2929-2932,
  :3863-3864).
- `Store::makeDimension` (`store.cpp:1057-1091`) — `makeCell(name)` +
  `setLink(tail, *dimsDimension_, POS, ref)` at rank tail (:1087-1088) +
  `registerDim` (:1089); refuses without `d.dims` genesis (:1059-1063), which
  `getOrCreate` handles (`dimension_registry.cpp:202-204`). `getOrCreate`
  (:194-217) mints + rebuilds the caller's manifold + registers — exact match
  for "DimensionRegistry::getOrCreate → Store::makeDimension per name".
- `arena_manifold.cpp:1766` — `if (from.textOf(edge.dim) == "d.dims")
  continue;` in the discovery walk. `:2030-2033` — `real.find(edge.dim)` /
  `real.find(edge.pos)` gate skipping both-missing folds. Combined with
  `:2021` (`!from.holdsOwn(arena)` skipping base cells' edges), "promote emits
  no `d.dims` links" is mechanically airtight — `d.dims` dim cells never enter
  `real` through the skipped edges, and base-anchored rank edges fail the own
  test even when the tail is ephemeral.
- `arena_manifold.cpp:78-105` — `ensureDimension`: `dimensionNamed` →
  `arenaDims_` → `intern` + `makeCell` + `arenaDims_` insert + `d.dims`
  rank-tail link (:91-103). Exact.
- `arena_manifold.cpp:1009-1012` — `cloneMaster` = `rankEnd(NEG)`. Exact — the
  `-d.clone` backlink traversal the text now names (:2217, :3807).
- `arena_manifold.cpp:1759-1764` — relocation-handle targets enqueued into
  `order`/`frontier` (:1759-1763). `:1764-1768` — the `d.dims` skip the (e)
  accretion bound cites. Exact.
- `arena_manifold.cpp:1934-1936` — `Int64` → `makeScalarCell(asInt64)`
  verbatim. `:1947-1951` — `OpHandle` → `isEphemeral(target) ? real.at(target)
  : target`. Exact.
- `manifold.cpp:771-775` — `dimensions()` = `rankAfter(*home_, *dimsDim_)`
  rank walk; `dimensionNamed` = registry `get` + `contains` fast path
  (:787-792), then rank+`textOf`/`d.alias` filter (:793-814), `registerDim`
  backfill (:815-819). Supports every "rank-resident re-binds / off-rank
  re-mints" claim.
- `mesh_value.cpp:485-501` — unmarked `OpHandle` demoted `None` (:485-490);
  relocation-marked re-minted `OpHandle(local.at(*target))` (:494-501). Exact.
- `vortex_vm.cpp:816-825` — `Clone`: `cloneTail = *c` (:815), walks
  `linked(cloneTail, dims().clone, false)` (`fromNegward(false)` = POS,
  `dim_vector.hpp:85-86`), `link`s the clone at the first free tail
  (:821-822) or breaks on re-encounter (:825). "Appends at the master's
  posward tail" exact; chained captures run clone→clone as stated.
- `vortex_core.cpp:344-346` — `masterOf` = `cloneMaster(...).value_or(cell)`.
  `:358-365` — operand lambda (`asInt64` → `contains` → `.value_or(masterOf)`).
  `:366` — `linked(opcode, dims_.grab, side)` — the corrected cite lands.
  `:372-374` — `inputsOf` = `wingOf(POS)` (`:373` is the POS delegation, as
  the changelog says). `:384-390` — `bindInput` `+d.grab` POS. `:113-126` —
  rankTail `+d` member edges. `:61` — `d.name` mint. `:181`,`:202` —
  `masterOf` at read path + `writeValue` funnel (`writeValue` covers
  Int64/Double/Bool/None, :196-229 — P16 row exact).
- `vortex_vm.cpp:98-100` — `setOpcodeKind` inserts `opcodeMap_`. `:620` —
  `Link` takes `DimRef` raw (`toInt(inputs[1])`). `:807-836` — `Clone` case
  span. `:74-75`,`:103-114` — opcode cell/`opcodeMap_`/`asInt64` fallback
  `[0, Linked]`. Exact.
- `arena_manifold.hpp:170-186` — `Mark` fields: `cellCount`, `linkSize`,
  `contentSize`, `scratchSize`, `trailSize`, `liveLinks`, `liveContent`,
  `shadowCount`, `proxyCount`, `quoteOccurrenceCount`,
  `proxyShadowedEdgeCount`, `quoteSpaceCount` — no dimension field. Exact.
- `mesh_evaluator.cpp:735` — `builder.link(cell, relocation,
  arena.makeCell("cell"))` inside the per-reference `for` (:724-736).
  Per-reference marker mint exact.
- `foreign_op_handle.cpp:98-117` — `isCellRelocationHandle` = `OpHandle` kind
  + `d.mesh.relocation`-dim edge to a `"cell"`-texted pos. Exact.
- `ops.hpp:154-163` — `ValueKind::{None..ForeignOpHandle}` enum span covering
  `OpHandle` (4) and `ForeignOpHandle` (7). Exact for "handle-kind".
- `arena_manifold.cpp:1347-1377` — `displacedUs`/`displacedIt` cleared via
  `setOneSide(..., noCell)` — the single-occupancy eviction the `+d.values`
  marker refusal cites (:1352-1373 form also used :2091). Exact.
- `compiler.cpp:769-772` — `dimConstant` = `literal(int64(
  findOrMintDimension(name)))`. `:475-483`/`484-486` — Variable-anchor
  compile-time resolution + mutating-path refusal. `:160-181` — `binding()`'s
  render/type check. Exact.
- `vagent-agent-language.md:1670` — P4 row (`Local wake: wake(pid) runs the
  executor with ActorExecution::message absent`). Exact.
- `mesh_actor.cpp` — `:119-122` spawn signature (no store), `:156` context
  mint, `:182`/`:188` `ReceivedValue checkpoint` + `!checkpoint.store` guard,
  `:260-261` `receiveValue` reading only `userPermascrollPtr()`, `:269-271`
  checkpoint→`WorkerContext(manifold.get())`, `:462` empty-mailbox skip,
  `:467-468` `front()`/`pop_front()`, `:659-662` `pending`=`mailbox.size()`,
  `:672` `checkpointView` `!mailbox.empty()` refusal. All exact.
- `mesh_actor.hpp:102-106` — `ActorCheckpointView` = continuation+arena+root,
  no store. `worker_context.hpp:52-54` — two-arg `(baseManifold, reader)`
  ctor. `arena_manifold.hpp:269` — `store()` exposing `store_`/`base_->store()`.
  Exact.
- `vortex_stdlib.cpp:603`,`:628` — `preconditionsOf`/`postconditionsOf` on op
  cells under the `-d.contract`/`+d.contract` comments. `:1953` —
  `textOf(sym) == symName` `d.vars` filter. Exact.
- `dim_vector.hpp:85-86` — `fromNegward`. Exact.

## Fix verification

- **R16-B (name-keyed `dim` endpoint + `d.dims` filing) — landed and
  mechanically sound.** The tuple `dim` field is now the stated lone
  position-keyspace exception: `+d.name` on the member record →
  `ensureDimension` re-resolution "exactly as the `dimConstant` arm"
  (:1174-1178, :1214-1218, :1071-1074, :1944-1948, :2089-2098, :2240-2243,
  :2300-2327, glossary :140, :2992 P16 row, :3863-3866). Filing is named at
  the three required sites (table :95, §2.8 :110-113/:544-547, §10.1
  :2926-2938) plus (b) :2240-2243 and §6.6 :1575-1577, :3360-3363.
  End-to-end: filing precedes the overlay (spawn-seed on the kept lineage
  `Store`, base-injected before `initGenesis`), so session
  `ensureDimension`/`mintNamedDimension`/`findOrMintDimension` all hit the
  base's persistent rank (`dimensionNamed` base arm
  `arena_manifold.cpp:58-70` → registry `get` + `contains` +
  `Manifold::dimensionNamed` rank walk `manifold.cpp:781-819`) and bind the
  canonical cell as a *base ref* — `promote` then folds the edges verbatim
  (non-ephemeral `edge.dim` → `real.emplace(arena, arena)`, :1867-1869). No
  twin is ever minted for a filed dim, so the sealed image's edges ride the
  same cell `dimensionNamed` resolves — the r16 Finding-1 split-identity
  defect is closed for the filed set, and honestly scoped to it ("unfiled
  session-minted" is the named residual at :1575, :2242-2243, :2935-2936).
- **R16-A (one-`+d.refs`-rank inventory) — landed consistently.** Emission
  (:382-388, :499-503, :2082-2102), diagram (:1053-1090), re-kind (:2131-2135),
  restore (:2148-2150), frontier (:1937-1939), (e) (:2313-2320), P16 row
  (:2992), cost (:3124-3128), changelog (:3846-3854) all say the same thing:
  every marked cell's `+d.ref` record lives on `+d.refs`; the `+d.recipe`
  member is the field cell itself carrying schema only. The re-kind/restore
  enumeration genuinely stays `+d.refs`-bounded (field cells reached through
  their `+d.ref` record, the `+d.recipe` side read only for the name arm).
- **Adopted minors verified in place:** "re-binds rather than re-mints"
  scoped to *rank-resident* dims (:1573-1577); `masterOf`/`cloneMaster`
  restated as the `-d.clone` backlink traversal (:2215-2218, :832-835,
  :3805-3808); pairing named — member = field cell, `+d.ref` the
  inventory-analog (:1168-1170, :3868-3870); `cap$N` re-clone → re-capture
  in the fixed `New`/`Link`/`Value` palette with the §11-`Clone`-refusal
  carve-out scoped (:1226-1230, :3834-3835); `set_weight` dispatch —
  junction keys off own `+d.weight` scalar (matches §8.1's
  `J +d.weight → FloatCell`, :2566-2568), clause off qualified `+d.name`,
  `+d.name`-less refusal scoped to clauses (:2573-2580); "dim mark
  (`+d.name`)" (:2301) and stale "program-side" qualifiers corrected;
  `Linked` binds a `DimRef`, "never the promoted twin `*handleTarget`
  names" (:2327-2328); `+d.clone →` first-capture/chained scoping
  (:2210-2215, :3801-3804); `wingOf` cite corrected to `:366` (:1190-1191)
  with `:373` named `inputsOf`'s POS delegation; `+d.values`-carrier
  superseded annotations in the 1.9/1.10/1.11 entries (:3538, :3594, :3634);
  (d)'s install set names `+d.recipe` records beside `+d.refs` (:2307-2308).

## New findings (v1.16 deltas only)

### Finding 1 — MAJOR

- **Location:** :387, :1069-1072, :1168-1169, :2097-2098, :3849-3850 ("the
  member record carrying the field's schema, name → tuple position") against
  :1175, :1214, :2093-2094 (`+d.name` on the `+d.recipe` member for the dim
  channel) and :2322-2327 ((e)'s arm key "a marked cell whose `+d.recipe`
  side bears `+d.name`").
- **Problem:** v1.16 puts `+d.name` on `+d.recipe` member records for the
  `dim`-field name channel, while never naming the carrier of the member's
  own field-schema name. `+d.name` is the document's universal name channel
  (head labels, `+d.refs` name-records, dim names all use it), so an
  implementer reading "the member record carrying the field's schema (name →
  tuple position)" and recording field names the same way hits two silent
  failures: (a) the `dim`-field member then wants `+d.name → "dim"` *and*
  `+d.name → "d.<x>"` — one cell, one `+d.name` edge — M14 single-occupancy
  eviction (`arena_manifold.cpp:1347-1377`) drops one channel; (b) worse,
  (e)'s dim arm keys on `+d.name` *presence* on the `+d.recipe` side — a
  `+d.name`-bearing `from`/`to`/`dir` member would be written
  `Int64(ensureDimension("from"))` — a spurious dim minted and the endpoint's
  position destroyed — and `closed`'s name-resolution check (:1946-1948)
  would likewise misread the member as a dim endpoint. The doc must pin the
  field-identity carrier — positional only via the member's `+d.step` chain
  index, or a named dim like `+d.grab` (the role-label precedent, :1062,
  :2006) — and state `+d.name` on a `+d.recipe` member is reserved to the dim
  channel.
- **Evidence:** `arena_manifold.cpp:1347-1377` (single-occupancy eviction),
  doc :1062/:2006 (`+d.grab` role labels — the established non-`+d.name`
  carrier), doc :2093 ("its `+d.recipe` record's `+d.name` carries the dim
  name" — the second use on the same cell), doc :2322-2327 (the `+d.name`-
  keyed arm selection).
- **Concrete fix:** one line in §6.1/step-1: "field identity is the member's
  position on the `+d.recipe` chain (`+d.step` index = tuple position) —
  `+d.name` on a `+d.recipe` member is reserved to the `dim` field's
  name-channel record" (or name the schema edge `+d.grab` if names must be
  recorded).

### Finding 2 — MINOR

- **Location:** :2328-2330 (genesis (e), the `Linked` sentence).
- **Problem:** "a session-minted dim in use accreting one such orphan per
  checkpoint off the folded `d.dims` rank (`arena_manifold.cpp:1764-1768`),
  dead weight beside the re-minted canonical cell the name arm re-resolves"
  is now accurate only for *unfiled* dims. For filed dims the session
  `ensureDimension` binds the canonical cell as a base ref — no ephemeral
  mint, no twin, no accretion — and the name arm re-**binds** the filed cell,
  never re-mints it. The same sentence :1575-1577 got this scoping; (e)'s
  gloss missed it.
- **Concrete fix:** "a session-minted dim left unfiled accreting one such
  orphan per checkpoint … dead weight beside the canonical cell the name arm
  re-resolves (filed at seal, §10.1)".

### Finding 3 — MINOR

- **Location:** :2210-2216 (agent-cell-legs restatement); same wording in the
  1.14-entry echo :3737-3741 and the 1.15-entry echo :3801-3805.
- **Problem:** "the `d.clone` rank's posward-tail append — the binding cell's
  own `+d.clone →` the `cap$N` slot on a *first* capture, the prior clone's
  on a chained one — … image-sealed for a declared fragment, never
  runtime-grown, and running state→program on a `d.clone` dim" — the
  state→program qualifier still modifies the joint append claim, but the
  chained edge runs prior-clone→clone, i.e. program→program (both `cap$N`
  slots are fragment cells). The r16 ruling scoped the *endpoint* claim; the
  direction qualifier needs the same scope.
- **Concrete fix:** "…running state→program on a first capture (program→
  program on a chained one) on a `d.clone` dim the walk's `+d` vocabulary
  does not list" — or drop the direction clause for the chained arm.

### Finding 4 — MINOR

- **Location:** :2992 (P16 register row).
- **Problem:** "`+d.name` on a record makes it a name-record — no marker,
  never re-kinded" is unscoped, and `+d.recipe` members are called "the
  member record" (:1168, :2097). Read to cover member records, the sentence
  unmarks the `dim`-field cell that :2093 explicitly keeps "marked like
  every field". The operative steps scope it correctly (the discriminator is
  on the `+d.refs` record, :2082-2085); the register's shorthand should
  match.
- **Concrete fix:** "`+d.name` on a `+d.refs` record makes it a name-record
  — no marker, never re-kinded — while `+d.name` on a `+d.recipe` member is
  the dim channel only (the member stays marked, §6.6 step 1)".

## Non-findings verified (delta claims that hold)

- **Filing cannot disturb the (a) comparand:** `makeDimension` ops append
  past the sealed image band (> `E_ops`, state-side refs), so the
  `1..E_ops` order comparand is untouched; `dimensionNamed` resolves them
  regardless of position (`manifold.cpp:766-775` rank walk, no ref bound).
- **`promote` truly cannot emit `d.dims` links:** `:1766` keeps `d.dims`-dim
  edges+cells out of `order`; even for an ephemeral rank tail the fold's
  `real.find(edge.dim)` fails (:2030-2033) since the `d.dims` dim cell is a
  base ref that only enters `real` via non-`d.dims` discovery — and
  `:2021`'s `!holdsOwn` blocks every base-anchored edge anyway. The doc's
  joint cite (`:1766`,`:2030-2033`) is the honest mechanism.
- **Chained captures decode correctly:** replayed `binding +d.clone → cap1`,
  `cap1 +d.clone → cap2` reproduce `Clone`'s append shape (`vortex_vm.cpp:
  815-829`), and `cloneMaster(capN)` = `rankEnd` NEG walks the whole chain
  back to the binding (`arena_manifold.cpp:1009-1012`) — `wingOf`'s operand
  lambda reads the master for each slot (`vortex_core.cpp:358-365`).
- **`dim`-field dual channels are coherent:** the field cell stays a marked
  int64 carrier (uniform rule), its `+d.refs` `+d.ref` record is `+d.name`-
  less (ref-record → marked), the dim name lives on the `+d.recipe` member
  (the field cell itself); restore writes `Int64(*handleTarget)` (C for a
  filed dim, dead T for unfiled) and (e) unconditionally corrects to
  `Int64(C)`. Replay resolves via `+d.name` → `ensureDimension` in-session
  and post-checkpoint alike — self-consistent for replayed structure.
- **`closed` frontier coverage:** the `dim`-field's endpoint is a dim cell —
  ephemeral → state-side; persistent → name-resolved through the member's
  `+d.name` (:1944-1948, glossary :140). Both arms stated.
- **Adapter vocabulary coverage:** the filed list
  (d.patch/d.code/d.refs/d.recipe/d.refFor/d.entry/d.weight/d.synapse)
  + "every other ensureDimension-minted name" covers the whole minted set
  named at :541-543 and :2922-2924 — and user/session dims outside the filed
  set are honestly the residual ("unfiled … folds to an orphan twin"), not a
  hidden contradiction.

VERDICT: NOT CONVERGED
