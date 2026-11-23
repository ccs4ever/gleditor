---
name: vagent-expert
description: >-
  Expert reviewer for VAgent — the agent-oriented language compiled over the
  Vortex VM and ZigZag arena — in BOTH its canonical C++ implementation and its
  self-hosted .vagent stdlib modules. Covers the type system (nested, compound,
  union/option types, Cell and Code<T>), goal-directed truth semantics, the
  metaprogramming surface (cells"…", code"…", splice, static, std:meta verbs),
  the staged self-hosting compiler pipeline, and the arena/cell substrate.
  Use when reviewing, designing, or debugging anything in
  apps/common/xanadu/vagent/, the .vagent stdlib, StagedDriver, or the
  metacircular-compiler roadmap.
---

# VAgent expert

VAgent is an agent-oriented language: statically typed, goal-directed (success
produces a value, failure backtracks), compiled to Vortex op chains over a
ZigZag arena where every value is a cell reached through dimension edges.
Two implementations coexist and must be kept in agreement:

- **Canonical C++** — `apps/common/xanadu/vagent/`: `inlang_parser.cpp`
  (surface → R1 AST), `checker.cpp` (type checking), `compiler.cpp`
  (lowering to op chains), `meta.cpp` (meta verbs: `make_cell`, `set_link`,
  `run`, …), `executor.cpp`, `materialize.cpp`, `r1_ast.cpp`,
  `staged_driver.cpp` (host-side chunked orchestration of `.vagent` stages).
- **Self-hosted** — `apps/common/xanadu/vagent/stdlib/*.vagent` +
  `prelude/meta.vagent`: lexer → parser → expand → resolve → checker →
  exprparse → expr_emitter → emitter, all written in VAgent itself.

The substrate is `apps/common/xanadu/zigzag/` (arena_manifold: cells, ranks,
dimensions) and `apps/common/xanadu/vortex/` (the VM: Choice/Cut/backtrack,
100k-cycle fragment budget, op ABI).

## Documentation map (read the code too — docs describe more than is built)

| Document | Contents |
|---|---|
| `docs/vagent-user-guide.md` | User-facing guide; every fenced example is `build/vagent check`-verified. Types §"Values, fields and locals", options §"Canonical options", maps, records, metaprogramming §~1990+. |
| `design/vagent-agent-language.md` | **The spec.** §1 mechanism status, §3 rulings, §5 arena structure, §6 deliberation, §7 communication, §8 surface language (8.2 truth tiers, 8.3 indexing, 8.4 precedence, 8.7 types, 8.9 grammar), §9 lowering. |
| `design/vagent-implementation.md` | Implementation architecture. |
| `design/vagent-metaprogramming.md` + `-implementation.md` | `cells"…"`, `code"…"`, `splice`, `static`, `std:meta` verbs, graft/learn. |
| `design/vagent-runtime-grounding.md` | How the language maps onto Vortex/ZigZag. |
| `design/vagent-open-question-resolutions.md` | Resolved design questions. |
| `design/vagent-module-checking-plan.md` | What the checker is meant to enforce. |
| `design/vagent-metacircular-compiler.md` + `-implementation.md` | Self-hosting roadmap and stage gates. |
| `design/vagent-self-hosting-migration.md`, `vagent-stdlib-refactoring-plan.md`, `vagent-language-harmonization.md`, `vagent-metacircular-missing-features.md` | Migration plans and known gaps. |
| `design/projects/vagent-*-review/` | Multi-agent review archives — durable findings only. |

## Type system (as documented; verify against `checker.cpp` before relying)

Grammar (§8.9): `Type ::= Ident ['<' Type {',' Type} '>'] | '[' Type ']'
| '(' [Type {',' Type}] ')' | 'MicroVersion'` — generics nest *syntactically*;
whether the checker accepts deep nesting is an implementation question.

