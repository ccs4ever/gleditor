# Round-2 writer critique — `design/vagent-metaprogramming.md` v1.1

Review lane: clarity, internal consistency, specification completeness, cross-reference accuracy,
example conformance and Markdown hygiene. Checked against `apps/common/xanadu/` (vortex, zigzag,
vagent, vql, mesh) and `design/vagent-agent-language.md` v3.9. Line numbers are v1.1's own (1,587
lines). Round-1 findings are taken from `r1-writer.md` under the rulings of `r1-disposition.md`.

## A. Disposition of round-1 findings

1. **`reflect` used but never defined — RESOLVED.** The glossary (:139) defines `reflect` as "the
   collective name for `std:meta`'s program-structure read functions … not a distinct verb"; §6.1
   (:754-755) repeats it over the member table; M8 (:249) and §7.3's open item (:1097) agree. Every
   remaining use is family terminology (`reflect`-family result :572, `reflect`-wrapped :787, :816,
   :996); no standalone `reflect(` call exists. One wording seam survives — MINOR N8.
2. **Contradictory `splice` legality for `def` parameters — RESOLVED.** M13 (:331-340) and §5.2
   (:562-575) draw one consistent line: a `Code` parameter is splicable only while its `def` is
   expanded at a call site; elsewhere it is opaque and composes through `run`/`graft` (:572-575).
   §5.4 (:639-642), the glossary (:127-128), §7.1 (:1041-1043) and the `graft_method`/effect-ness
   bars (:336-337, :874-875) all agree. Residual: the §5.1 :543 example violates the arity rule and
   uses an operand the statically-known enumeration omits — MINOR N6.
3. **Durable `learn`/promotion corruption through int64 refs — RESOLVED at the mechanism level; a
   related hole remains — MAJOR N5.** P16 is registered (§10.2 :1369), scoped to fragment-owned ref
   cells (`+d.refs` mark rank :305-308, :739, :1335-1336), on the `mesh_evaluator.cpp:720-736`
   publication precedent, with genesis/graft rehydration through the promote mapping's old→new
   association. The P16-conditionality is marked everywhere it matters (:90, :93, :133, :309-310,
   :977-979, :1426-1428). The r1 codebase caveat is honoured: the doc no longer overclaims
   mark-coverage — `Mark` records no dimension field (`arena_manifold.hpp:170-186`) and dims/op
   mints are confined to the no-marks genesis path (:107-112, :409-412, :957-958). Repo premises
   verified: `promote` copies Int64 verbatim, translating only links/`OpHandle` values
   (`arena_manifold.cpp:1934-1954`).
4. **Graft links on `+d.grab` evict opcode wings — RESOLVED.** Head indirection on dedicated
   `d.entry` is specified and used uniformly: glossary (:125), layout (:729-742), wing-eviction
   rationale (:744-752), §6.3 (:850-853), `graft_method` (:864-867), M14 (:342-351), §9.2 (:1257),
   §10.1 (:1356). Occupancy is the head's `-d.grab`/`-d.spin`/`-d.values` backlinks — free slots on
   a wingless cell — consistent throughout (:132, :345-346, :748). New ambiguity on the sibling
   `d.code` dim — MAJOR N4.
5. **Wrong r10-vortex citation — RESOLVED.** :464 cites `projects/vagent-review/r10-vortex.md`,
   which exists and covers the `cells"…"`/`Linked` traversal substrate.
6. **Worked examples do not type-check — RESOLVED with residual MINORs.** §9.3 declares typed
   predicates (:1266-1267), uses quoted `Str` atoms (:1273-1277), gives `navigate_to` a
   non-suspending `def` (:1269) and documents the escape rule (:1290-1292); §9.4's `> 0` comparison
   fixes the bare-Bool condition (:1304, :1312-1313) and the scope note (:1313-1315) names each
   example's assumed lowering. Residuals: undeclared events/goals/records (MINOR N7), the §5.1
   nested-splice slip (MINOR N6).
