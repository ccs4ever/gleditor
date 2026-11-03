# R2 — Xanadu / ZigZag topology critique (v1.1)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction semantics,
promotion reachability, identity/durability of cells and names.

Reviewed against `zigzag/arena_manifold.cpp` (`link` eviction 1347–1377, `dimensionsOf`/`DimLink`
956–979, `promote` BFS 1733–2054, `asInt64`/`handleTarget` 1067–1098),
`vortex/vortex_core.cpp` (`wingOf`/`bindInput`/`bindOutput` 354–414, `exportSymbol`/first-match
100–127, `writeValue`/`splice` 196–254, `initGenesis` dims 29–70), `vagent/compiler.cpp`
(`reference`/`literal`/`emitStatement` emission shapes 512–601),
`vortex/mesh_value.cpp` (`importValue` relocation path 386–509), `vortex/mesh_evaluator.cpp`
(publication precedent 720–736), `vortex/mesh_actor.{hpp,cpp}` (`checkpointView` 668–694,
hpp:188–190), `vortex/foreign_op_handle.cpp` (`isCellRelocationHandle` 98–117), and the vagent
spec's V12/V9 (§4.3–4.4).

## R1 verdicts

| r1 finding | verdict |
|---|---|
| 1 BLOCKING — `+d.grab` overload / head attach | **Partially resolved.** Head indirection on `d.entry`, occupancy backlinks, M14 consume-on-graft and "patch records names not links" all landed correctly and are topologically sound. But the *reachability* half of the finding is **not** fixed: an installed head that is `d.code`-reachable still has a live edge into the program. Reopened as finding 1 (BLOCKING). |
| 2 BLOCKING — promoted fragment dead on arrival | **Largely resolved, two new holes.** P16 + `promote`'s relocation-handle pull-in (`arena_manifold.cpp:1759-1764`) is the right mechanism — a marked OpHandle drags its target into `order` recursively, which transitively covers literals, `reference()` cells and op targets, and `importValue` rehydrates to local OpHandles (`mesh_value.cpp:494-502`). New gaps: live-arena corruption after the pass (finding 2, BLOCKING), the `d.mesh.relocation` marker ceremony omitted from the normative text (finding 6), and marking completeness over `reference()` cells (finding 5). |
| 3 MAJOR — `d.synapse` fan-in | **Resolved.** Junction cells on `+d.synapse` with qualified-name *label* endpoints express N→M without violating one-neighbour-per-dim (labels are content, not links). Residual: the "weight on a clause's `d.vars`" option still listed (finding 4); endpoint domain limited to qualified-name resolvables (minor finding). |
| 4 MAJOR — `graft_method` shadowing | **Resolved.** Unlink-first, redefinition-as-replacement, first-match cited correctly. |
| 5 MAJOR — `reify` mechanism | **Resolved.** Recipe replay (census + link tuples + literals + old→new map); `Clone` refused with the correct `masterOf` reason. Residual: tuple encoding and emission-vs-current semantics unstated (minor finding). |
| 6–10 MINORs | **Resolved.** Source label as name; qualified type identity; `+d.patch-head` chain carries both heads (test then body, graft order); `d.validate` mark-links with per-pass fresh marker (self-evicting membership test is correct); `static` residue priced; int-addressed entry covered in §6.4. |

## Findings

### 1. BLOCKING — an installed learned head drags the whole program skeleton into every checkpoint (§6.1, §6.6, M11, §12)

