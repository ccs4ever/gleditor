# Round 1 disposition — vortex-stdlib-prebuilt-store

Reviewer tally: writer 2B/2M/13m, vortex 3B/5M/8m, codebase 2B/6M/14m,
architect 3B/6M/7m, ergo 4B/7M/5m, zigzag 3B/7M/6m. Zero converged.

Findings are clustered by defect; critic references are `critic#item`. Every
BLOCKING/MAJOR was re-verified by the orchestrator against the worktree before
disposition (link() splice at `manifold.cpp:667–691`, `registerProgramImage` holder
contract at `vortex_vm.cpp:121–155`, `openActiveSegment` `O_RDWR|O_CREAT` at
`segmented_ops_spool.cpp:603`, `opref`/`contCell` at `vortex_stdlib.cpp:3497–3510`,
goal exports at `:3487–3489,4010`, publication-state secret at
`user_permascroll.cpp:156–158`, `zzResolveDim` wrong-rank at `:5787–5802`).

## Cluster 1 — Ref-valued scalars live in the wrong index space
(covers vortex B1,B2; ergo B1; zigzag B2,B3; architect B2,M2; codebase M1;
vortex M1/M5; zigzag M1; ergo M6-in-part)

**Disposition: ACCEPTED.** The design treated persistence-unsafe refs as a wing-slot
problem; the critics showed it is a *class*: wing slots, control-flow operand
constants (`opref`/`contCell`/`lit(dim)` — ~40 sites), the `"ops"` manifest members,
signature-record members, and the param cells the slots point at (which are orphan
cells promote cannot even reach). Doc changes:

- P26 rescoped to "relocatable refs": wing slots become `OpHandle`-valued marked
  cells (native promote reachability + translation); every ref-valued *scalar* —
  opref/contCell/dim literals — becomes a **marked Int64**
  (`+d.mesh.relocation → "cell"`), whose referents the generator harvests as promote
  `extraRoots` and whose values the graft translates through the cell map. Zero new
  VM decode sites (the value stays Int64; `toInt`/`resolveCell`/`refArg` untouched).
  The refused alternative (OpHandle scalars + `handleTarget` arms at ~40 decode
  funnels) is recorded with its price.
- `"ops"` manifest members become marked-Int64 cells too — graft translates them,
  then `registerProgramImage(map[home])` consumes the grafted record verbatim.
  `registerProgramImage` also gains a `handleTarget` arm as a P28 change so either
  convention registers. Multi-image note: registration walks **all** `"ops"`
  records.
- P26's reachability role is stated (marker edges pull orphans into the export;
  param/opref'd cells were absent from the measured 4,296).
- `d.mesh.relocation` minted via `findOrMintDimension` during bootstrap; the
  `zzResolveDim` wrong-rank latent bug (walks `+d.step` off the `d.dims` cell vs
  the `d.dims` chain) is recorded as a sibling observation for the concurrent work.
- Side-effect disclosure: OpHandle/marked wing slots enlarge exports for *every*
  `bindInput` caller (mesh_evaluator, vagent meta/executor, user `zzPromote`).

## Cluster 2 — `link()` replay severs populated ranks (ergo B2; zigzag B1)

**Disposition: ACCEPTED.** Verified `link()` splice semantics
(`manifold.cpp:667–691`). The graft distinguishes **internal edges** (both
endpoints graft-minted → verbatim replay) from **membership edges** onto shared
anchors (`home` `d.stdlib`/`d.vars`/`d.dims`, merged modules' `d.vars` →
`insertIntoRank`/`rankTail` append, matching `exportSymbol`/`getOrCreateModule`);
**`d.dims` edges are never replayed** — dimension identity is by name. Map-miss
discipline: refuse with a named reason inside the intended subgraph, skip outside.

## Cluster 3 — Per-instance state vs per-manifold presence (ergo B3)

**Disposition: ACCEPTED.** `ensureStdlib` splits into `ensureCells()` (once per
arena, presence-checked by resolving a known symbol) and `rebindInstance()` (always
per `VortexStdLib`: `routineBindings_.clear()` + rebuild from records, rebind every
cached ref, `registerProgramImage` into *this* VM). The ctor calls both;
`bindManifold` calls `ensureCells` then `rebindInstance`.

## Cluster 4 — `goalHalt_`/`goalSentinel_` unreachable and unrebindable
(vortex B3; ergo B4; zigzag M4; architect M4; codebase §P33)

