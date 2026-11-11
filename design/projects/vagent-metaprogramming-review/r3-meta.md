# Round 3 critique — `design/vagent-metaprogramming.md` v1.2 — Metaprogramming Expert lane

Scope: staging discipline — `cap$N` binding-table aliasing, `cloneExpr`/`expand` pipeline ordering, template expansion, hygiene/capture, `run`/`static` semantics, `reify`/`learn`/`closed` durability of code, P15/P16 staging-level correctness. Every load-bearing citation below was re-verified against the tree this round.

## 1. Verdicts on my r2 findings

**meta-1 (`cap$N` carrier → binding-table alias) — RESOLVED, with two residual MINORs (findings 4–5).**
v1.2 adopts the fix wholesale: §5.3 item 1 makes `cap$N` "a pure binding-table alias, not a synthesized `let`" — `ExprKind::Let`→value-copy (`compiler.cpp:705-707`) and `validName`'s `$`-refusal (`compiler.cpp:407-417`) are both cited and routed around; the checker records `{cap$N : type}` in its env *at the literal's position*; both emissions resolve through `CompilerBindings` to "the captured binding's own cell"; §11 records the refused synthesized-`let` alternative. The three r2 failure modes (unlexable name at preflight, snapshot-vs-cell divergence, head-of-block ordering) are each closed. Residual precision gaps — the env record's scope extent, and the channel for let-locals/`cells` anchors — are new minor findings 4–6.

**meta-2 (P16 marker edge + live-arena restore) — RESOLVED.**
§6.6 now specifies all three gaps: emission carries the inert `d.mesh.relocation→"cell"` marker edge *plus* a `+d.ref` record on `+d.refs`; the pre-checkpoint pass performs the `setValueBits(OpHandle, target)` re-kind (the `mesh_evaluator.cpp:720-736` precedent's both halves — verified `:733-735`); the post-publish pass writes `Int64(*handleTarget)` back on the live arena ("mechanical and idempotent"); genesis rehydrates `Int64(*handleTarget)` before `d.patch` replay since `resume` doesn't remap. Verified against `isCellRelocationHandle` (`foreign_op_handle.cpp:98-117` — kind AND marker), the promote handle-BFS (`arena_manifold.cpp:1759-1764`), the unmarked-`OpHandle` refusal (`:1797-1813`), Int64-verbatim vs OpHandle-relocated promotion (`:1934-1954`), and `checkpointView` handing the live arena by const ref (`mesh_actor.hpp:102-106`, `mesh_actor.cpp:668-694`). One residual spec ambiguity — which cells the pre-pass may skip — is finding 7.

**meta-3 (`cap$N` counter scope) — RESOLVED.**
§5.3 item 2: "One counter per `expand` run … shared across all literals and all substituted bodies … never per literal," with `cloneExpr` preserving names verbatim. Exactly the requested sentence.

**meta-4 (scope walk → resolve-only pass) — RESOLVED.**
§5.6 inserts a resolve-only pass: "stamps each `Identifier` with its binding's scope id — the checker's env construction extracted into a shared helper it reuses, so `expand` never re-implements the scope rules (`for`-body `copyEnvironment`, `as`-bindings, the implicit names, plan/handler parameters — `checker.cpp:579-582`, `:1505-1524`)". Verified: implicit names at `checker.cpp:579-582`; `copyEnvironment` at `:570-574`; for-bodies `:1505`, for-believes `:1524`, findall `:1543`, while `:1496`. Better than the requested fix — one scope truth rather than a parity claim.

**meta-5 (`cells"$anchor` rewriting) — RESOLVED, residual on let-local anchors (finding 6).**
§5.3 item 3: `expand` rewrites `CellsPath` anchor variable names through the rename map at body re-parse; §11 records the refusal alternative. `CellsPath::Anchor::Variable` is string data as charged (`cell_path.hpp:23`); the kernel resolves it via `bindings_.find` (`compiler.cpp:476`). Residual: `bindings_.find` consults `CompilerBindings` only — a `$cap$N` anchored on a let-local capture can't resolve there (the cell doesn't exist at bindings-build) — the local channel needs one clause or a stated refusal.

