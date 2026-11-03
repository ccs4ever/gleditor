# r5-writer — convergence critique of `design/vagent-metaprogramming.md` v1.4

Scope: per-finding verdicts on r4-writer's R4-1..R4-18 against v1.4 (2,291 lines), new-defect
sweep of the ~130-line delta (v1.3 `071a04e8` → v1.4 `f7229f4f`), `mdl -i`, verified against
`vagent-agent-language.md` v3.9.

## Verdicts on R4 findings

| R4 | Verdict |
| --- | --- |
| R4-1 MAJOR §9 declarations | **Fixed.** `goal Patrol() perform` :1780, `goal Feed() perform` :1814, `goal Wait(w: Int) perform` :1847 — all match `GoalDecl ::= 'goal' Ident '(' [ParamList] ')' GoalKind` (spec :1337). Both `Forager` predicates hoisted to top level :1815-1816 (spec :1326 `Member` excludes `PredicateDecl`). `event WithinBudget { n: Int }` :1848 matches `EventDecl` (spec :1304); `emit WithinBudget{n: w}` :1858 now has its field. Changelog :2270-2272 states the fix and corrects 1.3's false claim ("`import std:meta` in every std:meta example" is the new 1.3 ending). Residual: new R5-1 — the same fix exposed a `symbol_named` owner error in §9.3. |
| R4-2 `Code` arity | **Fixed.** :177-178 "arity chain maps `Code → 0-or-1`", :654-656 same, EBNF `Type ::= … \| 'Code' [ '<' Type '>' ]` :592, :1909, changelog :2288 — consistent everywhere. |
| R4-3 uniform marking vs name-records | **Fixed.** Every marking statement now carries the `whose +d.refs record carries no +d.name` qualifier: :350-352 (M11), :458-460 (§4.1), :952-954 (head layout), :1413-1419 (P16 step 1 — which also states the discriminator governs both halves), P16 register row :1954. |
| R4-4 re-learn carve-out | **Fixed.** :1285-1294: the registered-head argument "is reused as its own registration — the second `reify` is skipped and `insertIntoRank` no-ops", and "only that arg-form dedupes" — re-learning the literal mints a second registered copy. Matches the ruling exactly. |
| R4-5 label-colon self-contradiction | **Fixed.** :1503-1508: all three label kinds are "opaque label strings, matched verbatim; the *user* `#label` alone cannot spell `:` or `"`"; first-`:` splitting scoped to qualified-name lookups (`plan_named`, `symbol_named`, patch target names) and applied only when the first segment resolves as a module; `patch:near_water` is "just a label" to `code_named`. |
| R4-6 `symbol_named` grammar + arity | **Fixed, with a new defect surfaced → R5-1.** Two-arg signature used at every site (:985, :1113, :1387, :1832); genesis applies the §6.7 split (`module_named` on the qualifier, `symbol_named(owner, name)` on the rest, :1386-1391); the `[<module> ':'] [<Agent> '.'] <member>` grammar is stated once at :1517-1520. |
| R4-7 stale `(§14)` | **Fixed** — :1099 now reads "(U-M10, §15)"; U-M10 exists at :2173-2177. |
| R4-8 mdl blank line | **Fixed** — blank line before `### 6.7` (:1488). `mdl -i` on the file: clean, exit 0, no violations (run with and without `-c .mdlrc`). |
| R4-9 duplicated paragraph | **Fixed** — the marker/`+d.ref` restatement is gone; one paragraph :957-964 carries the per-ref-cell marker argument with the `isCellRelocationHandle` cite folded in. |
| R4-10 broken sentence | **Fixed** — :792-795 reads "…so names are part of the match (§7.2) — and `run` can find the cells it spills." |
| R4-11 row order | **Fixed** — `value_of` (pure) at :994 sits above `run` (opaque-impure) at :995. **Residual nit:** `next_rand` (pure, :1007) still sits below the impure block — same class, pre-existing from v1.3 (was :979 there). |
| R4-12 §8.2 stale | **Fixed** — :1661-1662 "a weight record naming the clause is its strength"; §8.1 now derives per-junction scalars from `link`'s one `-d.weight` slot (:1641-1644). |
| R4-13 impure-kind set | **Fixed** — §6.1 binding list :925 adds "the impure-kind set §7.2 requires" beside the ceiling cell and arity table. |
| R4-14 §7.3 resolved items | **Fixed** — :1607-1610 lists only genuine open items (stale foreign handles, memoization freshness, U-M7 `reify`, P17 resync); the old `opcodeMap_`/reflect-name/P16-shape items are gone. |
| R4-15 `opcodeMap_` genesis step | **Fixed** — genesis ordering gains step (c) "re-`import` the prelude and re-point `opcodeMap_`" :1459-1461, letters re-sequenced (a)–(g). |
| R4-16 `navigate_to` misdescription | **Fixed** — :1399-1403: the name-recorded callee is `demo:Forager.navigate_to`, `"water"` is the argument. |
| R4-17 "Int64-stale" wording | **Fixed** — :1466-1470: `importValue` converts the OpHandle-kind cell (`CellImport{OpHandle}` → promoted target, `arena_manifold.cpp:1934-1954` verified) while `resume` receives OpHandle-kind cells verbatim carrying pre-promotion targets; rehydration `writeValue(slot, Int64)`s each marked cell. No contradiction. |
| R4-18 nit bundle | **Fixed (all items).** `Code` for the run target (:1364 "opaque session `Code` handle"); "re-kinded" everywhere (:947, :1381, :1443); `(R2-B)` cite → `(§6.3)` :1331; `code_named` walks `d.code` only, twin unreachable by construction :1514-1516; `checkpointView` cite `mesh_actor.cpp:668-694` + `mesh_actor.hpp:102-105` :1406 (verified: fn :668-694, `const ArenaManifold &arena` member at hpp :102-105); enqueue cite `arena_manifold.cpp:1759-1764` :1383 (verified: `isCellRelocationHandle` → `handleTarget` → `seen`/`frontier` at :1759-1763, gate at :1797-1809); "three-part `module:Agent.plan`" :989, `<module>:<Agent>.<member>` :1332-1333; duplicate-label refusal stated at §6.7 :1499-1501; `graft_plan(goal, name, test, body)` qualified in §8.5 pseudocode :1713 with the "no bare `graft`" comment; `lexer.cpp:14-24` :706 (verified: `identifierStart` :14-18 + `identifierPart` :20-24); appendix order restored (1.2 sits between 1.1 and 1.3); M11 defers to §6.6's protocol :355-356; P16 "Where" splits `vagent/fragment.{hpp,cpp}` vs "the adapter's own executor TU" :1954; `static(param)` errors at emission :844-845; `def run`/`def value_of` refusal under `import std:meta` :611-613; identifier-legality tightened :1504-1505 (loose anchor → nit, below). |

