# r4-codebase: Round-4 codebase verification of `design/vagent-metaprogramming.md` v1.3

**Orchestrator summary (≤200 words):** v1.3 resolves 9 of 10 r3 findings; codebase-7 is resolved (frame-cell carve-out now named at :1202). Counts: **1 MAJOR, 6 MINOR, 0 BLOCKING**. Top three: **(1) MAJOR** — §6.6's program-side test (:1231-1236) names a `program()` walk of `{d.stdlib, d.vars, d.clause, +d.grab, +d.spin, +d.step}` that cannot reach *any* fragment head — heads' structural in-edges are `+d.code` (the seal, :1217-1226) and symbol `+d.values` (:1098, :1321), and `+d.values` is also the spine hop to the whole program skeleton. As written nothing classifies program-side; naively adding dims misclassifies grafted runtime cells hanging on `+d.clause` ranks. Needs a mint-window (`birthOp`) bound or rank-membership discriminator; the same broken walk underlies `closed`'s state-side arm (:1284). **(2)** Flag turn walks only registered heads' `+d.refs` (:1352) — unlearned grafts survive structurally with stale Call slots. **(3)** `navigate_to "G.travel"` example (:1327) conflates callee binding with string argument. **VERDICT: NOT CONVERGED.**

## Verdicts on r3 findings

- **codebase-1 (`d.validate` seen-set) — RESOLVED.** v1.3 adopts per-visited-cell tag cells: `visited +d.validate → tag_i`, fresh tag per visited cell per pass, membership = edge exists **and** `textOf(tag) == serial` (:1471-1479). Mechanically sound against `link`'s eviction semantics (`arena_manifold.cpp:1347-1379`): each tag is a distinct cell whose own `-d.validate` slot is always free, so links can never evict another visited cell's tag, and re-marking replaces a stale tag in the visited cell's single `+d.validate` slot. Trailed writes released on backtrack; stale tags fail the serial compare. Residual wording nit is finding 6.
- **codebase-2 (shared relocation marker) — RESOLVED.** Emission mints one `d.mesh.relocation→"cell"` marker *per ref cell* (:923, :1340-1344), matching the `mesh_evaluator.cpp:735` precedent (marker inside the per-reference loop, dim cell outside). `+d.name` on a record is the name-record discriminator — no marker, never re-kindled (:1314-1319).
- **codebase-3 (`["result"]` second dimension) — RESOLVED.** The descriptor record's result row now uses `+d.name` (:903-904), consistent with the name-record schema.
- **codebase-4 (recipe tuple shape) — RESOLVED.** `+d.recipe` tuples are per-field records (:916-917).
- **codebase-5 (`value_of` table row) — RESOLVED** (:967).
- **codebase-6 (`d.entry` missing from traversal dims) — RESOLVED.** `+d.entry` is now an op-graph traversal dimension (:1470: "`+d.spin`, wings, `+d.step`, `+d.entry`").
- **codebase-7 (Call-frame carve-out) — RESOLVED.** The "frame-cell carve-out" is now named in the P15 exemption list (:1202: "`Call`'s frame writes — the *frame-cell carve-out*, per-activation `makeCell` output (`:854-893`)") and the P15 row (:1845). §6.6's remaining carve-out (:1347-1348) is scoped to *reference cells* — activation frames are never published (spec §4 checkpoint table: "Activation frames … no"), so they cannot reach the marking ceremony. One cosmetic note: "a `Linked`-born ref box is re-minted" (:1347-1348) is loose for ref-boxes published inside `Cell`-typed values, which stay stale — but §6.7's session-handle accounting (:1395-1398) already owns that truth.
- **codebase-8 (`plan_named` qualification) — RESOLVED.** Three-part `module:Agent.name` parsing with first-`:` split rule (:1405-1408), `demo:Forager.patch:near_water` example (:1407, :1688).
- **codebase-9 (six citation residuals) — RESOLVED.** All verified against the live tree:
  - `:1506-1612` split into `:1506-1570` (trail) and `:1612-1700` (release) — :85, :508-511 ✓
  - "rewinds the PC" correctly re-attributed to `backtrack`/the drain, not `release` — :298, :1136 vs `vortex_vm.cpp:234-243` ✓
  - Executor catch cite widened to `:473-486` — :301, :1164; actual seam `mesh_actor.cpp:473-479` + failure path `:485-505` ✓
  - `compiler.cpp:224-498` case labels + `:500-503` refusal — :457; verified `compiler.cpp:108` mark-guard cite (:438, :1210) ✓
  - "three cells plus an optional result type" — :439-440 vs `compiler.hpp:28-34` (`CompiledStatement{entry,result,produced,resultType}`) ✓
  - Sixth residual (mark-guard/`opcodeMap_` framing) consistent throughout.
