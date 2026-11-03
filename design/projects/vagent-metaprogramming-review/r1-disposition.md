# R1 disposition — design/vagent-metaprogramming.md

Seven critics ran against v1.0 (writer, vortex, codebase, architect, ergo, zigzag, meta). All
returned NOT CONVERGED; the tallies across lanes are ~6 blocking, ~30 major, ~46 minor. This file
records the orchestrator's ruling on each finding after verifying the load-bearing claims against
the tree. Verification notes: `promote` copies Int64 content verbatim and relocates only links and
`OpHandle` values (`arena_manifold.cpp:1934-1954`); `bindInput` occupies `+d.grab` POS and
`bindOutput` `-d.grab` NEG (`vortex_core.cpp:354-414`) so a second `+d.grab` link on an op cell
evicts an input wing (`arena_manifold.cpp:1347-1377`); `jump`/`Call` targets are `int64` slot
content (`compiler.cpp:516-525`); `getOpcodeKind` decodes any int cell via `asInt64` fallback
(`vortex_vm.cpp:107-112`), not only `opcodeMap_` members; `cursorPC_`, `opcodeMap_`, `arenaDims_`
are untrailed (`vortex_vm.hpp:186-188`); `Mark` covers no dims or VM side maps
(`arena_manifold.hpp:170-186`); `Checker::infer` is const-in/void-out and `Expr` has no clone
(`checker.cpp:1121`, `ast.hpp:90-109`). Real pre-existing build defect found and fixed in passing:
`VAGENT_FRONTEND_SRCS` omitted `cell_path.cpp` (plus its `vql` parser and `scanner_base`
dependencies), so `make vagent`/`vagent_frontend_test`/`fuzz_vagent_frontend` could not link —
Makefile updated, `build/vagent` now links.

## Rulings

### BLOCKING — accepted, design must be revised

1. **Int64 intra-graph references die at promote** (vortex-1, zigzag-2, writer-3, codebase-B* —
   convergent). `jump`/`Call`/`Choice`/`Branch` targets and operand wing slots are int64 cell
   content; `promote` renumbers cells and rewrites links/`OpHandle` values only. A `d.code`/`learn`ed
   fragment promoted at checkpoint resumes with every internal reference aliasing an unrelated cell.
   `closed` constrains links, not int content, so even closed fragments carry the hazard.
   **Ruling: ACCEPTED.** Register **P16**: fragment emission marks fragment-owned reference cells
   (operand slot ints and control-flow targets) so a checkpoint-time relocation pass rewrites them
   to `OpHandle` values before `promote` walks the arena — the `mesh_evaluator.cpp:720-736`
   publication pass is the precedent — and genesis/graft-time rehydration converts rehydrated
   handles back to fresh int64 refs via an old→new association. Scope: fragment-owned cells only;
   the main program skeleton is not promoted, matching the promotion-reachability argument. The
   doc's §4.5 table gains an "intra-graph reference ints" row. `learn`/`d.patch` durability is
   conditional on P16; without it they must be stated session-only.

