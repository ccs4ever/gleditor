# R4 — System Architect critique (v1.3)

Scope: layering, ownership, adapter-vs-core boundaries, Makefile/dependency hygiene. All claims re-verified against the tree; cited line numbers are current. v1.3 lands the substance of all twelve r3 findings: the `d.validate` tag-per-cell scheme is mechanically right, the Executor-callback seam is the correct (and only) home for P16's mutating passes, and the unified `+d.refs` schema with the `+d.name` discriminator closes the genesis-rewrite hole. What remains lives inside the new flag protocol itself — it has no firing window when a checkpoint decision lands on an empty mailbox, and its phase/kind-guard precision is unfinished — plus a cluster of spec-precision minors and one Makefile list omission.

## R3 finding verdicts

1. **r3-1 (`d.validate` seen-set, BLOCKING) — RESOLVED.** §7.2 (`:1471-1485`) now specifies per-visited-cell tag cells — `visited +d.validate → tag_i`, a fresh tag minted per visited cell per pass, the tag's content carrying the pass serial, membership = edge exists **and** `textOf(tag) == serial`. This is exactly the nonce-per-mark fix r3 prescribed: every tag holds one `-d.validate` neighbour, so link eviction cannot clear another cell's mark; re-marking replaces a stale tag; stale tags from earlier passes fail the serial test. Traversal dims now include `+d.entry` (`:1470`); marks are stated trailed and non-semantic (`:1480-1481`); the O(visited)-mint + O(n)-membership price is stated (`:1482-1484`) and carried into §13 (`:1978`). Residual → new finding 4 (serial provenance).

2. **r3-2 (P16 mutable-arena seam, MAJOR) — RESOLVED in mechanism; the new protocol has its own residuals.** §6.6 (`:1331-1365`) names the seam r3 identified as the only mutable window — the adapter's own `Executor` callback inside `pumpOne` — correctly: `checkpointView`/`publishValue` take `const ArenaManifold&` (`mesh_actor.hpp:102-106`, `mesh_value.hpp:69-74`), while `executor_(…, *actor->context)` hands the callback the mutable `WorkerContext` (`mesh_actor.cpp:474-476`, `worker_context.hpp:63`). The ceremony — end-of-turn re-kind, publish between turns, head-of-next-turn restore on every exit path — is the right shape: ops execute only inside `executor_` (`mesh_actor.cpp:473-479`), so a head-of-turn drain genuinely guarantees restoration before any execution; `ActorCheckpointView`'s live `const&` sees the re-kind between turns; and the "never sees" → "never *executes*" restatement (`:1354-1357`) is honest (an OpHandle cell reached by `step` would decode `Nop` — `vortex_vm.cpp:1219` — so discipline, not hardware, is doing the work). Verified against the gates: `checkpointView` refuses stopped/running/non-empty-mailbox/nonzero-choiceDepth/nonzero-marks (`mesh_actor.cpp:668-693`) — all satisfiable in the between-turns window the protocol names. No forbidden coupling: the adapter already is the executor and pump driver; no mesh or core file is touched. Residuals → new finding 1 (empty-mailbox window + flag phases/kind-guard) and finding 7 (the `outstandingMarks() == 1` aside).

3. **r3-3 (unified `+d.refs` / name-record discrimination, MAJOR) — RESOLVED.** §6.1 (`:911-915`) unifies the schema: every record carries `+d.ref → the slot cell genesis must rewrite`; `+d.name` present ⇒ name-record (no marker, never re-kinded), absent ⇒ ref-record (full P16 ceremony). §6.6 (`:1313-1322`) states the pre-pass skip — with the correct reason, that `promote` would drag or nullopt a program-cell target (`arena_manifold.cpp:1797-1809`) — the genesis rebind chain, and the divergence clause (`:1323-1326`). Residual → new finding 5 (`+d.values` is not always a head).

