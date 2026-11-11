# Round 9 critique (vortex role): vagent-metaprogramming.md v1.8

Lane: VortexCore/VM execution semantics, store/spool positions, trail/marks,
handle relocation, promote, MeshActorRuntime invariants. Convergence check on
the v1.8 deltas only: (a) the adapter-owned publish `Store` (§4.5 :599-612,
§6.6 :1363-1421, §10.1 :2295-2309, §10.2 :2323-2341), (b) the dropped
`plan_named`/`symbol_named` `run`-operand arm (:1146-1149, :1564-1575,
:1616-1621), (c) genesis (a)'s positional assertion (:1732-1749), (d) the
`getOpcodeKind` rationale moved to §6.5's write-guard (:1289-1329,
:1752-1764).

## New findings (v1.8 deltas only)

### 1. BLOCKING — genesis (a)'s "walk-max must equal `E`" is unsatisfiable on the only named emission path

**Location:** §6.6 :1732-1736 ("every cell the `program()` structural walk
reaches must hold a persistent ref `≤ E`, and the walk's maximum must equal
`E` — the image prefix length"), with `E` defined at :1389-1390 and
:1382-1383 ("the image occupies persistent positions `1..E`").

**Problem.** `E` is the ops-segment *op count* of the image prefix — "the
image prefix length" over a spool where a persistent `CellRef` is a cell's
**birth-op index** (`Store::cellRefOf` returns `opsSpool.indexOf` of the mint
op, `store.hpp:677-681`; the fold sets `slots[d].birthOp = opIndex`,
`manifold.cpp:434`, and `self = slots[d].birthOp` is the ref links store,
`:487`). The positions `1..E` therefore include non-birth ops — splices,
setValues and links — and the image's *last* op is never a birth on any real
image: `promote` emits **all links last** (`arena_manifold.cpp:2010-2017`,
"Links last: both ends have to exist"), after the per-cell mint loop
(`:1863-1982`) and the cyclic-handle `setValue`s (`:1984-2003`); a sequential
toolchain emission ends the same way — `exportSymbol`'s last writes are the
`+d.values` and rank links (`vortex_core.cpp:119-127`), `addClause`'s the
rank link (`vortex_stdlib.cpp:4238-4241`). So `max(birth positions of image
cells) < E` on every linked image — and an unlinked "image" is no program.
The mandatory assertion **refuses the legitimate lineage on every genesis**:
the fail-closed arm is inverted — it fires on the happy path, not on
attackers.

**Fix.** One comparand, two landmarks: the band bound needs `E_ops` (last
image *op*), the walk-max needs `E_cell` (last image *cell birth*). Record
`E` as "the image's last cell-birth position" (the greatest persistent ref
the image contains) — `≤ E` then classifies births correctly on both sides,
and `walk-max == E` is satisfiable — or keep `E_ops` for the band and add
`E_cell` for the max clause; a third spelling is "the highest `lastOp` over
walked cells must equal `E`" (the last link's subject does reach `E_ops`
under links-last emission). Any of them also wants a coverage note: a
walk-unreachable last image cell makes `walk-max < E_cell`, so the walk's
dim vocabulary must provably cover every image cell.

### 2. MINOR — the positional test does not detect within-band permutation; the stated rationale overclaims

**Location:** :1733 — "**positionally**, since the count alone would pass a
scrambled one."

**Problem.** The stated test catches only *band-boundary* violations: an
image cell displaced above `E`, or an image truncated below it. A
permutation *inside* `1..E` passes (every walk cell `≤ E`, max `== E`) while
every intra-image positional reference silently aliases the wrong cell —
`Call`/`Jump`/`Branch` int64 operands are raw `CellRef`s
(`vortex_vm.cpp:848`, `:856`) and stored link `to` fields are birth
positions (`manifold.cpp:469`,`:522`). The reachable reorder path is
`adoptOpRecords`: it topologically re-sorts adopted records and tie-breaks
ready siblings by `produces` hash (`store.cpp:3880-3938`) — a linear chain
is forced into original order, but a *branched* artifact segment can land
image cells permuted within the band, scrambling links as well as int64
content. The doc's own "stronger optional comparand" — received ops `1..E`
equal the recorded segment (:1739-1741) — is the only stated check that
catches it.

