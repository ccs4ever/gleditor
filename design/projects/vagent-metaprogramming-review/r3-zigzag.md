# R3 — Xanadu / ZigZag topology critique (v1.2)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity.

Verified against `zigzag/arena_manifold.cpp` (`link` reciprocal eviction 1347–1377, doc comment
`arena_manifold.hpp:594-618`; `promote` BFS 1733–2054; `handleTarget`/`asInt64` 1067–1098;
`rankContains`/`insertIntoRank` hpp:620-628, cpp:1381+; `dimensionsOf` 956–981),
`vortex/foreign_op_handle.cpp` (`isCellRelocationHandle` 98–117), `vortex/vortex_core.cpp`
(`exportSymbol` 119–127, `findModule` 100–106, `wingOf`/`bindInput`/`bindOutput` 354–414),
`vortex/vortex_vm.cpp` (`resolveCell` 50–67, `getOpcodeKind` 102–114, `Call`/`Jump`/`Return`
836–895, `Linked` output boxes 654–723, `New` bound-output refusal 724–768),
`vortex/mesh_evaluator.cpp:720-736` (both halves of the precedent), `vortex/mesh_actor.cpp`
(`receiveValue`→`ReceivedValue.manifold` resume path 255–299, `checkpointView` 668–694),
`vortex/worker_context.hpp:52-55` (arena = ArenaManifold overlay on the received Manifold),
`vortex/mesh_value.cpp` (`importValue` relocation handling 386–509),
`vagent/compiler.cpp` (`reference()` 516–518, `dimConstant` 769–772, `literal` 559–569).

## R2 verdicts (zigzag-1..16)

| r2 finding | verdict |
|---|---|
| 1 BLOCKING — `d.code` reachability | **Resolved in design, one unhandled case.** §6.6 now states the invariant verbatim and the disciplines are complete for the normal flow: `run`/`graft_*` never register, `learn_*` grafts a `reify` and registers the pristine original, `d.code`-resident heads reify before grafting, wrappers moved clause-side, `unlearn` drops registration. I verified the installed reify is **not** reachable from `d.code` by another path: patch records hold *fresh text cells*, never links to heads (`+d.values`→name cell, §6.6:1177-1181); `reify` mints no edge from original to copy (`reify:<src>:<serial>` is text); capture links land state-side only. **Residual:** `learn_*` of an *already-installed* head can still register a program-backlinked head — new finding 4 (MAJOR); the invariant's "program cell" term needs the fragment-own-graph carve-out — minor finding 14. |
| 2 BLOCKING — live-arena corruption after P16 | **Resolved.** P16 step 3 restores `setValueBits(Int64, *handleTarget)` post-publish, idempotent, marker stays (§6.6:1224-1226). `handleTarget` returns the bits target (`arena_manifold.cpp:1091-1098`) exactly as needed. |
| 3 MAJOR — `d.name` uniqueness | **Resolved** (`reify:<src>:<serial>` labels, duplicate-label refusal) — but see finding 5: same-head re-registration still unspecified. |
| 4 MAJOR — `d.weight` homes | **Not fully resolved — MAJOR remains** (finding 6). Edge-vs-bits now disambiguated (`J +d.weight → FloatCell`) and clause weights keyed by clause `d.name`, but §8.1 still lists "d.vars records of a clause" as a home and §8.2 still says "a `d.weight` on the clause" — a clause is program structure; its `d.vars` record dies at `resume`, contradicting §8.1's own "never on program structure" in the same paragraph. |
| 5 MAJOR — mark completeness | **Resolved for enumeration** — wing slots, `reference()` cells, `dimConstant` slots all named plus the `Linked` carve-out. **New over-marking hole:** name-ref Call/event targets are also `makeScalarCell(int64(ref))` cells and get marked → the relocation pull-in floods the program into every checkpoint — new finding 2 (BLOCKING). |
| 6 MAJOR — relocation-marker ceremony | **Resolved.** Inert `d.mesh.relocation→"cell"` edge at emission + `+d.ref` record; both halves of `mesh_evaluator.cpp:723-735` cited; marker stays post-restore. |
| 7 MINOR — §4.4 contradiction | **Resolved.** "heads under §6.3's attach convention; op roots appear only in graphs predating it" (:525-527). |
| 8 MINOR — unlearn deregistration | **Resolved** (:1167-1168 — registration edge removed when the last patch naming it is undone). Residual: §6.3's stale "stays reachable through the patch" persists (:1055-1056) — minor finding 9. |
| 9 MINOR — head discrimination | **Resolved** — `+d.entry`-presence (:527, :894). |
| 10 MINOR — occupancy reservation | **Resolved** — M14 reserves `-d.grab`/`-d.spin`/`-d.values` to attach verbs explicitly (:407-410). |
| 11 MINOR — wrapper memo invalidation | **Resolved** — keyed by current attach cell, re-wrap on re-point (:1161-1163). But the clause `+d.code` memo is a single edge — `test_of`/`body_of` evict each other (finding 8), and the wrapper's entry encoding breaks `run` (finding 7). |
| 12 MINOR — recipe current-vs-origin | **Resolved** — recorded-emission-template replay; link-encoded endpoints, no census ints; current-graph copy is U-M7 (:944-960). |
| 13 MINOR — genesis ordering | **Resolved** — overlay → ensureDimension → `+d.refs` rehydration → patch replay → turns (:1227-1232). Residual: the `programRef` load is missing from the ordering (finding 11). |
| 14 MINOR — `unlearn` matching | **Partially** — `unlearn(clause: Cell)` and the patch's resolved qualified target name are both stated, but the comparison criterion is still never written in one sentence (finding 12). |
| 15 MINOR — bounded `validate` walk | **Resolved** — op-graph dims only (:1314-1315). Moot, though, until finding 1's seen-set is fixed — the walk never terminates on cycles regardless. |
| 16 MINOR — synapse endpoint domain | **Resolved** — qualified names of anything `symbol_named`/`code_named`/`plan_named` resolves; raw cell ids refused (:1377-1380). |

