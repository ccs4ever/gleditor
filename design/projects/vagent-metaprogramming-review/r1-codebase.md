# Round 1 critique — Codebase Expert lane

Document under review: `design/vagent-metaprogramming.md` (v1.0).
Verified against the live tree under `/data/git/gleditor-restore/apps/common/xanadu/` and the
sibling specs `design/vagent-agent-language.md` (v3.9, confirmed at line 5), `design/
vortex-hyperstructural-runtime.md`, `design/vagent-runtime-grounding.md`, and `design/
vagent-implementation.md`.

Overall grounding quality is high: `mintOpcode` writing `int64(op)` (`vortex_vm.cpp:73-82`),
`opcodeMap_`'s priority over the `asInt64` fallback (`:102-113`), `bindInput`/`bindOutput` on
`VortexCore` (`vortex_core.hpp:236-240`), `compileStatement`'s signature (`compiler.hpp:58-60`)
and `CompiledStatement` shape (`compiler.hpp:28-34`), `emitStatement`'s Choice/Cut/Halt shape
(`compiler.cpp:849-872`), the mark/trail/discard mechanics (`arena_manifold.cpp:1506-1612`),
`compact()`'s mark refusal (`arena_manifold.hpp:670-679`), `checkpointView`'s refusal
(`mesh_actor.cpp:669-674`), `Linked`'s input contract and seen-set (`vortex_vm.cpp:654-723`),
the `cells"…"` subset (`cell_path.cpp:17-183`, spec §8.7 at `vagent-agent-language.md:1239-
1272`), P1–P14's approved status (`vagent-agent-language.md:1659-1681`), and the
PrefixedStringLiteral merge (`lexer.cpp:379-398`, `token.hpp:22`) all check out as claimed.
The findings below are what did not.

______________________________________________________________________

1. **Severity: BLOCKING. Location: §10.2 (P15 row), §6.5, §12.**

   **Problem.** P15 is scoped to "`executeOpcodeBody`'s `Value`/`Splice` cases (`:771-806`),
   `runPipeline` (`:310-350`)" — specifically the `Value` write at `:783`, the `Splice` call
   at `:799`, and `runPipeline`'s `writeValue(paramCell)` at `:337`. But the VM has three more
   internal write sites that restate an arbitrary bound cell's content through the same
   `writeValue` funnel, all reachable with kind-bearing targets and none covered by the
   proposal as written:

   - `runPipeline`'s second write: `writeValue(outCells[i], …)` at `vortex_vm.cpp:343-347`
     (the bound-output write for non-structural pipeline ops — the doc covers only
     `paramCell`).
   - `step`'s memo-hit write-back: `writeValue(outCells[i], …)` at `vortex_vm.cpp:1269-1273`.
   - `step`'s generic output write-back: `writeValue(outCells[i], …)` at
     `vortex_vm.cpp:1287-1294` — this fires for **every** opcode kind not in the
     `{New, Clone, Link, Break, Linked}` exclusion list, i.e. for `Add`, `Bind`, `MakeTerm`,
     `Value`, `Splice`, and all arithmetic/logic ops, writing the op's outputs into whatever
     cells the output wings name.

   A mutated or hand-built graph that binds an op cell to any op's output wing — a
   `Link`/`setLink` re-point of the output wing, exactly the surgery `std:meta` performs, or a
   `cells"…"`/`Value` rewrite of the wing slot's `int64` — then re-executes that op rewrites
   the op cell's `int64` through `writeValue` → `setValueBits` with **no `Value`/`Splice`
   involvement at all**. `opcodeMap_` keeps the old kind; `asInt64` sees the new one — the
   exact desync P15 exists to kill. §12's property "given P15, an op cell's kind cannot change
   after minting" is therefore false under the proposal as specified, and §6.5's premise that
   `Value`/`Splice` are "the two opcodes that restate a target's content" is wrong: every
   opcode restates its bound outputs' content via step's write-back.

   **Evidence.** `vortex_vm.cpp:337,343-347` (`runPipeline`), `:1269-1273` (memo hit),
   `:1287-1294` (generic output write); `writeValue` → `setValueBits` path via
   `VortexCore::writeValue`; `VortexCore::outputsOf`/`inputsOf` resolving wing slots to
   arbitrary cells (`vortex_core.cpp:372-376`).

   **Fix.** Rewrite P15 so the refusal lives at every VM-internal `writeValue`/`splice` call
   site that writes into a caller-bound (non-fresh) cell: the `Value`/`Splice` cases **plus**
   `step`'s two output writes **plus** both `runPipeline` sites — preferably a single
   `getOpcodeKind`-guarded helper (`writeCellValueGuarded`) so the enumeration cannot drift.
   (An alternative is the check inside `VortexCore::writeValue`/`splice`; that moves the
   "Where" to `vortex_core.*` and extends enforcement to host callers — say which is intended,
   since `getOpcodeKind` currently lives on the VM, not the core.) Update §10.2's Where
   column, §6.5's "the two opcodes" premise, and the `runPipeline` parenthetical to name both
   write sites. Keep the fresh-cell exemption (`Bind`'s `valCell` at `:904`, `Call`'s frame at
   `:862`, `New`/`Linked` mints) explicit.

