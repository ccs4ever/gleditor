# Round 4 critique (vortex role): vagent-metaprogramming.md v1.3

Lane: VortexCore/VM execution semantics, trail/marks, handle relocation, promote, MeshActorRuntime invariants. v1.3 deltas re-verified against the tree: `Executor` signature `std::function<…(const ActorExecution &, WorkerContext &)>` invoked per message at `mesh_actor.cpp:473-477` with `*actor->context` — mutable `WorkerContext`, public `arena` (`mesh_actor.hpp:116`, `worker_context.hpp:63`); `publishValue` `const ArenaManifold&` (`mesh_value.hpp:69-74`); promote's handle arm (`arena_manifold.cpp:1759-1764` enqueues `handleTarget` into `seen`; `:1798-1809` requires `isCellRelocationHandle` for ephemeral targets and `store.getCompactOp` for base targets; `:1947-1951` writes `makeOpHandle`); `link` two-sided eviction (`arena_manifold.cpp:1348-1372`); `handleTarget` `nullopt` on non-OpHandle (`arena_manifold.cpp:1090-1097`); per-reference marker precedent (`mesh_evaluator.cpp:724-735`, fresh `makeCell("cell")` inside the loop); `Call` executes `toInt(inputs[0])` (`vortex_vm.cpp:854-856`); `insertIntoRank` documented idempotent (`arena_manifold.hpp:624-628`); `d.name` minted at `vortex_core.cpp:61`; `dimConstant` mints int64 ref cells (`compiler.cpp:769-772`); `checkpointView` lives at `mesh_actor.cpp:668-694`/`mesh_actor.hpp:190-191`, not `mesh_value.hpp`.

## R3-item verdicts

1. **vortex-1 (MAJOR — `learn` registers program-side originals) — resolved.** v1.3 takes the two-`reify` rule: "never registers the original … `learn` mints **two** reified copies — one for the `d.code`-registered head, one for the installed attachment" (:1226-1231); sideness test named (:1231-1236); installed-head refusal like M14 (:1237); `insertIntoRank`-idempotent re-learn (:1238-1241); `graft_*` reifies `d.code`-resident heads (:1242-1244, :1111-1113); patch names the registered head's `+d.name` (:1266-1274); `unlearn` re-points through a `reify` (:1273-1274). Residual: the sideness walk's dim set cannot reach most of the skeleton → new finding 1; re-learn's implicit exception → finding 7.
2. **vortex-2 (MAJOR — name-record `+d.ref→slot` edge missing) — resolved.** Unified schema: "every record: `+d.ref→the slot cell genesis must rewrite`; `+d.name→qualified name ⇒ NAME-record`" (:911-915, :1285-1286, :1313-1314); genesis rewrite chain spelled `symbol_named → +d.values → head → +d.entry → writeValue(slot, int64(entryOp))` (:1320-1322, :1370-1371); divergence clause (:1323-1326); §9.3 named as the working shape (:1327-1329).
3. **vortex-3 (MINOR — genesis omits program-artifact bind) — resolved.** Ordering is (a) overlay; (b) `ensureDimension`; **(c) bind `programRef → the program root` by the V10 mechanism**; (d) `+d.refs` rehydration; (e) `d.patch` replay; (f) turns (:1367-1377).
4. **vortex-4 (MINOR — release-guard parenthetical unsatisfiable) — unresolved.** The release clause still reads "`choiceDepth` zero *and* `outstandingMarks` zero" while `m0` is outstanding (:1155-1156); the `outstandingMarks() == 1` correction landed on the symmetric *checkpoint* clause (:1157-1158) instead → finding 5.
5. **vortex-5 (MINOR — P16 idempotence/restore-on-failure) — partially resolved.** Re-kind pass skips cells already `OpHandle` (:1353); restore runs "on every exit path including publish refusal/failure and a `consumeWork` throw" (:1361-1362). The skip-target-kind rule did *not* land on the restore pass (:1360) or genesis (d) (:1370), where `*handleTarget` on an already-`Int64` cell derefs `nullopt` (`arena_manifold.cpp:1093-1095`) → finding 4.
6. **vortex-6 (MINOR — wrapper `+d.entry`) — resolved.** Wrappers "carry `+d.entry → the wrapped graph's root op` like every head" (:1245-1247; M8 :279-280); memoization on the `clause +d.code → +d.step` chain keyed by attach cell (:1248-1249; M8 :281-283); `Halt`-tail exclusion kept (:1252-1253, :1464-1465).
7. **vortex-7 (MINOR — base-target arm) — resolved.** Acknowledged with the *learn → checkpoint → resume → learn → checkpoint* round-trip acceptance fixture (:1363-1365; P16 row :1846).
8. **vortex-8 (MINOR — `run` locals frame discipline) — resolved.** Now explicitly ABI debt: "fragment `let` locals ride the same §5.8 frame discipline as method locals (the ABI-debt class §5.3 names — per-activation, orphaned at `Return`)" (:1052-1054), consistent with §5.3's frame-bound class (:703-707) and §6.6's un-learnable-capture rule (:1289-1291).
9. **vortex-9 (MINOR — post-resume `opcodeMap_`) — resolved.** "Post-resume registration: `opcodeMap_` is VM-private per `WorkerContext` and empty after `resume`; the artifact's own op cells are re-registered by the program load path re-running `mintOpcode` at genesis … a kind rewrite of a never-registered cell … is harmless-by-decode" (:1185-1190).