## New findings

### 1. BLOCKING — §7.2 `d.validate` seen-set: the adopted direction holds ≤1 cell — the bug R2-C fixed, mirrored

The fix adopted from R2-C — `visited +d.validate → marker`, membership
`linked(visited, d.validate, POS) == marker` (:1316-1319) — is topologically identical in effect
to the direction it replaced. `link` maintains *one edge two cells share* ("linked(a,d,dir)==b
exactly when linked(b,d,-dir)==a", `arena_manifold.cpp:1359-1361`), so the marker's `-d.validate`
is a single slot. The second mark — `link(visited₂, validate, POS, marker)` — finds
`displacedIt = marker->neighbor(-dir) = visited₁` and calls
`setOneSide(visited₁, validate, POS, noCell)` (:1352-1356, :1369-1373): visited₁'s mark edge is
*deleted*. At most one cell can be marked at a time, in either arrow direction. The disposition's
rationale "one mark slot per visited cell, N backlinks on the marker for free" is impossible —
a cell has exactly one `-d` neighbour per dim, so the marker cannot accumulate backlinks.
Consequence: any back-edge (`Jump`/`Call`/`Choice` — i.e., every loop and alternative) re-visits
a cell whose mark was just evicted → re-walks forever → `consumeWork` throws → `ActorDown`
(`arena_manifold.hpp:420-426`; `mesh_actor.cpp:473-479`); deep DAGs re-walk exponentially.
`graft` runs `validate` on every install (:1324), so all non-trivial grafts fault — the exact
symptom r2 diagnosed, unchanged.

**Fix.** Per-visited tag cells: `visited +d.validate → tagᵢ` where `tagᵢ` is a fresh cell
carrying the pass serial; membership = the edge exists *and* `contentOf(tagᵢ) == serial`. O(1)
mark, O(1) test; a stale edge points at an old-serial tag and fails equality; re-marking a cell
evicts its old tag harmlessly. Alternative: a pass chain on the dedicated dim — `marker
+d.validate → v₁`, `vᵢ +d.validate → vᵢ₊₁` (each visited cell's `+d.validate` is free until
marked), membership = `linked(visited, d.validate, NEG)` occupied — O(1) — but it needs a
post-pass unlink walk or a pass-keyed comparison to keep stale chain-membership from falsely
satisfying later passes. The tag version is the smaller change to the current text. (The codebase
critic's own alternative — a marker-headed `+d.step` rank — collides with the wing/`+d.step`
chains on the visited cells themselves; a dedicated dim is required.)

### 2. BLOCKING — §6.6 P16 step 1: the uniform mark rule marks name-refs; the relocation pull-in then floods the program skeleton into every checkpoint

A `closed` fragment's name-bound Call/event target is an emitter-minted
`makeScalarCell(int64(programOpCell))` — `Call` reads `toInt(inputs[0])`
(`vortex_vm.cpp:854-856`), and the slot is `makeScalarCell(int64(...))`
(`vortex_core.cpp:384,402`). The uniform rule — "*every* `makeScalarCell(int64(ref))` the
emitter writes" gets marker edge + `+d.ref` record (:1212-1219, :443-445, :1650) — has **no
name-ref exclusion**. The pre-pass then re-kinds that cell `OpHandle`, and `promote`'s
relocation pull-in (`arena_manifold.cpp:1759-1764`: `isCellRelocationHandle` → `handleTarget` →
`seen` + `queued`) inserts the *program-cell target* into `order`/`frontier` — the `:1755`
guard does not stop it (ephemeral + `holdsOwn`) — so the op expands, its wings/`+d.spin`/home
links are enumerated, and the program skeleton (potentially `home` and everything it names) is
pulled into the checkpoint — the V12 failure re-entering through the relocation path, likely
past `PromotionBudget` (`:1785-1787`, `:1829-1831`). §6.6's "the checkpoint pass does not
OpHandle-rewrite name-records" (:1193) is only true if the pre-pass can *see* they are
name-records — i.e., they carry no `+d.ref` edge — which the uniform rule contradicts.
Second half of the hole: genesis must "resolve name-records through `symbol_named`" and rewrite
the ref cell (:1229), but the diagram's name-record carries `+d.values→label` (:876) — a text
cell — so nothing links the record to the ref cell it must rewrite.

**Fix.** (i) Scope the rule: a ref cell whose `int64` names a program-side cell is recorded as a
**name-record**, never a `+d.ref` record — the pre-pass consumes `+d.ref` edges only. (ii) The
name-record reaches its ref cell: `+d.values → refcell` + `+d.name → qname cell`; record kinds
are then distinguished by `+d.name` presence (name-record) vs `+d.ref` presence (ref-record).
(iii) Genesis' rewrite chain stated fully: `symbol_named(qname) → +d.values → (head →
+d.entry) → entry op → writeValue(refcell, int64(entryOp))` — the Call target must be an *op*
cell, not the symbol or head cell the loose wording permits. (iv) State the unresolvable-name
outcome (drop/fault, matching the dead-patch rule). §9.3's `learn_plan(..., code"{
navigate_to(\"water\") }")` is exactly this shape — the doc's own example hits the bug.

### 3. MAJOR — §6.6 (:1177-1186): the patch records the *installed* copy's `+d.name` label; replay resolves registered originals

"each `+d.values`-pointing at a fresh cell holding one recorded head's `+d.name` label
(**installed heads** in graft order — `learn_plan` records test head then body head)". The
installed head is the `reify`'d copy — label `reify:<src>:<serial>` — a session cell dead at
`resume`. `code_named`/`plan_named` resolve on `d.code`, which holds only pristine originals
(labelled `src:…`/`#label` or, per finding 4, a registration `reify:` label). A recorded
`reify:` name resolves to nothing at genesis → every patch drops with a fault → `learn`'s
durability silently does nothing. `installed` is a defined term (occupied install-dim
backlinks) which the registered head never has — the wording selects the wrong cell.

**Fix.** The patch records the *registered* head's name — the name `code_named` resolves at
replay: "each record `+d.values`-points at a fresh cell holding the registered head's `+d.name`
label — the original the patch re-installs at replay, in graft order (`learn_plan`: test
original then body original), followed by the prior head's registered name if any". Also state
that genesis replay performs reify+attach only — it re-registers nothing and mints no patch
cells (the "replay does not re-record" clause covers patches but not the `d.code` append step).

### 4. MAJOR — §6.6 (:1152-1155): `learn_*` of an installed head — the "pristine uninstalled original" does not exist

The flow assumes the fragment's head is always uninstalled. But a `code"…"` literal evaluates to
the *same* head on every evaluation (:414-415); after a session `graft_where(c, code"…")` that
head carries `-d.grab ← clause` — an edge into a program cell. `learn_where(c2, code"…")` on
the same literal then "registers the pristine uninstalled original" — except the original is
installed, and `agent +d.code → H` where `H -d.grab → clause` gives `promote` a backlink into
the program skeleton — the invariant breach the section exists to prevent. Does M14's
second-graft refusal fire on the *arg* (learn-of-installed refused — consistent but unstated) or
on the fresh copy (→ the breach)?

**Fix.** Pick one: (a) `learn_*` refuses installed/`d.code`-resident heads like any `graft_*` —
the caller `reify`s first; or (b) `learn_*` registers a fresh *uninstalled* `reify` when the arg
is installed — the registered head must carry zero install-dim backlinks. The "try it for the
session, then make it durable" workflow makes this the common path, not a corner.

### 5. MAJOR — §6.6/§6.7: re-registering an already-`d.code` head corrupts the rank or trips the duplicate-label refusal

Second `learn` of the same literal (or `learn` of an opaque `d.code`-resident `Code`) executes
"registers the pristine uninstalled original" again. An unconditional rank append —
`link(rankTail(agent, d.code), step, POS, O)` — evicts `O`'s existing `-d.step` neighbour
(`displacedIt` → `setOneSide(pred, step, POS, noCell)`, :1352-1373): the rank severs into a
live prefix plus a detached `O → … → tail → O` cycle, and `code_named` misses the orphaned
cells. Or, if implemented via the stated duplicate-label refusal (:954), the second `learn`
fails outright — breaking §8's repeated-learn workflows (two *patches* may legitimately name
one head by design).

