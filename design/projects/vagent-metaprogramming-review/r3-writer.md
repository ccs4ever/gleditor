# Round-3 Writer Critique — design/vagent-metaprogramming.md v1.2

Scope: v1.2 (~1,919 lines) re-reviewed end-to-end against `vagent-agent-language.md`
v3.9, `apps/common/xanadu/` sources, and the `r2-disposition.md` rulings. Seven GFM
tables verified well-formed; 11 code fences verified paired; TOC consistent with
headings; changelog claims verified against the body.

## A. Round-2 findings re-verdict

| # | Verdict | Evidence |
| --- | --- | --- |
| N1 | **Fixed.** | §7.2 :1315-1324 implements the ruling verbatim: fresh marker cell per pass, `visited +d.validate→marker`, membership `linked(visited,d.validate,POS)==marker` — sound because link eviction keeps exactly one `+d.validate` edge per cell and stale edges name a dead marker — plus op-graph-only traversal (:1314-1315). |
| N2 | **Fixed.** | `run(c: Code<T>, args)` checker special form everywhere (glossary :132, table :915, M6 :226-234, §6.2 :976-997); list literal with per-element `any`; name-bound `#Call` frame; `Return`-tails only, `Halt` refused; opaque-impure; soft `produced`-failure vs hard unwind (:989-995). All call sites use list literals (`[3, flag]` :733, `[count]` :1497, `[candidate]` :1444). |
| N3 | **Fixed.** | P15 tests `opcodeMap_.contains(masterOf(cell))` (:1121, :1649); `masterOf` wraps `cloneMaster` (`vortex_core.cpp:344-345`, `vortex_core.hpp:269-270`). |
| N4 | **Fixed** (wording nit → R3-14). | Wrappers "memoized on the clause by the current attach cell", never on `d.code`, invalidated on attach change (:1159-1166). |
| N5 | **Partially fixed → R3-1 (new MAJOR).** | Name-records on `+d.refs`, no OpHandle on name-records, genesis `symbol_named` resolution, learn-time re-verification — all present (:1188-1203). The *executable* form of the name-bound call is now the hole (below). |
| N6 | **Fixed.** | :620 splices the inner literal with argument `(x)`; §5.2 :642 admits inline literal operands. |
| N7 | **Residual, escalated MINOR→MAJOR → R3-2.** | Predicates/`navigate_to` declared in §9.3, but goals, events, verbs and `import std:meta` still missing — and `learn_plan("Feed",…)` now *fails at run time*, not just check time. |
| N8 | **Residual → MINOR.** | :898-899 still calls the table's top half "the `reflect` family … the read group" while `validate`, `weight_of`, `next_rand` share it. |
| N9 | **Fixed.** | Adapter comment :1625-1627 lists all 11 data-minted dims; matches :481-483 and :1815-1817. |
| N10 | **Fixed.** | :584 "of these only `code` may prefix a string". |
| N11 | **Fixed.** | :186-187 "const-in/judgment-out — it returns a `Type`, never a rewritten tree (`checker.cpp:1121`)". |
| N12 | **Fixed.** | :738-741: param cells chain `+d.step` off the descriptor's `+d.grab` in declaration order, same cells the `+d.vars` entries name. |
| N13 | **Residual → MINOR (R3-8).** | Signature still `unlearn(clause: Cell)` (:924) — cannot name a `learn_method` symbol patch; "matching" still undefined; the empty-`+d.grab` window until next genesis still unstated (:1239-1241, :1519). |
| N14 | **Fixed.** | :1003-1005 refuses bound-output `New` "append and non-append forms alike". |
| N15 | **Mostly fixed → MINOR (R3-9).** | `release` cite now :1612-1700 (:502); `Cut` `:255` (:504); contract cites bracket section comments. Remaining: `enableMemoization` actual :169-171, doc :170-172 (:492). |
| N16 | **Fixed.** | Glossary :136 verbatim fix. |
| N17 | **Fixed.** | :322, :1754 "once per bound-output writeback". |

## B. New findings in v1.2

### R3-1 [MAJOR] Name-bound program references have no specified executable form — and under the alternative reading they reopen the V12 hole

**Location:** §6.6 :1188-1194, :1212-1229; head diagram :873-879; §9.3 :1532-1539;
register P16 :1650.

**Problem.** `closed` (b) says outside references are "either (a) a link to a
state-side cell … or (b) a *name-bound program symbol* carried as a name-record on
`+d.refs` … int-encoded program references do not [stay legal and durable]"
(:1189-1192). So a closed fragment's `def` call — §9.3's flagship
`code"{ navigate_to(\"water\") }"` — cannot carry an int64 operand naming the
program op. Two readings, both unsatisfied:

- *Name-dispatch:* the fragment executes the call by resolving the name-record at
  run time. But the only mention is the two-word parenthetical "symbol dispatch"
  (:1191). What fills the `Call` operand in-session (a trampoline? a per-call
  resolved slot?), and what genesis step (c)'s "resolve name-records through
  `symbol_named`" (:1229) *writes*, if anything, is unstated. Under pure
  per-call dispatch, genesis resolution is validation-only — but then a callee
  renamed between checkpoint and resume silently dead-calls, with no stated
  fault.
