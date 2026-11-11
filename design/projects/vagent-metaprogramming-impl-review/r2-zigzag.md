# R2 (impl-plan) — ZigZag/promotion-semantics critique

Lane: cell-graph topology — dimension discipline, filing/promotion ordering, link
eviction, genesis sequencing. Target: `design/vagent-metaprogramming-implementation.md`
v2 (the repartitioned plan). Reference: `design/vagent-metaprogramming.md` v1.19.

Scope and method unchanged from r1. Every load-bearing cite re-verified against the
tree this round: `dimension_registry.cpp:194-217` (`getOrCreate` move-assigns its
`Manifold&` at :212 — the rebuilt fold *is* the injected base),
`vortex_core.cpp:35`/`:44` (`home`/`d.dims` genesis pair) and `:50-69` (exactly 17
named dim mints, `d.grab`→`d.role` — "nineteen cells all told" is correct),
`compiler.cpp:769-772` (`dimConstant`→`findOrMintDimension`), `vortex_vm.cpp:98-100`
(`setOpcodeKind`), `arena_manifold.cpp:34-46` (`dimensions()` base∪`arenaDims_`),
`mesh_actor.cpp:473-477` (the `executor_` invocation inside `pumpOne`),
`Makefile:643/649/688-690/692/697-699/1348-1350` (kernel glob, test glob, explicit
frontend list, frontend-test list, `vagent_runtime_test` objects + the
`vagent/`-dir filter-out, the three-binary `make test`). All land.

## R1 closure verification — all items closed

| r1 item | v2 disposition |
| ------- | -------------- |
| **B-1** (genesis (d)/(e) inverted + wrong letters) | **Closed.** (d) `d.patch` replay in ordinal order (:336), (e) `+d.refs` rehydration "last pass" *because* rebinds must see post-installation symbols, R7-B cited (:345-347), (f) correctly "turns" and out of scope (:324-325). The (d)→(e) install-set hand-off is named (:343-344), and the phase-5 exit adds the rebind-against-patch-installed-symbol case (:362-364). |
| **M1** (bind set dropped from filing vocabulary; never scheduled) | **Closed.** Phase 3 supplies the adapter bind set explicitly — program root, `agent_cell()`, all twelve apparatus dims, `validate` constants, V10 mechanism on the no-marks path (:216-223); phase 5's filing fixed-list leads with "the phase-3 bind set above" (:312); (c) binds it at genesis (:334-335); both orphan-twin exits extend to it (:363-365, :402-403). |
| **M2** (pin unplaced; emit-side filing ownerless) | **Closed.** The pin is an explicit step — phase-6 seam (i) "the pin on the spawn lineage" consuming the fold phase 5's filing returned, (ii) "the pin on the resume lineage" (:376-380). The full order is stated in the phase-5 goal (:297-300) and re-stated on spawn + resume in phase 6 (:371-373); the emit lineage gets the twin rule — file before the compile arena's `initGenesis` (:318-320). See m2-5 for a residual ambiguity note. |
| **M3** (`programRef` bind record / `E_ops` + enumeration unproduced) | **Closed.** Phase 3 owns the record's emission half — `E_ops` + recorded instruction enumeration written at emission, consumers named (P15, (a), (b), `learn`'s sideness bound) (:198-202). |
| **M4** (no genesis hook invoking (b)–(f)) | **Closed.** Phase 5 names "the genesis (a)–(e) driver — §1's priced `std:agent` genesis hook", harness-invoked there, wired to real spawn/resume in phase 6 step 2 (:323-325, :385-387). |
| **M5** (`code_path.cpp`/`expand.cpp` missing from `VAGENT_FRONTEND_SRCS`) | **Closed.** Both join the explicit list in phase 1, the phase that creates them (:105-106, :113-115); phase 0 keeps the per-phase rule (:82-84). |
| m1 (`learn_method` ordering) | **Closed** — "unlink-then-install — redefinition-as-replacement rides `graft_method`'s own verb" (:283-284). |
| m2 (per-ref-cell marker) | **Closed** — one marker per ref cell, sharing impossible, `link`-eviction + `mesh_evaluator.cpp:735` cited (:389-391). |
| m3 (`patch:<n>` re-mint) | **Closed** — verbatim re-mint, never re-link, second-genesis eviction named (:340-343); exit requires ≥2 re-geneses (:360-361). |
| m4 (`globalKeyOf` arm set) | **Closed** — `breakMarkerKey` / `scrollKey` / fail-outright arm set minus `localScroll` (:327-330). |
| m5 (`setOpcodeKind` at spawn too) | **Closed** — "every fresh-context image bind (spawn *and* resume)", both bullets (:258-260, :331-333). |
| m6 (marking/ceremony split wording) | **Closed** — "marker-edge mint — the record schema is phase-3's emission contract", extended to `reify`'s regenerated records (:388-392). |
| m7 (`staticFuel`) | **Closed** (:167-169). |
| m8 (19-dims miscount) | **Closed** — 17 system dims + the genesis pair, "nineteen cells all told" (:313-315). |
| m9 (P16 no-interleave window) | **Closed** (:394-396). |
| m10 (coverage residual) | **Closed** — names whose edges can fold; the honest `d.op-label`/`or_parallel` residual (:320-322). |
| n1 (v1.18 cite) | **Closed** (:3). |
| n2 (P17 gloss) | **Closed** — drain idiom + no-marks minting named (:426-428). |

