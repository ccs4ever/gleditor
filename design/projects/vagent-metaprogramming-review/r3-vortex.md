# Round 3 critique (vortex role): vagent-metaprogramming.md v1.2

Lane: VortexCore/VM execution semantics, opcode graphs, trail/backtracking, handle relocation, promotion, MeshActorRuntime invariants. All load-bearing claims re-verified against the working tree under `apps/common/xanadu/`; v1.2's code citations checked: `getOpcodeKind` map-first + `[0,Linked]` fallback (`vortex_vm.cpp:102-113`), `setCursorOpcode` map+bits + `noCell` throw (:131-145), `Call`/`Return` Int64 frame cells (:855-895), `pushChoicePoint` mark+stackFrame save/restore (:204-223, :238-243), `spawnCursor` on home's `+d.cursors` (:148-161), `Halt` parks without popping (:1220-1225), both `step()` writebacks + both `runPipeline` writes via `core_.writeValue` (:337, :343-345, :1271, :1293), `writeValue` Int64/Double/Bool/None arms over `masterOf` (`vortex_core.cpp:195-229`), `splice` delegate (:251-254), `resolveCell` slot-first (:50-67), `toInt` 0-fallback (:38-47), `New` append-refusal vs non-append re-link (:732-760), `Bind` fresh `valCell` (:902-904), `exportSymbol`/`findModule` first-match (`vortex_core.cpp:100-127`), `mintNamedDimension`/`d.name` (:16-70), `denseOf` range check (`arena_manifold.cpp:256-267`), `handleTarget` public + impl (`arena_manifold.hpp:539`, `arena_manifold.cpp:1090-1098`), `setValueBits` public (`arena_manifold.hpp:590`), `WorkerContext::arena` public member (`worker_context.hpp:63`), `mesh_evaluator.cpp:723-736` precedent (fresh `"cell"` marker per ref), `isCellRelocationHandle` (`foreign_op_handle.cpp:98-117`), `resume` overlay without `importValue` (`mesh_actor.cpp:263-273`), `checkpointView` quiescence (:668-694), `compile` no-marks/no-choices gate (`compiler.cpp:102-111`), `validName` `$` refusal (:407-417), `let`→value-copy (:705-707), `resolved()`/`externalReferences_` (:571-576, :755-758), `reference()`/`jump()`/`dimConstant` ref-cell minting (:516-525, :769-772), `created` temporaries bound at emission (:764-767, :793-799), `emitStatement` Halt tail (:849-872), kernel tables on `const Expr*` (:885-891), `pureBody` cells rule (`checker.cpp:861-865`), builtin signature table (:765-779), import allowlist (:2061-2067), `snake()` accepts `$` (:30-36), `CheckLimits` (`checker.hpp:12-27`), `CompilerBinding`/`ScalarCompilerLimits`/`CompiledStatement` (`compiler.hpp:15-34`), lexer `$` error + identifier charset + `PrefixedStringLiteral` merge (`lexer.cpp:333-335, :14-23, :379-395`), parser `cells` gate + `parseExpression` + `parameters` (`parser.cpp:848-855, :271-281, :316-339`), `CellsPath::Anchor::Variable` already exists (`cell_path.hpp:21-24`), `parseCellsPath` refusals (`cell_path.cpp:147-183`), drain idiom (`vortex_stdlib.cpp:3800-3803, :3841-3843`), well-nested marks comment (`arena_manifold.hpp:771-773`). Not re-verified: `enableMemoization` (:169-175), `Linked`'s internals, contract eval cite ranges, `arena_manifold.hpp:594-618` reciprocal-edge cite, `Mul` overflow at :457-463 — all plausible or previously verified.

## R2-item verdicts

