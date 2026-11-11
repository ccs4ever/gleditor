# Round 2 critique — vagent-metaprogramming.md v1.1 — Metaprogramming Expert lane

Scope: staging discipline, `expand`/`cloneExpr`/`cap$N` hygiene, typed splice semantics, `static`,
`run`, graft aliasing, P15/P16 staging-level correctness. Every load-bearing citation was checked
against the tree. R1 verdicts are per `r1-disposition.md`; unresolved residue is carried into the
findings below.

## R1 verdicts against v1.1

1. **meta-1 (P15 decodability) — RESOLVED.** M10/§6.5/§10.2 now test `opcodeMap_` *membership* at
   the `writeValue`/`splice` funnel covering all six writeback sites (`vortex_vm.cpp:771-806`,
   `:1269-1273`, `:1287-1294`, `:337`, `:343-346`), with fresh-cell exemption. Residual: the
   predicate must test `masterOf(target)` — see finding 16.
2. **meta-2 (definition-site binding) — PARTIALLY RESOLVED → supplanted by finding 1.** The
   `cap$N`+`CompilerBindings` direction was adopted (M4, §5.3, §5.6), but the carrier as specified
   cannot emit and does not deliver the claimed semantics.
3. **meta-3 (per-site deep copy) — RESOLVED.** `cloneExpr` is explicit and mandatory (M4, §5.2,
   §5.6); the `const Expr *` table hazard is correctly diagnosed (`compiler.cpp:884-891`).
4. **meta-4 (macro legality) — RESOLVED directionally → findings 7–8.** M13 draws the template
   line, residual `splice` is a compile error, CBV args adopted; remaining gaps are new findings.
5. **meta-5 (lowering vehicle) — RESOLVED.** §4.1 names the executor lowering and tags the scalar
   kernel as shape proof; §9.4 tags which examples assume it.
6. **meta-6 (static handle results) — RESOLVED.** M5/§5.5 restricts results to literal scalars,
   forbids arena-reading forms and `code` params, prices the no-mark residue (§13). Residual
   housekeeping: finding 13.
7. **meta-7 (reify mechanism) — RESOLVED directionally → finding 9.** §6.1 names the recipe
   (census + link tuples + literals + old→new map); `Clone` refused. Encoding details are a new
   MAJOR.
8. **meta-8 (specialize) — RESOLVED.** U-M9 defers it; §8.5 states the clause-granularity bound.
9. **meta-9 (graft arity) — RESOLVED.** Descriptor carries arity + ordered param cells (§5.4,
   §6.1); graft signatures include arity and purity (§6.3, §7.2).
10. **meta-10 (call-by-name splice args) — RESOLVED → see finding 8.** §5.4 binds args to fresh
    `let` locals in argument order; §9.1 shows `let p0 := count`.
11. **meta-11 (`Code<()>`) — RESOLVED.** M3/glossary: non-producing bodies are `Code<()>`.
12. **meta-12 (qualified type names) — RESOLVED.** `module:Name` + arity chain in `validateType`'s
    normal form (§6.1, §7.2).
13. **meta-13 (closed provenance) — RESOLVED.** `CompilerBinding` gains a state-side marker bit
    (§6.6, §10.1).
14. **meta-14 (wing arity) — RESOLVED.** `validate` compares per-op wing arity against the emitted
    table (§7.2).
15. **meta-15 (graft aliasing) — RESOLVED → see finding 11.** M14: "graft shares, reify isolates",
    single-installation via backlink occupancy.
16. **meta-16 (`splice` name collision) — RESOLVED.** Glossary disambiguates the keyword from
    `#Splice` (`vortex_vm.hpp:53`).

## Findings

### 1. BLOCKING — §5.3 / §5.6 / M4: the `cap$N` carrier cannot emit and does not capture what §5.3 claims

The mechanism is a synthesized `let cap$N := <name>` in the merged AST plus a `CompilerBindings`
entry `{cap$N → {cell, type}}` in the fragment record. As specified it fails three ways:

- **The kernel refuses the name.** `validName` at `compiler.cpp:407-417` admits `[a-z_]` start and
  `[a-z0-9_]` body — `$` fails, so every synthesized `let cap$0` dies at preflight
  ("scalar local names must be snake_case"). The checker's `snake()` (`checker.cpp:30-36`) only
  rejects uppercase, so `cap$0` passes checking and is refused *at lowering*. The doc's own
  "unlexable by construction" argument is what breaks it: the kernel applies the same lexical rule
  to synthesized names. No exemption or marker is specified.