- **codebase-10 (`unlearn` detach semantics) — RESOLVED.** `unlearn(clause_or_symbol)` matches by the patch target's resolved qualified name and detaches (:1278-1280, :1254-1255); `unlearn` never installs a named head in place — installs a `reify` (:1273-1274).

## v1.3 mechanism verification (all confirmed against the tree)

- `insertIntoRank` idempotent by contract (`arena_manifold.hpp:624-628`) and implementation (`rankContains` early-return, `arena_manifold.cpp:1430-1432`) — supports the `learn` idempotence claim (:1237-1241).
- Per-ref marker construction `makeCell("cell")` produces readable text (`arena_manifold.cpp:1245-1251`, `textOf` :1110-1155); `mesh_evaluator.cpp:723-735` per-reference precedent confirmed.
- `isCellRelocationHandle` requires `ValueKind::OpHandle` *first*, then the `d.mesh.relocation→"cell"` edge (`foreign_op_handle.cpp:98-117`) — supports the kind-and-marker claim (:1355-1357).
- Executor seam: `Executor` injected via `setExecutor` (`mesh_actor.hpp:113-127`, `mesh_actor.cpp:89-90,:209`), invoked inside `pumpOne` with the mutable `*actor->context` (`mesh_actor.cpp:473-476`; `WorkerContext::arena` public/mutable, `worker_context.hpp:63`); `checkpointView` is `const` returning `const ArenaManifold&` (`mesh_actor.cpp:668-694`, `mesh_actor.hpp:102-105`) and refuses non-quiescent actors (`running`, mailbox, `choiceDepth`, `outstandingMarks`, invalid root — :669-680); `publishValue` takes `const ArenaManifold&` (`mesh_value.hpp:69-74`). The flag protocol's three positions (end-of-turn re-kind inside the callback, between-turns publish on `const&`, head-of-next-turn restore) all exist. Restore is self-describing: `Int64(*handleTarget)` recomputes the original int, so no `(cell→int)` side record is needed (:1360-1361).
- `promote` follows links both directions (`arena_manifold.cpp:1781-1783`), copies Int64 verbatim (:1934-1936), translates `OpHandle` only (:1947-1951); ephemeral-target gate `isEphemeral(*target)` → `isCellRelocationHandle && seen.contains` else `nullopt` (:1802-1806); base-target arm `store.getCompactOp` (:1809) — all as claimed (:1362-1364, :1219).
- `importValue` *does* leave relocation-marked cells `OpHandle`-kind with remapped local targets (`mesh_value.cpp:494-501`) while `resume` overlays without it (`mesh_actor.cpp:260-271`) — the kind-precision distinction at :1372-1375 is correct.
- `writeValue` covers Double/Int64/Bool/None+content with `masterOf` clone-awareness (`vortex_core.cpp:196-229`) — P15's funnel description (:1192-1207) is accurate; `splice` delegates to `writeValue` (:251-254).
- Writeback exclusion set `{New, Clone, Link, Break, Linked}` verified (`vortex_vm.cpp:1288-1290`, `runPipeline` :340-342) — doc's "every non-structural kind" (:1197-1198, :319) is the accurate complement.
- `compiler.cpp:769-772` `dimConstant` mints `literal(int64(dim))` — ref cells as claimed (:1339).
- Frontend status correctly framed as proposal: `PrefixedStringLiteral` exists (`token.hpp:22`, `lexer.cpp:379-397`), parser accepts only `cells` (`parser.cpp:848-855`), `ExprKind` lacks `Code`/`Splice`/`Static` (`ast.hpp:37-88`), `validateType` scalar set lacks `Code` (`checker.cpp:553-567`), import allowlist is `std:agent`/`std:goal` only (`checker.cpp:2061-2067`), cells-path filters reject assigned values (`cell_path.cpp:64-69`) and creations are literal-only (`cell_path.cpp:133-139`) — all consistent with the doc's "owed" labels. Makefile: new `vagent/*.cpp` files are glob-picked (`Makefile:643`), but new *frontend* files need `VAGENT_FRONTEND_SRCS`/`VAGENT_SYNTAX_OBJS` list edits (:688-690, :695) — consistent with the doc's claims where made.
- P15/P16 numbering correctly continues the spec's P1–P14 register (`vagent-agent-language.md:10`, `:1677`).