1. **vortex-1 (BLOCKING — OpHandle rewrite omits `d.mesh.relocation→"cell"` edge) — resolved.** P16's four-step ceremony now emits the marker edge at fragment-emission time plus a `+d.ref` record on `+d.refs` (§6.6 :1212-1219; M11 :338-346; head diagram :879-886), explicitly states `isCellRelocationHandle` requires both halves (:1207-1209), and correctly cites the `mesh_evaluator.cpp:720-736` precedent. Mechanism verified feasible end-to-end: marker edge is an ordinary link (promotes), `setValueBits`/`handleTarget` are public (`arena_manifold.hpp:539,590`), adapter holds the live arena mutably (`worker_context.hpp:63`), and `handleTarget` returns the arena-side target for the restore (:1090-1097). Residuals → findings 5, 7.
2. **vortex-2 (MAJOR — no live-arena restore) — resolved.** Step 3 restores `Int64(*handleTarget)` on the live arena post-publish, idempotent, marker stays (:1224-1226). Residual: restore-on-failure unstated → finding 5.
3. **vortex-3 (MAJOR — `d.validate` seen-set backwards) — resolved.** Now `visited +d.validate → marker`, membership `linked(visited, d.validate, POS) == marker`, fresh marker per pass, one mark slot per cell by eviction (:1315-1319); traversal restricted to op-graph dims (:1314-1315).
4. **vortex-4 (MAJOR — bound-output `New` refusal too narrow) — resolved.** Refused "in `run`-reachable graphs and at *every* `graft_*` target — append and non-append forms alike" (:1003-1005, :1311-1312); wrapper/`Halt` interaction stated (:1166, :992-995). Verified the underlying asymmetry: append refuses on already-linked output (:736-743), non-append silently `setLink`s (:757-760).
5. **vortex-5 (MAJOR — funnel cannot produce `ExecutionResult`) — resolved.** "A refusal is a typed `success=false` `ExecutionResult` surfaced per site" (:1125-1127; P15 row :1649). Verified every write site in `vortex_vm.cpp` routes through `core_.writeValue` except the three named exempt-by-construction (`setCursorOpcode` :139-143, `Call` frame :862, `mintOpcode` :73-81); `Bind`'s `:904` hits a fresh cell. The `optional`→typed-refusal signature change is inside P15's diff scope.
6. **vortex-6 (MAJOR — `learn` trusts emission-time `closed`) — resolved in shape.** Re-verification at accept is now specified (:1194-1197: walks `+d.refs` + capture links, each endpoint state-side or name-record). But the runtime "state-side" *test* is never named, and the pristine-original rule collides with it for program-side fragments → finding 1.
7. **vortex-7 (MINOR — synthesized `let` snapshots) — resolved.** `cap$N` is a pure `CompilerBindings` alias to the binding's own cell; the `let`→copy lowering and `validName` citations are exact (:683-694). Bonus verification: `$` is unlexable outright (`lexer.cpp:333-335`), and `CellsPath` `$`-anchor rewriting is specified (:699-704).
8. **vortex-8 (MINOR — `run` args `[Cell]` vs values, where args land) — resolved.** List literal, per-element `any`, evaluated in order, frame-bound by parameter name as values (:976-986, :733-736); descriptor param cells are signature data, `result`/`produced` *cells* recorded on the descriptor (:864-872, :738-745).
9. **vortex-9 (MINOR — `choiceDepth()==0` wrong release guard) — partially resolved.** The rule now says "innermost outstanding mark" but the parenthetical "`choiceDepth` zero *and* `outstandingMarks` zero" is unsatisfiable for releasing a live `m0` → finding 4.
10. **vortex-10 (MINOR — `masterOf` predicate, exempt writes) — resolved.** `opcodeMap_.contains(masterOf(cell))` (:1120-1122); exempt-by-construction writes named (:1128-1131) matching the verified site list.
11. **vortex-11 (MINOR — arity table has no home) — resolved.** Adapter-bound constants named (:856-857, :1312).
12. **vortex-12 (MINOR — `reify` must retarget `+d.refs`) — resolved.** "regenerates the copy's `+d.refs` (records remapped to the new ref cells, name-records re-linked)" (:952-953).
13. **vortex-13 (MINOR — wrappers on `Halt` tails) — resolved.** "not `run`-nable if the wrapped graph's tail is `Halt`" (:1166, :992-995). New related gap → finding 6.
14. **vortex-14 (MINOR — "`d.code`/`d.patch`-reachable heads" imprecise) — resolved.** Pass spec reads "each registered head's `+d.refs`" (:1220); patch records hold names, never links (:1179-1182).
15. **vortex-15 (MINOR — pure `validate` writes marks) — resolved.** "trailed, non-semantic writes inside a pure builtin… backtracking removes them" (:1319-1321).

## New findings

### 1. MAJOR — §6.6: `learn` registers the pristine original unconditionally — but `code"…"`-born originals are program cells, violating the `d.code` invariant on the primary path

