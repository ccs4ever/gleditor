# R4 — Xanadu / ZigZag topology critique (v1.3)

Lane: cell-graph topology — dimension discipline, rank invariants, link eviction, promotion
reachability, cell/name identity.

Verified against `zigzag/arena_manifold.cpp` (`link` reciprocal eviction 1347–1377;
`insertIntoRank` same-dim splice + `rankContains` 1381–1439; `promote` BFS `edge.pos`/`edge.neg`
both discovered 1744–1788, relocation pull-in 1759–1764, `OpHandle` deferred/base arms 1797–1828,
Int64 verbatim 1934–1954; `handleTarget` 1091–1098; `dimensionsOf` 956–981),
`arena_manifold.hpp:594-628`, `vortex/vortex_core.cpp` (`exportSymbol` `sym +d.values → entryOp`
119–127; `findModule` `+d.vars` same-dim `rankTail` 100–127), `vortex/vortex_stdlib.cpp`
(`addClause` `d.clause`-chained rank 4226–4241; `chainCells` 45–52),
`vortex/mesh_actor.cpp:460-505` (executor callback seam, `ActorDown` path),
`vortex/worker_context.hpp:63` (`ArenaManifold arena` — mutable via `*actor->context`),
`vortex/mesh_value.hpp:69-94` (`publishValue` `const ArenaManifold&`),
`xanadu/foreign_op_handle.cpp:109-117` (`isCellRelocationHandle`: OpHandle kind **and**
`d.mesh.relocation→"cell"`), `vortex/mesh_evaluator.cpp:723-736` (per-reference marker precedent),
`vagent-agent-language.md` (clause rank, list `+d.grab`/`+d.step` convention).

## R3 verdicts (zigzag-1..16)

