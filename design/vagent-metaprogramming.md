# vagent metaprogramming: code as cells, self-modifying agents and the plasticity path

| field         | value                                                                                                                                                                                                                                   |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 1.4 (see [Appendix](#appendix-versioning-and-change-history))                                                                                                                                                                           |
| status        | Design draft. Nothing here is implemented. It rests on vagent v3.9, whose own implementation is in progress: the source frontend and the scalar lowering kernel exist; the actor executor does not.                                     |
| compiles to   | Vortex opcode graphs held in the agent's private `ArenaManifold`; a fragment is the same object `compileStatement` already produces (`vagent/compiler.hpp:28-34`)                                                                       |
| rests on      | vagent ([specification](vagent-agent-language.md)), Vortex ([runtime](vortex-hyperstructural-runtime.md)), `ArenaManifold`'s mark/trail/discard, the `cells"…"` embedded-VQL subset, the Spool-Mesh checkpoint/promotion machinery      |
| prerequisites | vagent §8.7 (`cells"…"`, `Cell`); the call/frame convention of §5.8 and §11.1 (owed by vagent itself); Vlog §6 clause layout for belief fragments; P1–P14 already approved there                                                        |
| changes       | new code (the `code"…"` literal, `splice`, `static`, an `expand` stage, a fragment-aware lowering pass, the `std:meta` prelude, adapter bindings) and **two** proposals, **P15** and **P16**, not approved — P15 is the one change to existing code; P16 needs no core change (§10.2) |

Compiled vagent is a Vortex opcode graph, and a Vortex opcode graph is arena cells: `mintOpcode`
writes `int64(op)` as cell content (`vortex/vortex_vm.cpp:73-82`), `bindInput`/`bindOutput` hang the
wings, `+d.spin` chains the instruction stream, and `+d.grab`/`+d.spin` hold a clause's test and
body exactly as Vlog lays out clauses. There is no bytecode blob, no AST database, no second
representation of the program. This document takes that fact seriously and asks the two questions it
raises.

First, the compile-time question: how does a programmer write programs that write vagent, in a
statically typed language with no closures? The answer given here is a typed quasiquote — a
`code"…"` literal parsed by the real parser behind a string prefix (the `cells"…"` precedent), a
`Code<T>` type for typed code fragments, and a `splice` form that composes fragments in a dedicated
`expand` stage between parse and check. Second, the runtime question: since the program is cells and
the structural opcodes (`Link`, `Break`, `New`, `Value`, `Splice`, `Clone`, `Linked`) already mutate
cells under the trail, how does an agent safely modify its own program? The answer is `std:meta`, a
Vortex package of reflection and grafting routines over the existing opcodes — plus two honest gaps,
P15 and P16: the VM's opcode-kind cache can diverge from cell content (needing a core refusal), and
a checkpoint would silently corrupt a fragment's int-encoded internal references (needing an adapter
ceremony — `std:meta` cannot write `OpHandle` and the mesh layer must not learn vagent's dims).

The longer-term motivation is stated plainly: **plasticity**. An agent whose behaviour structure —
plan bodies, clause ranks, handler sets, link topology — is mutable in the same way its beliefs are
mutable is an agent that can learn rules, not just weights. Weights are scalar cells; synapses are
ranks; growing and pruning them is the same rank surgery as everything else. This document prices
the machinery for that path honestly and refuses the shortcuts (a siloed AST format, textual `eval`,
unchecked self-modification) that would make it cheap in the wrong way.

Three positions argue through the rulings, and the text says who wins each. The Nelsonian purist
wants code as first-class cells in the docuverse: transclusion and provenance of fragments, self-
modification as ordinary arena mutation, no siloed syntax formats. The systems realist wants work
budgets, trail/checkpoint honesty about mutated graphs, defined behaviour when code mutates under
execution, and reproducible artifacts. The codebase grounding limits both to what exists: the scalar
kernel is the only implemented compiler, opcode integers are persisted cell content, `cells"…"` and
`Linked` are the navigation/mutation substrate, and `Choice`/`Cut`/`ChoiceDepth` are the search
discipline. §3 records the wins.

______________________________________________________________________

## Contents

- [1. Status of each mechanism](#1-status-of-each-mechanism)
- [2. Glossary](#2-glossary)
- [3. Rulings](#3-rulings)
- [4. The substrate: code is already cells](#4-the-substrate-code-is-already-cells)
- [5. Compile-time metaprogramming](#5-compile-time-metaprogramming)
- [6. Runtime mutation](#6-runtime-mutation)
- [7. Type safety, honestly priced](#7-type-safety-honestly-priced)
- [8. The neuro-network plasticity path](#8-the-neuro-network-plasticity-path)
- [9. Worked examples](#9-worked-examples)
- [10. What the C++ core has to gain](#10-what-the-c-core-has-to-gain)
- [11. Refused alternatives](#11-refused-alternatives)
- [12. Properties, with their conditions](#12-properties-with-their-conditions)
- [13. Breakages and costs](#13-breakages-and-costs)
- [14. Reconciliation with the convergence](#14-reconciliation-with-the-convergence)
- [15. Deferred work](#15-deferred-work)
- [Appendix: Versioning and Change History](#appendix-versioning-and-change-history)

______________________________________________________________________

## 1. Status of each mechanism

"Built" means present in the tree at the cited place. "Owed" means this document asks for it and §10
prices it. "Deferred" means outside the initial design; §15 gives the direction. Paths are under
`apps/common/xanadu/` unless stated. The vagent implementation state this table inherits is in
[vagent-implementation.md](vagent-implementation.md): the frontend and scalar kernel exist; the call
ABI, actor turns and `std:agent` are owed there and so are owed here.

| Concept                                  | Mechanism                                                                                                                                                                                                                                                                                           | State                                                    |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------- |
| Code as cells                            | Opcode graphs in the arena (`mintOpcode`, `vortex_vm.cpp:73-82`); a `CompiledStatement` is `{entry,result,produced}` cells                                                                                                                                                                          | built                                                    |
| Arena navigation                         | `cells"…"` subset: `##`/literal-id/`$` anchors, signed dim steps, `%` creations, one filter (`vagent/cell_path.cpp:147-183`)                                                                                                                                                                        | built                                                    |
| `Cell` handle                            | ref-box: scalar cell whose `Int` names a live arena cell; `==`/`!=` identity (`vagent-agent-language.md` §5.9, §8.7)                                                                                                                                                                                | built                                                    |
| Rank traversal                           | `#Linked` op, ref-box materialization, `consumeWork`-bounded, seen-set cycle guard (`vortex_vm.cpp:654-723`)                                                                                                                                                                                        | built                                                    |
| Structural mutation ops                  | `Link`/`Break`/`New`/`Value`/`Splice`/`Clone` (`vortex_vm.cpp:616-836`)                                                                                                                                                                                                                             | built                                                    |
| Transactional mutation                   | mark/release/discard trail; cells older than the innermost mark are copied out per write (`arena_manifold.cpp:1506-1570`, release truncation `:1612-1700`) — *arena state only*; `opcodeMap_`, `cursorPC_`, `arenaDims_` and `memoizedOps_` are VM-side and not mark-covered (`vortex_vm.hpp:186-188`, `arena_manifold.hpp:170-186`) | built                                                    |
| Compile-time fragment                    | `code"…"` literal + `Code<T>` type + `splice` expansion through an `expand` stage                                                                                                                                                                                                                   | **owed** (front end, §5, §10.1)                          |
| Staged evaluation                        | `static e`: pure+closed, lowered and run in the compile arena with no mark outstanding, bounded                                                                                                                                                                                                     | **owed** (kernel, §5.5)                                  |
| Runtime fragment call                    | `std:meta::run(c, args)`: checker special form — a list literal, per-element `any` wildcard, values bound on the `#Call` frame by parameter name (§5.8)                                                                                                                                               | **owed** (`std:meta`, call ABI is vagent's own debt)     |
| Reflection                               | `std:meta`'s `reflect` family of read functions over program structure; program root bound by the adapter like the thirteen dimensions (V10)                                                                                                                                                        | **owed**                                                 |
| Grafting                                 | `graft_*`/`prune` verbs attaching fragment heads at `+d.grab`/`+d.spin`/`d.clause`/symbol points; head→entry on `d.entry`                                                                                                                                                                           | **owed** (`std:meta` over existing ops)                  |
| Durable mutation                         | `learn_*` verbs + `d.patch` rank + genesis re-application — **conditional on P16** (§6.6)                                                                                                                                                                                                           | **owed** (`std:meta` + `std:agent` genesis hook)         |
| Session code identity                    | `d.code` registry (learn-side only — the §6.6 reachability invariant) + `d.name` labels; `code_named` re-resolution                                                                                                                                                                                 | **owed**                                                 |
| Kind immutability                        | every VM write to an `opcodeMap_`-registered cell refused — P15                                                                                                                                                                                                                                     | **proposed**                                             |
| Fragment reference relocation            | adapter-side ceremony around `publishValue`, run inside the adapter's own `Executor` callback: end-of-turn `OpHandle` re-kind + head-of-turn `Int64` restore + genesis rehydration; emission-side `+d.refs`/`d.ref`/`d.mesh.relocation` marks — P16, **no core change**                                                                                                  | **proposed** (adapter + emitter duty)                    |
| Weights and synapses                     | `d.weight`, `d.synapse` junction cells, `d.code`, `d.patch`, `d.entry`, `d.patch-head`, `d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.mesh.relocation` minted as data (`ensureDimension`, V10 mechanism) — minted only with no marks outstanding                                                    | **owed** (adapter duty, no core change)                  |
| Plasticity search                        | reify → graft → score → backtrack-drain, all in-turn under the trail                                                                                                                                                                                                                                | **owed** (`std:meta` composition, no new mechanism)      |
| Metacircular compiler (vagent in vagent) | a compiler written in vagent/Vortex                                                                                                                                                                                                                                                                 | deferred, U-M1                                           |
| Remote code mobility                     | sending `Code<T>` between agents with portable identity                                                                                                                                                                                                                                             | deferred, U-M2 (needs U-A2 authority)                    |
| Gradient-based learners                  | autodiff over a recorded computation graph                                                                                                                                                                                                                                                          | deferred, U-M3                                           |
| Learned/weighted plan selection          | a selector permutation inside U-A12's gate                                                                                                                                                                                                                                                          | deferred, U-M4                                           |
| Source-level reflection                  | AST-as-cells quoting                                                                                                                                                                                                                                                                                | deferred, U-M5 (refused as the base representation, §11) |
| Suspending fragments                     | `await`/`achieve`/`ask`/effect inside `code"…"`                                                                                                                                                                                                                                                     | deferred, U-M6 (needs U-A7 continuations)                |
| Generic deep-copy                        | `reify` of foreign (non-literal-born) graphs                                                                                                                                                                                                                                                        | deferred, U-M7                                           |
| Prelude grafting                         | metaprogramming `std:*` module cells                                                                                                                                                                                                                                                                | deferred, U-M8                                           |
| Partial application                      | `specialize` producing a smaller `Code`                                                                                                                                                                                                                                                             | deferred, U-M9                                           |

In the VM this document adds **no opcode**. It changes one existing behaviour (P15, a refusal — P16
reuses the existing relocation-handle machinery from the adapter and emitter, so it is a proposal
about a ceremony, not a diff), and it adds no `CompactOpNode` change, no `SystemDimensions`
field, no new `OpcodeKind` member. Every new dimension is minted as data through `ensureDimension`,
the V10 mechanism, which keeps the arena name table and `dimensions()` in agreement — but is **not**
mark-covered (`Mark` records no dimension field, `arena_manifold.hpp:170-186`), so dimensions are
minted only when no mark is outstanding (the adapter's genesis path), the same discipline §4.2 rules
for opcode minting. Most of what this document owes lives in vagent's own front end and in a
`std:meta` prelude written in Vortex — the two places vagent §11 says new capabilities must be tried
first, before the adapter and before existing code.

______________________________________________________________________

## 2. Glossary

| Term            | Meaning                                                                                                                                                                                                                                                        |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| fragment        | a lowered-but-composable opcode subgraph: an entry op, a `+d.spin` chain, wing cells, a `Return` tail and a head cell carrying its descriptor                                                                                                                  |
| `code"…"`       | the quasiquote literal: a prefixed string whose body is vagent expression source, parsed where the literal appears                                                                                                                                             |
| `Code<T>`       | the type of a fragment value: a ref-box `Cell` naming a fragment head, typed by the body's result type `T`; non-producing bodies are `Code<()>`                                                                                                                |
| head cell       | the non-op indirection cell at a fragment's root: `+d.entry` to the entry op, `+d.values` to the descriptor, `+d.name` to its label, `+d.refs`/`+d.recipe` to the relocation mark rank and the reify recipe. Clause/graft links point at heads, never op cells |
| descriptor      | the signature-as-cells on the head: ordered parameter name→type cells (count = arity), the result type name, `pure` and `closed` flags — type names qualified `module:Name` plus their arity chain (§6.1)                                                      |
| `splice`        | the unquote *keyword*: `splice c` or `splice c(args)` in expression position, substituted by the `expand` stage — not to be confused with `#Splice`, the content-restate opcode (`vortex_vm.hpp:53`)                                                           |
| `expand`        | the `Program → Program` stage between a resolve-only pass and check that parses `code` bodies, aliases captures (`cap$N` binding-table entries), substitutes splices via `cloneExpr` and expands `Code`-splicing `def`s                                          |
| `static e`      | staged evaluation: pure, closed `e` lowered and executed in the compile arena with no mark outstanding; the result becomes a literal                                                                                                                           |
| `run`           | `std:meta::run(c: Code<T>, args): T`, a checker special form: a list literal typed per-element by `any`, each element evaluated and bound on the `#Call` frame by parameter name (§5.8)                                                                          |
| `graft`         | a `std:meta` verb that attaches a fragment head at a typed program point (clause rank, `where`, body, method symbol) and thereby *consumes* it                                                                                                                 |
| installed       | the state of a `Code` after a successful `graft_*`: any of the head's `-d.grab`/`-d.spin`/`-d.values` install-dim backlinks is occupied (attach verbs reserve them — M14); a second `graft_*` of an installed fragment is refused                                  |
| `learn`         | a `graft` variant that reifies the fragment, installs one copy, registers a *second* fresh `reify` on `d.code` and records a `d.patch` entry — durable across checkpoint/resume, conditional on P16                                                                                                                            |
| patch           | a cell on the agent's `d.patch` rank recording one durable graft: ordinal, target descriptor record, a `+d.patch-head` link to a `+d.step` chain of recorded *registered-head* names, a `+d.name` label                                                         |
| closed fragment | a fragment with **no program-side references that lack a `+d.refs` name-record**: every outside reference is a state-side cell link, a captured field/module-var binding, or a `+d.refs` name-record carrying a qualified name; the flag is set at emission and **re-verified by `learn` at accept time** (§6.6) |
| session handle  | a `Cell`/`Code` value's ref-box `Int`: valid only while the named cell lives at that dense id — one arena, one incarnation, and unsafe carried between arenas at all (§4.5)                                                                                    |
| graft point     | a typed attachment: a goal/event `d.clause` rank, a clause's `+d.grab` (test/`where`), a clause's `+d.spin` (body), a module `d.vars` symbol slot                                                                                                              |
| focus slot      | the idiom every `Cell`-parameterized verb uses: `Value`-read the ref-box's `int64` referent, `Value`-write it into a fresh wing slot cell linked to the structural op (§6.1)                                                                                   |
| `reflect`       | the collective name for `std:meta`'s program-structure read functions — `program`, `agent_cell`, `module_named`, `symbol_named`, `clauses_of`, `test_of`, `body_of`, `plan_named`, `code_named`, `label_of` — not a distinct verb                              |
| plasticity      | mutation of an agent's behaviour structure — clause ranks, plan bodies, handler sets, link topology — as opposed to scalar state updates alone                                                                                                                 |
| synapse         | a *junction cell* on the agent's `+d.synapse` rank naming two endpoints by qualified-name label cells, with its weight on a `+d.weight` edge to a scalar cell (§8.1)                                                                                           |

## 3. Rulings

Each ruling states what it decides, why, and what it costs. Alternatives are in §11. The three
positions named in the introduction are marked where one of them carried a hard trade.

**M1. Code is cells; there is no second representation of programs.** A `Code<T>` value denotes a
lowered fragment graph — the same `{entry, result, produced, resultType}` object
`vagent/compiler.hpp:28-34` already returns — with a small head cell carrying its descriptor. There
is no AST-as-cells format and no side table, because the Vortex document already chose this: "all
Vortex instructions, call wings, contracts, and parameter pipelines are ordinary cells"
(`design/vortex-hyperstructural-runtime.md:1047-1049`). *Nelsonian wins.* *Price:* reflection is
structural, not source-level — a fragment can be walked, typed and grafted, but its original
spelling is recoverable only from the `d.name` label's source position, a *name* that needs an
external source map (§6.7), not from the graph. U-M5 defers source-level quoting.

**M2. `code"…"` is a prefixed string literal holding vagent expression source.** The `cells"…"`
precedent is followed exactly: the lexer already merges any identifier prefix into one
`PrefixedStringLiteral` token (`vagent/lexer.cpp:379-398`, `vagent/token.hpp:22`), so `code"…"`
needs no new token kind — the parser recognizes the prefix (the `!= "cells"` gate at
`parser.cpp:848-855` gains `code`), and the body is parsed by the real vagent expression parser, not
a shadow grammar. *Grounding wins.* *Price:* `Parser::parseExpression()` already exists and refuses
trailing input (`parser.cpp:271-281`); the genuinely new work is the `code` prefix case plus the
`CodeParams`/`#label` header parsed by a `parseCodeBody` entry on the real `Parser`, and nested
`code"…"` bodies cost a recursive parse+check per literal under threaded `CheckLimits` budgets
(§5.6).

**M3. `Code<T>` types a fragment by its result type, and `splice` composes only same-type
fragments.** `code"x + 1"` where `x: Int` is `Code<Int>`; `code"{ … }"` yielding nothing is
`Code<()>` — `CompiledStatement.resultType` is `std::optional` (`compiler.hpp:32-33`), and the
non-producing case is spelled `()`, never absent; `splice c` is legal wherever a `T` is expected and
`splice` of a parameterized fragment takes its arguments `splice c(1, 2)`. `Code<T>` is a distinct
type, not `Cell`, because only `Code` may be spliced or `run` — a raw `Cell` handle never silently
becomes executable. *Price:* `validateType`'s `scalars` set gains `Code` *and* its arity chain maps
`Code → 0-or-1` — `Code<T>` the typed form, bare `Code` the wildcard below
(`checker.cpp:555-562`'s arity arm accepts both counts); `Code` also joins
`builtinTypes` (`checker.cpp:2069-2071`) so no user `record`/`event Code` shadows it through the
`user` arm; and a signature descriptor emitted per fragment (§5.2), ~6–10 cells each. Separately, a
bare `Code` annotation is a new *parameterized wildcard* — accepts `Code<T>` for any `T`, refuses
non-`Code` — which `any` (`checker.cpp:27`) cannot express (§6.1's graft signatures use it).

**M4. Expansion is a separate `expand` stage — `{Program, FragmentTable} → {Program, FragmentTable}`
in, merged `Program` out, between a resolve-only pass and check — that substitutes cloned fragment
ASTs and aliases captures through `CompilerBindings`.** The checker cannot expand: `Checker::infer`
is const-in/judgment-out — it returns a `Type`, never a rewritten tree (`checker.cpp:1121`) — and
`Expr` children are `unique_ptr` with no clone (`ast.hpp:90-109`). So a dedicated stage owns it: a
resolve-only pass first stamps each `Identifier` with its binding's scope id (one scope truth — the
checker's env construction extracted as a shared helper, not a second implementation of it); then
`expand` parses each `code"…"` body at its literal position, binds every captured free *value* name
to a fresh `cap$N` — stamped with the captured binding's identity so the checker resolves it
straight to that binding (program-member names stay name-resolved, §5.3) — that aliases the
*captured binding's own cell* in `CompilerBindings` — a pure
binding-table alias, never a synthesized `let`, because `let` lowers to a value copy into a fresh
local (`compiler.cpp:705-707`, `:853-855`) and `validName` would refuse the name outright
(`compiler.cpp:407-417`) — substitutes each `splice` with a `cloneExpr`'d copy of the fragment's
AST, and expands `Code`-splicing `def`s per call site (M13). This *is* renaming — the honest version
of gensym, at the binding layer rather than the syntax layer (§5.3 corrects v1.0's "no gensym
needed" claim). Deep copying is mandatory, not optional: kernel side tables key on `const Expr *`
(`localReferences_`/`types_`/`paths_`/`discarded_`, `compiler.cpp:884-891`), so shared nodes would
share local cells. N splice sites mint N graphs. *Grounding wins.* *Price:* a new stage plus
`cloneExpr`/`cloneDecl` (§10.1), a `check` signature change to carry the fragment table,
`CheckLimits::maxExpansionDepth`/`maxSplices` threaded through the recursive parse (per-compile
counters, not per-parser), and the AST copying itself — O(fragment AST) per splice site.

**M5. The only compile-time evaluation is `static e`: pure, closed, run in the compile arena with no
mark outstanding.** `static e` lowers `e` and executes it with the same `VortexVM` the kernel
already holds (`Emitter` keeps `core_` and `vm_`, `compiler.cpp:100`), bounded by `staticFuel` in
`ScalarCompilerLimits` — then **explicitly unlinks its eval cursor from home's `+d.cursors` rank**
(`spawnCursor` registers it, `vortex_vm.cpp:148-161`). It runs under *no* mark: a mark+release would
mint op cells under a live mark and truncate them, exactly the `opcodeMap_`-poisoning hazard §4.2
rules out (`compiler.cpp:108-110` is the existing invariant this preserves). The residue — dead eval
graph, cursor cell, temporaries — is priced per `static` (§13) and stays in the compile arena,
which is discarded: none of it reaches the sealed artifact. `e` must be pure (`pureBody`,
`checker.cpp:827`) and closed in a *rewritten* sense: every free name resolves to a literal — a
`let x := <literal>` binding counts, a `var`-bound name does not (it is mutable) — or a
`static`-bound name (`let x := static e` is the binding form); `code` parameters are forbidden
inside `static` (they have no value until instantiation); arena-reading *forms* are forbidden —
`cells"…"`, `believes`, `findall`, `run`, host/effect calls and the implicit names (`self`,
`parent`, `inner`, `outer`, `occurrence`); and the result type is restricted to the
literal-expressible scalars `Int`/`Float`/`Str`/`Bool`/`()` — a `Cell`/`Code` result would bake a
compile-arena handle into the sealed artifact. *Realist wins — arbitrary compile-time vagent
evaluation is refused in §11.* *Price:* one bounded VM run per `static` — and per *site*: a
`static` inside a spliced fragment re-evaluates once per expansion site — plus the residue above.

**M6. `run` executes a fragment on the same cursor with the call/spill convention; fragments cannot
suspend.** `std:meta::run(c: Code<T>, args)` is a **checker special form**: `args` is a list literal
(the no-variadic-surface reason stands, `parser.cpp:316-339`) whose elements are each typed by a
per-element `any` wildcard — heterogeneous argument lists like `[3, flag]` are legal here and
nowhere else — evaluated in order and bound on the `#Call` frame by parameter name exactly as method
arguments are (`vagent-agent-language.md` §5.8 — values, not ref-boxes; there is no `Cell`-boxing
surface in the language and none is added). When the fragment is statically known the checker
verifies arity and element types at the call; otherwise the descriptor comparison of §7.2 applies at
run time. `run` emits a `#Call` to the entry op reached through the head's `+d.entry`, pushes the
§5.8 frame — argument cells are frame-bound per activation, so recursion is safe — spills and
restores the fragment's `result`/`produced` cells (recorded on the descriptor, §6.1) around it, and
yields `T` — `()` for `Code<()>` — or fails when `produced` is false. The descriptor's parameter
cells are signature data for `validate`, never argument receivers. Two distinct failure paths exist
and are kept separate: `produced` false is expression failure; a fragment graph that hits the shared
`Fail` op backtracks *through* the `run` into the caller's choice structure (the stack-top relink at
`vortex_vm.cpp:238-243` is what makes the unwind sound). Fragment tails are `Return`, not `Halt`: a
`Halt` would park the cursor mid-statement (`vortex_vm.cpp:1220-1225`) without popping the frame.
Suspending statements — `await`, `achieve`, `ask`, `yield`, effect calls — are refused inside
`code"…"` because a fragment has no step path (V4); U-M6 records the continuation future. `adopt`,
`emit`, `believe`, `install`, `graft`, `run` itself (a nested `run` pays a frame per level) and the
other non-suspending actions are allowed and stay impure. `run` is *not* re-entrant on
creation-bearing graphs: a fragment containing `cells"…%"` creations lowers to `New` with a
statically bound output (`compiler.cpp:793-799`), and `New` refuses a second use in append form and
re-links silently in non-append form (`vortex_vm.cpp:736-744`); the spill restores values, not
links, so `validate` refuses bound-output `New`s in any repeatedly-executable fragment (§7.2).
*Realist wins.* *Price:* a `run` costs a frame, O(arity) frame writes, a spill of two cells and a
`Call` — same as a method call; recursion pays a frame per level and dies at fuel.

**M7. Runtime mutation is ordinary structural ops on program cells, packaged as `std:meta` verbs —
`run`, `reify`, `graft_*`, `prune`, `learn_*`, `unlearn` — under the existing purity and position
rules.** `Link`/`Break`/`New`/`Value`/`Splice`/`Clone`/`Linked` already do every needed surgery
(`vortex_vm.cpp:616-836`); no new opcode is required, and per the project's rule new C++ must
justify why it is not Vortex — the surgery verbs are Vortex routines in `std:meta`, exactly the
precedent `std:agent` sets as a prelude rather than a C++ module (`vagent-agent-language.md` §10).
Two composition facts the primitives alone do not provide: every `Cell`-parameterized verb targets
its referent through the **focus-slot idiom** (§6.1 — a ref-box resolves to the box, not the
referent, `vortex_vm.cpp:50-67`), and every graft attaches a **head indirection cell**, never an op
cell, because `ArenaManifold::link` *evicts* the occupied same-dim neighbour
(`arena_manifold.cpp:1347-1377`) and an op's `±d.grab` is its wing (`vortex_core.cpp:354-414`).
Mutation verbs are impure, so the existing purity machinery (`checker.cpp:861-865` treats a mutating
`cells` path as a write; `pureBody` bars actions from conditions) confines them to
handler/plan/method bodies — and, written under the turn's mark, a speculative graft is undone by
backtracking along with everything else. *Nelsonian wins the mechanism; realist wins the
discipline.* *Price:* grafts are O(1) link ops plus O(rank) appends — ranks keep no tail pointer
(the spec's §5.1), so appending to a `d.clause` rank walks it — plus one head cell per fragment.

**M8. Reflection is `std:meta`'s `reflect` family — read functions returning `Cell` and `Code<Cell>`
handles over program structure, with the program root bound by the adapter.** The program's module
symbol table, `d.clause` ranks, `+d.grab`/`+d.spin` attachments and `d.vars` slots are all arena
cells; `cells"…"` walks them, with multi-hop navigation through `$` anchors (`cells"##/+d.stdlib"`
then `cells"$c/+d.vars"`). The adapter hands `std:meta` the program-root cell the artifact loader
registered — the same binding mechanism V10 uses for the thirteen dimensions. Read functions are
pure and allowed in conditions; `test_of`/`body_of` wrap raw clause graphs as `Code<Cell>` (a `run`
on them yields a `Cell` handle to the produced cell — the honest type at an unchecked boundary), and
the wrapper heads carry `+d.entry → the wrapped graph's root op` like every head — they are
session-side and unpromotable by construction, so the uniform head convention costs nothing. The
memoization is *clause-side*: a `clause +d.code → +d.step` chain of memo **records**, each
`{+d.values → the wrapper head, +d.name → the clause's current attach-cell key}` — `+d.step`
chains the records only, so a wrapper's own `+d.step` never enters `validate`'s op-graph walk, and
a key-mismatching memo is unlinked at re-wrap — so repeated reflection does not mint a fresh
~6-cell head per call, and `test_of`/`body_of` memos coexist on the chain rather than evicting each
other. The memo chain is never on the agent's `d.code` rank (§6.7): clause cells are program-side,
so wrappers are unpromoted and die with the program at `resume`, matching the session-handle rule
they already are.
*Price:* one adapter binding, memoized wrapper cells, and rank walks at O(length) per hop.

**M9. Mutation under execution is legal, deterministic and bounded — the VM reads links at step
time, cells are never freed mid-turn, and containment uses the drain idiom, never a bare
`release`.** The cursor's program counter lives in `cursorPC_`, a VM map that *wins* over the cursor
cell's bits (`vortex_vm.cpp:116-128`) and is **not** trailed — `backtrack` restores the PC only
because it explicitly calls `setCursorOpcode(*cursor, cp.altOp)` (`:238-243`), which rewrites both.
The arena's `release(m)` restores cell bits but leaves `cursorPC_` stale, and bare `release(m0)`
under live inner marks corrupts the mark machinery itself (`release` pops only the top floor,
`arena_manifold.cpp:1695-1698`; "marks are assumed well nested", `arena_manifold.hpp:773`). So
containment is the stdlib's own drain idiom — `while (choiceDepth() > base && backtrack(cursor))`
(`vortex_stdlib.cpp:3800-3802`, `:3841-3843`), which rewinds PC, stack top and marks in LIFO order —
and a bare `arena.release` mid-turn is forbidden while marks are live. Work-budget exhaustion is a
`throw` (`consumeWork`, `arena_manifold.hpp:420-426`) that escapes `step()` into the executor's
`catch` as `ActorDown` (`mesh_actor.cpp:473-486`) — a named fault channel, not an `ExecutionResult`.
*Realist wins.* *Price:* mutation is only as safe as the fuel and the drain discipline around it;
`run`/`graft` of unvalidated graphs is a budget risk, which is why §7 prices `validate`.

**M10. Opcode kinds are immutable at runtime, and P15 enforces it: every VM write into an
`opcodeMap_`-registered cell is refused.** `getOpcodeKind` consults the VM-private `opcodeMap_`
before falling back to `asInt64` (`vortex_vm.cpp:102-114`) — the map is the *only* thing that can
desync, since an unregistered int cell's content *is* its kind (that is exactly how `New`+`Value`
mints runtime ops). The refusal therefore tests **map membership of the resolved master, not
decodability**: refusing on `getOpcodeKind` decodes would bar every `Int` write whose small value
lands in `[0, Linked]`; and `masterOf` is the right cell to test because `writeValue` resolves
through it (`vortex_core.cpp:202`) — a `Clone` of a registered op is an unregistered cell that
writes the registered master's bits, so the predicate is `opcodeMap_.contains(masterOf(target))`.
The check guards the *write arm* only — `Value`-reads of op-cell content are `validate`'s kind
check — and it sits at the write funnel: `VortexCore::writeValue`/`splice`
(`vortex_core.cpp:196-254`) with a VM-installed kind predicate, covering `executeOpcodeBody`'s
`Value`/`Splice` cases (`vortex_vm.cpp:771-806`), `step()`'s memo-hit and generic output writebacks
(`:1269-1273`, `:1287-1294`), and `runPipeline`'s param and bound-output writes (`:337`,
`:343-346`) — because every non-structural op restates its bound outputs' content, not just the two
splice ops. Fresh cells are exempt: a cell with no map entry has no kind to desync — which also
covers the three writes that bypass the funnel by construction: `setCursorOpcode`'s `setValueBits`
(`vortex_vm.cpp:139-143` — cursor cells are never registered), `Call`'s frame write (`:860-863` —
a fresh cell), and `mintOpcode`'s own `setContent` (`:73-81` — registration itself). *Realist wins —
this is the one place Vortex cannot police itself, since `opcodeMap_` is invisible to Vortex code
and the adapter runs between turns, not between opcodes.* *Price:* one `opcodeMap_` lookup per VM
write into a non-fresh cell — a hash hit per writeback, once per bound-output writeback; §10.2
registers it.

**M11. Durable mutation is a patch list: `learn` = graft + a `d.patch` record, genesis re-applies
patches in order — and it is conditional on P16.** V12's rule that no state cell links into the
program exists because the checkpoint promotes what is reachable from the agent cell
(`vagent-agent-language.md` §4.3, §V12); program cells therefore do not checkpoint, so a session
graft is gone at `resume`. This document amends V12 **narrowly**: state may link to a *closed
fragment graph* (the `closed` descriptor flag — a fragment whose every outside reference is a
state-side binding cell or a name-bound program symbol, so linking it drags only the fragment, its
state captures and name-records the program can re-resolve — never the program skeleton). But the
link skeleton is not enough: a fragment's operand slots and `Jump`/`Call`/`Branch`/`Choice`
targets are `int64` *content* (`makeScalarCell(int64(target))`, `compiler.cpp:516-525`;
`vortex_core.cpp:384,402`), and `promote` copies Int64 content verbatim while translating only links
and `OpHandle` values (`arena_manifold.cpp:1934-1954`) — a promoted fragment's internal references
would silently alias unrelated cells. **P16** is the fix, and it is adapter-side, needing no new
core mechanism — the `mesh_evaluator.cpp:723-735` publication precedent already does *both* halves
`isCellRelocationHandle` tests for: `setValueBits(OpHandle, target)` *and* a `d.mesh.relocation`→
`"cell"` marker edge (`foreign_op_handle.cpp:98-117`; an unmarked `OpHandle` fails `promote`
outright, `arena_manifold.cpp:1797-1813`). Fragment emission marks every emitter-minted int64 ref
cell whose `+d.refs` record carries no `+d.name` — the uniform rule of §6.6 — with the inert marker
edge plus a `+d.ref` record on the head's `+d.refs` rank; the adapter's flag-driven ceremony inside
its own `Executor` callback re-kinds each marked cell `OpHandle` at the checkpoint-terminating
turn's end, `publishValue` runs between turns on `const&`, and a head-of-next-turn pass writes
`Int64(*handleTarget)` back so the continuing incarnation executes intact (the full protocol is
§6.6's); genesis rehydrates `Int64(*handleTarget)` per marked cell —
`resume` overlays without `importValue`'s remap (`mesh_actor.cpp:260-271`) — then replays `d.patch`.
Without P16, `learn`/`d.patch`/`d.code` are session-only and the document says so wherever
they appear. A patch entry on `d.patch` holds the ordinal, target descriptor, a `+d.patch-head`
chain of recorded head *names* and a `+d.name` label; `std:agent`'s genesis re-runs each graft after
resume. *Nelsonian wins — the patch list is an edit decision list, the same shape the test fixtures
use.* *Price:* O(live fragment cells) promotion ops per checkpoint, the relocation pass O(refs) per
promoted fragment each way, O(live patches) replay at genesis, and a `std:meta` closedness
re-verification before `learn` accepts a fragment.

**M12. Plasticity is structural mutation of behaviour, and its primitives are cells and ranks — no
neural runtime is added.** A weight is a `Float` scalar on a state-side cell; a synapse is a
*junction cell* on the agent's `+d.synapse` rank (one `+d`/`-d` neighbour per dim makes bare `A → B`
edges a star, not a graph — fan-in needs the junction); sprouting is `New`+`Link`, pruning is
`Break`/rank repair; a Hebbian update is an ordinary `w := w + lr * s` write through the focus-slot
idiom. Searching over programs is searching over cells: `reify` a candidate, `graft` it, score under
a bounded `run` or a `succeed when`, and let the drain undo losers — the search is goal-directed
under V7's discipline. Pseudo-randomness is a deterministic wrapping-safe mix over `Int` ops (§8.6 —
`Mul` overflow-faults, `vortex_vm.cpp:457-463`, and no `Xor`/shift opcodes exist), seeded from a
state cell so turn determinism and checkpoint determinism hold. *Grounding wins — nothing here needs
an opcode, a VM hook or a runtime type.* *Price:* §8 prices each operation; the deep limits (no
weighted plan selection yet — U-M4; no autodiff — U-M3; generativity bounded to clause granularity
over a frozen literal pool — U-M9) are deferred honestly.

**M13. A `def` that *splices* a `Code<…>` parameter is a template expanded at each call site — the
trigger is the use, not the signature.** The same `Code` parameter cannot be on both sides of the
quasiquote/eval line, so the line is drawn at the `splice`: a `def` whose expansion *anywhere*
splices a `Code` parameter — body, guard, contract condition, goal clause, or a nested `code"…"`
body inside it — has that parameter splicable *only while the `def` is being expanded*: `expand`
inlines the `def`'s body per call site with the arguments substituted (`cloneDecl` copies every
field, preserving the resolve pass's scope stamps so substitution does not re-resolve), and a
residual `splice` that reaches lowering is a compile error. A `def` that merely *takes*
`Code<…>` and composes it at run time
(`def apply(c: Code<Int>, x: Int): Int { run(c, [x]) }`) is an ordinary runtime `def` — it is
expanded nowhere and keeps every runtime capability. The trigger scope is the *direct* `Code<…>`
parameter: `Opt<Code<…>>`, `[Code<…>]` and `Code`-bearing record parameters are ordinary opaque
parameters throughout. The rule applies uniformly to `def`s and `method`s wherever members can
appear — agents, behaviors, skills, modules — and carries these edges:

- **Recursion is banned**, directly or transitively: `def f(c: Code<Int>): Code<Int> { splice
  f(code"x") }` would expand forever, so a template cycle is the named check error
  `recursive-template` (a `DiagnosticCode` naming the expanding `def` and the cycle), not a
  `maxExpansionDepth` budget death.
- **Control flow is refused in template bodies** — `return`, `yield`, `consume`, `terminate` —
  because an inlined `return` would exit the *caller*: the same capture class `cap$N` fixes for
  names, refused rather than rebound.
- **`Code` actuals must be statically known** — a literal, a `let`-bound literal, or an enclosing
  template's `Code` parameter — by §5.2's transitive rule; an opaque actual is a call-site error:
  "cannot expand `f`: `Code` argument is opaque — templates compose literal fragments only".
- **Diagnostic ownership is split**: position/purity re-verification and opaque-argument errors
  belong to the call site; body errors keep the `def`'s own locations. Bodies are checked per call
  site — the checker sees each expansion in its own context.
- Templates are barred from `effect`/`native`/`capacity` membership and `graft_method` targets —
  they exist only at expansion time. The refactor consequence is stated: adding a `Code<…>`
  parameter and splicing it converts a runtime `def` into a template, and the errors land on its
  `effect`/`capacity`/`graft_method` *uses*, not on the signature.

In every other context a `Code`-typed parameter, field or reflection result is *opaque*: it
composes through `run`/`graft`, never `splice`. *Price:* expansion depth and count share the
`maxExpansionDepth`/`maxSplices` budgets; template `def`s cannot be first-class values (no
capacities, no dynamic dispatch).

**M14. A `Code` denotes one installation; `graft` consumes it.** `link` maintains one `+d` and one
`-d` neighbour per dimension by eviction (`arena_manifold.cpp:1347-1377`): a second `graft_*`
attaching the same head would evict the first attachment's link — severed, not shared. So `graft_*`
marks the fragment installed — the occupancy test is *any* install-dim backlink on the head
(`-d.grab`, `-d.spin` or `-d.values`), and those three slots are **reserved to the attach verbs**:
no other convention may write `x +d.grab`/`+d.spin`/`+d.values → H` for a head `H`, or it would fake
an install (and likely drag the program; §6.7). A second `graft_*` of an installed `Code` is
refused; `reify` produces a fresh, installable head. The consumption contract: install is
occupancy, not destruction — `run`, `reify`, `label_of` on an installed fragment stay legal; the
marker is a trailed backlink, so a backtracked/drained graft frees the `Code` for re-graft, as does
a `prune` or re-point that unlinks it. And because a `code"…"` literal evaluates to the *same* head
on every evaluation, `for … in … do graft_where(c, code"…")` installs once and refuses thereafter —
loops and factories must `reify` each iteration. The aliasing contract is stated once: **`graft`
shares, `reify` isolates** — mutating an installed fragment is visible through every reader that
reaches it, and a copy is the only way to diverge. *Realist wins — the alternative, invisible
multi-installation, fails by silent severing, not by error.* *Price:* one occupancy test per
graft; sharing bugs become refusals, and `learn_*` pays a `reify` where a distinct installation is
wanted.

______________________________________________________________________

## 4. The substrate: code is already cells

### 4.1 What a compiled statement is

The scalar kernel proves the fragment shape exists. `Compiler::compileStatement`
(`vagent/compiler.hpp:58-60`) takes one `Expr`, refuses when a mark or choice is outstanding
("assemble a statement before opening turn marks or choices", `compiler.cpp:108-110`), and returns
`CompiledStatement{entry, result, produced, resultType}` (`compiler.hpp:28-34`) — three cells
plus an optional result type in the arena. `emitStatement` (`compiler.cpp:849-872`) mints a
`ChoiceDepth` read, a `Choice` whose
alternative is a failure tail, a `Cut` to the recorded depth on success, a `Halt`, and scalar
`true`/`false`/`produced` cells. The emitted graph is entirely cells: opcode cells (`mint`,
`compiler.cpp:512-514` → `vm_.mintOpcode`), `+d.spin` chains (`spin()`, `compiler.cpp:519-522`),
wing bindings (`core_.bindInput`/`bindOutput`, slots on `+d.grab`/`-d.grab` chained `+d.step`,
`vortex_core.cpp:354-414`), and `Jump` targets as `reference()` cells —
`makeScalarCell(int64(target))` (`compiler.cpp:516-518`).

A fragment is the same object with a `Return` tail and a head cell — so `code"…"` fragments need no
new emission machinery, only a `compileFragment` entry that mints the head and descriptor around the
existing body emission, marks every emitter-minted int64 ref cell whose `+d.refs` record carries no
`+d.name` for P16 — a `+d.ref` record on the head's `+d.refs` rank plus an inert
`d.mesh.relocation→"cell"` edge, the uniform rule of §6.6 — and
records the `reify` recipe (§6.1). That is owed work in `vagent/` (§10.1), not a core change. The
honest scope note: the
scalar kernel lowers the *scalar subset only* —
`{Integer, Float, String, Boolean, Unit, Identifier, Fail, Unary, Binary, Block, Let, If, Cells}`
(the case labels `compiler.cpp:224-498`, refused at `:500-503`); fragments containing `believes`,
`emit`, `adopt` or other actions lower
through the same executor lowering the artifact itself uses, which vagent §11.1 owes. The scalar
kernel is the shape proof, not the whole vehicle; §9's examples name which lowering each assumes.

### 4.2 Opcodes are int cells, and two caches watch them

An opcode is a cell whose `int64` content is the `OpcodeKind` (`vortex_vm.cpp:74`); the enum's
integers are therefore serialized data, which is why `Linked` was appended last
(`vortex_vm.hpp:89-93`) and why `getOpcodeKind`'s serialized ceiling follows it
(`vortex_vm.cpp:108-110`). The decode path has two layers, and their difference matters here:

- `opcodeMap_` (`vortex_vm.cpp:103-105`) — the VM-private map, populated by `mintOpcode` and
  `setOpcodeKind`. It wins over content, and it is the *only* layer that can desync: an unregistered
  cell's `int64` is both its content and its kind.
- `asInt64` fallback (`vortex_vm.cpp:107-112`) — a cell the map does not know decodes as
  `OpcodeKind` iff its `int64` is in `[0, Linked]`. This is how deserialized and runtime-minted op
  cells execute without registration — and why "refuse writes to anything `getOpcodeKind` decodes"
  would refuse every small-valued `Int` write; P15 tests *membership* instead (M10).

Two cache consequences are load-bearing for this document:

1. **Kind rewrites desynchronize.** A `Value` op — or any op's bound output writeback
   (`vortex_vm.cpp:1269-1294`, `runPipeline`'s `:343-346`) — that writes `int64` into a *registered*
   op cell changes what `asInt64` sees but not what `opcodeMap_` holds — the map wins, the cell
   lies. This is P15's motivation (M10, §10.2).
1. **Runtime-minted ops must avoid the map.** `mintOpcode` inserts into `opcodeMap_`
   (`vortex_vm.cpp:75`). If host C++ minted an op cell under a live mark and `release` truncated the
   cell away, the dense slot could be re-minted as a non-op cell that `opcodeMap_` still claims is
   an op. Today this cannot happen: the kernel mints opcodes only when no mark is outstanding
   (`compiler.cpp:108-110`). `std:meta` constructs ops exclusively through `New`+`Value`
   *opcode-level* writes — never `mintOpcode` — so nothing enters the map under a mark and the
   hazard stays theoretical. `arenaDims_` has the identical gap — `Mark` covers no dims
   (`arena_manifold.hpp:170-186`) — so `ensureDimension` mints `d.code`/`d.patch`/`d.entry`/
   `d.patch-head`/`d.validate`/`d.weight`/`d.synapse`/`d.refs`/`d.ref`/`d.recipe`/
   `d.mesh.relocation` only on the no-marks
   genesis path; a `dimensionNamed` hit on a mark-released mint would alias a wrong cell the same
   way. §6.4 states the rule; P15 closes the user-level half of it.

Other VM state follows the same split. `cursorPC_` is the *authoritative* PC — the map is consulted
first, the cursor cell's bits are the persistence backing (`vortex_vm.cpp:116-128`) — and it is
**not** trailed: backtracking restores the PC because `backtrack` calls `setCursorOpcode` explicitly
(`vortex_vm.cpp:238-243`, relinking the stack top too), not because the write was trailed.
`memoizedOps_` is VM-only and opt-in: `enableMemoization` is the only writer
(`vortex_vm.cpp:169-171`), and nothing in the fragment machinery opts in — §7.3 records the rule
that mutable graphs must not be memoized. The §1 "transactional mutation" row is thereby qualified:
the trail covers *arena* state, not these side channels.

### 4.3 The trail makes program mutation transactional

`mark()` records arena extents; a write to a cell older than the innermost mark is trailed — the
slot *and* its link and content runs are copied out above the mark (`arena_manifold.cpp:1506-1570`,
with the exact WAM-style conditional-trailing argument in `arena_manifold.hpp:722-730`).
`release(m)` undoes every write and truncates cells minted after `m`
(`arena_manifold.cpp:1612-1700`); `discard(m)` keeps the writes and, once the last mark is gone,
clears the trail (`arena_manifold.cpp:1596-1610`, P13). `Choice` takes the mark
(`vortex_vm.cpp:217`), `Fail` releases it (`:234`), `Cut` discards it (`:255`).

Program cells are cells. Re-pointing a `+d.spin` link, appending a clause cell, writing an op's
input wing — all are trailed writes. Therefore **a speculative program mutation is undone by
backtracking exactly like a speculative belief write** — the mechanism `bounded()` already relies on
for statements (`compiler.cpp:578-588`). Self-modifying search — mutate, evaluate, fail, and the old
program returns — is free, and it is the reason the plasticity section (§8.5) needs no
snapshot/restore machinery of its own. Two bounds the trail does not give: it does not cover the
VM-side maps (§4.2), and `release` assumes well-nested marks — undo over live marks goes through the
drain idiom (§6.4), never a bare `release(m0)`.

### 4.4 What `cells"…"` reaches already

The existing subset (`vagent/cell_path.cpp:17-143`, documented in `vagent-agent-language.md` §8.7):
anchors `##` (home), a literal id, or `$binding`; signed `+d.name`/`-d.name` steps; posward `%`
creations; at most one traversal step, which must be last; one link-presence shape filter.
Everything else VQL accepts is refused with a diagnostic, never silently narrowed — the discipline
this document keeps for `code"…"`.

Program cells are in the same arena, so `cells"…"` reaches them through `##` and the dimension
ranks: `##/+d.stdlib` for the standard modules, the program module's `+d.vars` for symbols, a goal's
`+d.clause` for plan alternatives, a clause's `+d.grab`/`+d.spin` for test and body — heads under
§6.3's attach convention; op roots appear only in graphs predating it (today's kernel emission is
exactly such a graph, which is why readers discriminate by `+d.entry`-presence, §6.1). The
one-traversal limit means multi-hop lookups go through `$`
anchors and iteration — `for c in cells"##/+d.stdlib" do …` then `cells"$c/+d.vars"` — which
`std:meta`'s read functions package. What `cells"…"` cannot do is write through a handle: `%`
creations anchored on a `$` binding are refused (`compiler.cpp:484-486`), so a `Cell` value cannot
be dereferenced for mutation at all — the focus-slot idiom of §6.1 is the `std:meta` answer.

What `cells"…"` deliberately cannot write today: scalar-typed cell content (its `%` literals are
strings, `cell_path.hpp:31`), and hence op kinds — an op needs `int64` value bits, minted through
`New`+`Value` with a scalar-typed source cell, not a string literal. The compiled-VQL subset is
narrower still: `validatePath` refuses whole-rank traversal outright (`vql/compiler.cpp:36-80`,
notably `:71`), so nothing there helps — `cells"…"`'s own `Linked` path is the traversal substrate,
as `projects/vagent-review/r10-vortex.md` records.

### 4.5 What survives a backtrack, a turn and a checkpoint

| Boundary                    | Program cells              | Fragment heads + graphs                           | `Code`/`Cell` handles (ref-box ints)   | `d.patch` records              | Intra-graph reference ints (`reference()`, wing slots)                                                             |
| --------------------------- | -------------------------- | ------------------------------------------------- | -------------------------------------- | ------------------------------ | ------------------------------------------------------------------------------------------------------------------ |
| backtrack inside a mark     | trailed → restored         | minted-under-mark copies die; older ones restored | trailed write → restored               | as written                     | trailed → restored                                                                                                 |
| turn commit (`discard(m0)`) | persist                    | persist                                           | persist                                | persist                        | persist                                                                                                            |
| checkpoint (`publishValue`) | **not** promoted (V12)     | promoted iff reachable (e.g. `d.code`)            | **stale**: int names a dense id        | promoted (state cells)         | Int64 promoted verbatim (`arena_manifold.cpp:1934-1936`) — stale without P16                                       |
| `resume`                    | reloaded from `programRef` | rebuilt if promoted; fresh ids                    | **meaningless**: stale or aliased ints | present; re-grafted at genesis | **aliased**: names whatever cell landed at the old dense id — rehydrated by P16's genesis pass, garbage without it |

Three rows deserve emphasis. Program cells do not checkpoint — that is V12 working as intended, and
it is why session grafts are ephemeral and `learn` needs `d.patch`. Ref-box ints are *session
handles*: `promote` mints store cells in reachability order, not mint order (the BFS at
`arena_manifold.cpp:1744-1788`, structure at `arena_manifold.hpp:235-240`), and `resume` rebuilds an
arena whose dense numbering need not match the old one — a stale `Int` can name the wrong cell
silently. And the last column is the one v1.0 missed: the same int-content mechanism is *inside*
every fragment graph, so a promoted fragment that survived reachability still executes garbage until
P16 relocates its ref cells — `closed` constrains links, not int content. Durable references are
therefore structural — links, `d.name` labels, and *names* (the `+d.refs` name-records of §6.6,
re-resolved at genesis) — never stored ints. `Cell` values already
carry this caveat (`vagent-agent-language.md` §5.9 names "a live arena cell"); `Code` inherits it —
and the caveat extends **cross-agent**: `resolveCell`'s `arena.contains` check is a dense-range test
(`vortex_vm.cpp:50-67`; `arena_manifold.cpp:256-267`), so a foreign ref-box whose dense index
happens to be in range resolves to a *live local cell*, not `nullopt` — received `Code`/`Cell`
values are opaque, re-resolved by `d.name` label, and never passed raw to `cells"…"`, `Linked`,
`run` or `graft`.

______________________________________________________________________

## 5. Compile-time metaprogramming

### 5.1 The `code"…"` literal

```ebnf
Primary    ::= … | PrefixedStringLiteral | 'splice' Postfix | 'static' Unary ;
Type       ::= … | 'Code' [ '<' Type '>' ] ;
```

(`PrefixedStringLiteral` is one token — the lexer merges `Ident '"'` with no gap
(`lexer.cpp:381-393`), so `code"…"` is one literal and `code "…"`, with a space, is an identifier
followed by a string, which cannot form an expression — the same adjacency rule the spec's
`CellsLit` note states for `cells "##"`.)

`code`, `splice` and `static` are **contextual** keywords, in the same class as `succeed` and
`holds` — `code` stays legal as an identifier everywhere, while `splice`/`static` (and
`run`/`value_of` when `std:meta` is imported) stay legal only as member names after `.`; and of
these only `code` may prefix a string
(the lexer already produces `PrefixedStringLiteral` for any identifier prefix,
`lexer.cpp:379-398`; the parser owns prefix validation, as the spec's §8.1 rule "any other prefix
on a string literal is a syntax error" delegates to it — the `!= "cells"` gate at `parser.cpp:851`
gains `code`). Two reservations this creates: `splice` and `static` are prefix forms wherever an
operand can follow — a bare `splice` or `static` with no operand is a parse error — so a user
`def`/`method` named `splice` or `static` is unreachable as a bare call — the checker rejects such
declarations (`x.splice()` still works, since any `Word` follows `.`) — and the refusal extends
to `let`/`var`/parameter bindings named `splice`/`static`, and to `run`/`value_of` declarations and
bindings when `std:meta` is imported (a user `def run` or `def value_of` is refused under the
import — those names must keep denoting the imported members and the `run` special form).
`splice f(args)` instantiates
a parameterized fragment named `f`; it never splices a call *result* — when the operand's outermost
postfix ends in `( args )` the arglist belongs to the `splice` (the peeling rule), and any other
call-shaped operand is a check error (`run(f(x))` is the runtime path). `static` is
`Unary`-prefixed, so the folded operand is written parenthesized: `static (60 * 1000)` folds the
product, whereas `static 60 * 1000` would fold `60` and multiply the result.

The body of `code"…"` is vagent expression source with two inner-grammar extensions — an optional
label and an optional parameter header:

```ebnf
CodeBody   ::= [ '#' Ident ] [ CodeParams '|' ] Expr ;
CodeParams ::= Ident ':' Type { ',' Ident ':' Type } ;
```

A leading `'#' Ident` is a user label on the fragment head (§6.7); `#` can begin neither a
`CodeParams` nor an `Expr`, so the prefix is unambiguous — and it is stripped textually before the
inner lex runs, since `#` is not a vagent token.

`code"w: Int | x + w"` is a parameterized fragment `Code<Int>` taking one `Int`. A leading
`Ident ':' Type … '|'` is unambiguous: no `Expr` can begin with a type annotation, so the header is
present iff it parses, exactly the way `Ident '{'` resolves to a record literal by case convention
in the spec's §8.1. Inside the body, `splice` is the unquote — `code"x + splice c"` — written with
the same spelling it has outside, since the inner grammar *is* the vagent grammar. `,name` unquotes
are refused (§11): inside the string they would collide with `ArgList` separators (`code"f(a, b)"`).
Fragments are *expressions*; declarations are produced at run time by `graft_*`, never by expansion
— there is no decl-level macro.

Escapes work as in `cells"…"` — `code"…\"…\"…"` — because the literal is one token with the usual
string escapes (`vagent-agent-language.md` §8.1); nesting composes as levels,
`code"x + splice code\"n: Int | n + 1\"(x)"` giving `Code<Int>` around an inner fragment spliced
with its argument (one level of `\"` per enclosure — an inline literal is a statically-known
operand, §5.2). Diagnostics inside a body are reported at the literal's own source position
qualified by the inner offset — `src:<module>:<line>:<col>+<off-line>:<off-col>`, e.g.
`src:demo:12:4+3:10` — so an inner error names the string, not bare inner coordinates. `return` is
refused inside `code"…"` (a fragment's produced value is its last expression's; early produce is
written with `if`), and suspending statements are refused per M6.

### 5.2 `Code<T>` and `splice`

`Code<T>` joins the scalar type set: `validateType`'s `scalars` gains `Code` and its arity chain
maps `Code → 0-or-1` (`checker.cpp:555-562`'s arity arm accepts both counts — `Code<T>` the typed
form, bare `Code` the §6.1 wildcard), `Code<Int>` is a field, parameter and result type, and
`==`/`!=` on `Code` compares identity like `Cell`. `T` is the fragment body's checked result type,
computed at the literal's position; a body that produces nothing is `Code<()>` — never an absent
`T`. A `code"…"` literal is a *pure* expression — it produces a value and has no run-time arena
effect, so it is legal in `where`, guards, `var` initializers and answers; `splice`'s purity is the
substituted body's, and `static`'s purity is required (M5). A `splice` is never a place: a captured
name expands to a `cap$N` *binding*, not a re-assignable slot, so `splice c := 3` is a check error —
assignment targets stay explicit locals.

`splice c` — in any expression position, inside or outside `code"…"` — requires `c` to name a
**statically known** fragment, a judgment `expand` makes *transitively and post-substitution*: a
`code"…"` literal (including an inline literal operand, §5.1's nested form), a `let`-bound name
whose initializer is itself statically known — a literal, an identifier naming a statically-known
binding, or a substituted actual inside a template expansion — or a `Code` parameter of a `def`
*while that `def` is being expanded at a call site* (M13 — in every other context the parameter is
opaque). A `var`-bound literal is an explicit refusal: the binding is mutable, so its initializer
is not a stable referent. `splice c(splice d)` composes inside-out: the inner `splice` substitutes
`d`'s *body* (an expression of type `T`), so the argument to `c` is a `T`, not a `Code<T>` — legal
exactly when `c`'s parameter is `T`. The `expand` stage substitutes a `cloneExpr`'d copy of the
fragment's AST and re-verifies the composition in the *site's* context: the position must accept
`T`, and the substituted body's position and purity obligations are re-checked where it lands (a
fragment checked inside a handler body, where `:=` or an effect call is legal, does not become legal
inside an operand or a
`where` — the checker re-walks it under the site's `context.bounded`/`suspend`/`body`, or
equivalently checks a position-requirement mask the descriptor records), and `splice c(a, b)` on a
parameterized fragment checks the arguments against the parameter signature. `splice` of a `Code`
value that is *not* statically known — a field, a `reflect`-family result, a `var`-bound name, any
`def` parameter outside its own expansion — is a check error: opaque fragments compose at run time
(`run`, `graft`), not at expansion time. This is the line between quasiquote and eval, and a
residual `splice` that reaches lowering is a compile error by definition.

`splice` in statement position on `Code<()>` is the "macro call" form: the fragment body is inlined
as the statement. The type rule is the same as any expression in that position, so a `Code<Int>`
spliced where a `Bool`-tested operand is wanted is a type error — the `requireSame` discipline
(`compiler.cpp:189-193`) applied one stage earlier.

At expansion each splice site instantiates a fresh *clone* of the fragment AST (`cloneExpr`, §5.6):
sharing is impossible (one parent per `unique_ptr` node) and unsound (kernel tables key on
`const Expr *`, `compiler.cpp:884-891`). N sites mint N graphs — the compile-time equivalent of
`reify`, with no sharing unless the programmer `run`s one fragment instead.

### 5.3 Hygiene

Hygiene is *explicit renaming at the binding layer* — v1.0's "no gensym needed" was wrong, and the
correction is the honest version of the claim:

1. **Captures are aliased (`cap$N`).** `expand` walks each `code"…"` body against its
   definition-site scopes (the resolve-only pass's scope ids — one scope truth, §5.6); every free
   name that resolves to an enclosing *value* binding is renamed in the body AST to a fresh `cap$N`
   identifier — unlexable in source (`$` is a lexical error at `lexer.cpp:333-335` and absent from
   the identifier charset, `lexer.cpp:14-24`), and the checker *also* refuses `$`-bearing names on
   user-supplied decls, so no programmatically built `Program` can squat on the namespace either
   (`snake()` accepts `$` today, `checker.cpp:30-36` — the guard is §10.1's). The rename covers
   *value bindings only*: program-member references (`def`/method/plan/goal names) stay
   name-resolved at emission and carry `+d.refs` name-records (§6.6) — they are never
   capture-renamed. The renamed `cap$N` keeps a resolve stamp naming the *captured binding
   itself*, so the checker resolves it straight to that binding under §5.6's stamp-honoring rule;
   the `{cap$N : type-of-name}` env records `expand` also inserts at the literal's position are a
   diagnostics and readability aid — `cap$N` renders in messages as the captured *name*, never
   `'cap$3'` — not the resolution channel. `cap$N` is a **pure
   binding-table alias, not a synthesized `let`**: `ExprKind::Let` lowers to `emit(initializer →
   fresh localCells_ slot)` (`compiler.cpp:705-707`, `:853-855`) — a value copy that would snapshot
   the capture where the contract is cell identity — and `validName` would refuse the name at kernel
   preflight anyway (`compiler.cpp:407-417`). Fragment emission and the merged emission resolve
   `cap$N` through `CompilerBindings` to **one
   cell: the captured binding's own cell**. For fields and module vars — the *stable* class —
   that is true cell identity and reads stay live. Frame-bound bindings — `def`/plan/handler
   parameters *and* `let` locals — are the ABI-debt class: under the §5.8 frame convention a
   local's cell is per-activation like a parameter's (the scalar kernel's static `localCells_` is
   the interim no-call shape, `compiler.cpp:853-855`), so a captured local is orphaned at `Return`
   and reads back the last-written value — the same observable rule parameters get. For that class
   there is no emission-time cell to name: the `cap$N` alias and its capture-slot edge are
   established **per-activation by frame binding** (the ABI debt), and `cells"$cap$N"` anchors are
   refused on frame-bound captures — anchor resolution is compile-time
   (`compiler.cpp:475-483`), so anchors are supported for stable captures only. Captured cells
   are referenced by *links* — the emission wraps the binding cell in a capture-slot cell on the
   operand's wing, decoded by `wingOf`'s operand lambda (`asInt64` fails → `masterOf(slot)`,
   `vortex_core.cpp:356-365`) — and the reason is `closed`-verifiability: a linked capture is
   discoverable by edge at `learn`'s re-verification, while an int needs a record to be found
   (the link is the *checkable* form; P16 would relocate a marked int too, but links are what the
   closed walk reads). `cap$N` aliases bypass `binding()`'s render check (`compiler.cpp:160-181` —
   capture cells are compile-time-unvalued; the checker already proved the type) and resolve
   through a `cap$N`-keyed entry in `externalReferences_`, materialized while the enclosing `emit`
   still holds the minted cells.
1. **One counter per `expand` run.** `cap$N` is allocated from a single counter shared across all
   literals and all substituted bodies in a compile — never per literal — so a `cap$0` inside a
   cloned body cannot collide or cross-resolve with an enclosing literal's `cap$0` (`cloneExpr`
   preserves the names verbatim).
1. **`cells"…"` `$` anchors are renamed too.** `cells"$w/…"`'s anchor is string data —
   `CellsPath::Anchor::variable` (`cell_path.hpp:23`), resolved through `bindings_` at kernel
   preflight (`compiler.cpp:475-483`) and invisible to AST renaming — so `expand` rewrites
   `CellsPath` anchor variable names through the same rename map when it re-parses each body (the
   `$cap$0` anchor then resolves through the same `CompilerBindings` entry — stable captures only;
   item 1 refuses the anchor on frame-bound captures). Refusing `$` anchors
   inside `code"…"` was the alternative; §11 records it.
1. **Locals cannot collide.** `let` locals inside a fragment become kernel `localCells_` minted per
   compile (`compiler.cpp:432-434`, `853-855`), and splice clones the AST per site — each site's
   locals are distinct nodes minting distinct cells. Same-name locals shadow by the scope maps, as
   ordinary `let`s do.
1. **Parameters are explicit.** A parameterized fragment's only holes are its declared `CodeParams`;
   everything else is closed over definition-site bindings.

The statically-known operand rule is also the **capture-liveness rule**: because a splicable
fragment is a `let`-bound literal in scope or a call-site argument, every binding it captured is in
scope at its expansion site by construction — the `cap$N` aliases are live wherever the literal is
live — so `splice` never reads a dead cell. The subtle case that survives is `run`, not `splice`,
and it is the frame-bound rule: a fragment that captures a `def`/plan parameter's or a `let`
local's binding cell captures the cell, and frame cells are `makeCell`-fresh per `#Call`
(`vortex_vm.cpp:860`), orphaned — not freed — when `Return` re-points the `+d.stack` link
(`vortex_vm.cpp:878-890`). The observable rule: **a `run` on such a fragment reads the binding
cell's last-written value** — the argument or local of the most recent activation of the body the
literal sits in. That is honest closure semantics — the
cell *is* the variable — at the price of one dead frame cell per captured call until reclamation.
§13 prices it; §11 records the refused alternative (value-snapshot captures, which would break the
cell identity model).

### 5.4 Parameterized fragments

`code"w: Int, b: Bool | if b then w + 1 else w"` — a fragment is a template. Two uses:

- `splice c(3, flag)` expands inline, **call-by-value**: the expand stage binds each argument to a
  fresh `let` local at the head of the expanded body — each argument evaluated once, in argument
  order — matching `run`'s frame discipline rather than textual inlining (`splice c(f())` must not
  evaluate `f()` per parameter use). A `Code`-splicing `def` (M13) follows the same rule for its
  non-`Code` arguments — call-by-value through fresh `let` locals at the inlined body's head —
  while its `Code` arguments are statically-known bindings, never re-evaluated.
- `run(c, [3, flag])` calls it at run time: `args` is a list literal typed per-element by `any`
  (M6's special form — heterogeneous lists are legal here only), each element evaluated in argument
  order and bound on the `#Call` frame by parameter name exactly as method arguments are (§5.8 of
  the language spec) — values, not ref-boxes.

The parameter signature lives in the descriptor (§6.1): the descriptor record holds the parameter
*count* and an ordered rank of parameter cells — chained `+d.step` off the descriptor's `+d.grab` in
declaration order, the same cells the `+d.vars` entries name, each `+d.name`-labelled with the
parameter name and `+d.values`-typed — plus the result type name and links to the `result`/
`produced` cells themselves, so `validate` can check wing arity, `graft_*` can compare
signatures as ordered `(name, type)` pairs — binding is name-keyed, so names are part of the match
(§7.2) — and `run` can find the cells it spills. `run` and `graft` compare descriptors against the
call or attachment signature at run time (§7.2); argument cells live on the frame per activation,
never in the descriptor.

Nested `splice` composes inside-out by construction: a fragment body's own `splice`s were resolved
at *its* definition-site expansion, so a checked fragment AST contains no residual `splice`; the use
site expands exactly one level. Fragments containing `Code`-typed *parameters* are the one
exception: `code"f: Code<Int> | splice f + 1"` is legal only where `f` is being instantiated by an
enclosing `splice`/template expansion — such a fragment is splice-only, refused by `run`/`graft` (it
has no value-level parameter to receive) **and refused in evaluated positions**: it may appear only
as a `splice`/template actual, never as a `let`/`var` initializer or call argument — a `let`
binding left dead by full substitution is erased by `expand`, or refused by the checker where
erasure is unsafe. A splice-only literal emits **no head and no
descriptor** — `expand` consumed it into sites before fragment emission ran, so nothing reaches
`fragment.cpp`.

### 5.5 `static`: staged evaluation

`static e` is the only compile-time *evaluation*. Its rules:

- `e` must be **pure** — the checker's `pureBody` analysis (`checker.cpp:827`), so no actions,
  assignments or `cells"…%"` creations — and **closed** in the form-level sense: every free name
  resolves to a literal (a `let x := <literal>` counts; a `var`-bound name does not — mutable
  bindings are not closed) or a `static`-bound name (`let x := static e` binds one); `code`
  parameters are forbidden (unvalued until instantiation); and arena-reading *forms* are forbidden
  outright —
  `cells"…"` in any mode (a non-mutating `cells` is `pureBody`-pure but would read the *compile*
  arena), `believes`, `findall`, `run`, host/effect calls, and the implicit names `self`, `parent`,
  `inner`, `outer`, `occurrence`.
- `e`'s result type must be literal-expressible: `Int`, `Float`, `Str`, `Bool` or `()`. A `Cell` or
  `Code` result would emit a compile-arena handle as artifact data — the §4.5 stale-handle failure
  baked in at compile time.
- The kernel lowers `e`, runs it in the compile arena with `vm_.run(cursor, staticFuel)`, reads the
  `result` cell (`CellValue`, `vortex_core.hpp:29`) — which must match `e`'s checked type — then
  unlinks the eval cursor from `+d.cursors` and emits the value as a `literal()` in the enclosing
  graph. The run takes **no mark**: eval ops are minted with no marks outstanding, preserving the
  `compiler.cpp:108-110` invariant that keeps `opcodeMap_` honest (§4.2); the mark+release
  alternative is refused in §11 for exactly that reason.
- A `static` that fails, faults or exhausts fuel is a compile error (`DiagnosticCode`), like a bad
  literal; so is a `static` that leaves `choiceDepth() != 0` or `outstandingMarks() != 0` at run
  end (the next `compileStatement` would refuse it, `compiler.cpp:108-110`), and a bare `static`
  with no operand is a parse error — `static` is `Unary`-prefixed and the operand is mandatory.
- The residue stays in the compile arena, which is discarded — dead eval graph, cursor cell and
  temporaries never reach the sealed artifact.

`static` is deterministic by construction — pure + closed means the run depends only on the source,
and the compile arena is fresh per compile. It is how `code"…"` fragments get compile-time constants
folded in (`splice inc(static (2 * 3000))` — an `Int` argument for `inc`'s `n: Int`) and how
small metaprograms evaluate at stage 0. A `static` inside a *parameterized* fragment is judged in
the context the parameter lands in — legal where the substituted site is `static`-legal (the
fragment's own `code` parameters are still forbidden operands, being unvalued at evaluation); a
`static(param)` inside a fragment *emitted* for `run`/`graft` rather than spliced errors at
emission, the operand being unvalued there. `run`
and the rest of M6's allow-list are legal inside `code"…"` for the same reason: the fragment checks
under its own position mask, and `run` executes at fragment run time, not expansion time. What
it refuses — impure `static`, arena-reading `static`, handle-typed results, `static` that calls
`run` — is recorded in §11, with the reason: a compile-time `run` would need a program to run
against, and the program does not exist until the artifact is sealed.

### 5.6 Where expansion sits in the pipeline

```text
source ──► lexer ──► parser ──► resolve ───────► expand ────────────────► checker ────────────► kernel ──► arena
        tokens    AST        resolution-only:     Program → Program:      stage 0 typing:        lowering:
                             stamp each            parse code bodies        check merged tree      emit fragments
                             Identifier's          alias captures           (+ fragment table)     (head + descriptor +
                             binding scope id      (cap$N bindings)          compute T, purity,     refs rank + recipe +
                             ── one scope truth    rewrite $ anchors         closed flags, arity   body graph)
                                                  clone+splice ASTs          tables                static eval (no mark)
                                                  expand Code-splice defs
                                                  residual-splice check
```

Expansion is a real pass over `Program`, not a lexer trick, not a parse-time rewrite and not the
const-AST checker — a pipeline member between a resolve-only pass and check. The boundaries hold:

- The **lexer** only merges `code"…"` into one `PrefixedStringLiteral`; it never interprets the
  body. A `cells`/`code` confusion is a parser error, as with any wrong prefix.
- The **parser** produces `Expr{kind: Code, text: body}` (new `ExprKind::Code`, `Splice`, `Static`
  beside `ast.hpp:37-88`'s `Cells`) and `Type{name:"Code",…}` (the generic type syntax already
  parses it, `ast.hpp:15-19`). The `code` body's own parse happens in `expand` through
  `parseCodeBody` — a recursive call on the real `Parser`, with `CheckLimits::maxExpansionDepth` and
  `maxSplices` threaded through as per-compile counters (each nested literal spawns a fresh `Parser`
  whose own limits would not accumulate).
- The **resolve-only pass** stamps each `Identifier` with its binding's scope id — the checker's
  env construction extracted into a shared helper it reuses, so `expand` never re-implements the
  scope rules (`for`-body `copyEnvironment`, `as`-bindings, the implicit names, plan/handler
  parameters — `checker.cpp:579-582`, `:1505-1524`): one scope truth, no drift. The stamp rides
  on the `Expr` itself (a field `cloneExpr`/`cloneDecl` preserve — pointer-keyed side tables die
  under cloning); `expand` invokes the same helper per `code"…"` body subtree, seeded with the
  literal's scope, and the driver sequencing parse→resolve→expand→check is `frontend.cpp` (§10.1).
- The **expand stage** owns semantics of composition: `cap$N` capture aliasing (binding stamps plus
  `CompilerBindings` entries and diagnostic env records at literal position, §5.3), `CellsPath`
  `$`-anchor rewriting, splice
  substitution via `cloneExpr`, `Code`-splicing `def` expansion, the residual-splice check, and the
  statically-known judgment (transitive, post-substitution, §5.2). It emits the merged `Program` —
  plus the fragment table, **keyed by `SourceLocation`/ordinal stamped on the `Code` node** so
  `cloneExpr`'d literal copies share the original's record; the record's body AST is itself
  re-cloned per emission because kernel tables key on `const Expr *` (`compiler.cpp:884-891`) and a
  second emission of the same nodes would double-register `localReferences_`. `check` gains the
  table as a parameter — a signature change (§10.1).
- The **checker** then types the merged tree once — honoring the resolve pass's scope-id stamps
  on `Identifier`s (one resolution truth; it does not re-walk env for stamped names — `cap$N`
  nodes carry the captured binding's own stamp and resolve straight to it, the env records
  `expand` inserts serving diagnostics only) and re-verifying the substituted AST in
  each site's context (§5.2) — and computes the per-fragment arity/purity/closed tables. The kernel
  lowers the result and emits fragment heads; it never sees `splice` — expansion is complete before
  lowering, keeping the kernel's contract "assemble a statement before opening turn marks or
  choices" (`compiler.cpp:108-110`).

Diagnostics across the stage boundary keep attribution honest: `cap$N` renders in messages as the
captured *name* (`'cap$3'` is unactionable); site-context re-verification failures name the splice
site *and* the fragment's own `src:m:l:c+o:c` position — `cloneExpr` retains `loc` on every copied
node so the +offset mapping survives substitution, and nested literals chain offsets `+o:c` per
level; `learn`'s closedness refusal names the offending captures or name-refs; and template-body
errors report the call site with the `def`'s location noted (M13).

______________________________________________________________________

## 6. Runtime mutation

### 6.1 The `std:meta` package

`std:meta` is a second prelude in the `std:agent` style — Vortex and vagent source linked into
artifacts that `import std:meta`, with no C++ registration. It is not implicitly imported: only
programs that metaprogram pay its prelude cost (the import allowlist at `checker.cpp:2061-2067`
gains `std:meta` beside `std:agent`/`std:goal`, and the builtin member-signature table at
`checker.cpp:765-779` gains its entries — §10.1). Its members are called bare after
`import std:meta`, exactly as `std:agent`'s `text`/`intentions` are. Its adapter-supplied bindings
(V10's mechanism, minted with no marks outstanding): the program root cell, the agent's own cell,
the data-minted dimensions `d.code`, `d.patch`, `d.weight`, `d.synapse`, `d.entry`, `d.patch-head`,
`d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.mesh.relocation`, and the `validate` constants (the
`OpcodeKind::Linked` ceiling cell, the per-op arity table and the impure-kind set §7.2 requires —
adapter-bound data, not prelude
literals, because the enum's integers are serialized data that a later opcode would stale).

The fragment head layout (existing dimensions plus the data-minted `d.entry`, `d.refs`, `d.ref`,
`d.recipe`, `d.mesh.relocation`; `d.name` is `initGenesis`'s, `vortex_core.cpp:61`):

```text
fragment head H
  ├─(+d.entry)──► entry op cell (the fragment graph root)      -- head→entry, never +d.grab
  ├─(+d.values)─► descriptor record cell
  │                ├─(+d.vars)──► ["result"] ─(+d.values)─► the fragment's result CELL
  │                │                 (+d.name→"Int" on the same record = result type name)
  │                ├─(+d.vars)──► ["produced"]─(+d.values)─► the fragment's produced cell
  │                ├─(+d.vars)──► ["pure"]   ─(+d.values)─► true/false      -- checker purity flag
  │                ├─(+d.vars)──► ["closed"] ─(+d.values)─► true/false      -- closure flag (M11)
  │                ├─(+d.vars)──► ["arity"]  ─(+d.values)─► int             -- parameter count
  │                ├─(+d.vars)──► [param "w"] ─(+d.values)─► "Int"          -- parameter signature
  │                └─(+d.grab)──► [param cell]* chained +d.step, decl order -- arity/shape for validate
  ├─(+d.refs)───► record chain (+d.step) — UNIFIED schema, every record:
  │                  +d.ref→the slot cell genesis must rewrite  (+d.ref, never +d.values:
  │                  a later +d.values write could evict the marker — link eviction, §4.3)
  │                  +d.name→qualified name  ⇒ NAME-record (no marker, never re-kinded)
  │                  +d.name absent           ⇒ REF-record (full P16 ceremony)
  ├─(+d.recipe)─► reify recipe chain — per-field records (link tuples + literals
  │                + capture records), each field its own record cell          -- §6.1 reify
  ├─(+d.name)───► label cell "src:demo:12:4"  (or user label)
  └─ ref cells ─► every emitter-minted int64 ref cell whose +d.refs record
                   carries no +d.name (a ref-record) gets a per-ref-cell
                   d.mesh.relocation→"cell" marker edge  (P16, §6.6)
```

Each marked ref cell's *marker* is a freshly minted `"cell"`-texted cell **per ref cell** — sharing
is impossible: `link` gives each cell one `-d` neighbour, and a second ref's edge would evict the
first (the precedent mints per-reference inside its loop, `mesh_evaluator.cpp:735`). The marker is
inert graph structure; `isCellRelocationHandle` (`foreign_op_handle.cpp:98-117`) requires it plus
the `OpHandle` kind. The `+d.ref` record on `+d.refs` is the inventory the checkpoint ceremony walks
— both exist per marked cell because the marker alone is undiscoverable (no per-cell dimension
enumeration, U-M7) and the record alone is not relocation machinery. Name-records skip both halves:
no marker, no re-kind — `+d.name`'s presence is the discriminator the pre-pass reads.

`+d.grab` and `+d.spin` are deliberately *not* used on the head: those dims are the op cells' wing
convention (`bindInput`/`bindOutput`, `vortex_core.cpp:354-414`), and `link` evicts the occupied
same-dim neighbour — a `+d.grab` attach would sever an input wing or the previous attach. The head
is an indirection cell with no wings of its own, so graft links `clause +d.grab → H` land harmlessly
(H's `-d.grab`/`-d.spin`/`-d.values` backlinks are free — they are the install-occupancy markers of
M14). Execution reaches the graph through `+d.entry`: a reader of a clause's `+d.grab`/`+d.spin`
resolves the linked cell directly when it is an op, and follows `+d.entry` once when it is a head —
the attach convention the call ABI owes (§10.1), uniform for grafted and program-emitted heads
alike.

The package surface, split by purity exactly as vagent splits `believes`/`believe`. The read
group sits in the top half — `reflect` names the program-structure subset of it (`program` through
`label_of`), not the whole group and not a verb:

| `std:meta` member                                                                  | Purity                     | Type                                  | What it does                                                                                                                                                                                    |
| ---------------------------------------------------------------------------------- | -------------------------- | ------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `program(): Cell`                                                                  | pure                       | `Cell`                                | the program root cell the adapter bound                                                                                                                                                         |
| `agent_cell(): Cell`                                                               | pure                       | `Cell`                                | this agent's own cell — the `cells"…"` anchor for state ranks                                                                                                                                   |
| `module_named(path: Str): Opt<Cell>`                                               | pure                       | `Opt<Cell>`                           | module cell under the program/stdlib ranks                                                                                                                                                      |
| `symbol_named(owner: Str, name: Str): Opt<Cell>`                                   | pure                       | `Opt<Cell>`                           | a `d.vars` symbol cell (the spec §5.7 symbol table)                                                                                                                                             |
| `clauses_of(cell: Cell): [Cell]`                                                   | pure                       | `[Cell]`                              | the `+d.clause` rank — plans of a goal, handlers of an event type                                                                                                                               |
| `test_of(clause: Cell): Opt<Code<Cell>>`                                           | pure                       | `Opt<Code<Cell>>`                     | wraps the clause's `+d.grab` graph as a `Code` handle — wrapper memoized clause-side, keyed by the current attach cell (§6.1)                                                                    |
| `body_of(clause: Cell): Opt<Code<Cell>>`                                           | pure                       | `Opt<Code<Cell>>`                     | wraps the clause's `+d.spin` graph (same session-side memoization)                                                                                                                             |
| `plan_named(qualified: Str): Opt<Cell>`                                            | pure                       | `Opt<Cell>`                           | a clause cell by its qualified three-part `module:Agent.plan` name                                                                                                                              |
| `code_named(label: Str): Opt<Code<Cell>>`                                          | pure                       | `Opt<Code<Cell>>`                     | `d.code` rank lookup by `d.name` label — the durable re-resolution path (§6.7)                                                                                                                  |
| `label_of(c: Cell): Opt<Str>`                                                      | pure                       | `Opt<Str>`                            | reads a head's `+d.name`                                                                                                                                                                        |
| `validate(c: Cell): Bool`                                                          | pure                       | `Bool`                                | the well-formedness walk of §7.2 — per-op `maxCycles`-bounded, seen-set on `d.validate`                                                                                                         |
| `weight_of(cell: Cell): Opt<Float>`                                                | pure                       | `Opt<Float>`                          | a `d.weight`/`d.vars` scalar read (§8)                                                                                                                                                          |
| `value_of(c: Cell): any`                                                           | pure                       | `any`                                 | reads a `Cell`'s content — the `Code<Cell>` companion: `run` yields the produced cell, `value_of` reads its bits (§6.2)                                                                           |
| `run(c: Code<T>, args): T`                                                         | opaque-impure              | `T`; checker special form — `args` a list literal, per-element `any` | §6.2 — frame-per-activation call, `Return` tails only; a bare-`Code` actual yields `unknown`                                                                                                    |
| `reify(c: Code<T>): Code<T>`                                                       | impure                     | `Code<T>`                             | replays the head's `+d.recipe` — link tuples + literal contents + old→new map + regenerated `+d.refs` records and fresh per-ref-cell `d.mesh.relocation` markers + `+d.recipe` — minting a fresh copy, closed iff the source was (§6.1) |
| `graft_plan(goal: Str, name: Str, test: Code, body: Code<()>)`                     | impure                     | `()`                                  | mint a named clause cell, `+d.grab`→test head, `+d.spin`→body head, append to the goal's `d.clause`; test must be `pure`                                                                        |
| `graft_handler(event: Str, name: Str, guard: Code, body: Code<()>, priority: Int)` | impure                     | `()`                                  | same shape on the event type's `d.clause`, priority scalar on `d.vars`; guard must be `pure`                                                                                                    |
| `graft_where(clause: Cell, test: Code)`                                            | impure                     | `()`                                  | re-point the clause's `+d.grab` at the test's head; test must be `pure`                                                                                                                         |
| `graft_body(clause: Cell, body: Code<()>)`                                         | impure                     | `()`                                  | re-point the clause's `+d.spin` at the body's head                                                                                                                                              |
| `graft_method(owner: Str, name: Str, code: Code)`                                  | impure                     | `()`                                  | unlink any existing `owner.name` symbol (redefinition-as-replacement), then symbol cell on the module's `d.vars`, `+d.values`→head — callable by name; `code`'s effect-ness must match the host member (fragments are never effect members, so an effect host refuses) |
| `prune(clause: Cell)`                                                              | impure                     | `()`                                  | unlink a clause from its `d.clause` rank (predecessor→successor repair, the spec §5.1 removal idiom)                                                                                            |
| `learn_*` (each `graft_*` verb's durable twin)                                     | impure                     | `()`                                  | reify twice — graft one fresh copy, register a second fresh `reify` on `d.code` (the program-side original is never registered), record `d.patch`; closedness re-verified at accept (§6.6); **conditional on P16**                                                                           |
| `unlearn(clause_or_symbol: Cell)`                                                  | impure                     | `()`                                  | undo the latest patch whose target's resolved name equals the argument's: unlink a learned clause/symbol, or re-point via a `reify` of the named prior head — none recorded for program-original attachments; dead patches drop with a fault record (§6.6) |
| `set_weight(cell: Cell, w: Float)`                                                 | impure                     | `()`                                  | write the `d.weight` scalar through the focus slot                                                                                                                                              |
| `sprout_synapse(from: Str, to: Str, w: Float)`                                     | impure                     | `Cell`                                | mint a junction cell on the agent's `+d.synapse` rank naming the two qualified-name labels, `+d.weight`→its own `w` scalar (§8.1)                                                              |
| `next_rand(seed: Cell): Int`                                                       | pure                       | `Int`                                 | wrapping-safe mix over `Int` ops (32-bit halves via `DivMod`); the seed lives in a state cell (§8.6)                                                                                            |

Three notational points in that table. A bare `Code` parameter — `test`, `guard`, `code` — is a
*parameterized wildcard*: `Code<T>` for any `T`, a new form (not the existing `any`, which would
accept a `Cell` or `Str` too — the wildcard binds `T`, not the argument's kind); `Code<Cell>`
specifically is `run`-yielding-`Cell`, the honest type for a reflect-wrapped graph whose produced
value's type is not statically known. `Opt<…>` results play by vagent's rule: read in an expression
they yield the value or fail the statement, so `learn_where(plan_named("demo:Guard.cautious"),
test)` fails harmlessly when the name does not resolve. And every `Cell`-parameterized verb —
`graft_where`,
`prune`, `set_weight`, `test_of` — names its target through the **focus-slot idiom**: `Value`-read
the ref-box's `int64` referent, `Value`-write it into a fresh wing slot cell bound to the structural
op — because `Link`/`Break`/`Value`/`New` resolve their subject through `resolveCell`, which returns
the *operand cell itself* (`vortex_vm.cpp:50-67`); a bound ref-box resolves to the box, never the
referent, and `cells"…%"` cannot write through a `$` binding either (`compiler.cpp:484-486`). The
idiom is the plumbing that makes "the surgery is `Link`/`New`/`Value`/`Break`/`Splice`/`Linked` —
all present" true of the composition, not just the primitives.

`reify`'s mechanism is the **recipe**: the fragment emitter records on the head's `+d.recipe` chain
a reconstruction template — the `(from, dim, dir, to)` link tuples with endpoints encoded as
*links* (each record's `+d.values` points at the actual endpoint cells, never a census integer —
checkpoint renumbers dense ids, §4.5), the literal contents, and the capture records — and `reify`
replays it through `New`/`Link`/`Value`, building an old→new association that rewrites every copied
ref-box `int64` to the copy's cells and remaps every tuple endpoint through the same map.
**External captures are preserved verbatim** — the copy's capture links point at the *same*
definition-site cells the source's did, so the copy is `closed` iff the source was. `reify`
regenerates the copy's `+d.refs` (records remapped to the new ref cells, name-records re-linked)
and `+d.recipe` (a self-describing recipe for the copy, endpoints remapped), and mints the copy's
label `reify:<src>:<serial>` — registration refuses duplicate labels (§6.7). v1 replays the
*recorded emission template* — the recipe the emitter wrote — because the fragment graph's shape is
fixed at emission; copying the *current* graph (after in-arena mutation) is the U-M7 walk.
`Clone` is refused for this (§11): `Clone` appends to the source's `d.clone` rank
and shares content/value through `masterOf` (`vortex_vm.cpp:807-836`; `vortex_core.cpp:181,202`) — a
transclusion-instance that aliases the original, not a copy. A `reify` of non-literal graphs
(program-side `reflect` output) needs per-cell dimension enumeration that no opcode provides — U-M7.

A grafted plan alternative must carry a **name**, because intentions record plans by qualified name
(`vagent-agent-language.md` §5.7: `module:Agent.travel`, and `tried` is a name set). Grafted clauses
take `name` parameters, held on a `d.name` label — learned ones are conventionally named
`patch:<n>` — and
`graft_method`/`learn_plan` dedupe by name: an existing same-named symbol or clause is unlinked
first (redefinition-as-replacement, since the `d.vars` name walk is first-match-wins,
`vortex_core.cpp:100-127`).

Every read function is honest about what the graph can prove: `test_of` returns `Code<Cell>` because
a program-side test graph has no compiler-emitted descriptor — its produced value's type is not
statically known, so `run` on it yields a `Cell` handle to the produced cell. Upgrading that to
`Code<Int>` is a `validate`+re-descriptor step the caller requests, not an assumption.

### 6.2 `run`: a fragment is a same-cursor call

`run(c, args)` is a **checker special form** (M6): `args` is a list literal typed per-element by a
fresh `any` wildcard — heterogeneous argument lists are legal here and only here — with a
compile-time arity check against the fragment's signature when the fragment is statically known,
and a run-time descriptor arity check otherwise. It lowers to: read the head's `+d.entry` for the
entry op; evaluate each element in order and bind it on a fresh per-activation `#Call` frame *by
parameter name, as a value* (the frame writes of `vortex_vm.cpp:854-893`, the same mechanism method
calls use — values, not ref-boxes); push the frame whose successor is the post-`run` op; jump to
the entry op.
The fragment's tail `Return` pops the frame and resumes. After return, `produced` false makes `run`
fail like any expression; true yields `result` as `T` — the descriptor records the `result`/
`produced` *cells themselves* (§6.1), which need no `+d.refs` marks since they are reached by links,
not ints. `run` on `Code<()>` yields `()`, and the fragment still runs its body for effect. This is
the language spec's §5.8 convention verbatim — one cursor (V3), choice depth preserved, no second VM
— and it owes the call ABI that `vagent-agent-language.md` §11.1 already owes: `run` is a consumer
of it, not a new one. Two failure paths are kept distinct in the spec so the caller can tell them
apart: `produced` false is *soft* expression failure inside the current choice structure, while a
hard `Fail` in the fragment backtracks *through* the `run` into the caller's alternatives — the same
unwind a method failure causes. A fragment whose tail parks on `Halt` instead of `Return` is not
`run`-nable — `Halt` parks the cursor without popping the frame (`vortex_vm.cpp:1220-1225`), so
`run` refuses it at the descriptor/`validate` check; a `Code` value that needs `Halt` semantics
(a program `def` tail) is reflected, not `run`. `value_of(c: Cell): any` is the companion for
`Code<Cell>` results — `run` on a reflected graph yields a `Cell` handle to the produced cell, and
`value_of` reads its bits (the focus-slot read, §6.1).

Recursion through `run` is safe the way method recursion is: frames stack on `+d.stack`, each
activation gets fresh frame cells, and fragment `let` locals ride the same §5.8 frame discipline as
method locals (the ABI-debt class §5.3 names — per-activation, orphaned at `Return`), and the
fragment's static `result`/`produced` cells are spilled
per activation. The boundary is creation: the spill restores values, not links, so a fragment whose
graph contains `cells"…%"`-born `New` ops with statically bound outputs is single-shot — `New`
refuses an already-linked bound output (`vortex_vm.cpp:736-743`). `validate` therefore refuses
bound-output `New`s in `run`-reachable graphs and at *every* `graft_*` target — append and
non-append forms alike, since a grafted graph re-executes (§7.2); the alternatives — per-activation
dynamic mints, or link-restoring frames — are recorded in §11. A fragment that `run`s itself
directly or transitively pays a frame per level and dies at fuel, as any recursive plan does. `run`
inside `code"…"` is legal — named in M6's allow-list — so a fragment can invoke another `Code`
value at run time.

`run` is **opaque-impure** for compile-time purity: `pureBody` treats a `run` call as impure
regardless of what the fragment's descriptor claims, because the descriptor flag is forgeable —
a `run_checked`/`run_pure` variant that trusts a validated `pure` descriptor inside pure contexts is
the named future candidate (U-M10, §15). At run time the fragment executes whatever it executes; the
compile-time claim is just conservative.

### 6.3 `graft` and `prune`: typed attachment points

Four graft points, each with the signature the program side already uses. In every case the link
lands on a **head**, never an op cell — `link(clause, grab, POS, H)` writes `H -d.grab = clause`,
which is free on a wingless indirection cell, whereas pointing at the entry op would evict its
output wing (M14):

- **`d.clause` append** (`graft_plan`, `graft_handler`): the clause cell shape of the spec's §5.7 —
  test head on `+d.grab`, body head on `+d.spin`, priority on `d.vars` for handlers. The goal/event
  cell is found by name through `symbol_named`; the clause is appended at the rank tail (walk
  O(length)). A bare `goal`/`event` name is self-implicit — resolved against the agent's own
  module's `d.vars` (`symbol_named(owner, name)` taking the agent as owner); qualified
  `module:Agent.plan` spellings resolve cross-module the same way intentions do.
  `graft_plan`'s body/test signatures are the *goal's* formals — a plan fragment must
  declare (or be `Code<()>`-shaped against) the same parameter list *the goal's other clauses
  share, names included*, since selection binds the goal's arguments by name at every clause.
  `graft_handler`'s formals are the handler's bound names: the event cell under `occurrence`, or
  under the `as`-name when the target clause declares one; event fields are read through the bound
  event exactly as declared handlers read `occurrence.f`, never bound as formals — the same
  name-keyed rule `run` uses (§6.2). After a `learn_plan` on goal `G`,
  `G`'s plan list contains the new clause in selection order — a learned plan is a plan.
- **Body replace** (`graft_body`): re-point `+d.spin` at the new body head. The old body graph stays
  allocated (dead until reclamation); the swap is one trailed link.
- **`where`/guard replace** (`graft_where`): re-point `+d.grab` at the new test head. Applicability
  changes on the next evaluation.
- **Method symbol** (`graft_method`): unlink any existing `d.vars` symbol of the same name first —
  name walks are first-match (`firstOf(rankAfter(…))`, the pattern `findModule` uses at
  `vortex_core.cpp:100-106`), so appending would shadow forever — then mint a symbol cell named
  `owner.name` whose `+d.values` names the fragment head (`+d.entry` leads to the entry op, as every
  attach does). `owner` takes the same bare-or-qualified convention as the goal/event names above —
  bare resolves against the agent's own module's `d.vars`, `module:Agent` spellings cross-module.
  Capacity dispatch and `def` resolution find it by the spec's §5.3/§5.7 name walk, so
  a grafted method is callable by name immediately — and redefinition is replacement, not shadowing.

`graft_*` checks before linking: the fragment's `validate` must pass (§7.2 — which includes the
bound-output `New` refusal for every graft target); it must not already be installed (M14); its
descriptor signature must match the graft point's expected signature — an *ordered `(name,
type)`-pair match plus arity, result type and purity* (the names matter: binding is name-keyed,
§6.2) —
`graft_where`/`graft_handler` tests must be `pure` and nullary (or the clause's standard formals),
plan/handler bodies are `Code<()>` and may be impure, `graft_method` must match the declared
signature including effect-ness, and a pure-requiring graft refuses a reflect wrapper because its
descriptor is unknown-flagged (§6.1) — and for `learn_*` closedness is re-verified at accept time,
not trusted from the flag (§6.6). A `Code` argument whose head is already registered on `d.code`
is reified first and the copy installed — never grafted in place — so a registered head's links are
never re-pointed and the one-installation contract holds (§6.6). Mismatches fail the statement —
they are statement-level failures under V13, undoing only themselves.

`prune` repairs the rank: predecessor's link re-pointed to the successor, the clause cell left
allocated but unreachable. A pruned clause's fragment, if it was a `learn`ed patch's, stays
reachable through its `d.code` registration — which the patch names — until `unlearn` removes it.
A clause with an empty `+d.grab` (test-less after `unlearn` of a program-original test, before
genesis restores the skeleton's own attachment) is unconditionally applicable — spec §5.7's
test-less-clause rule — so the window is benign but visible to selection until the next genesis.

### 6.4 Mutation under execution and under marks

The ordering facts from §4.2–§4.3 combine into the execution model:

- **Ahead of the PC**: a re-pointed `+d.spin`/`+d.grab`/wing link is read at the next `step`'s
  advance (`vortex_vm.cpp:1333-1335`) — mutation takes effect immediately and deterministically,
  since the agent is single-threaded and nothing preempts between ops.
- **At the PC**: rewriting the currently-executing op's kind is forbidden by P15; rewriting its
  wings is legal but ineffective — the op read its inputs at step entry and will not re-read them
  until its next execution.
- **Behind the PC**: unlinking ops already executed is a no-op for this activation; their cells stay
  valid until the turn ends, because arena cells are freed only by `release` truncation — or by
  checkpoint rebuild (`release` itself never touches `cursorPC_`).
- **Under a mark**: every write is trailed; backtracking rewinds structure *and* PC (the explicit
  `setCursorOpcode` in `backtrack`, `vortex_vm.cpp:238-243` — the cell-bits restore alone would
  leave `cursorPC_` stale), so a failed speculative graft leaves the pre-mutation graph exactly
  restored. This is the property that makes §8.5's search cheap.
- **Contract chains**: `+d.contract`/`-d.contract` clauses on grafted fragments are ordinary cells
  evaluated at `step` (preconditions `vortex_vm.cpp:1229-1239`, postconditions `:1306-1322`) — a
  grafted method can carry `requires`/`ensures`, and a violated contract becomes the spec §6.7
  statement failure (cause `contract`), never a stop.

Two containment disciplines close the hazards the trail does not cover:

1. **The drain idiom, never a bare `release(m0)` under live marks.** `release(m)` restores trailed
   writes but leaves `cursorPC_` pointing at the post-mutation PC (§4.2 — the map is authoritative
   and untrailed), and releasing over outstanding inner marks leaves `choiceStack_` holding marks
   whose recorded extents exceed the truncated arena — the next `backtrack` would `resize` the
   trail/slots *upward* into ghost slots (`arena_manifold.cpp:1657-1698`; "marks are assumed well
   nested", `arena_manifold.hpp:773`). Containment is therefore
   `while (choiceDepth() > base && backtrack(cursor))` — the stdlib's drain
   (`vortex_stdlib.cpp:3800-3802`, `:3841-3843`) — which unwinds PC, stack top and marks in LIFO
   order; a `release(m0)` may run only when `m0` is the only outstanding mark
   (`choiceDepth() == 0` *and* `outstandingMarks() == 1` — `m0` itself, nothing else standing);
   symmetrically, `checkpointView` requires `outstandingMarks() == 0` outright
   (`mesh_actor.cpp:672-675`) — the `== 1` regime is `hydratePrefix`'s floor
   (`arena_manifold.cpp:2123`), a parallel-worker import artifact (`or_parallel.cpp:349`), not the
   actor path.
   A host caller that ignored this
   gets P17's problem (map/marks desync) for free — which is why P17 is deferred, not registered
   (§10.2).
1. **`consumeWork` exhaustion is a named fault, not an `ExecutionResult`.** Work-budget exhaustion
   `throw`s `std::runtime_error` (`arena_manifold.hpp:420-426`), escapes `step()` uncaught, and
   lands in the executor's `catch` as `ActorDown` (`mesh_actor.cpp:473-486`) — the adapter spec
   names this channel, since a malformed or looping mutation that burns its budget dies there, not
   in the soft-failure path.

The remaining hazard is *logical*: mutating the chain the turn is currently inside, and
int-addressed control flow. The rule is the same one the language already gives programmers —
statements are bounded (V13), so a graft mid-statement affects *subsequent* executions, and the
statement's own completion is unaffected unless it re-reads the mutated link. And pruning a chain
stops only *chain* advance: `Jump`/`Branch`/`Call`/`Choice` targets are int-encoded ref-boxes
(`vortex_vm.cpp:839-873`, `:1150-1168`) that reach unlinked ops as surely as linked ones, so
containment stays `maxCycles` + drain, as stated — there is no "the op is unreachable so it cannot
run" guarantee for int-addressed entry.

### 6.5 Kind immutability and P15

`getOpcodeKind` prefers `opcodeMap_` (`vortex_vm.cpp:103-105`); a `Value` write — or any non-
structural op's output writeback — into a *registered* op cell changes the arena record while the
map keeps the old kind. The trail does not cover the map. The rule and its enforcement:

1. **Runtime code never rewrites an op cell's content.** `std:meta` verbs only link and mint;
   `New`+`Value` mints fresh op cells, which decode through `asInt64` — and whose rewriting is
   *consistent* precisely because no map entry exists (their content is their kind). **Post-resume
   registration:** `opcodeMap_` is VM-private per `WorkerContext` and empty after `resume`; the
   artifact's own op cells are re-registered by the program load path re-running `mintOpcode` at
   genesis (the same `ensureDimension`/`symbolNamed` window). A kind rewrite of a *never-registered*
   cell is not refused — but it is also harmless-by-decode (content-is-kind), so the guard's
   coverage gap is exactly the set of cells where a desync cannot form.

1. **P15 makes the VM enforce it at the write funnel**: `VortexCore::writeValue`/`splice`
   (`vortex_core.cpp:196-254`) take a VM-installed kind predicate — the target is refused when
   `opcodeMap_.contains(masterOf(cell))` (master-aware, so a write through a clone that shares an
   op's content chain is refused too) — covering every site that restates a non-fresh cell's
   content: `executeOpcodeBody`'s `Value`/`Splice` cases (`vortex_vm.cpp:771-806`), `step()`'s
   memo-hit writeback (`:1269-1273`), `step()`'s generic output writeback (`:1287-1294`, which fires
   for every non-structural kind), and both `runPipeline` writes (`:337`, `:343-346`). A refusal is
   a typed `success=false` `ExecutionResult` surfaced per site — `Value`/`Splice` bodies and the
   writeback sites each return it, and the adapter classifies it like the numeric contract faults.
   Exempt-by-construction writes are named: `setCursorOpcode`'s direct writes (`:139-143`), `Call`'s
   frame writes — the *frame-cell carve-out*, per-activation `makeCell` output (`:854-893`) —
   and `mintOpcode`'s own `setContent` (`:73-82`) — plus every write to
   a cell absent from `opcodeMap_`, which keeps `New`+`Value` minting, `Bind`'s fresh `valCell`
   (`:904`) and `Linked`'s ref-boxes legal. Blast radius is deliberately-pathological writes only:
   registered op cells are reachable as write targets at all only through the surgery this document
   adds.
1. **The `opcodeMap_` staleness hazard stays out by construction**: `std:meta` mints op cells only
   through `New`+`Value` opcode execution — never through `vm.mintOpcode` — so the map's writers
   remain compile-time-only, preserving the `compiler.cpp:108-110` invariant that op registration
   happens with no marks outstanding. `arenaDims_` gets the same discipline: dimensions are minted
   only on the no-marks genesis path (§4.2). (A host caller that ignored either could still poison
   the maps; the rule is stated for C++ callers in §10.2's row.)

### 6.6 Durability: `learn`, `d.code` separation, and P16

**The `d.code` reachability invariant.** `d.code` is the agent's registration rank — the channel
through which a fragment head becomes checkpoint-reachable — and `promote` follows graph links in
*both* directions (`arena_manifold.cpp:1782-1783`). Therefore: **no cell on the agent's `d.code`
rank may carry an edge in either direction into a program-skeleton cell outside the fragment's own
graph.** One registration edge in the wrong place drags the program skeleton into every checkpoint —
the V12 failure with the roles reversed. The disciplines:

- `run` and the ordinary `graft_*` verbs **never register** — they install a head and use it
  session-side; nothing on `d.code` means nothing reaches program cells through the rank.
- `learn_*` **never registers the original.** A `code"…"` head is a program-side artifact cell —
  the literal's emission anchors it into the compiled artifact's cell space by a `+d.values` edge
  from its emission record (the same symbol→head convention `graft_method` uses), so it is
  reachable from the program root — and registering it directly would put a program-side cell on
  `d.code` and violate the invariant. Registration is always a *fresh,
  uninstalled `reify`*: `learn` mints **two** reified copies — one for the `d.code`-registered head,
  one for the installed attachment — and the argument head is left untouched. *Sideness test:*
  "program-side" means *both* reachable from `program()` by a bounded walk over the program's
  **full structural dim vocabulary** — `d.stdlib`, `d.vars`, `d.clause`, `+d.values`, `+d.code`,
  `+d.entry`, `+d.name`, `d.name`, `+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`, `+d.step`,
  `d.contract`, `d.belief`, `d.source`; every dim program structure can hang on is in the walk,
  and the bound is at least skeleton depth — *and* minted within the program's own mint window:
  `birthOp` at or below the program-image boundary the genesis `programRef` bind marks
  (`birthOp` is recorded per slot, `arena_manifold.cpp:1183-1186`, and consulted by the relocation
  path, `mesh_value.cpp:399`). Grafted and learned runtime cells minted after that boundary exceed
  the window and stay classifiable — a `learn`ed clause sitting on a program `d.clause` rank does
  not become "program-side" and forfeit the durable path. "State-side" is link-reachable *from the
  agent cell* and *not* program-side; `closed`'s state-side arm uses the same test. The walk is
  link-bounded: a program cell reachable only through int64 content — `Jump`/`Call`/`Branch`/
  `Choice` control-flow targets — is invisible to it, and an outside edge to one is a §11-door
  limitation, not claimed soundness (the `closed` frontier below names the same door). A
  `code"…"` argument passes the test and takes the two-copy path; a fragment already detached (a
  session `reify` result) is state-side but still gets a fresh registered copy — the invariant is
  uniform: **never the original** (the one carve-out: an argument that already *is* the registered
  head, next sentence).
  `learn_*` of an already-installed head is refused exactly like `graft_*`'s second-`graft_*` rule
  (M14) — an installed head is consumed. Re-`learn`ing an already-registered head is the carve-out:
  the argument is reused as its own registration — the second `reify` is skipped and
  `insertIntoRank` no-ops on the already-present rank entry (`arena_manifold.hpp:624-628`) — so a
  second learn of a `code_named`-resolved head re-installs a copy and re-records a patch
  without duplicating the rank entry. Only that arg-form dedupes: re-`learn`ing the same
  *literal* mints a second registered copy with a fresh `reify:` serial, a second rank entry and
  a second patch — correct EDL behaviour.
- Grafting a `d.code`-resident head is never done in place: the graft verbs **reify first** (§6.3)
  rather than refuse, because mutating a registered head's links would break the one-installation
  contract *and* could add an edge the invariant forbids.
- Reflect wrappers (the heads `test_of`/`body_of` mint) are **session-side** — never on `d.code` —
  and carry `+d.entry → the wrapped graph's root op` like every head (M8); the head convention is
  uniform, and a wrapper that did *not* link `+d.entry` could not satisfy `run`'s entry lookup at
  all. Their memoization lives clause-side (M8's `clause +d.code → +d.step` record chain — records
  `{+d.values → wrapper, +d.name → attach key}`, `+d.step`-linked) so they stay program-adjacent,
  never registered, and invalidate when the clause's attach cell changes. Wrapper descriptors are
  unknown-flagged — `pure`/`closed` unset — so a pure-requiring graft refuses them; body/symbol
  grafts may install one under the narrowed rules of §6.3 (validate still runs). A wrapper is not
  `run`-nable if the wrapped graph's tail is `Halt` (§6.2).
- `unlearn` removes the head's `d.code` registration edge when the last patch naming it is undone;
  dead patches are dropped after recording a `ConditionFault`-style fault.

`learn_plan(goal, name, test, body)` (and its twins) does the trailed writes: reify each fragment
twice (registered head, installed copy), graft the installed copy, register the registered head on
`d.code`, append a patch cell to the agent's `d.patch` rank. The patch carries `{ordinal, target
descriptor record, +d.patch-head→record chain, +d.name→label, priority for learn_handler}` —
`d.patch-head` is a dedicated first-edge dimension rather than reusing `+d.step` or `+d.values` at
the patch, matching the record-chain convention this document uses throughout — a chain's first
edge and its step edges are different dims (`+d.refs`/`+d.step`, `+d.recipe`/`+d.step`, the memo
chain's `+d.code`/`+d.step`); the agent's top-level ranks themselves — `d.code`, `d.patch`,
`d.synapse`, `d.clause` — are *same-dim* chains per `insertIntoRank`/`rankTail`
(`arena_manifold.cpp:1423-1439`, the `addClause` precedent `vortex_stdlib.cpp:4238-4241`) — and
avoiding an occupancy collision — the patch's `+d.values` already
points at the descriptor record, so a `+d.values` first edge would evict it. `learn_handler`'s
patch record carries `priority`, since handler selection orders clauses by it and a replayed graft
must restore the same ordering. The chain is `+d.step`-linked record cells, each
`+d.values`-pointing at a fresh cell holding one **registered** head's `+d.name` label — the
pristine `d.code`-resident original the patch names, never the installed copy (`learn_plan` records
registered test head then registered body head, followed by any recorded prior head's name). Heads
are recorded **by name, never by link**: a head is referenced by up to two patches (installed on
one, prior on a later replace), and a second `+d.*` link to the head cell would sever the first
record by eviction; `unlearn` and genesis replay resolve each name through `code_named`/
`plan_named` (§6.7). An `unlearn` re-point never installs the named head in place — it installs a
`reify` of the named prior head, per the same never-graft-a-registered-head rule (§6.3). The
target descriptor records the target's **resolved qualified name** — `<module>:<Agent>.<member>`,
the three-part name intentions use — read off the clause's `d.name` label through the
`plan_named` resolution path; a `learn_*`
whose target has no resolvable name (an anonymous grafted clause) is refused, because a patch that
cannot be re-found at genesis is a patch that lies. `unlearn(clause_or_symbol)` *matches* by the
patch target's resolved name: the argument's symbol/clause cell resolves to its qualified name, and
the latest patch whose recorded target name equals it is undone.

**`closed`, correctly.** A fragment is `closed` iff **no program-side reference lacks a
`+d.refs` name-record**: every reference out of its own graph is either (a) a link to a *state-side*
cell — a capture or `cells"…"` path resolved at emission, where state-side means link-reachable
*from the agent cell* and not program-side under the sideness test above — or (b) a *name-record*
on `+d.refs`: `+d.ref → the operand's slot
cell` plus `+d.name → the callee's qualified name`. The user-level contract follows: a learnable
fragment is *self-contained* — literals, its own `code` parameters instantiated, `believes`/
`findall` reads, `cells"##…"`/`$` paths onto state-side cells, captures of *fields and module vars*
— plus program calls bound by name. **Capturing a `let` local or a parameter makes a fragment
un-learnable** — those cells are frame-bound (§5.3), orphaned per activation, and neither
state-side nor name-recordable, so the `learn` refusal names them (§5.6's diagnostics). A capture
whose declared type is or *contains* `Cell`/`Code` (`any`, `Opt`, lists, records) — or whose cell's
content is a ref-box int naming a program-side cell — is likewise non-`closed`: a session handle
cannot be durable, so a fragment
closed over one is refused unless it is name-resolved (`d.name`/`code_named`) at emission. The
closed walk is bounded by the *enumerable frontier* — `+d.refs` records, capture edges on the
emission's wing/record dims, and the head's own structural dims; an outside edge smuggled onto a
non-op-graph dimension (`dimensionsOf` being C++-only, U-M7) evades it — as does an edge to a
program cell reachable only through int64 content (control-flow targets are ints, invisible to the
link walk, §6.4) — and both are named §11-door
limitations, not soundness holes to claim closed. `learn` **re-verifies** closedness at accept
time — it walks those records and links checking each endpoint is state-side or a name-record,
because an emission flag can be forged and a `reflect`-spliced fragment can smuggle program
structure — and `run` targets inside a learnable fragment must be *name-resolved* (a `run` of an
opaque session `Code` handle is not durable either — the emitter name-records every
statically-known `run` operand, so an opaque `run` target in a learnable fragment is a closedness
refusal at emission). A non-`closed` fragment — e.g., a
`reflect`-wrapped program-side graph — is refused, because linking it from `d.patch` would drag the
program skeleton into every checkpoint. Such graphs can be `graft`ed for the session; making them
durable requires `reify` into a closed copy first — and `reify` of non-literal graphs is deferred
(§15), so the honest v1 rule is: **`learn` accepts `code"…"`-born fragments and their `reify`
descendants only.** The `closed` flag's provenance is the adapter's: `CompilerBinding` gains a
state-side marker bit (§10.1) so emission knows which captured bindings name checkpointed cells.

**Name-bound calls are ordinary `Call`s with an int64 operand — model (b), no new execution
machinery.** A fragment's call to a program symbol emits the same `Call` op every call emits: the
operand cell is an emitter-minted int64 slot holding `int64(calleeEntryOp)` at emission
(`vortex_vm.cpp:854-857` executes `toInt(inputs[0])`), so in-session the call executes the
emission-time binding directly. What makes it *durable* is the `+d.refs` name-record alongside:
`+d.ref → that very slot cell` plus `+d.name → the callee's qualified name`. **`+d.name` present is
the discriminator**: a record with `+d.name` is a *name-record* — it gets **no** `d.mesh.relocation`
marker and is **never** re-kinded `OpHandle`, because a program-cell target must never reach
`promote`'s relocation path (which enqueues `handleTarget` into `seen`/`frontier` —
`arena_manifold.cpp:1759-1764` — and gates on it at `:1797-1809`, either dragging the program
skeleton in or returning `nullopt` when the target is
excluded). A record without `+d.name` is an ordinary ref-record and gets the full P16 marker +
`OpHandle` + rehydrate ceremony. At genesis, name-records rebind: resolve `+d.name` — a qualified
name — through the §6.7 split (`module_named` on the module qualifier, `symbol_named(owner, name)`
on the rest) → the symbol's `+d.values` → **apply §6.1's head/op discriminator at the hop**: the
resolved cell is the entry op when it is an op (`exportSymbol` binds `sym +d.values → entryOp`
directly, `vortex_core.cpp:119-127`), else follow its `+d.entry` once (a head) →
`writeValue(slot, int64(entryOp))` on the `+d.ref`'d slot. The same op-or-head hop applies wherever
a name-record chain lands on a symbol, genesis and patch replay alike. A `run` target's name-record
rebinds differently at the first step only: its `+d.name` is the target head's *label*, resolved
through `code_named` → head `+d.entry` → `writeValue(slot)`. An unresolved name faults/drops the
patch like a dead `d.patch` record — genesis does not install a call it cannot bind. **Divergence
clause:** a session `run` retains the *emission-time* binding, while a resumed genesis rebinds
through the durable name and therefore follows *post-resume* redefinitions — a `graft_method`
re-point between checkpoint and resume is picked up by genesis's rewrite, not by the session copy.
§9.3's `learn_plan` works by exactly this design: the `Call` to `navigate_to` inside the learned
body `code"{ navigate_to(\"water\") }"` is a name-recorded `Call` — `+d.name →` the *callee's*
qualified name `"demo:Forager.navigate_to"`, while `"water"` is just the argument — rebound at
genesis to whatever `navigate_to` then names, which is why the
learned clause survives where an int64 callee id would not.

**P16 is the checkpoint ceremony, adapter-side — no new core mechanism.** `checkpointView` yields a
view over `const ArenaManifold&` (`mesh_actor.cpp:668-694`, `mesh_actor.hpp:102-105`) and
`publishValue` takes `const ArenaManifold&` (`mesh_value.hpp:69-74`), so the re-kind/restore cannot
run inside them; the mutable seam is the adapter's **own `Executor` callback inside `pumpOne`**
(`mesh_actor.cpp:473-477` invokes `executor_(…, *actor->context)` around each turn, and
`WorkerContext` hands the callback the mutable arena, `worker_context.hpp:63`). The ceremony is
flag-driven across the checkpoint-terminating turn:

1. **At emission**, every emitter-minted int64 ref cell **whose `+d.refs` record carries no
   `+d.name`** — the `+d.name` discriminator above governs both halves of the ceremony: a
   name-bound `Call` operand is still an emitter-minted int64 ref cell, but its record carries
   `+d.name`, so it gets the record and no marker — that is, both wing slots, `reference()` cells,
   and dimension-constant slots (`compiler.cpp:769-772`'s `dimConstant` emission mints ref cells
   too — the uniform rule is *every* `makeScalarCell(int64(ref))` the emitter writes) — gets **its
   own** inert `d.mesh.relocation→"cell"` marker edge plus a `+d.ref` record on the head's `+d.refs`
   rank: one freshly minted marker cell per ref cell, as `mesh_evaluator.cpp:735` does inside its
   per-reference loop — a shared marker cannot exist, since `link` gives each cell one `-d`
   neighbour and a second ref's edge would evict the first (`arena_manifold.cpp:1352-1373`). The
   mark dimension is `d.ref`/`d.mesh.relocation`, not `+d.values`, because `+d.values` has
   occupancy semantics — a later descriptor write could evict a `+d.values` marker. Runtime-minted
   reference cells are the carve-out: `Linked` rebuilds its rank per execution, so a `Linked`-born
   ref box is re-minted, not marked.
1. **Request.** The adapter sets its relocate-before-checkpoint flag when the next turn is the one
   whose end calls `publishValue` — and only when `pending(pid) == 1`, so a terminating turn exists
   whose end leaves the mailbox empty (`pending` exposes `mailbox.size()`,
   `mesh_actor.cpp:659-662`; `pumpOne` skips empty-mailbox actors, `:462`, and `checkpointView`
   refuses a non-empty one, `:672`). A checkpoint decision that lands on an empty mailbox instead
   mints a P4 `wake` turn so the flag has a window. The flag is two-phase — *re-kind pending →
   restore pending → drained* — and the callback owns it: cleared on `publishValue` failure or
   refusal, and on `ActorDown` (a stopped actor never re-enters `pumpOne`, so no restore is owed
   there either).
1. **End of that turn, before `publishValue`**, the `Executor` callback — on the mutable arena —
   walks each registered head's `+d.refs` ref-records and `setValueBits(OpHandle, target)`s every
   marked ref cell, skipping any already in `OpHandle` kind (idempotent; the kind check is cheap and
   the walk is `+d.refs`-bounded). The VM **never executes** `OpHandle`-kind cells — the re-kind
   happens at turn end when no op can run, and `isCellRelocationHandle`
   (`foreign_op_handle.cpp:98-117`) requires kind *and* marker edge, so no stray `OpHandle` write is
   a handle, and no `MeshOp` kind is checkpointed. The walk's scope is honest: only *registered*
   heads carry the durable path — an installed-but-unlearned graft's ref cells are never re-kinded
   (`run`/`graft_*` never register), so an unlearned graft may structurally survive a checkpoint
   with stale calls; `learn_*` is what makes a graft's references durable.
1. **`publishValue` executes between turns on `const&`** — `promote` relocates the handles through
   the existing path (`arena_manifold.cpp:1759-1764`, `:1934-1954`).
1. **Head of the *next* turn** the callback restores `setValueBits(Int64, *handleTarget)` on every
   marked cell **still in `OpHandle` kind** — the kind-guard makes an unmatched or repeated restore
   a no-op — on every exit path including publish refusal/failure and a `consumeWork` throw
   (the callback owns the flag and drains it before resuming normal turns). Each of the three
   passes — re-kind, restore, genesis rehydrate — skips cells already in its target kind before
   reading `asInt64`/`handleTarget`. A marked ref whose
   target resolves to a base-manifold (non-ephemeral) cell is acknowledged: `promote`'s gate
   excludes it, and restore writes its Int64 back unchanged — the round-trip test
   *learn → checkpoint → resume → learn → checkpoint* is the acceptance fixture.

At genesis, ordering is fixed: (a) arena overlay; (b) `ensureDimension` for every data-minted
dimension (no marks outstanding); (c) **re-`import` the prelude and re-point `opcodeMap_`** — the
program-image artifact's own op map is what `code` re-emission and `opcode_named` consult, and the
overlay carries stale indices into it; (d) bind `programRef → the program root` by the V10
mechanism — the artifact binds its own program artifact so `code_named`/patch name resolution has
a root to walk; (e) `+d.refs` rehydration — per ref-record `Int64(*handleTarget)`, per name-record
the `symbol_named → +d.values → head/op discriminator → writeValue(slot, int64(entryOp))` chain
above — because `resume` overlays the received arena without `importValue`'s remapping
(`mesh_actor.cpp:260-271`) — the two paths differ in kind, not just order: `importValue` converts
an `OpHandle`-kind cell itself — `CellImport{OpHandle}` becomes `Cell` with the promoted target
(`arena_manifold.cpp:1934-1954`) — while `resume`/`genesis` receives the `OpHandle`-kind cells
verbatim — handles carrying pre-promotion targets — and rehydration `writeValue(slot, Int64)`s each
marked cell back with the promoted target;
(f) `d.patch` replay in ordinal order (replay does not re-record — replayed grafts mint no patch
cells); (g) turns.

Two discipline rules keep the checkpoint boundary honest:

- **Replaces of program-original attachments record no prior head.** A `learn_where` on a
  kernel-emitted clause cannot keep the prior attachment on the patch — that edge would be
  state→program-skeleton, exactly the link V12 exists to exclude. The patch records the new head
  only; `unlearn` of such a patch detaches it, and genesis restores the program's own attachment for
  free because `programRef` reloads the skeleton. (Unlearning a *previously learned* replace does
  record a prior head — a closed fragment is safe to link.)
- **Replay is monotonic but not cumulative-garbage.** Genesis walks `d.patch` in ordinal order and
  re-runs each graft; a patch whose target no longer resolves (renamed plan, pruned clause) is
  dropped after recording a `ConditionFault`-style fault — compaction, not a ceiling — so dead
  patches are not re-skipped forever. And the patch's source field is a *name* (`src:m:l:c` or the
  user's `#label`) that requires an external source map — the artifact does not seal the literal
  body, so the label is provenance by naming, not transclusion (§12 disclaims recoverability).

### 6.7 Session handles and durable identity

`Cell`/`Code` values are session handles — §4.5's table is the honest accounting, and it cuts both
ways: a handle may be *out of range* after `resume` (fails `contains`) or *in range* (silently
aliases a wrong local cell — the same hazard a foreign `Code`/`Cell` received in a message poses,
since `resolveCell`'s check is dense-range only). The durable layer on top:

- `d.code` (agent → fragment heads): registration makes a fragment checkpoint-reachable. Only
  `learn_*` registers — a fresh, uninstalled `reify`, never the program-side `code"…"` original and
  never the installed copy (§6.6's invariant) — `run`/`graft_*` never do, and `code"…"`-born heads
  stay session handles until learned. Registration refuses a second head carrying an existing
  `d.name` label — the duplicate-label refusal `reify`'s `reify:` serial exists to avoid (§6.1):
  labels are the re-resolution key, so a duplicate would make `code_named` ambiguous.
- `d.name` labels: `code"…"` heads carry an automatic `src:<module>:<line>:<col>` label; a
  `code"#label …"` inner-grammar prefix overrides it, and `reify` labels `reify:<serial>` — all
  three are *opaque label strings*, matched verbatim; the *user* `#label` alone cannot spell `:`
  or `"` (identifier-legality rule, §6.6's source field). First-`:` splitting applies only to
  qualified-name lookups — `plan_named`, `symbol_named`, patch target names: `demo:` is treated
  as a module qualifier only when it resolves as a module on `programRef`'s walk, otherwise the
  whole string is the name — never to `code_named`, where `patch:near_water` is just a label.
  `reify`'s `<serial>` comes from a *durable per-agent
  counter* on `d.vars`, not a per-incarnation one, so reified labels never collide with a promoted
  registration's after `resume`. `code_named(label)` resolves label → live head per incarnation —
  the recommended durable reference, and the required one for handles received from another agent:
  a received `Code`/`Cell` is opaque data until re-resolved by label, never a ref-box to
  `run`/`graft`/`cells` against directly. `code_named` walks `d.code` only: the program-image twin
  a learned literal leaves behind is unreachable by construction — a `code"…"` literal is never
  `d.code`-registered — so no rank-order tie-break is ever needed.
- `symbol_named(owner, name)` / `plan_named` resolve qualified names — the grammar is
  `[<module> ':'] [<Agent> '.'] <member>`: the first `:` separates a module qualifier (treated as a
  module only when it resolves), the remaining dotted prefix names the owning `Agent`'s `d.vars`
  cell, the last segment the member name — walked verbatim on `d.name`/`d.vars` labels.
- `d.patch` (agent → patch records): the mutation EDL of §6.6.

The rule for programmers mirrors `Cell`'s existing caveat: **`Code`/`Cell` in a `var` field is a
session handle — it works within one incarnation, and across `resume` you re-resolve by label.**
`learn`'s links are structural and suffer nothing. This is deliberately the same honesty vagent
already applies to intention ids (scalar, not refs) and plan names (qualified names, not
`CellRef`s).

______________________________________________________________________

## 7. Type safety, honestly priced

### 7.1 What the checker proves

At compile time, the static guarantees are complete, because expansion finishes before lowering:

- A `code"…"` body is fully type-checked in its definition-site environment — names, types,
  positions, purity — by the same checker as ordinary code.
- `splice` fits only a fragment whose `T` matches the position; parameterized splices check
  arguments against the signature *and* re-verify the substituted body's position/purity obligations
  in the site's context (§5.2 — a fragment legal in a handler body is not thereby legal in a
  `where`). A `Code<Int>` cannot splice into a `Str` slot — ever.
- `splice` never runs: it is eliminated in `expand`, so no expansion happens at run time and no
  ill-typed graph can be minted by expansion; a residual `splice` reaching the kernel is a compile
  error, not a runtime event.
- `static` evaluates only what purity and closedness make deterministic — and only what the
  literal-expressible result types can carry back.
- Suspension positions, the `Bool` rule, `:=`/`bounded` positions, read-only answers — all apply
  inside `code"…"` unchanged, because the checker treats the body as ordinary code.

### 7.2 What the runtime checks

Runtime mutation cannot re-prove typing — the VM executes cells — so `std:meta` checks:

- **Descriptor comparison**: `graft_*` and `run` compare the fragment's descriptor — ordered
  `(name, type)` parameter pairs, arity, result type name, `pure`, `closed`, and `effect`-ness for
  `graft_method` — against the call or graft signature. The names are compared because binding is
  name-keyed: a `(x: Int)` fragment onto `def f(a: Int)` passes arity+type but reads an unbound
  frame slot, so names are part of the match — arity included, so a parameterized fragment cannot be
  installed where plan selection has no
  argument to supply. Type names are *qualified* `module:Name` strings plus the arity chain, in the
  same normal form `validateType` produces — surface text would conflate same-named types across
  modules. O(descriptor) per call — type names as cells, not proofs, and the comparison is a
  `std:meta` routine in Vortex.
- **`validate(c)`**: a bounded walk over the fragment subgraph verifying: every op cell's `int64`
  decodes within `[0, Linked]` — against the adapter-bound ceiling cell, not a prelude constant,
  since the enum is serialized data (`vortex_vm.hpp:27-93`); `+d.spin` chains terminate at `Return`
  for `run`-reachable graphs (a `Halt` tail would park the cursor on the frame it never pops,
  `vortex_vm.cpp:1220-1225`; `Halt` is correct only for cursor-entry graphs); `Jump`/`Call`/
  `Choice`/`Branch` targets resolve to op cells (`Choice` itself enforces this at run time,
  `vortex_vm.cpp:1156-1158`); no bound-output `New` in `run`-reachable graphs or at any graft target
  (§6.2); per-op wing arity matches the adapter-bound arity table (§6.1's constants — the named
  home); and no descriptor-flagged-`pure` fragment contains an op from the adapter-bound impure-kind
  set. Traversal is restricted to the op-graph dimensions (`+d.spin`, wings, `+d.step`, `+d.entry`)
  and known op references — it never walks arbitrary head or program backlinks. The seen-set is
  per-visited-cell **tag cells** on the adapter-minted `d.validate` dim — `visited +d.validate →
  tag_i`, a *fresh `Int64` scalar cell minted per visited cell per pass* holding the pass serial —
  the serial itself drawn from the durable per-agent `d.vars` counter, the same source `reify`'s
  `<serial>` uses (§6.7); membership is the edge existing **and** `value_of(tag) == serial`. Both
  simpler schemes are mechanically wrong: a shared marker cell can hold only one `-d.validate`
  neighbour, so the second visited cell's edge evicts the first's; and direction-reversed marks
  suffer the same eviction on the visited cell's single `+d` slot. Tag-per-cell survives eviction
  harmlessly — re-marking a visited cell replaces its stale tag, and any stale tag from an earlier
  pass fails the serial comparison. Marking is O(1), membership O(1), at the price of one tag-cell
  mint per visited cell per pass (priced at §13). The tag links are trailed, non-semantic writes
  inside a pure builtin — they live on `d.validate`, touch no op-graph dim, and backtracking removes
  them like any write. `validate` is pure and costs the per-op `maxCycles` budget of its own
  execution plus O(visited) tag mints and O(n) membership tests where a test is needed
  (`Jump`/`Call` target graphs); linear `+d.spin`/`+d.step` chains need only a step counter.
  `graft` runs it on every installation.
- **The arena's own guarantees**: `link` maintains reciprocal edges (`arena_manifold.hpp:594-618`),
  so a graft cannot produce a one-sided chain; `New`'s already-used refusal
  (`vortex_vm.cpp:736-743`) catches double-installation of a bound output; and the head-indirection
  convention (M14) turns what would be silent evictions into refused double-installs.

### 7.3 What remains unproved

- **Behavioural correctness**: a grafted plan can be wrong — that is the agent's problem,
  containable by the trail (undo) and fuel (cycle bound), and is the premise §8's search loop relies
  on: bad candidates are drained, not prevented.
- **Memoization freshness**: a mutated graph that was memoized serves stale entries. The rule is a
  documented one — the fragment machinery never calls `enableMemoization`, and `d.cache` on mutable
  graphs is programmer error, not a checked invariant.
- **Stale and foreign handles**: a session `Code`/`Cell` may alias after `resume` (§4.5) — and a
  foreign one may alias *now*, since `resolveCell` is a dense-range check — mitigated by the
  `d.code`/`d.name` re-resolution rule, not eliminated for stored fields or received values.
- **The open-questions apparatus**: this list and §15 *are* this document's apparatus — a deviation
  from the house open-questions table, recorded rather than hidden: genuine open items (stale
  foreign handles, memoization freshness, U-M7's foreign-graph `reify`, P17's deferred map/marks
  resync) live here and in §15's deferrals, each with its stated direction.

______________________________________________________________________

## 8. The neuro-network plasticity path

"Plasticity" here means mutating the agent's **behaviour structure** — plan bodies, clause ranks,
handler sets, link topology — not merely learned scalar state. The claim of this section is narrow
and verifiable: the machinery of §§5–6 is sufficient substrate for weights, synapses, Hebbian cycles
and gradient-free program search, using only cells, ranks and the trail. What it does not yet buy is
stated as deferrals.

### 8.1 Weights and synapses are cells and ranks

- A **weight** is a `Float` scalar carried as a cell's *bits*, never an edge: `d.weight` is a
  data-minted dimension whose `+d.weight` link on a junction `J` points at the scalar cell holding
  the `Float` — so "junction `d.weight`" means `J +d.weight → FloatCell`, and `d.vars` is the same
  record convention on the agent (`cell +d.vars → [key] +d.values → scalar`, the spec §5.7 shape).
  Weights live only on a junction's `d.weight` or on `d.vars` records of the *agent* —
  never on op cells or program structure, and **never directly on a clause cell**: clauses are
  program-side, so a state-side `d.weight`/`d.vars` edge *to* one is the V12 edge. A
  clause-correlated weight is a record on the agent's own `d.vars` keyed by the clause's qualified
  `d.name` string: `set_weight`/`weight_of` resolve `clause → +d.name → qualified string` then
  read/write the agent-`d.vars` record carrying that name (the same name `learn`'s patch records
  and `plan_named` resolves). The write is an ordinary trailed scalar write through the focus slot
  (§6.1) — `Value`-read the ref-box's referent int, `Value`-write the new `Float` into the named
  cell's slot.
- A **synapse** is a *junction cell* on the agent's `+d.synapse` rank — not a bare `A → B` link,
  because a dim gives each cell exactly one `+d` and one `-d` neighbour (`link` evicts,
  `arena_manifold.cpp:1347-1377`), so endpoint-to-endpoint edges can express only a star. The
  junction `J` carries endpoint labels as qualified-name *strings* on its descriptor (`from`, `to` —
  content, not links, so labels are free of the eviction problem), plus its **own** `+d.weight`
  scalar link — each junction owns a distinct `FloatCell` by mechanics, not merely convention:
  `link` gives `W` one `-d.weight` slot, so a second junction's `+d.weight → W` edge would evict
  the first's — and a shared scalar would couple unrelated updates into one write besides. The
  endpoint domain is **qualified names of things a name can resolve to**: a plan
  (`module:Agent.plan`, the `plan_named` form), a clause label, a fragment head label, or a
  synapse junction's own label — anything `symbol_named`/`code_named`/`plan_named` can resolve at
  rehydration; raw cell ids are not legal endpoints (session handles, §6.7). Fan-in is N junctions
  naming the same `to` label; fan-out likewise. `sprout_synapse(from, to, w)` mints one; pruning
  unlinks it.
- **Session vs durable**: the junction, its labels and its weight are state-side — reachable from
  the agent cell, so they promote and survive `resume`. What is *not* durable is the endpoint cell a
  label resolves to this incarnation (program cells die at `resume`; session handles alias) — a
  synapse naming a program-side clause is exactly as durable as that clause's qualified name.
  Weights therefore live state-side from the start; `d.synapse` itself is adapter-minted data (V10),
  no `SystemDimensions` field, no core change.

### 8.2 The plan library is the synaptic structure

The mapping the design offers: a goal's `d.clause` rank is a bank of candidate rules; each clause's
`where` test is the rule's condition; its `+d.spin` body is the action; a weight record naming the
clause is its strength. Plan selection reads this bank in declaration order
(`vagent-agent-language.md` §6.3); a `learn`ed clause is appended at the tail. Growing a rule is
`learn_plan`; strengthening is `set_weight`; pruning is `prune`/`unlearn`. This is *structural*
plasticity of the behaviour repertoire — the thing weights-as-state alone cannot express. One
honesty bound: clause tests (`where`/guard fragments) must be `pure` (§6.3), so the condition
structure can grow and re-point but cannot itself act.

### 8.3 Hebbian cycles on the proposed surface

Percepts already flow in as host deliveries and become beliefs through handlers
(`vagent-agent-language.md` §7.5). A Hebbian rule — "fire together, wire together" — is:

```vagent
import std:meta

// event PerceptPair declared per §7.5's handler convention
on PerceptPair as p {
  let lr := 0.1
  let a := agent_cell()                       // this agent's cell, for the $ anchor
  for s in cells"$a/+d.synapse" do {
    // reinforce associations whose pattern matched this percept
    let w := weight_of(s)
    if [w] then set_weight(s, w + lr)          // [w] is the Opt presence test
  }
}
```

All scalar writes, all trailed, all inside one turn. The synapse rank is an ordinary rank of
junction cells; the walk is `Linked`; the update is the focus-slot `Value` sequence on a state-side
scalar — `s` is a `Cell` handle, and `set_weight` writes *through* it per §6.1's idiom, not to the
box.

### 8.4 Structural plasticity

Growing and pruning *rules*, not just weights:

- A handler that notices a recurring percept pattern `learn`s a new clause into a goal's rank — the
  agent literally grows a plan.
- `graft_where` tightens or loosens an existing rule's applicability — rewriting the condition of a
  synapse, not its weight.
- `unlearn`/`prune` removes rules whose `d.weight` decayed under threshold — a periodic handler
  walks `d.synapse` and `d.clause` doing exactly that.
- `graft_method` installs a newly distilled routine as a named, callable method — the agent's
  learned behaviour becomes addressable code, not data interpreted by a fixed loop.

### 8.5 Searching programs is searching cells

The gradient-free loop the trail makes cheap:

```text
candidate := reify(base_rule)                      // fresh closed copy, fresh head
graft_plan(goal, name, test, body)                 // the qualified verb — there is no bare
   on the installed copy under this mark           //   `graft`; trailed — installs the clause
mutate: graft_where(installed_clause, perturbed)   // acts on the INSTALLED clause cell,
                                                   // not the Code — or raw cells+Value surgery
score := run(fitness, [candidate])                 // bounded, same cursor
if score >= incumbent: cut — candidate stands      // discard the mark's alternative
else: fail — backtrack drains to the pre-graft mark // free undo
```

Every step is in-turn, same-cursor, mark-bounded: `reify`/`graft`/`run` are trailed writes and the
drain is the undo — no snapshot format, no shadow arena. The scoring function can be a `def`, a
`succeed when` clause, or a `run` of a `Code<Float>` fitness fragment. Determinism is preserved: the
search is a fixed sequence of bounded steps inside the turn's fuel. The generativity bound is stated
honestly: the candidate pool is *clause-granular over a frozen literal pool* — `graft_*` recombines
literal-born tests and bodies, `reify` copies, but nothing in v1 instantiates a parameterized
fragment into a new fragment; that verb (`specialize`) is U-M9, and within-clause novelty before it
is raw `cells`+`Value` surgery (§11's unguarded door).

### 8.6 Deterministic pseudo-randomness

Mutation operators (perturb a weight, pick a clause to prune) need randomness, and the runtime
offers none — deliberately, since turn determinism is a spec-§14 property. `std:meta::next_rand` is
a wrapping-safe mix written in Vortex `Int` ops over a seed kept in a state cell — 32-bit halves
extracted and reassembled via `DivMod`/`Mod` (`Mul` overflow-faults at `vortex_vm.cpp:457-463`, and
no `Xor`/shift opcodes exist — a modulus-form LCG is the fallback shape) — deterministic per seed,
checkpointable (the seed is a cell), replayable, and honest that it is pseudo-randomness, not
entropy.

### 8.7 The road to gradient learners

Gradient-based learning needs a *tape*: a record of which ops produced which values, so errors
propagate backward. A Vortex opcode graph *is* a computation graph, and `+d.grab`/`+d.spin` edges
already encode the dependency structure — a backward walk over `Linked` is structurally possible.
What is missing is the value-level machinery (per-op partial derivatives, a forward-recording
discipline, parameter vectors) and its interaction with `release` (a taped forward pass is trailed
state). This is U-M3: recorded as a direction, not claimed. The honest near-term target is
gradient-free methods — hill-climbing, evolutionary search over `code"…"` fragment pools, Hebbian
and reinforcement-style weight updates — all of which run on §8.5's loop today.

______________________________________________________________________

## 9. Worked examples

### 9.1 A parameterized fragment, spliced

```vagent
event Tick                                   // declared; `splice`/`code` are prefix forms —
agent Counter {                              // no std:meta import needed here
  on Tick {
    let inc := code"n: Int | n + 1"      // Code<Int>, one Int parameter
    let next := splice inc(count)        // expands to { let p0 := count; p0 + 1 } at expand
    count := next
  }
  var count: Int := 0
}
```

`splice inc(count)` binds `count` to a fresh `let` local and substitutes the cloned body; the merged
statement lowers once. `run(inc, [count])` would do it at run time through the frame convention — a
`Call` + spill instead of inlining.

### 9.2 A plan that tightens its own `where`

```vagent
import std:meta

event Attack                                 // §7.5 handler trigger
goal Patrol() perform                        // the plan rank `cautious` joins
predicate threat(level: Float)               // the belief the test reads

agent Guard {
  def patrol() { /* steer the patrol route */ }
  plan cautious for Patrol where believes threat(?t) & t > 0.7 {
    patrol()
  }

  on Attack as a {
    // learned caution: raise the bar next time, durably
    learn_where(plan_named("demo:Guard.cautious"), code"believes threat(?t) & t > 0.9")
  }
}
```

`plan_named` resolves the clause by its qualified name — `Opt<Cell>` in expression position yields
the cell or fails the statement — and `learn_where` validates the fragment, re-points the clause's
`+d.grab` at the new test's *head* (not the entry op — M14), and records the patch. Because the
replaced test is program-original, the patch records **no** prior head: genesis restores the
skeleton's own attachment for free (§6.6), and `unlearn` simply detaches. The change affects the
*next* plan selection, since `where` is evaluated per selection (§6.3). (`?t` binds *inside* the
test fragment only: a grafted test cannot export `?`-variables for the clause body — the body's own
`?` names resolve in its own evaluation — grafted tests never export `?`-bindings: a body
cannot receive them and cannot even name the test's `?t`, so the situation is unparseable rather
than refused.)

### 9.3 Hebbian growth of a rule rank

```vagent
import std:meta

event Sighting                               // percept events, §7.5
event Meal
goal Feed() perform                          // the rank learn_plan grows
predicate sees(what: Str, near: Str)         // predicates are TopDecls — top-level, never
predicate learned_rule(what: Str)            //   agent members (spec §4.2)

agent Forager {
  def navigate_to(place: Str) { /* steer toward the named landmark */ }

  on Sighting as s {
    // grow a candidate rule the first time we see food near water
    if believes sees("food", "water") & not believes learned_rule("food_water") then {
      learn_plan("Feed", "patch:near_water",
                 code"believes sees(\"food\", \"water\")",
                 code"{ navigate_to(\"water\") }")
      believe learned_rule("food_water")
    }
  }

  on Meal as m {   // it worked — strengthen
    for c in clauses_of(symbol_named("Forager", "Feed")) do {
      let w := weight_of(c)
      if [w] then set_weight(c, w + 0.1)
    }
  }
}
```

The grown clause participates in plan selection like any plan — it is a plan. (`navigate_to` is a
`def`, not an effect — non-suspending, so the body fragment is legal under M6. Predicates take typed
arguments; quoted `Str`s serve as atoms, and the `\"` escapes are the §5.1 one-token rule at work.)

### 9.4 A compile-time `static`

```vagent
goal Wait(w: Int) perform                    // the two plans' rank
event WithinBudget { n: Int }                // emit target, §7.5 — the `n` field the body emits

agent Waiter {
  def hold() { /* suspend until the wait elapses */ }
  plan patient for Wait(w) where w < static (60 * 1000) {   // folded at compile time
    hold()
  }

  plan timed for Wait(w) {
    let policy := code"limit: Int | limit - w"    // w captured here, at definition site
    if splice policy(static (60 * 1000)) > 0 then emit WithinBudget{n: w}
  }
}
```

`static` runs `60 * 1000` in the compile arena; `splice` instantiates `policy` with the folded
literal — both resolved before the artifact is sealed, nothing at run time. The fragment's free name
`w` binds to `timed`'s plan parameter at the literal's own position — the `cap$0` rename binds it to
that cell (§5.3) — and the splice produces an `Int`, so the `> 0` comparison satisfies the `Bool`
rule rather than leaving a bare `Bool` in condition position. (The examples in §9.2–§9.4 assume the
executor lowering for belief/action bodies — §4.1's scope note; only §9.1 is inside today's scalar
subset.)

______________________________________________________________________

## 10. What the C++ core has to gain

The same test vagent applies: Vortex code first (`std:meta`), then the adapter (new, vagent's own),
then existing code. Two proposals survive the filter — one VM write-funnel guard (P15) and one
adapter ceremony over existing relocation machinery (P16).

### 10.1 New code

```text
apps/common/xanadu/vagent/
  parser.cpp additions         // 'code' in the PrefixedStringLiteral gate (:848-855); Primary/Type
                               //   productions; parseCodeBody (#label/CodeParams header + inner
                               //   offset diagnostics, # stripped pre-lex)
  expand.{hpp,cpp}             // the Program → Program stage: code-body parse, scope walk
                               //   (shared with checker's env construction — resolve.cpp),
                               //   cap$N aliasing + env records, $-anchor rewriting,
                               //   splice substitution, Code-splice def expansion,
                               //   residual-splice check — joins VAGENT_FRONTEND_SRCS
  resolve.{hpp,cpp}            // the resolve-only pass stamping Identifier scope ids — the
                               //   checker's env walk extracted and shared (§5.6); the stamp
                               //   lives on Expr and cloneExpr/cloneDecl preserve it;
                               //   joins VAGENT_FRONTEND_SRCS
  frontend.{hpp,cpp}           // the driver sequencing parse → resolve → expand → check —
                               //   owns invocation order and the per-literal resolve seeding;
                               //   joins VAGENT_FRONTEND_SRCS (source→checked-Program driver,
                               //   no arena dependency — it belongs with its siblings, not the
                               //   kernel glob)
  ast.hpp                      // ExprKind::{Code,Splice,Static} + the Expr scope-stamp field
                               //   cloneExpr/cloneDecl preserve (§5.6) — listed for the record
  ast_clone.{hpp,cpp}          // cloneExpr/cloneProgram — deep copy for per-site instantiation
                               //   (every ExprPtr field incl. children/scope/recipients/
                               //   deadline/guard/recovery + parallel names; Decl-granularity
                               //   for M13) — joins VAGENT_FRONTEND_SRCS
  fragment.{hpp,cpp}           // compileFragment: head + descriptor + +d.refs mark rank +
                               //   +d.recipe emission around the kernel — kernel-side, picked
                               //   up by the existing source glob, no list edit
  checker.cpp additions        // Code scalar + 0-or-1 arity chain (arity 0 = wildcard);
                               //   bare Code = parameterized wildcard; run(c, [args]) special
                               //   form + value_of; stamp-honoring (resolve owns name truth);
                               //   $-bearing user-name refusal; import allowlist gains std:meta;
                               //   builtin member-signature table gains std:meta's entries;
                               //   fragment table (SourceLocation-keyed), purity flags,
                               //   per-fragment arity table, run special form, static
                               //   closedness, position-mask re-verification,
                               //   CheckLimits::maxExpansionDepth/maxSplices threaded counters
  compiler.cpp additions       // merged-AST lowering; static evaluation (no-mark vm run, cursor
                               //   unlink, post-run choiceDepth/outstandingMarks baseline
                               //   check); CompilerBinding gains a state-side marker bit for
                               //   the closed flag's provenance; cap$N CompilerBindings emission —
                               //   cap$N lookups bypass binding()'s unvalued render check and read
                               //   externalReferences_ directly (§5.3)
  code_path.{hpp,cpp}          // narrowing + src:m:l:c+o:c diagnostic mapping helper (like
                               //   cell_path) — joins VAGENT_FRONTEND_SRCS
  stdmeta prelude sources      // std:meta routines in vagent/Vortex, linked like std:agent
apps/vagent:                   // no new commands; `check` gains code"…" coverage
adapter:                       // binds program root + agent cell + d.code/d.patch/d.weight/
                               //   d.synapse/d.entry/d.patch-head/d.validate/d.refs/d.ref/
                               //   d.recipe/d.mesh.relocation + the validate ceiling, arity
                               //   & impure-kind cells (V10 mechanism, no-marks path); owns
                               //   P16's flag/re-kind/restore/rehydrate ceremony
Makefile:                      // VAGENT_FRONTEND_SRCS gains expand.cpp, resolve.cpp,
                               //   ast_clone.cpp, code_path.cpp, frontend.cpp
                               //   (Makefile:643,688-690);
                               //   fragment.cpp needs no edit (kernel glob); VAGENT_SYNTAX_OBJS
                               //   (:695) stays unchanged — `code` stores raw text and expand
                               //   does body parsing, so no new syntax object
```

The `run`/`graft_*` verbs need the call ABI (`#Call` + frame + spill + the `d.entry` head
resolution) — owed by vagent §11.1 itself, not new debt here. `static` needs a `staticFuel` entry in
`ScalarCompilerLimits` (`compiler.hpp:23-26`) plus the two expansion budgets in `CheckLimits`
(`checker.hpp:12-27`). All of this is new code and not proposals, matching the spec's §11.1 rule.

### 10.2 Proposed changes to existing code

Continuing vagent's register after P14. **P15 and P16 are proposed, not approved.** P17 is recorded
below the register as deferred-not-registered.

| No. | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | Where                                                                                                                                                                     | Why it cannot live in `std:meta` or the adapter                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Without it                                                                                                                                                                                                                                                                                                              | Used by         |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------- |
| P15 | Every VM write into an `opcodeMap_`-registered cell is refused — kind-immutability at the write funnel: `VortexCore::writeValue`/`splice` take a VM-installed kind predicate (`opcodeMap_.contains(masterOf(cell))`), covering `executeOpcodeBody`'s `Value`/`Splice` cases, `step()`'s memo-hit and generic output writebacks, and both `runPipeline` writes; refusal is a typed `success=false` per site. Exempt by construction: `setCursorOpcode`, `Call` frame writes (the frame-cell carve-out), `mintOpcode`'s `setContent`, every write to a fresh (unregistered) cell. Post-resume the program load path re-runs `mintOpcode` at genesis, repopulating the empty per-`WorkerContext` map.                                                                                                                               | `vortex_core.cpp:196-254` (`writeValue`/`splice` funnel + installed predicate); call sites `vortex_vm.cpp:771-806`, `:1269-1273`, `:1287-1294`, `:337`, `:343-346`; tests | `opcodeMap_` is private VM state (`vortex_vm.hpp:186`). No Vortex opcode can see the map, and the adapter runs between turns, not between opcodes. Every non-structural op restates bound outputs' content through `writeValue` — a wing re-point (this document's whole subject) can aim an output at an op cell and rewrite its `int64` with no `Value`/`Splice` involvement; the map claims the old kind while the cell names a new one, and the trail cannot undo it because the map is not arena state. | Any write to a registered op cell silently changes what the cell *means* — a stale-kind divergence invisible to every `std:meta` guarantee, making "the graph says X" unverifiable. The only alternative would be refusing all program-side writes, which is `cells"…"`'s ordinary content write and cannot be refused. | M10, §6.5, §7.2 |
| P16 | Checkpoint-time relocation of fragment-owned int64 reference cells — an **adapter-side ceremony needing no new core mechanism**: emission marks every fragment-owned int64 ref cell whose `+d.refs` record carries no `+d.name` with *its own* `d.mesh.relocation→"cell"` marker cell (one per ref, `mesh_evaluator.cpp:735` precedent) plus a `+d.ref` record on `+d.refs`; `+d.name` on a record makes it a name-record — no marker, never re-kinded, genesis rebinds via `symbol_named → +d.values → head/op discriminator → writeValue(slot, int64(entryOp))`. The mutable seam is the adapter's own `Executor` callback inside `pumpOne` (flag-driven: end-of-turn re-kind `Int64`→`OpHandle` on the mutable arena, `publishValue` between turns on `const&`, head-of-next-turn `Int64` restore on every exit path); genesis rehydrates before `d.patch` replay (resume skips `importValue`, `mesh_actor.cpp:260-271`). Acceptance: a learn → checkpoint → resume → learn → checkpoint round-trip test. Scope: fragment-owned ref cells only. | emission-side marking: `vagent/fragment.{hpp,cpp}` (kernel-glob TU, §10.1); the flag/checkpoint/genesis ceremony: the adapter's own executor TU — all new adapter/vagent code reusing `foreign_op_handle`/`publishValue`/`checkpointView` as-is | `promote` copies Int64 content verbatim and translates only links and `OpHandle` values (`arena_manifold.cpp:1934-1954`); no opcode can write `OpHandle` kind (`writeValue` covers Int64/Double/Bool/None, `vortex_core.cpp:203-227`), and `resume` bypasses `importValue` — so only the adapter (which owns emission and checkpoint timing) can run the ceremony; it needs no VM change because `isCellRelocationHandle` and the promote path already exist.                                                                                                                                                                                       | Every promoted fragment's wing slots and `Jump`/`Call`/`Branch`/`Choice` targets stay stale dense ids after `resume` — silent aliasing into unrelated cells; `learn`/`d.patch`/`d.code` durability is impossible and must degrade to session-only.                                                                      | M11, §4.5, §6.6 |

P17 — `opcodeMap_`/`cursorPC_`/`arenaDims_` eviction or resync on `release` — is **deferred, not
registered**: the containment discipline (drain idiom, no bare `release` under live marks, no-marks
minting for ops and dims alike) keeps it unnecessary; a future proposal that needs mid-turn release
of op- or dim-minting marks must raise it then.

______________________________________________________________________

## 11. Refused alternatives

| Alternative                                                          | Refused because                                                                                                                                                                                                                                                                            |
| -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| An AST-as-cells side representation                                  | two truths to keep in sync; the opcode graph is already the executable truth (Vortex §7.3); an AST format buys source-level quoting at the cost of a metacircular compiler to make it runnable — deferred as U-M5, not the base representation                                             |
| Textual `eval(string)`                                               | a string assembled at run time has no static type — `Code<T>`'s `T` would be unprovable; runtime parsing ships the parser per arena and grants unbounded authority; `cells"…"`'s precedent is *narrow at check time*, which `eval` cannot do                                               |
| Untyped `Code` (no parameter)                                        | a splice would accept a `Float` fragment where `Bool` is wanted; `T` is the price of the same-type guarantee (M3)                                                                                                                                                                          |
| `,name` / `,expr` unquote inside `code"…"`                           | `,` collides with `ArgList` inside the string (`code"f(a, b)"`); general expression unquotes need macro-time evaluation, which M5 restricts to `static`. `splice` is the one uniform spelling                                                                                              |
| Full unrestricted self-modification via `cells"…"` on program cells  | raw `cells`+`Value` surgery has no signature checks, no `validate`, and (until P15) a kind-rewrite hole; `std:meta` verbs are the typed, checked door. Raw access is not *prevented* — the machine cannot forbid it — but it is documented as unguarded                                    |
| New opcodes for graft/validate/reify                                 | the surgery is `Link`/`New`/`Value`/`Break`/`Splice`/`Linked` — all present, composed through the focus-slot idiom; validation is a `Linked`-walk; new C++ must justify why it is not Vortex (vagent-runtime-grounding.md's rule). P15 is the residue that genuinely needs the VM; P16 is adapter ceremony over existing machinery |
| A second cursor or second arena for sandboxed eval                   | V3: `backtrack` pops foreign cursors' choice points; `VortexStdLib::call`'s second-cursor pattern under a live turn is the bug the grounding audit names                                                                                                                                   |
| Persisting choice points/marks across turns or an install            | `checkpointView` refuses non-zero `choiceDepth()`/`outstandingMarks()` (V2); marks are LIFO truncation, not portable state                                                                                                                                                                 |
| Compile-time macro = arbitrary vagent evaluation                     | a `def`-evaluating stage needs a program to run against and a turn to run it in; `static` is the pure+closed subset that is actually deterministic                                                                                                                                         |
| Scheme-style rename hygiene / syntax-case                            | fragments are typed closures, not syntax transformers — but hygiene *is* renaming: `cap$N` capture renaming at the binding layer does the work (§5.3); syntax-case machinery would solve a problem the model does not have                                                                 |
| Extending `cells"…"` to mint opcode cells                            | unnecessary: `Value` op writes `int64` when its source is a scalar cell, so `New`+`Value` already mints ops; `%` literals stay strings                                                                                                                                                     |
| `Code` as a `Cell` synonym                                           | loses the splice/run boundary: a `Cell` from `reflect` must not become executable without a descriptor-checked upgrade (§6.1)                                                                                                                                                              |
| Parse-time textual expansion (token macros)                          | untyped and pre-check — it would defeat `Code<T>` entirely; expansion belongs in `expand`/`check` (M4)                                                                                                                                                                                     |
| `run` in a fresh cursor/VM per call                                  | same refusal as the second-cursor item; `run` is a same-cursor `#Call` by definition (M6)                                                                                                                                                                                                  |
| Suspending fragments now                                             | an `await` inside `code"…"` would need the continuation reification of U-A7; until then the step-path model (V4) cannot name a fragment position. U-M6 records it                                                                                                                          |
| `learn` persisting a *source string* and recompiling at load         | needs the compiler at load — compile-side machinery dragged into the runtime; stored graphs are cells all the way down and, with P16, checkpoint for real (M11)                                                                                                                            |
| Durable `Code`/`Cell` handles stored as ints                         | dense ids do not survive `promote`/`resume` renumbering (§4.5); durable identity is `d.name` labels + links                                                                                                                                                                                |
| `Clone` as `reify`'s copy primitive                                  | `Clone` shares content and value bits through `masterOf` (`vortex_core.cpp:181,202`; `vortex_vm.cpp:807-836`) — a transclusion-instance that aliases the original, so mutations write through both and `closed` is unverifiable. Re-emission from the `+d.recipe` is the honest deep copy  |
| Mark+release for `static` eval                                       | mints op cells under a live mark then truncates them — the `opcodeMap_` poisoning §4.2 rules out and `compiler.cpp:108-110` exists to prevent; the adopted discipline is no-mark eval + cursor unlink + priced residue (M5)                                                                |
| Bare `release(m0)` as runaway-mutation containment                   | leaves `cursorPC_` stale and corrupts the mark machinery over live inner marks (`arena_manifold.cpp:1657-1698`); the drain idiom is the containment, P17 stays deferred                                                                                                                    |
| Multi-installation of one `Code`                                     | `link` evicts the occupied `-d.grab`/`-d.spin` backlink — a second graft would silently sever the first attach (M14); consume-on-refuse is the honest version of sharing                                                                                                                   |
| Per-activation dynamic `New` mints / link-restoring frames for `run` | a different `New` calling convention or frames that restore link state — priced alternatives to the adopted rule (validate refuses bound-output `New` in `run`-reachable graphs, §6.2)                                                                                                     |
| Value-snapshot captures for `cap$N`                                  | a copied value breaks cell identity — `run` on a fragment that captured a parameter would read a frozen snapshot, not the binding's live cell; the alias keeps the §5.3 "the cell *is* the variable" contract and pays the dead-frame-cell caveat instead                                                               |
| Synthesized `let cap$N := …` for captures                            | `ExprKind::Let` lowers to a value copy into a fresh `localCells_` slot (`compiler.cpp:705-707`, `:853-855`) — a snapshot, not an alias — and `validName` refuses `$`-bearing names at preflight (`compiler.cpp:407-417`); the binding-table alias does both jobs                                                            |
| Refusing `cells"…"` `$` anchors inside `code"…"`                     | lawful bodies use `$w/…` paths today (§8.3's synapse walk); refusing them would narrow the body language for a solvable renaming problem — `expand` rewrites anchors through the same rename map (§5.3)                                                                                      |
| `withContext`-style host callback for P16's mutable passes          | the seam must fire inside `pumpOne`'s turn lifecycle, bracketed by the turn's own grant/cursor state — a detached host context cannot see the between-turns boundary; the adapter's own `Executor` callback already owns it (§6.6)                                                               |
| `run(c, a, b)` variadic spelling                                    | vagent has no variadic call surface (`parser.cpp:316-339`); the list literal is the v1 spelling — one list packs the frame, and `splice c(a,b)` already owns paren-args                                                                                                                       |
| Walking all of `dimensionsOf` for the `closed` check                | `dimensionsOf` is C++-only (U-M7); an outside edge smuggled onto a non-op-graph dimension evades both `closed` and `validate` — named as a door-left-open risk, bounded by the enumerable frontier (§6.6)                                                                                    |

The general rule behind the register, worth stating once: **new C++ cannot live in Vortex unless a
Vortex-level composition is genuinely impossible.** Project policy is that the agent's own language
is the extension vehicle — spec §11.1's capability layering puts new behaviour in vagent/Vortex
source linked as prelude, and the adapter is the place for anything that needs host-side timing
(checkpoint ceremony, genesis rehydration). `vagent-runtime-grounding.md`'s audit is the evidence
base: every time the earlier drafts reached for C++ — opcodes for graft/validate/reify, a second
cursor, a sandboxed eval VM — an existing mechanism or a documented hazard already answered it (the
audit names the second-cursor-under-live-turn bug outright). The residue that survives the filter is
exactly what touches state no opcode can see: `opcodeMap_` membership (P15) and checkpoint-boundary
`OpHandle` re-kinding (P16 — which is itself adapter ceremony over existing machinery, not new VM
mechanism). Everything else in this document is `std:meta` source, adapter plumbing, or frontend —
the table above is the list of doors closed on the way to that conclusion.

______________________________________________________________________

## 12. Properties, with their conditions

- **Well-typed sources produce well-formed graphs.** Given the kernel's existing contract (refusal
  before allocation for unsupported/ill-typed input, `compiler.hpp:51-57`), a `code"…"` body that
  passes the checker lowers to a graph whose ops, wings and chains are structurally sound. `splice`
  cannot change that: it composes already-checked trees — and clones them, so sites cannot alias
  each other's locals.
- **All static checks are complete before runtime.** Expansion, typing, purity, closedness, arity
  and `static` evaluation all finish in `expand`/checker/kernel; run time never expands, parses or
  type-checks, and a residual `splice` is a compile error.
- **Mutation is transactional within the discipline.** A `graft`/`learn`/`prune` under a mark is
  undone exactly by backtracking/drain — with the same completeness as a belief write, because it
  *is* the same write path; VM-side maps are outside the trail, which is why the drain idiom (not
  bare `release`) is the containment (§6.4).
- **A mutated graph cannot hang the agent.** `maxCycles` bounds `run` and turn execution;
  `validate`'s walk is per-op budgeted plus O(n·membership); a malformed or looping mutation dies as
  the §6.4 fault-channel class (`ActorDown` on budget exhaustion), never a block.
- **P15 restores a single decode truth.** Given P15, an `opcodeMap_`-registered cell's kind cannot
  change after minting — through *any* VM write path, not only the two splice ops.
- **P16 makes `learn`'s durability real.** Given P16's adapter ceremony — marker edges + `+d.refs`
  records at emission, `OpHandle` re-kinding across `publishValue`, Int64 restore on the live arena,
  `+d.refs` rehydration at genesis — a promoted fragment's internal int references are relocated
  like every other handle; without it, `learn`/`d.patch`/`d.code` are session-only and §6.6
  collapses to graft-for-the-session.
- **No cross-arena mutation or execution — with an aliasing caveat.** V1: `resolveCell` yields
  `nullopt` for foreign cells *only when out of dense range*; in range, a foreign ref aliases a live
  local cell silently (`arena_manifold.cpp:256-267`). The rule is therefore a discipline, not a
  mechanism: received `Code`/`Cell` values are opaque — re-resolve by `d.name` label before
  `run`/`graft`/`cells`, and never pass a foreign ref-box to `Linked`. Remote code mobility is U-M2.
- **A `Code` installs once.** Graft consumption (M14) turns the link-eviction hazard into a checked
  refusal; sharing is explicit via `reify`.
- **Turn determinism is preserved.** Mutation is deterministic arena writes; `next_rand` is
  deterministic given its seed cell; the spec's §14 single-turn determinism condition carries over
  unchanged.
- **Session mutation is honestly ephemeral; durable mutation is explicit.** Session grafts die at
  `resume` (V12 stands for program cells); anything that must persist travels as a `d.patch` patch
  whose fragment is reachable — and the V12 amendment is *scoped* to closed fragments, so the
  program skeleton still cannot leak into a checkpoint (and replaces of program-original attachments
  record no prior head, keeping it that way).
- **Not claimed:** behavioural correctness of grafted code, source-level recoverability of fragment
  source (the `d.name` label needs an external source map), freshness of memoized results on mutated
  graphs, durability of stored `Cell`/`Code` field values across `resume` (use labels), or any
  guarantee past `maxCycles` but before fuel exhaustion.

______________________________________________________________________

## 13. Breakages and costs

- **One new core proposal plus an adapter ceremony** (P15; P16 needs no core change). P15 changes
  what any VM write may target (one `opcodeMap_` hash hit per write into a non-fresh cell, once per
  bound-output writeback at most); programs that *deliberately* wrote `int64` into registered op
  cells would now fail — no such program exists in the tree (the compiler mints ops via
  `mintOpcode`, not `Value`). P16 adds two adapter passes at every checkpoint boundary and a
  `+d.refs` rehydration at genesis — O(marked refs) per promoted fragment each way — plus the marker
  edge and `+d.ref` record per emitter-minted ref cell.
- **New keywords/types**: three contextual keywords (`code`, `splice`, `static`), one type
  constructor (`Code<T>` + arity chain), one `ExprKind` triple (`Code`, `Splice`, `Static`), a new
  pipeline stage (`expand`), `cloneExpr`/`cloneProgram`, plus `ExprKind::Code`-handling across
  checker passes — modest front-end surface, all in vagent's own code. `splice`/`static` reserve
  bare-call position (§5.1).
- **Expansion copies**: N splice sites clone the fragment AST N times and mint N graphs; `cap$N`
  costs one `CompilerBindings` alias entry plus one env record per captured name per literal (no
  synthesized `let`, §5.3); a `static` inside a spliced body re-evaluates per expansion site — the
  folded value is per-clone, not shared; `Code`-splicing `def`s expand per call site — all bounded
  by `maxExpansionDepth`/`maxSplices`. Splice bookkeeping itself is the fragment table
  (`SourceLocation`-keyed records) plus the rename map — O(captures + splices) per compile.
- **Fragment overhead**: O(ops + slots + recipe) per `code"…"` literal — ~8–14 cells for head +
  descriptor alone, plus one `+d.refs` record *and* one marker cell per emitter-minted ref cell
  (~1–2 per op), plus the `+d.recipe` chain (O(fragment cells + links)) — on top of one copy of
  `std:meta` per artifact that imports it, over `std:agent`'s per-arena copy cost (spec §4.4).
  Reflection wrappers memoize session-side per clause on the `clause +d.code → +d.step` chain,
  keyed by the current attach cell (§6.6 — never on `d.code`).
- **`run` costs** a `#Call` frame + spilled cells, same as a method call, plus one list-literal
  materialization per call — the `[args]` literal mints a head + O(n) element cells through the
  ordinary `materialize` path (`compiler.cpp:733-741`); `graft` costs O(1) links + O(rank) append
  walks; `validate` costs per-op `maxCycles` + O(visited) tag-cell mints + O(n) membership tests
  per install (§7.2); `learn` adds a `d.patch` cell + head-chain records + checkpoint ops for live
  fragment cells + P16's two boundary passes.
- **`static` residue**: each `static` leaves its eval graph, an unlinked cursor cell and temporaries
  as dead cells in the compile arena (no mark, by M5's ruling) — O(eval size) dead cells per
  `static`, bounded by `staticFuel`.
- **Checkpoint growth**: `d.code`-registered fragments and `d.patch` patches are live state and pay
  the spec-§4.4 promotion price per checkpoint; patch replay costs O(live patches) at genesis —
  unresolvable patches are dropped, not re-skipped forever.
- **Dead cells accumulate** between checkpoints like everything else — replaced bodies, pruned
  clauses, unlearned patches, head chains, consumed (`installed`) heads stay allocated until
  reclamation (the spec §4.4 story; U-A1's deferral applies unchanged).
- **The captured-binding caveat** (§5.3): a fragment closing over a frame cell keeps one dead frame
  cell per captured call until reclamation — the `splice` path is exempt by the capture-liveness
  rule.
- **`d.clause` appends walk the rank** (no tail pointer): a `learn`ed plan is O(plans) to install;
  selection reads it in declaration order — learned plans are *last*, matching U-A12's fixed
  selection until U-M4.
- **`d.synapse` junctions are one cell per association** plus label cells — N→M connectivity costs
  the junctions honestly; there is no free fan-in.
- **Fuzzing surface**: the `code"…"` parser entry, the `expand` stage and the `splice`/`static`
  checker paths are new input surfaces and get `tests/fuzz/` coverage per the language's existing
  rule (§15 of the spec).

______________________________________________________________________

## 14. Reconciliation with the convergence

- **R5 (`noCell == 0`).** `std:meta` returns `Opt<Cell>`/`Opt<Code<…>>`, never a sentinel; `run`
  failure is expression failure (no value), not a magic result.
- **R6 (scalars).** Opcode kinds, weights, ordinals, `produced` flags and ref-box ints are scalar
  cells — the same R6 contract `mintOpcode` and `makeScalarCell` already satisfy; P16 exists
  precisely because ref-ints are scalar content that promotion does not relocate.
- **R7 (a `SetLink`'s subject is its chain).** Mutation writes no store operations: arena mutation
  stays arena mutation (V9); the only promotion is the checkpoint's, which follows R7 through
  `promote` as before.
- **R8 (two regimes).** `learn`/`graft` mutate arena state only; a patch becomes durable only
  through the checkpoint promotion into the published value's own transient store — never into a
  store a user visits. `StoreWriter` remains U-A9 and untouched.
- **R12 (a dimension is a cell).** `d.code`, `d.patch`, `d.weight`, `d.synapse`, `d.entry`,
  `d.patch-head`, `d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.mesh.relocation` are minted as
  data through `ensureDimension`, which also records them in the arena's name table — the V10
  discipline, not a `SystemDimensions` field — and the no-marks minting rule keeps `arenaDims_`
  honest for the same reason P15 keeps `opcodeMap_` honest.
- **U1 (rank indexing).** Another consumer: clause/synapse walks stay linear until U1 lands.

______________________________________________________________________

## 15. Deferred work

U-M identifiers are new; they do not collide with vagent's U-A list. This section and §7.3 together
are the document's open-questions apparatus (§7.3, last item).

- **U-M1 — A metacircular compiler.** `code"…"` fragments are compile-time-born; a
  vagent-compiler-in-vagent would allow runtime source→graph. Deferred: it needs the AST-cells
  representation refused in §11, plus the whole frontend re-expressed in Vortex. Direction: if it
  ever lands, it must lower through the same kernel discipline, not a second emitter.
- **U-M2 — Remote code mobility.** Sending a `Code` across agents needs a portable code identity (an
  artifact-level ref, not a `CellRef`), plus cross-owner authority (U-A2). A `Code` in a message is
  *aliased* data today — opaque by discipline (§4.5), not by mechanism.
- **U-M3 — Gradient learners.** Autodiff over opcode graphs: forward-recording tapes, per-op
  derivatives, parameter vectors, and the tape's interaction with `release`. Direction: record the
  forward pass as cells; backward-prop walks `Linked` against `+d.grab`. Unpriced.
- **U-M4 — Weighted/learned plan selection.** The plan library's weights exist (§8.2), but selection
  order is fixed until U-A12's gate: any learned selector must permute only the eligible untried
  set, fault to declaration order on bad output, and preserve the spec §6.4 isolation.
- **U-M5 — Source-level reflection.** AST-as-cells quoting for pretty-printing and transformation
  tooling. Refused as the *base* representation (§11); viable later as a *view* (reconstructed
  source) rather than the truth.
- **U-M6 — Suspending fragments.** `await`/`achieve`/`ask`/effects inside `code"…"` need U-A7's
  continuation reification: a step path that names a position inside a fragment, and a statement
  split across turns whose undo semantics are specified, not implied.
- **U-M7 — Generic deep-copy.** `reify` of non-literal graphs (program-side `reflect` output) needs
  per-cell dimension enumeration — an opcode or path form the current subset lacks (`cells"…"` walks
  named dims; `dimensionsOf` is C++-only). Literal-born fragments are covered by the `+d.recipe`
  mechanism (§6.1); the foreign-graph case stays open — if ever needed, price a `#Dimensions`-style
  op or a `cells` extension.
- **U-M8 — Metaprogramming the stdlib/`std:agent` prelude.** Grafting onto `std:*` module cells is
  technically possible (they are cells in the same arena); the discipline — prelude immutability vs.
  learned extensions — needs a policy, not a mechanism. Default rule for v1: graft points are the
  *agent program's own* clause ranks and symbols.
- **U-M9 — `specialize`: partial application producing a smaller `Code`.** Binding a parameterized
  fragment's parameters to values → a new closed fragment — is the one generativity hole in §8.5's
  search (the pool is clause-granular over a frozen literal pool until then). Direction: same recipe
  machinery as `reify`, plus bound-literal parameter cells; granularity boundary stays clause-level
  while deferred.

- **U-M10 — `run_checked`/`run_pure`: a `run` variant trusted inside pure contexts.** The
  `run_checked` candidate §6.2 names — trusting a validated `pure` descriptor so pure bodies may
  `run` a certified-pure fragment — is deferred with its validation cost recorded: it needs a
  *complete* purity walk over the target graph (the unknown-flagged wrapper case included) before
  `pureBody` may admit it, which is §7.2's walk plus a flag-trust rule, not free.

______________________________________________________________________

## Appendix: Versioning and Change History

**1.0 — Initial draft.** Establishes `code"…"`/`Code<T>`/`splice`/`static` for compile-time
metaprogramming and `std:meta` (`run`, `reify`, `graft_*`, `prune`, `learn_*`, `unlearn`,
`reflect`-family reads, weights/synapses) for runtime mutation; rules opcode kinds immutable at run
time (P15, proposed); amends V12 narrowly so closed fragment graphs may be linked from state
(`d.code`, `d.patch`); maps neuro-plasticity onto clause ranks, weight cells and trail-bounded
search; defers metacircular compilation, code mobility, gradient learners, weighted selection,
source-level reflection, suspending fragments, generic deep-copy and prelude grafting as U-M1–U-M8.

**1.1 — First review revision (r1 disposition, `projects/vagent-metaprogramming-review/`).** Repairs
the flattened tables to GFM; replaces `+d.grab` head attachment with head indirection cells on a
dedicated `d.entry` dim (M14 single-installation; `graft_method` links the head, patches record
heads by `d.name` on `+d.patch-head` chains — links would sever each other by eviction); rewrites
P15 to `opcodeMap_`-membership at the `writeValue`/`splice` funnel covering all VM writeback sites;
adds P16 (checkpoint-time relocation of fragment-owned int64 ref cells to `OpHandle` + genesis
rehydration — `learn`/`d.patch`/`d.code` durability is explicitly conditional on it); moves
expansion into a dedicated `expand` stage with `cloneExpr` and `cap$N` binding-layer renaming
(correcting the "no gensym" claim); makes `Code`-parameterized `def`s per-call-site templates (M13);
gives `run` the signature `run(c: Code<T>, args: [Cell]): T` and descriptors a parameter arity
table; rewrites `static` discipline (no-mark eval, cursor unlink, form-level closedness, scalar-only
results); specifies the `reify` recipe (census + link tuples + old→new map; `Clone` refused), the
focus-slot idiom, the drain-idiom containment (no bare `release` under live marks), `d.validate`
seen-set, junction-cell synapses, `graft_method` redefinition-as-replacement, no-prior-head for
program-original patches, monotonic patch replay with drops, qualified type identity, and
`CheckLimits` expansion budgets; defines `reflect` as the read family; defers P17 and adds U-M9
(`specialize`).

**1.2 — Second review revision (r2 disposition, `projects/vagent-metaprogramming-review/`).**
Re-scopes P16 from "new pass beside `publishValue`" to an **adapter-side ceremony over existing
machinery** — `d.mesh.relocation` marker edges + `+d.ref`/`+d.refs` records at emission,
`OpHandle` re-kinding across `publishValue`, idempotent Int64 restore on the live arena, `+d.refs`
rehydration at genesis before `d.patch` replay (resume bypasses `importValue`); P16's register row
now says no core change is needed. States the **`d.code` reachability invariant** (no `d.code` cell
may edge into a program cell): `run`/`graft_*` never register, `learn_*` grafts a `reify`d copy and
registers the pristine original, `d.code`-resident heads are reified before grafting, reflect
wrappers move session-side keyed by attach cell with unknown-flagged descriptors, and `unlearn`
drops the registration edge. Corrects `closed` to cover program-side references (name-records on
`+d.refs`, genesis-resolved by `symbol_named`; int-encoded program refs excluded) with learn-time
re-verification. Replaces synthesized `let` captures with **`cap$N` binding-table aliases**
(`CompilerBindings` → the binding's own cell, one counter per `expand` run, `CellsPath` `$` anchors
rewritten, `$`-bearing user names refused; frame-parameter capture is the stated ABI-debt rule).
Re-specifies `run` as a **checker special form** (`args` a per-element-`any` list literal, values
bound on the `#Call` frame, `Halt` tails not `run`-nable, `run` named in the code-body allow-list,
opaque-impure with `run_checked` as the named candidate, `value_of` for `Code<Cell>` results).
Reverses `d.validate`'s seen-set to `visited +d.validate → marker` with a fresh marker per pass and
op-graph-restricted traversal; names the arity table's adapter home. Re-specifies `reify` recipes
with link-encoded endpoints (no census ints), verbatim external captures, regenerated
`+d.refs`/`+d.recipe`, `reify:<src>:<serial>` labels, and recorded-emission-template scope (current-
graph copy stays U-M7). Tightens templates (actual-splice trigger, direct `Code` params only,
recursion and control-flow refusals, Decl-granularity cloning), grafting (bound-output `New` refused
everywhere, effect-member `graft_method` refusal, `?`-export rule, `graft_plan` formals), `d.weight`
edge-vs-bits disambiguation and the synapse endpoint domain, the Makefile TU lists
(`expand`/`resolve`/`ast_clone`/`code_path` → `VAGENT_FRONTEND_SRCS`, `fragment` via glob), and the
citation batch; adds the §11 why-not-Vortex paragraph and the §13 per-site `static` cost.

**1.3 — Third review revision (r3 disposition).** Replaces the `d.validate` seen-set with
**per-visited-cell tag cells** carrying the pass serial — the shared-marker and reversed-edge
schemes both break under `link`'s reciprocal eviction — and adds `d.entry` to the traversal dims.
Fixes name-bound calls to **model (b)**: an ordinary emitter-minted int64 `Call` operand, every
`+d.refs` record carrying canonical `+d.ref → slot cell`, `+d.name` present marking a name-record
(no marker, never re-kinded) vs absent marking a P16 ref-record, and the genesis rewrite spelled
`symbol_named → +d.values → head → +d.entry → writeValue(slot, int64(entryOp))` with the
session-vs-resume rebind divergence stated. Corrects `learn` to register only a **fresh, uninstalled
`reify`** — the program-side `code"…"` original is never registered, taking two copies (registered
head + installed copy) with the `program()`-walk sideness test named; patch records name the
registered head's `+d.name` and `learn_handler`'s carry `priority`; `unlearn` re-points through a
reify; re-registration is `insertIntoRank`-idempotent; `learn_*` of an installed head refuses like
`graft_*`. Grounds P16's mutable seam in the adapter's own **`Executor` callback inside `pumpOne`**:
flag-driven end-of-turn re-kind → between-turns `publishValue` on `const&` → head-of-next-turn
restore on every exit path, per-ref-cell markers, `+d.name` discriminator, base-target arm
acknowledged, round-trip acceptance test. Defines **state-side** as link-reachable and not
`program()`-walk-reachable; moves `let` locals into the frame-bound ABI-debt class (un-learnable
captures, with parameters); makes `Cell`/`Code`-typed captures non-`closed`; and bounds the closed
walk to the enumerable frontier. Restores `+d.entry` on reflect wrappers with the `clause +d.code →
+d.step` memo chain; makes signature equality compare ordered `(name, type)` pairs plus arity,
result, purity and `graft_method` effect-ness; has the checker honor the resolve pass's `Expr`-borne
scope stamps through `cloneDecl`; broadens M13's trigger to splices anywhere in the expansion; and
applies the minor bundle — genesis `programRef` binding order, `outstandingMarks() == 1` checkpoint
guard, `result`-row second dim, per-field recipe records, `value_of` in the API table,
`unlearn(clause_or_symbol)` name matching, empty-`+d.grab` semantics, `run(Code)→unknown`, `Code`
arity 0-or-1, extended name reservations, splice operand peeling, `?`-export rephrasing with
`occurrence` formals, the `run(c,a,b)` refusal row, three-part `plan_named`, durable `reify` serial,
per-junction weight scalars, clause weights on agent-`d.vars`, §8.5 pseudocode order, `prune`
wording, post-resume `opcodeMap_` re-registration, the frame-cell carve-out name, splice-only
fragments emitting no head, per-context `static`, canonical `+d.ref`, §13 repricing, the `d.code`
wording fix, resume-vs-import kind precision, citation corrections, and `import std:meta` in every
std:meta example.

**1.4 — Fourth review revision (r4 disposition).** Repairs the §9 examples to valid vagent
declarations — `goal Patrol()`/`Feed()`/`Wait(w: Int) perform`, the `Forager` predicates moved to
top level (predicates are `TopDecl`s), `event WithinBudget { n: Int }` — correcting 1.3's changelog,
which had claimed the declarations were already fixed. Tightens the sideness test to the **full
structural-dim vocabulary plus a `birthOp`-bounded mint window** so learned/grafted cells stay
classifiable, with the int64-endpoint link-walk door clause named on both `closed` and
`program()`-side. Makes `code_named`/`+d.name` matching **verbatim** with first-`:` splitting scoped
to qualified-name lookups only, and drops the unreachable program-image-twin sentence. Applies the
§6.1 head/op discriminator at every `+d.values` hop — `exportSymbol` binds `sym +d.values →
entryOp` directly. Scopes P16's marking and ref-record ceremony to `+d.name`-less ref-records.
Narrows `cap$N` to value-binding renames with a binding-targeted stamp, and makes frame-bound
capture anchors refused ABI debt. Corrects `graft_handler` formals to the bound event only. Fixes
the P16 flag protocol precisely — `pending(pid) == 1` arming, the empty-mailbox P4 wake, the
two-phase flag, kind-guarded restore on every exit path. Applies the minor batch: release's
`outstandingMarks() == 1`, `checkpointView`/`pending`/`birthOp` cites, the unlearned-graft
stale-call boundary, `closed`'s `Cell`/`Code`-content arm, `run`-operand name-records, the
`symbol_named` qualified split, `frontend.cpp`/`ast.hpp` in the inventory, the same-dim rank
convention, §8.2 weight mechanics, the genesis `opcodeMap_` step, the memo-record shape, `Code`
0-or-1 arity everywhere, the splice-only evaluated-position refusal, `static(param)`'s emission
error, `def run`/`def value_of` refusal under `import std:meta`, the duplicate-label refusal,
`graft_plan` pseudocode qualification, the P16 "Where" kernel/executor-TU split, and citation
corrections.
