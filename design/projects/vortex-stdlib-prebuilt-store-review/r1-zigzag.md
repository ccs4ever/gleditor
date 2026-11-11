# Round 1 critique: Xanadu / ZigZag Purist (vortex-stdlib-prebuilt-store.md)

Paths below are relative to the repo root.
Tally: 3 BLOCKING, 7 MAJOR, 6 MINOR.

## BLOCKING

### 1. (§5.2 step 3, §6.4) Raw `link()` replay onto populated ranks severs them — the merge replaces, it never appends

`Manifold::link` is a **splice**, not an append: it clears *both* displaced adjacencies
without repair (`manifold.cpp:653–695`; `unlinkRank` exists because unlink does not repair
pred→succ, `vagent/records.hpp:41–52`; `insertIntoRank` is the no-sever primitive,
`manifold.cpp:739–753`). Under §6.4, local `home —d.stdlib→ std:math` with
`map[localMath] = existingMath` replays to `link(arenaHome, d.stdlib, POS, existingMath)`:
the rank head loses its negward edge and `existingMath`'s predecessor loses its posward —
**every module before `std:math` is orphaned**. The merged `+d.vars` head edge replaces
the existing symbol rank. A *new* local module severs the head identically. And on
registry edges: for a same-build virgin context the replay is idempotent by accident of
identical mint order; on divergent paths (older image, document-bound base, local images)
each divergent edge orphans two cells off `d.dims` → `findDimension` misses → duplicate
same-named dim minted.

**Fix:** distinguish *internal* edges (both endpoints graft-minted — safe raw replay) from
*rank-membership* edges onto shared anchors (`home`'s `d.stdlib`/`d.vars`/`d.dims`, a merged
module's `d.vars`): (a) never replay an inbound membership edge whose mapped target
pre-exists; (b) new members append at `rankTail`/`insertIntoRank` — the builders' own
discipline; (c) `d.dims` edges are never replayed — dimension identity is by name.

### 2. (§3.1, P28) The `"ops"` manifest's Int64 positions are dead refs after graft — `registerProgramImage` assumes the overlay model

The consumer reads `asInt64` → `CellRef` (`vortex_vm.cpp:142–154`) — built for vagent's
*overlay* model where the image is the arena's base (`genesis.hpp:9–12`, `genesis.cpp:538`,
`or_parallel.cpp:345–363`; `finalizeProgramImage` emits `Int64(op)` members,
`fragment.cpp:438–459`). Under clone-graft, members misresolve — refusal in a virgin
arena, silent misregistration of document cells in a bound one (`registerProgramOps`
guards only `contains` + `isRegistrableOpcode`, `vortex_vm.cpp:97–119`).

**Fix:** (a) OpHandle-valued members + `handleTarget` arm — the zz-pure option, a position
is a *reference* spelled as a ref (`ops.hpp:159`), and composes with §6.4's two-image
grafts; or (b) keep Int64, build the translated enumeration via the graft map, call
`registerProgramOps` directly — never `registerProgramImage` on the grafted record.

### 3. (§2.3.1, P26) The Int64-position leak is systemic — `opref`/`contCell`/dim operands are exported dead, not just wing slots

`opref`/`contCell` mint `makeScalarCell(Int64(target))` — "a Jump/Branch/Choice target
travels as an operand cell's *value*" (`vortex_stdlib.cpp:3494–3510`, ~30 bind sites
`:3564–4152`); dim refs travel as `lit(Int64(dims().scan))` (`:4056–4059`); the decode is
the pervasive operand convention (`toInt` in Jump/Branch/Call `vortex_vm.cpp:1665–1711`,
`resolveCell`/`toInt` for cell/dim operands `:768–829,:53–70`). Under P26's slot-only fix
they die verbatim through promote (`arena_manifold.cpp:1969–1971`).

**Fix:** rescope P26 to *every scalar whose value names a cell or dim*: (a) marked-Int64
convention — `+d.mesh.relocation → "cell"` (`records.hpp:63–72`), harvest referents as
promote roots, restate each marked Int64 through the twin map, the discipline
`lineage.cpp:189–249` already runs — zero new decode sites; or (b) OpHandle values +
`handleTarget` arms at every position-decode site. House rule "cells and dimensions over
bit-packing": an Int64 secretly a pointer is foreign serialization inside a cell.

## MAJOR

### 1. (P29) The `+d.params` signature record's encoding is unspecified — one wrong choice re-creates the dead-ref bug

`RoutineBinding{inputParams, outputParams}` holds `CellRef`s (`vortex_stdlib.hpp:578–582`)
— how does each member name its parameter cell? Int64 reintroduces the dead-ref bug; a
direct link is 2-degree-safe only while each param is single-owned. "Two slot cells per
param" is unexplained arithmetic.

**Fix:** OpHandle-valued member cells on a `+d.params` chain (consistent with P26,
translated by the deferred pass); loader rebuilds by key (entry op = symbol's `+d.values`
target). State the in/out boundary (count cell or two ranks).

### 2. (§4) Record order must come from manifold walks, not `unordered_map` iteration

`routineBindings_`/`opcodeMap_` are `unordered_map`s — iterating them to emit records is
nondeterministic across runs → the byte-identical gate fails.

**Fix:** all emitted records walk the manifold (`Manifold::cells()` birth order,
`+d.vars` ranks); consult maps by key only.

### 3. (§3.1 vs P28) Manifest anchor contradiction: `home +d.vars` vs "the image's stdlib root"