**All 18 r4 findings are resolved.** Mechanical checks: 22 fence markers = 11 tagged pairs;
all TOC links resolve to the 51 `##`/`###` anchors (no heading renumbering); all tables have
consistent column counts; `mdl -i` clean.

## New findings

**R5-1 MAJOR — `symbol_named("Forager", "Feed")` cannot resolve `Feed`; `symbol_named` has no
spelling for module-level symbols.** §9.3 declares `goal Feed() perform` at top level (:1814) —
goals are module-level symbols on the module's `d.vars` (spec §5.7 :627-629: "one name cell per
goal, event type, question, and per method, answer and `def` (as `skill.method`, `Agent.def`)").
The new §6.7 grammar (:1517-1520) makes `symbol_named`'s owner "the owning `Agent`'s `d.vars`
cell", so `symbol_named("Forager", "Feed")` (:1832) resolves `Forager.Feed` — a member that does
not exist; `Opt` failure fails the `for` statement and the `on Meal` handler silently no-ops.
Two compounding defects in the delta: (a) the grammar gives *no* owner spelling for module-level
symbols — `symbol_named("demo", "Feed")` is not a form the stated grammar covers — yet §6.3
(:1111) makes `symbol_named` the lookup path for goals/event types, which are module-level per
the spec; (b) §6.3's own convention contradicts the new bullet: ":1113 resolves bare
goal/event names "against the agent's own module's `d.vars` (`symbol_named(owner, name)` taking
the agent as owner)" — owner-as-agent resolving *module* `d.vars`, vs :1519's owner-as-Agent's
`d.vars` cell. Three statements, three owner semantics. Fix: state that owner accepts a module
for module-level symbols (`demo:Feed` → `symbol_named("demo", "Feed")`) or add the
bare-name/module-implicit form, and correct §9.3 to match — `learn_plan("Feed", …)` already uses
the correct module-implicit form in the same handler.

