# R7 — Xanadu / ZigZag topology critique (v1.6)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the r6 disposition and the v1.6 deltas
(:1313-1347 sideness/extent/root-exemption; :1323-1329 `importValue`/sibling arms; :1371-1373 memo
label; :1528-1540 publish window; :1574-1605 image-first genesis).

Verified against `apps/common/xanadu/zigzag/arena_manifold.cpp` (`link` reciprocal eviction
1347-1377; `rankContains`/`insertIntoRank` no-op 1381-1439; `shadow` keeps base ref/`birthOp`
1465-1503, overlay_emplace at 1501; `promote` BFS 1733-1788 — root-exemption built in at 1755
(`current != root`), relocation-handle pull-in 1759-1764, `d.dims` skip 1766, both-direction
discovery 1781-1783, `OpHandle` gate 1797-1809, Int64 verbatim 1934-1936, `real.at(target)`/
verbatim-persistent arm 1947-1951, non-ephemeral self-map 1867-1868, ephemeral-only `out.cells`
2004-2007, posward-only link writeback 2024-2027; `ensureDimension` find-or-mint 78-103),
`arena_manifold.hpp` (`refOf = ephemeralBit|dense` 468-470; `insertIntoRank` idempotence contract
624-628), `manifold.hpp` (`ephemeralBit`/`isEphemeral` 154-158; `birthOp == this CellRef` 295),
`dim_vector.hpp:23` (`noCell == 0`), `manifold.cpp:434` (`birthOp = opIndex`),
`cell_views.hpp:258` (`rankTail`), `mesh_value.cpp` (`importValue` fresh base-less arena 416,
all-ephemeral twins 481-492, relocation-marked→`OpHandle` naming local twin 494-501,
unmarked→`None` 485-490; `publishValueImpl` → `promote` 168-169, `sealLocalSpool` whole-spool
226-228; `receiveValue` → `restorePublication` + `rebuildManifold` 345-351), `publication.cpp`
(`sealLocalSpool` `opsAlreadySealed=0` default → segment `at=0` covering the whole spool
611-617,666; `restorePublication` contiguous-segment gate `segment.at == count` 1139-1141, in-order
fold 1156-1165), `mesh_actor.cpp` (`receiveValue`+`WorkerContext(checkpoint->manifold)` resume
260-271; `pumpOne` executor callback 473-477, empty-mailbox skip 462; `pending`=mailbox.size
659-662; `checkpointView` non-empty-mailbox/`outstandingMarks`/choiceDepth refusals 672-677;
`grant->programRef == continuation->programRef` string-equality gate 215-216),
`foreign_op_handle.cpp:98-117` (`isCellRelocationHandle` = OpHandle kind *and* `+d.mesh.relocation`
→"cell" marker edge), `vortex_vm.cpp` (`getCursorOpcode`/`getOpcodeKind` int-decode 102-128;
cursors appended on `home`'s `+d.cursors` rank 157-160; `d.stack` frame chain off cursor cells
206, 860-871; `Call` executes `toInt(inputs[0])` 854-856), `vortex_core.cpp`
(`exportSymbol` `sym +d.values → entryOp` 117-122; `+d.grab`/`+d.step` input wing 386-395),
`vortex_stdlib.cpp` (`makeCell(name)` per-var fresh label 4051-4054; `addClause` per-predicate
`rankTail(predCell, d.clause)` 4238-4241), `ops.hpp:154-163` (OpHandle/ForeignOpHandle kinds —
never executable op content).

## R6 verdicts (my five findings)