- **The `let` delivers a snapshot, not a cell.** `emit` for `Let` mints a fresh local cell
  (`localCells_.push_back(temporary())`, `compiler.cpp:853-855`) and emits the initializer into it;
  `emit` for `Identifier` is `copy(output, resolved(expr))` (`:605-606`) — a `Value` op copying the
  source cell's *content*. So `cap$0` is a second cell holding `w`'s value as of block-head
  evaluation. §5.3 claims the opposite twice: "`cap$N` thereby binds the definition-site cell" and
  "a fragment that captures a parameter's binding cell captures the cell, not a snapshot".
  Observable divergence: `var count: Int; on Tick { let c := code"count+1"; count := 5; run(c,[]) }`
  — the block-head `cap$0 := count` already ran; `run` reads the stale snapshot, not the live field.
- **Head-of-block insertion precedes mid-block declarations.** "Inserted at the head of the
  innermost block enclosing the literal" — in `{ let w := 3; let c := code"w+1"; … }` the cap let
  lands *before* `let w`, so `cap$0 := w` either fails as unbound or binds an outer `w`. The
  qualifier "inside the captured scopes" cannot rescue it: a block head is before every statement
  in that block. And positions with no enclosing block (a `var` field initializer
  `var x: Code<Int> := code"y+1"`, a bare expression body) have nowhere to insert the let at all.

M4's wording deepens the ambiguity: "`cap$N` … bound in `CompilerBindings` to the *definition-site*
cell" — if that means `w`'s own cell, the run path reads the live cell while the spliced path reads
the snapshot (divergent semantics for the same fragment), and for `def`/plan parameters it is
unimplementable — frame cells are `makeCell`-fresh per `#Call` (`vortex_vm.cpp:860`) and do not
exist at fragment-emission time. The only consistent reading is that the bindings entry names the
*synthesized let's* minted cell; the doc never says so.

**Fix:** drop the AST `let` for runtime binding; make `cap$N` a pure binding-table mechanism:
(a) the checker records `{cap$N : type-of-w}` in its env at the literal's position (positioned *at*
the literal, so mid-block declarations resolve correctly); (b) `compileFragment` and the merged
emission both resolve `cap$N` via `CompilerBindings` to a single cell — for bindings with stable
cells (fields, module vars, let-locals) that is the binding's own cell (true cell-identity, live
reads); for frame parameters a refreshed static capture cell written at the literal's enclosing
statement head — and state the semantics honestly: frame captures read the value at last entry,
state captures read live. Either way the two paths must resolve to the *same* cell.

### 2. BLOCKING — §10.2 P16 / §6.6 / §4.5: the relocation pass misses the marker link and never restores the live arena

Two specification gaps, each independently fatal to `learn`'s durability claim:

- **The relocation marker is load-bearing and unnamed.** The cited precedent does *two* rewrites
  per ref cell: `setValueBits(cell, OpHandle, target)` *and* `builder.link(cell, relocationDim,
  makeCell("cell"))` (`mesh_evaluator.cpp:733-735`). `isCellRelocationHandle` requires both —
  `OpHandle` kind plus the `d.mesh.relocation`-dim edge to a `"cell"`-texted marker
  (`foreign_op_handle.cpp:109-117`). `importValue` routes on exactly this test: *marked* handles
  are rehydrated to `OpHandle → local.at(target)` (`mesh_value.cpp:494-501`); *unmarked* `OpHandle`
  cells are converted to `ForeignOpHandle` descriptors naming a store operation
  (`mesh_value.cpp:421-434`, `:463-469`). The doc's "rewrites them to `OpHandle`" produces
  unmarked handles that import as foreign-op descriptors — the round-trip never reaches the
  rehydration branch.
- **Nothing restores the live arena.** `promote` takes `const ArenaManifold&`
  (`mesh_value.cpp:168-169` calls it on the live arena), so the pre-promote pass must mutate the
  *live* fragment's ref cells. Afterwards they stay `OpHandle`-kind: `asInt64` returns `nullopt`
  (`arena_manifold.cpp:1082-1087`), `wingOf`'s operand reader falls back to `masterOf(slot)` and
  returns the slot cell itself (`vortex_core.cpp:358-365`), and every `Jump`/`Call`/`Branch`/
  `Choice` target is dead. A learned fragment survives its first checkpoint as data and is
  unexecutable in the same incarnation — the exact failure P16 exists to prevent, applied to the
  live arena instead of the image. The precedent never restores because the mesh evaluator builds
  its artifact for publication only; the fragment stays live.

