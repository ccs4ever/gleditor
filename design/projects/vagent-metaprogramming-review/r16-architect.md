# R16 — System Architect critique (v1.15)

Scope: verify the three assigned minors — (e)'s program-side writeback `Int64(resolved DimRef)`,
the agent-legs "runtime-grown outbound edge" restatement, `set_weight`'s `+d.name`-less refusal —
then the architectural close-out: whether (d) verb replay → (e) `+d.refs` sweep → copies'
regenerated records now holds end-to-end with every endpoint class (external int64, internal map,
program-side dim) having a defined write path. Re-verified against the tree
(`arena_manifold.cpp:78-105` `ensureDimension` name arm, `:91-103` ephemeral rank append,
`:1759-1763` relocation targets queued, `:1765-1768` `d.dims` edge skip, `:1867-1869` non-ephemeral
identity map, `:1948-1951` `real.at(target)` handle write, `:2020-2053` link emission gated on
`real.find(edge.dim)`/`real.find(edge.pos)`, `:2091-2095` `arenaDims_` snapshot arm;
`manifold.cpp:766-821` `dimensions()` = `d.dims` rank walk + registry fast-path only;
`vortex_core.cpp:16-27` `mintNamedDimension`, `:95-98` `findOrMintDimension`, `:354-369` `wingOf`
(`linked(opcode, dims_.grab, POS)` at `:366`, `inputsOf` at `:373`);
`vortex_vm.cpp:816-825` `Clone` `+d.clone` append, `:817-822` posward tail;
`arena_manifold.cpp:1009-1012` `cloneMaster` = `rankEnd` NEG-ward; `compiler.cpp:769-772`
`dimConstant` = `int64(findOrMintDimension)`, `:779-780` `Linked` binds it;
`dimension_registry.cpp:194-216` `getOrCreate` → `store.makeDimension`;
`vql/compiler.cpp:1128-1141` "1. Replicate dimensions" precedent;
`mesh_value.cpp:404-406` importValue's own `d.dims` skip; `mesh_actor.cpp:271` fresh
`WorkerContext`/arena on resume → fresh `arenaDims_`).

## Verification — all three minors landed