`learn_*` "grafts a `reify`d copy … and registers the **pristine uninstalled original** on `d.code`" (:1152-1155; :1899-1900), and `learn` accepts "`code"…"`-born fragments and their `reify` descendants only" (:1201-1202). But a `code"…"` literal is compiled once into the artifact (a literal "evaluates to the *same* head on every evaluation", :414-415): its head and graph are emitted in the compile arena and must be link-reachable to be sealed, so at runtime they are **program-side base cells**. Registering one puts a program cell on `agent +d.code` — the rank edge is itself an edge *into* a program cell, and the head's artifact-side links (whatever rank/registry makes it seal-reachable — itself unspecified) drag program structure, the exact failure the stated invariant exists to forbid: "**no cell on the agent's `d.code` rank may carry an edge in either direction into a program cell**" (:1145-1147). The canonical example exercises it: `learn_plan("Feed", "patch:near_water", code"…", code"…")` (:1537-1539) passes two artifact-born heads. The closedness re-verification (:1194-1197) cannot catch it either — it scrutinizes the fragment's *outbound* references, and the doc never names a runtime test for whether a *cell itself* is program-side (`isEphemeral` does not discriminate: checkpointed state cells are also base cells after `resume`, `arena_manifold.cpp:256-267`; the test must be programRoot-reachability, the artifact's cell range, or an adapter-kept tag).

**Evidence:** `design/vagent-metaprogramming.md:1145-1155, :1194-1202, :414-415, :1537-1539`; `arena_manifold.cpp:256-267` (dense-range only); `worker_context.hpp:63` + `mesh_actor.cpp:270-271` (overlay/base structure).

**Fix:** `learn_*` reifies twice — one copy installs, a second registers — or registers the original only when it is already session-side (a `reify` descendant): "a program-side original is never registered; the registered head is always a `reify`d copy" costs one extra copy and keeps `code_named`/`unlearn`/genesis unchanged since they resolve by name. Also name the sideness test the re-verification uses, and state where fragment heads are linked in the artifact so seal keeps them (which is what makes their program-sideness and their backlinks concrete).

### 2. MAJOR — §6.6/§6.1: name-records carry no edge to the slot they govern — genesis cannot find the cell to rewrite

A name-bound call's slot is deliberately *not* OpHandle-rewritten (its target is program-side; marking it would drag the program through `promote`'s handle expansion, `arena_manifold.cpp:1759-1764`), so its int64 is promoted verbatim and must be re-resolved at genesis (:1192-1194, :1229). But the name-record is specified as "a `+d.values`→label, `+d.name`→ the callee's qualified name" (:876, :1190-1191) — **no link to the slot cell**. The doc itself states the inventory exists because "the marker alone is undiscoverable (no per-cell dimension enumeration, U-M7)" (:884-886): without a slot edge, the genesis pass can resolve the qualified name through `symbol_named` but has no way to learn *which* cell to write `Int64(target)` into — the same defect class r2-finding-1 caught (the one edge that makes the pass work, dropped).

**Evidence:** `design/vagent-metaprogramming.md:873-879, :1190-1194, :1228-1231`; `arena_manifold.cpp:1759-1764` (marking would drag program cells — the skip is required); `foreign_op_handle.cpp:98-117`.

**Fix:** give name-records the same `+d.ref→slot` edge ref-records carry (a record with `+d.name` is a name-record: skip re-kind at pre-checkpoint, re-resolve at genesis; without it: OpHandle path). One clause in the head diagram and in §6.6; the pass discriminator already exists.

### 3. MINOR — §6.6: genesis ordering omits program-artifact binding, which (c) and (d) both consume

The fixed ordering is "(a) arena overlay; (b) `ensureDimension` …; (c) `+d.refs` rehydration … resolve name-records through `symbol_named`; (d) `d.patch` replay …; (e) turns" (:1227-1232). `symbol_named` (and `code_named`/`plan_named` in (d), :1182-1184) read the *program* skeleton — module `+d.vars` symbol cells — which must be loaded and bound from `programRef` before any name resolves. The ordered list never binds it.

**Fix:** insert "(b′) program artifact bind (programRef → program root binding, the V10 mechanism)" before (c); the resume path's rehydration claim (`mesh_actor.cpp:260-271`) is otherwise correctly cited.