## Findings

### Finding 1 — MAJOR

- **Location:** §6.6, lines :1231-1236 (sideness test); same broken walk reused by `closed`'s state-side arm at :1282-1285.
- **Problem:** The stated program-side test — *"reachable from `program()` by a bounded walk over the program's named dimensions — `d.stdlib`, `d.vars`, `d.clause`, `+d.grab`, `+d.spin`, `+d.step`"* — cannot reach **any** fragment head, so as written essentially nothing classifies program-side. A fragment head's only structural in-edges are (a) membership on a `+d.code` rank — the seal itself (:1217-1226), rooted at the program cell for sealed heads and at the agent/clause cells for learned heads — and (b) a symbol's `+d.values` (:1098, :1321). Neither `+d.code` nor `+d.values` is in the listed walk. Worse, the entire program skeleton sits one `+d.values` hop behind the `d.vars` name cells: `program() → d.vars → symbol → +d.values → goal/clause/op-graph` (spec §5.7:627-633; `exportSymbol` binds `+d.values`, `vortex_core.cpp:100-106`), so the listed walk stops at name cells and never reaches clauses, op graphs, or sealed heads at all. The claim *"A `code"…"` argument passes the test"* (:1234) is false as literally written.
- **Evidence:** head in-edges per the doc itself — `+d.code` registration (:1217-1226) and symbol `+d.values` (:1098, :1320-1322); spec §5.7 module spine (:627-633); `exportSymbol`'s `+d.values` binding (`vortex_core.cpp:100-106`). `closed` inherits the same walk at :1284 ("state-side means link-reachable and *not* on the `program()` walk"), so direct links to program cells classify as arm (a) "link to a state-side cell" and are accepted — accepting exactly the cell-bound program references the name-record machinery exists to reject.
- **Why naive extension fails:** adding `+d.values`, `+d.clause`, `+d.code` reaches the skeleton but then also reaches runtime-minted cells hanging on program ranks — a `graft_clause` product appended to a program cell's `+d.clause` rank (spec §5.7:629-633) and clause-side `+d.code` registrations rooted at grafted clauses (:1248-1249) classify program-side, so `learn`/`reify` of learned or grafted heads would misclassify and refuse the durable path the feature exists to provide.
- **Concrete fix:** the discriminator must be a *mint boundary*, not a bare reachability walk. Options: (i) define program-side = reachable from `program()` **and** `slot(ref)->birthOp` within the program's own mint window — the boundary the genesis `programRef` bind (step (c), :1368-1369) already marks; `birthOp` is already recorded per slot and consulted by `promote`/`importValue` (`arena_manifold.cpp:1798-1799`, `mesh_value.cpp:399`); or (ii) for `Code`-typed arguments test rank-membership on `program()`'s `+d.code` (the seal) plus `symbol_named`/`plan_named` resolution for clause-cell arguments. Also extend the walk list to the program's actual structural dims (`+d.values`, `+d.clause`, `+d.code`, `+d.entry`, `+d.refs`, `+d.recipe`, `+d.name`) so the walk traverses the skeleton it claims to traverse.

### Finding 2 — MINOR

- **Location:** §6.6, :1351-1353 (flag-turn `+d.refs` walk).
- **Problem:** The re-kind walk enumerates *"each registered head's `+d.refs`"* — but `run`/`graft_*` **never register** (:1224-1225, :1402). An installed-but-unlearned grafted clause is still published (its `+d.clause` rank edge promotes as an ordinary link), while its marked `Call`/ref slots are never OpHandle'd → they promote as Int64-verbatim → stale. The checkpoint thus restores a graft that *looks* alive but whose program-symbol calls silently alias — the exact hazard class the doc documents for `Cell`/`Code` values (:1395-1398) without naming it here.
- **Evidence:** `run`/`graft_*` never register (:1224-1225); `+d.refs` records exist from emission regardless of registration (:1338-1344); promote walks all links from `stateRoot` (`arena_manifold.cpp:1781-1783`).
- **Concrete fix:** either state the boundary explicitly — "a session-installed graft's reference slots are not re-kindled; `learn_*` is what makes a graft's references durable; unlearned grafts may structurally survive a checkpoint with stale calls" — or walk every `+d.refs`-bearing head reachable in the published graph, which deliberately extends ref-durability to session fragments (a design choice to take or refuse, not leave implicit).