1. **(e) program-side writeback** — `:2259-2264` now names the write: "a `+d.name`-bearing dim
   record instead writes `Int64` of the `DimRef` `ensureDimension`'s name arm re-resolves
   (`arena_manifold.cpp:78-105`) — `Linked` binds a `DimRef`, never a promoted position, and
   `*handleTarget` names the dim-twin a session-minted dim leaves orphaned off the folded `d.dims`
   rank (`arena_manifold.cpp:1764-1768`), dead weight beside the re-minted canonical cell; the write
   arm keys on the record's `+d.name`, never the sideness test". The dead-twin acknowledgment is
   the fix I asked for, and (d)'s counterpart at `:2245-2247` scopes "never the endpoint value"
   with the `+d.name`-bearing exception ("the record's `+d.name` *is* the endpoint source and the
   slot takes `Int64` of the resolved `DimRef`, not the donor's promoted twin"). §6.1's enumeration
   `:1179-1185` carries the same arm. Correct per `compiler.cpp:769-772` (`Linked`'s dim operand is
   `int64(DimRef)`).
2. **"Runtime-grown outbound edge" restated** — `:2157-2171` now says plainly: legs "enter this
   walk only where a runtime-grown outbound edge leads to them — under the sealed image they are
   simply unreached", the capture's only edge is the binding cell's `+d.clone →` the `cap$N` slot
   — image-sealed for a declared fragment, state→program on a dim the `+d` vocabulary does not
   list (`:1524-1526` confirmed `d.clone` absent) — and `masterOf` is the operand decode
   (`cloneMaster` = `rankEnd` NEG-ward, verified `:1009-1012`; the append verified
   `vortex_vm.cpp:816-825`), "never a traversal". The R15-A ruling applied verbatim; my r15 nit is
   moot — the `cap$N` edge is now named image-sealed, not runtime-grown. Changelog echoes at
   `:3641-3647`, `:3659-3664` rewritten to match, and the retro-edits confessed `:3730-3734`.
3. **`set_weight` refusal** — `:2508-2509`: "a `+d.name`-less target, a declared handler clause
   (spec V15), resolves no key and is refused like any unresolved focus-slot verb: only named
   clauses key — declared plans, `patch:<n>`-labelled learned clauses"; table row `:1125` glosses
   "the keyed `d.vars` record minted on first write, a `+d.name`-less clause refused". `weight_of`'s
   `Opt` disengage is scoped to the named-no-record case — refusal/disengage asymmetry is coherent
   (no key = cannot look; key + no record = none).

## Architectural close-out — the ordering holds, on an unstated dim-persistence precondition

The (d)/(e) shape is now self-consistent: (d) re-enacts each patch's *verb* (re-attached rank
re-match, replay-keyed alias, `patch:<n>` label re-mint verbatim off the record — no serial
consumed) and hands (e) the install set (`:2248-2250`), because copies hang off install sites no
head-only walk reaches; (e) sweeps heads' + copies' records last — coverage (copy name-records,
`:2288-2290`) and ordering (post-installation symbols, `:2290-2294`) both justified. Endpoint
classes each have a defined write: external/state-side → `Int64(*handleTarget)` (verbatim bits
equal the donor's, `:2283-2284`); internal → old→new map for copies, `*handleTarget` for donors
(`:2256-2258`, `:2281`); program-side/dim → `Int64(resolved DimRef)` both at (d) on the copy and
(e) on the donor; name-records → role-split rebind (`int64(entryOp)`/`int64(head)`, `:2264-2265`);
copy ref slots arrive pre-rehydrated and kind-guard the (e) arm to a no-op (`:2285-2288`).
**Conditionally correct — see finding 1: every inventory read in that ordering goes through an
`ensureDimension`-resolved dim, and the doc's stated mint path cannot make those dims re-resolve.**

## New findings (v1.15 deltas only)

### 1. MAJOR — session-minted dims promote to *off-rank* twins; `ensureDimension` re-mints, and every folded edge keyed on them is unreachable by name — the (b)/(e) "re-binds" claim is contradicted by v1.15's own new sentence (§6.6 `:2184-2185`, `:1535-1538` vs `:2261-2263`; §10.1 `:2849-2853`)

v1.15's (e) now states that a session-minted dim "leaves orphaned off the folded `d.dims` rank …
dead weight beside the re-minted canonical cell" (`:2261-2263`) — while genesis (b) still asserts
"dims resolve from base/cache before any mint (`arena_manifold.cpp:78-105`)" (`:2184-2185`) and
`:1536-1538` still asserts "a promoted dim cell re-binds rather than re-mints". Both cannot hold:
the orphan exists precisely *because* the re-bind fails. The code says the orphan is the rule, not
the exception, for any dim minted via `ArenaManifold::ensureDimension`/`mintNamedDimension`:

- `promote`'s discovery skips `d.dims` edges outright (`arena_manifold.cpp:1765-1768`), so the
  `d.dims` dim cell never enters `order`/`real`; at link emission, an edge whose `edge.dim` is
  absent from `real` is dropped (`:2030-2033`) — and the rank link's own ends (`home`/rank-tail,
  base-owned) fail `from.holdsOwn` (`:2021`). **No `d.dims` rank link can ever be emitted by
  `promote`** — the twin is minted (`:1863-1967`) and edges fold keyed on it, but it is never
  filed on the rank.
- `Manifold::dimensionNamed` resolves via `DimensionRegistry` + the `d.dims` *rank walk* only
  (`manifold.cpp:766-821`); `ArenaManifold::dimensionNamed` adds `arenaDims_`
  (`arena_manifold.cpp:56-75`) — a per-arena map, empty after `resume` rebuilds a fresh
  `WorkerContext` (`mesh_actor.cpp:271`). An off-rank twin is unreachable by name forever.
- The engine knows this: `vql/compiler.cpp:1128-1141` runs a "1. Replicate dimensions" pass —
  every `d.*`-texted arena cell is filed through `DimensionRegistry::getOrCreate` →
  `store.makeDimension` *before* edges mint — the persistent-rank path the doc never names.