## New findings

### 1. MAJOR — §6.6: the sideness test's named-dim enumeration cannot reach most of the program skeleton — the `closed` re-verification passes links it exists to refuse

**Location:** §6.6 :1231-1236 (sideness test), consumed by the `closed` definition :1282-1301.

**Problem.** "`program-side` means reachable from `program()` by a bounded walk over the program's named dimensions — `d.stdlib`, `d.vars`, `d.clause`, `+d.grab`, `+d.spin`, `+d.step`". The program's own attach conventions route through dims *absent* from the list: symbol→head is `+d.values` ("a symbol cell named `owner.name` whose `+d.values` names the fragment head", :1097-1098 — mirrored for every descriptor/record payload, :903-910); head→entry-op is `+d.entry` (:901). Under the written walk, every method head behind a symbol, **every op cell behind a head's `+d.entry`**, every record/label/contract cell is unreachable → classified "state-side" → a `learn`ed fragment holding a link to one passes `closed` → its registered copy links into program cells → `promote`, which walks **all** dims via `dimensionsOf` (`arena_manifold.cpp:1765`, skipping only `"d.dims"` :1766), drags the program skeleton — the V12-reversed failure the invariant exists to prevent (:1219-1222). The test must *over*-approximate program-sideness relative to promote's all-dim walk; a named-subset walk under-approximates, and the failure direction is permissive (accept), not conservative. Two adjacent gaps: the "bounded" walk's budget-vs-skeleton-depth condition is unstated (cells past budget test state-side), and the doc still never names the artifact edge that makes a `code"…"` head program-side — "hangs off the program's own op graph … reachable from the program root" (:1227-1228) asserts reachability without the anchor edge, so a literal head's classification under its own test is unverifiable (if the anchor is an int64 ref-box or an unlisted dim, the walk misses it too).

**Evidence:** `design/vagent-metaprogramming.md:901, :1097-1098, :1219-1236, :1282-1301`; `arena_manifold.cpp:1765-1766` (promote walks every dim); `vortex_core.cpp:354-414` (op wings on `+d.grab`/`+d.spin`).

