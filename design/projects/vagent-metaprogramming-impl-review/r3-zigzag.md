# R3 (impl-plan) — ZigZag/promotion-semantics critique

Lane: cell-graph topology — dimension discipline, filing/promotion ordering, link
eviction, genesis sequencing. Target: `design/vagent-metaprogramming-implementation.md`
v3. Reference: `design/vagent-metaprogramming.md` v1.19.

Method as r1–r2: every new/changed cite re-verified against the tree. This round's
new verifications: `arena_manifold.cpp:1733-2055` (`promote` end-to-end — the
`d.dims` discovery skip at :1766-1768, the `!isEphemeral` map-through at
:1867-1870, the `holdsOwn`-gated link emission at :2020-2053), `store.cpp:1057-1090`
(`makeDimension` = `makeCell` + `setLink` ops — durable), `manifold.cpp:782-821`
(`dimensionNamed` registry + `d.dims`-rank walk), `worker_context.hpp:51-55`,
`vortex_core.cpp:14/:29-70/:196-229`, `vortex_vm.cpp:98-100/:337/:343-346/:771/:854-856/
:1269-1273/:1287-1294`, `mesh_actor.cpp:182-189/:261/:473-477/:657-661`,
`mesh_evaluator.cpp:735`, `mesh_value.cpp:473`, `or_parallel.cpp:182-185`,
`publication.cpp:1093-1103/:1129-1131`, `store.cpp:121-123/:1416-1427/:3836`,
`compiler.cpp:108-110/:160-181/:498-502/:769-772/:853-855/:884-891`,
`compiler.hpp:15-18/:23-26`, `checker.hpp:12-27/:29-30`,
`checker.cpp:765-770/:2059-2070`, `parser.cpp:848-855`, `mesh_actor.hpp:79-83`,
`mesh_jobs.cpp:304-318`, Makefile:643/649/688-690/692/695/697-699/1147/1348-1352.
All land (two one-line drifts, nits below).

## R2 closure verification — all items closed

| r2 item | v3 disposition |
| ------- | -------------- |
| **M2-1** (dim-record arm unassigned) | **Closed, cleanly.** (e) now reads "three record arms" (:418): the **dim-record arm** — `+d.name`-bearing `+d.recipe` records (ref-capture dim records *and* `dim`-field members) write `Int64(DimRef)` re-resolved through `ensureDimension`'s name arm into the `+d.refFor`-paired slot or the member cell itself — *never* the promoted twin (:420-424); **every-genesis scope** stated — "not a no-op pre-P16 — it runs at every genesis off the filed rank" (:424-426); the **(d)-derivation exception** landed — "the record's `+d.name` *is* the endpoint source and the slot takes `Int64` of the resolved `DimRef`, not the donor's promoted twin" (:413-416); phase-6's arm narrowed to `*handleTarget` writes inside the P16 step (:478-480); a matching exit added (:445-446). One residual exemption clause, m3-2. |
| **R2-B** (re-genesis producer) | **Closed.** The loop names promote → `rebuildManifold` → fresh `WorkerContext` → re-run (a)–(e) (:378-385); Requires lists the harness objects (:436-438). See M3-1 for what the loop still doesn't say. |
| m2-1 ((b) arm split) | **Closed** — "two arms, not one scope" (:395-399). |
| m2-2 (no re-record) | **Closed** (:407-409), incl. the `reify:` serial still drawing the shared counter. |
| m2-3 (bind-set narrowing) | **Closed** — name-bearing members file, bound non-dim members bind at (c) and never file (:257-259, :361-363). |
| m2-4 (`fragment` placement) | **Closed** — `vagent/` placement stated as load-bearing for the filter-out, spec :2944 verified (:221-223). |
| m2-5 (both lineages + pairwise comparand) | **Closed** — spawn/resume get the long form, emit the compressed "file before first resolution" (:343-348); the comparand is named pairwise with the second-store refusal fixture (:391-394). |
| n2-1/n2-2/n2-3 | **Closed** (:444, :406, :388-390). |

