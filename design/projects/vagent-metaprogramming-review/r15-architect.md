# R15 — System Architect critique (v1.14)

Scope: verify the r14 dispositions — `closed`'s program-side carve-out, agent-legs restatement,
§10.2 (v) narration, `documentId_` ctor-or-load, displaced-vs-appended bound — assess the two
unapplied r14 minors for escalation, and hunt new architectural defects in the v1.14 deltas only.
Re-verified against the tree (`store.hpp:132-133` two-arg ctor decl, `:1435` `documentId_` member,
`store.cpp:123` `Store::Store()` no-id signature, `:135` `documentId_ = documentId`,
`:4450` `documentId_ = tables.documentId` on directory load; `publication.cpp:1097`
`make_unique<Store>()` in `historyFromSeal`, `:1127-1128` `Store(readerPermascroll, pub.storeId)`
in `restorePublication`; `arena_manifold.cpp:78-105` `ensureDimension` name arm, `:1756-1762`
relocation-handle targets queued into the promote walk, `:1948-1951` `isEphemeral(target) ?
real.at(target) : target`; `zigzag/dim_vector.hpp:22` `DimRef = CellRef`;
`vagent/compiler.cpp:769-772` `dimConstant` lambda on `findOrMintDimension`,
`vortex_core.hpp:207`/`vortex_core.cpp:95`).

## Verification — all r14 items landed, verbatim and correct

1. **`closed`'s program-side carve-out** — `:1171-1173` states it inline ("a `+d.name`-bearing
   program-side record's endpoint being the named dim rather than a cell and counting as
   name-resolved"); `:1859-1864` adds arm (c) in full, including the load-bearing detail that the
   slot's own `+d.refs` record *stays a REF-record* — a `+d.name` there would promote it to a
   name-record and skip the marker ceremony (verified against P16's marking rule `:2869`, which
   keys on the `+d.refs` record). The refused-class sentence `:1862-1865` and the frontier
   enumeration `:1880-1883` ("`+d.recipe` ref-capture records whose endpoints are read off their
   paired slots' content") are consistent; the re-verify walk `:1887-1889` names (b) or (c)
   correctly. Glossary `closed` row `:137` mirrors all three arms.
2. **Agent-legs (R14-A) restated** — `:2134-2143` now uses the two-root vocabulary: legs hang on
   the agent cell's own `±d.vars` rank, enter only across a genuine cross-root edge, the surviving
   case being a §5.3 `cap$N` capture slot resolving through `masterOf`/`d.clone` — accurate per
   `:822-824` (capture-slot cell is a `Clone` of the binding cell) — and a ref-capture record
   contributing no reach since its referent rides slot content. Conclusion preserved.
3. **§10.2 (v) narrated** — registered at `:2859-2863` beside (iii); narrated in the 1.13
   changelog tail `:3610-3613` with the corrected cite `store.cpp:123` (the no-id `Store()`
   signature — verified: `:123`, not the `:124` `userPermascroll_` initializer; the 1.14 entry
   `:3643-3645` records the correction itself).
4. **`documentId_` ctor-or-load** — all three sites (`:634-637`, `:1562-1564`, `:2787-2789`)
   now carry `store.hpp:133`,`:1435` (two-arg ctor decl + member) and both writers
   (`store.cpp:135` ctor, `:4450` store-tables directory load) — all four cites verified line-exact.
   "The segment apply carries ops, never identity" — correct: `applyOpsSegment` writes op records,
   `documentId_` is set only by ctor or the `:4450` load.
5. **Displaced-vs-appended bound** — `:1234-1237` bounds auditability to *declared* victims
   (`+d.name` folds back in `1..E_ops` for the join) and states the session-grafted victim case
   reads append "which is also what replay makes of it" — matching the replay rule `:1240-1248`
   verbatim; the grafted-twin caveat is echoed into the 1.13 changelog `:3600-3601`.
6. **Secondary adopted items** — recipe tuples' `+d.values`→endpoint conditional (`:1137-1141`,
   shared endpoints ride content under claimed `-d.values`; head diagram `:1052-1061` matches);
   (d)-time `*handleTarget` donor read (`:2214-2216`, `:2242-2245`, `:2225`, with the equality
   correctly bounded to the external arm `:2248-2250`); "no session resolver" mirrored at `:136`
   and `:1240`; reify copy-sentence fixed (`:1176-1178`: slots carry the same definition-site
   positions for external referents, twins for internal); 1.13-entry program-side regrouping fixed
   (`:3580-3584` — program-side now on the `+d.name`→`ensureDimension` arm, `(what promote
   already translates)` scoped to the external `*handleTarget` clause only).

## Register closure — YES, end-to-end