**Fix.** Registration is membership-guarded — `rankContains(agent, O, d.code)`
(`arena_manifold.cpp:1381`), or simply name `insertIntoRank`, which is already "idempotent if
@p cell is already in the rank" (`arena_manifold.hpp:624-628`): already-registered → reuse; the
duplicate-label refusal applies to *distinct* heads sharing a label.

### 6. MAJOR — §8.1 (:1366-1371)/§8.2 (:1393): clause weights are still homed on the clause — program-side, dead at `resume`

§8.1 permits weights on "`d.vars` records of a clause or the agent" and §8.2 says "a `d.weight`
on the clause is its strength". A clause cell is program structure: a record on the clause's own
`d.vars` is reachable only through the clause → never promoted → the weight dies at `resume` —
r2-4's failure verbatim — and contradicts §8.1's own "never on op cells or program structure"
in the same paragraph. The name-keying ("resolve clause → `+d.name` → string then read/write the
`d.vars` record carrying that name") is only coherent if the record lives on the **agent's**
`d.vars` — keying is pointless on the clause itself.

**Fix.** "Weights live only on a junction's `+d.weight` scalar cell or on the agent's `d.vars`
records; a clause-correlated weight is an agent-`d.vars` record keyed by the clause's qualified
`d.name` — never on the clause cell (program-side; dies at `resume`)." §8.2 then reads "a
weight record naming the clause is its strength".

### 7. MAJOR — §6.6 (:1159-1166)/§6.2 (:979-982): wrapper heads carry no `+d.entry`, yet `run`/`graft`/`validate` reach the graph only through `+d.entry`

Wrappers "name the graph through a ref-box Int naming the attach op, not a `+d.entry` link" —
but §6.2's `run` lowering reads "the head's `+d.entry` for the entry op". A wrapper has none →
`run(wrapper)` cannot find an entry as specified, contradicting M8's "a `run` on them yields a
`Cell` handle" for Return-tailed wraps. The ref-box choice guards against promotion drag — but a
clause-memoized wrapper is unreachable from state and unpromotable anyway, so `+d.entry` on it
is checkpoint-safe. Also unspecified: which dim links the ref-box (and the memo key's
attach-cell ref-box) off the wrapper head.

**Fix.** Simplest: wrappers carry `+d.entry` normally — the head-indirection convention stays
uniform and the edge is harmless on an unpromoted cell. Alternative: `+d.entry → refboxCell`
with `run` resolving the extra hop (`+d.entry` → `asInt64` → op). Either way, name the ref-box's
dim and say the wrapper keys/returns the head when the attach is itself a head (grafted
fragments — "attach op" presumes op roots).

### Minor findings

8. **`clause +d.code → wrapper` is a single edge — `test_of`/`body_of` memos evict each other**
   (M8 :279-283, §6.6). One `+d.code` neighbour per clause → the second memo clears the first's
   backlink → alternating reflection never memoizes → the "~6-cell head per call" cost returns.
   Fix: memo chain (`clause +d.code → W_t +d.step → W_b`, keyed by recorded attach), two memo
   dims, or state a single-slot policy.

9. **§6.3 stale wording** (:1055-1056): "stays reachable through the patch" — patches hold
   names, not links; the channel is the `d.code` registration retained *while a patch names the
   head*.

10. **Ref-records carry both `+d.values→refcell` and `+d.ref→refcell`** (:873-876) — the
    `+d.values` edge is redundant with the record's own eviction warning; keep `+d.ref`
    canonical and drop it, or state why both exist.

11. **Genesis ordering omits the `programRef` load** (:1227-1232): `symbol_named` at step (c)
    and patch replay at (d) both need the fresh program — add it before (c).

12. **`unlearn` matching criterion + handler priority**: state matching = the clause's resolved
    qualified `d.name` vs the patch's recorded target name; a `learn_handler` patch must also
    record `priority` for replay.

13. **Detach semantics**: `unlearn` of a program-original replace leaves `+d.grab`/`+d.spin`
    empty for the rest of the incarnation — say what a testless/bodiless clause means to
    selection until genesis restores the skeleton.

14. **The invariant's "program cell" term** (:1146): fragment graphs are arena code cells too —
    the rule should name *program-skeleton cells outside the fragment's own closed graph*.

15. **Shared weight scalars self-evict**: `J₁ +d.weight → W` and `J₂ +d.weight → W` — `W`'s
    `-d.weight` is one slot, so the second link severs the first's edge. State each junction
    owns a distinct scalar cell.

16. **`test_of`/`body_of` on head-attached clauses**: "ref-box Int naming the attach op"
    presumes op-root attaches; for grafted heads the attach cell is a head — say the wrapper
    keys by it or returns it directly.

## Verified accurate (v1.2)

`promote`'s relocation-handle pull-in (1759–1764), deferred-cycle minting (1797–1828), verbatim
Int64 (1934–1936), OpHandle store-mint (1947–1951), non-ephemeral identity mapping (1867–1869),
posward-only link write (2024–2042) and `d.dims` skip (1766) all behave as P16 assumes;
`isCellRelocationHandle` requires OpHandle kind *and* the `"cell"`-texted marker edge on
`d.mesh.relocation` (`foreign_op_handle.cpp:109-117`) — v1.2's ceremony matches.
`checkpointView` gates on `choiceDepth()`/`outstandingMarks()`/mailbox/quiescence
(`mesh_actor.cpp:672-682`) and hands `actor->context->arena`; resume rebuilds a store-backed
`Manifold` via `receiveValue` and the `WorkerContext` overlays it — no `importValue`
(`mesh_actor.cpp:255-271`; `worker_context.hpp:52-55`) — so the genesis `Int64(*handleTarget)`
step is necessary on the OpHandle-kind cells rebuild yields, and `handleTarget` returns
store-local refs valid in the overlay. `insertIntoRank` is already idempotent
(`arena_manifold.hpp:624-628`). The `+d.patch-head` dedicated-dim reasoning is correct (the
patch's `+d.values` holds the descriptor and `+d.step` the rank chain). `wingOf`'s
`masterOf(slot)` fallback (`vortex_core.cpp:358-364`) confirms why the post-publish restore is
load-bearing. `Linked` output boxes are runtime-minted ref cells (`vortex_vm.cpp:706-707`),
validating the mark carve-out. `exportSymbol`'s `symCell +d.values → entryOp`
(`vortex_core.cpp:122-126`) matches `graft_method`'s `+d.values → head` shape.

VERDICT: NOT CONVERGED