**Fix:** P16's pass must (a) replicate both halves of the precedent — `OpHandle` value *plus* the
`d.mesh.relocation → "cell"` marker link; (b) after `promote` completes, restore each marked ref
cell to `Int64` (and drop or leave-inert the marker) before the next turn, or state a snapshot
discipline that does not touch the live arena; (c) genesis rehydration converts the rehydrated
`OpHandle` cells back to fresh `int64` refs — state which of the two import outcomes it consumes.

### 3. MAJOR — §5.3 / §5.2: `cap$N` numbering scope is unstated; per-literal numbering collides and mis-captures

"a fresh `cap$N` identifier" never says whether `N` is per-literal, per-block, or per-expansion.
All three failure modes exist:

- Two literals in the *same* enclosing block each synthesize `let cap$0` at its head →
  `DuplicateName` (`checker.cpp:1392-1395`, `compiler.cpp:429-431`) — a loud but unnecessary error.
- The silent case is nested: `let c := code"w+1"; let outer := code"x + splice c"`. `c`'s body
  references `cap$0` (its `w`); the outer literal's *own* expansion may also mint `cap$0` for its
  own captured `w'`. When `c`'s cloned body lands inside `outer`'s expansion, its `cap$0`
  references resolve to outer's `cap$0` — the wrong cell, silently: the exact capture failure the
  hygiene section exists to prevent.
- `cloneExpr`'d bodies carry their cap names *into* the enclosing expansion, so the merged tree's
  cap namespace and the fragment records' `{cap$N → cell}` map must be one shared, globally fresh
  namespace for the whole top-level `expand` invocation — not per literal.

**Fix:** one sentence — `cap$N` is allocated from a single counter per `expand` run, shared across
all literals and all substituted bodies; the bindings map is likewise one namespace. (Also requires
cloneExpr to preserve cap names verbatim — it does, being a structural copy.)

### 4. MAJOR — §5.6: the `expand` scope walk must replicate the checker's scoping exactly, and nothing says so

`expand` resolves free names "against its definition-site scopes" *before* the checker runs — a
parallel scope implementation. The checker's env has non-obvious rules `expand` must reproduce or
captures mis-resolve: `copyEnvironment` is taken for `for`-in bodies (`checker.cpp:1505`),
`for`-believes (`:1524`), `findall`/recovery bodies (`:1496`, `:1543`, `:1112`); the `for` binding
is a fresh scope entry typed at `depth+1` (`:1514-1517`); `as`-bindings, plan/handler parameters and
the implicit names (`self`, `parent`, `inner`, `outer`, `occurrence`) each open entries with their
own rules. If `expand`'s model diverges anywhere, a name resolves to the wrong binding (or a
different scope's same-named binding) and `cap$N` preserves the *wrong* cell — silently.

**Fix:** state that `expand`'s scope walk is defined as the checker's `env` construction replayed
for name resolution only (no types), or that `expand` runs *after* a resolution-only checker pass
that stamps each `Identifier` with its scope id — and handle the implicit names explicitly.

### 5. MAJOR — §5.3: `cap$N` renaming cannot reach `cells"…"` `$` anchors — the capture hole re-opens through the embedded language

A fragment body may contain `cells"$w/+d.vars"`. The `$w` anchor is not an `Identifier` node: it is
string data — `CellsPath::Anchor::variable` (`cell_path.hpp:23`), parsed from `expression.text` at
kernel preflight (`compiler.cpp:466-496`), resolved via `bindings_.find(path->anchor.variable)`
(`:476`). AST-level renaming never sees it. Consequences: (a) spliced path — the merged tree's
`bindings_` resolve `$w` to the *splice site's* `w` — silent capture, the failure §5.3 denies;
(b) fragment path — the fragment record's `CompilerBindings` holds `cap$0`, not `w`, so `$w` is
unbound and the fragment fails to compile. The fix exists (`$cap$0` resolves through the
`{cap$0 → cell}` entry) but requires the renamer to parse each `cells` body and rewrite anchor
names in the string — or to refuse `$` anchors inside `code"…"` bodies and say so.

**Fix:** one rule, stated: either `expand` rewrites `CellsPath` anchor names through the same
rename map (re-serializing the literal text), or `$`-anchored `cells` paths are a check error
inside `code"…"` (record the refusal in §11).

### 6. MAJOR — §5.6 / §5.2: the fragment table's key type is unstated, and `cloneExpr` breaks `Expr*`-keying

