# r4-writer — critique of `design/vagent-metaprogramming.md` v1.3

## Verdicts on R3 findings

| R3 | Verdict |
| --- | --- |
| R3-1 name-bound executable form | **Fixed** — model (b) fully specified at :1309-1329 (emitter-minted int64 `Call` operand + `+d.name` name-record; no marker/re-kind; genesis `symbol_named → +d.values → +d.entry → writeValue(slot, int64(entryOp))`; session/resume divergence at :1323-1326). Residual: uniform-marking overclaim → R4-3. |
| R3-2 undeclared entities / `import std:meta` | **Partially fixed → R4-1 (MAJOR).** Imports present (:1573, :1674, :1707; §9.1's "no import needed" note :1656-1657). But the added declarations are still malformed — see below. |
| R3-3 `+d.refs` contradiction | **Fixed** — unified schema :911-915 (`+d.ref` canonical; `+d.name` discriminates). |
| R3-4 result-cell dimension | **Fixed** — :903-904 adds `+d.values` link to the result cell plus `+d.name→"Int"` type row. |
| R3-5 `unlearn` re-point | **Fixed** — :976, :1273-1274 (reify of the named prior head). |
| R3-6 `run_checked` dead-end | **Partially fixed → R4-7.** U-M10 exists (:2065-2069) but :1069 still says "(§14)". |
| R3-7 `value_of` row | **Fixed** — :967, :1048-1050. Residual: row order → R4-11. |
| R3-8 `unlearn` matching + empty-grab | **Fixed** — :976, :1278-1280, :1119-1121. |
| R3-9 citations | **Fixed** — re-verified: `vortex_vm.hpp:27-93` enum, `vortex_vm.cpp:1156-1158` Choice, `compiler.cpp:484-486`, `mesh_value.hpp:69-74`, `arena_manifold.cpp:1797-1809`. |
| R3-10 learn boundary | **Fixed** — :1294-1297 enumerable frontier + §11 :1886. |
| R3-11 qualified names | **Fixed** — three-part names throughout + first-colon parse rule. Residual: R4-5. |
| R3-12 §8.5 ordering | **Fixed** — :1610-1617. |
| R3-13 prune wording | **Fixed** — :1117-1118. |
| R3-14 memo slot | **Fixed** — :1245-1249 (`+d.entry` on wrappers; `clause +d.code → +d.step` memo keyed by attach cell). |

## New findings

**R4-1 MAJOR — §9 declarations still cannot compile; changelog overclaims.** The R3-2 fix added declarations but broke them against `vagent-agent-language.md` v3.9. Evidence: (a) `GoalDecl ::= 'goal' Ident '(' [ParamList] ')' GoalKind` (spec :1337-1338) — `goal Patrol` (:1677) and `goal Feed` (:1711) lack both `()` and a kind; `goal Wait(w: Int)` (:1745) lacks the kind. (b) `PredicateDecl` is `TopDecl`-only (spec :1301-1306); `Member` (:1326) excludes it — yet `predicate sees(…)`/`predicate learned_rule(…)` sit inside `agent Forager` (:1714-1715); §9.2's `threat` (:1678) is correctly top-level. (c) `event WithinBudget` (:1746) declares no fields but `emit WithinBudget{n: w}` (:1756) supplies `n` → `UnknownName` "unknown or implicit record field 'n'" (checker.cpp:1463-1466). Fix: `goal Patrol() perform`, `goal Feed() perform`, `goal Wait(w: Int) perform`; hoist both predicates above `agent Forager`; `event WithinBudget { n: Int }`. Changelog :2132 claims "the example declaration fixes" — premature.

**R4-2 MINOR — `Code` arity contradiction.** :177-178 "arity chain maps `Code → 1`" vs :1802 "0-or-1 arity chain (arity 0 = wildcard)" vs changelog :2126 "arity 0-or-1". Under `arguments.size() == arity` (checker.cpp:565), `Code → 1` rejects bare `Code`. Fix :178 to state 0-or-1.

**R4-3 MINOR — uniform-marking rule contradicts name-records.** "every emitter-minted int64 ref cell … gets its own marker" (:919-920, :1338-1344, M11 :345-346) vs "no marker, never re-kinded" for `+d.name` records (:914-915, :1315-1316). The name-recorded `Call` operand *is* an emitter-minted int64 ref cell. Scope the rule ("except `+d.name`-recorded slots") or state the marker is permitted-but-inert there.

**R4-4 MINOR — `learn` registration invariant vs idempotent re-learn.** "Registration is always a fresh, uninstalled `reify` … **never the original**" (:1229-1236) contradicts :1238-1241, where `insertIntoRank`'s no-op fires only if the already-`d.code`-resident argument registers *itself*. State the carve-out: "unless the argument is already the registered head".

**R4-5 MINOR — label-colon rule self-contradicts.** "labels may not contain `:`/`"`" (:1405-1406) vs `src:demo:12:4` (:918), `patch:near_water` (:1017, :1407), `reify:<src>:<serial>` (:1007). The real invariant is that the *module-name component* can't contain `:` (first-colon split then works). Scope or restate.

**R4-6 MINOR — `symbol_named` owner grammar + arity.** Signature `(owner, name)` (:957); genesis calls `symbol_named(qname)` one-arg (:1321, :1371); "taking the agent as owner" (:1083) never defines owner (agent? module?) while lookup runs "the module's `d.vars`". Define the owner→module rule and a one-arg qualified overload or explicit split.

**R4-7 MINOR — stale `(§14)` ref** for `run_checked` (:1069) → §15/U-M10.

**R4-8 MINOR — mdl failure:** no blank line before `### 6.7` (:1392-1393); `make lint` (mdl) is CI-blocking for markdown.

**R4-9 MINOR — duplicated paragraph** :923-930 ≈ :932-936 (marker/+d.ref inventory restated).

**R4-10 MINOR — broken sentence** :767-768 ("(§7.2).\nand `run` can find the cells it spills.").

**R4-11 MINOR — `value_of` (pure) below `run` (opaque-impure)** (:966-967) breaks the "split by purity / top half" claim (:948-950).

**R4-12 MINOR — §8.2 stale:** "a `d.weight` on the clause is its strength" (:1559-1560) vs §8.1 "never directly on a clause cell" (:1528-1533).

**R4-13 MINOR — §6.1 binding list omits the impure-kind set** (:892-894) that §7.2 (:1469) and §10.1 (:1824) require.

**R4-14 MINOR — §7.3 lists resolved items as "genuine open items"** (`reflect` name, P16 pass shape, splice-`def` expansion order) (:1506-1509).

**R4-15 MINOR — genesis ordering omits `opcodeMap_` re-registration** (:1367-1377 vs :1185-1188, P15 row :1845).

**R4-16 MINOR — "`navigate_to "G.travel"`" misdescribes §9.3** (:1327-1329): the call is `navigate_to("water")`; the name-recorded callee is `demo:Forager.navigate_to`.

**R4-17 MINOR — "receives Int64-stale cells … converts each marked cell `OpHandle`→`Int64`"** (:1373-1375) is self-contradictory; the cells arrive `OpHandle`-kind (relocated at publish) and rehydration writes `Int64(*handleTarget)`.

**R4-18 nit bundle:** `Cell` should be `Code` at :1300-1301; "re-kindled" vs "re-kinded" (:1316); `(R2-B)` ruling cite in body (:1274); `code_named` "both reachable by label lookup order" is wrong — the rank searched is `d.code` only (:1413-1416 vs :962); "unreadable in prefix position" doesn't cover `run`/`value_of` (:601-602); `checkpointView` cite covers only `publishValue` (:1331-1332 vs `mesh_actor.hpp:102-106`); "enqueues `handleTarget`" cites the gate `:1797-1809`, not the enqueue `:1759-1764` (:1317-1318); "`Agent.plan`-style" reads two-part (:961); duplicate-label refusal anchored at §6.7 never stated there (:1007); bare `graft(candidate)` pseudocode (:1611).

Checked clean: 11 fences paired/tagged; TOC anchors match all 52 headings; seven tables GFM-valid; dim lists consistent (11 data-minted dims, `d.name` correctly attributed to `initGenesis`); all re-verified cites accurate (`OpcodeKind` is `.hpp`, not `.cpp` — doc correct).

VERDICT: NOT CONVERGED