The seam/ownership register is architecturally closed: **spawn seed** (§10.1 adapter row
`:2774-2809` — adapter constructs/keeps the lineage `Store`, `historyFromSeal` + span-source pair,
base-injection via `WorkerContext` two-arg ctor before `initGenesis`, (i)) → **verify** (genesis
(a) comparand against the kept store's decoded band, or the `arena.base()->store()` verify arm,
(ii)/(iii)) → **checkpoint ceremony** (P16 marker + adapter `Executor` callback in `pumpOne`,
P4's approved wake (iv)) → **genesis** (a) comparand → (b) `ensureDimension` + `setOpcodeKind`
→ (c) `programRef` bind → (d) `d.patch` verb replay handing its install set to → (e) `+d.refs`
rehydration → (f) turns → **`documentId_` adoption** registered as owed surface (v). P17
deferred-not-registered. Every named seam has an owner and a status; nothing is silently assumed.

## Escalation assessment — the two unapplied r14 minors: both stay MINOR

- **Program-side (e) writeback** (below, finding 1): the mechanism (name-arm re-resolution) is
  stated and the content-carried rule leaves exactly one place the resolved `DimRef` can land —
  the slot. No competing construction exists; an implementer cannot build the wrong thing, only
  spell it out. Not MAJOR.
- **`set_weight` nameless-clause** (finding 2): there is no key material to mint under, so the
  only open question is refuse-vs-no-op — a diagnostic nicety, not a wrong-structure hazard, and
  `:1820-1828` already establishes the anonymous-clause refusal idiom an implementer would copy.
  Not MAJOR.

## New findings (v1.14 deltas only)

### 1. MINOR — genesis (e) program-side slot writeback names the resolution but not the write (§6.6 `:2223-2230`, `:2244-2245`)

(e) reads "per ref-record `Int64(*handleTarget)` — the slot's own promoted target *is* the
referent's fold position for internal and external endpoints alike — … `+d.name` → the dim name
re-resolved through `ensureDimension`'s name arm when the referent is program-side". The write
verb covers internal/external only; for the program-side arm the *resolved* `DimRef` must be
written into the slot (`Linked` binds `int64(DimRef)`, `compiler.cpp:769-772` + `:779-780` —
a persistent promoted position is not a `DimRef` and resolves no `arenaDims_` dim), but the
sentence only says the name "re-resolves". The outcome is determined — the content-carried rule
makes the slot the only place an endpoint can live — yet every sibling arm spells its write
(`int64(entryOp)`, `int64(head)`), so the asymmetry reads as a gap. Compounding mildly: the
marked slot's promote path does *work* — the relocation walk pulls the ephemeral dim anchor into
`order` (`arena_manifold.cpp:1759-1762`) so `real.at(dim)` mints a dead persistent dim-twin per
program-side slot per checkpoint — garbage the design never acknowledges. **Fix:** close (e)'s
program-side clause with "…the slot then written `Int64` of the resolved `DimRef` (the promoted
dim-anchor twin the handle names is dead weight — the dim is re-minted, not adopted)".

### 2. MINOR — `set_weight`/`weight_of` key resolution still silent on `+d.name`-less clauses (§8.1 `:2470-2474`)

The key is `clause → +d.name → qualified string`; declared handler clauses carry no `+d.name`
(`:1820-1821`, spec V15), so the resolve disengages with no stated outcome — `weight_of`'s `Opt`
disengage is named, `set_weight`'s write arm is not (its `()` signature forces a refusal idiom,
not an `Opt`). **Fix:** one clause — "a `+d.name`-less target refuses like any unresolved
focus-slot verb; only named clauses (declared plans, `patch:<n>`-labeled learned clauses) key".

### 3. MINOR — "reached only where a runtime-grown edge leads to them" is narrower than its own example (§6.6 `:2134-2139`)

The restated agent-legs sentence scopes leg-reachability to runtime-grown edges, then cites the
§5.3 `cap$N` capture slot as the surviving case — but a *declared* fragment's `cap$N`→binding-cell
`d.clone` edge is image-sealed, not runtime-grown (the fragment lives in `1..E_ops`). Legs are
reachable through image edges too; the classification outcome is unchanged. **Fix:** "reached
only where a surviving `+d` path — image-sealed capture slots or runtime-grown install/member
edges — leads to them".

## Nits

- `:1690` "capture-linked binding cells sit behind program-side anchors" — loose (pre-v1.14): a
  `d.vars` binding cell is independently agent-reachable and promotes; only the capture *slot*
  and frame-bound capturees die with the incarnation.
- `:1859-1864` arm (c) and glossary `:137` correctly refuse a `cap$N` link landing program-side —
  consistent with §5.3's value-bindings-only rename (`:799-800`), so the refused case is reachable
  only via forged/`reflect`-spliced fragments; worth no change, noted for completeness.

## Structural note

v1.14 is the first revision whose diff is *only* vocabulary alignment to a mechanism already
ruled sound — every changed sentence deletes an edge the prose still implied (capture edges,
capture links, endpoint-via-record) and replaces it with the content/map/name three-arm shape.
The architectural object (no recipe edge lands on a cell the recipe does not own) survived the
rewrite intact; nothing was relocated, only renamed. The register is closed.

VERDICT: CONVERGED
