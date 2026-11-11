# Round 4 critique — Programming Language Design & Ergonomics (v1.3)

Scope: vagent surface syntax, typing rules, scoping/hygiene, staging discipline, error paths, learnability, spec coherence, consistency with `design/vagent-agent-language.md` v3.9 and `apps/common/xanadu/vagent/`.

## R3 verdicts

- **R3-F1 (`let` locals → ABI-debt class)** — **RESOLVED.** §5.3 item 1 (:703-707) now reads "Frame-bound bindings — `def`/plan/handler parameters *and* `let` locals — are the ABI-debt class… a captured local is orphaned at `Return` and reads back the last-written value — the same observable rule parameters get." The observable rule paragraph (:738-744) covers "a `def`/plan parameter's or a `let` local's binding cell" explicitly; §6.2 (:1053-1055) repeats it; §6.6 (:1289-1291) makes captured locals/params un-`learn`able. The scalar-kernel `localCells_` shape is correctly demoted to "the interim no-call shape". Exactly the fix proposed.
- **R3-F2 (name-bound executable form)** — **RESOLVED.** Model (b) adopted with all three demanded carve-outs: §6.6 (:1309-1329) — int64 `Call` operand at emission (`vortex_vm.cpp:854-857` semantics preserved in-session); `+d.name`-present ⇒ name-record ⇒ **no** `d.mesh.relocation` marker and never `OpHandle`-re-kinded (:1314-1319); `+d.ref → that very slot cell` (:1314); genesis chain spelled fully (`symbol_named(qname) → +d.values → head → +d.entry → writeValue(slot, int64(entryOp))`, unresolvable ⇒ fault/drop, :1320-1323); the session-vs-resume rebind divergence is stated once (:1323-1326); §9.3 declared to work by design (:1327-1329). `closed` reworded to "no program-side reference lacks a `+d.refs` name-record" (glossary :137, §6.6 :1282-1286, M11 :335). *Residual nit (not a finding):* P16 step 1 (:1338-1344) still says "every emitter-minted int64 ref cell … gets its own marker edge", which literally includes name-recorded Call-operand slots; :1314-1316 exempts them at the record level. The end-of-turn pass walks records, so the mechanism is safe either way — one clause in step 1 ("ref cells whose `+d.refs` record carries no `+d.name`") would align the two texts.
- **R3-F3 (signature check + param names)** — **RESOLVED.** §6.3 (:1104-1107): "an *ordered `(name, type)`-pair match plus arity, result type and purity* (the names matter: binding is name-keyed)". §7.2 (:1451-1457) adds the motivating example — "(x: Int) fragment onto `def f(a: Int)` passes arity+type but reads an unbound frame slot". §5.4 (:765-767) states names are part of the match.
- **R3-F4 (patch records registered heads)** — **RESOLVED.** :1266-1269 — "each `+d.values`-pointing at a fresh cell holding one **registered** head's `+d.name` label — the pristine `d.code`-resident original the patch names, never the installed copy". Re-`learn` idempotence stated via `insertIntoRank` (`arena_manifold.hpp:624-628`, :1238-1241).
- **R3-F5 (closed user contract)** — **RESOLVED.** §6.6 :1286-1293 — the self-containedness list (literals, instantiated `code` params, `believes`/`findall`, `cells`/`$` paths onto state-side cells, field/module-var captures, name-bound calls), the locals/params exclusion named bold, `Cell`/`Code`-typed captures non-`closed` unless name-resolved, `run` targets in learnable fragments must be name-resolved (:1300-1301), enumerable-frontier bound (:1294-1297), rebind rule = the divergence clause.
- **R3-F6 (descriptor result/name-record dims)** — **RESOLVED.** §6.1 layout (:903-905): `["result"] ─(+d.values)─► the fragment's result CELL` with `(+d.name→"Int" on the same record = result type name)` — both dims named. Name-record schema (:911-915) distinguishes `+d.ref` (the slot genesis rewrites) from `+d.name` (the qualified name genesis looks up).
- **R3-F7 (`:`-label parse + goal qualification + durable serial)** — **RESOLVED IN SUBSTANCE**, with residual wording defects bundled as R4-F3 below. First-`:` split rule stated with the `patch:near_water` example (:1404-1410); bare `goal`/`event` self-implicit convention stated (:1082-1084); `reify` serial is a durable per-agent `d.vars` counter (:1408-1410).
- **R3-F8 (`value_of`/`unlearn`/`run(Code)`/Code arity)** — **RESOLVED except two residuals (R4-F6).** `value_of(c: Cell): any` row added, marked pure (:967); `unlearn(clause_or_symbol: Cell)` with name-match rule (:976); bare-`Code` actual yields `unknown` (:966 — `unknown` is the checker's internal type, `checker.cpp:26`, fine as doc notation); "0-or-1 arity chain (arity 0 = wildcard)" stated at :1802-1803. Residuals: §6.2 :1031 still says "when `c`'s type carries it statically" (the type never carries arity — the M6 :231 "statically known" phrasing wasn't propagated), and §5.1's EBNF can't produce bare `Code`.
- **R3-F9 (examples)** — **RESOLVED except `WithinBudget`'s field (R4-F2).** `import std:meta` present in §8.3 (:1573), §9.2 (:1674), §9.3 (:1707); §9.1 notes none is needed (:1657). Declarations added: §9.2 `event Attack`/`goal Patrol`/`predicate threat` (:1676-1678); §9.3 `event Sighting`/`event Meal`/`goal Feed` + predicates (:1709-1715); §9.4 `goal Wait(w)`/`event WithinBudget` (:1745-1746). §5.5 arg fixed to `static (2 * 3000)` → `Int` (:812). §8.3 retitled "on the proposed surface" and §9.4 gained the executor-lowering honesty note (:1765-1767).
- **R3-F10 (splice peeling + dead-name bindings)** — **RESOLVED.** §5.1 :602-607 — the peeling rule stated ("when the operand's outermost postfix ends in `( args )` the arglist belongs to the `splice`; any other call-shaped operand is a check error; `run(f(x))` is the runtime path"), plus the `static`-operand parenthesization note. Declaration refusal extended to `let`/`var`/param bindings named `splice`/`static` (+`run`/`value_of` on `import std:meta`, :600-602).
- **R3-F11 (`?`-export + occurrence formals)** — **PARTIALLY RESOLVED.** The `?`-export rephrase landed verbatim (:1699-1702 — "unparseable rather than refused"). The `occurrence` formals were stated (:1088-1089) but the stated formals are **wrong** — see R4-F1.
- **R3-F12 (run/splice spelling row)** — **RESOLVED.** §11 :1885 — "`run(c, a, b)` variadic spelling | vagent has no variadic call surface (`parser.cpp:316-339`); the list literal is the v1 spelling — one list packs the frame, and `splice c(a,b)` already owns paren-args".

## Findings

### R4-F1. MAJOR — §6.3 (:1088-1089): `graft_handler` formals "occurrence plus the event's declared fields — bound by name" contradicts the spec's handler-binding convention and produces the R3-F3 unbound-read pathology

**Problem.** "`graft_handler`'s formals are `occurrence: <EventType>` plus the event's declared fields — bound by name, the same rule `run` uses (§6.2)." The spec is explicit that event fields are **not** bound names: "A handler binds the event as `occurrence`, or as the name after `as`, and reads fields by name" (`vagent-agent-language.md:1205-1206`), and "The parameter name `amount` is a field of the event, not an automatically imported local" (`:2153-2154`; member read `occurrence.distance`, `:1195`, `:1519`). Under the doc's own name-keyed signature check (the R3-F3 fix), the stated formals are defective on two axes:

1. **All-fields requirement.** A spec-idiomatic guard `code"occurrence: Add | occurrence.amount > 5"` is refused for not declaring `amount`; a fragment that does declare every field passes the check — but under standard dispatch (spec §5.8 binds `occurrence`/as-name, fields hang on the event cell's `d.vars`/`d.values`, spec :571) those field slots are never written → the exact unbound-read bug R3-F3 fixed for `graft_method`. The only escape is a new binding convention (grafted clauses bind descriptor-declared formals) that is unstated and diverges from declared handlers.
2. **`as`-name ignored.** `graft_body`/`graft_where` onto a declared `on Add as a` clause must satisfy *that clause's* bound names — `a`, not `occurrence`. The bullet supplies only `occurrence`; correct fragments are refused (fail-closed, but wrong).

**Fix.** "The formals are the handler's bound names: the event cell under `occurrence`, or under the `as`-name when the target clause declares one; event fields are read through the bound event exactly as declared handlers read `occurrence.f`, never bound as formals." If fields-as-formals is a deliberate minted-clause convention, it must be flagged as *new* (dispatch binds descriptor-declared formals for grafted clauses) and priced.

### R4-F2. MINOR — §9.4 (:1746 vs :1756): `emit WithinBudget{n: w}` uses an undeclared field

**Problem.** `event WithinBudget` is declared bare, then emitted as `WithinBudget{n: w}` — `n` is not a declared field, so the example fails the checker as written ("unknown or implicit record field", `checker.cpp:1465`). This is the residual of the r3 "declare all entities" fix — the declaration was added but not the field the example uses.

**Fix.** `event WithinBudget { n: Int }`.

### R4-F3. MINOR — §6.7 (:1404-1416): the label-parse rule set is self-contradictory and breaks `code_named` on generated labels

**Problem.** Three coupled defects in the new paragraph. (a) "labels may not contain `:`/`"`" is stated in the same bullet that defines the automatic label `src:<module>:<line>:<col>` — which contains `:` — as do `reify:<src>:<serial>` (:1408) and the learned-clause name `patch:near_water` (:1407). The constraint can only govern *user* `#label`s; as written it outlaws its own examples. (b) "a `:`-labelled `code_named`/`plan_named` string splits at the *first* `:`" makes generated labels unresolvable: `code_named("src:demo:12:4")` parses to (`src`, `demo:12:4`), which never matches the stored label `src:demo:12:4` — yet `code_named` on a generated label is "the recommended durable reference" (:1410-1411). The split belongs to `plan_named`'s qualified form only; `code_named` must match the label verbatim. (c) "both reachable by label lookup order, which `code_named` resolves deterministically by rank-first match" (:1413-1416) — the program-image twin is not on the `d.code` rank, so `code_named`'s own walk never sees it; the claimed disambiguation is vacuous (harmless, but confusingly phrased).

**Fix.** "User `#label`s may not contain `:`/`"`; generated `src:`/`reify:` labels and clause names may. `code_named` matches the stored label verbatim; `plan_named` parses qualified form by first-`:` module split then agent.member split." Recast or drop the twin sentence.

### R4-F4. MINOR — §6.1 (:923-936): duplicated marker/record paragraphs — stale v1.2 wording survives next to the v1.3 version

**Problem.** Two adjacent paragraphs describe the same mechanism: :923-930 (v1.3, per-ref-cell marker + `+d.ref` record + name-record skip) is immediately followed by :932-936 (v1.2 residue: "Each marked ref cell's *marker* is the inert `d.mesh.relocation→"cell"` edge … Both exist per marked cell"), which half-repeats and half-contradicts the first (it omits the per-ref-cell and name-record rules).

**Fix.** Delete the second paragraph; fold its `isCellRelocationHandle` citation into the first.

### R4-F5. MINOR — §5.4 (:767-768): stranded sentence fragment "and `run` can find the cells it spills."

**Problem.** The `(name, type)`-pair clause was inserted mid-sentence; the original continuation "and `run` can find the cells it spills." now dangles as its own line after "(§7.2)." — an editing artifact that breaks the sentence's grammar.

**Fix.** Merge: "…links to the `result`/`produced` cells themselves, so `validate` can check wing arity, `graft_*` can compare signatures as ordered `(name, type)` pairs …, and `run` can find the cells it spills."

### R4-F6. MINOR — §6.2 (:1031), §5.1 EBNF (:584): `Code<T>` "carries" no arity (wording not unified) and the bare-`Code` wildcard is unparseable per the stated grammar

**Problem.** "a compile-time arity check against the fragment's signature when `c`'s type carries it statically" — `Code<T>` carries only `T`; arity lives in the descriptor, and the check requires a *statically known fragment* (M6 :231-232's correct phrasing — the r3-F8 unification wasn't applied). Separately, the §5.1 EBNF gives only `Type ::= 'Code' '<' Type '>'`; the bare-`Code` wildcard every graft signature uses (:969-973, :981-984) needs `'Code' ['<' Type '>']` to parse as `Type{"Code",{}}`.

**Fix.** "when the fragment is statically known" in §6.2; optional-bracket the EBNF production.

### R4-F7. MINOR — §13 (:1979-1980): `learn_*`'s mandatory double `reify` is unpriced

**Problem.** `learn_*` mints two reified copies per fragment argument (:975, :1229-1231, :1257-1258) — `learn_plan` performs four recipe replays (test + body, O(fragment cells + links) each) and leaves three live copies of each graph (untouched original + installed + registered). §13 prices "`a d.patch` cell + head-chain records + checkpoint ops" but not the copy work or the extra dead cells — the most surprising cost in the whole surface.

**Fix.** One §13 clause: "`learn_*` mints two fresh copies per fragment argument (registered + installed) — O(fragment) recipe replay per copy — and leaves the original dead."

### R4-F8. MINOR — §5.4 (:777-779): splice-only literals "emit no head" but their position constraint is unstated — a fully-substituted `let`-bound one still evaluates

**Problem.** "a splice-only literal emits **no head and no descriptor** — `expand` consumed it into sites". A `let f := code"g: Code<Int> | splice g + 1"` whose uses are all consumed as splice/template actuals still *evaluates the literal* at run time in initializer position — and nothing was emitted to evaluate. The literal is only "refused by `run`/`graft`" (:776-777); `let`/`var`/non-template-call positions aren't covered, and dead-`let` removal isn't claimed either.

**Fix.** One clause: "a splice-only literal is refused in evaluated positions — it may appear only as a `splice`/template actual (the residual-splice check already covers unconsumed bodies); expand erases `let` bindings left dead by full substitution, or the checker refuses them."

### R4-F9. MINOR — §6.6 (:1320-1321): genesis calls `symbol_named(qname)` with one string against the two-argument signature

**Problem.** The genesis rewrite resolves `+d.name` "through `symbol_named(qname)`", but `symbol_named(owner: Str, name: Str)` (:957) is binary and the name-record stores one qualified string (`demo:Forager.navigate_to`). The qname → (owner, name) split (first-`:` for module, then agent.member) is unstated — the same parse R4-F3 fixes for `plan_named` is silently assumed here and in `unlearn`'s name matching.

**Fix.** State the split in §6.7's parse rule (it already defines the first-`:` convention; extend it to owner/name) or give the genesis chain a `symbol_named(qualified)` overload.

### R4-F10. MINOR — §6.6 (:1338-1344) vs (:1314-1319): the P16 marking rule literally marks name-recorded slots

**Problem.** Step 1: "every emitter-minted int64 ref cell … gets **its own** inert `d.mesh.relocation→"cell"` marker edge" — the Call-operand slot of a name-bound call is such a cell; :1314-1316 exempts it ("gets **no** marker") at the record level. The mechanism is safe either way (the re-kind pass walks records and skips `+d.name`; a stray marker on an Int64 cell is inert to `promote`), but the two normative texts disagree on whether the edge exists.

**Fix.** Step 1: "every emitter-minted int64 ref cell *whose `+d.refs` record carries no `+d.name`* gets a marker edge".

### R4-F11. MINOR — small wording residuals

**Problem.** (a) :1017 "`learn`ed ones default to `patch:<n>`" — but `learn_plan`'s `name: Str` is a required parameter (:969); "default" has no surface form unless the parameter is optional. (b) :1275-1276 "read off the clause's symbol cell" — clauses are not symbols; the resolution path is `plan_named`/`d.name`. (c) `graft_method(owner: Str, …)` — `owner`'s bare-vs-qualified convention isn't stated (the goal/event self-implicit rule at :1082-1084 covers only those). (d) §5.1 :592-595 "they stay legal as identifiers elsewhere" now sits next to the declaration-name refusals — tighten to "`code` stays legal as an identifier; `splice`/`static`/`run`/`value_of` stay legal only as member names after `.`".

**Fix.** One clarifying clause each.

## Summary

v1.3 discharges all twelve r3 findings in substance: `let` locals moved into the frame-bound ABI-debt class (F1); name-bound calls fully specified under model (b) with the marking exemption, `+d.ref` slot pointer, genesis rewrite chain and rebind divergence (F2); `(name, type)`-pair signatures (F3); registered-head patch naming + idempotent re-learn (F4); the user-level `closed` contract (F5); named dims on `result`/name-records (F6); first-`:` parse + self-implicit goal/event + durable serial (F7); `value_of`/`unlearn`/`unknown`/0-or-1 arity (F8); imports, declarations, `Int` arg, retitle (F9); peeling + dead-name refusals (F10); `?`-export rephrase (F11); the variadic `run` refusal row (F12). The new defects are concentrated where the fixes' wording went one clause too far or not far enough: the graft_handler formals contradict the spec's explicit "fields are not bound locals" rule and ignore `as`-naming (R4-F1, the one MAJOR); then ten minors — an undeclared event field, the label-parse/`code_named` contradiction, a duplicated paragraph, a stranded sentence, the "type carries arity"/EBNF residuals, the unpriced double reify, the splice-only position hole, the unary `symbol_named(qname)` call, the marker-rule exemption wording, and small nits.

VERDICT: NOT CONVERGED
