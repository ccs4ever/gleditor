# R8 — Xanadu / ZigZag topology critique (v1.7)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity. Convergence check on the r7 disposition and the v1.7 deltas
(:583-598 lineage/table; :1332-1375 sideness/fail-closed; :1384-1393 `home`; :1574-1608
pump/publish ceremony; :1650-1700 genesis reorder).

Verified against `apps/common/xanadu/zigzag/arena_manifold.cpp` (`ensureDimension`
find-or-mint 78-105; `denseOf` ephemeral range-test vs `overlay_` shadow map 255-268; `slot`
base fall-through 791-797; `link` reciprocal eviction 1347-1378; `rankContains`/
`insertIntoRank` no-op 1381-1439; `shadow` keeps base ref/`birthOp` 1465-1503; `promote`
BFS 1733-1788 — root-exemption 1755, relocation pull-in 1759-1764, `d.dims` skip 1766,
both-direction 1781-1783, `OpHandle` gate 1797-1809, non-ephemeral self-map 1867-1868,
Int64 verbatim 1934-1936, `real.at(target)`/verbatim-persistent OpHandle 1947-1951,
ephemeral-only `out.cells` 2005-2008, posward-only writeback 2024-2027), `arena_manifold.hpp`
(`refOf = ephemeralBit|dense` 468-470; `Promoted` 235-240), `manifold.hpp` (`ephemeralBit`/
`isEphemeral` 154-158; `birthOp ==` CellRef 295), `manifold.cpp` (`birthOp = opIndex`,
`byRef.emplace(opIndex, dense)` 427-442; `home_` = first MakeCell 447-454),
`dim_vector.hpp:23` (`noCell == 0`), `publication.cpp` (`sealLocalSpool` default
`opsAlreadySealed=0` → `segment.at=0, length=opsTotal` 585-590,611-617,664-672;
`sealableOps` exports every op verbatim 975-1020; `restorePublication` contiguous gate
`segment.at == count` 1139-1141, in-order fold 1156-1165; `applyOpsSegment` 1030+),
`publication.hpp:337-347` (defaults), `mesh_value.cpp` (`publishValueImpl` → `promote` 168,
`sealLocalSpool` whole-spool 226-228; `receiveValue` → `restorePublication` +
`rebuildManifold` 345-351; `importValue` base-less arena 416, unmarked OpHandle→`None`
483-490, relocation-marked→`OpHandle` naming local twin 494-501; `mergeValues` 512-545),
`mesh_value.hpp:69-94` (`publishValue` takes `const ArenaManifold&`), `mesh_jobs.cpp`
(`job.base` → `store = base->store` → `publishValue` on the lineage store 303-326;
`meta->hash() != job.program` content-pins the artifact 290-292), `mesh_actor.cpp`
(`request.programRef`/`grant->programRef`/`baseRef` pins 196-199,215-216;
`receiveValue`+`WorkerContext(checkpoint->manifold)` resume 260-271; `pumpOne`
empty-mailbox skip 462, executor callback 473-477, `running` guard 472-480,
`!result → stopped` 501; `pending`=mailbox.size 659-662; `checkpointView`
stopped/running/non-empty-mailbox/`choiceDepth`/`outstandingMarks`/undelivered refusals
668-694), `mesh_actor.hpp:102-105` (`ActorCheckpointView` over `const ArenaManifold&`),
`worker_context.hpp:52-66` (`arena` mutable member; `vm`/`opcodeMap_` per-context),
`foreign_op_handle.cpp:98-117` (`isCellRelocationHandle` = OpHandle kind *and*
`d.mesh.relocation`→"cell" edge, both overloads), `vortex_vm.cpp` (`resolveCell`
`arena.contains` 50-67; `mintOpcode` stores `int64(op)` as content + registers 73-82;
`setOpcodeKind` map write 98-100; `getOpcodeKind` map-then-`asInt64` fallback 102-114;
`getCursorOpcode` int-decode 116-129; `setCursorOpcode` writes 131-146; cursors onto
`home`'s `+d.cursors` rank 148-160; `d.stack` off cursor cells 200-206, 854-895;
`Call` `toInt(inputs[0])` 854-856), `vortex_core.cpp` (`initGenesis` adopts
`*base->home()` 30-36; `d.name` mint 61; `exportSymbol` `+d.values`→entryOp + `+d.vars`
rank append 119-127; `+d.grab`/`+d.step` wing chains posward 386-412),
`vagent/compiler.cpp` (no-marks/choice compile gate 105-110; `$`-anchored `%` refused
484-489; `dimConstant` mints int64 ref cells 769-772), `vql/compiler.cpp` (`validatePath`
whole-rank refusal at :71 within 36-80), `cell_path.hpp:31` (string literals),
`mesh_evaluator.cpp:723-735` (fresh `"cell"` marker per reference), `ops.hpp:154-163`
(OpHandle=4/ForeignOpHandle=7 value kinds — never executable op content),
`mesh_host.cpp:484-530` (`mergeValues` into base-or-fresh store, `setCurrentVersions`).