- *Operand int64 + name-record metadata:* the operand cell holds the program
  ref for in-session execution, rehydrated by name at genesis. Then the cell is
  emitter-minted int64 — and :879, :1212-1215 and :1650 mark **every** such cell
  `d.mesh.relocation→"cell"`. The pre-pass re-kinds it `OpHandle`; `promote`
  enqueues `handleTarget` (`arena_manifold.cpp:1759-1764`), which `contains` the
  program cell → the program cell is dragged into the checkpoint — the exact V12
  failure the `d.code` invariant exists to prevent. This reading additionally
  needs a `+d.ref` edge from the name-record to the cell it rehydrates; the
  diagram gives name-records only `+d.values→label, +d.name→qualified name`
  (:876).

**Evidence.** `promote` BFS enqueues relocation-handle targets
(`arena_manifold.cpp:1759-1764`); the only exemption the doc gives is
runtime-minted ref cells (`Linked`-born, :1217-1219). The `closed` re-verification
(:1195-1197) would *refuse* a marked program-ref cell — which is consistent with
name-dispatch but again leaves execution unexplained.

**Fix.** State explicitly, in §6.6 and the head diagram: (i) how a name-bound
call executes in-session — e.g., the emission rewrites the operand to a
fragment-local dispatch stub that resolves the name-record via `symbol_named`
and writes the result into a *runtime-minted* ref cell per activation (which
lands inside the existing runtime-minted carve-out, :1217-1219); (ii) that
name-bound cells never carry the relocation marker; (iii) what genesis (c) does
with a resolved name (validation + fault when unresolvable, or the write
target); (iv) add the `+d.ref` edge to the name-record shape if any cell is
rewritten. One paragraph closes it.

### R3-2 [MAJOR] N7 residual — worked examples still undeclare their goals, events, verbs and `std:meta` itself; §9.3 now fails at run time, not just check time

**Location:** §8.3 :1407; §9.1 :1487; §9.2 :1504-1510; §9.3 :1529-1549;
§9.4 :1561-1567; import rule :848.

**Problem.** Undeclared program entities: `goal Patrol` (:1504), `goal Feed`
(:1537), `goal Wait(w)` (:1561, :1565); `event Tick` (:1487), `Attack` (:1508),
`Sighting`/`Meal` (:1534, :1544), `PerceptPair` (:1407), `WithinBudget` (:1567 —
`emit` requires a declared event, spec :950/:1219); `predicate threat` (:1504);
`def patrol`/`hold` (:1505, :1562). And no example that calls `std:meta` members
(`clauses_of`, `symbol_named`, `learn_*`, `weight_of`, `set_weight`, `run`,
`agent_cell`, `plan_named`) includes `import std:meta`, despite :848's own
"It is not implicitly imported".

The escalation: v1.2 fixed §9.3's `def`/predicates but left `learn_plan("Feed",…)`
naming a goal that was never declared. `graft_plan` finds the goal "by name
through `symbol_named`" (:1025-1026); with no `goal Feed` the lookup returns
`none`, the statement fails, `believe learned_rule(…)` never runs — the example
demonstrating "Hebbian growth of a rule rank" grows nothing.

**Fix.** Per the r2 fix that was only half-applied: declare the goals/events/
verbs in each example (one line each), or add a single "declarations elided"
note; and put `import std:meta` at the top of every std:meta-using snippet (or
state once that snippets assume it).

## C. Residual/minor findings

- **R3-3 [MINOR] §6.1 head diagram self-contradicts.** :873 draws each `+d.refs`
  record "`+d.values`→one marked ref cell"; :874-875 corrects to "`+d.ref`→its ref
  cell — P16's mark dim, NOT `+d.values`". Delete the first clause's `+d.values`.
- **R3-4 [MINOR] Descriptor result-cell link unstated.** :866 shows
  `["result"] +d.values→"Int"`; §5.4 :741-742 promises links to the
  `result`/`produced` *cells themselves*. `produced`'s cell edge is drawn (:867);
  the result cell's edge has no dim (`+d.values` is taken by the type name). Name
  the dim or split the records.