7. **Deferred-row ordering/completeness — RESOLVED.** §15 orders U-M5 (:1538) before U-M6 (:1541)
   and carries rows for U-M7 (:1544), U-M8 (:1549) and U-M9 (:1553); the §1 register rows
   (:100-104) match.
8. **Invalid `§6.5.3` reference — RESOLVED.** :1092 now reads "(§6.5, item 3)", and §6.5's item 3
   (:954-959) is the caller-discipline content referenced.
9. **`splice`/`static` call-position reservation — RESOLVED.** §5.1 (:511-514) states the prefix
   forms take operand position, user `def`/`method`s so named are unreachable as bare calls and
   rejected by the checker, and `x.splice()` stays legal.
10. **`code` adjacency rule — RESOLVED.** §5.1 (:502-505) documents the no-gap
    `PrefixedStringLiteral` merge (`lexer.cpp:381-393`) and that `code "…"` cannot form an
    expression, mirroring the spec's `CellsLit` note.
11. **Open-questions apparatus — RESOLVED.** §7.3's last bullet (:1095-1098) explicitly records the
    house-style deviation and names where items live (§7.3 + §15); §15 (:1522-1523)
    cross-references back.

## B. New findings

### BLOCKING

**N1. §7.2 (:1068-1071) — `validate`'s seen-set cannot exist on one dimension.** Specified
mechanism: "mark-links on the adapter-minted `d.validate` dim keyed by a per-pass fresh marker cell
— `marker +d.validate → visited`, membership tested as `visited -d.validate == marker`". One cell
holds exactly one `+d` and one `-d` neighbour per dim (`link` evicts,
`arena_manifold.cpp:1347-1377` — the rule the doc itself invokes at :240-241, :319-320, :745-746,
:1117-1118, :1402). With `marker → visited`, the marker's single `+d.validate` edge means marking a
second cell evicts the first backlink; with `visited → marker`, the marker's single `-d` edge evicts
the first cell's forward mark (`displacedIt`). Either orientation holds at most one member — the
cycle guard sees only the last-marked cell. The freshness rationale is backwards: "its own links
evict the first pass's backlinks" is the very eviction that kills within-pass marks. The
r1-disposition alternative (a fresh `d.validate` *dim* per pass) is independently closed by the
doc's own no-marks minting rule (:107-112, :409-412): `validate` runs mid-turn under the turn mark,
and dims mint only at genesis. **Fix:** take a `mark()` before the walk and `release` it after
(marks vanish with the pass; membership = "v has a `+d.validate` edge"); or self-links
`v +d.validate → v` plus a cleanup walk; or an explicit seen-rank walked O(n) — the last matches
the doc's own O(n·membership) pricing (:1072-1073).

### MAJOR

**N2. `run(c: Code<T>, args: [Cell]): T` is contradicted by the doc's prose and every call site.**
The signature is stated identically three times (:130, :213, :771), yet §5.4 :627 writes
`run(c, [3, flag])` (`Int`/`Bool`), §9.1 :1237 `run(inc, [count])` (`Int` field), and §8.5 :1184
`run(fitness, [candidate])` (`Code`). The prose model sides with the examples: "the argument list
is spilled onto the `#Call` frame exactly as method arguments are" (:627-629) — method arguments
are values, not ref-boxes. Either `[Cell]` is the wrong element type (the row should spell a
heterogeneous, checker-special arg list arity-checked against the descriptor at run time, per
§6.2 :823) or callers must box arguments — undocumented and inconsistent with the frame-spill
description. **Fix:** restate the second operand as a list expression whose elements are ordinary
values spilled per §5.8 (drop or gloss `[Cell]`), or make boxing explicit and rewrite
§5.4/§8.5/§9.1.

**N3. P15's membership test must apply to the resolved master, not the operand — a `Clone` of a
registered op bypasses the funnel.** §6.5 item 2 (:942-947) and P15 (:1368) refuse a target "when
`opcodeMap_` contains it". `writeValue` resolves through `masterOf(cell)` (`vortex_core.cpp:202` —
the doc itself cites `masterOf` sharing at :804): the master's bits are what actually change.
`Clone` (`vortex_vm.cpp:807-836`) appends an unregistered clone cell to the master's `d.clone`
rank; `Value`-writing the clone passes the membership test while the registered master's `int64`
is rewritten — the exact stale-kind desync P15 exists to forbid, reachable in pure Vortex code.
**Fix:** specify the predicate on `masterOf(cell)` ("the resolved master cell"), and note in the
register row that `Clone`-aliased subjects resolve to the master via `vortex_core.cpp:181,202`.

