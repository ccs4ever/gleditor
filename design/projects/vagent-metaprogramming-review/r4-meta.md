# Round 4 critique — `design/vagent-metaprogramming.md` v1.3 — Metaprogramming Expert lane

Scope: staging discipline — `cap$N` binding-table aliasing, resolve-stamp honoring, `expand`/`cloneExpr`/`cloneDecl` pipeline ordering, template expansion hygiene, capture/wing representation, `run`/`static` staging, `reify`/`learn`/`closed` durability, P15/P16 staging-level correctness. All load-bearing citations below were re-verified against the tree this round.

## 1. Verdicts on my r3 findings

**meta-1 (template-`def` free names → stamps honored?) — RESOLVED, but the resolution creates a new MAJOR ambiguity (new finding 1).** v1.3 adopts ruling R3-I wholesale: §5.6 (`:863-865`) — "The **checker** then types the merged tree once — honoring the resolve pass's scope-id stamps on `Identifier`s (one resolution truth; it does not re-walk env for stamped names) and reserving env construction for the `cap$N` records `expand` invents"; M13 (`:379`) — "`cloneDecl` copies every field, preserving the resolve pass's scope stamps"; §10.1 — "stamp-honoring (resolve owns name truth)". This is exactly fix (b) I recommended: a template body's `scale`-style free names keep their def-site stamps and can no longer capture caller bindings. The residual — what stamp a *renamed* `cap$N` node carries — is new finding 1.

**meta-2 (wrapper `+d.entry` restored) — RESOLVED.** §6.6 (`:1245-1253`) and M8 (`:279-287`): wrapper heads "carry `+d.entry → the wrapped graph's root op` like every head … session-side and unpromotable by construction", memoized on `clause +d.code → +d.step` chains keyed by the current attach cell, unknown-flagged descriptors refused by pure-requiring grafts. `run`'s entry lookup, the uniform attach convention, and `validate` all work again.

**meta-3 (`Cell`/`Code` captures non-closed) — RESOLVED, one residual hole (new finding 3).** §6.6 (`:1291-1293`): "`Cell`/`Code`-typed captures are likewise non-`closed` … refused unless it is name-resolved (`d.name`/`code_named`) at emission." But v1.3 keeps only the *declared-type* arm of the R3-F ruling and drops "or whose cell's content is a ref-box int naming a program-side cell" — see finding 3 for the `any`/`Opt<Code>`/`[Code]` gap.

**meta-4 (env record extent) — SUPERSEDED by finding 1.** Still no "persists to scope tail" clause, and the question is now folded into the larger stamp-vs-env-record ambiguity.

**meta-5 (let-local/anchor channel) — PARTIALLY RESOLVED.** The identifier channel is now specified: "`cap$N` aliases bypass `binding()`'s render check (`compiler.cpp:160-181` — capture cells are compile-time-unvalued; the checker already proved the type) and resolve through a `cap$N`-keyed entry in `externalReferences_`, materialized while the enclosing `emit` still holds the minted cells" (`:713-716`). Verified `binding()`'s render check at `compiler.cpp:160-180` and the `Expr*`-keyed `externalReferences_` at `:181`, `:490`, `:575`, `:757`, `:885-891`. The `cells"$cap$0"` anchor hole remains — folded into finding 2.

**meta-6 (capture-slot wing form) — RESOLVED in substance, residual on frame-bound captures (finding 2).** The mechanism is now a real link, not a wing int: "the emission wraps the binding cell in a capture-slot cell on the operand's wing, decoded by `wingOf`'s operand lambda (`asInt64` fails → `masterOf(slot)`, `vortex_core.cpp:356-365`)" — verified: the lambda at `vortex_core.cpp:356-365` falls back to `masterOf` on non-Int64 slots, so a `d.clone`-linked slot resolves the operand to the binding cell itself. This is better than what I asked for: the capture is a link (promote translates it, the `closed` walk can enumerate it) and needs no P16 record. The stated rationale ("the link is the *checkable* form") is the right one. Residual: for frame-bound bindings there is no emission-time cell for the clone-link to name — finding 2.