**Disposition: ACCEPTED.** Verified: minted `vortex_stdlib.cpp:3487–3489`, only
`fail` exported `:4010`, sentinel use `:4211–4273`, halt use `:2461–2489`. Fix:
export both as `std:goal/halt` and `std:goal/sentinel` (a visible
`symbolsInModule` change, documented); P33's cache inventory names every member
including the `predVortex*` five.

## Cluster 5 — Config cannot reach the ctor-time decision
(ergo M1; architect B3; codebase M5)

**Disposition: ACCEPTED.** The decision moves out of `VortexHostConfig`: a
process-level default chain — explicit early setter > `$GLEDITOR_STDLIB_DIR` >
`$GLEDITOR_ASSET_DIR`/compiled datadir/`./assets` — consulted at graft time;
`VortexHostConfig` keys apply as overrides for later contexts. The
mode×condition refusal matrix is written (§5.4) and the error channel named
(`std::expected<_, StdlibImageRefused>`; ctor records the refusal and `call()`/
`solve()` surface it on first use, since the ctor is `void`).

## Cluster 6 — `--extend` diff/update path (vortex M4; architect M5; codebase B1;
zigzag M7)

**Disposition: ACCEPTED (scope reduced).** Verified: `exportSymbol` appends,
`resolve()` first-match — a re-minted symbol appends *after* the old one and the
update never takes effect; no differ exists. `stdlibSource` v1 ships a **complete
sealed image per release with an atomic asset swap**; shared-lineage `--extend`,
delta transport, and retraction semantics move to a new open question **U-X7**
with the repoint/shadow problem named. The OSMIC story stays honest: microversions
are the image's *internal* generation audit trail; cross-release lineage is v2 work.

## Cluster 7 — `d.mesh.relocation` premise + registry count
(vortex M2; writer M2; architect M6; codebase B2; zigzag m2)

**Disposition: ACCEPTED.** Verified `findOrMintDimension` is the mint path and the
registry holds 19 dims + `d.dims` (≈21 cells with relocation/params). Costs
restated.

## Cluster 8 — Read-only open cannot work (architect B1)

**Disposition: ACCEPTED → new P34.** Verified `O_RDWR|O_CREAT`-only open.
`SegmentedOpsSpool` gains the primedia-spool `O_RDONLY` fallback + `readOnly`
flag; `UserPermascroll::Config` uses its existing `readOnly`; U-X4 collapses into
P34.

## Cluster 9 — Determinism + secret material
(codebase M2; architect m2; zigzag M2)

**Disposition: ACCEPTED.** Generator pins a fixed `documentId` and a fixed
`masterIdentity`/`deviceKeys` pair (or strips `publication-state/` — the shipped
asset must carry **no secret material**; the auto-minted secret-key bencode at
`user_permascroll.cpp:156–158` is the hazard). All record emission walks manifold
ranks (`cells()`/`+d.vars`), never `unordered_map` iteration.

## Cluster 10 — Manifest anchor/scope/ownership
(writer M1; ergo M7; zigzag M3; architect M2,M3; codebase m3)

**Disposition: ACCEPTED.** Both records anchor on `home +d.vars` ("stdlib root" =
grafted `home`, defined once); graft scope = whole fold; `registerProgramImage`
argument = the holder cell; `stdlib.image` field encodings fixed
(`imageOpcodeCount`/`imageVerbCount` Int64s folded into the `imageEpoch` digest
member — text `hash` of the ordered kind→label table; `generatorHead` = text
`ver.str()`; `contentVersion` Int64; `expectedScrollBytes` Int64). Image lifecycle:
process-static `{UserPermascroll, Store, Manifold}` holder +
`resetStdlibImageForTesting()` wired to `resetAssetDirForTesting`.

## Cluster 11 — Marks-free precondition (vortex M3)

**Disposition: ACCEPTED.** `ensureStdlib`/`graftImage`/`registerProgramImage`
assert (and under `Image` mode, refuse) when `outstandingMarks_ > 0`; documented
alongside the `bindManifold` ordering note.

## Cluster 12 — Unattributed copies (zigzag M5)

**Disposition: ACCEPTED.** §5.2 gains the named compromise: graft copies, does not
transclude; promoted-into-user-document cells spool fresh primedia (attribution
severed); in-context stdlib cells are history-less; v2's shared base is what
restores single-instance attributable cells. §10 gains the authorship-goal
sentence (unsigned asset is the preprint).