**meta-6 (fragment table keying) — RESOLVED.**
§5.6: "keyed by `SourceLocation`/ordinal stamped on the `Code` node so `cloneExpr`'d literal copies share the original's record; the record's body AST is itself re-cloned per emission because kernel tables key on `const Expr *` (`compiler.cpp:884-891`)". Both asks adopted verbatim; the double-registration hazard is named.

**meta-7 (template trigger/recursion/diagnostics) — RESOLVED.**
M13 rewritten: trigger is *use* ("a `def` whose body splices a `Code` parameter"), `def apply(c: Code<Int>, x: Int)` explicitly stays an ordinary runtime def; recursion is the named `recursive-template` check error; `return`/`yield`/`consume`/`terminate` refused in template bodies; diagnostic ownership split (call-site loc for context checks, def-site for body errors); per-site checking stated; trigger scope = direct `Code<…>` params (`Opt`/`[Code]`/records opaque); member coverage (`def`s/`method`s in agents, behaviors, skills, modules); `effect`/`native`/`capacity`/`graft_method` barred; the refactor consequence is written down. Residual: what counts as "the body" for the trigger (contracts? nested literals?) — finding 12.

**meta-8 (CBV + transitive static knowledge) — RESOLVED.**
§5.2: statically-known is "a judgment `expand` makes *transitively and post-substitution*" — literal, let-bound-to-statically-known-initializer, substituted actual, or `Code` param during expansion; `var`-bound gets an explicit refusal; `splice c(splice d)` gets the one clarifying sentence (inner splice substitutes `d`'s body — arg is `T`, not `Code<T>`). §5.4 CBV arg-lets (`let p0 := count`, §9.1) compose with the transitive rule.

**meta-9 (recipe link-encoding/external captures/closed-iff) — RESOLVED.**
§6.1: endpoints "encoded as *links* (each record's `+d.values` points at the actual endpoint cells, never a census integer)"; "External captures are preserved verbatim"; "closed iff the source was"; `+d.refs`/`+d.recipe` regenerated for the copy; `reify:<src>:<serial>` labels with duplicate refusal; v1 scope = recorded emission template, current-graph copy = U-M7. All four asks adopted.

**meta-10 (`run` signature → special form) — RESOLVED.**
M6/§6.2: checker special form, `args` a list literal typed per-element `any` (heterogeneous legal here only), elements evaluated in order and bound on the `#Call` frame *by parameter name, as values* — "there is no `Cell`-boxing surface in the language and none is added"; compile-time check when statically known; `run` on `Code<()>` yields `()`; descriptor links `result`/`produced` cells; `value_of` named for `Code<Cell>` results; `Halt` tails not `run`-nable (`vortex_vm.cpp:1220-1225` verified — `Halt` returns without popping the frame).

**meta-11 (wrapper descriptors) — RESOLVED in direction, but the chosen naming creates a new MAJOR (finding 2).**
§6.6: unknown-flagged descriptors (`pure`/`closed` unset); §6.3: pure-requiring grafts refuse them; the graft-for-session promise narrows to body/symbol grafts; `Halt`-tailed graphs not `run`-nable. But the mechanism chosen to keep wrappers off `d.code` — "names its graph through a ref-box Int naming the attach op, *not* a `+d.entry` link" — contradicts the `+d.entry` convention everything else uses. See finding 2.

**meta-12 (occupancy any-of) — RESOLVED.** Glossary + M14: "any install-dim backlink (`-d.grab`, `-d.spin` or `-d.values`)" plus the attach-verbs reservation.

**meta-13 (`static` housekeeping) — RESOLVED.** §5.5: non-zero `choiceDepth()`/`outstandingMarks()` at run end is a compile error (`compiler.cpp:108-110` invariant preserved); residue stays in the discarded compile arena, never sealed.

**meta-14 (post-substitution static knowledge) — RESOLVED.** Folded into §5.2's "transitively and post-substitution".

**meta-15 (labels) — RESOLVED.** `reify:<src>:<serial>`; "registration refuses duplicate labels" — collisions become loud refusals at `d.code` registration rather than silent `code_named` first-match. (Residual serial-scope question — finding 14.)

**meta-16 (P15 `masterOf`) — RESOLVED.** M10 + P15 row: "`opcodeMap_.contains(masterOf(cell))`" — `writeValue`'s `masterOf` resolution verified at `vortex_core.cpp:202`.

**meta-17 (`run` in allow-list) — RESOLVED.** M6 names `run` explicitly; §6.2: "`run` inside `code"…"` is legal — named in M6's allow-list".

**Score: 17/17 resolved.** The R2-D/R2-E/R2-F rewrite is faithful and substantially better-specified than v1.1. All remaining issues are new.

## 2. Findings (new in v1.2)

### 1. MAJOR — §5.3 / M13 / §5.6: template-`def` bodies keep un-renamed free names — the `cap$N` capture class in the second expansion vehicle

`cap$N` renaming is specified for `code"…"` bodies only ("`expand` walks each `code"…"` body … every free name that resolves to an enclosing binding is renamed"). A template `def`'s body is inlined per call site with "arguments substituted" (M13) and "checked per call site — the checker sees each expansion in its own context" — i.e., its free names re-resolve in the *caller's* env. Nothing renames or pre-binds them. Example:

```vagent
module util {
  var scale: Int := 2
  def wrap(c: Code<Int>): Int { scale * splice c }   // template: splices c
}
agent A {
  var scale: Int := 100
  on Tick { let out := util.wrap(code"n * 2") }
}
```

Expanded: `{ let p0 := …; scale * (n*2) }` inside `A.Tick` — `scale` resolves to `A`'s field (owner-env fields precede globals, `checker.cpp:588-592`), silently reading `100` where the `def` meant `util.scale = 2`. `self`/other fields/module symbols are the same class — the exact silent capture §5.3 exists to prevent. The resolve-pass scope-id stamps *could* fix this (the `def` body's identifiers are stamped at resolve time) — but the doc never says the checker honors stamps at check time; its stated `cap$N` channel (env records + `CompilerBindings`) is what makes the hole visible.