### 4. MINOR — §6.4: the release guard's parenthetical is unsatisfiable

"a `release(m0)` may run only when `m0` is the innermost outstanding mark (`choiceDepth` zero *and* `outstandingMarks` zero …)" (:1090-1091). If `m0` is outstanding, `outstandingMarks() >= 1`; the parenthetical describes the state *after* the release, not the precondition. The intent (the ruling's `outstandingMarks() == 1` under cp-only marking) is present but mis-stated.

**Fix:** "(no other mark is outstanding — under cp-only marking, `outstandingMarks() == 1`)".

### 5. MINOR — §6.6 P16: pass idempotence and failure-path restore are under-specified

The restore pass is called "mechanical and idempotent; already-Int64 cells are skipped" (:1224-1226), but the same discipline is missing in three places: (a) the *pre*-pass computes the target from `asInt64` — an already-`OpHandle` cell (a ref cell named on two heads' `+d.refs` ranks after surgery, or a re-checkpoint without restore) yields `nullopt` and must be skipped via kind check or `handleTarget`; (b) genesis's `Int64(*handleTarget)` on an already-Int64 cell dereferences `nullopt` — needs the same skip; (c) the restore must run on publish *failure* and on a `consumeWork` throw too — `promote` refusal returns a failure result (`mesh_value.cpp:168-171`), and if the restore is skipped the live arena keeps `OpHandle` slots that make `wingOf`'s `asInt64` fail and `toInt`→0→`setCursorOpcode` throw → `ActorDown` on next execution (the r2-finding-2 mechanics).

**Fix:** one line per pass: "skip cells already in the target kind; the restore runs on every exit path, success or refusal".

### 6. MINOR — §6.6/§6.2: reflect wrappers name their graph by ref-box Int, but `run` reads `+d.entry`

A wrapper "names its graph through a ref-box Int naming the attach op, not a `+d.entry` link" (:1160-1161), yet `run`'s protocol is "read the head's `+d.entry` for the entry op" (:979-980) — so *no* wrapper is `run`-nable, while :1166 implies `Return`-tailed ones are. Since wrappers are session-side and never promote, a `+d.entry→attachOp` link on the wrapper is harmless to the checkpoint invariant (and would be caught by `learn`'s re-verification anyway).

**Fix:** either give wrappers `+d.entry` (making run uniform) or state wrappers are never `run`-nable regardless of tail.

### 7. MINOR — §6.6 P16: base-target marked cells hit `promote`'s non-ephemeral arm

r2-finding-1's flagged corner persists unaddressed: a generation-2 fragment whose marked ref cell names a *base-manifold* cell (a head rehydrated in an earlier checkpoint, then re-marked) hits `promote`'s arm requiring `store.getCompactOp(target)` (`arena_manifold.cpp:1809`) — whether a resumed-manifold ref resolves in the local store is unestablished.

**Fix:** acknowledge the arm and require the round-trip test (learn → checkpoint → resume → learn → checkpoint).

### 8. MINOR — §6.2: "`run` recursion is safe the way method recursion is" is stronger than the stated mechanism

"each activation gets fresh frame cells, and the fragment's static `result`/`produced` cells are spilled per activation" (:999-1001) — but a fragment's `let` locals are `localCells_` minted statically at emission (`compiler.cpp:853-855`), shared across activations; a local written before an inner `run` and read after is clobbered. Method recursion is safe only if the §5.8 ABI makes locals frame-relative — which the scalar kernel's fragment shape does not have.

**Fix:** state that fragment locals ride the same frame discipline as §5.8 method locals (explicit ABI debt), or admit `run` recursion is safe only for arguments/result until then.

### 9. MINOR — §6.5/P15: the kind predicate covers only *registered* cells; post-`resume` registration is unstated

P15 tests `opcodeMap_` membership; the map is VM-private per context (`vortex_vm.hpp:186`) and presumably empty on a fresh `WorkerContext` (`mesh_actor.cpp:270-271`). A program op cell in a resumed incarnation is unregistered unless the program load path re-runs `mintOpcode` — in which case a kind rewrite is silently `asInt64`-consistent rather than refused.

**Fix:** one clause — either program/artifact op cells re-register at genesis (the load path mints), or the doc accepts that post-resume kind rewrites of unregistered cells are effective-by-decode (still safe, just unguarded).

VERDICT: NOT CONVERGED
