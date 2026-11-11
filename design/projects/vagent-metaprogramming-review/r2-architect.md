# R2 — System Architect critique (v1.1)

Scope: layering, DRY, C++-vs-Vortex division of labour, resource lifecycle, mechanical sympathy,
configuration/magic-number hygiene, Makefile/source-list hygiene — the same lane as R1. All claims
re-verified against the tree at the cited lines. v1.1 cites cleanly; the section below gives a
verdict on each R1 finding before the new findings.

## R1 finding verdicts

1. **P15 covers only `Value`/`Splice` bodies, not writeback (MAJOR) — RESOLVED.** §6.5/M10 and the
   P15 row now site a map-membership refusal at the `VortexCore::writeValue`/`splice` funnel
   (`vortex_core.cpp:196-255` — `splice` forwards at :251-255, `insert`/`append`/`newCell` route
   through it at :241/:257-264), scoped to the write arm, with all five sites enumerated:
   `executeOpcodeBody` Value/Splice (`vortex_vm.cpp:771-806`), memo-hit writeback (:1269-1273),
   generic output writeback (:1287-1294), `runPipeline` param write (:337) and bound-output writes
   (:343-346). Fresh-cell exemption keeps `New`+`Value`, `Bind` (:904), `Call` frames (:860-863) and
   `setCursorOpcode` legal. Correct and complete.
2. **`static` arena discipline (MAJOR) — RESOLVED.** M5/§5.5 adopts R1's option (a): no-mark eval,
   explicit `+d.cursors` unlink (`spawnCursor` registers at `vortex_vm.cpp:148-161`), priced residue
   (§13), form-level closedness, literal-expressible result types only.
3. **`splice` AST channel (MAJOR) — RESOLVED with new issues.** A dedicated `expand` stage
   (`Program → Program`, M4/§5.6) with `cloneExpr`/`cloneProgram` (`ast_clone.{hpp,cpp}`, §10.1) is
   the right answer to the const-AST problem (`checker.cpp:1121`, `ast.hpp:96-109`). Residuals are
   findings 3 and 4 below.
4. **`reify` mechanism (MAJOR) — RESOLVED.** The `+d.recipe` (census + link tuples + literals +
   old→new map) is stated (§6.1), `Clone` is refused on the correct ground — `masterOf` resolves
   reads *and writes* through the master (`vortex_core.cpp:181-202`, `:344-346`; `Clone` appends to
   `d.clone`, `vortex_vm.cpp:807-836`). Encoding details remain — finding 5.
5. **`std:meta` import/builtins (MINOR) — RESOLVED.** §6.1/§10.1 name both the import allowlist
   (`checker.cpp:2061-2067`) and the builtin member-signature table (`checker.cpp:770-779`).
6. **`validate` magic numbers (MINOR) — RESOLVED.** §6.1 binds the `OpcodeKind::Linked` ceiling cell
   and the impure-kind set through the adapter; §8.6 names the DivMod 32-bit-half mix (no `Xor`
   exists, `vortex_vm.hpp:27-93`). The new `d.validate` seen-set has an orientation bug — finding 8.
7. **M2 price overstatement (MINOR) — RESOLVED.** M2 now cites `Parser::parseExpression()`
   (`parser.cpp:271-281`) and narrows the work to the prefix case (`parser.cpp:848-855` gate at :851)
   plus `parseCodeBody`; the one-token note is added (§5.1; `lexer.cpp:379-395`,
   `token.hpp:22`).
8. **Makefile lists (MINOR) — RESOLVED.** `VAGENT_FRONTEND_SRCS` now lists `cell_path.cpp`,
   `vql/lexer.cpp`, `vql/parser.cpp`, `scanner_base.cpp` (`Makefile:688-690`); §10.1 owns the new-TU
   additions. Precision nit remains — finding 11.
9. **Reflect wrapper pricing (MINOR) — RESOLVED, but the fix creates a new hazard.** M8 memoizes
   wrapper heads on `d.code` — which puts them on the checkpoint rank; see finding 6.