**N4. `test_of`/`body_of` memoization "per clause cell on `d.code`" is ambiguous — one reading
silently breaks V12.** M8 :257 and the table (:764-765) memoize wrapper heads "on `d.code`", the
dim §6.7 :1012 defines as the **agent → fragment heads** registry that makes a fragment
checkpoint-reachable. If wrappers sit on that rank, each wrapper's `+d.entry` points into a
program-side clause graph and promotes it — the state→program leak the V12 amendment (:298-300),
the no-prior-head rule (:983-988) and P16 exist to prevent. The safe reading (a clause-side edge
`clause +d.code → wrapper`, session-scoped, dying with the program at `resume`) is implied only by
"per clause cell" and never stated. **Fix:** one sentence pinning the memo edge's owner — e.g.
"the memo edge is `clause +d.code → wrapper`; it is never on the agent's `d.code` rank (§6.7), so
wrappers die with the program at `resume` like the clauses they wrap."

**N5. `learn`ed fragments that call program-side `def`s cannot survive `resume` — and §9.3's
flagship example does exactly that.** The learned body is `code"{ navigate_to(\"water\") }"`
(:1276): a call to a program-declared `def` lowers to `#Call` with an int-encoded ref-box target
(:928; `reference()` cells, `compiler.cpp:516-518`) naming a *program-side* op cell. P16 relocates
fragment-owned ref cells on `+d.refs`, but its rehydration runs "through the same old→new
association `promote`/`importValue` already computes" (:307-309) — which covers promoted cells
only; the call target is never promoted (V12, :297). Either promotion drags the program cell in
(the V12 violation) or the handle dangles at resume. `closed` cannot catch it as specified: the
flag marks which *captured bindings* are state-side (:1001-1003, :135) — a name-resolved `def`
call is not a captured binding, so the fragment reads `closed` and `learn` accepts it. If `closed`
is instead read to cover call targets, §9.3's `learn_plan` refuses its own body and the example is
self-refuting. **Fix:** choose and state one — (a) extend `closed`/`validate` so `learn`able
fragments must have ref-cell targets resolving within fragment/state cells, and give §9.3 a
self-contained body; or (b) record `Call` targets by qualified name on `+d.refs` and re-resolve
via `symbol_named` at genesis replay. (§9.2's `learn_where` survives: `believes` resolves
predicate cells on the state-side `d.belief` rank, spec :478.)

### MINOR

**N6. §5.1 :543 — the nesting example doubly violates the `splice` rules.**
`code"x + splice code\"n: Int | n + 1\""` (a) splices an inline literal, yet the statically-known
enumeration (:562-564) admits only a `let`-bound literal in scope or an expanding `def`'s `Code`
parameter — an inline literal is unlisted; (b) supplies no argument list for a `CodeParams` header
that declares `n: Int` — the arity check of :570-571 fails and `n` would be unbound at the site.
**Fix:** admit inline literals in the enumeration and splice with arguments —
`splice code"n: Int | n + 1"(x)` — or use an unparameterized inner literal.

**N7. §8.3/§9 examples still reference undeclared events, goals, records and verbs.** `on Tick`
(:1227), `for Patrol` (:1244), `on Attack` (:1248), `on Sighting`/`on Meal` (:1271, :1281),
`learn_plan("Feed", …)` with no `goal Feed` (:1274 — worse than a check error: the patch target
never resolves at genesis), `for Wait(w)` (:1298, :1302), `on PerceptPair` (:1147),
`emit WithinBudget{n: w}` (:1304 — `emit` needs a declared `event`, spec :950/:1219),
`patrol()`/`hold()` (:1245, :1299). `on E` requires a declared `event` (checker `DeclKind::Event`,
`checker.cpp:511-526`) and `for G` a declared `goal` (spec §6.2 :721-728). **Fix:** one decl per
name or an explicit "declarations elided" note, as §9.3 already does for `navigate_to`.