The pipeline emits "a fragment table (literal → checked-body AST + descriptor facts)". If keyed by
the literal's `const Expr *` — the natural choice, matching every kernel table — then *cloned*
`Code` literals have no entry. §5.1's own example produces them:
`code"x + splice code\"n: Int | n + 1\""` — the inner literal is inside the outer's body AST; when
the outer is spliced, `cloneExpr` copies the inner `Code` node into the merged tree; the checker's
fragment-table lookup on the clone's pointer misses. Nested `code"…"` + outer splice is legal and
advertised (§5.1 "nesting composes as levels"), so this is not an edge case.

**Fix:** key the table by `SourceLocation` (or a fragment ordinal stamped on the `Code` node by
`expand`), or specify that `cloneExpr` re-registers cloned `Code` nodes against the original
record. Note per-emission safety: each fragment emission is a fresh `Emitter` (fresh
`localCells_`/tables per compile, `compiler.cpp:96-115`, `:853-855`), so sharing the *record* is
sound; sharing the *body AST* across sites is not — the record's body must itself be re-cloned per
site (the doc says "checked-body AST" — clarify whether the table stores a template re-cloned per
emission, since two emissions of the same nodes would double-register `localReferences_`).

### 7. MAJOR — M13 / §5.2: the template model is signature-triggered, not use-triggered — recursion impossible, legal uses over-barred, diagnostics unowned

- **Recursive `Code`-param `def`s are silently impossible.** `def f(c: Code<Int>): Code<Int> {
  splice f(code"x") }` expands forever and dies at `maxExpansionDepth` — a budget error, not a
  fixpoint. Nothing states "template `def`s cannot be recursive, directly or transitively"; an
  implementer could try to support it or misdiagnose the error.
- **The rule is signature-based.** M13: "a `def` with `Code<…>` parameters is a template expanded
  at each call site". `def apply(c: Code<Int>, x: Int): Int { run(c,[x]) }` never splices `c`,
  yet is forced into template-ness: expanded per call site (wasting budget) and barred from
  `graft_method`/capacity dispatch although its runtime use is fully legal. The bar should trigger
  on *use* (the `def` splices a `Code` parameter), not on the signature.
- **Diagnostic ownership unstated.** `cloneExpr` copies `loc` fields; the same expanded body
  erroring at three call sites reports three identical locations (the `def` body site). Site-
  contextual errors — position/purity re-verification failures that only exist because of *where*
  the call lands — should name the call site; definition-context errors the `def` site. Unstated.
- **`Code`-param actuals need static knowledge transitively** — see finding 8.

**Fix:** trigger on splice-of-parameter *use*; state the recursion ban; state diagnostic ownership
(call-site loc for context checks, def-site for body errors).

### 8. MAJOR — §5.4 / §5.2: CBV arg-lets break the static-knowledge chain for `Code` arguments

CBV binds each argument to a fresh `let` local: `splice c(d)` becomes, inside `c`'s expanded body,
`let p0 := d` then `splice f` where `f` was the parameter. But §5.2's rule is "a `let`-bound
`code"…"` literal in scope" — `p0`'s initializer is `Identifier(d)`, not a literal node. Under the
literal reading the canonical composition `splice c(d)` — passing a named fragment to a
parameterized fragment — *fails*: `p0` is not statically known, `splice f` is a check error. The
propagation must be transitive: a let-bound name is statically known iff its initializer is, which
holds iff it is a literal, an identifier naming a statically-known binding, or (inside template
expansion) the substituted actual. Nested staging compounds it: `splice c(splice d)` — the inner
splice substitutes `d`'s *body* (an expression of type `T`), so the arg is an `Int`, not
`Code<Int>` — a type error that needs one sentence to be read correctly rather than mis-expanded.

**Fix:** define "statically known" recursively — `let`-bound to a statically-known initializer —
and state that `expand`'s analysis, not the checker's, performs it (expansion completes before
checking).

### 9. MAJOR — §6.1 `reify` / §4.5: the recipe's link tuples are stale at *load*, not only at resume

The recipe carries "(from, dim, dir, to) link tuples". If `from`/`to` are int64 cell ids (the
natural encoding — they mirror the runtime layout), they are compile-arena dense ids: the sealed
artifact is loaded into a runtime arena with fresh numbering, so the recipe is stale **at artifact
load**, before any checkpoint exists — `reify` replays tuples naming whatever landed at those ids.
The same staleness recurs at `resume`, and the recipe cells are not on `+d.refs` (which marks
fragment-internal ref cells), so P16 does not cover them. Two more recipe details: external
capture references must be *exempt* from the old→new rewrite (they name cells outside the copy —
replay must preserve them, not remap them, or the copy severs its closure); and "fresh closed copy"
overstates the result — `reify` of a non-closed fragment produces a non-closed copy (fine —
`learn` checks `closed` on the descriptor anyway — but say it).