4. **r3-4 (state-side predicate, MAJOR) — RESOLVED.** §6.6 (`:1231-1234`) names the test r3's fix proposed: program-side = reachable from `program()` over the named-dim vocabulary (`d.stdlib`, `d.vars`, `d.clause`, `+d.grab`, `+d.spin`, `+d.step`), state-side = link-reachable and not program-side. `closed` is correctly restated around it (`:1282-1307`): frame-bound and `Cell`/`Code` captures unlearnable, `run` targets name-resolved, enumerable-frontier bound with the honest §11-door clause (`:1294-1296`). Residual → new finding 6 (int-referenced program cells are invisible to the link walk).

5. **r3-5 (per-ref-cell marker, MAJOR) — RESOLVED.** §6.1 (`:919-930`) and §6.6 (`:1338-1348`) state one freshly minted `d.mesh.relocation→"cell"` marker per ref cell, with the correct reason — `link` gives each cell one `-d` neighbour, so a shared marker's second edge would evict the first — citing the verified precedent (`mesh_evaluator.cpp:723,735`; constants `foreign_op_handle.hpp:24-25`). Residual → finding 8 (duplicated paragraph).

6. **r3-6 (`cap$N` render-check bypass, MINOR) — RESOLVED.** §5.3 (`:713-716`) names it: `cap$N` lookups bypass `binding()`'s render check (`compiler.cpp:160-181`) and resolve through a `cap$N`-keyed `externalReferences_` entry materialized while the enclosing `emit` holds the cells.

7. **r3-7 (resolve carrier/invocation/driver, MINOR) — RESOLVED.** §5.6 (`:847-853`): the stamp rides on `Expr` (a field `cloneExpr`/`cloneDecl` preserve — right call; pointer-keyed tables die under cloning); `expand` invokes the shared helper per `code"…"` body seeded with the literal's scope; `frontend.cpp` drives parse→resolve→expand→check. Residual → finding 2 (`frontend.cpp` absent from the Makefile edit).

8. **r3-8 (capture-slot representation, MINOR) — RESOLVED.** §5.3 (`:708-713`) names the form — a capture-slot cell wrapping the binding cell on the operand's wing, decoded by `wingOf`'s `asInt64`-fails→`masterOf(slot)` fallback (`vortex_core.cpp:356-365`) — with the honest justification (links are the closed-verifiable form). Clarity nit, no finding: the slot is a `d.clone`-ship clone of the binding cell (`masterOf`, `vortex_core.cpp:344-346`) — naming that mechanism costs half a clause and forecloses a second invented slot convention.

9. **r3-9 (§13 fragment-overhead O-form, MINOR) — RESOLVED.** §13 (`:1969-1972`) decomposes the constant: "O(ops + slots + recipe) per `code"…"` literal — ~8–14 cells for head + descriptor alone, plus one `+d.refs` record *and* one marker cell per emitter-minted ref cell (~1–2 per op)".

10. **r3-10 (`run` list-literal materialization, MINOR) — RESOLVED.** §13 (`:1975-1977`): one materialization per call — head + O(n) element cells via `materialize` (`compiler.cpp:733-741`).

11. **r3-11 (single memo slot thrash, MINOR) — RESOLVED.** M8 (`:281-283`) adopts the `clause +d.code → +d.step` memo chain keyed by attach cell — test/body wrappers coexist. Consistent with the `d.code` invariant: the chain hangs off the program-side clause, never the agent's rank, so wrappers die with the skeleton at resume as stated (`:284-286`, `:1248-1249`).

12. **r3-12 (`run`/`value_of` namespace, MINOR) — RESOLVED.** §5.1 (`:600-602`) extends the `splice`/`static` reservation to `let`/`var`/parameter bindings and to `run`/`value_of` when `std:meta` is imported — the import gate (`checker.cpp:2058-2067`, gaining `std:meta` per `:886-888`) supplies the scoping.

## New findings

### 1. MAJOR — the P16 flag protocol has no executor window on an empty mailbox, and its phase/kind-guard semantics are unstated (§6.6 `:1336-1365`; P16 row `:1846`)