**Fix:** one rule, either formulation: (a) apply `cap$N` renaming to the free non-parameter names of template bodies at expansion (their bindings are known from the same stamps), or — cleaner — (b) state that the checker resolves stamp-carrying `Identifier`s to their resolve-pass bindings rather than re-walking env (one resolution truth; `cap$N` env records then remain only for invented names). Also say whether `cloneDecl` preserves the stamps.

### 2. MAJOR — §6.6 vs §6.1/§6.2: reflect-wrapper heads name their graphs by ref-box Int, not `+d.entry` — the uniform attach convention cannot reach them

§6.6: "A wrapper names its graph through a ref-box Int naming the attach op, **not a `+d.entry` link**, so the wrapper head itself drags no program structure." But: (i) §6.1's attach convention — "follows `+d.entry` once when it is a head … uniform for grafted and program-emitted heads alike" — has no third branch for ref-box heads; a grafted wrapper (body/symbol grafts "may install one under the narrowed rules", §6.3) is unreachable by execution; (ii) §6.2's `run` mechanism reads "the head's `+d.entry` for the entry op" — no entry means `run` on a `test_of`/`body_of` result — the *documented* use (`Code<Cell>` "a `run` on it yields a `Cell` handle", §6.1/M8) — cannot find its graph; (iii) §7.2's `validate` walks the fragment subgraph — from the entry — and would vacuously pass. The stated motivation is also wrong-headed: wrappers are never `d.code`-registered, so an `+d.entry`→attach-op edge is session-side and can drag nothing into a checkpoint.

**Fix:** give wrapper heads `+d.entry` → the wrapped graph's root op (safe: session-side, unpromoted by construction); keep the ref-box/attach-cell as the memo key only. If a link is truly unwanted, specify the alternate entry resolution for `run`/attach/`validate` everywhere — the doc currently states none.

### 3. MAJOR — §6.6 `closed` re-verification misses handle-valued captures — a learned fragment can carry a stale `Code`/`Cell` through the capture channel

`closed` (b): every outside reference is "(a) a link to a state-side cell — a capture or `cells"…"` path — or (b) a name-bound program symbol". A `var stash: Code` (or `let d := code"…"`) captured into `code"{ run(d,[]) }"` resolves to a *value cell* — an arena cell whose `int64` content is a head ref-box. The `learn` re-verification "walks the fragment's link frontier + `+d.refs` targets checking each endpoint is state-side or a name-record" — the endpoint is a non-program cell, so the fragment is accepted. At checkpoint the capture link promotes `d`'s cell with **Int64 content verbatim** (`arena_manifold.cpp:1934-1936`); post-resume the cell holds a stale dense id, `run(d,[])` resolves it through the dense-range check (`vortex_vm.cpp:50-67`) into whatever live cell landed there, and executes arbitrary graph — §4.5's documented aliasing hazard, silent, *inside* the mechanism §6.6 claims is durable. A captured `Cell`/`Code` binding is precisely "an int-encoded program reference" that `closed` claims to exclude — just smuggled inside a state cell's content.