| r6 finding | verdict |
|---|---|
| 1 MINOR — single-numbering/`importValue` ingress | **Resolved in mechanism; one wording residual.** v1.6 adopts the fix wholesale: :1318-1325 — "genesis restores the image *first*, ref-preserved … the image's ops are the spool's low prefix and the ops-spool indices *are* the persistent refs — before the checkpointed state joins the numbering and before any `learn`-replay mints a cell … an image arriving through `importValue` ingress or a sibling manifold reports an empty program-side set by design"; mirrored at genesis (a) :1574-1582. The single-numbering requirement and both wrong ingresses are now named. Two residuals ride into new findings: "not an error" drops the *prohibition* (the consequences are real failures, below — N1), and the sibling arm's "empty set" is not mechanically guaranteed (N3). |
| 2 MINOR — state-side root exemption | **Resolved, verbatim-equivalent and now root-strong.** :1335-1336 — "(the walk prunes at the program-side boundary — its own root exempt: a program-side `agent_cell()` cannot prune the durable set)". The phrasing binds the exemption to the walk's root and states the consequence; it is *stronger* than my proposed clause (it no longer depends on the agent cell's sideness at all) and it matches `promote`'s own root rule (`arena_manifold.cpp:1755`, `current != root` exempts the root from the endpoint rule). Neighbour classification stays independent, so exempting a program-side root does not leak the skeleton — each neighbour re-tests both arms. |
| 3 NIT — memo label freshness + bare-op identifying label | **Resolved in full, both halves.** :287-293 — the `+d.name` label is "a fresh text cell per record (the `makeCell(name)` precedent, `vortex_stdlib.cpp:4051`), since an interned label cell could hold only one `-d.name` edge and the second memo's would evict the first's"; and "a label-less attach cell (a bare op under §6.1's pre-head convention) has no key to write, so its wrapper never memo-hits and is re-minted per call". Mirrored at :1370-1373. The eviction reasoning checks out against `link`'s reciprocal eviction (:1352-1373) — each memo record occupies its own fresh label/key cells, so `+d.name` and `+d.grab` never contend. |
| 4 NIT — `src:`/`reify:`/`patch:` as module names | **Resolved, belt and suspenders.** :1475-1479 — the shape test still splits on first-`:` *and* requires module resolution on `programRef`'s walk, and the failure is now bounded: "unless a module literally takes that name (which degrades to a dropped patch, never a wrong bind)" — verified: a mis-split name resolves `symbol_named(module, "demo:12:4")` → unresolved → :1493-1494 drops the patch; a wrong bind would need a symbol literally named `demo:12:4`, which the `:`-free identifier grammar excludes. Belt: :1647-1649 — "the emitter's automatic prefixes (`src:`, `reify:`) are label-reserved, so no module can take those names and reroute a label down the qualified path". The corner is closed at the namespace level too. |
| 5 NIT — :1304-1305 "durable path" phrasing | **Resolved.** :1328-1329 — "a `learn`ed clause sitting on a program `d.clause` rank does not become "program-side" (its durability rides the registered twin and patch replay, not its own promotion)". The misreadable claim that the installed copy keeps a durable path is gone. |

## Direct answers to the round's three scoped questions

**Does ref-preserved replay conflict with `promote`'s renumbering? — No mechanical conflict —
but the invariant that makes it true is asserted, never maintained (N1).** The numbering survives
the round trip *for free*: `publishValueImpl` seals with `opsAlreadySealed=0`, so each checkpoint's
ops segment covers the *entire* spool from index 0 (`publication.cpp:611-617,666`);
`restorePublication` refuses gapped segments and folds in order (`:1139-1165`); `rebuildManifold`
assigns `birthOp = opIndex` (`manifold.cpp:434`, `byRef.emplace(opIndex, dense)`). Restored refs
therefore equal the publisher's op positions exactly — image band `≤E`, state band `>E` — **iff the
publishing store's spool began with the image**. `promote`'s renumbering never collides with it:
non-ephemeral cells map to themselves (`:1867-1868`), and ephemeral state cells mint strictly
appended above the image ops. The conflict surface is the *unsaid* part — see N1. (Also, under the
same-store lineage, step (a)'s "replay the program artifact's own spool" is a no-op the fold already
performs; read literally as a separate replay it invites the double-image bug — N2.)

**Does the sibling-manifold empty-set claim hold? — For `importValue`, verified; for sibling
manifolds, only under an unstated aliasing assumption (N3).** `importValue` builds a base-less
`ArenaManifold` and mints every twin ephemeral (`mesh_value.cpp:416,481,492`), so no imported cell
satisfies the persistent-`≤E` arm — empty program-side, verified. A *sibling manifold's* CellRefs
are just numbers in the arena's numbering space, though: a `programRef` carrying a sibling ref that
names a contained base ref `≤E` walks a foreign subgraph and reports a non-empty, wrong program-side
set — over-pruning, not the promised empty set.