**meta-7 (name-record marking skip + `+d.ref`) — RESOLVED.** §6.6 (`:1309-1326`) + the §6.1 diagram (`:911-915`): unified `+d.refs` schema, `+d.ref → slot` canonical on every record, `+d.name` present ⇒ name-record (no marker, never re-kinded — with the correct reason: `promote` enqueues `handleTarget` into `seen`, `arena_manifold.cpp:1797-1809`), absent ⇒ full P16 ceremony. The genesis rewrite is spelled fully: `symbol_named(qname) → +d.values → head → +d.entry → writeValue(slot, int64(entryOp))`, unresolvable → fault/drop, and the divergence clause (session binding vs post-resume rebind) is stated once.

**meta-8 (state-side definition) — RESOLVED, one definitional nit (finding 10).** §6.6 (`:1231-1234`): program-side = reachable from `program()` over `d.stdlib`/`d.vars`/`d.clause`/`+d.grab`/`+d.spin`/`+d.step`; state-side = link-reachable and not program-side. Let-local/parameter captures are declared "neither state-side nor name-recordable" → un-learnable (`:1289-1291`), matching the R3-F ruling.

**meta-9 ("when `c`'s type carries it statically") — NOT FIXED.** §6.2 (`:1031`) still reads "a compile-time arity check … when `c`'s type carries it statically". `Code<T>` carries `T`; the fragment's parameter arity lives in the descriptor/`CodeParams`, not the type. Should still read "when `c` is statically known" (M6's phrasing is already correct). Remains MINOR — finding 10.

**meta-10 (splice-only no-head) — RESOLVED, tiny residual (finding 10).** §5.4 (`:777-779`): "a splice-only literal emits **no head and no descriptor** — `expand` consumed it into sites before fragment emission ran, so nothing reaches `fragment.cpp`". Exactly as requested.

**meta-11 (`static` per-context) — RESOLVED.** §5.5 (`:813-815`): judged "in the context the parameter lands in — legal where the substituted site is `static`-legal (the fragment's own `code` parameters are still forbidden operands, being unvalued at evaluation)". The per-context rule I recommended.

**meta-12 (M13 trigger breadth) — RESOLVED.** M13 (`:376-378`): "a `def` whose expansion *anywhere* splices a `Code` parameter — body, guard, contract condition, goal clause, or a nested `code"…"` body inside it". Contracts and nested literals are in.

**meta-13 (`value_of`/`unlearn`/`run` namespace) — RESOLVED.** `value_of(c: Cell): any` is in the table (`:967`); `unlearn(clause_or_symbol)` with name matching (`:976`, `:1278-1280`); the `splice`/`static`/`run`/`value_of` reservation extends to `let`/`var`/parameter bindings when `std:meta` is imported (`:600-602`).

**meta-14 (durable serial) — RESOLVED.** §6.7 (`:1408-1410`): the `reify` serial "comes from a *durable per-agent counter* on `d.vars`, not a per-incarnation one, so reified labels never collide with a promoted registration's after `resume`".

**meta-15 (`run` name-resolution) — RESOLVED, residual on the genesis arm (finding 6).** §6.6 (`:1300-1301`): "`run` targets inside a learnable fragment must be *name-resolved* (a `run` of an opaque session `Cell` handle is not durable either)". Residual: the name-record genesis chain is spelled only for `symbol_named`; a `run`-target head resolves via `code_named` — and whether the emitter auto-records statically-known `run` targets or the programmer must spell `code_named(...)` is unstated.

**meta-16 (citations) — RESOLVED, one new slip (finding 5).** `mesh_actor.cpp:473-486` now covers the `ActorDown` mapping (`:485-486` verified); the resume-vs-import mechanism is corrected to "differ in kind" (`:1372-1375` — verified against `mesh_value.cpp:481-501`); `lexer.cpp:14-23` still cited where `identifierPart` closes at `:24` — trivial.

