# Round 1 critique — vagent-metaprogramming.md — Metaprogramming Expert lane

Scope: staging discipline, typing of code fragments, hygiene, phase separation, the
compile-time/runtime boundary, and the plasticity claim as metaprogramming. Every load-bearing
code citation was checked against the tree; the ones that hold are listed at the end.

## What the design gets right (verified, not findings)

- **`run : Code<T> → T` is exactly MetaML's `run`.** `T` is the fragment's *result* type, not
  the fragment's type — the correct convention. Taha's "run needs evaluation under binders only
  for open code" is handled by an honest mechanism: free names bind to *cells*, so the arena is
  the environment and no eval-under-binders is needed. The stale-read semantics that buys (a
  `run` reads a captured frame cell's *last* value, since Return only unlinks the frame,
  `vortex_vm.cpp:878-895`) is disclosed and priced (§5.3, §13).
- **The unquote inside the string is specified.** `splice` is the unquote and the body grammar
  is vagent's own grammar, so `code"x + splice c"` parses naturally — no second syntax needed.
  Nested `code"…"` literals compose as levels (`Code<Code<Int>>`), and `,name`/`${}`
  interpolation is refused for a stated reason (§11).
- **Phase discipline is clean in intent.** Stage 0 = the checker (fragment check, splice
  expansion, `static` evaluation); stage 1 = `run`/`graft`. `static` is the Zig-`comptime`
  analogue with a sound pure+closed determinism constraint (the `pureBody` gate exists,
  `checker.cpp:827`). Nothing expands at run time.
- **The `cells"…"` precedent is faithfully followed** — the lexer merges any identifier prefix
  into `PrefixedStringLiteral` (`lexer.cpp:379-398`, `token.hpp:22`), the parser narrows, the
  checker validates (`checker.cpp:1410-1428`), and `code`/`splice`/`static` as *contextual*
  keywords lex as `Identifier` (`token.cpp:9-71` confirms `succeed`/`holds` are not reserved),
  so `code"…"` merges exactly as claimed.
- The `d.patch` edit-list + genesis replay is a coherent image+EDL persistence story, and the
  V12 amendment is flagged rather than smuggled.

## Findings

### 1. BLOCKING — §10.2 P15 / §6.5 / M10: the proposed check refuses every small-valued Int cell

P15's rule is "`Value` and `Splice` … refuse a target cell that `getOpcodeKind` decodes". But
`getOpcodeKind` (vortex_vm.cpp:102-114) falls back to `asInt64`, which returns non-null for
**every `ValueKind::Int64` cell** whose content is in `[0, Linked]` (arena_manifold.cpp:1082-1087;
the doc itself states this decode rule at §4.2). Ordinary `Int` scalar cells are `Int64` cells.
Consequences:

- `count := next` in §9.1's own example fails on its first assignment, because `count` holds
  `Int64 0` and 0 ∈ [0, Linked].
- `x := v` fails whenever `x`'s current value is in range — i.e. most small-valued variables.
- `depth` cells (`makeScalarCell(0)`), `reference(target)` cells whose target's dense id is
  small, `d.vars` scalars, and `runPipeline` parameter cells all "decode" and would refuse
  writes — the blast radius is the entire language, not "deliberately-pathological writes
  only" as §10.2 claims.

It is also internally inconsistent: a runtime-minted op (`New`+`Value`) *decodes* after its
first write, so literal P15 makes it immutable — intended — while also freezing every Int
variable — fatal.

**Fix:** the refusal must test `opcodeMap_.find(c) != opcodeMap_.end()` — the private registry
is the only thing that can desync; an unregistered Int cell's content *is* its kind, so
rewriting it is consistent (and is exactly how `New`+`Value` mints runtime ops). Reword P15 to
map-membership, and restate blast radius honestly (a `Value` write to a registered op cell —
today impossible without `mintOpcode` under a mark).

### 2. BLOCKING — §5.3 / §5.2 / M4: definition-site binding cannot survive merged-AST lowering