## Cluster 13 — Validation gaps (codebase M4; ergo M2,M3,M5; architect m5;
vortex m4; zigzag m4–6; ergo m4)

**Disposition: ACCEPTED.** `stdlibPresent` checks a symbol (`resolve("std:math/abs")`)
not just a module cell; `stdlib.image.expectedScrollBytes` + dir-presence precheck
makes the missing-permascroll failure loud with the right cause; isomorphism gate
compares **modulo the graft bijection** (text/shape/values; CellRef outputs mapped);
the image is exactly-one-bootstrap canonical with the observable introspection-count
change documented; export is only defined for `arena().base() == nullptr` (§2.3
gap 6 — verified `baseDimensionNamed` adoption + positional `linkId` writes); the
fold-order fiat (home_/dimsDimension = first two births) and single-owner
invariants are stated; module order note (`std:memoize` first); merge key is full
module text; run(cursor,100)/runPipeline-1000 ceilings listed; `routineBindings_`
reworded as derived index, `clear()` before rebuild.

## Cluster 14 — Epoch semantics (ergo M4; zigzag M6)

**Disposition: ACCEPTED.** `imageEpoch` = digest over the ordered
`OpcodeKind`/`StructureVerb` → label tables (counts recorded as separate Int64
members); `engineEpoch` defined symmetrically; same-count reorder now fails.

## Cluster 15 — `d.params` record shape (ergo M6; zigzag M1; writer m11)

**Disposition: ACCEPTED.** One new dim `d.params` (minted via `findOrMintDimension`;
qattr collision in `sql_lens.cpp:703` is a different namespace, justified in doc);
shape = `sym +d.params → sigCell`; `sigCell` is a vars record with `"in"`/`"out"`
members whose `+d.values` heads are `+d.step` chains of marked-Int64 member cells.

## Cluster 16 — `assetPath` / SDL layering (architect M1; ergo m2)

**Disposition: ACCEPTED.** Engine-side resolution uses a self-contained chain
(explicit path > `$GLEDITOR_STDLIB_DIR` > `$GLEDITOR_ASSET_DIR`/`vortex-stdlib` >
`GLEDITOR_DATADIR` compile-time > `./assets/vortex-stdlib`) implemented in
`paths.hpp`-inline style; the SDL-dependent `assetPath()` is used only at the app
seam. `make install` installs the image for all consumers.

## Cluster 17 — Stale citations (writer B1,B2; all critics' stale-cite minors)

**Disposition: ACCEPTED.** Re-pinned against the current tree at revision time and
the drift note kept — the file is verified live-edited (5274→6265 lines observed
by three critics independently).

## Refused

- **writer m12** — "`GEMINI.md` absent from the tree": verified present at repo
  root (`ls GEMINI.md` succeeds in this worktree). Citations stand.
- **Shared `d.mesh.relocation` marker cell** (zigzag m3): the orchestrator specs
  a shared `"cell"`-texted marker; `isCellRelocationHandle` reads the slot's
  outgoing edge + target text, so the marker's `-d.mesh.relocation` backlink
  eviction is harmless. Recorded as an implementation verify-point: if the check
  needs the backlink, fall back to per-slot markers (~2 ops/slot).
- **`--extend` in v1** (codebase B1 option (a)): a full re-mint appended to the
  same spool still hits first-match shadowing — deferred entirely, not half-built.

## Deferred to U-X

- **U-X7** (new): shared-lineage releases, delta transport, retraction/repoint
  semantics — needs the differ + symbol-shadow spec.
- U-X1 (local-image cell patching), U-X2 (shared base), U-X3 (prefix hydration),
  U-X5 (signing/authorship), U-X6 (per-app subsets) — carried forward; U-X4
  collapsed into P34.

## Proposal-register outcome

P26 rescoped (relocatable refs, both conventions); P27 unchanged+ (whole-space
export + base-null gate); P28 fixed (anchor, marked-Int64 members, all-records
walk, `handleTarget` arm); P29 fixed (varsRecord shape, marked members); P30
split (`ensureCells`/`rebindInstance`, marks-free invariant); P31 rewritten (lazy
default chain, headless lookup, expectedScrollBytes); P32 rewritten (determinism
pinning, no `--extend` in v1); P33 expanded (goal trio exports, full inventory,
`clear()`); **P34 new** (read-only ops spool + read-only permascroll config).