**Score: 14/16 resolved, meta-5/meta-6 partially (frame-bound channel residuals consolidated as finding 2), meta-9 unfixed (minor).** The R3-A..R3-I rulings are implemented faithfully; verified `insertIntoRank` idempotence (`arena_manifold.hpp:624-628`), the Executor seam (`mesh_actor.cpp:473-476` mutable `WorkerContext&`; `mesh_actor.hpp:104` `const ArenaManifold&`; `mesh_value.hpp:69-74`), `exportSymbol`'s `+d.values → entryOp` (`vortex_core.cpp:119-127`), `receiveValue`→`rebuildManifold` overlay (`mesh_value.cpp:345-359`) vs `importValue`'s OpHandle rehydration (`mesh_value.cpp:494-501`), `dimConstant` ref cells (`compiler.cpp:769-772`), and `unknown`/`snake` (`checker.cpp:26`, `:30-36`).

## 2. Findings (new in v1.3)

### 1. MAJOR — §5.3 item 1 / §5.6: the `cap$N` rename's stamp fate is load-bearing and unspecified — under the keep-the-stamp reading every capture fails as unbound

v1.3 made the checker resolve stamp-carrying `Identifier`s "to their resolve-pass bindings rather than re-walking env", while `cap$N` names are "records `expand` invents" resolved through env records at the literal's position. The doc never says what stamp (if any) a renamed `cap$N` node carries, and the four readings diverge:

- **(a) Keep the captured binding's scope-id stamp** — the natural implementation, since renaming mutates the name field of a stamped node and `cloneExpr`/`cloneDecl` "preserve" stamps: the checker resolves `cap$0` in the *captured binding's* scope, where no `cap$0` is declared → **every captured name fails as unbound** — systematic breakage of the flagship mechanism.
- **(b) Stamp carries binding identity** — `cap$0` resolves straight to the captured binding; the env records become dead machinery the doc still specifies.
- **(c) Drop the stamp** — the env-record channel works, but needs meta-4's still-unstated clause: records must persist to the scope tail so `cap$N` occurrences inside `cloneExpr`'d substituted bodies at splice sites resolve (capture-liveness keeps them in scope, but only if the records live that long).
- **(d) Stamp to the literal's own scope** — the env record at the literal's position is found by stamp, which also makes meta-4 moot.

Evidence: §5.6's two clauses (`:850-853` "a field `cloneExpr`/`cloneDecl` preserve"; `:863-865` honoring stamps / "reserving env construction for the `cap$N` records `expand` invents") are jointly underdetermined — the doc specifies both channels and never says which one `cap$N` takes.

**Fix:** one sentence at §5.3 item 1, e.g. option (c/d): "the rename clears the resolve stamp — `cap$N` identifiers are unstamped and resolve through the env records `expand` inserts at the literal's position, which persist to the enclosing scope's tail" (then meta-4 is also fixed), or option (b): "the renamed `cap$N` keeps the resolve-pass stamp naming the captured binding itself, so the checker resolves it to that binding; the env records are a readability aid for diagnostics only." Either is coherent; the doc must pick.

### 2. MINOR — §5.3 items 1&3 / §6.6: frame-bound captures still have no concrete channel — anchors, `CompilerBindings`, and the capture-slot all silently require an emission-time cell that does not exist

Three statements each assume a binding cell exists at compile time:

- `cells"$cap$0"` anchors: "the `$cap$0` anchor then resolves through the same `CompilerBindings` entry" (`:725`) — but `bindings_.find` runs at *preflight* (`compiler.cpp:476`, recording `externalReferences_` at `:490`), `bindings_` is a `const` input built before emission (`compiler.cpp:100`, `compiler.hpp:19`), and `localCells_` mints during `emitStatement` (`:853-855`). A `cap$N` anchored on a `let`-local capture cannot resolve. Under the §5.8 ABI it is worse: a frame-bound binding has no compile-time cell *at all*.
- "aliases the captured binding's own cell in `CompilerBindings`" (`:191-192`): `CompilerBinding` is `{cell, type}` — a parameter capture cannot be entered (no cell until activation).
- "the emission wraps the binding cell in a capture-slot cell" (`:708`): a `def`/plan parameter's cell is `makeCell`-fresh per `#Call` (`vortex_vm.cpp:860`); the clone-link has no target at emission. The "last-written value" observable rule requires the frame setup to mint or re-point the capture-slot's `d.clone` edge per activation — the mechanism behind the rule, never stated.