10. **`d.patch` replay monotonic (MINOR) — RESOLVED.** §6.6 drops unresolvable patches after a fault
    record (compaction) and §13 prices replay.
11. **`splice`/`Splice` name collision (MINOR) — RESOLVED.** §2 distinguishes `splice` (unquote) from
    `#Splice` (opcode, `vortex_vm.hpp:53`).
12. **Expansion budgets (MINOR) — RESOLVED.** `CheckLimits::maxExpansionDepth`/`maxSplices` named as
    per-compile counters threaded through the recursive parse (§5.6, §10.1; `checker.hpp:12-27`
    currently lacks them, as §10.1 admits).
13. **`validate` cost model (MINOR) — RESOLVED.** §7.2 restates the bound honestly: per-op
    `maxCycles` plus O(n·membership); `consumeWork` is C++-side (`arena_manifold.hpp:420-426`).
14. **`closed` flag provenance (MINOR) — RESOLVED at the plumbing level.** `CompilerBinding` gains a
    state-side marker bit (§6.6, §10.1; today the struct is `{cell, type}` only,
    `compiler.hpp:15-19`). The `cap$N` mechanism then breaks the semantics the flag computes on —
    finding 3.

## New findings

### 1. BLOCKING — P16 omits the mandatory `d.mesh.relocation` marker, and the rewrite mutates the live arena with no restore rule (§10.2 P16, §4.5, §6.6)

**Problem.** Two gaps, each fatal alone:

(a) **`promote` refuses a bare OpHandle cell whose target is ephemeral.** The mint-order pass
requires `isCellRelocationHandle(from, cell)` — an `OpHandle` cell *plus* a `d.mesh.relocation`-dim
edge to a cell whose text is `"cell"` (`foreign_op_handle.hpp:24-25`,
`foreign_op_handle.cpp:109-117`; checked at `arena_manifold.cpp:1797-1813`, returns `nullopt`
otherwise). P16's row specifies only "rewrites … from `Int64` to `OpHandle`". As written, the first
checkpoint carrying a `d.code` fragment fails: `publishValueImpl` gets `nullopt` and returns
"promotion refused" (`mesh_value.cpp:168-171`). The `mesh_evaluator.cpp:720-736` precedent the row
cites *does* write the marker — `builder.link(handle, relocation, POS, marker)` — because it builds
the artifact's graph explicitly; the marker edge is the load-bearing half of the pattern, and it is
absent from P16's spec.

(b) **The rewrite hits the *live* actor arena, and nothing puts the Int64s back.** `checkpointView`
returns `const ArenaManifold &arena` (`mesh_actor.hpp:102-104`; the runtime fills it from
`actor->context->arena`, `mesh_actor.cpp:668-694`), `publishValue` only *reads* the arena
(`mesh_value.hpp:63-74`, `:156-169`), and the actor keeps running afterwards. Wing slots and control
targets are read through `asInt64`/`toInt`: `wingOf`'s operand resolution (`vortex_core.cpp:356-365`)
falls back to `masterOf(slot)` — the slot cell itself — when the slot is not `Int64`; `Jump`,
`Branch`, `Call` take `toInt(inputs[i])` (`vortex_vm.cpp:839/:848/:856`); `Choice` validates its
int64 alternative with `contains`+`getOpcodeKind` (:1150-1158). An unrehydrated OpHandle slot
therefore doesn't fail loudly — it silently resolves to the wrong operand (`masterOf(slot)`) or a
garbage target, in the *session that continues after a successful checkpoint*. The publication
precedent rewrites a doomed evaluation arena; here the arena must remain executable. No mark-wrap
restore and no Int64 write-back is specified.

**Fix.** In the P16 row and §6.6, specify: (i) each marked ref cell gets an OpHandle value *and* a
`d.mesh.relocation`-dim edge to one shared `"cell"`-texted marker cell — the marker can be one cell
per pass, priced O(1)+O(refs) links; (ii) the whole rewrite runs under a mark taken by the checkpoint
driver *before* `checkpointView`'s quiescence check matters (a mark held across `publishValue` is
legal — it only reads), then `release`d after promotion returns; alternatively write each Int64 back
from the recorded ref list. (iii) The genesis mirror: `+d.refs` walk, `setValueBits(Int64,
*handleTarget)` per cell — see finding 7 for ordering.