**Does the publish-window ordering respect link invariants? — Yes, verified.** The marker edge is
per-ref-cell and `+d.mesh.relocation`-posward, matching `isCellRelocationHandle`'s kind-and-marker
test (`foreign_op_handle.cpp:98-117`); per-cell markers are forced by the single `-d` slot
(`:1352-1373`) — both as :1521-1524 claims. During the window the marked cells are `OpHandle`-kind,
which is precisely what `promote`'s relocation path consumes: pull-in enqueues `handleTarget` into
`seen`/`frontier` (`:1759-1764`) before the gate reads `seen.contains(*target)` (`:1803-1804`), and
`:1947-1951` writes `real.at(target)` for ephemeral targets. Restore writes `Int64(*handleTarget)` —
the same numeric value the Int64 slot already held, re-kinded — so the round trip is value-stable.
The "no turn between" claim is an adapter-pump discipline layered on `pumpOne` (which has no flag
awareness, `:460-505`); violating it runs a turn against `OpHandle`-kind slots, which faults ops
(`toInt` reads fail), not silent corruption — and `checkpointView`'s non-empty-mailbox refusal
(`:672`) is the backstop. Quiescence preconditions (empty mailbox, `choiceDepth()==0`,
`outstandingMarks()==0`, `:672-677`) plus `pending(pid)==1` (`:659-662`) cover the window. The
marker edge persists through the checkpoint as an ordinary link, which is what lets `importValue`
re-detect the handle on the other ingress (`:494-501`) — consistent end to end.

## New findings (v1.6 deltas only)

### 1. MINOR — the image-prefix invariant is asserted, but the discipline that maintains it is never stated

"the image's ops are the spool's low prefix" (:1576-1577) is the load-bearing fact of the whole
sideness test, and it is true **iff every `publishValue` ran on a store whose spool began with the
program image** — i.e., on the store backing the arena's base manifold (the `checkpoint->store`
lineage). Nothing in §6.6 or the genesis list says that. If a checkpoint is ever published off a
fresh or foreign store — or the image was loaded into a different store than the one the adapter
publishes — the restored manifold numbers *state* cells at refs `1..N` with no image band at all:
step (a)'s "ref-preserved at `0..E`" has nothing to restore from (those indices are already
allocated), and pre-existing state cells at `≤E` now *misclassify program-side* — the transit prune
then empties the durable set, a silent total durability loss that is strictly worse than the V12
bloat the wrong-ingress arms were warning about. Relatedly, the "reports an empty program-side set
**by design, not an error**" (:1324-1325, :1581-1582) softening understates both wrong-ingress
consequences: an arena-resident imported skeleton classifies entirely state-side (silent V12 plus
session objects promoted durable), and a truly-absent image means every `+d.refs` name rebind
faults — both contradict design invariants, so the arm should read "a genesis-refused
configuration", not "not an error". One clause fixes both: *the checkpoint store is always the
store whose spool carries the image as its low prefix — `publishValue` runs on the store backing
the arena's base manifold, and a `programRef` that names no cell of that manifold is a genesis bind
refusal.*

### 2. MINOR — "persistent positions are stable across the rebuild" holds only under a byte-identical image prefix; the active-replay phrasing invites the double-image bug