`promote`'s BFS discovers edges **in both directions**: `discover(edge.pos); discover(edge.neg)`
(`arena_manifold.cpp:1782-1783`), skipping only `d.dims` (`:1766`). Every cell of a `DimLink`
carries both endpoints (`dimensionsOf` 956–979), so a head's `-d.grab`/`-d.spin`/`-d.values`
backlink to its clause/symbol is enumerated when the head is expanded. `learn_*` does three
writes — "the graft, a patch cell, and a `d.code` registration for the fragment" (§6.6:963-965) —
and heads register "when first `run`/`graft`ed or stored" (§6.7:1012-1013). So the normal case —
a learned fragment installed at a clause and registered on `d.code` — is exactly the poisoned
shape: `agent +d.code → H`, `H -d.grab → clause`, `clause ±d.clause → goal`, `goal → module →
home`. The program lives *in the arena* (spec §4.4: "each agent's arena loads its own copy of the
compiled program", `vagent-agent-language.md:425-427`); program cells are ephemeral and
`holdsOwn`-ed, so nothing in the `:1755` guard stops the flood — every clause, op, module cell and
`home` lands in `order` and is minted into the checkpoint (`:1863+`), which is precisely the
failure V12 exists to prevent ("a link from state into a program cell would drag the compiled
program and `home` into every checkpoint", spec :244-250) — and likely blows the 65,536-op
`PromotionBudget` (`:1785-1787`, `:1829-1831`), making the checkpoint *fail outright* after the
first `learn`.

r1-1's fix (d) — "graft a *copy*, keep the *original* uninstalled and closed, patch-record the
original" — was not adopted: v1.1 registers the *installed* head. The head indirection repaired
the eviction half of r1-1 and left the reachability half untouched: `clause +d.grab → H` is
still `H -d.grab → clause`.

A second instance of the same breach: `test_of`/`body_of` wrapper heads "memoized per clause cell
on `d.code`" (§6.1:257-258) — a wrapper's `+d.entry` points at a *program op root*, so a promoted
wrapper drags the program through the forward edge the same way.

The correct invariant: **no cell on `d.code` may carry any edge — in either direction — into a
program cell.** Concretely: `graft_*` never registers; `learn_*` grafts a `reify` (the installed
copy is session-side, unreachable, dies at resume as session grafts do) and registers the
*pristine uninstalled original* under the patch's recorded name; `unlearn`'s re-point likewise
installs a reify of the named prior head; `graft_*` of any `d.code`-resident head reifies first
(or refuses, forcing explicit `reify`); reflect wrappers move off `d.code` onto a session rank —
they wrap program graphs and must not be checkpoint-reachable anyway. An uninstalled registered
head's reachable set is then genuinely "the fragment and its state-side captures" (closed by
construction: `+d.entry`→graph, `+d.values`→descriptor, `+d.refs`/`+d.recipe` chains, `+d.name`
label, `±d.code` rank neighbours — all fragment- or state-internal).

### 2. BLOCKING — P16's pre-promote rewrite corrupts the *live* arena; no post-promote restore is specified (§10.2 P16, M11, §6.6)

`checkpointView` is "owner-thread borrow: publish before resuming execution"
(`mesh_actor.hpp:188-190`); it hands `actor->context->arena` to `publishValue`
(`mesh_actor.cpp:692`, `publishValueImpl` calls `promote(store, parent, arena, …)`,
`mesh_value.cpp:168-169`) and the actor *resumes on the same arena*. P16's pass rewrites marked
ref cells `Int64 → OpHandle` in that live arena — and the document specifies rehydration only at
"genesis/graft-time", i.e., on the imported arena of the *next* incarnation. The continuing
incarnation then executes with OpHandle-kind wing slots and `reference()` cells: `asInt64`
requires `ValueKind::Int64` (`arena_manifold.cpp:1083-1086`), so `wingOf`'s operand decode falls
through to `masterOf(slot)` (`vortex_core.cpp:358-364`) and every op reads its *slot cell* as its
operand; `Jump`/`Branch`/`Call`/`Choice` target ints likewise undecode. Every promoted fragment —
i.e., every `learn`ed rule — silently mis-executes from the checkpoint onward.

**Fix.** The pass needs a third step: after `publishValue` returns (and before the actor
resumes), re-walk each `d.code` head's `+d.refs` rank and write `setValueBits(Int64,
*handleTarget(cell))` — `handleTarget` already returns the value-bits target
(`arena_manifold.cpp:1091-1098`), and targets do not move in the live arena, so the restore is
mechanical and needs no association table. State it explicitly; a mark can't bracket the publish
(`checkpointView` demands zero outstanding marks, `mesh_actor.cpp:672-676`), so the restore is a
second pass, not a release. A publish-copy alternative does not exist — `promote` reads the live
arena by const-ref.

### 3. MAJOR — `d.name` uniqueness is never established; patch names replay ambiguously (§6.6, §6.7, §6.1 `code_named`)

Patches deliberately record head *names* (correct — two `+d.*` links to one head would evict
each other), resolved at replay/`unlearn` through `code_named`/`plan_named`, which are
first-match lookups in the `findModule` idiom (`vortex_core.cpp:100-106`). But nothing keeps
`d.name` labels unique on the `d.code` rank: `code"#label …"` user labels collide trivially; a
`reify` copy's label is unspecified (if the recipe copies the head's `+d.name` cell or its text,
every reified candidate in §8.5's search carries its parent's label); reflect wrappers may add
more. Two heads sharing a label → `code_named` returns the first → genesis replays the wrong
fragment, and `unlearn` re-points to the wrong head, silently. Two *patches* legitimately naming
the same head is fine (installed + prior); the hole is two *heads* sharing a name.

**Fix.** Pick one: `d.code` registration refuses duplicate labels; or `reify` mangles the copied
label (`<label>#r:<n>`); or the patch records a minted unique id the head also carries. Also
state that genesis replay performs the graft steps but does *not* re-record a patch (else the
rank doubles per incarnation).