**Fix:** encode recipe tuples as links (record cells `+d.values`-pointing at the `from`/`dim`/`to`
cells) so `promote`/load relocation handles them for free; state that external captures are
preserved verbatim; restate `reify` as "fresh copy, closed iff the source was".

### 10. MAJOR — §6.1/§6.2 `run`'s signature: `args: [Cell]` cannot accept the documented call

`run(c: Code<T>, args: [Cell]): T`, used as `run(c, [3, flag])` (§5.4). `[3, flag]` is `[Int]`;
`Cell` is a distinct type — "a ref-box: a scalar cell whose Int content names a live arena cell",
"a `Cell` value refers to an existing arena cell" (§4.5, glossary). There is no scalar→`Cell`
boxing form in the language — `3` is not a `Cell`, and no syntax produces a `Cell` naming a fresh
value cell (a `cells"…%{lit}"` creation returns `[Cell]` of *created* cells — the only way, and
convoluted). Under the literal signature the documented call is a type error and parameterized
`run` is unusable for value arguments.

**Fix:** either the signature is a checker-special wildcard (`args` accepts any homogeneous list;
each element is evaluated and boxed into a fresh scalar cell on the §5.8 frame — state the boxing),
or keep `[Cell]` and specify how callers obtain ref-boxes to argument cells. The first matches the
examples and the "exactly as method arguments" claim.

### 11. MAJOR — §6.1/§6.3: `reflect` wrappers are views whose descriptor is undefined — grafting them is indeterminate

`test_of`/`body_of` mint wrapper heads memoized on `d.code` over *live clause graphs* — the doc
honestly types them `Code<Cell>` and admits "no compiler-emitted descriptor". But §6.3 requires
every `graft_*` to compare "descriptor signature … including purity and arity", and §6.6 promises
"Such graphs can be `graft`ed for the session". For a reflect wrapper there is no `pure` flag, no
arity table, no `closed` flag: a conservative implementation marks it impure/unknown and refuses
*every* reflect graft (contradicting the promise); a permissive one installs unvalidated structure.
Also: grafting a wrapper shares the live graph (M14 "graft shares") — a second reader cannot
isolate it (`reify` of program-side graphs is U-M7), so a clause test grafted from `test_of`
aliases the source clause permanently — worth one sentence even though the contract is consistent.

**Fix:** define the wrapper's descriptor — `pure`/`arity`/`closed` set to unknown — and the rule
per graft point: either `validate` derives purity/arity by graph walk (state it), or grafts that
require `pure` refuse unknown-flagged wrappers (and the §6.6 promise is narrowed to body/symbol
grafts).

### 12. MINOR — §6.3 / glossary `installed`: the occupancy test is ambiguous across attach dims

The test is "the head's `-d.grab`/`-d.spin`/`-d.values` backlink is occupied" — any-of, or
matching-dim? A `graft_method`-installed head has `-d.values` occupied; if the test is per-dim it
could then pass `graft_where`'s check and install twice (two different dims). M14's intent is
single-installation anywhere — say "any install-dimension backlink occupied".

### 13. MINOR — §5.5 / §13: `static` housekeeping — post-eval VM baseline and artifact reach

`vm_.run(cursor, staticFuel)` must leave `choiceDepth()==0` and `outstandingMarks()==0` or the next
`compileStatement` refuses (`compiler.cpp:108-110`); a pure closed eval should, but the
deviation-as-compile-error rule is unstated. Also state that eval residue lives only in the compile
arena and never reaches the sealed artifact (implied, worth one clause — the compile arena is
discarded, so the residue is priced but harmless).

### 14. MINOR — §5.2: static knowledge must be judged post-substitution