New-work hunt results: filing vocabulary is complete (bind set + 17 system dims +
genesis pair + V10 thirteen + `d.store-refs` + `dimConstant` mints, :312-318); the
filed-vs-bound distinction is present but left as an implementer TODO (m2-3); the
ordering chain is stated for emit (compressed) and spawn/resume (full); orphan-twin
tests present at both phase-5 and phase-6 exits. One **new** major and follow-ups:

## Findings

### M2-1 — MAJOR — plan:345-350 — genesis (e)'s *dim-record* arm is unassigned; `Int64(DimRef)` appears nowhere in the document

**Claims:** (e) "`+d.refs` rehydration — last pass, name-record arm here — the
role-split rebind (`call`-role `int64(entryOp)` vs `run`-role `int64(head)`) … The
ref-record arm — per-record `Int64(*handleTarget)` writebacks — stays phase 6 with
P16" (:345-350). Phase 6 repeats "the deferred ref-record arm … `Int64(*handleTarget)`
writebacks" (:386-387).

**Spec:** (e) has **three** per-record write classes, not two (:2385-2404): per
ref-record `Int64(*handleTarget)`; **per `+d.name`-bearing `+d.recipe` record — the
ref-capture dim record *and* the `dim`-field member alike — `Int64` of the `DimRef`
`ensureDimension`'s name arm re-resolves**, into the `+d.refFor`-paired slot or the
member cell itself (:2394-2397); per name-record the role-split rebind. The plan names
arms 1 and 3; arm 2 falls between them — it is not a `*handleTarget` writeback (spec is
explicit: "`Linked` binds a `DimRef`, never the promoted twin `*handleTarget` names",
:2397-2398) and it is not the name-record rebind. It is also **not promote-gated**: it
re-binds filed names to their canonical cells and re-mints only unfiled ones
(:2401-2402) — the same filed-name invariant the plan's own orphan-twin exits assert —
so it is owed at *every* genesis, spawn included, i.e. it belongs in phase 5's driver,
not deferred with P16. Two failure readings: an implementer who lumps it under "the
ref-record arm" ships it in phase 6 with the wrong value source (`*handleTarget` — the
exact bug :2397-2398 warns against); one who reads the (e) bullet literally ships
genesis with no dim-writeback at all, and `dim`-field members keep emission-time
`DimRef` integers across every fold. The same `+d.name`-bearing exception belongs in
(d)'s slot-derivation summary (:2369-2372 — "the record's `+d.name` *is* the endpoint
source and the slot takes `Int64` of the resolved `DimRef`"), which the plan's (d)
bullet also omits.

**Fix:** name the third arm inside phase-5 (e) — "`+d.name`-bearing `+d.recipe` records
(ref-capture dim records and `dim`-field members) write `Int64(DimRef)` re-resolved
through `ensureDimension`'s name arm — the filed-cell rebind, never `*handleTarget`;
runs every genesis" — scope phase-6's deferred arm to the `*handleTarget` writes alone,
and add the `+d.name`-exception clause to (d)'s endpoint derivation.

### Minor findings

- **m2-1 — plan:331-333** — (b)'s "ensureDimension find-or-mint + `setOpcodeKind`
  re-registration **over the phase-3 recorded enumeration**" scopes both verbs to the
  instruction enumeration. Only `setOpcodeKind` runs over it (op cells);
  `ensureDimension` resolves dim *names* (filed → canonical cell, unfiled → re-mint,
  spec :2294-2299, :2401-2402). Split the arms so the enumeration binds to
  `setOpcodeKind` alone.
- **m2-2 — plan:336-344** — (d) omits "replay does not re-record": replayed grafts
  mint no `d.patch` cells and consume no `patch:<n>` serial (spec :2346-2347), though
  each install copy's `reify:<src>:<serial>` label still mints off the same counter.
  "Verb re-enactment" invites the reading that replay invokes the session `learn_*`
  verb — which appends a record and mints a serial, growing `d.patch` every genesis.
  One clause.