2. **Severity: MAJOR. Location: §1 (`ensureDimension` claim), §14 R12, M11.**

   **Problem.** §1 claims `ensureDimension` "keeps the arena name table, `dimensions()` and
   marks in agreement." Marks are not in the agreement. `struct Mark`
   (`arena_manifold.hpp:170-186`) records `cellCount`/`linkSize`/`contentSize`/`trailSize`/
   `shadowCount`/proxy/quote counts — **no dimension-table field**. `release()`
   (`arena_manifold.cpp:1612-1660`) restores trailed slots and truncates shadows/proxies/
   quotes; it never touches `arenaDims_`. The only writers of `arenaDims_` are
   `ensureDimension` (`:89`), `hydratePrefix` (`:2105` — restores from a snapshot, not mark
   undo), and `exportPrefix` (`:2091-2094` — merely *filters* entries whose cell sits above a
   mark, acknowledging the staleness exists). So `ensureDimension` called under a live mark
   leaves a stale `name → DimRef` pair after `release`: `dimensionNamed` then returns a
   `DimRef` that names a truncated (or worse, re-minted) cell, and `link()`/`dimensions()`
   silently treat it as live. This is precisely the untrailed-side-map desync class the
   document itself diagnoses for `opcodeMap_` and prices P15 to fix — while asserting the same
   property holds for `arenaDims_` by construction. It does not.

   The design's actual paths are safe — the adapter mints `d.code`/`d.patch`/`d.weight`/
   `d.synapse` at genesis with no marks outstanding — but the stated invariant is false and
   could licence exactly the bug P15 prevents.

   **Evidence.** `arena_manifold.hpp:170-186` (Mark fields), `arena_manifold.cpp:1506-1660`
   (trail/mark/discard/release — no `arenaDims_`), `:78-105` (`ensureDimension`), `:48-74`
   (`dimensionNamed` consults `arenaDims_`), `:2091-2105` (export/hydrate handling).

   **Fix.** In §1, drop "and marks" — `ensureDimension` keeps the name table and
   `dimensions()` in agreement with each other; it is *not* mark-covered. State the discipline
   explicitly beside §6.5's opcode rule: "dimensions are minted only when no mark is
   outstanding (the adapter's genesis path); a `dimensionNamed` hit on a released mint aliases
   a wrong cell — the `opcodeMap_` rule applied to `arenaDims_`." Optionally add a §7.3
   unproved-property line.

3. **Severity: MAJOR. Location: §12 (V1 property), §4.5.**

   **Problem.** §12 claims V1 gives "**No cross-arena mutation or execution**: `resolveCell`
   yields `nullopt` for foreign cells, so `run`/`graft`/`cells` on them are `noCell`/`[]` — a
   `Code` value received in a message is inert data elsewhere." The first clause is false for
   aliased refs. `resolveCell` (`vortex_vm.cpp:51-67`) checks only `arena.contains(c)`, which
   bottoms out in `denseOf` (`arena_manifold.cpp:256-267`): for an ephemeral ref
   `ephemeralBit|d` it is a bounds check `d < slots_.size()`. A `Code`/`Cell` ref-box `Int`
   arriving from another agent — or any stale foreign ref — whose dense index happens to lie
   inside the *receiving* arena's extent resolves to a **live local cell**, not `nullopt`.
   `run`/`graft`/`cells` then operate on the wrong cell silently — not inert data but an
   active mutation/execution hazard. The doc itself states the identical hazard across
   `resume` (§4.5: "a stale `Int` can name the wrong cell silently") and prices `d.name` label
   re-resolution for it; the message path asserts inertness instead. `Linked`'s follow step
   has the same `contains`-only check (`vortex_vm.cpp:668-671`), so `cells"$c/+d.x"` follows a
   foreign box to a wrong local member too.

   **Evidence.** `vortex_vm.cpp:51-67` (`resolveCell`), `arena_manifold.cpp:256-267`
   (`denseOf`), `:791-797` (`slot` — `contains` is `denseOf`-or-base, never identity-verified),
   `vortex_vm.cpp:668-671` (`Linked` follow), §4.5's own aliasing sentence at lines 394-396.

   **Fix.** Qualify §12's V1 row: foreign refs fail `contains` only when out of dense range;
   inside range they *alias* live local cells — the same session-handle caveat §4.5 prices for
   `resume` applies to received `Code`/`Cell`. Add the mitigation rule (received `Code`/`Cell`
   values are opaque: re-resolve by `d.name` label before `run`/`graft`; never pass a foreign
   ref-box to `cells"…"`/`Linked`) and a §7.3 "stale handles" line covering the cross-agent
   direction. U-M2's deferral note should say code mobility is unsafe-and-aliased today, not
   inert.

4. **Severity: MINOR. Location: §3 M2 (Price), §10.1.**

   **Problem.** M2's price says "the parser needs an expression entry point (it parses units
   today — new code in vagent's own front end)." The entry point exists:
   `Parser::parseExpression()` (`parser.cpp:271-281`) lexes and parses one expression and
   refuses non-EOF trailing input. What `code"…"` actually owes is the `CodeBody` wrapper —
   the optional `#label`/`params |` header before delegating to `expression()` — not an
   expression entry point. Relatedly, §10.1's new-code list names `code_path.{hpp,cpp}`,
   `fragment.{hpp,cpp}`, checker and compiler additions but omits `parser.cpp` itself, even
   though the `Primary`/`Type` grammar changes (`'code' StrLit`, `'splice' Postfix`,
   `'static' Unary`, `Code<T>`) land there — `parser.cpp:848-855` is where
   `PrefixedStringLiteral` currently rejects `!= "cells"`.

   **Evidence.** `parser.cpp:271-281` (`parseExpression`), `:848-855` (prefix gate),
   `:296-314` (`type()` already parses `Ident<args>` — `Code<Int>` parses today).

   **Fix.** Reword M2's price: "the expression entry point exists (`parseExpression`); owed
   work is the `CodeBody` wrapper (label + parameter header) and a recursion budget." Add
   `parser.cpp` (Primary/Type productions, prefix gate) to §10.1's list.

5. **Severity: MINOR. Location: §6.3, §6.6, §9.2.**

   **Problem.** `graft_where`/`graft_body` "re-point the clause's `+d.grab`/`+d.spin`" — but
   re-point at *what*? The clause's `+d.grab`/`+d.spin` must name the **executable graph
   root** (an op cell — spec §5.7/§5.8, `vortex_vm.cpp` executes ops reached from the clause),
   not a fragment head cell, whose `+d.grab` is a link *to* the entry op. Grafting the head
   itself lands the cursor on a non-op cell (`getOpcodeKind` → `Nop` → silent advance). The
   text never says whether the link targets head or entry; `unlearn`'s "the prior head on a
   `+d.step` chain" repeats the ambiguity — for a replace-patch the prior object is a *graph
   root*, not a fragment head.

   **Evidence.** §6.1's head layout (`+d.grab` → entry op), `vortex_vm.cpp:1218-1225` (non-op
   cell → `Nop`), spec §5.7 clause layout (`vagent-agent-language.md:508`).

   **Fix.** State the convention once in §6.3: "graft links the clause's `+d.grab`/`+d.spin` at
   the fragment's **entry op**, resolved through the head's `+d.grab`; the head rides along
   only for `d.name`/`d.patch` addressing." Fix §6.6/§9.2's "prior head" to "prior graph
   root".