`let d := splice c` where `c: Code<Code<Int>>`: after inside-out substitution `d`'s initializer is
`c`'s cloned body — a `Code` literal node — so `d` is statically known and `splice d` is legal
(level-2 composition, the §5.1 "levels" claim). This requires `expand` to re-run its
static-knowledge analysis on substituted initializers; the wording ("a let-bound `code"…"` literal
in scope") reads as pre-substitution source-tree analysis, under which `d` is not a literal.

### 15. MINOR — §6.7 `d.name` labels: cloned literals share `src:` labels; user `#label` collisions unhandled

`code_named(label)` resolves by `d.name` label. Auto labels are `src:m:l:c`; a literal inside a
template `def` expanded at N sites produces N heads sharing one label — `code_named` first-match
returns an arbitrary one. User `#label`s can likewise collide. Either state labels need not be
unique (and `code_named` is a hint), or dedupe at registration (site-suffix the auto label).

### 16. MINOR — §6.5 / §10.2 P15: the predicate must test `masterOf(cell)`

`writeValue` resolves every write through `masterOf` (`vortex_core.cpp:202` — a `Clone` shares the
master's value bits). A `Value` write aimed at a *clone of an op cell* writes the master's bits:
the operand cell is unregistered, the master is registered. P15's "the target is refused when
`opcodeMap_` contains it" must test `opcodeMap_.contains(masterOf(target))`, else cloning an op
cell opens the exact desync P15 closes.

### 17. MINOR — M6: name `run` in the allowed-inside-`code"…"` list

`run` inside a fragment is coherent (nested `#Call`, same cursor, frames stack; a self-`run` pays a
frame per level and dies at fuel — §6.2 already says so). It is the staging verb itself and deserves
naming beside `adopt`/`emit`/`believe` in M6's allow-list rather than resting on "other
non-suspending actions".

## Orchestrator's checklist, answered

- **`expand`/`cloneExpr`/`cap$N` hygienic for nested splices?** `cloneExpr` yes (fresh nodes per
  site; every kernel table keys `const Expr *`, `compiler.cpp:884-891`). `cap$N` no — findings 1,
  3, 5 (carrier semantics, numbering collisions, embedded-anchor hole).
- **`cap$N` scoped per-expansion or global?** Unstated; must be global-per-`expand`-run (finding
  3) — same-site and nested-literal collisions produce DuplicateName or silent capture otherwise.
- **Kernel-table identity invariant under cloning?** Preserved structurally; the gap is `expand`'s
  own fragment table if `Expr*`-keyed (finding 6).
- **Per-call-site expansion → repeated checking, recursion bound?** Re-checking each site is
  intended (position/purity); recursion is impossible and must be stated (finding 7).
- **Diagnostic ownership across call sites?** Unstated (finding 7).
- **CBV for `Code` arguments / nested staging?** Breaks static knowledge unless transitive
  (finding 8); `splice c(splice d)` needs one clarifying sentence.
- **`static` option (a) residue inert?** Yes — dead cells in the *compile* arena, never shipped;
  the compile VM's `opcodeMap_` registrations die with it. Housekeeping rule owed (finding 13).
- **P15+P16 sufficient, third invariant needed?** No third invariant for entry identity — head +
  `d.entry` + link relocation covers it. But P16 itself has two mechanism gaps (finding 2) and the
  recipe needs link-encoding (finding 9).
- **`run` same-cursor: first-stage vs second-stage?** Coherent — it is ESL/MetaML `run`: executes
  a stored program now; fragments containing `run` nest frames consistently (finding 17).
- **`reflect` results views or copies?** Views (memoized heads over live graphs) — consistent with
  "graft shares", but the wrapper descriptor is undefined for graft's signature check (finding 11).

## Verified-claims appendix (checked this round)

- `binding()` resolves `scopes_` before `bindings_` (`compiler.cpp:143-183`); `let` locals are one
  static cell per compile (`:853-855`); `Identifier` emits `copy` (`:605-606`).
- `validName`/`snake()` gates: `compiler.cpp:407-417`, `checker.cpp:30-36`.
- `CellsPath` anchor variable is string data: `cell_path.hpp:23`, resolved at `compiler.cpp:475-483`.
- `isCellRelocationHandle` requires kind+marker: `foreign_op_handle.cpp:109-117`; precedent at
  `mesh_evaluator.cpp:723-736`; import routing at `mesh_value.cpp:421-434`, `:463-469`, `:494-501`.
- `asInt64` nullopt on non-Int64; `wingOf` fallback to `masterOf`: `arena_manifold.cpp:1082-1087`,
  `vortex_core.cpp:358-365`; `writeValue` via `masterOf`: `vortex_core.cpp:202`.
- `promote` is `const&` on the live arena: `mesh_value.cpp:154-170`; Int64 verbatim, OpHandle
  relocated: `arena_manifold.cpp:1934-1954`; relocation-handle BFS: `:1759-1764`.

VERDICT: NOT CONVERGED