The doc's "ABI-debt class" umbrella (`:702-707`) acknowledges the debt at the class level, but the three concrete channel sentences contradict it silently. **Fix:** one clause — "frame-bound captures resolve through the per-activation frame slot: the `cap$N` alias and capture-slot edge are established by the frame-binding mechanism each activation (the ABI debt); `cells"$cap$N"` anchors are supported for stable-class captures (fields/module vars) only, and are refused on frame-bound captures because anchor resolution is compile-time."

### 3. MINOR — §6.6 `closed`: the `Cell`/`Code` capture test drops R3-F's content arm — `any`/`Opt<Code>`/`[Code]`/record-typed captures smuggle session handles

v1.3 keeps only "whose declared type is `Cell`/`Code`". A `var stash: any`, `var memo: Opt<Code>`, `var pool: [Code]`, or a record with a `Code` field is *not* `Cell`/`Code`-typed yet its cell's content transitively contains ref-box ints naming program-side cells — the exact stale-handle smuggle the ruling was written to exclude (`run` on the stashed value post-resume resolves the stale int through the dense-range check into a live unrelated cell, `vortex_vm.cpp:50-67`). **Fix:** extend the sentence — "a capture whose declared type is or *contains* `Cell`/`Code` (`any`, `Opt`, lists, records) — or whose cell's content is a ref-box int naming a program-side cell, checked on the captured cell at re-verification, not only the declared type — is non-`closed`."

### 4. MINOR — §6.7: the first-`:` label split breaks `code_named` on `reify:`/`src:`/`patch:` labels

"a `:`-labelled `code_named`/`plan_named` string splits at the *first* `:`" (`:1405-1408`). Under that rule `code_named("reify:src:demo:12:4:7")` parses as module `reify`, rest `src:demo:12:4:7` — but `d.name` labels are opaque strings on the `d.code` rank, not `module:name` pairs, so every system label (`reify:…`, `src:…`, `patch:…`) fails lookup through the very channel the doc calls "the durable re-resolution path". **Fix:** scope the split to qualified-name resolution — "`code_named` matches `+d.name` labels verbatim (labels are opaque); the first-`:` split applies only to the `module:Owner.name` form `plan_named`/`symbol_named` and patch target names take."

### 5. MINOR — §6.6 genesis: "receives Int64-stale cells" contradicts the `OpHandle`→`Int64` conversion it describes

`:1373-1375` — "`resume`/`genesis` receives Int64-stale cells and the rehydration *converts* each marked cell `OpHandle`→`Int64` with the promoted target." Mechanically: promote serializes marked cells as `OpHandle` store records with translated targets (`arena_manifold.cpp:1947-1951` — verified: `store.makeOpHandle(out.version, isEphemeral(target) ? real.at(target) : target, …)`), so the resumed base manifold presents **OpHandle-kind cells with already-promoted targets** — not "Int64-stale" cells. The operation is right; the received-kind claim is wrong. **Fix:** "receives OpHandle-kind cells whose targets are the promoted store-space ids; genesis writes `Int64(*handleTarget)` to restore the executable form."

### 6. MINOR — §6.6: `run`-target name-records resolve via `code_named`, not the spelled `symbol_named` chain; auto-record vs programmer-spelled unstated

The genesis rewrite is spelled only for program symbols (`symbol_named → +d.values → head → +d.entry`). A `run` target inside a learnable fragment is a fragment head — its name-record's `+d.name` is the head's *label*, resolved by `code_named`, and `code_named → head → +d.entry → writeValue(slot)` is a different first step. The doc also never says whether the emitter writes the name-record automatically for a statically-known `run` operand or whether the programmer must spell `run(code_named("helper"), …)` inside learnable fragments. **Fix:** "a name-record whose `+d.name` is a `d.code` label rebinds through `code_named` at genesis; the emitter name-records every statically-known `run` operand — an opaque (`reflect`/`var`/parameter) `run` target in a learnable fragment is a closedness refusal at emission."

### 7. MINOR — §5.3 item 1 / §6.6: does `cap$N` rename program-member names? The `+d.name` provenance for name-records hangs on the answer