### 4. MAJOR — "a weight on a clause's `d.vars`" contradicts "weights live state-side from the start" (§8.1, §8.2)

§8.1 lists three weight homes — "on a clause's `d.vars`, on a junction cell's `d.weight`, or on
the agent's `d.vars`" — then claims "weights therefore live state-side from the start". A clause
cell is program-side: program-emitted clauses are unreachable from the state root by V12, and a
*learned* clause is reachable only from its goal's `d.clause` rank (still program-side; the patch
names it, doesn't link it) and is re-minted at genesis — a weight on it dies at `resume` exactly
like r1-3 warned. Worse, if any state cell ever links to that weight, the weight's `-d.vars`
backlink drags the clause — finding 1's breach by a second door. **Fix:** durable weights live
only on the junction's `d.weight` or the agent's `d.vars`; a clause-correlated weight keys the
clause's *name* on a state-side record. Also specify whether `d.weight` is a `+d.weight →`
scalar-cell edge or the junction's own bits — `weight_of`/`set_weight` read "a `d.weight`/`d.vars`
scalar", which is ambiguous.

### 5. MAJOR — `+d.refs` marking completeness is asserted, not enumerated (M11, §4.5, §10.2 P16)

The marked set is "operand slot ints and control-flow targets", but the emitted indirection is
two-level: a wing slot holds `int64(referenceCell)` (`bindInput(op, reference(target))`,
`vortex_core.cpp:384` + `compiler.cpp:523-527`) and the `reference()` cell — *unlinked, reachable
only through the slot's int* — holds `int64(targetOp)` (`compiler.cpp:516-518`). Both are ref
cells; an unmarked `reference()` cell promotes Int64 verbatim (`arena_manifold.cpp:1934-1936`)
→ the same silent aliasing P16 kills. The doc must name the complete class: **every cell whose
`int64` content is a `CellRef`** — wing slots (both wings), `reference()` cells, and any
ref-boxes embedded at emission. Runtime-minted ref cells inside a promoted fragment (`Linked`'s
materialized output boxes, `vortex_vm.cpp:706-710`) are unmarkable — benign only because `Linked`
rebuilds its output rank on re-execution (`:683-695`); state that carve-out or refuse `Linked` in
`learn`able graphs. Related minor hazard: marks are `record +d.values → refcell` — safe today
because emission leaves `-d.values` free on slots/`reference()` cells, but any later surgery
writing `+d.values` into a marked cell evicts the mark silently; a dedicated record→ref dim (or
the invariant stated) is the honest version.

### 6. MAJOR — P16 omits the relocation-marker link; `isCellRelocationHandle` requires it (§10.2 P16, §6.6)

An OpHandle-kind cell is a relocation handle only when it also carries `+<d.mesh.relocation> →
"cell"` (`isCellRelocationHandle`, `foreign_op_handle.cpp:109-117`); the cited precedent does
*both* `setValueBits(OpHandle, target)` and `builder.link(cell, relocation, makeCell("cell"))`
(`mesh_evaluator.cpp:723-735`). P16's normative text says only "rewrites … from `Int64` to
`OpHandle`". Without the marker: the BFS pull-in (`arena_manifold.cpp:1759`) doesn't fire, and an
OpHandle naming an ephemeral target hits `!isCellRelocationHandle` at `:1803` → `promote` returns
`nullopt` → the checkpoint fails. With it: works, and the marker also reaches the imported arena
(edges copied at `mesh_value.cpp:503-508`). Specify the full ceremony — `ensureDimension
("d.mesh.relocation")` (quiescent, so legal under the no-marks rule) + marker link per ref cell —
and whether the marker is left in place post-rehydration (harmless: the handle test requires
OpHandle kind; say so).

### MINOR findings

7. **§4.4 vs §6.3 contradiction** (:452): "a clause's `+d.grab`/`+d.spin` for test and body (heads
   or op roots)" — op-root attach is the r1 eviction bug verbatim (`entryOp -d.grab` is the
   output wing, `vortex_core.cpp:398-414`). §6.3's "never an op cell" is right; §4.4 should say
   "heads; op roots appear only in graphs that predate the convention".