**Fix:** make the ops-equality comparand mandatory (it is also the only
test that survives finding 1's recalibration), or narrow the claim:
positionality defeats interleaving and displacement, not permutation.

### 3. MINOR — `program()` is walked at step (a) but bound at step (c)

**Location:** :1732-1736 (step (a) asserts over "the `program()` structural
walk") vs :1765-1766 (step (c), "`programRef → the program root` bind by
the V10 mechanism").

**Problem.** "The ordering is fixed" (:1729), yet (a)'s mandatory walk is
rooted at the program root three steps before the root is bound into the
incarnation. On the first resume there is no bound `program()` until (c)
runs; read literally, the assertion's walk has no root — an empty walk
fails `max == E` (or worse, passes vacuously under the `≤ E` clause). The
only reading that works is the adapter resolving the *recorded* programRef
on the received fold at (a), with (c) re-binding it for in-arena use — but
that resolution is what (c) describes, not (a).

**Fix:** name it — "the structural walk rooted at the adapter-recorded
program root" — or move the bind before (a).

### 4. MINOR — "the platform holds no store accessor" is false; `ArenaManifold::store()` is public and already reachable

**Location:** :1417-1419 ("Fresh-store publication is therefore the
fail-closed arm **by detection**, not by construction: the platform holds no
store accessor that could prevent it").

**Problem.** The prevention half is right — `publishValue` accepts any
`Store&` and nothing cross-checks it against the arena's — but the accessor
half is wrong: `ArenaManifold::store()` is public `const` and falls through
to `base_->store()` (`arena_manifold.hpp:269-272`), and the base's `store_`
is set by the fold (`manifold.cpp:113-123`,`:592`). It is reachable
in-session through `checkpointView`'s `const ArenaManifold&`
(`mesh_actor.hpp:102-106`) and through the `Executor`'s `WorkerContext&` —
so *detection* needs no new surface today (`snapshot->arena.store()`'s band
can be asserted directly). What is actually owed is the *supply* direction:
`spawn()` takes no store (:156), `resume()` keeps its `receiveValue`-rebuilt
one private (:260-271).

**Fix:** "no write-path *enforces* the lineage store — `publishValue` takes
any `Store&` and nothing compares it to `arena.store()`; the adapter can
detect in-session through the existing accessor but cannot supply/verify at
spawn/`resume`." The correction strengthens the doc's own fail-closed
story.

### 5. MINOR — `applyOpsSegment` is file-private; the adapter's seeding seam is `historyFromSeal`

**Location:** :601-602, :1367-1368, :1743-1744 ("applied into the fresh
store by `applyOpsSegment` (`publication.cpp:1030+`)").

**Problem.** `applyOpsSegment` sits in an anonymous namespace
(`publication.cpp:1023-1091`); the public entry is
`historyFromSeal(segments, from, scrolls)` (`publication.cpp:1093-1099`,
`publication.hpp:606`), which moreover *creates and returns* the `Store` —
the adapter does not pass a fresh store in, it keeps the one
`historyFromSeal` builds after `addScroll(from)`. Naming the inner step as
the adapter's call sends an implementer to a function they cannot invoke.

**Fix:** "sealed into a fresh `Store` by `historyFromSeal`
(`publication.cpp:1093-1099`) — `applyOpsSegment` (:1030+) is its
per-segment step; position-stability then rests on `adoptOpRecords`' order
(`store.cpp:3836-3940`)."

### 6. MINOR — "`mergeValues` promotes imported twins into a fresh store" is only the no-base arm

**Location:** :1737-1738 (and changelog :2818).

**Problem.** `mesh_host.cpp:488-511` writes the merge into `base->store` —
the *continuing* lineage store — whenever `job.base` is set, and into a
fresh `empty` store only otherwise. In the continuing arm the merged
lineage keeps `1..E` legitimately and must *not* fail closed; only the
no-base arm produces the prefix-less store the doc's parenthetical describes
(and there the imported value graphs contain no program cells at all —
`importValue`'s BFS from the value root never reaches them,
`mesh_value.cpp:392-414` — so the refusal is `programRef`-bind, not the
positional assertion).

**Fix:** "`mergeValues` into a *fresh* store (the no-`job.base` arm,
`mesh_host.cpp:508-511`); under `job.base` it appends above the base's
prefix and the band survives legitimately."

### 7. MINOR — `run(code_named(qualified-L))`'s emission-side slot content is unspecified

**Location:** :1564-1571, :1616-1621.

**Problem.** `code_named(L)` with qualified `L` is named "the only route a
`run`-role record has to the qualified resolution path", and the text
admits "it fails `code_named`'s `d.code` walk in session" — and the
program-image label walk too, since no head label spells
`module:Agent.member`. Yet §6.2 makes the `run` operand slot head-valued
*by construction* ("the emitter writes `int64(head)` into it", :1122-1126).
What the emitter writes for an emission-unresolvable `code_named` operand
is never stated: refusing the operand form at compile time makes the "only
route" dead text, while a tombstone slot needs its session-side failure
named (the descriptor lookup at §6.2). One sentence decides it — and the
coherent choice is the tombstone, since the feature exists *for* the
post-install genesis bind.

**Fix:** "for an emission-unresolvable `code_named` operand the emitter
writes the slot as the failed lookup — session-side the `run` faults at the
descriptor check; genesis writes `int64(head)` when the qualified name then
resolves to one."

## Items verified clean in the v1.8 deltas

- **`attach(Space)` installs `base_`/`store_` when unset** —
  `arena_manifold.cpp:274-287` (`:279-284`), a real public API
  (`multi_store.cpp:225`, `overlay.cpp:406`, `quoted_structure.cpp:525`).
  Its rank/storeCell mints are ephemeral overlay writes on a fresh context.
- **Position-stable, not byte-identical** — `applyOpsSegment` remaps
  `record.op.span.scroll` into the receiving `ScrollId` space via
  `history.addScroll` (`publication.cpp:1048-1070`,`:1079-1087`) while
  everything else "travels as it was written" (`:1074-1078`); the seal-side
  named table is `:981-1008`. For a *linear* chain `adoptOpRecords`' topo
  order is forced = original order (`store.cpp:3880-3938`) — positions are
  preserved (branched-segment caveat is finding 2).
- **Store ownership** — `spawn()` creates/accepts no store
  (`mesh_actor.cpp:152-179`, context at `:156`); `resume()` rebuilds via
  `receiveValue`→`rebuildManifold` (`mesh_value.cpp:350-351`) and keeps it
  private in `actor->checkpoint` (`:260-271`); `checkpointView`/`Executor`
  signatures expose no store (`mesh_actor.hpp:102-106`,`:113-116`);
  continuation pins at `:199` (`request`) and `:215-216` (`grant`) — all
  citations accurate.
- **Whole-spool seal** — `publishValue` calls `sealLocalSpool` without
  `opsAlreadySealed` (`mesh_value.cpp:226-228`), `publish` gets
  `{*seed.history.opsSegment}` (`:239-241`); `sealableOps(store, 0)` covers
  `1..opsTotal` (`publication.cpp:611-617`,`:664-672`,`:982-984`).
- **`job.base` continuing store** — `fetchValue` → shared `base->store` →
  `evaluate`/`publishValue` (`mesh_jobs.cpp:304-325`).
- **Fresh-store publish is today's only path** —
  `mesh_actor_resume_test.cpp:125` (`xanadu::Store store;` →
  `publishValue(store, {}, …)`), accurate to the line.
- **Fail-closed detection is mechanically implementable** — the
  discriminator keys on adapter-held state (the `programRef` bind record +
  recorded `E`) over the arena's base manifold; a fresh-store publish
  leaves no program cells → `programRef` bind refusal at (c) / empty
  program-side set → `learn` refused session-side. No store accessor is
  needed for the walk (and `arena.store()` exists anyway — finding 4).
- **`plan_named`/`symbol_named` arm dropped cleanly** — no dead text: the
  enumeration at :1564-1575 is literal/`code_named`-qualified-L producers
  only, §6.2's headless-`run` class carries the session-side refusal
  (:1142-1149), the signatures justify it (`Opt<Cell>` at :1046,1050 vs
  `Opt<Code<Cell>>` :1051; `run` demands `Code<T>`, M3 :171), and P16's row
  (:2336) matches.
- **`getOpcodeKind` rationale correctly relocated** — the pass exists for
  map *membership*: `getOpcodeKind` decodes via `asInt64` ≤ `Linked`
  (`vortex_vm.cpp:102-114`), while P15 refuses on
  `opcodeMap_.contains(masterOf(cell))`; `setOpcodeKind` (`:98-100`) vs
  `mintOpcode` minting an ephemeral `makeScalarCell` (`:73-82`) — "mints
  twins" verified. Write-funnel sites all exist: `Value`/`Splice`
  `:771-806`, memo-hit `:1269-1273`, generic writeback `:1287-1294`,
  `runPipeline` `:337`,`:343-346`; exemptions verified — `setCursorOpcode`
  writes `setValueBits` directly, bypassing `writeValue` (`:139-143`),
  `Call` frames are fresh `makeCell`s (`:860-863`), `mintOpcode`'s own
  `setContent` (`:78-79`). `masterOf` is real
  (`vortex_core.cpp:344-346`).
- **Genesis (d)→(e) hand-off and copy-inheritance** — install set named
  (:1768-1774); `reify`'s `New`/`Link`/`Value` replay can't mint
  handle-kind cells, so copies' ref slots inherit `int64(target)`
  pre-rehydrated while name-record slots still need the rebind
  (:1784-1795) — consistent with `importValue`'s demote
  (`mesh_value.cpp:483-501`).
- **Support claims** — `noCell == 0` (`dim_vector.hpp:23`); shadow keeps
  the base ref (`arena_manifold.cpp:1489-1490`); `insertIntoRank` same-dim
  chains (`:1423-1439`); `exportSymbol` `sym +d.values → entryOp`
  (`vortex_core.cpp:119-127`); `initGenesis` adopts `*base->home()`
  (`:30-36`); `pending`/`checkpointView`/`pumpOne` cites
  (`mesh_actor.cpp:659-662`,`:672`,`:462`,`:467-468`,`:474-476`);
  `receiveValue` folds the checkpoint's own spool (`mesh_value.cpp:350-351`
  → `mesh_actor.cpp:270-271`).

## Nits (non-blocking)

- :614, :1386 — "`[0..opsTotal)`" is off by one: ops are 1-based
  (`noCell == 0` unused); `sealableOps(0)` seals `1..opsTotal`. Write
  `1..opsTotal` or `[1..opsTotal]`.
- :156 cite for `spawn()` is the context-construction line; the claim is
  right but the tighter cite is the function body `:152-179`.

## Summary for the orchestrator

All four v1.8 delta areas verify against the tree except genesis (a)'s
positional assertion, which is **unsatisfiable as specified**: `E` is the
ops-segment length while cell refs are birth-op positions and `promote`
emits links last — the image's last op is never a birth, so `walk-max == E`
refuses the legitimate lineage on every resume. One comparand conflates two
landmarks (`E_ops` for the band, `E_cell` for the max). The adapter-owned
`Store`, the dropped `run`-operand arm, and the `getOpcodeKind` rationale
are otherwise sound; fail-closed detection is mechanically implementable
(and easier than the doc claims — `arena.store()` is already reachable).
Counts: **1 BLOCKING / 6 MINOR / 2 nits**.

VERDICT: NOT CONVERGED