`stdlibRoot` is the *first module cell* (`vortex_stdlib.cpp:5502–5505`) — on a module's
`+d.vars` the records join the symbol table (`symbolsInModule` returns them;
`resolve("std:memoize/ops")` hops `+d.values` into the manifest). `registerProgramImage`
takes the first `"ops"`-texted member of `imageRecord +d.vars` (`vortex_vm.cpp:128–137`).

**Fix:** `home +d.vars` (lands on arena home via step 1's mapping; keeps symbol tables
clean). Corollaries: (a) unscoped record matches any `varsRecord` owner query — fine, say
it; (b) under §6.4 two images each carry `"ops"` — first-match stops early. Specify the
registration walks **all** `"ops"`-named records or unions them.

### 4. (P33) `rebindCaches()` cannot recover `goalHalt_`/`goalSentinel_` — no symbol path exists

Only `goalFail_` is exported (`vortex_stdlib.cpp:4010`); `goalHalt_`/`goalSentinel_` are
used as default suspension target (`:2461–2488`) and `cont` prefills (`:3508–3510`). After
graft they're `noCell` — suspension silently deadlocks/misfires.

**Fix:** export them (`std:goal/halt`, `std:goal/sentinel`) or record on `stdlib.image`;
P33's enumeration names every cached ref explicitly.

### 5. (§5.2, §7.2) Grafted cells are unattributed copies — name the transclusion compromise

`makeCell(text)` copies bytes: no `d.clone` arm, no provenance edge, no span into the
shipped permascroll — none is expressible across index spaces in v1. The costs: (a)
grafted text is a *copy*, not a transclusion of the shipped span — attribution severed;
(b) a stdlib cell promoted into a user document spools *fresh primedia* — the quotation
relationship and transcopyright provenance are gone (`arena_manifold.cpp:1993–1995`);
(c) `std:hypertime` can answer only on the image fold — in-context stdlib cells are
history-less by construction.

**Fix:** three sentences — v1 graft is copying not cloning; the v2 shared base restores
single-instance attributable cells; quote-into-user-document attribution loss is a named
v1 price.

### 6. (§3.1, §6.3) `stdlib.image` field encodings unspecified — `imageEpoch` risks bit-packing, `generatorHead` can't be Int64

"the `OpcodeKind` and `StructureVerb` counts" reads as one member naming two counts;
`generatorHead` is a `MicroversionId` — a segmented name (`microversion.hpp:63+`), not an
integer.

**Fix:** two Int64 members (`imageOpcodeCount`, `imageVerbCount`); `generatorHead` as a
text cell holding `ver.str()` (round-trips through `MicroversionId::parse`,
`foreign_op_handle.cpp:74` precedent) — or OpHandle to the head op.

### 7. (§4, §6.1) `--extend` diff semantics: retraction is unspecified

A diff that *removes* a symbol or repoints a wing needs `SetLink`-clears/`SetValue`
restates — promote emits clears only under `ShadowCommit::Clears`
(`arena_manifold.cpp:2066–2095`); purely additive diffing can never remove.

**Fix:** state which change classes `--extend` supports, how retraction is emitted, and
the named refusal for what it can't express (e.g. a removed dimension → refuse).

## MINOR

1. Stale citations — `exportStandardLibraryToStore` at `:5500–5517`, verbatim copy at
   `:5534–5541`, `:5662` lands inside `kXuzzActions` (file live-edited; re-pin at freeze).
2. (P27) Registry rank ≈ 21 cells (19 dims + `d.dims` + relocation).
3. (P26) Mark cost mispriced — the mark is `slot +d.mesh.relocation→` a `"cell"`-texted
   cell: ~2 ops/slot; *if* a shared marker is impossible (fan-in on its `-d.mesh.relocation`
   evicts all but the last backlink) — verify which side `isCellRelocationHandle` reads.
4. Export reachable-set boundary unexamined — promote's BFS walks neg edges too
   (`arena_manifold.cpp:1783–1797`), reaching `home`'s other ranks (`+d.cursors`,
   `+d.pinning-cursors`, `+d.vars`); clean only on a virgin context. State the constraint.
5. Latent single-owner invariants — `sym +d.values→entry`, `opcode ±d.contract→first`,
   `param +d.spin→head` assume single-owned targets; aliased symbol/condition silently
   evicts (the `varsRecord` comment states the rule for `+d.owner`, `records.hpp:93–96`).
   One sentence so generator authors keep it true.
6. (§5.2) Map-miss discipline — a link whose target lies outside the cloned set: refuse
   with a named reason when inside the intended subgraph, skip when outside by design.

## The purist question — assessed

The chosen object is honest: an OSMIC spool beside its permascroll is foldable, diffable
(`xudu-dump`), hypertime-addressable — *not* an opaque binary asset. §3.2's refusal of
`encodeArenaPrefix` for distribution is exactly right; §6's append-only lineage is
faithful non-destructive versioning. What separates v1 from the right end-state — the
stdlib as a *published, authored, transcludable xanadoc* — is two named gaps:
(a) **authorship** — the store carries no publisher/author scroll; U-X5's deferral is
correct but the doc should state authorship is the *goal* and the unsigned asset the
preprint; (b) **transclusion-preserving consumption** — v1's copies are unattributed
(MAJOR 5) and v2's shared base restores it. v1 does not foreclose the direction — provided
findings 1–3 are fixed so the image actually *works* as a xanadoc when read back.

VERDICT: NOT CONVERGED