**Fix:** extend the `closed` re-verification: captured bindings whose declared type is `Cell`/`Code` (or whose cell's content is a ref-box int naming a program-side cell) are non-closed — refuse, or require the fragment to resolve its code targets by `d.name`/`code_named` (the durable channel) — and add the same sentence to §6.7's session-handle rule ("captured handle values inherit the stored-field rule").

### 4. MINOR — §5.3: the `cap$N` env record's scope extent is unstated

"the checker records `{cap$N : type-of-name}` in its env *at the literal's position*" — for the record to cover `cap$N` occurrences in *substituted* bodies at splice sites, it must live in the env enclosing the literal and persist to scope tail (the checker's `infer` mutates env in place, `checker.cpp:1396`, so the natural reading works) — but an implementer could scope the records to the fragment-body check and every spliced capture then fails as unbound. For inline operands (`splice code"…"`) the record must also precede checking the substituted body at the same position. One sentence fixes it — or say the checker resolves `cap$N` through the rename map directly, skipping env.

### 5. MINOR — §5.3: "resolve `cap$N` through `CompilerBindings`" glosses the channel split — let-locals and `$cap$N` anchors need the local channel

`CompilerBindings` (the `bindings_` map, `compiler.hpp:19`) is a `const` input built before emission — a let-local's cell doesn't exist until `localCells_` mints at `emitStatement` (`compiler.cpp:853-855`), so `{cap$N → let-local cell}` cannot be a `CompilerBindings` entry at table-build. The workable impl is alias-at-resolution: the `cap$N` identifier resolves through the *captured binding's own channel* — `localReferences_` index for locals, `bindings_`/`externalReferences_` for externals (`compiler.cpp:571-576`, `:143-183`). Same hole one level down: `cells"$cap$0"` resolves its anchor via `bindings_.find` *only* (`compiler.cpp:476`, not `scopes_`), so a `$cap$N` anchored on a let-local capture can't resolve — needs the local channel or a stated refusal. **Fix:** name the alias mechanism once: "`cap$N` resolves exactly as the captured identifier resolved — local index for `let` locals, bindings cell for externals; anchors follow the same channel."

### 6. MINOR — §5.3: "the emission's wing and record edges to the binding cell" is imprecise on both channels — and impossible for frame params

Wing slots are `int64` content (`bindInput`/`bindOutput` mint `makeScalarCell(int64(operand))`, `vortex_core.cpp:384,402` — the doc's own M11 says so), not links. The reachability story is: record/link edges (capture records) make the binding cell checkpoint-reachable; the `int64` wing refs (P16-marked) supply the reads. And for `def`/plan parameters there is *no* emission-time cell to point an edge at — §5.8 frame args are `makeCell`-fresh per `#Call` (`vortex_vm.cpp:860`; spec §5.8 "the callee's arguments and locals hang on that frame cell's `d.vars`/`d.values`") — so the "last-written value" rule implies the capture record's edge is written at *evaluation/activation*, not emission. The doc declares it ABI-debt, but the emission-edges sentence contradicts it. **Fix:** one clause — "capture reachability is by record/link edges; reads go through `int64` wing refs marked for P16; frame-parameter captures re-point the capture edge per activation (the mechanism behind the last-written rule)".

### 7. MINOR — §6.6 P16: name-bound target slots vs the uniform marking rule — state which records the pre-pass skips

P16 item 1 marks "*every* emitter-minted int64 ref cell"; `closed` (b) says "P16 does **not** OpHandle-rewrite name-records". A `Call`-to-program-`def` target slot *is* an emitter-minted int64 ref cell — under the literal uniform rule it is marked, and the pre-pass "calls `setValueBits(OpHandle, target)` on every marked ref cell" → `promote` follows the handle (`arena_manifold.cpp:1759-1764`) → **drags a program op into the checkpoint — the V12/`d.code`-invariant violation P16 exists to prevent.** The doc's pieces (name-records carry `+d.name`; genesis resolves them by `symbol_named`) strongly imply the pass skips `+d.name` records, but the pass spec doesn't say it. **Fix:** one clause — "name-bound target slots carry the name-record (never the `d.mesh.relocation` marker); the pre-pass re-kinds only records lacking `+d.name`"; and state that name-records carry `+d.ref`→the target slot like other records so genesis knows which cell to rewrite.

### 8. MINOR — §6.6: "state-side" is undefined for let-local/param captures; the marker bit's test is unstated

`closed` (a) requires capture endpoints to be "state-side". A `let`-local's cell is a compiler temporary — not pre-checkpointed — yet §5.3 treats let-local captures as first-class stable captures and the capture link itself makes the cell promoted/state-reachable. The `CompilerBinding` state-side marker bit ("which captured bindings name checkpointed cells", §6.6/§10.1) reads as if let-locals fail — under that reading no `let`-capturing fragment is ever `learn`able, contradicting §5.3. **Fix:** define "state-side" as "endpoint reachable through links and not program skeleton" (i.e., the bit discriminates program-symbol bindings — name-record/refuse — from arena-value bindings — link-capture/accept), not "already on a state rank".

### 9. MINOR — §6.2/M6: "when `c`'s type carries it statically" — `Code<T>` never carries arity

M3 types fragments by result type only: `code"w:Int|w+1"` and `code"x+1"` are both `Code<Int>` — arity lives in the body's `CodeParams`/descriptor, not the type. §6.2's "a compile-time arity check … when `c`'s type carries it statically" should read "when `c` is statically known (a literal or let-bound literal whose `CodeParams` are visible)". M6's phrasing is already correct; §6.2's is the loose one.

### 10. MINOR — §5.4: "splice-only" fragments should be stated as emitting no head/descriptor

"`code"f: Code<Int> | splice f + 1"` is legal only where `f` is being instantiated … such a fragment is splice-only, refused by `run`/`graft`" — but its body contains a residual `splice f`, which "reaching lowering is a compile error" (§5.2). So the literal can never emit a runnable graph — it is expansion material, not a `Code` value at all. Say so ("emits no head/descriptor; exists only as a splice source") rather than implying a runnable object with two refusals.

### 11. MINOR — §5.5: does `static` inside a parameterized fragment see the literal's `CodeParams`?

"`code` parameters are forbidden" is ambiguous between `Code<…>`-typed def params and the literal's own `n: Int`. Under post-substitution closedness, `code"n:Int|static(n*2)"` spliced as `splice c(3)` is closed (p0 := literal) and foldable — but at the literal's own emission `n` is unvalued → error. Pick the rule and say it (recommended: judged at each context — error at emission, legal at sites whose actuals close it; or always refused).

### 12. MINOR — M13 trigger scope: contracts and nested literals

Does `splice c` in a `requires`/`ensures` clause count as template-triggering "use"? M13 says "whose body splices". Also a `splice c` inside a `code"…"` nested in the `def` body (`code"splice c"`) — transitively triggers. One clause: "the trigger is any `splice` of the parameter anywhere in the def's expansion — contracts and nested `code"…"` bodies included".

### 13. MINOR — §6.1 surface-table gaps

`value_of(c: Cell): any` is named in §6.2 but absent from the member table — an implementer building `std:meta` from the table misses it. `unlearn(clause: Cell)` should accept clause-or-symbol per R2-I ("matching = resolved target name") — `graft_method` patches otherwise have no unlearn path. Whether `import std:meta` reserves bare `run` against a user `def run` is unstated.

### 14. MINOR — §6.1/§6.7: `reify:<src>:<serial>` serial scope

A session counter collides with checkpointed same-serial labels after `resume` → registration refused spuriously. Draw the serial from a durable per-agent counter (or the patch ordinal).

### 15. MINOR — §6.2: `run` inside a `learn`able fragment needs a name-resolution clause

Independent of finding 3's mechanism: even for `closed`-clean cases, `run`'s target should be stated name-resolved (`code_named`) for durability — a captured `Code` value is an int-encoded program reference under §6.6's own vocabulary.

### 16. MINOR — citation wobbles (non-load-bearing)

`mesh_actor.cpp:473-479` produces `ActorError::ExecutionFailed`; the `ActorDown` *message-type* mapping is at `:486` (just outside the cite). The resume parenthetical "does not run `importValue`'s remapping" is imprecise — the continuation path builds the arena via `receiveValue` (`mesh_actor.cpp:260-271`), which *is* the import path; `mesh_value.cpp:494-501` rehydrates marked handles to `OpHandle → local-id`. Genesis's `Int64(*handleTarget)` is needed because import leaves them `OpHandle`-kind with already-remapped targets, not because handles are unmapped — the conclusion is right, the mechanism sentence slightly off. `lexer.cpp:14-23` should be `:14-24` (identifierStart+identifierPart).

## 3. Task-posed questions, answered

- **Two-channel binding coherence under resolve-only ordering:** coherent. Parse→resolve(stamps)→expand→check works because `cap$N` names are invented post-resolve and checked via env records at the literal's position (records must persist to scope tail — finding 4). The emission channel needs the alias-channel clause (finding 5).
- **Name-bound-Call capture vs splice substitution:** `code"…"` bodies are safe — `f` is `cap$N`-bound to its definition-site binding, routed to a name-record at emission (program-symbol binding via the marker bit). **Template-`def` bodies are not** — finding 1 is exactly this question answered negatively.
- **`run`-inside-`code` staging:** now consistent — allow-listed, opaque-impure, nested frames per level; residual is the `closed`/handle interaction (finding 3) not the staging model.

## 4. Verified-claims appendix (spot-checked this round)

- Frame/ABI: `Call` target = `toInt(inputs[0])`, frame = `makeCell` holding return-op int (`vortex_vm.cpp:854-877`); `Return` pops via `+d.stack` (`:878-893`); `Halt` parks without frame pop (`:1220-1225`); spec §5.8 args-on-frame convention (vagent-agent-language.md:643-655).
- P16 machinery: precedent `mesh_evaluator.cpp:733-735` (both halves); `isCellRelocationHandle` `foreign_op_handle.cpp:98-117`; promote both-directions BFS + handle discovery `arena_manifold.cpp:1759-1764`, `:1781-1783`; unmarked→ephemeral fails `:1797-1805`; Int64-verbatim/OpHandle-relocated `:1934-1951`; import rehydrate `mesh_value.cpp:494-501`; `checkpointView` live-arena const-ref `mesh_actor.hpp:102-106`.
- Kernel: `localCells_` per-emission `compiler.cpp:853-855`; `resolved` local-vs-external `:571-576`; `bindings_.find` anchor `:476`; `externalReferences_`/`localReferences_` `const Expr *` tables `:885-891`; `dimConstant` `:769-772`; `New` bound output `:793-799` + VM refusal `:736-744`; `validName` `:407-417`; `Let` emit `:705-707`; `Identifier` copy `:605-606`; mark/choice invariant `:108-110`.
- Checker: `snake` accepts `$` `:30-36`; `any` `:27`; scalars `:554-556`; builtin member table `:765-779`; `pureBody` `:827` (`cells` mutating → impure `:861-865`); allowlist `:2061-2067`; `builtinTypes` `:2069-2071`; DuplicateName depth-test `:1392-1395`; env mutation `:1396`.
- Front end: `$` lexical error `lexer.cpp:333-335`; prefix merge `:379-398`; `!= "cells"` gate `parser.cpp:851`; `postfix` Call/Index/Field `:740-767`; `Unary` prefix `:730-735`; `parseExpression` trailing refusal `:271-281`; `ExprKind` has no `Code`/`Splice`/`Static` `ast.hpp:37-88`; `Expr` fields incl. scope/recipients/deadline/guard/recovery/names `ast.hpp:96-109`.
- Misc: `Linked` rebuilds ref-box rank per exec `vortex_vm.cpp:680-713`; `Choice` alt-op validity `:1150-1158`; `Mul` overflow `:457-463`; `writeValue` `masterOf` `vortex_core.cpp:202`, kinds `:203-227`, `splice` funnel `:251-254`; `Mark` has no dim field `arena_manifold.hpp:170-186`; `consumeWork` throws `:420-426`; "well nested" `:773`; drain idiom `vortex_stdlib.cpp:3800-3802`,`:3841-3843`; Makefile `VAGENT_FRONTEND_SRCS` `:688-690`, `VAGENT_SYNTAX_OBJS` `:695`, kernel glob `:643`.

VERDICT: NOT CONVERGED