**Problem.** The ceremony is correct *given* a turn — but the only executor window is a `pumpOne` turn, and `pumpOne` skips actors with an empty mailbox (`mesh_actor.cpp:462`). If the adapter decides to checkpoint while `pending(pid) == 0` — an idle actor holding unpublished `learn`s, a timer-driven checkpoint, a compaction checkpoint under the §4.4 reclamation story — no turn will ever run the flag. Both unspecified outcomes are bad: wait indefinitely (checkpoint/migration stalls), or call `checkpointView` anyway — which passes every gate (empty mailbox *satisfies* `:672`) — and `publishValue` promotes every marked slot's Int64 **verbatim** (`arena_manifold.cpp:1924-1937`): exactly the stale-id aliasing P16 exists to prevent. Related coverage edge: with `pending() > 1` the flag fires at the next turn's end but `checkpointView` then refuses (non-empty mailbox, `:672`) — harmless since the head-restore unwinds it, but unpriced.

The second half is precision. The flag needs two phases — re-kind pending → restore pending — but step 5's restore reads "`setValueBits(Int64, *handleTarget)` on every marked cell" (`:1360-1361`) **without the kind-check the re-kind arm states** (`:1353`, "skipping any already in `OpHandle` kind"). A restore firing with nothing re-kinded — the flagged turn's own head if implemented as a single flag check, a second flagged turn before publish, a drained-and-re-set flag — dereferences `handleTarget` on an Int64 cell, which returns `std::nullopt` for non-OpHandle kinds (`arena_manifold.cpp:1090-1098`) → UB. And "the callback owns the flag and drains it" (`:1361-1362`) gestures at a state machine never stated: who clears the flag on publish failure/refusal, and whether an unmatched restore is a no-op.

**Fix.** Three additions: (a) the flag is set only when `pending(pid) == 1` — the terminating turn exists and its end leaves the mailbox empty — or, for an empty-mailbox decision, the adapter mints a P4 `wake` turn so the flag has a window (`pending()` exposes `mailbox.size()`, `mesh_actor.cpp:659-662`); (b) the flag's phases are stated — re-kind pending → restore pending → drained — including who clears it on `publishValue` failure and on `ActorDown`; (c) the restore arm mirrors the re-kind's guard: "restores `setValueBits(Int64, *handleTarget)` on every marked cell **still in `OpHandle` kind**," making an unmatched or repeated restore a no-op.

### 2. MINOR — `frontend.cpp` enters the tree but not `VAGENT_FRONTEND_SRCS`; `ast.hpp` unlisted (§10.1 `:1793-1794`, `:1826-1830`)