"every free name that resolves to an enclosing binding is renamed" (`:690-691`). If `def`/method/plan names count, a `cap$M()` call loses its qualified name — `CompilerBinding` is `{cell, type}` (`compiler.hpp:15-18`, no name field) — and the `+d.name → qualified name` arm of the `+d.refs` schema has no source. In the existing kernel, member resolution never touches `binding()`/`bindings_` (locals→`localReferences_`, externals→`bindings_.find`, `compiler.cpp:143-183`), so the coherent reading — members are *not* renamed, stay name-resolved, and get name-records — is available but unstated. **Fix:** "`cap$N` renames *value bindings* only; program-member references (`def`/method/plan/goal names) are resolved by name at emission and carry `+d.refs` name-records — they are never capture-renamed."

### 8. MINOR — M13/§5.4: template-`def` value-argument binding semantics unstated (textual substitution vs CBV)

M13 says the body is inlined "with the arguments substituted" (`:378`); §5.4 specifies call-by-value `let`-binding only for `splice`. For a template `def wrap(c: Code<Int>, x: Int)` whose body uses `x` twice, textual substitution evaluates an effectful actual twice — divergent from `splice`'s stated CBV. **Fix:** "non-`Code` arguments of a template `def` bind call-by-value like `splice` (a fresh `let` local per argument at the inlined body's head); `Code` arguments are statically-known bindings, never re-evaluated."

### 9. MINOR — §6.1: duplicated paragraph (`:923-930` vs `:932-936`)

"Each marked ref cell's *marker*…" appears twice with divergent wording — the second instance drops the per-ref-cell minting rule and the `+d.name` discriminator sentence. Merge artifact; delete the second block (`:932-936`).

### 10. MINOR — residual micro-items

- §6.2 (`:1031`) still says "when `c`'s type carries it statically" → "when `c` is statically known" (r3 meta-9, unfixed).
- §6.6 sideness test: "state-side is link-reachable" names no root — a frame cell on `+d.stack` is link-reachable from the cursor; say "reachable from the agent cell" to make the frame-bound exclusion definitional rather than fiat. Relatedly, "a `code"…"` head … hangs off the program's own op graph" (`:1227-1228`) never names the edge — the `program()`-walk dims (`d.stdlib`/`d.vars`/`d.clause`/`+d.grab`/`+d.spin`/`+d.step`) reach the head only if the literal's emission links it into the graph on one of those dims; say which (e.g., the literal's emitted slot `+d.values → head`, or a per-module fragment rank).
- §5.4 splice-only literal: `let f := <splice-only>` still *evaluates* the literal — its value-semantics unstated (recommend: "a splice-only literal is not a value; it may appear only in `let`-bindings whose name is spliced").
- §5.5: a fragment containing `static(param)` that is *emitted* (for `run`/`graft`) rather than spliced errors at emission when the operand is unvalued — implied, unstated.
- §5.1: whether a user `def run`/`def value_of` under `import std:meta` is rejected — the binding-name reservation is stated, the `def` case only implied.
- `lexer.cpp:14-23` → `:14-24` (`identifierPart` closes at `:24` — verified); appendix lists 1.3 before 1.2 (`:2101` vs `:2134`).

## 3. Answers to the task-posed staging questions

- **Stamp-honoring × `cap$N` env records:** underdetermined — the doc specifies both channels and never says which resolves a `cap$N`; one reading (scope-id stamp kept on the renamed node) breaks every capture. Finding 1.
- **Two-`reify` `learn` flow and capture edges:** consistent. `reify` preserves external captures verbatim per copy (`:1003-1004`), regenerates `+d.refs`/`+d.recipe` with endpoints remapped (`:1005-1007`), and the patch names the *registered* head's label so `unlearn`/`code_named`/`plan_named` resolution works (`:1266-1274`). Recipe-minted marker cells remap through the old→new map, so each copy's ref cells get their *own* fresh markers — the shared-marker eviction trap does not reappear.
- **Model-(b) name-binding × splice substitution:** consistent. Spliced bodies keep *definition-site* stamps (hygiene), so the same fragment spliced at sites with different `g`s still calls its own `demo:g` — correct; the durable `+d.name` record rebinds by qualified name at genesis, which is the stated divergence feature. The one unclosed edge is `+d.name` provenance when the call comes through an alias — finding 7.

VERDICT: NOT CONVERGED