Consequence, end-to-end: every edge written in-session on an adapter-minted dim folds keyed on an
off-rank twin; next genesis `ensureDimension` re-mints, and **all name-keyed reads of folded
structure return empty**: `d.patch` ((d)'s replay enumeration), `d.code` (registered heads),
`d.refs`/`d.ref`/`d.recipe`/`d.refFor` ((e)'s sweep and the slot pairing), `d.entry` (`run`'s
deref), `d.patch-head`, `d.weight`/`d.synapse` (§8's junctions), `d.validate`,
`d.mesh.relocation`, the agent-anchored V10 ranks (`d.belief`/`d.intention`/…). The (d)/(e)
ordering is correct *as a sequence* but enumerates empty inventories — the doc's own acceptance
fixture "learn → checkpoint → resume → learn → checkpoint" (`:2106-2107`) dies at the first
post-checkpoint genesis: the second `learn` finds no patch rank to replay, and the donor heads'
marked slots stay `OpHandle`-kind. Nor is "dead weight" the right diagnosis: the twin is what the
folded edges actually key on — the *re-mint* is what name-resolution returns — a split-identity
dim, one orphan accumulating per session-minted dim per checkpoint. `d.vars`/`d.name`/`d.grab`/
`d.spin` escape only if the sealed image's own rank carries them (which `:621-623` presumes — the
same off-rank twin question for the artifact's emission path, likewise unstated).

**Fix:** state the dim-provisioning precondition once, at (b) and §10.1's bind list: every named
dim the apparatus reads must be a *persistent-rank* cell — the adapter files each name through
`DimensionRegistry::getOrCreate`/`Store::makeDimension` on the kept lineage `Store` at spawn-seed
(and the artifact's image emission must do the same — the `vql/compiler.cpp:1128-1141` precedent;
store-level ops, no core change, consistent with "P16 is adapter ceremony"), so `ensureDimension`
only ever re-binds. Then correct `:1537` ("re-binds" holds only for rank-resident dims) and keep
(e)'s orphan sentence scoped to the slot-value arm — where re-mint genuinely is harmless because
the slot is rewritten each genesis. If the intended answer is instead "records are re-resolvable
positionally", that must be said — nothing in the current text enumerates by position.

### 2. MINOR — (d) still calls the `+d.name` mark "the program-side mark" after arm (c) dropped the sideness qualifier (§6.6 `:2242-2243`)

"the record supplying the pairing (`+d.refFor`) and the program-side mark (`+d.name`)" — v1.15's
own disposition dropped "program-side" from arm (c) (`:1876`) because session-minted dims are
ephemeral, and (e) says the arm keys on `+d.name` "never the sideness test" (`:2263-2264`). The
(d) label lags its own rule. **Fix:** "and the dim mark (`+d.name`)".

### 3. MINOR — "`Linked` binds a `DimRef`, never a promoted position" overstates; the accumulation is unbounded (§6.6 `:2260-2263`)

For an image-resident dim referent — a `dimConstant` naming an artifact `d.*` dim — the resolved
`DimRef` *is* a persistent (folded-band) position; "never a promoted position" is true only of the
*session-minted* twin. The intended contrast is with `*handleTarget` on the marked cell.
Compounding: one orphan twin mints per session-minted dim in use per checkpoint — a monotonic
dead-cell accretion the design acknowledges ("dead weight") without bounding. **Fix:** "`Linked`
binds a `DimRef`, never the promoted dim-twin `*handleTarget` names — each checkpoint leaves one
such orphan per session-minted dim in use".

## Nits

- `:2250` — the install set handed to (e) is described as "the `+d.refs` records `reify`
  regenerated on it"; the copy's `+d.recipe` ref-capture records (where the `+d.name` dim arm
  lives) ride along only implicitly, via each slot's `-d.refFor` backlink. Recoverable, but the
  inventory wording could name both chains once.
- Finding 1's shadow: `importValue` mints *fresh unnamed arena cells* for received dims
  (`mesh_value.cpp:404-406`, `:481-492`, `:505-508`) — the same off-rank dim identity at the
  transport layer; consistent with this being an unowned cross-cutting rule, not a v1.15-local
  slip.

## Structural note

v1.15 is the revision where the doc first *admits* the dim-twin mechanism in-line — and the
admission instantly contradicts the standing claim it was supposed to sit beside. The close-out
answer: the (d)/(e) ordering holds and every endpoint class has a defined write path, **if** —
and only if — the record/rank dims re-resolve after the fold; the stated mechanism
(`ensureDimension` find-or-mint) does not deliver that for any dim the sealed image's rank does
not already carry. One stated step (persistent-rank filing of the V10/adapter vocabulary at
spawn-seed and image-seal) closes it without touching the register.

VERDICT: NOT CONVERGED