**Problem.** The tree lists `frontend.{hpp,cpp}` as the pipeline driver (`:1793-1794`), but the Makefile edit adds only `expand.cpp, resolve.cpp, ast_clone.cpp, code_path.cpp` (`:1826-1827`). If `frontend.cpp` is glob-only like `fragment.cpp`, it lands in `COMMON_XANADU_OBJS` (`Makefile:643`) — reachable from xuzz-family binaries but **not** from `build/vagent` or `vagent_frontend_test`, which link only `$(VAGENT_FRONTEND_OBJS)` (`Makefile:1147-1152`) — the two places a parse→resolve→expand→check driver must be reachable. (`Makefile:698`'s `filter-out` on all `vagent/*.cpp` also keeps glob-picked TUs out of `VAGENT_RUNTIME_CORE_OBJS`, so the glob alone reaches no vagent test binary.) Separately, `ast.hpp` never appears in the inventory though `ExprKind`/`Expr` change (`:841-842`, `:850-852`).

**Fix.** Add `frontend.cpp` to the `VAGENT_FRONTEND_SRCS` edit — it is a source→checked-Program driver with no arena dependency, so it belongs with its siblings — and one inventory line for `ast.hpp` (`ExprKind::{Code,Splice,Static}` + the `Expr` scope-stamp field).

### 3. MINOR — genesis receives OpHandle-kind marked cells, not "Int64-stale" ones (§6.6 `:1373-1375`)

**Problem.** "`resume`/`genesis` receives Int64-stale cells and the rehydration *converts* each marked cell `OpHandle`→`Int64`" — the sentence claims the received marked cell is both Int64-stale and OpHandle. `promote` writes marked cells as `makeOpHandle(real.at(target))` (`arena_manifold.cpp:1948-1951`), so promoted marked cells arrive OpHandle-kind with *correct* targets; it is the unmarked int64 cells — name-record slots, plain scalars — that arrive stale. The intended contrast is real (`importValue` remaps an OpHandle's target, `mesh_value.cpp:386-435`; `resume` overlays raw bits, `mesh_actor.cpp:260-271`) but the sentence as written mislabels which cells arrive in which kind.

**Fix.** Reword: "resume overlays raw bits — marked cells arrive still `OpHandle`-kind (targets correctly promoted) and rehydration converts them to `Int64`; unmarked int64 cells, name-record slots among them, arrive stale and are rewritten through their records."

### 4. MINOR — the `d.validate` pass serial's provenance is unstated; a per-incarnation serial can alias a stale promoted tag (§7.2 `:1471-1479`)

**Problem.** Membership = edge exists ∧ `textOf(tag) == serial` — correct — but the serial's source is never named. Tags ride `promote` on `d.code`-registered fragments, so a stale tag survives resume; a serial minted per-incarnation can collide with it and treat never-visited cells as visited — a silently skipped validation subgraph. The doc already owns the right mechanism: the durable per-agent counter on `d.vars` that `reify`'s `<serial>` draws from (`:1407-1409`).

**Fix.** One clause: "the pass serial is drawn from the durable per-agent counter on `d.vars` (the `reify` label counter, §6.7), so a serial a promoted tag carries can never be re-issued."

### 5. MINOR — the genesis name-record chain assumes `+d.values` always lands on a head (§6.6 `:1320-1322`)

**Problem.** The chain is spelled `symbol_named(qname) → +d.values → head → +d.entry → entry op`. But `exportSymbol`'s existing shape links `symCell +d.values → entryOp` **directly** — no head (`vortex_core.cpp:119-124`). For program-original symbols, `+d.values` yields the entry op itself; the `+d.entry` hop finds no edge and the patch drops as unresolvable. §6.1 already owns the fix — "resolves the linked cell directly when it is an op, and follows `+d.entry` once when it is a head" (`:943-946`).

**Fix.** Apply the same discrimination in the chain: "resolve `symbol_named(qname) → +d.values`; the result is the entry op directly when it is an op cell, else follow `+d.entry` once — the head/op discrimination of §6.1."

### 6. MINOR — the `program()` reachability test cannot see int-referenced program cells (§6.6 `:1231-1234`, `:1294-1296`)

**Problem.** Program-side = reachable over named *link* dims. But `Jump`/`Branch`/`Call`/`Choice` targets are int64-encoded (`vortex_vm.cpp:839-873`, `:1150-1168`; stated at `:1172-1173`), so a program op reachable only through int content is not on the walk — an outside edge to it classifies state-side and passes `closed`. In vagent-emitted programs this is near-unreachable (no link path to such a cell exists for a cells-path to capture), and forming the edge otherwise is the raw-surgery class `:1294-1296` already disclaims — but that clause names smuggled edges on non-op-graph dims; an endpoint invisible to the walk on a *walked* dim is a different gap.

**Fix.** Extend the door clause one sentence: "…or an outside edge whose endpoint is a program cell reachable only through int64 content (control-flow targets) — the `program()` walk is link-bounded and cannot classify it; both are §11-door limitations, not claimed soundness."

### 7. MINOR — "`checkpoint` under checkpoint-only marking requires `outstandingMarks() == 1`" contradicts the actor API (§6.4 `:1157-1158`)

**Problem.** The aside says a checkpoint may hold one mark. `checkpointView` holds no mark and refuses `outstandingMarks() != 0` outright (`mesh_actor.cpp:672-675`). The only `== 1` regime in the tree is `hydratePrefix`'s floor (`arena_manifold.cpp:2123`, drained by `evictOldestMark` via `stealNearRootChoicePoint`, `vortex_vm.cpp:289-301`) — a parallel-worker import artifact (`or_parallel.cpp:349`), not the actor path. As written, the sentence tells an implementer the actor checkpoint tolerates a mark it cannot hold.

**Fix.** Delete the aside (the actor API is `== 0`) or rewrite it to name the hydration floor explicitly and state the actor rule as `checkpointView` enforces it.

### 8. MINOR — the marker-cardinality paragraph appears twice (§6.1 `:923-930` vs `:932-936`)

**Problem.** `:923-930` and `:932-936` state the same three facts — per-ref marker, the single-`-d`-neighbour eviction reason, and the `+d.ref` inventory role — an edit-merge artifact keeping both the expanded and the original paragraph.

**Fix.** Keep `:923-930` (it carries the U-M7 justification); delete `:932-936`.

### 9. MINOR — `reify`'s "`+d.refs` regeneration" should name marker re-minting explicitly (§6.1 `:968`; §6.6 `:1338-1344`)

**Problem.** `reify` regenerates `+d.refs`/`+d.recipe` (`:968`). Remapped records alone leave the copy's ref cells unmarked — they would promote verbatim-stale at the next checkpoint, the unmarked-ref failure through another door. "Both exist per marked cell" (`:927-930`) presumably intends markers to regenerate too, but an implementer reading only the table row will produce one.

**Fix.** Amend the row: "regenerated `+d.refs` records *and* fresh per-ref-cell `d.mesh.relocation` markers — the copy's slots are marked exactly as an emission's are."

### 10. MINOR — the P16 row's "Where" calls `fragment.{hpp,cpp}` "the adapter's emission" — it is vagent/kernel-side (P16 row `:1846`)

**Problem.** §10.1 places `fragment.{hpp,cpp}` under `apps/common/xanadu/vagent/`, glob-picked (`:1799-1801`). The adapter *drives* the ceremony (flag, timing, genesis ordering) around emission the vagent compiler performs. The conflation invites putting marking logic in adapter code, and the flag's concrete home — the adapter TU that wires `pumpOne`/`executor_` — goes unnamed.

**Fix.** Reword the Where: "emission-side marking in vagent `fragment.{hpp,cpp}` (kernel glob); the flag protocol, checkpoint passes and genesis rehydration in the adapter's executor TU."

## Structural notes (no finding)

- **Two-reify `learn` ownership is consistent.** The registered head is a durable template on `d.code` — never installed, free `-d.grab`/`-d.spin` backlinks so it stays re-graftable — while the installed copy is program-adjacent and dies with the skeleton. The original is never registered; patch chains record heads *by name* so records never sever each other (`:1266-1273`); genesis ordering (d) before (e) propagates rehydrated name-records into replayed installs, matching the divergence clause (`:1323-1326`); `unlearn` removes the rank edge when the last naming patch is undone (`:1254-1255`). No new asymmetry found.
- **No forbidden coupling.** The adapter already supplies the `Executor` and drives `pumpOne`; the flag lives entirely in adapter state; `publishValue`, `promote`, `isCellRelocationHandle`, and `mesh_actor.{hpp,cpp}` are reused as-is — the row's "no new core mechanism" claim is accurate.
- **Restoration-before-execution holds by construction.** Ops execute only inside `executor_` (`mesh_actor.cpp:473-479`); a head-of-turn drain in the same callback precedes the turn body on every subsequent turn, including wake turns and post-publish-failure retries; a stopped actor never re-enters `pumpOne`, so no restore is owed on `ActorDown`.

## Verdict

The r3 agenda is fully discharged — all twelve findings resolved in substance. The P16 flag protocol is the right mechanism in the right home, but it is new this round and under-specified at exactly its edge cases: the empty-mailbox window (finding 1) leaves a path to silent verbatim promotion or checkpoint starvation, and the flag's phase/kind-guard precision is unfinished. Everything else is precision work.

VERDICT: NOT CONVERGED