### Finding 3 — MINOR

- **Location:** §6.6, :1327-1329.
- **Problem:** *"`navigate_to "G.travel"` inside the learned body is a name-recorded `Call`, rebound at genesis to whatever `G.travel` then names."* The name-record carries the **callee's** qualified name — `…navigate_to`, per :1313-1314's own `+d.name → the callee's qualified name` — not the string argument. `"G.travel"` is opaque `Str` content; genesis rebinds the callee *binding*, never operand content. The example also mismatches §9.3's own snippet (`navigate_to("water")`, :1724).
- **Concrete fix:** recast as e.g. `code"{ navigate_to(\"water\") }"` — *"the `Call` to `navigate_to` is a name-recorded `Call` (`+d.name → "demo:Forager.navigate_to"`), rebound at genesis to whatever `navigate_to` then names."*

### Finding 4 — MINOR

- **Location:** :1317-1318.
- **Problem:** `(which enqueues \`handleTarget\` into \`seen\` — \`arena_manifold.cpp:1797-1809\`)` cites the *gate*, not the enqueue. The `handleTarget`→`seen`/`queued` enqueue is `:1759-1763` (the `isCellRelocationHandle` discovery arm); `:1797-1809` is the later gate that reads `seen` and returns `nullopt`.
- **Concrete fix:** cite `arena_manifold.cpp:1759-1763` (enqueue) alongside `:1797-1809` (gate), or `:1759-1809` for the whole mechanism.

### Finding 5 — MINOR

- **Location:** :1155-1156.
- **Problem:** *"a `release(m0)` may run only when `m0` is the innermost outstanding mark (`choiceDepth` zero **and** `outstandingMarks` zero — the mark is innermost by LIFO…)"* is self-contradictory: if `outstandingMarks()` is zero, `m0` is not outstanding at all. The intended condition (matching the symmetric clause at :1157-1158, "requires `outstandingMarks() == 1` — its own mark, nothing else standing") is `choiceDepth() == 0 && outstandingMarks() == 1` — `m0` itself, nothing else.
- **Concrete fix:** change `outstandingMarks` *zero* → `outstandingMarks() == 1 — m0 itself, nothing else standing`.

### Finding 6 — MINOR

- **Location:** :1471-1479 (`d.validate` tag cells), :964 (`validate(c: Cell): Bool`, declared pure).
- **Problem:** membership is specified as `textOf(tag) == serial` and "the tag's text/content holding the pass serial" — but `textOf` is a C++-only API (`arena_manifold.hpp:543-548`); no Vortex opcode reads raw cell text as `Str`, so the mechanism only works if `validate` is a C++-side `std:meta` builtin (like the `vortex_stdlib.cpp` members). If `validate` is an op-graph, the serial must be scalar content. The serial's *source* is also unnamed.
- **Concrete fix:** pin the representation — "each tag is a freshly minted `Int64` scalar cell holding the pass serial" (readable by `Value`/scalar read either way) — and name the serial source, e.g., the same durable per-agent counter on `d.vars` `reify` already uses (:1408-1409), or a per-pass counter cell minted at walk entry.

### Finding 7 — MINOR

- **Location:** :1320-1322 and :1370-1371 (genesis name-record rewrite chain).
- **Problem:** the chain `symbol_named(qname) → the symbol's +d.values → the head → the head's +d.entry → the entry op` presumes the owed "uniform head convention" — a symbol's `+d.values` holding a *head*. Today's `exportSymbol` binds `+d.values → the entry op` directly (`vortex_core.cpp:100-106`), with no `+d.entry`. On artifacts compiled before the head convention lands, the `+d.entry` hop finds nothing.
- **Evidence:** `vortex_core.cpp:100-106` (`firstOf(rankAfter(...))` — the bound cell is the op); the doc's own reader-side discriminator (:943-944, "resolves the linked cell directly when it is an op, and follows `+d.entry` once when it is a head").
- **Concrete fix:** one clause — "apply the §4.4 `+d.entry`-presence discriminator at the `+d.values` hop: edge present ⇒ head, follow once; absent ⇒ the cell *is* the entry op" — which also makes the rewrite correct for pre-head-convention checkpoints.

VERDICT: NOT CONVERGED
