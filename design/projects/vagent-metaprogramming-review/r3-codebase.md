# r3-codebase.md — Round-3 codebase critique of `design/vagent-metaprogramming.md` v1.2

Scope: verification of every r2 finding and every v1.2 file:line citation against the live tree (`apps/common/xanadu/{vagent,vortex,zigzag,vql,mesh…}`), plus the sibling specs the doc anchors on.

## Part 1 — Verdicts on the r2 codebase findings

**codebase-1 — `d.validate` seen-set — NOT RESOLVED, still BLOCKING.** v1.2 applied R2-C's direction reversal verbatim (`:1315-1319`: `visited +d.validate → marker`, membership `linked(visited, d.validate, POS) == marker`), but the mechanism is broken in *both* directions. `link` evicts on both ends — `displacedIt = marker->neighbor(-dir)` then `setOneSide(v1, POS, noCell)` (`arena_manifold.cpp:1352-1356`,`:1369-1373`; contract "evicts whatever either end held", `arena_manifold.hpp:594-618`). Each `link(v_{n+1}, +d.validate, marker)` evicts the marker's single `-d.validate` backlink and **clears v_n's mark**. The set holds at most the last-marked cell; cyclic graphs still spin to `maxCycles` death. The disposition's premise "N backlinks on the marker for free" (r2-disposition:69) is mechanically false. Fix below (finding 1).

**codebase-2 — `run` argument typing — RESOLVED.** `run(c: Code<T>, args): T` is now a checker special form; `args` is a list literal typed per-element by a fresh `any` wildcard, heterogeneous legal only there (`:226-232`,`:733-736`,`:915`,`:976-986`,`:1907-1909`). Elements bind on the `#Call` frame by parameter name as *values* — consistent with spec §5.8 (`vagent-agent-language.md:645-652`). This is exactly the recommended reading.

**codebase-3 — `result`/`produced` reachability — RESOLVED, minor residual.** The prose now says the descriptor links to "the `result`/`produced` *cells themselves*" (`:741-743`,`:984-985`), which are `compiler.cpp:849-871`-minted cells (`output` :858, `produced` :859). `["produced"]` row shows `+d.values`→produced cell (`:867`). The `["result"]` row (`:866`) is ambiguous — see finding 4.

**codebase-4 — ten-citation batch — PARTIALLY RESOLVED.** Fixed: `:255` (`Cut` discard), `:1612-1700` (`release` — now correct at :1082-1090), `:170-172` (`enableMemoization` body :169-171 — lands correctly), `cell_path.cpp:147-183` acceptable alongside §4.4's `:17-143`. Persisting — see finding 9.

**codebase-5 — wildcard `Code` — RESOLVED.** Bare `Code` is now an explicit parameterized wildcard distinct from `any` (`:181-182` M3, `:929-931`, `:1610` §10.1) — `checker.cpp:27` confirms `any` accepts `Cell`/`Str`, so a new form is correctly required.