The rest of the disposition's minor bundle in my lane all landed: env-walk
fallback (:171-173), `closed` off outside-reference classification (:174-178),
`CompilerBinding` bit as new work (:189-191), descriptor field schema (:210-214),
memo record chain (:238-241), coverage floor (:264-268), `learn_method` never-an-alias
(:323-324), `run`/`reify` purity labels (:230-231), `OpHandle` no-op justification
(:428-429), wake proxy wording (:467-468, :482-483), emit-side owed-substrate
(:369-371), `const Manifold *` wording (:461-462), `Call`-operand precision
(:261-262), conditional-last seam note (:470-471), Emitter's tables (:162-164),
`vortex_core.cpp:14` ctor cite (:370), ref-arm reordered inside the P16 step
(:478-480).

## Findings

### M3-1 — MAJOR — plan:378-385 — the loop's "each cycle promotes them into the fold" is wrong about `promote` for two mutation classes the plan itself prices

**Claims:** "session mutations live in the `ArenaManifold` overlay and append no
ops, so each cycle promotes them into the fold — `promote(Store&, parent, arena,
root)` → `Store::rebuildManifold` …" (:378-385). The durable-semantics bullet then
asserts "`patch:<n>` monotone serial off the agent cell's `d.vars` counter — never
recycled" (:432-434) as a property *of this mechanism*.

**Code:** `promote` emits content/value ops only for `isEphemeral` cells — a
shadowed base cell hits `real.emplace(arena, arena); continue;` and gets **no
content op** (`arena_manifold.cpp:1867-1870`). The link loop emits `setLink` only
for `holdsOwn` cells' posward edges, deduped against the base (:2020-2053): new
links on shadowed base cells promote; **a removed base edge is never un-emitted**
(the old op stays in the store). And `d.dims` edges are skipped in discovery
(:1766-1768) so overlay dim registrations never become ops. Net: promote carries
*ephemeral-cell mints (content + links) and link-writes on held cells* — nothing
else.

**Consequence:** two plan-priced mechanisms ride on mutation shapes promote cannot
carry. (i) The `d.vars` counter — the durable serial `patch:<n>`,
`reify:<src>:<serial>` and `validate`'s pass tag all draw (spec :2505-2509,
:2596) — as an in-place `writeValue` bump on a post-fold-1 base cell it is
*silently dropped at fold 2*: the serial rewinds and the next mint collides with a
live `patch:<n>` — the exact ABA the bullet asserts against. It survives only if
represented mint-and-repoint or rank-append, which nothing states. (ii)
`set_weight`'s junction arm — "writes its own `+d.weight` scalar" (:307-308) —
same class: an in-place value write on an existing cell, and no `d.patch` record
carries weight, so no replay arm covers it. Base-edge unlinks (`learn_plan`'s
supersede-unlink, `unlearn`'s record drop) do survive — but *by replay*, not
promote: the folded prefix resurrects them and (d) re-enacts the unlink — a
distinction the loop never draws. The ≥2-re-genesis exit would catch a serial
collision, as a red test whose cause is far from obvious.

**Fix:** one clause in the loop naming the promote-visible mutation class —
ephemeral mints (content + links) and link-writes on held cells promote;
in-place value writes and edge removals on base cells do not
(`arena_manifold.cpp:1867-1870`, `:2020-2053`) — so counters and scalar writes
must mint-and-repoint (or append), removals must ride (d) replay or take
record-shaped (tombstone) form. This is also the answer to "what happens across
the second fold": filed dims survive fine (ops replay; `makeDimension` ops at
`store.cpp:1057-1090`, `dimensionNamed` walk at `manifold.cpp:782-821`,
`rebuildManifold` self-attaches `store_` at `store.cpp:511/:532`); it is the
*unfiled and in-place* mutation shapes that do not.

### Minor findings

- **m3-1 — plan:379-381** — `promote(Store&, parent, arena, root)` leaves `root` a
  bare metavariable, and the choice is not cosmetic: the spec fixes the convention
  "the checkpoint promotes what is reachable from the agent cell" (spec :375,
  :635) — program-side cells deliberately never republish (they ride the folded
  prefix and (d) re-enacts). Name `root` = the agent cell and say that the
  state/program split *is* the root choice — anything the agent cell cannot reach
  (a patch rank anchored elsewhere, a stray state structure) drops silently:
  `promote` refuses only on budget or a bad root, never on unreached cells
  (`arena_manifold.cpp:1737-1739`).