§5.3's central claim — "a fragment spliced into a scope that happens to declare `w` still reads
the `w` of its own definition" — has no mechanism. The kernel resolves every `ExprKind::Identifier`
by **name**: `Emitter::binding()` walks `scopes_` then `bindings_` (compiler.cpp:143-183),
`Expr` carries no resolved-binding payload (ast.hpp:96-109), and the kernel *refuses* expression
metadata (compiler.cpp:208-213). When the checker substitutes the fragment AST and the merged
tree is preflighted, the fragment's `w` re-resolves in the **splice site's** scopes — capture,
the exact failure §5.3.1 denies. Worse, for `splice` of a `code` parameter across `def`s the
captured cell is not even in the callee's `bindings_`, so resolution silently captures or fails
unbound. The refusal of "Scheme-style rename hygiene" (§11) is premature: the model has the
problem at the name-resolution layer even though cells are the values.

**Fix — pick one and specify it:**

- (a) The checker renames each captured free name to a fresh `cap$N` identifier and adds
  `{cap$N → definition-site cell}` to the merged statement's `CompilerBindings`. This *is*
  explicit renaming — retract the "no gensym needed" framing; it is renaming at the binding
  layer, and it is the cheapest correct mechanism.
- (b) Add a prebound-resolution channel (an `Expr` field or a checker→kernel
  `const Expr* → CellRef` map) that `Emitter::binding` consults before `scopes_`/`bindings_`.

Either way, the fragment's free names must arrive at the kernel *already resolved*, because the
kernel cannot reconstruct definition-site scope from a merged tree.

### 3. MAJOR — §5.2: splice must deep-copy the fragment AST per site; the copy requirement is never stated

Every kernel side table is keyed on `const Expr*`: `localReferences_`, `externalReferences_`,
`types_`, `literals_`, `paths_`, `discarded_` (compiler.cpp:884-891), with `localCells_` indexed
by `localCount_` (compiler.cpp:432-434, 853-855). If substitution grafts the *same* checked
subtree at N sites, every site's `let` locals share one index — one mutable cell serving all N
expansions; mutation at one site is visible at the others. "N sites mint N graphs" (§5.2)
asserts the outcome but never names the mechanism that delivers it, and an implementer who
grafts rather than clones breaks locals silently. This is the cells-world analogue of
fresh-renaming: node identity, not spelling, is what carries resolution — which is precisely
why sharing is unsound.

**Fix:** state explicitly that splice substitution *clones* the fragment AST (fresh `Expr`
nodes per site), so each site's locals and temporaries mint fresh cells. Alpha-renaming stays
unnecessary — but only because copy-per-site is mandatory.

### 4. MAJOR — §5.2 / §5.4: this is a typed macro system with two unstated legality rules — price the ceiling and state the rules

The static-knowledge rule makes the design a compile-time macro/template layer plus runtime
code values — *not* full staging (MetaML/TH let `~x` splice any code-valued name in scope,
including parameters; here only lexically-obvious fragments splice). That is a defensible
choice, but two rules it forces are nowhere in the doc:

- **A `def` that splices a `Code` parameter is expansion-time-only.** `def combine(a: Code<Int>,
  b: Code<Int>): Code<Int> { code"splice a + splice b" }` works only if `combine` is inlined
  at each call site with statically-known actuals. The doc never says such a `def` cannot be
  called dynamically — `run`-dispatched, `graft_method`-installed, capacity-dispatched, or
  invoked with an opaque `Code` argument — nor what `validate`/`emit` does when a `Splice` node
  survives into a graph. State it: a `def` whose body contains `splice` of a parameter is
  expanded at check, and any residual `splice` reaching lowering is a compile error.
- **Are a fragment's own `Code<T>` parameters "statically known" inside `splice f`?**
  `code"f: Code<Int> | splice f + 1"` can splice-resolve when instantiated by `splice`
  (the argument is an AST naming a known fragment) but never by `run` (the argument is a value).
  Fragments containing param-splices need a "splice-only" marking — legal to `splice`, refused
  by `run`/`graft` — or `CodeParams` must exclude `Code` types. Pick one.