**codebase-6 — `plan_named` qualification — PARTIALLY.** The §9.2 worked example now uses `plan_named("demo:Guard.cautious")` (three-part `module:Agent.plan`, matching spec §5.7:640 `module:Agent.travel` and the doc's own `:1183`,`:1378`), but `plan_named("G.p")` at `:934` and the table's "qualified `Agent.plan`-style name" at `:910` remain two-part. MINOR residual (finding 8).

**codebase-7 — grounding citation — RESOLVED.** §11's closing paragraph (`:1689-1700`) now frames the "why not Vortex" rule as project policy with `vagent-runtime-grounding.md` cited as the audit/evidence record — correct attribution.

## Part 2 — Re-verification of v1.2's new mechanism claims (all confirmed unless noted)

- **Marker edge inert while `Int64`:** `isCellRelocationHandle` checks `ValueKind::OpHandle` *first* (`foreign_op_handle.cpp:100,109-117`) — the `d.mesh.relocation` edge is inert on `Int64` cells. ✓
- **`arena_manifold.cpp:1797-1813` unmarked-OpHandle promotion failure:** verified — the loop rejects ephemeral-target `OpHandle`s lacking `isCellRelocationHandle` or a discovered target (`:1800-1805` `return nullopt`). ✓
- **`mesh_actor.cpp:260-271` resume bypasses `importValue`:** verified — `resume` (`:182`) calls `receiveValue` (`:260`) and wraps the received manifold directly in `WorkerContext` (`:270-272`); `importValue` lives separately in `mesh_value.cpp:387`. ✓
- **`compiler.cpp:475-483` `$`-anchor:** verified — `bindings_.find(anchor.variable)` :476, `Cell`-type requirement :480-483, mutating refuse :484-486. ✓
- **`vortex_vm.cpp:854-893` frames:** verified — `Call` :854-876 (frame mint :860, `setValueBits(Int64, nextOp)` :861-863, `+d.stack` link), `Return` :878-894 (stack re-point :886-889). ✓
- **`promote` internals:** `Int64` verbatim `:1934-1936`; `OpHandle` remap `:1947-1954`; BFS both directions `:1782-1783`; `d.dims` skip `:1766`; handle discovery `:1759-1764`; reachability-order mint `:1863`. ✓
- **`link` reciprocal eviction:** `arena_manifold.cpp:1347-1377` + contract `hpp:594-618`. ✓
- **`insertIntoRank`/`rankContains` exist:** `arena_manifold.cpp:1381-1421`,`:1423-1439`. ✓ (load-bearing for finding 1's fix)
- **`mesh_evaluator.cpp:720-736` precedent:** re-kinds `Int64`→`OpHandle` (`:733`) and links `arena.makeCell("cell")` **inside the per-reference loop** (`:735`) — per-ref markers, not shared. Load-bearing for finding 2. ✓
- **`handleTarget` public:** `arena_manifold.hpp:539`, `arena_manifold.cpp:1091`; used by `promote` `:1800`,`:1948` and `mesh_evaluator.cpp:1036`. ✓
- **Frames/mark/trail:** `Mark` has no dimension field (`arena_manifold.hpp:170-186`); well-nested assumption `:773`; release pops top floor only `:1695-1698`; ghost-slot path `:1657-1698`; stdlib drain `vortex_stdlib.cpp:3800-3802`,`:3841-3843`. ✓
- **Compile-side cites:** `dimConstant` :769-772; `let` mint :853-855 + copy :705-707; `validName` `$` refuse :407-417; `bindings_.find` :476; `localReferences_`/`externalReferences_` :884-891; `requireSame` :189-193; emitStatement :849-871; `mint`/`reference`/`spin` :512-522; `cells` emit :791-799; `compiler.cpp:108-110` quote verbatim. ✓
- **VM cites:** `resolveCell` :50-67; `opcodeMap_`/`mintOpcode` :73-82; kind ceiling :108-110; `cursorPC_` untrailed `hpp:186-188`; Choice mark :217 + target check :1156-1158; `backtrack` `setCursorOpcode` :238-243; `Mul` overflow :457-463; `Linked` :654-723; `New` append-refusal :736-743; `Value`/`Link`/`Break`/`New`/`Splice`/`Bind` :771-904; int-encoded targets :839-873,:1150-1168; `Halt` :1220-1225; step advance :1333-1335; `run(cursor, maxCycles)` :1344-1345. ✓
- **Core/VM support cites:** `initGenesis` is `VortexCore::initGenesis()` `vortex_core.cpp:29` (`d.name` mint :61); `findModule` first-match :100-127; `d.vars`/`exportSymbol` :181; `writeValue` funnel :196-254 (`masterOf` :202, value arms :203-227); wing convention :354-414 (`bindInput` :384 POS, `bindOutput` :402 NEG); `CellValue` variant `vortex_core.hpp:29`. ✓
- **Lexer/parser/checker:** `$` reject `lexer.cpp:333-335`, charset :14-23, prefix merge :379-398; `code` prefix absent today (proposed, `parser.cpp:848-855` only accepts `cells`); `Type{name:"Code"}` generic syntax parses :296-313; `ExprKind` has no `Code`/`Splice`/`Static` (ast.hpp:37-88 — "owed" framing accurate); `snake` env `checker.cpp:30-36`; builtin table :765-779; `pureBody` :827; mutating-cells impure :861-865; import allowlist :2061-2067; `for` env-copy :1505-1524. ✓
- **Build:** `Makefile:643` glob picks up new `vagent/*.cpp`; `VAGENT_FRONTEND_SRCS` explicit list :688-690; `VAGENT_SYNTAX_OBJS` :695. ✓
- **Spec/text cites:** `§5.7:640` qualified names; `§5.8:643-653` frame convention verbatim ("a temporary that must live across a call… spilled to the frame" :651-652); `§5.9:670` `Cell` = ref-box; `§8.1:1045-1046` prefix rule; `V10:224-234`; `V12:244-250`; `V13:252-257`; `vortex-hyperstructural-runtime.md:1047-1049` quote; `vagent-runtime-grounding.md` and `projects/vagent-review/r10-vortex.md` exist. ✓

## Part 3 — Findings

### 1. BLOCKING — `d.validate` seen-set still degenerates to one element

**Location:** §7.2, `design/vagent-metaprogramming.md:1315-1319`.

**Problem:** The R2-C direction reversal was applied (`visited +d.validate → marker`), but `link` eviction is two-sided: the marker cell's `-d.validate` slot holds exactly **one** backlink. Marking `v_{n+1}` evicts `v_n`'s edge. The seen-set holds only the most recently visited cell; on any cyclic op-graph (`Jump`/`Call` back-edges — precisely where the doc says a membership test is needed, :1322-1323), the walk never recognizes a revisit and burns `maxCycles` until budget death. `graft` runs `validate` on every installation (`:1324`), so every non-trivial graft fails.

**Evidence:**
- `ArenaManifold::link` computes `displacedIt` from the *target's* opposite-side neighbour and clears it: `arena_manifold.cpp:1352-1356` (`theirs = existingLink(to); displacedIt = theirs->neighbor(-dir)`), `:1369-1373` (`setOneSide(*displacedIt, dim, dir, noCell)`).
- Header contract: "Maintains both ends and evicts whatever either end held" — `arena_manifold.hpp:594-618`.
- The doc's own parenthetical only reasons about the *visited* side ("a `visited` cell keeps at most one `+d.validate` edge…", :1318-1319) — the same eviction applies symmetrically to the marker side and was missed.

**Concrete fix:** make the seen-set a **marker-anchored rank**: `insertIntoRank(visited, marker, d.validate, POS)` to mark (builds `marker +d.validate → v1 → v2 …` chained on `d.validate`), membership `rankContains(marker, visited, d.validate)` — both primitives exist (`arena_manifold.cpp:1381-1421`,`:1423-1439`). O(n) membership matches the doc's stated `O(n·membership)` pricing (:1322). The per-pass fresh marker still gives staleness immunity: stale edges hang on chains rooted at dead markers and `rankContains` never crosses roots; re-marking a cell under a new pass's marker steals it out of the dead chain, which is harmless. Alternatively `visited +d.validate → markSlot_i` with per-visited slots named by the pass — the rank form is simpler and uses one dim.

### 2. MAJOR — P16 `"cell"` marker cardinality is unspecified; the "shared" reading self-evicts

**Location:** §6.6 P16 item 1 (`:1212-1219`) and the head-layout diagram (`:879`,`:882-886`).

**Problem:** The doc says every emitter-minted int64 ref cell "gets the inert `d.mesh.relocation→"cell"` marker edge" but never states whether the `"cell"`-texted marker is shared or per-ref-cell. The r2-disposition explicitly prescribed "**one shared `"cell"`-texted marker (minted once per pass)**" (`r2-disposition.md:22-23`), and the doc neither adopts nor corrects it. Under a shared marker, each `link(ref_i, +d.mesh.relocation, S)` evicts the previous ref's edge (S's single `-d.mesh.relocation` backlink — same two-sided eviction as finding 1): only the last-linked ref cell retains a marker. At promote, every other re-kinded `OpHandle` fails `isCellRelocationHandle` → `promote` returns `nullopt` (`arena_manifold.cpp:1797-1813`, refusal `:1803-1805`) → `publishValue` fails for any fragment with ≥2 ref cells — i.e., every fragment (wing slots, `reference()` cells, `dimConstant` slots, :1212-1214).

**Evidence:**
- Per-ref minting precedent: `arena.makeCell("cell")` executes **inside** the per-reference loop, `mesh_evaluator.cpp:735` (in the `for` over references beginning :723).
- Two-sided eviction: `arena_manifold.cpp:1369-1373`, `arena_manifold.hpp:596-598`.
- `isCellRelocationHandle` tests the edge's *target text*, not identity: `foreign_op_handle.cpp:111-114` — per-ref markers satisfy it identically.

**Concrete fix:** one clause at `:1215` or the `:879` diagram row: "a **fresh `"cell"`-texted marker cell per ref cell**, as `mesh_evaluator.cpp:735` mints in-loop — a shared marker self-evicts, because each `link` on `d.mesh.relocation` clears the previous ref's edge." Cost note for §13: +1 cell per ref cell (~already inside "the marker edge and `+d.ref` record per emitter-minted ref cell", :1757-1758, but the cell itself should be counted).

### 3. MINOR — `["result"]` descriptor row leaves the second edge's dim unwritten

**Location:** §6.1 diagram, `:866` — `["result"] ─(+d.values)─► "Int" + result-type cell link`.

**Problem:** The authoritative prose (:741-743, :984-985) requires a link to the `result` cell *itself* for `run`'s spill, but the row shows `+d.values` consumed by the type name `"Int"` and an unwritten second edge "+ result-type cell link". `+d.values` is single-valued — the two edges cannot share it (eviction). Fix the row to name the second dim (e.g. `+d.ref`-style or an explicit record edge, consistent with `["produced"]─(+d.values)─► produced cell` at :867 which uses `+d.values` for the cell and has no type-name edge to fight). Also reword "result-type cell" → "the fragment's result cell" for symmetry.

### 4. MINOR — recipe tuples can't put two endpoints on one `+d.values`

**Location:** `reify` paragraph, `:944-947` — "each record's `+d.values` points at the actual endpoint cells" (plural).

**Problem:** A `(from, dim, dir, to)` tuple has ≥2 cell endpoints; one record's `+d.values` is single-valued. The working shape is the descriptor's own convention — per-field records under `+d.vars` (`["from"]─(+d.values)─►cell`, `["to"]─(+d.values)─►cell`, `dim`/`dir` as scalars or dim-cell links — `dim` as int64 would itself be a ref-int needing relocation or a name). One clause resolving the shape closes it; an implementer could otherwise read it literally.

### 5. MINOR — `value_of` named but absent from the member table

**Location:** named at `:995-997` (`value_of(c: Cell): any`); the §6.1 member table `:905-927` has no row. R2-F required the companion reader to be named — it is, but table completeness matters because the table is the binding surface. Add the row (pure, `any`).

### 6. MINOR — `d.entry` missing from `validate`'s traversal-dim enumeration

**Location:** `:1314` — "Traversal is restricted to the op-graph dimensions (`+d.spin`, wings, `+d.step`) and known op references".

**Problem:** R2-C required the walk bounded to "(d.spin, d.step, ±d.grab wings, `d.entry`, int-ref targets)" — `d.entry` is the head→graph entry edge the walk must cross first (`:893-895` reader convention). A literal implementer refuses to cross the head's `+d.entry`. Add `d.entry` to the list.

### 7. MINOR — runtime-minted-ref carve-out should name Call frames

**Location:** `:1217-1219` — "Runtime-minted reference cells are the carve-out: `Linked` rebuilds its rank per execution…"

**Problem:** `Linked` re-mints, but `Call` frames are also runtime-minted int64 ref cells (`vortex_vm.cpp:860-863`, `int64(nextOp)`), are *not* re-minted after resume, and outlive a turn boundary whenever an actor suspends mid-call — a quiescent suspended actor is checkpointable, so live frames carry stale dense ids after reload. This is a pre-existing vagent-runtime hole (frames belong to the §5.8 call ABI, outside P16's stated "fragment-owned" scope), but the carve-out's implied safety ("runtime-minted ⇒ re-minted") overstates: name it — "other runtime-minted refs (Call frames under suspension) are the runtime's own checkpoint problem, not covered by P16."

### 8. MINOR — `plan_named` two-part residuals (codebase-6)

**Location:** `:934` `plan_named("G.p")`; `:910` "qualified `Agent.plan`-style name". **Fix:** `demo:G.p`-style three-part, or state module elision once — the convention the doc cites (:1183, :1378, spec §5.7:640) is `module:Agent.plan`.

### 9. MINOR — codebase-4 citation residuals

- `:85` (§1 table) still cites `arena_manifold.cpp:1506-1612` for mark/release/discard — §4.3 was corrected to `:1506-1570`/`:1612-1700`; apply the same to §1.
- `:1069-1070` — "arena cells are freed only by `release` truncation — **which also rewinds the PC**" — false and self-contradicted: `release` (`arena_manifold.cpp:1612-1700`) touches only arena state; `cursorPC_` is untrailed (`vortex_vm.hpp:186-188`), and the doc's own next bullet (:1071-1073) says backtracking needs an *explicit* `setCursorOpcode` (:238-243) precisely because cell restore leaves `cursorPC_` stale. Drop "which also rewinds the PC".
- `:1076` postconditions cite `vortex_vm.cpp:1306-1318` — the evaluation block is `:1313-1322` (`:1306-1311` is the snapshot); cite `:1313-1322`. Preconditions cite `:1229-1235` should be `:1229-1239`.
- `:297`,`:1097` `mesh_actor.cpp:473-479` — the try/catch is :473-479 but `ActorDown` construction/selection is `:485-486`; cite `:485-486` (or `:473-486`).
- `:449` `compiler.cpp:500-503` for the enumerated scalar set — lands on `default:` refuse; the set is the case labels `:224-498` (the cite does support the "only" enforcement, so borderline — either cite both or reword).
- `:432` "four cells in the arena" — `CompiledStatement` is three cells + `optional<Type>` (`compiler.hpp:28-34`).

### 10. MINOR — `unlearn` of a program-original patch leaves interim semantics unstated

**Location:** `:1236-1241` — "unlearn of such a patch detaches it, and genesis restores the program's own attachment".

**Problem:** after detach, the clause's `+d.grab` (or `+d.spin`) is **empty** until the next genesis — an unguarded clause / missing body for the rest of the incarnation. Spec §5.7:629-634 defines the test/body as those edges; absence semantics aren't stated. One clause fixes it: either "the clause sits unguarded (empty `+d.grab` = no `where`) until genesis" or have `unlearn` refuse/detach the clause itself.

---

**Summary:** 1 BLOCKING (finding 1 — `d.validate` seen-set still collapses under two-sided eviction; r2's reversal moved the failure from the marker's POS slot to its NEG slot), 1 MAJOR (finding 2 — `"cell"` marker must be per-ref-cell; the disposition's "one shared marker" is self-evicting and the doc doesn't correct it), 8 MINOR. All other v1.2 claims and ~90 file:line citations verified accurate. codebase-2/3/5/7 resolved; codebase-4/6 partially.

**VERDICT: NOT CONVERGED**