2. **`+d.grab` cannot carry graft/entry/head links** (zigzag-1 — convergent with codebase's wing
   mechanics and architect's writeback finding). `ArenaManifold::link` evicts the existing
   same-dim-same-direction neighbour; graft links on `+d.grab` would evict opcode input wings or
   decapitate fragments. **Ruling: ACCEPTED.** Adopt the convention the zigzag critic proposed:
   (a) clause/program `+d.grab` links point at fragment *head* indirection cells, never op cells;
   (b) head→entry uses a dedicated `d.entry` dimension; (c) patch→head on `+d.step` or a
   `d.patch-head` dim, disjoint from both `+d.grab`/`+d.step` semantics — the doc must pick one and
   name it; (d) a `Code` value is consumed (marked installed) by graft — a second `graft_*` of the
   same installed fragment is refused; `reify` produces a fresh installable head.

3. **P15 as drafted both over- and under-covers** (meta-1 over-reach: `getOpcodeKind` decodes
   ordinary int cells, so "refuse a target that `getOpcodeKind` decodes" would refuse scalar writes;
   architect-1/codebase-1/vortex-4 under-cover: the generic `step` output writeback, memo-hit
   writeback, and `runPipeline` output write at `vortex_vm.cpp:1269-1294,343-346` all funnel through
   `writeValue` unguarded). **Ruling: ACCEPTED.** Rewrite P15: the refusal predicate is
   `opcodeMap_` *membership* (the only thing that can desync), not decodability; the check guards
   the write arm only (reads of op-cell int content are `validate`'s kind check); and it must be
   sited at the write funnel — either `VortexCore::writeValue`/`splice` with a VM-installed kind
   predicate, or an enumerated list of all VM writeback sites (both `step` writebacks, both
   `runPipeline` writes, `Value`/`Splice` bodies). Fresh-cell exemption stated explicitly.

4. **`splice` has no AST channel and no hygiene mechanism** (architect-3: `infer` const-in/void-out,
   no `cloneExpr`, kernel tables key on `const Expr*`; meta-2: definition-site binding asserted but
   no mechanism; meta-3: N-site splicing needs deep copies). **Ruling: ACCEPTED.** Specify an
   `expand` stage (`Program → Program`, between parse and check) plus a `cloneExpr`/`cloneProgram`
   helper added to §10.1; definition-site binding is explicit renaming into `CompilerBindings`
   (`cap$N` capture identifiers bound to definition-site cells at fragment emission — the doc's
   "no gensym" claim is corrected: it *is* renaming, at the binding layer, which is the honest
   version). `Expr`/`Statement` deep-copy semantics stated: N splice sites mint N graphs; kernel
   tables must not see shared nodes.

5. **`def`-parameter staging contradicts itself** (writer-2, ergo-1). The same `Code`-typed `def`
   parameter is on both sides of the quasiquote/eval line. **Ruling: ACCEPTED.** Adopt the
   template model: a `def` with `Code<…>` parameters is a macro-like definition expanded at each
   call site; such defs are barred from `effect`/`native`/`capacity`/`graft_method` attachment
   points; `splice` of a `Code` parameter is legal only while its `def` is being expanded with a
   concrete fragment argument; in every other context a parameter/field/reflection result is opaque
   and composes only through `run`/`graft`. A residual `splice` that reaches lowering is a compile
   error (meta-4). Splice argument evaluation is call-by-value: arguments are bound to fresh `let`
   locals at the head of the expanded body, matching `run`'s frame discipline (ergo-4, meta-10).

6. **Graft descriptors have no arity/parameter shape** (meta-9, meta-14: `validate` cannot check
   wing arity without a descriptor). **Ruling: ACCEPTED.** Descriptors record parameter count and
   shape; the checker emits a per-fragment arity table; `graft_*` and `validate` compare against
   it. `run`'s signature becomes `run(c: Code<T>, args: [Cell]): T` (codebase-6).

### MAJOR — accepted

7. **`d.synapse` cannot express fan-in** (zigzag-3). One neighbour per dim/direction makes
   `A → B` rank edges a star, not a graph; Hebbian coupling needs N→1 junction cells.
   **Ruling: ACCEPTED.** Rewrite §8's synapse structure: junction cells on the agent's `+d.synapse`
   rank; endpoints named by qualified-name label cells (durable, V11-consistent); weights on
   state-side cells. State clearly which parts are session-only vs durable.

8. **`cursorPC_` desync under release; mark scope includes live frames** (vortex-2/-3,
   codebase-2's `arenaDims_` gap). **Ruling: ACCEPTED.** Spec containment: learned-rule cleanup
   uses the drain idiom (backtrack through choice points per `vortex_stdlib`) and *never* a bare
   `release(m0)` while sibling marks are live; `consumeWork` exhaustion is a thrown fault the
   handler must name. The §1 "all side channels" claim is qualified: `arenaDims_` is not
   mark-covered — dims are minted only with no marks outstanding (adapter genesis path), same
   discipline as opcodes.

9. **`reify` has no mechanism** (architect-4, zigzag-5, meta-7, codebase-8). **Ruling: ACCEPTED.**
   `reify` re-emits from a compile-recorded reconstruction recipe carried in the descriptor:
   cell census + `(from, dim, dir, to)` link tuples + literal contents + an old→new map pass.
   `Clone` is refused for reify because `masterOf` sharing would alias the copy's interior into the
   master's. Recipe emission is added to §10.1's fragment-emitter duties and priced in §13.

10. **`static` discipline unspecified and trips its own §4.2 hazard** (architect-2, meta-6,
    ergo-3, vortex-10). **Ruling: ACCEPTED.** Option (a): `static` evaluates with no mark
    outstanding (respecting the minted-ops invariant), then explicitly unlinks its eval cursor
    from `+d.cursors` and the eval-graph residue is priced per `static` (small, dead cells).
    Meta-6's suggested mark+release is rejected *as a mechanism* because it breaks the no-marks
    opcode invariant — its requirement (bounded, cleaned eval) is adopted via option (a).
    Closedness is rewritten: free names must resolve to literals or static-bound names; `code`
    params forbidden; arena-reading forms (`cells"…"`, `believes`, `findall`, `run`, host calls)
    forbidden at the form level; result types restricted to literal-expressible scalars
    (`Int`/`Float`/`Str`/`Bool`/`()`); `static` runs under `staticFuel` already named.

11. **Position/purity not re-verified at composition sites** (ergo-2). **Ruling: ACCEPTED.** At
    splice the substituted AST's position/purity obligations are re-checked in the site context
    (or a position-requirement mask recorded in the descriptor and checked at expansion).
    Per-graft-point requirements: `graft_where`/`graft_handler` tests are pure; bodies are
    `Code<()>`; `graft_method` matches effect-ness of the host. `code"…"` literals are pure;
    non-producing bodies are `Code<()>` (meta-11).

12. **Cross-agent received `Code`/`Cell` handles alias, not stay inert** (codebase-3, vortex-12).
    **Ruling: ACCEPTED.** V1's "inert" wording is qualified: received handles are opaque and must
    be re-resolved by `d.name` label; the §4.5 caveat extends cross-agent.

13. **`unlearn`'s prior-head link for program-original grafts re-violates V12** (vortex-7).
    **Ruling: ACCEPTED.** Patches replacing program-original attachments record no prior head;
    genesis restores the program's own attachment for free.

14. **`graft_method` redefinition is shadowed by first-match-wins** (zigzag-4).
    **Ruling: ACCEPTED.** `graft_method` unlinks any existing same-named symbol first; the doc
    states redefinition-as-replacement explicitly.

15. **`std:meta` import surface incomplete in §10.1** (architect-5, writer-1: `reflect` used
    normatively but undefined). **Ruling: ACCEPTED.** `reflect` is defined in §6.1/the glossary as
    the named family of program-structure reads (or replaced by concrete members everywhere —
    reviser picks one and is consistent); the import allowlist (`std:meta` beside `std:agent`/
    `std:goal`) and the builtin member-signature table entries are added to §10.1.

16. **`validate` opcode ceiling + impure-kind set are magic numbers** (architect-6, zigzag-9's
    seen-set mechanism, vortex-8's Return tails, vortex-9's SplitMix overflow).
    **Ruling: ACCEPTED.** The ceiling and impure-kind set are adapter-bound cells (V10
    `ensureDimension`-style binding), not prelude constants; the seen-set for `Jump`/`Call`
    traversal uses mark-links on a fresh `d.validate` dim (fresh per pass — a stale mark must not
    confuse a second pass); `validate`'s cost bound is restated honestly (per-op `maxCycles`
    budget plus O(n·membership) where a membership test is needed); `next_rand` uses a wrapping-
    safe construction (32-bit halves via `DivMod` or a modulus LCG — no `Xor`/shift opcodes exist).

17. **Focus/target convention for `Cell`-parameterized verbs** (vortex-5). **Ruling: ACCEPTED.**
    Specify the focus-slot idiom: `Value`-read the ref-box's int, `Value`-write it into a fresh
    wing slot cell linked under the structural op — existing ops only, no new proposal.

18. **`run` is not re-entrant for graphs containing bound-output `New`** (vortex-6).
    **Ruling: ACCEPTED.** `validate` for `run`-reachability refuses graphs containing bound-output
    `New` ops; the doc states the rule and the alternative (unbound-output mint or a refusal
    diagnostic at graft/run).

19. **Expansion budgets unbounded** (architect-12). **Ruling: ACCEPTED.** Name
    `CheckLimits::maxExpansionDepth`/`maxSplices`, threaded through the recursive parse (not
    per-parser).

20. **`closed` flag provenance missing** (architect-14, meta-13). **Ruling: ACCEPTED.** A
    state-side marker bit on `CompilerBinding` (adapter marks which bound cells are state cells);
    one sentence in §10.1.

21. **`d.patch` replay is monotonic; source provenance is a name** (architect-10, zigzag-6/-8).
    **Ruling: ACCEPTED.** Genesis drops unresolvable patches after recording the fault
    (compaction, not a ceiling); the patch record's source field is a name requiring an external
    source map — state that explicitly, and `learn_plan`'s patch chains both heads via `+d.step`.

22. **Lowering vehicle for action bodies unnamed** (meta-5). **Ruling: ACCEPTED.** Fragments
    lower through the same executor lowering the artifact uses; v1's shape is proven by the
    scalar kernel; examples containing `effect`s are tagged as requiring that lowering.

23. **Type identity is stringly; `to`/`source` formats unstated** (meta-12, zigzag-7, codebase-7's
    `Code` arity chain). **Ruling: ACCEPTED.** Descriptors carry qualified `module:Name` type
    strings plus the arity chain; the codegen-side canonicalization (same normal form the
    checker's `validateType` produces) is named.

### MINOR — accepted (folded into revision; not individually re-litigated)

Writer: `r10` path fix to `projects/vagent-review/r10-vortex.md`; §9.3 example retyped (declared
predicate, quoted atoms, `navigate_to` as `def`, `static` uses a literal); U-M table reorder +
U-M7/U-M8 rows; "§6.5 item 3" citation; `code` adjacency clause mirroring `CellsLit`; all table
repairs (see below). Ergo: user decls named `splice`/`static` in callable position are rejected by
the checker (one sentence); capture-liveness invariant sentence; nested-`code` example plus
`src:m:l:c+o:c` diagnostic mapping and `#`-line stripping; "code-quoted cell graph" naming note +
expression/declaration boundary sentence. Architect: `M2` price corrected (`parseExpression`
exists — new work is the `code` prefix case + `CodeParams`/`#label` header on the real `Parser`);
`runPipeline` row enumerates both writes; reflect wrapper cells get a reuse/memoize rule; `splice`
vs `#Splice` opcode terminology note; expansion copies priced. Meta: `reify`'s aliasing contract
(graft shares, `reify` isolates); nested-`splice` ordering. Zigzag: `d.entry` naming adopted;
`validate` seen-set on fresh dim (folded into §16); `static` residue priced under §10's option (a).

New deferred item: **U-M9 — `specialize` verb** (meta-8): partial application producing a smaller
`Code` — granularity boundary is clause-level over a frozen literal pool until then.

W11 (open-questions apparatus): §7.3 + §15 *are* the apparatus; the deviation from the named
open-questions convention is noted in the doc.

### Markdown damage — accepted, structural repair

Writer-4/codebase-10: every normative table was flattened by an `mdformat` run without the gfm
plugin. The required plugins (`mdformat_gfm`, `mdformat_dollarmath`, `mdformat_frontmatter`) are
not installed in this environment, so the Makefile gate cannot run; the reviser must hand-restore
all tables to GFM (header field table, §1 status, §2 glossary, §4.5 survival, §6.1 `std:meta`
surface, §10.2 register, §11 alternatives, §1 deferred rows) and verify `mdl -i` cleanliness by
inspection.

## Proposal register updates for v1.1

- **P15** — rewritten per ruling 3: `opcodeMap_`-membership predicate, write-arm only, sited at
  the write funnel or an enumerated site list, fresh-cell exemption.
- **P16 — new**: checkpoint-time relocation of fragment-owned int64 reference cells to `OpHandle`
  (publication-pass precedent) + genesis/graft rehydration back to fresh int64s. Required by
  `learn`/`d.patch`/`d.code` durability.
- **P17 — deferred, not registered**: `opcodeMap_`/`cursorPC_` eviction on `release`. The
  containment discipline (drain idiom, no bare release under live marks, no-marks opcode minting)
  keeps it unnecessary; if a future proposal needs mid-turn release of op-minting marks it must
  be raised then.

## Revision order for v1.1

P16 and the `d.entry`/head topology first (they reshape §5–§6), then the expand-stage/`cloneExpr`
and `def`-expansion semantics (§5.2/§5.6), `static` discipline (§5.5), `std:meta` surface + focus
slots (§6.1), `validate` cost and mechanisms (§7.2), synapse junction cells (§8), pricing
(§13), register (§10.2), then all minors and table repairs.

VERDICT: NOT CONVERGED — revision required; round 2 critics re-review v1.1.