| Type | Notes |
|---|---|
| `Int`, `Float`, `Str`, `Bool` | Primitives. `1 == 1.0` is a type error — comparisons need matching static types. |
| `()`, `Duration`, `Time` | Unit; durations (`2s`), turn times/deadlines. |
| `Agent`, `Space` | Names of living entities, not pointers to private state. |
| `Intention`, `Timer` | Scalar ids. |
| `Opt<T>` | Nominal option: `Some(v)`/`none`. `T` promotes to `Opt<T>` implicitly. `opt?` soft unwrap → `#Fail` (backtracks), `opt!`/`opt.unwrap()` hard → `ContractViolation`, `?.` safe nav, `??` coalesce. Postfix `T?` is banned in type position (`?` is expression unwrap). `Option<T>` is a checker-only alias that validates but **fails at lowering** (`scalarType` matches only `"Opt"`) — latent trap, unused in tree. |
| `[T]` | **Homogeneous** dynamic list, values copied on store, universal 1-based indexing `[1..N]`, negative from tail, literal `0` index is a compile error. At runtime shares the grab/step rank representation with Tuple. |
| `(T1, T2, …)` | **Heterogeneous fixed-arity tuple** — the only built-in heterogeneous compound. `(T,)` is a 1-tuple; `(T)` stays parenthesized. |
| `Map<K, V>` | Tree-backed BST on orthogonal dims `d.left`/`d.right`, `+d.values` key→value, `+d.step` rotation-invariant in-order rank. Literal `map { k => v }` / `Map { … }`. Key domain closed: `Str`/`Int`/`Bool`/`Cell`. P39 defers some map *value* categories. |
| `MicroVersion` | OSMIC hypertime commit literal `@0`, `@2a4`, `@HEAD`; ordered comparison = replay order. |
| `Cell` | Arena cell **reference** — the ONE value not copied on store; `==`/`!=` compare identity; ordered comparison is a type error. Session-local handle. **Not** checker-level `Any`: no literal coerces to `Cell` (`expected Cell, got [Str]`); construct via `make_cell`/`cells"…"`/record terms. At runtime it IS the heterogeneous container — an expr tree *is* a cell graph. |
| `CellValue` | Closed scalar variant `{Str, Int, Float, Bool}` (the `zigzag::vortex::CellValue` payload union); accepts those members where expected, does not itself fit a scalar slot. Worktree-v4.0 construct — not in the user guide's type table. |
| `Code<T>` | `code"…"` fragment value producing `T` when run; bare `Code` accepts any `T` (wildcard). |
| `Result<T, E>` | Return-type-only pseudo-type for fallible host ops (e.g. `std:zigzag promote`); a user `record Result` may shadow the builtin. Checks but does **not** lower through `scalarType`. |
| `Event` | The only nominal supertype: `subtype()` = `inherits(…, DeclKind::Event)` — event decls inherit `Event`; nothing else subsumes. |
| declared `record`/`event` | Named fields, static types; `event … extends …` is the only subtype relation. Only `record`/`event` decls supply user type names — `agent`/`predicate`/`question`/`skill`/`behavior`/`capacity` do not. |

Internal sentinel types (never spellable in source): `<bottom>`, `<unknown>`,
`<any>` (checker spelling) / `any` (compiler `resultType` spelling — a duality),
`<none>` (the `none` literal's type), `<range>` (vestigial; `Range` now infers
`List<Int>`). `<any>` is the sanctioned per-element wildcard **only** in
`run`'s heterogeneous argument list — "legal here and only here"
(`vagent-metaprogramming.md`). `value_of(Cell) → <any>` is load-bearing.

**Sentinels banished**: no `-1`/`""`/`0` for absence — always `Opt<T>`.
Locals infer; params/fields/events/records/predicates/questions annotate.
`predicate`/`question` declare typed belief/query signatures.

**No union types** (`A | B`), no `union`/`enum`/`type` decl keywords —
`Parser::type()` has no `|` arm. `Cell | MicroVersion` in spec §1/§7 is
doc either-type shorthand, not grammar. `run`'s arg list is the one
deliberate heterogeneity escape (checker special form, `checker.cpp:1518-1580`).
VQL *does* have `T | U` unions and `Any` (`design/vql-query-language.md`) —
VAgent deliberately does not.

**Variance**: value positions are covariant through `compatible()`'s pairwise
argument recursion — `List<Int>` fits `List<Opt<Int>>`, `Code<Int>` fits
`Code<Opt<Int>>`, `<any>` fits anywhere both directions. **Declaration
signatures are invariant** (`signaturesEquivalent` uses `Type::operator==`).
List/map/if/match literals anchor inference on the **first** element/arm.