- **R3-5 [MINOR] `unlearn`'s re-point omits the mandatory reify.** :924 "re-point
  to the patch's recorded prior head" — the prior head is `d.code`-registered;
  re-pointing a clause's `+d.grab` at it installs it in place, violating the
  section's own "reify first / never grafted in place" (:1049-1050, :1156-1158).
  Say "installs a `reify` of the named prior head" (the R2-B ruling's words).
- **R3-6 [MINOR] `run_checked` "(§14)" dead-ends.** :1013-1014 names the
  `run_checked`/`run_pure` future candidate "(§14)"; §14 contains no entry. Add a
  U-M item to §15 or drop the ref (the changelog :1909 claims it is "the named
  candidate").
- **R3-7 [MINOR] `value_of` missing from the API table.** §6.2 :995-997 defines
  `value_of(c: Cell): any` as the `Code<Cell>` companion, but the §6.1 table
  (:901-927) omits it.
- **R3-8 [MINOR] N13 residuals.** `unlearn(clause: Cell)` (:924) cannot name a
  `learn_method` symbol patch ("clause or symbol cell"); "the latest matching
  patch" never says what matching compares (suggest: "the patch whose target's
  resolved name equals the argument's"); and the empty-`+d.grab` window between a
  program-original `unlearn` and next genesis (:1239-1241, :1519) remains
  undescribed — is a test-less clause always- or never-applicable?
- **R3-9 [MINOR] Citation wobbles remain.** `enableMemoization` is
  `vortex_vm.cpp:169-171` (doc `:170-172`, :492); contract cites bracket section
  comments but end mid-loop (pre `:1229-1235`, eval loop :1230-1239; post
  `:1306-1318`, loop through :1322).
- **R3-10 [MINOR] `learn` re-verification boundary.** :1195-1196 walks
  "`+d.refs` records and capture links"; an outside edge smuggled onto a
  non-op-graph dim evades both this check and `validate` (op-graph dims only,
  :1314-1315). One clause bounding the check to the enumerable frontier and
  naming it a §11-door risk suffices — `dimensionsOf` is C++-only (U-M7).
- **R3-11 [MINOR] Qualified-name format drift.** `plan_named("G.p")` :934 is
  two-part while the convention is `module:Agent.plan` three-part (:963, :1183,
  :910); §9.3's `learn_plan("Feed",…)` bare goal name vs
  `symbol_named(owner, name)` two-string lookup (:906, :1025-1026).
- **R3-12 [MINOR] §8.5 pseudocode misleading.** "mutate(candidate): e.g.
  `graft_where` with a perturbed test" (:1442) — `graft_where(clause: Cell,
  test)` takes a clause cell, not the `Code` candidate, and appears before
  `graft(candidate)`. Intended order: graft the candidate, then `graft_where` on
  the installed clause — or note mutation is raw `cells`+`Value`.
- **R3-13 [MINOR] `prune` wording.** :1055-1056 "stays reachable through the
  patch" — patches record head *names*, not links; reachability is via `d.code`
  registration. "reachable through its registration, which the patch names".
- **R3-14 [MINOR] Wrapper memoization slot unstated.** :1159-1163 memoizes
  wrappers "on the clause by the current attach cell" — the dim the wrapper edge
  lives on, and whether `test_of`/`body_of` memos share one slot (which would
  thrash between the two attach cells), is unspecified. One clause.

## D. Checked-and-clean

- Seven tables (field :3-10, status :78-105, glossary :122-143, survival
  :543-548, `std:meta` :901-927, register :1647-1650, refused :1661-1687) are
  GFM-valid and internally consistent — including the new `run`/`learn_*`/
  `unlearn`/`graft_*` rows.
- 11 code fences, all paired; `ebnf`/`text`/`vagent` languages consistent.
- TOC :51-66 matches all 15 headings + appendix; the 1.1/1.2 changelog entries
  match the body (dim lists consistent at :481-483, :1625-1627, :1815-1817).
- Spec citations re-verified: §5.8 frame/value convention, §6.3 selection order,
  §6.7 `contract` cause, §7.5 percepts, §8.1 prefix rule + case convention +
  escapes, §8.7 cells-subset, §10 prelude precedent, §11.1 call-ABI debt,
  §11.2 P1–P14 register, §15 fuzzing — all accurate.
- Code citations re-verified: `foreign_op_handle.cpp:98-117`,
  `mesh_evaluator.cpp:720-736`, `mesh_actor.cpp:182-271` (WorkerContext takes
  the checkpoint manifold as base before `initGenesis` — genesis ordering
  sound), `arena_manifold.cpp:1347-1377`, `:1744-1788`, `:1759-1764`,
  `:1934-1954`, `:1863-1870`, `:2020-2042`, `vortex_vm.cpp:50-67`, `:73-82`,
  `:102-114`, `:169-181`, `:214-255`, `:654-769`, `:771-893`, `:1218-1335`,
  `vortex_core.cpp:55-127`, `:180-228`, `:344-345`, `:354-414`,
  `checker.cpp:30-45`, `:553-597`, `:765-865`, `:2058-2079`,
  `compiler.cpp:100-110`, `:478-497`, `:512-524`, `:760-784`,
  `vortex_stdlib.cpp:3800-3802`, `:3841-3843`, `Makefile:686-695`,
  `dimension_registry.cpp`, `manifold.cpp:782-821` — all accurate except the
  residuals at R3-9.
- Design-level consistency confirmed: learned grafts attach program-side so
  installed copies are never promoted (only `d.code` originals + `d.patch`
  replay); dim cells self-reconcile by name through promoted copies;
  `ensureDimension`-after-overlay ordering matches existing resume practice.

VERDICT: NOT CONVERGED