6. **Severity: MINOR. Location: §6.1 (`run` signature), §6.2.**

   **Problem.** `run(c: Code<T>, args…): T` uses `args…`, which vagent's surface grammar
   cannot express: `parameters()` is comma-separated `name: Type` (`parser.cpp:316-339`), and
   `def`/`native` decls have no variadic form. `run(c, 3, flag)` parses as an ordinary `Call`
   node, and "checker-special (like `text(x)`)" covers typing — but `text` is fixed-arity, so
   the variadic *surface* mechanism (arbitrary-arity call accepted by the checker, spilled
   count taken from the descriptor) is unpriced new machinery inside the one feature the doc
   calls "owed".

   **Evidence.** `parser.cpp:316-339` (`parameters`), spec `text` builtin
   (`vagent-agent-language.md:1554`, fixed arity).

   **Fix.** Pick one: declare `run`'s second parameter `args: [Cell]`-style list (no surface
   change, descriptor still validates arity), or state explicitly that the checker treats
   `run` calls variadically (new checker code — list it under §10.1 `checker.cpp additions`).

7. **Severity: MINOR. Location: §5.2 / M3 (`validateType` price).**

   **Problem.** "`validateType` accepts `Code` with arity 1 (`checker.cpp:553-567` gains it
   beside `Cell`)" — mechanically, `scalars` membership alone yields arity **0**: arity is
   computed in a separate chain (`checker.cpp:560-562`, `List`/`Opt`→1, `Map`→2, else 0).
   Adding only `"Code"` to `scalars` makes `Code<T>` reject "wrong number of type arguments".
   The wording "with arity 1" implies both edits but an implementer reading "gains it beside
   `Cell`" will likely miss the arity chain.

   **Evidence.** `checker.cpp:555-557` (scalars set), `:560-562` (arity chain).

   **Fix.** One clause: "`scalars` gains `Code` and the arity chain maps `Code → 1`."