### 2. MAJOR — P16's home and justification are mis-scoped: the pass is adapter-implementable; "beside publishValue" is a layering inversion (§10.2 P16)

**Problem.** The row's "why it cannot live in `std:meta` or the adapter" argues only that *Vortex
code* cannot write `OpHandle` — true (`writeValue` produces Int64/Double/Bool/None only,
`vortex_core.cpp:203-227`) — but says nothing against the *adapter*. Everything the pass needs is
public arena API: `setValueBits` (the precedent's own call, `mesh_evaluator.cpp:733`), `link` for the
marker edge, `rankAfter` to walk `+d.refs`, `handleTarget`/`valueKindOf` to rehydrate. Meanwhile "a
pass beside `publishValue`" would put vagent vocabulary — `d.code`, `d.patch`, head topology,
`+d.refs` — inside the generic mesh layer, and `publishValue` takes `const ArenaManifold&`, so a
mutating pass cannot even live inside its signature. The constness points the other way: the only
mutable access is the adapter holding the actor's `WorkerContext` before `checkpointView` runs.

**Fix.** Re-own the pass: it is vagent-adapter code running on the mutable arena immediately before
`checkpointView`/`publishValue`, with genesis rehydration in the same adapter's post-resume path.
P16 then shrinks to either nothing core-side, or a narrowly stated mesh-side affordance (e.g. a
relocation hook inside `checkpointView`, or a mutable-arena borrow for the driver) — name whichever
survives in the register and why. This also fixes the fragment-discovery question the row leaves
open: the adapter owns the `d.code`/`d.patch` dims it minted, so it can enumerate heads without the
mesh layer learning the vocabulary.

### 3. MAJOR — `cap$N` via synthesized `let` copies the captured value: contradicts §5.3's aliasing and guts `closed` provenance; counter scope unstated (§5.3, M4, §6.6, §9.4)

**Problem.** `let cap$N := <name>` is a value copy, not a cell alias: `ExprKind::Let` lowers to
`emit(initializer → resolved(let-local))`, a `copy` into a fresh `localCells_` slot
(`compiler.cpp:705-707`, locals minted per-compile at :432-434/:853-855), and `binding()` consults
`scopes_` before `bindings_` (:143-155) so a synthesized `let` *shadows* the very
`CompilerBindings {cap$N → {cell,type}}` entry the doc calls "the binding-layer half". Consequences:

- §5.3's run-caveat claims "captures the cell, not a snapshot — the cell *is* the variable … a
  `run` later reads the cell's last-written value". Under `let` semantics the fragment reads the
  *copy*, frozen when the enclosing statement evaluated the let. §11's refused alternative —
  "value-snapshot captures" — is exactly what the stated mechanism builds.
- `closed` breaks either way: if the cap-local counts as fragment-internal, `closed` is vacuously
  true and `learn` accepts anything while the state cell is never dragged into the checkpoint; if it
  counts as a program-arena outside reference (it is one), `closed` is never true for a capturing
  fragment and `learn` refuses all of them (§9.4's `w`-capture included). The flag R1-14 had
  restored is computed on the wrong reference.
- Collision discipline is missing: `cap$N`'s counter scope is unstated (per literal? per
  expansion?), so a nested `code"…"` whose inner literal re-synthesizes `cap$0` can shadow an outer
  `cap$0` still live in the same block; and the "unlexable" claim covers *source* only — `$` is a
  lexical error at `lexer.cpp:333-335` and absent from the identifier charset (:14-24), but
  `snake()` accepts `$`-bearing names (`checker.cpp:30-36` rejects only uppercase/empty), so any
  programmatically built `Program` (`check` is public, `checker.hpp:29-30`) can bind `cap$0` itself.

**Fix.** Drop the `let` node; make `cap$N` a pure *alias*: the fragment's scope entry for `cap$N`
maps to the captured name's *existing* binding — same `Local` index in `scopes_`, or the same
`bindings_`/`externalReferences_` entry — and the checker's `Environment` gets the parallel
name→type entry. No new cell, no copy, `cap$0` and `w` name the identical slot. State that the
counter is monotonic per `expand` run (never per literal), and that the checker refuses `$`-bearing
names on *user-supplied* declarations/locals/bindings (one guard in `snake` or a separate internal
channel) so no AST — lexed or built — can squat on the internal namespace.

### 4. MAJOR — `expand` re-implements the checker's scoping, and its fragment-table contract is unspecified (§5.6, M4, §10.1)

**Problem.** To rename captures, `expand` must resolve each `code` literal's free names at its
definition site — "a scope-tracking walk, since the checker has not yet run". That walk reproduces
`Checker`'s scope model: seeded `self`/`parent`/`inner`/`outer` (`checker.cpp:579-582`), owner
parameters and fields (:584-591), `occurrence` in handlers, `planBindings`, `for`-loop and query
bindings (:1515, :1009), `let` block shadowing. A second implementation of the same rules is a
textbook DRY drift hazard: any rule the two disagree on binds a capture to the wrong cell
*silently*. Two contract gaps compound it: the pipeline diagram says bodies are "checked at expand"
while §5.6's text says the checker "types the merged tree once" and the checker consumes the fragment
table — but `check(const Program&, CheckLimits)` (`checker.hpp:29-30`) has no fragment-table
parameter, so the table's path from expand into the checker (and onward to `compileFragment`) is
never plumbed; and the table's ownership is unstated — body ASTs are `ExprPtr`s that must live
somewhere while `ExprKind::Code` nodes keep only `text`.

**Fix.** Say it plainly: `expand` returns `{Program, FragmentTable}`; `check` gains the table (a
signature change worth registering in §10.1) or `Program` gains a fragments field; body ASTs are
owned by the table and `Code` nodes are opaque to the checker (type = recorded `T`), while
splice-substituted clones are re-checked at the site. For scoping, prefer extracting the checker's
scope/environment builder into a shared helper `expand` reuses — one source of scope truth; if a
second walk is chosen instead, price the drift risk explicitly and name the test that pins both
implementations to the same result.

### 5. MAJOR — `+d.refs` coverage, recipe encoding and `reify`'s mark regeneration are under-specified; head overhead underpriced (§6.1, M11, §10.2, §13)

**Problem.** M11 correctly names "operand slots and `Jump`/`Call`/`Branch`/`Choice` targets" — every
`bindInput`/`bindOutput` mints an `int64` slot cell (`vortex_core.cpp:384`, `:402`), and `gate`/
`bounded`/`emitStatement` mint `reference()` cells freely (`compiler.cpp:523-527`, `:582`,
`:865`). What the enumeration misses:

- **Dim-constant slots.** `emitCells`' `dimConstant` writes `int64(findOrMintDimension(name))`
  (`compiler.cpp:769-772`) — a `Linked`/`New` op's dim operand is a cell ref too. Either those slots
  go on `+d.refs` (uniform rule: *every* `makeScalarCell(int64(ref))` the emitter mints — simplest
  and safest), or the doc must state that dimension ids are incarnation-stable because all dims are
  minted in a fixed genesis order — a load-bearing invariant that lazy `findOrMintDimension` calls
  (mid-fragment, mid-turn) would break.
- **Recipe cell addressing.** The recipe's link tuples must address cells by *census index*; if any
  record carries an arena `int64` ref, the recipe itself needs `+d.refs` marks (turtles down).
  Unstated.
- **`reify` must regenerate `+d.refs` and `+d.recipe` for the copy** — the copy's ref cells are new
  cells; without fresh marks it promotes with stale Int64s on the next checkpoint, and without a
  copied recipe it cannot be re-reified. One sentence needed.
- **Price.** "~8–14 cells per literal" (§13) counts head+descriptor only; `+d.refs` adds ~2–4
  records per op (wing slots) plus control refs, and `+d.recipe` is O(graph). Realistic overhead is
  tens of cells; the estimate should say O(fragment size), not a small constant.

**Fix.** State the uniform marking rule, the census-index encoding, the reify regeneration duty, and
re-price §13's fragment overhead as O(ops + slots + recipe), keeping the small-constant example only
for trivially small bodies.

### 6. MAJOR — `d.code` registration and memoized wrapper heads drag program-side graphs into every checkpoint (§6.1 M8, §6.7, §4.5; V12)

**Problem.** M8 memoizes `test_of`/`body_of` wrapper heads "per clause cell on `d.code`", and §6.7
registers every `code"…"`-born head "when first `run`/`graft`ed or stored". `d.code` is the
checkpoint-reachability rank — and a wrapper head's `+d.entry` link points at the *program-side*
clause graph it wraps. Result: one `test_of` call makes the clause's whole test graph reachable from
the agent cell and promotes it — the exact state→program-skeleton edge V12 exists to exclude
(`vagent-agent-language.md:244-250`), and that graph's Int64 refs carry no `+d.refs` marks, so after
`resume` it is silently aliased *and* registered. `run` on an unclosed literal fragment is a smaller
version of the same hole: registration isn't conditioned on `closed`, and once P16 marks exist the
OpHandle edges *extend* reachability (`arena_manifold.cpp:1759-1764` follows relocation handles), so
an unclosed fragment's program-side refs get dragged too.

**Fix.** Scope registration: `run`/`graft` must not register at all — registration is the
checkpoint-reachability act and belongs to `learn`/store only, conditioned on `closed` plus a
present descriptor. Wrapper heads name their graphs by ref-box `int64`, not `+d.entry` link — an
int is not a promote-followed edge, so the head promotes alone and degrades to a session handle
after `resume` (consistent with §6.7's own rule), instead of severing by eviction *and* dragging the
program in.

### 7. MINOR — genesis rehydration ordering is unstated, and the "old→new association" claim doesn't hold on the resume path (§6.6, §10.2, §4.5)

**Problem.** `resume` does not run `importValue`: it calls `receiveValue` and overlays the arena on
`checkpoint->manifold` (`mesh_actor.cpp:260-271`, `:182-289`). The promoted OpHandle cells therefore
arrive verbatim (no `local.at` remap — `importValue`'s rehydration pass, `mesh_value.cpp:494-501`,
runs only on the value-import path). P16's "rehydrates … through the same old→new association
`promote`/`importValue` already computes" is loose: on this path there is no renumbering — the
rehydration is `Int64(*handleTarget)` per `+d.refs`-marked cell, an adapter step that must be named.
Ordering matters: `d.patch` replay runs `graft`→`validate`, whose kind checks read `asInt64` —
OpHandle-kind refs read as `nullopt` and every replayed patch faults. Required order: arena
overlay → `ensureDimension` pass (all new dims) → `+d.refs` rehydration → `d.patch` replay → turns.

**Fix.** Add the ordering sentence to §6.6 and correct the mechanism sentence in the P16 row.

### 8. MINOR — `d.validate` seen-set orientation is backwards (§7.2)

**Problem.** "`marker +d.validate → visited`, membership tested as `visited -d.validate == marker`"
degenerates: `link(marker, d.validate, POS, v2)` evicts the marker's *only* `+d.validate` edge and,
reciprocally, clears `v1`'s backlink (`arena_manifold.cpp:1359-1378`) — the set holds exactly one
cell at a time. **Fix.** Flip it: `visited +d.validate → marker`, membership
`linked(visited, d.validate, POS) == marker`; the marker collects N backlinks for free, and stale
edges point at a *previous* pass's marker, which is what makes the fresh-marker-per-pass rule work.

### 9. MINOR — clone coverage: `Expr` carries five more child-holding fields; `Decl` needs cloning for M13 (§5.6, §10.1)

**Problem.** `cloneExpr` must copy `children` *and* `scope`, `recipients`, `deadline`, `guard`,
`recovery` plus the parallel `names` (`ast.hpp:100-108`) — a children-only clone silently drops
action sub-expressions. `Code`-parameter `def` expansion (M13) inlines `Decl` bodies per call site,
which carries `body`/`guard`/`initializer`/`trigger`/`contracts[].condition`/
`goalClauses[].condition` (`ast.hpp:167-174`) — a `cloneDecl`, not just `cloneExpr`; `cloneProgram`'s
actual caller is never named. **Fix.** One line in §10.1: `cloneExpr` copies every `ExprPtr` field;
M13 clones at `Decl` granularity.

### 10. MINOR — `Code` must join `builtinTypes` to refuse user-declared shadowing (§5.2, §10.1)

`validateType`'s acceptance is `scalars.contains(name) || user || arity != 0`
(`checker.cpp:553-567`): after `Code` joins `scalars`, a user `record Code`/`event Code` still
passes via `user` and both readings validate. Add `"Code"` to the `builtinTypes` guard at
`checker.cpp:2069-2071` alongside the scalars/arity-chain additions in §10.1.

### 11. MINOR — Makefile note: name which explicit list each new TU joins (§10.1)

The line "new TUs must be added" is correct but under-specified. `VAGENT_FRONTEND_SRCS`
(`Makefile:688-690`) needs `expand.cpp`, `ast_clone.cpp`, `code_path.cpp` — whichever of them
`checker.cpp`/the driver calls — since the frontend test binary links only that list; `fragment.cpp`
is kernel-side, called by `compiler.cpp`, which no frontend target links today, so the `find` glob
(`Makefile:643`) suffices; `VAGENT_SYNTAX_OBJS` (`Makefile:695`) stays unchanged *only if* the
`code`-prefix case stores raw text for `expand` rather than calling `parseCodeBody` from
`parser.cpp` — the doc's own split says the body parses in `expand`, so say that dependency
explicitly; `VAGENT_FRONTEND_FUZZ_OBJS` derives automatically.

### 12. MINOR — `run`'s `args:[Cell]` reuses `CellList` — say so; list materialization and the frame-binding convention are unpriced/unspecified (§6.2, §13)

`args: [Cell]` is the existing `Scalar::CellList` — the `+d.grab`/`+d.step` rank `materialize()`
already builds (`compiler.cpp:497`, `:733-741`; spec §5.9 `vagent-agent-language.md:667`) — no
duplicate list machinery needed, but the doc never says it reuses it. Two gaps: a `[Cell]` literal
mints a head plus O(n) ref-box cells *per `run` call* — dead cells between checkpoints, unpriced in
§13; and "spilled onto the `#Call` frame exactly as method arguments are" is ambiguous — spec §5.8
hangs args on the frame's `d.vars`/`d.values` as separate cells (`vagent-agent-language.md:645-647`),
so either each element is unwrapped to a `d.vars` slot (O(arity) writes) or the list head is bound
and the fragment reads `[Cell]` — the descriptor's arity convention must name which.

### 13. MINOR — `d.patch-head` is the right mechanism but its justification is never stated (§6.6, §2, §14)

The patch's head-name records ride a `+d.step` chain hung off `+d.patch-head` — correct and
consistent with the `Linked` box-rank convention (`vortex_vm.cpp:706-713`), because the patch cell's
`+d.patch` edge is consumed by rank membership and `+d.values`/`+d.name` carry the descriptor and
label. Say that in one clause — the R12 reconciliation lists the dim without noting *why* a
dedicated first-edge dim rather than `+d.step` from the patch, which reads as accidental.

## Summary

All fourteen R1 findings are resolved as written. v1.1's new substance introduces one blocking gap
(P16's marker and live-arena restore — the checkpoint cannot work as specified) and five major ones:
the P16 pass's home, the `cap$N` mechanism contradicting its own aliasing/`closed` story, `expand`'s
scope-model duplication and unplumbed table, `+d.refs` coverage/encoding, and `d.code` registration
reopening the V12 hole. All are fixable inside the current architecture; none requires a third core
proposal beyond the two already registered — but P16's row needs rewriting, not polishing.

VERDICT: NOT CONVERGED