## R7 verdicts (my four findings)

| r7 finding | verdict |
|---|---|
| 1 MINOR — prefix maintenance invariant unstated | **Resolved, adopted near-verbatim.** :1367-1375 — "Prefix maintenance is an **adapter invariant on publish**, not an assertion: `publishValue` runs against the store lineage that carries the prefix — the store backing the arena's base manifold — and a prefix-less lineage (a publish off a foreign or fresh store, an `importValue` ingress of ephemeral twins, a sibling manifold) reports an empty program-side set, under which the discriminator **fails closed**: `learn` is refused outright … and a `programRef` that names no cell of the received manifold is a genesis bind refusal." Repeated at genesis (a) :1657-1664 and §4.5 :593-598. The r7 "not an error" softening is gone — replaced by two named refusal classes, which also answers my "genesis-refused configuration" complaint. Mechanically verified end to end: `job.base` reuses the restored lineage store for publish (`mesh_jobs.cpp:307-318`), `sealLocalSpool` defaults `opsAlreadySealed=0` so the segment covers `at=0, length=opsTotal` (`mesh_value.cpp:226-228`; `publication.cpp:611-617,664-672`), `restorePublication` refuses gapped segments and folds in order (`:1139-1141,1156-1165`), `rebuildManifold` writes `birthOp = opIndex` (`manifold.cpp:434,442`; persistent CellRef *is* a spool position, `manifold.hpp:295`). |
| 2 MINOR — byte-identical prefix + double-image reading | **Resolved, both halves.** "loaded verbatim (byte-identical, or no position is stable)" :1342-1344; and the genesis rewrite kills the two-spool reading entirely — "the persistent manifold **arrives — it is not constructed**" (:1650-1653), step (a) is now "lineage bind" and the list states "nothing in it re-materializes an image" (:1655). The active "replay the program artifact's own spool" phrasing that invited the double-image bug is gone. The same-named-re-artifact residual is adequately pinned: `job.program` is a content hash (`mesh_jobs.cpp:290-292`) and the continuation pins `programRef` + `baseRef` by string/hash equality (`mesh_actor.cpp:196-199,215-216`). |
| 3 NIT — sibling-manifold "empty program-side set" under ref aliasing | **Resolved by my proposed clause, verbatim** — "a `programRef` that names no cell of the received manifold is a genesis bind refusal" (:1374-1375, :1663-1664). |
| 4 NIT — `0..E` off-by-one | **Resolved.** "positions `1..E`" (:596, :1344, :1355, :1658, :1708), with `noCell == 0` (`dim_vector.hpp:23`) named as "the only ref below them" (:1345). |

## Direct answers to the round's three scoped questions