**N8. §6.1 :754-755 — "the `reflect` family is the read group in the top half" conflicts with the
glossary's closed list (:139).** By purity the top half also contains `validate`, `weight_of`,
`next_rand` and `run` (conditionally), none of which the glossary enumerates. **Fix:** reword —
"the program-structure reads — the `reflect` family of the glossary — sit in the top half
alongside `validate`, `weight_of` and `next_rand`".

**N9. §10.1 :1348-1350 — the adapter comment lists 7 of the 9 data-minted dims** (`d.refs`,
`d.recipe` missing) while :409-410, :721-722, :94 and :1511-1512 enumerate nine. **Fix:** complete
the list or say "the §6.1 dimension set".

**N10. §5.1 :508 — "only `code` may prefix a string" is false as written** — `cells` prefixes a
string; the claim means "of these three contextual keywords". **Fix:** "of these, only `code` may
prefix a string".

**N11. M4 :181 — "`Checker::infer` is const-in/void-out (`checker.cpp:1121`)".** `infer` returns
`Type` (`checker.cpp:1121`); the intended point — it cannot hand back a rewritten AST — is right,
but the signature claim is wrong. **Fix:** "const-in, judgment-out — it returns a `Type`, never a
rewritten tree".

**N12. Descriptor param rank — direction and chain dim unstated.** The diagram (:738) puts
`[param cell]*` on the descriptor's `+d.grab`; §5.4 (:631-633) describes an ordered rank of
`+d.name`/`+d.values`-labelled param cells. What chains them (`+d.step` like wing ranks?) and
whether they are the same cells the `+d.vars` entries name is unspecified — and `graft_*`'s
signature comparison (:634-635) depends on the order. **Fix:** one line — "parameter cells chain
`+d.step` off the descriptor's `+d.grab` in declaration order, and are the same cells the `+d.vars`
entries name".

**N13. `unlearn` (:780) underspecified for non-clause patches.** Parameter `clause: Cell` cannot
name a `learn_method` symbol patch, and "the latest matching patch" never says what matching
compares. Also, `unlearn` of a program-original replace "simply detaches" (§9.2 :1259) — leaving
the clause's `+d.grab` empty and the plan test-less until `resume` restores the skeleton
(:986-988); presumably intended but unstated. **Fix:** "clause or symbol cell; matching = the
patch whose target's resolved name equals the argument's", plus a sentence on the empty-`+d.grab`
window.

**N14. "`New` refuses an already-linked bound output" is narrower than stated.** The refusal lives
inside the append-mode bound-output branch (`vortex_vm.cpp:736-744`); a non-append bound-output
`New` re-links silently via `setLink`. Compiled `%` creations always pass `literal(true)`
(`compiler.cpp:793-799`), so the claim holds for emitted shapes — but `validate`'s rule (:838,
:1066) should state it covers both forms. **Fix:** "refuses in append form; a non-append bound
output re-links, which `validate` also refuses".

**N15. Citation wobbles (±1–4 lines).** `release`'s truncation cite `arena_manifold.cpp:1612-1660`
(:429) ends before the resizes at :1671-1676 that truncate minted cells — cite through `:1676`.
`enableMemoization` is :169-171, doc says `:170-172` (:419). `cut`'s `discard(cp.mark)` is :255,
doc says `:253` (:431). Precondition eval runs :1230-1239, doc says `:1229-1235` (:901);
postcondition eval :1313-1322, doc says `:1306-1318` (:901). None changes meaning.

**N16. Glossary `patch` (:134) says "a `+d.patch-head` chain"** but §6.6 (:965-966) puts the
patch→records link on `+d.patch-head` and the record chain on `+d.step`. **Fix:** "a
`+d.patch-head` link to a `+d.step` chain of recorded head *names*".

**N17. §13 :1454-1455 — "once per op per step at most" overstates.** The generic writeback loops
`outCells` (`vortex_vm.cpp:1287-1294`); `DivMod` writes two outputs per step. **Fix:** "once per
bound-output writeback".