:1583-1584 claims state positions are "stable across the rebuild". Under the single-fold reading
(image band arrives inside the checkpoint's own ops segment) it is trivially true. Under the
two-spool reading step (a)'s active phrasing suggests — "re-materialize the program image …
re-`import` the prelude and replay the program artifact's own spool" — state ops land at
`imageLen+i`, equal to their checkpointed positions only if the replayed image prefix is
op-count-identical to the prefix the checkpoint was promoted over. The resume gate pins only the
job *string* (`grant->programRef == continuation->programRef`, `mesh_actor.cpp:215`) — a
same-named re-artifact with a different spool length silently shifts every persistent state ref,
and rehydration repairs only *marked* cells: `##` ref-boxes and unmarked Int64 ref cells re-point
silently (`resolveCell`'s dense-range check passes, :1624-1627). The same phrasing invites a second
bug: replaying the artifact spool into a manifold that already carries the image band duplicates
every program cell above `E`, where `code_named`/label walks can misbind. One disambiguating clause:
*on resume the image band arrives inside the checkpoint's own ops segment — step (a) replays a
foreign spool only into a store that does not already carry the image, and only when the prefix is
byte-identical (op-count-equal to the recorded extent).*

### 3. NIT — the sibling-manifold "empty program-side set" arm is not guaranteed under ref aliasing

As scoped above: sibling-manifold refs are numerically indistinguishable from the arena's own low
dense indices, so a mis-bound `programRef` reports a wrong *non-empty* program-side set rather than
the promised empty one. The claim holds only if `programRef` resolves inside the arena's base or
not at all — make "programRef names no cell of this manifold" a genesis bind refusal and the arm
becomes true by construction.

### 4. NIT — "positions `0..E`" off-by-one

`noCell == 0` (`dim_vector.hpp:23`) and op indices run `1..opCount` (`mesh_value.cpp:251`,
`publication.cpp:1167`). ":1579 positions `0..E`" reads as though ref 0 were a program cell; the
boundary is `≤E` with `E` the max minted ref — tighten to `1..E` or just "at or below `E`".

## Verified accurate (v1.6)

The sideness vocabulary's bare-`d.` clarification (:1304-1307) is right: rank membership edges run
`rankTail +d → member` posward, so outbound-following reaches members — `addClause`
(`vortex_stdlib.cpp:4238-4241`), cursor append (`vortex_vm.cpp:157-160`), and `exportSymbol`'s
`d.vars` rank insert (`vortex_core.cpp:119-122`) all chain posward. "`d.stack` hangs off *cursor*
cells, never `home`" (:1342-1343) verifies at `vortex_vm.cpp:206,860-871`; cursors on `home`'s
`+d.cursors` rank at :157-160; `home` arena-minted/ephemeral → not a program-side anchor —
consistent. "`##` anchors are ref-box ints, invisible to the link walk" (:1345-1346) matches
`getCursorOpcode`'s `asInt64` decode (:116-128). The shadow-sideness claim (:1330-1333) verifies at
`arena_manifold.cpp:1489-1503` — the shadow's ref is the base ref, `holdsOwn` true, so a
pre-extent-persistent shadow is program-side *and* would be promote-expanded if reached: the
`d.code` invariant + `closed` arms are what keep that hole shut, exactly as the doc intends. The
re-`learn` carve-out's `insertIntoRank` no-op (:1356-1359) verifies against hpp:624-628 and
cpp:1430-1432. `ensureDimension` find-or-mint (:1309-1311, :1587-1588) verifies at :78-103 —
`dimensionNamed` (base) then `arenaDims_` cache then mint. The `+d.name`-present discriminator
(:1463-1468) is coherent: name-records skip the marker so their program-cell targets never reach
`promote`'s relocation path — the gate at :1797-1809 would otherwise `nullopt` the publish or pull
a program cell into `seen`. The head-valued vs entry-valued writeback split (:1470-1475, :1485-1492)
matches `Call`'s `toInt(inputs[0])` (:854-856) and `exportSymbol`'s direct `+d.values → entryOp`
(:117-122). `resume`-without-remapping (:1597-1603) verifies against `mesh_actor.cpp:260-271` +
`restorePublication`: `OpHandle`-kind cells arrive verbatim carrying promoted targets, and
`importValue`'s remap/demote (:485-501) is correctly cited as the contrasting path.

VERDICT: **CONVERGED** — 0 blocking, 0 majors, 2 minors (unstated image-prefix/store-lineage
invariant; positions-stable caveats + double-replay reading), 2 nits (sibling-ref aliasing; `0..E`
off-by-one). All five r6 findings resolved with mechanically correct replacements; the remaining
defects are specification gaps in the newest deltas, each a one-clause fix.