**Does the low-prefix claim survive `sealLocalSpool`'s whole-spool seal? — Yes; the
whole-spool seal is what makes it true.** `publishValueImpl` calls `sealLocalSpool` with the
defaults `priorScroll={}, opsAlreadySealed=0` (`mesh_value.cpp:226-228`;
`publication.hpp:337-347`), so each checkpoint's single ops segment is `at=0,
length=opCount` (`publication.cpp:664-672`). `sealableOps`/`exportBinaryOps` emits every op
verbatim — the :982-1008 loop only builds the scroll-name table, nothing filters the op
stream. On receipt, `restorePublication` requires `segment.at == count` contiguity
(`:1139-1141`) and folds into an *empty* store in order, so restored op positions are
identical to the publisher's. `job.base` then reuses that same store for the next publish
(`mesh_jobs.cpp:307-318`) — new ops append strictly above the prefix. Lineage verified
mechanically, not just asserted.

**Does replay-before-sweep leave replayed copies' ref-records uncovered? — Coverage is
stated and mechanically sound; the enumeration path is not (N2).** (e) explicitly sweeps
"every `learn`-registered head's records **and every replay-installed copy's regenerated
records**" (:1682-1684), and the copy-inheritance rationale is right: an `OpHandle`-kind
source slot yields an `OpHandle`-kind copy whose `handleTarget` is a promoted store ref —
valid in the received manifold — so `Int64(*handleTarget)` resolves correctly on the copy
(`arena_manifold.cpp:1091-1098` reads OpHandle valueBits verbatim). The residual is purely
topological: installed copies hang off *program-side* anchors — exactly the cells the
publish-time walk prunes — so no anchor the sweep was defined over (registered heads'
`+d.refs` ranks) reaches them; (d) must hand (e) its install set.

**Does fail-closed break legitimate foreign agents? — No for every flow checked; the one
gap is assertion strength, not refusal scope (N1).** Foreign *state* — `importValue` twins,
`mergeValues` results — is unaffected: imported cells are all-ephemeral
(`mesh_value.cpp:416,481-492`), classify state-side, and promote legitimately while the
prefix rides the local store lineage underneath them. A foreign agent's *checkpoint* carries
its own image prefix and is refused at (a)/the `programRef` pin — correctly, since it is a
different program. A self-image `importValue` can't smuggle a second skeleton into `learn`:
a twin argument is ephemeral → fails the persistent arm → takes the "already detached" fresh
copy (:1400-1402), so the twin itself is never `d.code`-registered (and ref-boxed twin roots
are int content, invisible to `promote`). What the empty-set arm cannot see is a
*scrambled* lineage — every program cell present but at wrong positions — see N1.

## New findings (v1.7 deltas only)

### 1. MINOR — genesis (a)'s "high-water" assertion is the sole guard against a *scrambled* lineage, and its strength is unstated

The fail-closed story detects *absent* prefixes (empty program-side set; `programRef` binds
no cell). It does not, on a weak reading, detect a lineage whose spool contains the program
cells **at the wrong positions** — the `1..E` band filled by unrelated ops while program
twins sit `>E`. That shape is producible: `mergeValues` promotes imported (all-ephemeral)
twins into a fresh store in discovery order (`mesh_value.cpp:512-545`,
`mesh_host.cpp:484-530`); a merged store re-published through `publishValue` yields a value
whose low band is merge-ordered cells, not the image — yet `programRef` still binds (the
root twin *is* a cell of the received manifold, so the bind refusal never fires) and the
fold is verbatim. Everything then hinges on (a): ":1658 the persistent high-water below
which program cells sit equals `E`" reads ambiguously between a *count* check (passes a
scrambled lineage — fail-open misclassification at exactly the clause added to fail closed)
and a *positional* check over the walk. One clause fixes it: state that (a) asserts every
cell reached by the program-side structural walk holds a persistent ref `≤E` — equivalently
the walk's maximum ref is `E` — so any image twin minted above `E` (or any non-image cell
inside `1..E`) refuses genesis; optionally stronger, compare the prefix's ops bytes/hash
against the artifact segment recorded on the `programRef` bind record, since
"byte-identical" is already the load requirement.

### 2. MINOR — (e)'s sweep claims coverage of replay-installed copies' records but names no path that reaches them

":1682-1684 and every replay-installed copy's regenerated records" is the right coverage —
copies mint OpHandle-kind slots at (d) that only (e) can rehydrate — but the copies hang off
program-side install sites (the `d.clause`/attachment anchors the publish walk prunes), so
neither "`+d.refs`-bounded" nor "registered heads" enumerates them. As written an
implementer must invent the traversal (walk each replayed patch's install site? every
program clause rank?). One clause: **(d)'s replay records, per patch, the minted install
copy and the `+d.refs` records `reify` regenerated on it; (e) iterates (d)'s install set
plus the registered heads.** (Adjacent and correct as stated: replay does not re-record,
:1678-1679; the name-record/misbind rationale for the reorder, :1695-1699 — a call to a
patch-replaced symbol binding the pre-replacement image under the old order is exactly the
dependency the swap fixes.)

### 3. NIT — (b)'s stated dependency names the weakest `opcodeMap_` consumer

"This pass is what `code` re-emission and `getOpcodeKind` need filled" (:1673-1675)
overstates the latter: `mintOpcode` stores `int64(op)` as the op cell's own content
(`vortex_vm.cpp:74`), and `getOpcodeKind` falls back to `asInt64` decode (`:107-113`), so
folded image ops answer their kind *without* registration. The consumers that truly need
map *membership* are re-emission and §6.5's write-guard — ":1296-1298 every write to a cell
absent from `opcodeMap_`" is legal, so without (b) the folded skeleton's op cells are
surgically writable. The pass is required and correctly placed; only the rationale is
incomplete — name the write-protection dependency (it is the load-bearing one).

### 4. NIT — the `home`/frames invariant is stated directionally while the walk is either-direction

":1390-1391 nothing mints a `+d` edge *into* `home`" protects one direction of a test that
walks both. The load-bearing fact is *no link path from the agent cell to `home`, the
`+d.cursors` rank, or any `d.stack` cell* — an outbound cursor→agent-cell edge would satisfy
the stated form yet connect the frames. The conclusion still verifies: rank membership runs
`rankTail +d → member` (`vortex_vm.cpp:157-160`; `vortex_core.cpp:113-126`), `##` anchors
are ints invisible to the walk, and under prune semantics any path *through* persistent
`home` (the store's first MakeCell, `manifold.cpp:447-454`, adopted by `initGenesis`,
`vortex_core.cpp:30-36` — a pre-extent persistent ref, hence program-side) transits a
program-side cell and is cut — which is what makes "were `home` itself to classify
program-side the prune would exclude the same frames" (:1392-1394) hold. Rephrase the
invariant to paths, not edge direction.