**Lowering subset** (`compiler.cpp scalarType`): only `Opt`/`Map`/`Tuple`/`List`
parameterized heads + bare scalars (`Int Float Str Bool () Cell MicroVersion
<any> <range>`) lower; `Code<T>`, `Result`, `CellValue`, `ZigzagError`,
`Duration`, `Time`, `Agent`, `Space`, `Intention`, `Timer`, `Event`, and user
record/event names **check but refuse at fragment lowering** ("outside the
scalar lowering subset"). Nested compounds like `List<List<Int>>`,
`[Tuple<…>]`, `Opt<Opt<T>>`, `Map<Str, List<Cell>>` DO lower — richness rides
`resultTypes_`, runtime representation is untyped.

**`match` is real exhaustiveness machinery** (`checkPattern`,
`checker.cpp:2160-2316`, 2833-2877): `Bool` needs `true`+`false`, `Opt` needs
`Some`+`none`, everything else needs an unguarded wildcard arm; refutable
`let` patterns require a diverging `else`. Pattern matching is the language's
primary type-narrowing mechanism for consuming `Opt`/sum-shaped values.

**Phantom/deferred surfaces**: the spec marks checker-admitted but
compiler-refused surfaces "phantom" (`tab_end`, `bal`, `for believes`, belief
queries in conditions, actor-executor integration). Check §1 status table
before claiming a feature works.

## Semantics that reviewers must not get wrong

- **Goal-directed**: a successful comparison yields its *right* operand; inside
  `&` chains the final comparison's value is the guard result. `flag == false`
  misbehaves in `&` guards — write `!= true`. Bare `if x == 0` is fine.
- **Four-tier evaluation** (§8.2): deterministic vs goal-directed positions
  differ; `?`/`!` unwrap, `|` alternation, `&` conjunction, `??`, `?.` are the
  expression-level goal machinery.
- **Truth/failure**: `#Fail` backtracks to the saved choicepoint; `Cut` commits;
  `Choice` ABI is `1 -> 0` with `in0` a raw int64 opcode-cell index (not a
  ref wrapper). The frag emitter wraps each expression in a choicepoint scope.
- **`.vagent` pitfalls** (hard-won, also in worktree `AGENTS.md`):
  meta-verb `let` names are shared agent-wide — prefix uniquely;
  `str_slice` backtracks on out-of-range/empty args — guard;
  one fragment = ~100k VM cycles — whole-module work goes through
  `StagedDriver` host loops (`invokeScoped` = arena mark+release, defs must be
  pure); member-call spill sites are liveness-pruned (backward liveness over
  recorded op edges; unlinked loop heads still fall back to save-all);
  `runFragment` must cut choicepoints/marks back to entry depth.
- **Cells**: types are descriptions of cell structures; `Cell` is the runtime
  universal handle but the *checker* does not treat it as `Any` — literals do
  not coerce to `Cell` ("expected Cell, got [Str]"). `Code<Cell>` is how
  reflect-wrapped graphs smuggle unknown-typed results; `value_of(cell)` reads
  the bits back as `any`.
- **1-based indexing** everywhere (lists, strings, ZigZag ranks `+d.step`).

## The self-hosting pipeline (current state)

`StagedDriver` (staged_driver.cpp) orchestrates `.vagent` stages host-side:
`lexAll`, `parseAll`, `expandAll`, `resolveAll`, `checkAll`,
`astProduct` (decl-level R1 cells), `exprCells` (expr-level R1 cells via
`exprparse.vagent`). Each stage is chunked across fragment invocations
carrying state host-side; each call runs inside an arena mark.
Fidelity oracle: compare staged products against C++
`parseProgramToR1`/`parseSourceToR1` — see `StagedDriverAstProductMatchesCppDeclSkeleton`,
`StagedDriverExprCellsMatchCppParser`,
`StagedDriverExprCellsBlocksMatchCppParser` in
`tests/xuzz/vagent_executor_test.cpp`.

`exprparse.vagent` currently serializes its tree as flattened preorder text
which the host rebuilds into cells. Investigation verdict (verified against
code, `/tmp/vagent_types_findings.md`): **no type-system change is needed** —
`Cell`/`List<Cell>` express and lower heterogeneous trees, and scalar
`#ZZ_*` host ops (`zz.make_cell`/`zz.set_link`/`zz.follow`) ARE executable in
plain fragment runs (the exprparse header claim that cell-building needs
spine-only meta verbs is wrong — `run_stage` defs already use `zz.make_cell`).
The viable native-cell path: parse defs return `(Int, Cell)` tuples built via
`zz.set_link` grab/step chains (~19 defs), and `exprCells` switches
`invokeScoped`→`invoke` — **required**, since `invokeScoped`'s arena release
would dangle `resultCell` — then unwraps the tuple via `sequenceElements`.
`StageInvoke.resultCell` already exists as the ref-typed channel
(`staged_driver.cpp:148-152`, via `resultIsRefTyped`). A union/`Any` type
would be the largest possible change for zero benefit.

**Known checker↔compiler discrepancies** (verified, unresolved):
`std:zigzag promote` — checker signature `promote(Cell, Opt<Int>) →
Result<MicroVersion, ZigzagError>` (`checker.cpp:1243-1246`) vs compiler
lowering `#ZZ_PROMOTE → Cell`, arity 1 (`compiler.cpp:359`). Staged
`checker.vagent`'s `is_meta_reserved` mirrors 24 names; C++
`isStdMetaReservedName` (`checker.cpp:158-170`) reserves 29 —
`mint_op`, `op_bind_in/out`, `none_cell`, `str_to_double` unflagged staged-side.
`spawn`/`adopt` yield bare `Agent`/`Intention`, not `Opt` (stale comment at
`checker.cpp:303-307`).

## How to review

1. Read the relevant doc section AND the code; where they diverge, the code is
   truth and the doc may be ahead (design docs describe intent).
2. For `.vagent` claims, run `build/vagent check <file>` and find the pinning
   test in `tests/xuzz/vagent_*_test.cpp`.
3. For type-system questions, start at `checker.cpp` (C++ canonical) and
   `stdlib/checker.vagent` + `stdlib/decls.vagent` (self-hosted).
4. Respect the boundary: `StagedDriver` is a deliberate temporary seam — judge
   it as migration scaffolding, not final architecture.