8. **Severity: MINOR. Location: §6.1 (`reify`), §10.1, §15 U-M7.**

   **Problem.** `reify` is described as "a fresh closed copy of a literal-born fragment
   (compile-assisted; §15 U-M7 for foreign graphs)." The compile-assistance is the whole
   mechanism and is unpriced: runtime code cannot dimension-enumerate a subgraph
   (`dimensionsOf` is C++-only — the doc's own U-M7), so literal-born reify needs a
   per-literal reconstruction recipe emitted at compile time (or a descriptor-recorded cell
   census to copy). That emission belongs in §10.1's `fragment.{hpp,cpp}` row; currently it
   lives in a parenthetical, so §10's "what the C++ core has to gain" undercounts the fragment
   emitter's duties.

   **Evidence.** U-M7's own admission (`design/vagent-metaprogramming.md:1213-1217`),
   `dimensionsOf` absence from the op set (`vortex_vm.cpp` opcode list, `vortex_vm.hpp:60-93`).

   **Fix.** Expand "compile-assisted" into §10.1: "`fragment.{hpp,cpp}` also emits the reify
   recipe (descriptor-recorded cell census / reconstruction template) that `std:meta::reify`
   consumes; foreign-graph copy is U-M7."

9. **Severity: MINOR. Location: citation precision (§1, §4.3, §4.4, §4.5, §5.3).**

   **Problem.** A handful of near-miss line cites; the files are right, the numbers are off:

   - §4.4's "`r10-vortex.md`" resolves to `design/projects/vagent-review/r10-vortex.md`, not
     `design/` — cite the path (it is a review artifact, not a sibling design doc).
   - §5.3's `vortex_vm.cpp:859` for Call's frame `makeCell` is the `if (nextOp)` line;
     `makeCell()` is `:860`.
   - §4.3's "`Cut` discards it (`:255`)" — `cut()`'s `discard(cp.mark)` is `vortex_vm.cpp:253`.
   - §1's `arena_manifold.cpp:1506-1610` for the mark/release/discard mechanism — `release()`
     begins at `:1612`; cite `:1506-1660` or split the cites.
   - §4.5's "reachability order" cite `arena_manifold.hpp:235-240` lands on the `Promoted`
     struct; the order itself is the BFS at `arena_manifold.cpp:1744-1788` — cite both.

   **Evidence.** The cited files at the lines above.

   **Fix.** Adjust the five cites as listed; no design change.

10. **Severity: MINOR. Location: §1, §2, §6.1, §10.2, §11 (tables).**

    **Problem.** Several tables are mangled in source: header rows and separator rows are
    split across lines by hard wrapping — e.g. the §10.2 register row reads `| No. | Change |
    Where | Why it cannot live … | Without it | Used by | |` followed by stray `|--- |`
    fragments (lines 1038-1058); the same pattern hits §1 (82-114), §2 (127-151), §6.1
    (581-616), §11 (1064-1104). The proposal register — the document's central artifact for
    this review — is nearly unreadable in source form, and the broken separator rows risk
    mdl/mdformat churn.

    **Evidence.** `design/vagent-metaprogramming.md:82-114, 127-151, 581-616, 1038-1058,
    1064-1104`.

    **Fix.** Re-wrap the tables so each row is one source line (mdformat `--wrap 100`
    reflows prose, not table cells — keep rows unwrapped or rebuild them under the wrap),
    then `mdl` the file.

______________________________________________________________________

## Verified-without-finding spot checks

For the record — claims checked and confirmed as stated, in case other lanes question them:
`Linked` input contract (subject/dim/neg + optional follow/filter, `vortex_vm.cpp:654-723`);
`Call`/`Return` frame mechanics and the "Return orphans the frame" observation
(`:854-892`); `Choice` taking the mark, `backtrack` releasing it and relinking the stack top
(`:215-248`); `cut` discarding marks (`:250-260`); `setCursorOpcode` dual state
(`:131-146`); `Halt` exit (`:1220-1226`); fresh `+d.spin` read per step (`:1332-1335`);
pre/postcondition evaluation (`:1229-1235`, `:1306-1319`); `New`'s already-used refusal
(`:736-744`); `maxCycles` bound (`vortex_vm.hpp:162`, `vortex_vm.cpp:1344-1346`); `Emitter`
holding `core_`/`vm_` (`compiler.cpp:98-100`); the no-marks compile contract
(`compiler.cpp:107-110`); `requireSame` (`:188-193`); `bounded()` (`:577-588`); `Cells`
preflight (`:465-498`); `pureBody` incl. mutating-cells purity (`checker.cpp:827-869`); cells
type-inference validation (`checker.cpp:1412-1429`); `contextual-keyword` claim (`token.cpp:
8-72` — `code`/`splice`/`static`/`succeed`/`holds` all unreserved; only `Identifier` prefixes,
`lexer.cpp:381`); `checkpointView` refusal (`mesh_actor.cpp:669-676`); `promote`
reachability-order minting (`arena_manifold.cpp:1744-1788`); `exportPrefix`/`hydratePrefix`
(`:2057-2125`); `evictOldestMark` (`:2127-2133`); `ValueLimits::operations = 65536`
(`mesh_value.hpp:25-28`); `setExecutionLimits` exists (`mesh_actor.cpp:165,278`);
`noCell == 0` (`dim_vector.hpp:23`); `d.name` from `initGenesis` (`vortex_core.cpp:61`);
`VortexStdLib::call` host dispatch (`vortex_stdlib.cpp:1976-1989`); `vagent check` is the only
command (`apps/vagent/main.cpp:77`); spec claims — v3.9 (`:5`), P1–P14 approved
(`:1659-1681`), §8.7 `cells"…"` (`:1239-1272`), "any other prefix … syntax error" (`:1046`),
"CellsLit is one token" (`:1421`), Cell = "a live arena cell" (`:670`), rank-no-tail-pointer
and predecessor→successor removal (`:491-494`), Opt presence test (`:1103-1104`),
`std:agent` as prelude (`:1536-1544`), §5.8 call/spill convention (`:643-655`), and the
grounding audit's second-cursor bug (`vagent-runtime-grounding.md:100,133-164`).

______________________________________________________________________

VERDICT: NOT CONVERGED