## C. Cross-cutting verification

- **GFM tables:** all seven tables (field :3-10, status :77-104, glossary :120-141, survival
  :468-473, `std:meta` :757-783, register :1366-1369, refused :1380-1403) are well-formed pipe
  tables with header separators — the v1.0 flattening is fully repaired.
- **Fences:** every `ebnf`/`text`/`vagent` block opens and closes in pairs.
- **`§` cross-references:** all internal `§N[.M]` references resolve; "(§6.5, item 3)" resolves as
  numbered. Spec references (§5.1, §5.7, §5.8, §5.9, §6.2–6.4, §6.7, §7.5, §8.1, §8.7, §10, §11.1,
  §14, §15, V2–V16, U-A1/2/7/9/12, P1–P14) all exist in `vagent-agent-language.md`.
- **Terminology:** `splice` (keyword) vs `#Splice` (opcode) distinguished at :127 and respected;
  `#Call`/`#Linked`/`#Choice` used with the prefix, bare `Link`/`Value`/`Splice` for opcode names
  in prose lists — cosmetic inconsistency only. `reflect` family-vs-verb consistent (N8 aside);
  `installed`/consumption and "graft shares, reify isolates" used uniformly; `expand`/`cloneExpr`/
  `cap$N` consistent — lexer verified `cap$N` unlexable (`lexer.cpp:14-23`, `:333-335`), backing
  §5.3's hygiene claim (:594).
- **Changelog:** the v1.1 entry (:1571-1587) is accurate — every claimed change exists in the
  body; the Makefile note (:1351-1353) correctly frames the `cell_path.cpp` repair as pre-existing
  context; P17 is recorded deferred-not-registered (:1371-1374) per the disposition.
- **Repo citations:** all spot-checked `file:line` citations land on the claimed constructs
  (`mintOpcode` :73-82, `getOpcodeKind` :102-114, `resolveCell` :50-67, wing machinery
  `vortex_core.cpp:354-414`, trail :1506-1570, `release` :1612-1700, promotion :1934-1954, BFS
  :1744-1788, `Choice` alt-kind check :1156-1158, `New` append refusal :736-744, `Halt` :1220-1225,
  PC advance :1333-1335, drain `vortex_stdlib.cpp:3800-3802`/:3841-3843, `ActorDown` channel
  `mesh_actor.cpp:473-479`, `OpHandle` precedent `mesh_evaluator.cpp:720-736`, cells-subset
  `cell_path.cpp:17-183`, scalar-subset gate `compiler.cpp:500-503`, no-marks contract
  `compiler.cpp:108-110`/`compiler.hpp:51-57`, `pureBody` `checker.cpp:827`, mutating-cells check
  `:861-865`, import allowlist `:2061-2068`, builtin table `:765-779`, `CheckLimits`
  `checker.hpp:12-27`, pointer-keyed tables `compiler.cpp:884-891`, `Mod`/`DivMod` presence
  `vortex_vm.hpp:33-34`) except the wobbles collected as N15.
- **Markdown hygiene:** no tabs, no trailing whitespace; prose lines within wrap convention;
  heading numbers match the Contents list (:50-65).

## D. Summary

v1.1 resolves all eleven round-1 findings, most in full, and the added machinery is specified with
real file-level grounding: head indirection on `d.entry`, `+d.patch-head` name records, P15's
write-funnel membership test, P16's `+d.refs` relocation scope, M13 templates, M14 consumption,
the `expand` stage with `cloneExpr`/`cap$N`, junction synapses, and the drain-idiom discipline.
Remaining: one mechanism that cannot exist as specified (N1), two correctness gaps in flagship
mechanisms (N3's clone-alias bypass of P15; N5's un-resumable `learn`ed bodies calling
program-side `def`s), one signature contradicted by its own examples (N2), one ambiguity with a
hazardous wrong reading (N4), and twelve minor consistency/citation/example defects. All are
paragraph-scale repairs; none requires re-architecting.

VERDICT: NOT CONVERGED