**Fix:** extend the enumeration to the full skeleton vocabulary — at minimum add `+d.values` (symbol→head and all record payloads), `+d.entry` (head→op), `+d.name`, `+d.contract` — and state the exhaustiveness obligation symmetric to the R3-F "enumerable frontier" clause: any dim on which program structure can hang must be in the walk, plus the bound ≥ skeleton-depth condition; name the edge anchoring a literal head in the artifact (e.g., a `+d.values`/record edge from the emission site) so its program-sideness is derivable rather than asserted.

### 2. MINOR — §7.2: `textOf(tag) == serial` uses the C++ API name; the serial has no provenance and a promoted-tag collision path exists

**Location:** §7.2 :1472-1480.

**Problem.** (a) `textOf` is the arena C++ call (`arena_manifold.hpp:544`); the document's own surface names the cell-content read `value_of(c: Cell): any` (:967, :1050) — one name should be used. (b) "The pass serial" has no source. If it is a fresh per-pass cell's int64 id, a false "seen" is reachable: tag cells hang off visited cells by `+d.validate` edges, so tags on a *promoted* fragment's ops persist through checkpoint; `promote` copies Int64 content verbatim, and `resume` renumbers — a post-resume pass whose fresh mint lands on the stale tag's stored id makes `validate` skip that cell (under-validation). The durable per-agent counter §6.7 already gives `reify` (:1408-1409) is the named source to reuse. (c) Tags surviving on promoted cells are unpriced checkpoint dead weight (foldable under :1987's dead-cell item).

**Fix:** "`value_of(tag) == serial`, the serial drawn from the durable per-agent counter on `d.vars` (the `reify` serial's source), so no post-resume mint can collide with a promoted tag's stored value."

### 3. MINOR — §6.6 genesis (d): "receives Int64-stale cells" contradicts "converts `OpHandle`→`Int64`" in the same clause

**Location:** §6.6 :1373-1375.

**Problem.** "`importValue` leaves an `OpHandle`-kind cell with its target remapped, while `resume`/`genesis` receives Int64-stale cells and the rehydration *converts* each marked cell `OpHandle`→`Int64`". The store records `OpHandle` ops (`promote`'s `makeOpHandle`, `arena_manifold.cpp:1947-1951`), so the received manifold's marked cells are `OpHandle`-kind — the operative spec two lines up ("per ref-record `Int64(*handleTarget)`", :1370) is correct precisely because `handleTarget` yields the remapped target on `OpHandle` cells and `nullopt` on `Int64` (`arena_manifold.cpp:1093-1095`). If cells truly arrived "Int64-stale" there would be nothing to convert and `*handleTarget` would deref `nullopt`. The genuinely Int64-stale class is the *name-record* slots — never re-kinded, emission-time binding verbatim — which the sentence conflates with the ref-record class.

**Fix:** "ref-record cells arrive `OpHandle`-kind (import remaps the handle's target); name-record slots arrive Int64-stale — (d) converts the first kind and rebinds the second."

### 4. MINOR — §6.6/P16: the skip-already-target-kind hygiene landed only on the re-kind pass

**Location:** §6.6 :1353 (stated), missing at :1360-1361 (restore) and :1370 (genesis d).

**Problem.** R3-D required "every pass skips cells already in the target kind (kind-check before `asInt64`/`handleTarget`)". The restore "restores `setValueBits(Int64, *handleTarget)` on every marked cell" and genesis writes "per ref-record `Int64(*handleTarget)`" with no kind check. A marked cell that is `Int64` at restore time — reachable, since P15 refuses writes only to `opcodeMap_`-registered cells and ref slots are unregistered (`vortex_vm.cpp:854-856`'s `toInt` accepts any cell) — derefs `nullopt`. Edge-rate low, failure silent-or-throw.

**Fix:** one clause applying the rule uniformly: "each of the three passes skips cells already in its target kind before reading `asInt64`/`handleTarget`."

### 5. MINOR — §6.4: the release guard's parenthetical is still unsatisfiable

**Location:** §6.4 :1155-1158.

**Problem.** "a `release(m0)` may run only when `m0` is the innermost outstanding mark (`choiceDepth` zero *and* `outstandingMarks` zero …)" — if `m0` is outstanding, `outstandingMarks() ≥ 1`; the `outstandingMarks() == 1` correction R3-J asked for was applied to the *checkpoint* clause (`:1157`) but not to `release`. Intent recoverable, literal reading impossible.

**Fix:** "(no other mark is outstanding — under cp-only marking, `outstandingMarks() == 1`)."

### 6. MINOR — §3 M11: the seam description is stale relative to §6.6's Executor-callback protocol; `checkpointView` citation is wrong

**Location:** §3 M11 :347-350; §6.6 :1331-1332.

**Problem.** M11 still describes "the adapter's pre-checkpoint pass re-kinds each marked cell `OpHandle` on the live arena **before `checkpointView`**; a post-publish pass writes `Int64(*handleTarget)` back" — the v1.2 seam. §6.6 and the P16 register row (:1846) now specify the flag-driven end-of-turn re-kind inside the `Executor` callback → between-turns `publishValue` → head-of-next-turn restore; the rulings section contradicts the mechanism section on where the mutable window is. Separately, "`checkpointView` and `publishValue` take `const ArenaManifold&` (`mesh_value.hpp:69-74`)" — `publishValue` is at `mesh_value.hpp:69-74`, but `checkpointView` is `MeshActorRuntime::checkpointView` (`mesh_actor.cpp:668-694`, `mesh_actor.hpp:190-191`) returning `ActorCheckpointView{…, actor->context->arena, …}`.

**Fix:** update M11's three-pass sentence to name the callback seam (or defer to §6.6); cite `mesh_actor.cpp:668-694` for `checkpointView`.

### 7. MINOR — §6.6: re-`learn` idempotence needs an implicit exception to "never the original"

**Location:** §6.6 :1238-1241.

**Problem.** "a second learn of the same `code_named`-resolved head re-installs a copy and re-records a patch *without duplicating the rank entry*" — but `code_named` returns the `d.code`-resident registered head (:962), and under the uniform two-`reify` rule `learn` would mint a *new* registered copy, which `insertIntoRank` does not no-op (a different cell). The stated outcome holds only if `learn` reuses an argument that is already the registered head — an unstated exception to "never the original".

**Fix:** one clause: "an argument that is itself the registered head is reused — the second `reify` is skipped and `insertIntoRank` no-ops."

### 8. MINOR — §6.7: `code_named`'s "both reachable by label lookup order" over-specifies a one-rank lookup

**Location:** §6.7 :1413-1416.

**Problem.** `code_named` is defined as "`d.code` rank lookup by `d.name` label" (:962) — the "dead program-image twin" is never in its domain, so "both reachable by label lookup order, which `code_named` resolves deterministically by rank-first match" implies a multi-rank search the API does not define. As written it suggests the twin could be returned, contradicting "only the `d.code` copy is ever the live one".

**Fix:** "the program-image twin is unreachable to `code_named`, which walks `d.code` only; a wider lookup is a deliberate exclusion."

---

**Summary for the orchestrator:** r3 verdicts — 7 resolved, 1 partially (vortex-5: restore/genesis kind-skip unstated), 1 unresolved minor (vortex-4 release parenthetical). New: **0 BLOCKING / 1 MAJOR / 7 MINOR.** Top issues: (1) MAJOR — the `program()`-walk sideness test omits `+d.values`/`+d.entry`, so program heads and op cells classify "state-side" and `learn`'s closed re-verification certifies exactly the program-linked fragments it exists to refuse (permissive direction, `promote` walks all dims); (2) genesis (d)'s "Int64-stale" clause contradicts the `OpHandle→Int64` conversion it describes; (3) `textOf`/serial provenance in the tag-cell seen-set (promoted-tag collision path). All mechanics otherwise verify: executor-callback seam, two-`reify` learn, name-record schema, per-ref markers, P15 coverage.

VERDICT: NOT CONVERGED