- **m3-2 — plan:478-480** — phase 6's "per-ref-record `Int64(*handleTarget)`
  writebacks" does not restate spec :2394-2396's exemption: `+d.name`-bearing
  records — the `dim`-field member and the slot's `+d.refFor`-paired ref-capture
  record — are exempt *from that arm* because the dim arm owns them. A literal
  "per-ref-record" reading writes them twice, the second time with the promoted
  twin — the exact forbidden value source spec :2397-2398 names. One clause
  ("minus the `+d.name`-bearing records the dim arm owns").
- **m3-3 — plan:372-375, :378-385** — the residual names only *session*-minted
  dims, but (b)/(e)'s every-genesis re-mints are *genesis*-minted names landing in
  the fresh `arenaDims_`; they churn the identical orphan-per-fold path the plan
  concedes — same mechanism, wider scope. And the loop never says whether filing
  re-runs per cycle: the filing bullet's own enumeration source is
  "base∪`arenaDims_`" (:359-361), so a per-cycle re-enumeration before
  `rebuildManifold` would file session mints and shrink the residual to zero —
  while deliberately not re-filing (each `makeDimension` is a permanent rank
  append) is also defensible. Either way the cycle owes one clause: re-file per
  fold, or state that everything not on the fixed list — session *or* genesis
  minted — orphans per fold, edges along it splitting onto the dead cell.

### Nits

- **n3-1 — plan:375** — `or_parallel.cpp:181-184` → the four `ensureDimension`
  calls sit at :182-185 (off by one).
- **n3-2 — plan:380** — `arena_manifold.hpp:845-847` → the `promote` signature
  spans :845-848.

## Verified accurate (v3 carries v1.19 correctly here)

- **Filed dims across the second fold: correct.** `Store::makeDimension` writes
  `makeCell` + `setLink` ops on the `d.dims` rank (`store.cpp:1057-1090`); a fold
  replays them and `Manifold::dimensionNamed` resolves via the registry fast-map
  or the rank walk (`manifold.cpp:782-821`); `rebuildManifold` attaches `store_`
  itself (`store.cpp:511/:532`). Filed names survive every fold — the plan's
  pre-pin invariant is stated correctly on both lineages.
- **The overlay `d.dims` skip is transposed faithfully** — spec :566-567,
  :1257-1258 and :2399-2401 ground the same orphan-twin residual on
  `arena_manifold.cpp:1766`/`:2030-2033`; the plan's "honest residual" matches the
  spec's sanctioned wording.
- **The (e) three-arm split is now exact** — name-record role-split rebind,
  dim-record `Int64(DimRef)` name-arm, deferred `*handleTarget` ref arm — matches
  spec :2383-2406 including the install-set hand-off and the R7-B ordering note.
- `attach()`'s pair is fully covered by the filing list: `d.stores`
  (`arena_manifold.cpp:289`) is one of the 17 `initGenesis` dims
  (`vortex_core.cpp:66`), so only `d.store-refs` needed naming — the list is
  complete, not undercounted.
- `WorkerContext`'s two-arg ctor takes `const Manifold *` + reader
  (`worker_context.hpp:51-55`) — the plan's "not a `Store`" gloss is accurate;
  `opcodeMap_` per-context rationale holds (`vortex_vm.cpp:98-100`).
- `writeValue` covers Int64/Double/Bool/None only (`vortex_core.cpp:196-229`) —
  the ref arm's no-op-pre-P16 justification is literally true.
- Program-side grafts *not* promoting is spec-correct: "program cells are never
  republished — they ride the shared base lineage" (spec :376-377, :635-636);
  replay is the durability path for them by design.

## Counts

- BLOCKING: 0 — MAJOR: 1 — MINOR: 3 — NIT: 2

Top issues: (1) the re-genesis loop's "each cycle promotes them into the fold"
overclaims `promote`: in-place value writes on base cells (the `d.vars` serial
counter, `set_weight`'s junction arm) are silently dropped and base-edge unlinks
never un-emit — the loop owes the promote-visible mutation class, with counters
mint-and-repoint and removals on replay (M3-1); (2) `promote`'s `root` is an
unnamed metavariable where the name *is* the state/program split — the agent cell,
per spec :375/:635 (m3-1); (3) phase 6's "per-ref-record `Int64(*handleTarget)`"
omits the `+d.name`-bearing exemption, inviting a double-write with the forbidden
promoted-twin value (m3-2).

VERDICT: NOT CONVERGED — every r2 finding closed cleanly (the dim-record arm
landed exactly, scope and exception included); one new MAJOR that is a one-clause
constraint statement, three minors, all one-clause fixes.