Without these, an implementer cannot decide the legality of the one idiom that makes the system
macro-useful (codegen combinators).

### 5. MAJOR — §4.1 / M1 vs §5.1 / M6 / §9: the cited substrate refuses every action the doc allows inside fragments

§4.1 grounds the fragment on `compileStatement`, whose preflight supports only
{Integer, Float, String, Boolean, Unit, Identifier, Fail, Unary, Binary, Block, Let, If, Cells}
— everything else hits `default: "construct is not supported by the scalar lowering iteration"`
(compiler.cpp:500-503). No `Call`, `Believes`, `Emit`, `Adopt`, `Install`. Yet M6 allows
`adopt`, `emit`, `believe`, `install`, `graft` inside `code"…"`, and §9.2/§9.3 quote
`code"believes sees(food, water)"` and `code"{ navigate_to(water) }"`. The scalar kernel proves
the *shape*; it cannot emit a single worked example. The doc says implementation is in progress,
but never says fragment bodies are bounded by whatever the artifact build lowers — nor whether
v1 fragments are deliberately scalar-only.

**Fix:** one sentence in §4.1/§5.1 naming the lowering vehicle (the owed executor lowering) and
admitting the scalar kernel is the shape proof only — or restrict v1 `code"…"` to the scalar
subset explicitly and mark the action examples as forward-looking.

### 6. MAJOR — §5.5 `static`: handle-typed results cross the compile→artifact arena boundary

`static e` emits the result cell's `CellValue` as a literal. For `e: Code<T>` or `Cell` the
emitted `Int64` names a cell in the **compile arena** (fresh per compile, §5.5) — a meaningless
handle baked into the sealed artifact, contradicting §4.5's own stale-handle accounting.
`static (code"1")` is pure and closed under the stated rules and produces exactly this garbage.
Separately, `vm_.run(cursor, staticFuel)` mints cursors (`spawnCursor` registers on
`+d.cursors`, vortex_vm.cpp:148-161) and temporaries in the compile arena — whether those work
cells are released before sealing is unstated.

**Fix:** `static`'s result type must be a non-handle scalar (`Int`, `Float`, `Str`, `Bool`,
`()`) — refuse `Code`/`Cell` results; and specify that the `static` run executes under a mark
released after the result literal is minted.

### 7. MAJOR — §6.1 `reify`: the central copy verb has no implementable mechanism

`reify(c)` is "a fresh closed copy of a literal-born fragment (compile-assisted)". But
`Linked` walks a single dimension and `dimensionsOf` is C++-only — U-M7 admits this for
*foreign* graphs, yet literal-born fragments have the identical enumeration problem: nothing in
Vortex can list a graph's edges. "Compile-assisted" must therefore mean the compiler emits a
construction recipe with the fragment — an ordered cell index plus (from, dim, dir, to) link
tuples on the head — that `reify` replays through `New`/`Link`/`Value`. That recipe is never
specified, and `learn`'s copy semantics depend on `reify`.

**Fix:** specify the recipe (it also fixes the `closed`-copy definition), or defer `reify` with
U-M7 and restrict `learn` to as-installed `code"…"` fragments.

### 8. MAJOR — §8.5: the plasticity loop has no way to *perturb* a fragment — the pool is compile-frozen