- **m2-3 — plan:312-313, :222-223** — the filing bullet's bind-set gloss "(the
  apparatus dims and the `validate` cells)" narrows spec's "the bind set above"
  (:3017) inconsistently: program root and agent cell are dropped (correctly — bound
  at (c), not fileable names) while the `validate` constants are kept (same category —
  bound cells, and `makeDimension` files dim *names*). Relatedly, phase 3's "state
  which names are minted at bind vs. filed at filing" asks the implementer to derive
  what the plan can state outright: the bind set's twelve dim names file; its bound
  non-dim members (root, agent cell, `validate` cells) bind at (c) and never file.
- **m2-4 — plan:197 vs :438-440** — `fragment.{hpp,cpp}` is "kernel-glob TU under
  `apps/common/xanadu/`" (Makefile:643 glob), but the `vagent_runtime_test` filter-out
  (Makefile:697-699) removes only `apps/common/xanadu/vagent/` objects: a root-level
  `fragment.cpp` *is* linked by `vagent_runtime_test` — contradicting "cannot link
  `fragment`/`compiler`/`expand` at all" and link-fatal if it touches filtered
  vagent-frontend symbols. Name the directory (`vagent/` for filter coverage, or root
  + a kernel-self-containment constraint).
- **m2-5 — plan:297-300** — "The ordering discipline on **both** lineages" is
  ambiguous: the filing bullet means emit + spawn (:318-320), phase 6 means spawn +
  resume (:371-373). And the five-step chain is spawn-shaped — the emit lineage has no
  pin step (its freeze is the compile arena's `initGenesis` itself); say which
  lineages take the long form and that emit's rule is the compressed "file before
  first resolution". Also worth one clause in (a): the comparand is *pairwise* —
  adopted lineage band vs received band (spec :2228-2232) — which phase-5's single-
  store fixture degenerates; only phase 6's permuted-lineage test exercises the
  two-store shape.

### Nits

- **n2-1 — plan:362** — "rebind-against-patch-installed- symbol case": stray hyphen
  artifact.
- **n2-2 — plan:339** — "`learn_where`/`learn_body`'s descriptor re-point": the verbs
  re-point a where/body *attachment* (head edge) on the target clause, not a
  "descriptor"; spec's own phrasing is just "re-enacts each patch's verb".
- **n2-3 — plan:327** — (a)'s "canonical post-fold `OpRecord` fields verbatim": spec's
  precision is "every *non-scroll* field verbatim", post-*remap* (:2232-2235);
  harmless given the scroll-by-key clause follows, but the exact wording forecloses a
  raw-`ScrollId` reading.

## Verified accurate (v2 carries v1.19 correctly here)

- Genesis letters and order — (a) lineage bind/comparand → (b) find-or-mint +
  re-register → (c) `programRef`→root → (d) ordinal `d.patch` replay → (e) `+d.refs`
  last → (f) turns — now exactly §6.6, including the (d)→(e) install-set hand-off
  (:2373-2380) and the R7-B ordering rationale (:2435-2439).
- Phase-5/6 transport boundary: everything a local `Store` hosts (seed, span-source
  bind, filing, driver) vs wire-crossing seams — matches the spec's own seam split
  (§10.1's spawn-seed vs §10.2's (i)–(v)).
- The pinned ordering invariant and its `getOrCreate` mechanism — rebuilt fold is the
  injected base, post-pin filing lands outside it (:3031-3039) — correctly stated on
  the spawn side and correctly compressed to "file before first resolution" on emit.
- `patch:<n>` verbatim re-mint + ≥2-re-genesis exit — the label-eviction mechanics
  (:2349-2357) correctly transposed.
- `call`/`run` role-split rebind; ref-arm deferral justified by the kind-guard no-op
  (:2405-2417, :2208-2209); P16 per-cell marker rationale (`link` single `-d`
  occupancy) and the no-interleave window (:2171-2179); `historyFromSeal` +
  `setContentSource`/`bindPublishedLocalScroll` pairing; comparand's `globalKeyOf`
  arm set; the 17+2=19 count — all verified against spec and tree.

## Counts

- BLOCKING: 0 — MAJOR: 1 — MINOR: 5 — NIT: 3

Top issues: (1) genesis (e)'s `Int64(DimRef)` dim-record arm is unassigned — owed at
every genesis, neither the deferred `*handleTarget` arm nor the name-record rebind,
and the natural mis-grouping lands it in phase 6 writing the wrong value
(M2-1); (2) (b)'s "over the recorded enumeration" wrongly scopes `ensureDimension`
to the op enumeration (m2-1); (3) (d)'s "verb re-enactment" omits the no-re-record
rule — replayed grafts mint no `d.patch` cells and consume no `patch:<n>` serial
(m2-2).

VERDICT: NOT CONVERGED — all six r1 findings and every minor/nit closed; one new
MAJOR (the unassigned dim arm) plus five minors, all one-clause fixes.