## Verified accurate (v1.7)

The lineage paragraph (:1340-1358) is the round's best work: "checkpoints append above it
and `sealLocalSpool` seals the whole spool" matches `opsAlreadySealed=0` exactly; "a flat
append after state would land them above it and invert the test, which is exactly why the
prefix is a load-time commitment" is the correct reason the image cannot be a genesis step
(the manifold arrives folded and `const` — `mesh_actor.cpp:260-271`, `mesh_actor.hpp:102-105`,
`mesh_value.hpp:69-74`). Shadow-sideness (:1364-1366) verifies at `:1489-1501` (shadow keeps
the base ref → pre-extent persistent → program-side, `holdsOwn` true). The pump ceremony is
honest about its seams: `pumpOne` skips empty mailboxes (`:462`), invokes `executor_` around
each turn (`:473-477`), gives no ordering of its own (`:659-672`), `running`/`stopped`
transitions (:472,:480,:501), `pending` exposes `mailbox.size()` (`:659-662`),
`checkpointView`'s quiescence refusals (`:672-682` — including undelivered outgoing, which
the doc doesn't cite but which strengthens the window). The "no turn between" claim is
correctly attributed to *proposed adapter discipline* with the pump limit named
(:1613-1617). Marker mechanics verify: per-cell marker cells forced by single-`-d` occupancy
(`arena_manifold.cpp:1349-1377`; `mesh_evaluator.cpp:723-735` mints `"cell"` per reference);
`isCellRelocationHandle` requires kind *and* marker (`foreign_op_handle.cpp:98-117`); the
re-kind/restore/rehydrate kind-guards are safe since `asInt64`/`handleTarget` are
kind-checked (`:1082-1087`, `:1091-1097`). `importValue` vs `resume` contrast (:1685-1691)
verifies at `mesh_value.cpp:483-501` + `arena_manifold.cpp:1947-1951`. "`d.name` is a
single-link label dim" is right in substance (label use, not rank-chained) — the
`vortex_core.cpp:61` cite is the mint site, though; single-link-ness itself is forced for
every dim by `link`'s one-`-d`-neighbour rule, worth one word someday. `1..E` boundary and
`noCell` usage now consistent throughout.

VERDICT: **CONVERGED** — 0 blocking, 0 majors, 2 minors (genesis-(a) assertion strength
unstated — the one fail-open reading left in the fail-closed arm; (e)'s copy coverage claims
records no defined traversal reaches), 2 nits ((b)'s consumer list misses the §6.5
write-guard; `home` invariant stated directionally). All four r7 findings resolved with
mechanically verified replacements; both remaining minors are one-clause fixes.