"mutate(candidate): e.g. graft_where with a perturbed test" — the perturbed test must be a
`code"…"`-born fragment; the pool is fixed when the artifact is sealed. The agent can recombine
clauses from literal tests and bodies (`graft_plan` composes test×body — real clause-granularity
composition), but cannot produce even a parameterized variation: `run(c, args)` executes but
yields a value, not a new fragment; `reify` copies but does not instantiate. There is no
`specialize(c, v…): Code<T>` (bind a param'd fragment's parameters to values → new closed
fragment). Without it, "searching over `code"…"` fragment pools" degenerates to enumerating a
finite literal set; within-clause novelty is raw `cells`+`Value` surgery (§11's unguarded door)
or U-M1.

**Fix:** add `specialize` (a compile-assisted verb: descriptor-declared params → closed copy
with param cells pre-bound to the argument values — the same recipe as `reify` plus bound
literals), or state the granularity boundary as crisply as U-M3/U-M4 are stated. As a
metaprogramming judgment the rest of §8 is honest — learned structure *is* executed code, not
interpreted data — but the typed generativity boundary (clause granularity over a frozen pool)
should be named, since "learn rules" oversells it without `specialize`.

### 9. MAJOR — §6.3: graft-point signatures never mention arity

`Code<T>` erases the parameter signature into the runtime descriptor; `graft_*`'s "expected
signature" is stated as result types only ("plan test: any producing type; plan/handler body:
`()`"). A parameterized fragment — `code"x: Int | x > 0"` — grafted as a clause test cannot be
invoked by plan selection (nobody supplies `x`), and the stated checks all pass. Unspecified
failure at selection time.

**Fix:** graft-point signatures must include the parameter shape — nullary for clause
test/body (or the clause's standard formals, per the spec's §5.7 plan-binding convention) —
checked against the descriptor at graft. Consider `Code<(A,B) → R>` or at minimum a param count
in the descriptor contract; today arity mismatches surface only as `run` failures.

### 10. MINOR — §5.4: parameter substitution is call-by-name AST inlining

"the checker binds parameter names to the argument ASTs" — `splice c(impure())` duplicates the
argument at every parameter use: N uses, N evaluations, N effects — the classic
don't-eval-twice hazard, plus N× code growth. Suggest implicit `let`-binding of splice
arguments (call-by-value), or document the call-by-name rule where the hazard lives.

### 11. MINOR — §5.2: no `Code` spelling for a statically non-producing body

`CompiledStatement.resultType` is `std::optional` — `nullopt` means statically non-producing
(compiler.hpp:32-33). `code"fail"` or `code"{ fail; 1 }"` therefore has no `T`; `Code<T>` has
no bottom case. Rule it: `Code<()>` for non-producing bodies, or refuse to quote them.

### 12. MINOR — §6.1: descriptor type names are nominal and unqualified

Signature comparison compares type-name cells — two different `Point` records in different
modules compare equal, wrongly accepting a graft. Store qualified (`module:Name`) names, the
same convention `plan_named` uses.

### 13. MINOR — §6.1 / glossary: the `closed` flag's provenance is undefined

"The flag is set at emission" — but "state-side" is an adapter/runtime property, and
`CompilerBinding` is `{cell, type}` with no provenance (compiler.hpp:15-18). At emission time
nothing distinguishes a program-cell capture from a state-cell capture. Specify: a provenance
bit on `CompilerBinding`, or a runtime reachability test at `learn` instead of a stored flag.

### 14. MINOR — §7.2 `validate`: the well-formedness walk cannot check per-op wing arity

`validate` verifies kind-range, chain termination, jump-target legality and purity flags — but
input/output wing counts per opcode exist only in the C++ dispatch (`executeOpcodeBody`). A
malformed fragment (an `Add` with one wing) passes `validate` and mis-steps. Either emit a
per-fragment shape record the checker computes (it knows arities) and `validate` compares, or
admit the gap in §7.3.

### 15. MINOR — §6.3: `graft` installs by reference; the aliasing contract is unstated

The same fragment graph serves the grafted clause *and* remains reachable through the `Code`
value; a double graft shares one graph between two owners; later mutation through either
handle affects all users. State the rule: `graft_*` shares, `reify` then `graft` isolates —
one sentence.

### 16. MINOR — naming: `splice` collides with `OpcodeKind::Splice`

The keyword `splice` and the content-splice opcode (`vortex_vm.cpp:792`, `core_.splice`) share
a name, and this very document uses both senses (P15's `Splice` vs §5's `splice`). Rename one
(`unquote`, `ins`, `inline`) or flag the collision.

VERDICT: NOT CONVERGED