**R5-2 MINOR — `opcode_named` is a dangling name (:1460).** New genesis step (c): "the
program-image artifact's own op map is what `code` re-emission and `opcode_named` consult" —
`opcode_named` appears nowhere else: not in the `std:meta` table (:980-1007), not in the spec,
not in the codebase. Rename to the intended member or drop it.

**R5-3 MINOR — `reify` label format inconsistent.** §6.7 :1503 "`reify` labels `reify:<serial>`"
vs §6.1 :1035 "`reify:<src>:<serial>`" (and changelog :2229 `reify:<src>:<serial>`). The new
§6.7 sentence drops the `<src>` component.

**R5-4 MINOR — sideness "full structural dim vocabulary" omits `d.extends`.** :1269-1273 claims
"every dim program structure can hang on is in the walk" — but `d.extends` is spec-attested
program-side (:481 "program side", :634 "Supertypes are reached along `d.extends`"). A fragment
edge to a supertype name cell evades the walk and misclassifies. Add `d.extends` or soften
"full … every".

**R5-5 nit bundle:** (a) "(identifier-legality rule, §6.6's source field)" :1505 — the legality
rule is §5.1's `'#' Ident` grammar; §6.6's source field (:1485) only *uses* the label — imprecise
anchor; (b) `learn_*` table row :1003 still says flat "reify twice" without acknowledging the
registered-head-arg carve-out of :1288-1294; (c) checker-additions comment lists
"run(c, [args]) special form" (:1911) *and* "run special form" (:1915) — duplicated entry,
pre-existing from v1.3; (d) `next_rand` (pure) below the impure block — residual of R4-11's
class, pre-existing; (e) `symbolNamed` :1224 names nothing in the tree (the codebase has
`exportSymbol`, used correctly at :1389) — pre-existing, now inconsistent beside the correct
name; (f) closed's name-record description :1344-1345 says `+d.name` is "the callee's qualified
name" while run-target records hold the target head's *label* (:1392-1394) — the carve-out is
stated but the (b) arm's phrasing could name it.

## Citation spot-checks on the delta (all verified)

`mesh_actor.cpp:668-694` checkpointView + `mesh_actor.hpp:102-105` `const ArenaManifold &`;
`mesh_actor.cpp:659-662` `pending` → `mailbox.size()`; `:462` empty-mailbox skip; `:672-675`
quiescence incl. `outstandingMarks() != 0`; `arena_manifold.cpp:2123` `outstandingMarks_ = 1`
hydratePrefix floor; `or_parallel.cpp:349` `hydratePrefix` call; `arena_manifold.cpp:1183-1186`
`birthOp = refOf(dense)`; `mesh_value.cpp:399` `birthOp != *target` relocation gate;
`arena_manifold.cpp:1759-1764` handle enqueue; `insertIntoRank` same-dim `:1423-1439` +
`rankTail` at `vortex_stdlib.cpp:4238-4241` (`addClause`); `exportSymbol` `sym +d.values →
entryOp` `vortex_core.cpp:119-127`; `arena_manifold.hpp:624-628` idempotent-insert comment;
`worker_context.hpp:63` `ArenaManifold arena`; `compiler.cpp:769-772` `dimConstant`;
`lexer.cpp:14-24` identifier charset; P4 `wake` = spec :81's approved local wake.

Checked clean: changelog 1.4's every claim maps to a real delta (verified item-by-item against
`git diff 071a04e8 f7229f4f`); no TOC/heading renumbering; table and fence integrity hold;
`import std:meta` present in every std:meta-using example (§8.3, §9.2, §9.3); §9.1/§9.4 correctly
need none.

VERDICT: NOT CONVERGED — all r4 findings resolved; 1 major + 4 minors + nit bundle new in v1.4.
