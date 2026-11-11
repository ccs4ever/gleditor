# r5-codebase: Round-5 codebase verification of `design/vagent-metaprogramming.md` v1.4

**Orchestrator summary (≤200 words):** All seven r4 findings are resolved correctly with accurate
citations — the r4 mechanism rulings landed as designed. The `birthOp` mint window is
mechanically sound: `birthOp` is recorded per slot at `arena_manifold.cpp:1183-1186`
(`refOf(dense)` = `ephemeralBit|dense`), survives shadow-overlay (`:1489-1490`, base ref carried
untouched) and the arena's own serialization (`writeU32`/`readU32`, `:2233`/`:2302`), and is
consulted by the relocation path at `mesh_value.cpp:399` as cited. Program-image cells carry
non-ephemeral (shadow) or early-dense birthOps, runtime mints carry later ephemeral ones, so the
"`birthOp` ≤ program-image boundary" test discriminates cleanly in both the baseless first
incarnation and the checkpoint-resume regime. Counts: **0 BLOCKING, 0 MAJOR, 4 MINOR**. Top
issues, all in the v1.4 genesis/flag rewrite: **(1)** `CellImport{OpHandle}` at :1467 is a
fabricated type (zero occurrences in the tree), and the sentence inverts `importValue`'s actual
semantics — marked cells stay `OpHandle`-kind with remapped *local* targets
(`mesh_value.cpp:494-501`), unmarked ones demote to `None` (`:485-490`); the cite
`arena_manifold.cpp:1934-1954` is promote's store-side mint switch, the wrong file for an
`importValue` claim. **(2)** "handles carrying pre-promotion targets" (:1468-1469) is backwards —
promote translates targets (`:1950`), and rehydration works *because* the handle carries the
promoted one. **(3)** "`MeshOp` kind" (:1442) names a `ValueKind` that does not exist
(`ops.hpp:154-163`). **VERDICT: NOT CONVERGED** — minors only, all localized.

## Verdicts on r4 findings (codebase-1..7)

### codebase-1 (MAJOR, sideness test) — RESOLVED

v1.4 :1268-1286 implements the adopted ruling faithfully:

- **Dim vocabulary** (:1270-1272): the walk now lists `d.stdlib`, `d.vars`, `d.clause`,
  `+d.values`, `+d.code`, `+d.entry`, `+d.name`, `d.name`, `+d.refs`, `+d.recipe`, `+d.grab`,
  `+d.spin`, `+d.step`, `d.contract`, `d.belief`, `d.source` — the `+d.values` spine hop and the
  `+d.code` seal that r4-finding-1 showed missing are both present, so the walk actually reaches
  the skeleton it classifies.