| r3 finding | verdict |
|---|---|
| 1 BLOCKING — `d.validate` seen-set | **Resolved.** Per-visited tag cells adopted verbatim (:1471-1485): `visited +d.validate → tagᵢ`, fresh tag per cell per pass, serial as tag content, membership = edge exists **and** serial match. Mechanically sound under `link` eviction — each tag has exactly one `-d.validate` occupant (nothing evicts it), re-marking a cell swaps its tag (same serial, still seen), stale edges fail the serial check. `+d.entry` added to traversal dims; trailed-marks clause retained. Residual: tag-serial provenance unstated — new finding 5 (MINOR). |
| 2 BLOCKING — uniform mark rule + `+d.refs` | **Resolved in design; two residuals.** Unified schema landed: every record carries `+d.ref → slot`; `+d.name` present ⇒ name-record (no marker, never re-kinded), absent ⇒ ref-record (:911-915, :1313-1319); genesis rewrite stated with `writeValue(slot, int64(entryOp))`; divergence clause (:1323-1326); unresolvable → fault/drop; §9.3 named as the design case. **Residuals:** the rewrite chain assumes `+d.values → head`, which is the grafted-symbol shape, not `exportSymbol`'s — new finding 1 (MAJOR); P16 step 1's text still marks *every* ref cell — new finding 4 (MAJOR). |
| 3 MAJOR — patch records installed copy | **Resolved** (:1266-1269 — each record `+d.values`→ a cell holding the *registered* head's `+d.name`, never the installed copy; :1376 "replay does not re-record"). |
| 4 MAJOR — learn-of-installed | **Resolved** (:1237-1238 — refused exactly like `graft_*`'s M14 second-install rule). |
| 5 MAJOR — re-registration corrupts rank | **Resolved** (:1238-1241 — `insertIntoRank` idempotent via `rankContains`, `arena_manifold.cpp:1381-1439`). Loose wording remains — minor finding 10. |
| 6 MAJOR — clause weights program-side | **Substantively resolved** — §8.1 now: weights only on junction `+d.weight` or *agent* `d.vars` records keyed by clause qualified name, "**never directly on a clause cell**: clauses are program-side" (:1527-1535). **Residual:** §8.2 :1559 still says "a `d.weight` on the clause is its strength" — the exact contradiction — minor finding 8. |
| 7 MAJOR — wrapper `+d.entry` | **Resolved** — wrappers carry `+d.entry → wrapped graph's root op` (M8 :279-280; §6.6 :1245-1253), keyed clause-side. |
| 8 minor — memo single-slot eviction | **Resolved** — `clause +d.code → +d.step` chain, keyed by attach cell; `test_of`/`body_of` coexist (:281-283, :1248). Mechanics gaps remain — minor finding 7. |
| 9 minor — prune wording | **Resolved** (:1117-1118 — channel is the `d.code` registration the patch names). |
| 10 minor — `+d.ref` canonical | **Resolved** (:911-915 — `+d.values` dropped, `+d.ref` canonical with the eviction rationale). |
| 11 minor — genesis `programRef` order | **Resolved** — genesis step (c) binds `programRef → program root` before `+d.refs` rehydration and patch replay (:1367-1370). |
| 12 minor — unlearn matching + priority | **Resolved** (:1275-1280 — match by patch target's resolved qualified name; `learn_handler` records `priority` :1264-1266). |
| 13 minor — empty-`+d.grab` window | **Resolved** (:1119-1121 — test-less clause = unconditionally applicable per spec §5.7, benign-but-visible until genesis). |
| 14 minor — invariant wording | **Resolved** (:1219-1221 — "program-skeleton cell *outside the fragment's own graph*"). |
| 15 minor — shared weight scalars | **Outcome resolved** — "each junction owns a distinct `FloatCell`; two junctions never share one scalar" (:1540-1542). Stated reason is semantic, not eviction — minor finding 8. |
| 16 minor — head-attached wrapper keys | **Resolved** — "keyed by the clause's current attach cell" (:282, :1248); the attach cell *is* the head under M14. |

## New findings

### 1. MAJOR — §6.6 genesis rewrite chain assumes `+d.values → head`; program `def` symbols point `+d.values` at the entry op directly

The name-record rebind (:1320-1322, repeated :1371, P16 row :1846, appendix :2107) is one fixed
chain: `symbol_named(qname) → the symbol's +d.values → the head → +d.entry → entry op →
writeValue(slot, int64(entryOp))`. That shape is `graft_method`'s (`sym +d.values→head`, :973,
:1098). A program `def`'s symbol is minted by `exportSymbol`, which links `symCell +d.values →
entryOp` with **no head hop** (`vortex_core.cpp:119-126`). Following the stated chain on a `def`
symbol: `+d.values` yields the entry op, then "the head's `+d.entry`" dereferences an op's empty
`+d.entry` → dead end → "an unresolved name faults/drops the patch" (:1322-1323) — every
name-recorded call to a `def` drops at genesis. §9.3's `navigate_to` — the flagship `learn_plan`
body — is a `def` (:1717), so the doc's own example hits the dead end; the doc even retells it as
the design case at :1327. A rescue reading — "program-emitted heads" for defs under the owed call
ABI (:945) — is nowhere stated and would change `exportSymbol`'s signature/shape without a register
row.

**Fix.** Resolve through §6.1's own discriminate rule (:943-945): "`symbol_named(qname)` → the
symbol's `+d.values` → the resolved cell is the entry op when it is an op, else follow its
`+d.entry` once (a head) → `writeValue(slot, int64(entryOp))`". If instead defs are to gain
program-emitted heads, say so and name the `exportSymbol`/ABI change it requires.

### 2. MAJOR — §6.7 `:`-label parse rule makes every generated label unresolvable through `code_named`; genesis replay resolves exactly those labels

"labels may not contain `:`/`"`; a `:`-labelled `code_named`/`plan_named` string splits at the
*first* `:`" (:1405-1408). But every system label *is* `:`-bearing: `src:<module>:<line>:<col>`,
`reify:<src>:<serial>`, `patch:<n>` (:1007, :1017, :1404) — the ban can only apply to user `#label`s
(`#` Ident, :617), which the text does not say. Applied literally, `code_named("reify:src:demo:12:4:7")`
splits → qualifier `reify`, rest `src:demo:12:4:7` → `reify` names no module → the lookup fails —
and genesis replay resolves patch head-records "through `code_named`/`plan_named`" on the raw
recorded `reify:` label (:1267-1273), so every patch drops at genesis. `unlearn`'s
`code_named`-resolution inherits it. A verbatim-label fallback is the obviously intended
behaviour — `code_named` matches `d.name` text, and the generated labels embed `:`s by
construction — but it is never stated; an implementer following the stated rule builds a resolver
that cannot resolve any label it is asked to resolve.

**Fix.** "`d.name` labels (`src:`/`reify:`/`patch:`/user `#label`) match verbatim — the first-`:`
split applies only to *qualified-name* lookups (`plan_named`, `symbol_named`-style) and only when
the first segment resolves as a module; unqualified `:`-bearing strings are tried verbatim first."
And scope the no-`:` ban to the `#label` grammar explicitly.

### 3. MAJOR — §6.6 sideness test's named-dim enumeration omits `+d.values` (and `+d.entry`, `+d.name`, `d.name`, …) — the exact dims carrying program structure, so `closed` re-accepts the V12 edge

"*Sideness test:* 'program-side' means reachable from `program()` by a bounded walk over the
program's named dimensions — `d.stdlib`, `d.vars`, `d.clause`, `+d.grab`, `+d.spin`, `+d.step`"
(:1231-1234). `+d.values` is absent — yet it is the only in-edge from the program root to a `def`'s
op graph (`exportSymbol`: `sym +d.values → entryOp`), to every `graft_method` head
(`sym +d.values→head`), and to every head's descriptor (`head +d.values → descriptor`). Those
cells classify *state-side*; a `closed` re-verification then accepts a `cells`-resolved link or
capture landing on them; `learn` registers the fragment; `promote` follows the edge **and its
`-d.values` backlink both ways** (:1782-1783) into the symbol and the module rank — the program
skeleton pulled into the checkpoint through the misclassification, i.e., the failure the invariant
exists to prevent. `+d.entry`, `+d.name`/`d.name`, `+d.refs`, `+d.recipe`, `d.dims` are likewise
unlisted (smaller blast radius — they mostly point fragment-ward). The `dimensionsOf`-is-C++-only
door is honestly admitted (:1294-1297), but `+d.values` is in the doc's own named vocabulary, not a
smuggled dim.

**Fix.** Either complete the enumeration — "every named dim the program itself links on:
`d.stdlib`, `d.vars` (then `+d.values`), `d.clause`, `+d.grab`, `+d.spin`, `+d.step`, `+d.entry`,
`+d.values`, `+d.name`, `d.name`, `d.belief`, `d.source`, `d.contract`, `d.dims`" — or run the
sideness walk adapter-side where `dimensionsOf` exists (the same U-M7 admission the `closed`
frontier already makes), saying so once.

### 4. MAJOR — §6.6 P16 step 1 still marks *every* ref cell — name-bound Call slots included — contradicting the `+d.name` exemption two paragraphs up

"**At emission**, every emitter-minted int64 ref cell — both wing slots, `reference()` cells, and
dimension-constant slots … — gets **its own** inert `d.mesh.relocation→"cell"` marker edge plus a
`+d.ref` record" (:1338-1344). A name-bound Call operand is precisely a `reference()` cell
(`compiler.cpp:516-518`; model-(b) calls it "an ordinary emitter-minted int64 slot" :1311) — so the
step's literal text assigns it a marker, while the discriminator paragraph says a `+d.name` record
"gets **no** `d.mesh.relocation` marker and is **never** re-kindled" (:1315-1316). The pre-pass then
"`setValueBits(OpHandle, target)`s every marked ref cell" (:1351-1353): an implementer who reads
step 1 literally re-kinds the name-ref slot → `promote` enqueues `handleTarget` (a program cell)
into `seen`/`frontier` (:1759-1764) and either drags the skeleton or `return nullopt` when the
target resolves to a non-store cell (:1797-1813) — the r3-2 failure rebuilt. The two paragraphs
contradict on the exact clause the disposition ruled on.

**Fix.** Scope step 1: "every emitter-minted int64 ref cell **that is not a name-record slot** gets
its own marker edge plus a `+d.ref` record — the `+d.name` discriminator of the paragraph above
governs both halves (no marker, no `OpHandle` re-kind for name-records)."

### Minor findings

5. **`d.validate` tag serial provenance unstated** (:1471-1480). Stale `+d.validate` edges + tags
   persist on committed cells and promote with them (tags are reachable; `d.dims` is the only
   skipped dim, :1766). If the pass serial comes from a per-incarnation counter that restarts at
   `resume`, a promoted stale tag's serial collides with a fresh pass's → false "seen" → cells
   skipped → `validate` unsoundly passes. Narrow (the doc's own paths validate fresh reify copies;
   exposure is a user `validate(code_named(…))` on a promoted registered head), but the doc already
   solved the identical problem for `reify` serials. Fix: "the serial comes from the durable
   per-agent counter on `d.vars`, never repeating across incarnations" — and say whether the serial
   lives in the tag's text or value bits (`textOf(tag) == serial` currently hedges "text/content").

6. **"Rank convention" parenthetical mis-cites `d.clause`; `d.code`/`d.patch`/`d.synapse` chain dims never stated** (:1261-1263). The claim "a rank's first edge and its chain edges are different dims: `+d.clause`/`+d.step`" is backwards for `d.clause`: `addClause` chains the clause rank on `d.clause` itself (`rankTail(arena, predCell, dims().clause)` + `link`, `vortex_stdlib.cpp:4238-4241`), as do `exportSymbol`/`d.vars`, `insertIntoRank`, and every `rankTail` append in the tree — the two-dim form is the *list* convention (`+d.grab`/`+d.step`, spec §5.9) and the doc's own record chains (`+d.refs`/`+d.step`, `+d.patch-head`/`+d.step`). More importantly, the top-level `d.code`/`d.patch`/`d.synapse` ranks' chain dim is never stated: `insertIntoRank` is named for registration idempotency and only builds same-dim chains (`link(anchor,dim,dir,cell)` + `link(cell,dim,dir,displaced)`, :1423-1439), so `d.code` must chain on `d.code` — but an implementer applying the parenthetical would `+d.step`-chain it and break `rankContains`-based idempotency outright. Fix: "the agent's `d.code`/`d.patch`/`d.synapse` ranks are same-dim chains per `insertIntoRank`/`rankTail` (the codebase's rank convention); the first-edge/chain split is the list convention used by `+d.refs`, `+d.recipe`, `+d.patch-head` and the memo chain — `+d.clause` ranks are not an example of it."

7. **Memo mechanics under-specified** (:281-286, :1248, :1973-1974). "A `clause +d.code → +d.step` chain of memo cells, each memo keyed by the clause's current attach cell" never says what a memo cell *is* (the wrapper head itself, or a record `+d.values → wrapper`) nor which dim holds the attach-key (a `+d.values`/`+d.name` on a wrapper head collides with descriptor/label occupancy). If the `+d.step` chain links the wrapper heads directly, `validate`'s `+d.step` traversal (:1470 — the wing-slot chain dim in op graphs, `vortex_core.cpp:394,412,460`) escapes the fragment into memo siblings' graphs — wasteful and boundary-breaking. Stale-memo handling (unlink vs leave-as-garbage on re-attach) unstated. Fix: "the memo cell is a record `{+d.values → wrapper head, +d.name → attach-cell key}`; the `+d.step` chain links records only, keeping wrapper `+d.step` free of `validate`'s walk; a key-mismatching memo is unlinked at re-wrap" (or state the alternative).

8. **§8.2 residual + weakened eviction rationale** (:1559-1560, :1540-1542). §8.2 still reads "a `d.weight` on the clause is its strength" — the program-side home §8.1 now forbids in the same breath it gives the correct `d.vars`-record rule (r3-6's contradiction, half-fixed). And §8.1 justifies distinct junction scalars as "a shared scalar would couple unrelated updates" — the actual reason is mechanical: `J₂ +d.weight → W` evicts `J₁`'s edge (`W`'s `-d.weight` is one slot, :1352-1373), so sharing is impossible, not merely bad practice. Fix: §8.2 → "a weight record naming the clause is its strength"; §8.1 → "each junction owns a distinct scalar cell — `link` gives `W` one `-d.weight` neighbour, so a second junction's edge would evict the first's."

9. **Editorial leftovers.** The marker explanation appears twice near-verbatim (:923-930 ≈ :932-936). `symbol_named` is declared two-arg (`symbol_named(owner, name)`, :957) but invoked as `symbol_named(qname)` in the rewrite chain (:1321). `learn_*` "idempotent" oversells (:1238-1241): re-`learn` of the *same literal* mints a second registered copy — distinct `reify:` serial, second rank entry, second patch — which is correct EDL behaviour, but only arg-is-registered-head re-learn dedupes; say so.

## Verified accurate (v1.3)

`link` reciprocal eviction (:1349-1377), `insertIntoRank`/`rankContains` same-dim semantics
(:1381-1439) and `promote`'s bidirectional walk with `edge.pos`/`edge.neg` discovery (:1782-1783),
relocation pull-in (:1759-1764), `OpHandle` seen-gate `return nullopt` (:1797-1809), Int64 verbatim
(:1934-1936), `d.dims` skip (:1766), `handleTarget` = value-bits target (:1091-1098) all behave as
the v1.3 text assumes. The Executor-callback seam is real: `executor_(ActorExecution{…},
*actor->context)` at `mesh_actor.cpp:474-476` hands the mutable `WorkerContext` (`ArenaManifold
arena`, `worker_context.hpp:63`) into the callback while `publishValue`/`checkpointView` take
`const ArenaManifold&` (`mesh_value.hpp:69-94`) — flag-driven end-of-turn re-kind, between-turns
publish, head-of-turn restore on every exit path is a coherent use of it, and `ActorDown` on a
`consumeWork` throw lands exactly where the doc says (:477-486). `isCellRelocationHandle` requires
OpHandle kind *and* the `"cell"`-texted marker edge (`foreign_op_handle.cpp:109-117`); the
precedent mints one marker per reference inside its loop (`mesh_evaluator.cpp:723-735`), matching
P16's per-ref-cell rule. `exportSymbol`'s `symCell +d.values → entryOp` (`vortex_core.cpp:122-126`)
is cited correctly for `graft_method`'s `+d.values → head` *contrast* — which is what makes finding
1 a real defect rather than a nit. The two-reify learn flow's `reify:<src>:<serial>` labels cannot
collide across copies or incarnations (durable per-agent counter, :1407-1410) and
`code_named`'s rank-first match is deterministic (:1415) — the r3-prompted collision questions are
handled; the *parse* side of the same labels is not (finding 2). The memo chain on `clause +d.code`
evicts nothing — clause `+d.code` is free-by-construction and `rankContains(agent, ·, d.code)`
cannot reach it (:284's claim checks out).

VERDICT: NOT CONVERGED