8. **Stale wording, §6.3** (:880-881): "a pruned clause's fragment … stays reachable through the
   patch" — patches record *names*, not links; reachability is via `d.code`. Same clause for
   `unlearn`: does it remove the head's `d.code` registration? Unspecified — a durable dead
   fragment stays checkpoint-reachable forever (~O(fragment) promotion ops per checkpoint,
   indefinitely). State the deregistration rule and what happens to patches that still name the
   head ("dropped after recording a fault" presumably — say it covers this too).

9. **Head-vs-op discrimination unstated** (§6.1:749-752): the reader "resolves the linked cell
   directly when it is an op, and follows `+d.entry` once when it is a head" — by what test?
   `getOpcodeKind` decodes any Int64 in `[0, Linked]` (`vortex_vm.cpp:102-113`), so a head must
   carry non-decodable content (Str/None) or the test must be `+d.entry`-presence. State one —
   `exportSymbol`'s `+d.values → entryOp` (`vortex_core.cpp:119-127`) means real program symbols
   point at op roots, so both shapes genuinely occur.

10. **Occupancy-marker reservation rule** (§2 `installed`, M14): `-d.grab`/`-d.spin`/`-d.values`
    on a head must be written *only* by the attach verbs — the doc never says so. Any future
    `x +d.values → head` convention (e.g., a memoization keyed `clause +d.values → wrapperHead`,
    which the unspecified `d.code`-memoization mechanism might plausibly choose) creates a false
    "installed" positive and simultaneously a program-dragging edge. Name the reservation.

11. **Reflect-wrapper memoization has no invalidation rule** (§6.1:257-258): a wrapper memoized
    per clause cell goes stale on `graft_where`/`graft_body`/`prune` — §8.5's loop would score
    dead structure. Key on the clause's *current attach cell* (a re-point then misses naturally)
    and keep the memo session-only.

12. **`reify` recipe encoding** (§6.1:772, :799-806): link tuples must name cells by census
    ordinal, not dense ids — as arena ids they are unmarked int refs that stale at promote.
    Also state whether `reify` replays the *emission template* (as written) or the *current*
    graph — a surgically mutated fragment's reify currently returns its origin, which §8.5's
    `mutate(candidate)` flow makes worth one sentence.

13. **Genesis ordering** (§6.6, M11): state that rehydration of all `+d.refs` cells completes
    *before* `validate`/patch replay/`run` touches a promoted fragment (a replay-time `validate`
    on un-rehydrated refs resolves garbage).

14. **`unlearn` matching criterion** (:780): "the latest matching patch" — matching by target
    name, by clause cell, by head? For repeated `learn_where` on one clause the pop order is
    implied by ordinal; say which identity "matching" compares.

15. **`validate` walk scope** (§7.2:1060-1074): bound the traversal to the op-graph dims
    (`d.spin`, `d.step`, `±d.grab` wings, `d.entry`, Jump/Call targets) — a naive all-dims walk
    would cross the head's occupancy backlink into the program and mark/traverse the whole
    skeleton.

16. **Synapse endpoint domain** (§8.1, `sprout_synapse`): endpoints are qualified-name labels —
    what resolves `patch:<n>` learned-clause names, and do belief/percept endpoints have a name
    domain at all? "Exactly as durable as that clause's qualified name" covers only clauses.

## Verified accurate (v1.1)

`promote`'s relocation-handle pull-in (`arena_manifold.cpp:1759-1764`), deferred-cycle minting
(`:1797-1828`), verbatim Int64 promotion (`:1934-1936`), OpHandle store-mint (`:1947-1951`),
non-ephemeral identity mapping (`:1867-1869`) and posward-only link write (`:2024-2042`) all
behave exactly as P16 assumes; `importValue`'s relocation rehydration (`mesh_value.cpp:494-502`)
is the right shape for genesis. The head subgraph (`+d.entry`/`+d.values`/`+d.refs`/`+d.recipe`/
`+d.name` + `±d.code`) uses six distinct dims with no intra-head slot collision; pointing
ref-records *at* ref cells via `+d.values` (rather than `+d.step`-chaining them) correctly avoids
double chain-membership. The `d.validate` seen-set is self-evicting per pass as claimed.
`graft_method`'s unlink-first matches `exportSymbol`/`findModule` semantics.

VERDICT: NOT CONVERGED