- **Mint window** (:1273-1278): "minted within the program's own mint window: `birthOp` at or
  below the program-image boundary the genesis `programRef` bind marks". Mechanism verified:
  `birthOp` is written per slot at `arena_manifold.cpp:1183-1186` (`.birthOp = refOf(dense)`, the
  cite is exact); shadow cells keep the *base* ref as `birthOp` (`:1489-1490`, "an overlaid cell
  is the *same* cell"), so checkpoint-arriving program cells always test in-window; ephemeral
  mints (`refOf = ephemeralBit|dense`, `arena_manifold.hpp:468-469`) post-date the boundary.
  Serialized at `:2233`/`:2302`, restored verbatim by `hydratePrefix` (`:2100`), and consulted by
  the relocation path at `mesh_value.cpp:399` (`slot(*target)->birthOp != *target`) as cited.
  `promote` writes into the store, never the session slots, so it cannot disturb the window;
  `importValue` mints fresh ephemeral-birthOp cells (`mesh_value.cpp:472`,`:481`), correctly
  outside it.
- **Grafted/learned cells stay classifiable** (:1276-1278): post-boundary mints exceed the
  window — the `d.clause`-rank graft case the r4 finding raised is handled.
- **`closed` inherits the test** (:1342-1343): "link-reachable *from the agent cell* and not
  program-side under the sideness test above" — the second broken walk is fixed at the source.
- **Int64 door clause** (:1280-1282, mirrored :1357-1359): `Jump`/`Branch`/`Call`/`Choice`
  targets are int-encoded (`vortex_vm.cpp:839`/`848`/`856`,`:1150-1168` verified) and named a
  §11-door, not claimed soundness.
- Residual nits, not findings: the "boundary" is a named watermark, never spelled out as a value
  (dense extent at the `programRef` bind); `birthOp` equality is dense-allocation order, not an
  op counter; `+d.grab` in the list vs the `-d.grab` output wing is moot under bidirectional
  walk semantics (promote discovers both directions, `arena_manifold.cpp:1782-1783`, cited at
  :1254); `symbolNamed` camelCase at :1224 vs the doc's `symbol_named` elsewhere.

### codebase-2 (unlearned-graft `+d.refs` boundary) — RESOLVED

:1442-1445 states it verbatim: "an installed-but-unlearned graft's ref cells are never re-kinded
(`run`/`graft_*` never register), so an unlearned graft may structurally survive a checkpoint
with stale calls; `learn_*` is what makes a graft's references durable." Exactly the adopted
boundary statement; no mechanism change implied.

### codebase-3 (`navigate_to` example) — RESOLVED

:1399-1403 recast correctly: the name-record carries `+d.name → "demo:Forager.navigate_to"` —
the callee's qualified name — and `"water"` is named as "just the argument". Consistent with the
§9.3 snippet and the `+d.name` schema at :947.

### codebase-4 (enqueue cite) — RESOLVED

:1382-1383 now cites `arena_manifold.cpp:1759-1764` for the `handleTarget` → `seen`/`frontier`
enqueue (verified: `:1762-1763` `seen.insert`/`frontier.push_back` inside the
`isCellRelocationHandle` arm at `:1759`) alongside `:1797-1809` for the gate (verified:
`:1802-1809` `isEphemeral`/`seen.contains`/`getCompactOp` checks). Both halves cited correctly.

### codebase-5 (release guard contradiction) — RESOLVED

:1189-1190 now reads `choiceDepth() == 0` *and* `outstandingMarks() == 1` — `m0` itself, nothing
else standing" — the self-contradiction is gone and matches the symmetric clause. The
checkpoint aside is resolved: `checkpointView`'s `outstandingMarks() != 0` refusal is at
`mesh_actor.cpp:672-675` (cited :1191-1192), the `== 1` floor is `hydratePrefix`'s
`outstandingMarks_ = 1` at `arena_manifold.cpp:2123` (cited :1193), and the parallel-worker
import artifact is `or_parallel.cpp:349` (`workerArena.hydratePrefix`) as named.

### codebase-6 (tag serial representation and source) — RESOLVED

:1577-1579: tag is "a *fresh `Int64` scalar cell minted per visited cell per pass* holding the
pass serial — the serial itself drawn from the durable per-agent `d.vars` counter, the same
source `reify`'s `<serial>` uses (§6.7); membership is the edge existing **and** `value_of(tag)
== serial`". Representation pinned to `Int64`, serial source named (matches :1509-1511's
durable-counter claim), read via `value_of` not `textOf` — all three requested fixes.

### codebase-7 (genesis `+d.values` hop / `exportSymbol`) — RESOLVED

:1388-1391 applies §6.1's head/op discriminator (defined :971-973) at the `+d.values` hop:
"the resolved cell is the entry op when it is an op (`exportSymbol` binds `sym +d.values →
entryOp` directly, `vortex_core.cpp:119-127`), else follow its `+d.entry` once (a head)".
Verified: `exportSymbol` at `vortex_core.cpp:119-127` does `link(symCell, dims_.values, Posward,
entryOp)` at `:123` — no head. (The doc's `:119-127` cite is correct; r4's `:100-106` reference
was `findModule`.) The same discriminator is re-applied at the genesis chain :1464 and the
changelog :2278 — uniform.

## New findings (v1.4 deltas only)

### Finding 1 — MINOR

- **Location:** :1466-1468 (genesis step (e), the `importValue`/`resume` kind contrast).
- **Problem:** "`importValue` converts an `OpHandle`-kind cell itself — `CellImport{OpHandle}`
  becomes `Cell` with the promoted target (`arena_manifold.cpp:1934-1954`)" — three defects in
  one sentence. (a) `CellImport` does not exist anywhere in the tree (zero matches repo-wide);
  the import path returns `ImportedValue` (`mesh_value.hpp:122`). (b) The semantics are
  inverted: `importValue` *keeps* relocation-marked cells `OpHandle`-kind with remapped **local**
  targets (`mesh_value.cpp:494-501`, `setValueBits(OpHandle, local.at(*target))`), and demotes
  *unmarked* `OpHandle` cells to `None` (`:485-490`); nothing becomes a plain `Cell`. (c) The
  cite `arena_manifold.cpp:1934-1954` is promote's store-side mint switch — the wrong file for an
  `importValue` claim. v1.3's version of this contrast was verified correct; the v1.4 rewrite
  introduced the regression. The load-bearing half — `resume` overlays without `importValue`'s
  remapping (`mesh_actor.cpp:260-271`, `WorkerContext(actor->checkpoint->manifold.get())`) — is
  verified and unchanged.
- **Concrete fix:** "importValue re-mints each marked cell `OpHandle`-kind naming the *local*
  twin of its promoted target (`mesh_value.cpp:494-501`) — while `resume`/`genesis` overlays the
  `OpHandle`-kind cells verbatim".

### Finding 2 — MINOR

- **Location:** :1468-1470.
- **Problem:** "receives the `OpHandle`-kind cells verbatim — handles carrying **pre-promotion**
  targets — and rehydration `writeValue(slot, Int64)`s each marked cell back with the promoted
  target". Wrong direction: promote translates the handle target to the promoted ref
  (`arena_manifold.cpp:1949-1951`, `isEphemeral(target) ? real.at(target) : target`), so the
  stored handle carries the *post-promotion* target — which is precisely why
  `Int64(*handleTarget)` (:1463) yields the durable id. As written the sentence is
  self-undermining: rehydration cannot read "the promoted target" out of a handle that carries
  the pre-promotion one.
- **Concrete fix:** "handles carrying their promoted targets — and rehydration
  `writeValue(slot, Int64(*handleTarget))`s each marked cell back to a plain int".

### Finding 3 — MINOR

- **Location:** :1442.
- **Problem:** "no `MeshOp` kind is checkpointed" — `MeshOp` is not a `ValueKind` and appears
  nowhere else in the tree; the enum is `None/Double/Bool/Int64/OpHandle/ExternRef/Timestamp/
  ForeignOpHandle` (`ops.hpp:154-163`). The intended claim — a re-kinded `OpHandle` cell is not
  executable opcode content (`getOpcodeKind` decodes `opcodeMap_` then `asInt64`,
  `vortex_vm.cpp:102-113`, so an `OpHandle` cell can never run as an op) — is true but named by
  a ghost kind.
- **Concrete fix:** "and no handle-kind cell (`OpHandle`/`ForeignOpHandle`) is executable op
  content, so nothing checkpointed can run as a ghost op".

### Finding 4 — MINOR

- **Location:** :1317-1319.
- **Problem:** "same-dim chains per `insertIntoRank`/`rankTail` (`arena_manifold.cpp:1423-1439`,
  the `addClause` precedent `vortex_stdlib.cpp:4238-4241`)" — the cite covers `insertIntoRank`
  (`:1423-1439`, verified, with `rankContains` idempotent early-return `:1430-1432`) and
  `addClause`'s `link(rankTail(predCell, clause), clause, clauseCell)` (`:4239-4241`, verified),
  but `rankTail` itself lives in `cell_views.hpp:258`/`:266`, not `arena_manifold.cpp`.
- **Concrete fix:** cite `cell_views.hpp:258` for `rankTail` alongside
  `arena_manifold.cpp:1423-1439`, or drop `rankTail` from the parenthetical.

## Re-verified v1.4 delta citations (all confirmed against the tree)

- `pending` = `mailbox.size()` — `mesh_actor.cpp:659-662`; `pumpOne` empty-mailbox skip `:462`;
  `checkpointView` non-empty-mailbox + marks refusals `:672-675`; `Executor` callback on mutable
  `*actor->context` `:473-477`; `ActorCheckpointView` holds `const ArenaManifold&`
  `mesh_actor.hpp:102-105`; `publishValue` takes `const ArenaManifold&` `mesh_value.hpp:69-74`;
  `WorkerContext::arena` public `worker_context.hpp:63`; stopped-actor skip `:462`/`:501`.
- `isCellRelocationHandle` requires `OpHandle` kind first, then the marker edge —
  `foreign_op_handle.cpp:98-117`; per-reference marker mint inside the loop —
  `mesh_evaluator.cpp:735`; `link` reciprocal eviction — `arena_manifold.cpp:1352-1373`,
  `arena_manifold.hpp:594-618`; `insertIntoRank` idempotent — `arena_manifold.hpp:624-628`,
  `arena_manifold.cpp:1430-1432`.
- `getOpcodeKind` map-first `vortex_vm.cpp:102-113`; `opcodeMap_` private `vortex_vm.hpp:186`;
  `mintOpcode` `:73-82`; `setCursorOpcode` writes `:139-143`; `Call` `toInt(inputs[0])` `:856`
  and frame writes `:854-877`; `Bind` `writeValue` `:904`; `Value`/`Splice` bodies `:771-806`;
  memo-hit writeback `:1269-1273`; generic writeback with `{New,Clone,Link,Break,Linked}`
  exclusion `:1287-1294`; `runPipeline` writes `:337`,`:343-346`; `New` already-used refusal
  `:736-743`; `Choice` op-target enforcement `:1156-1158`.
- `writeValue` covers Double/Int64/Bool/None `vortex_core.cpp:196-254` (no `OpHandle` arm —
  supports "no opcode can write `OpHandle`"); `splice` delegates `:251-254`; `dimConstant` mints
  `literal(int64(dim))` `compiler.cpp:769-772`; compile-time mark guard `compiler.cpp:108-110`;
  `consumeWork` throws `arena_manifold.hpp:420-426`; drain idiom `vortex_stdlib.cpp:3801-3802`,
  `:3841-3842`; `release` truncation `arena_manifold.cpp:1657-1698`; "marks are assumed well
  nested" `arena_manifold.hpp:773`; `ensureDimension` `arena_manifold.hpp:286`.
- Makefile: `COMMON_XANADU_SRCS` glob `:643` (covers `fragment.cpp`, no edit needed);
  `VAGENT_FRONTEND_SRCS` `:688-690` gains `expand`/`resolve`/`ast_clone`/`code_path`/`frontend`
  as listed; `VAGENT_SYNTAX_OBJS` `:695` correctly unchanged (no new syntax TU). `ast.hpp`
  inventory entry (:1900-1901) consistent with `ExprKind` lacking `Code`/`Splice`/`Static`.
- P4 `wake` is a real approved proposal — `vagent-agent-language.md:1670` (`wake(pid)` with
  `ActorExecution::message` optional); the empty-mailbox window argument is consistent with
  `pumpOne`'s `:462` skip and `pending`'s `:659-662` definition.
- `module_named`/`symbol_named` are defined doc verbs (:141, :984-985); the §6.7 first-`:`
  split at :1387 and :1505-1508, `code_named` walks `d.code` only (:1514-1516), and the
  verbatim-label rule are all internally consistent with R4-B's ruling.

VERDICT: NOT CONVERGED — 0 blocking, 0 major, 4 minor, all localized to sentences already written.
