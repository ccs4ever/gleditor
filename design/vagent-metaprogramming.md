# vagent metaprogramming: code as cells, self-modifying agents and the plasticity path

| field         | value                                                                                                                                                                                                                                   |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| version       | 1.14 (see [Appendix](#appendix-versioning-and-change-history))                                                                                                                                                                          |
| status        | Design draft. Nothing here is implemented. It rests on vagent v3.9, whose own implementation is in progress: the source frontend and the scalar lowering kernel exist; the actor executor does not.                                     |
| compiles to   | Vortex opcode graphs held in the agent's private `ArenaManifold`; a fragment is the same object `compileStatement` already produces (`vagent/compiler.hpp:28-34`)                                                                       |
| rests on      | vagent ([specification](vagent-agent-language.md)), Vortex ([runtime](vortex-hyperstructural-runtime.md)), `ArenaManifold`'s mark/trail/discard, the `cells"…"` embedded-VQL subset, the Spool-Mesh checkpoint/promotion machinery      |
| prerequisites | vagent §8.7 (`cells"…"`, `Cell`); the call/frame convention of §5.8 and §11.1 (owed by vagent itself); Vlog §6 clause layout for belief fragments; P1–P14 already approved there                                                        |
| changes       | new code (the `code"…"` literal, `splice`, `static`, an `expand` stage, a fragment-aware lowering pass, the `std:meta` prelude, adapter bindings) and **two** proposals, **P15** and **P16**, not approved — P15 is the one change to existing code; P16 needs no mechanism beyond P4's already-approved wake plus the named lineage-store seam (§10.2) |

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
| Fragment reference relocation            | adapter-side ceremony around `publishValue` on the adapter-kept lineage `Store` (§4.5, §10.1), run inside the adapter's own `Executor` callback: end-of-turn `OpHandle` re-kind + head-of-turn `Int64` restore + genesis rehydration; emission-side `+d.refs`/`d.ref`/`d.mesh.relocation` marks — P16, **no core change**                                                                                                  | **proposed** (adapter + emitter duty)                    |
| Weights and synapses                     | `d.weight`, `d.synapse` junction cells, `d.code`, `d.patch`, `d.entry`, `d.patch-head`, `d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.refFor`, `d.mesh.relocation` minted as data (`ensureDimension`, V10 mechanism) — minted only with no marks outstanding                          | **owed** (adapter duty, no core change)                  |
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
| patch           | a cell on the agent's `d.patch` rank recording one durable graft: the serial ordinal, target descriptor record, a `+d.patch-head` link to a `+d.step` chain of recorded *registered-head* names, a `+d.name` label (the verb-minted `patch:<n>` — `<n>` a durable monotone serial off the *agent cell's* `d.vars` counter, never the rank position), the user `name` alias on the record — no session resolver consults it, `learn_plan`'s replay key and the `d.patch` rediscovery join do |
| closed fragment | a fragment with **no program-side references that lack name-resolution**: every outside reference is a state-side cell — reached by a `cap$N` capture link (a captured field/member-var binding, §5.3) or carried as a ref-box slot's `int64` content on a `cells"…"` path — or is name-resolved by a `+d.refs` name-record carrying a qualified name (or, on a `run`-target record, the target head's label) or, for a program-side dim, by `+d.name` on the paired ref-capture record; a program-side reference carrying none is the refused class on every arm; the flag is set at emission and **re-verified by `learn` at accept time** (§6.6) |
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
resolve-only pass first stamps each `Identifier` with its binding's identity — scope id *plus*
binding ordinal within that scope, so two bindings in one scope never share a stamp (one scope
truth — the
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
run time. `run` reads the head's `+d.entry` itself — the operand slot is head-valued (§6.2) — and
pushes the §5.8 `#Call` **frame**: the call/frame convention, not a `Call`-opcode operand — `Call`
executes `toInt(inputs[0])` (`vortex_vm.cpp:854-856`), an entry-valued slot that cannot name a head.
Argument cells are frame-bound per activation, so recursion is safe; `run` spills and
restores the fragment's `result`/`produced` cells (recorded on the descriptor, §6.1) around the
call, and yields `T` — `()` for `Code<()>` — or fails when `produced` is false. The descriptor's
parameter cells are signature data for `validate`, never argument receivers. Two distinct failure
paths exist and are kept separate: `produced` false is expression failure; a fragment graph that
hits the shared `Fail` op backtracks *through* the `run` into the caller's choice structure (the
stack-top relink at `vortex_vm.cpp:238-243` is what makes the unwind sound). Fragment tails are
`Return`, not `Halt`: a `Halt` would park the cursor mid-statement (`vortex_vm.cpp:1220-1225`)
without popping the frame.
Suspending statements — `await`, `achieve`, `ask`, `yield`, effect calls — are refused inside
`code"…"` because a fragment has no step path (V4); U-M6 records the continuation future. `adopt`,
`emit`, `believe`, `install`, `graft`, `run` itself (a nested `run` pays a frame per level) and the
other non-suspending actions are allowed and stay impure. `run` is *not* re-entrant on
creation-bearing graphs: a fragment containing `cells"…%"` creations lowers to `New` with a
statically bound output (`compiler.cpp:793-799`), and `New` refuses a second use in append form and
re-links silently in non-append form (`vortex_vm.cpp:736-744`); the spill restores values, not
links, so `validate` refuses bound-output `New`s in any repeatedly-executable fragment (§7.2).
*Realist wins.* *Price:* a `run` costs a frame, O(arity) frame writes and a spill of two cells —
same as a method call; recursion pays a frame per level and dies at fuel.

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
symbol table, `d.clause` ranks, `+d.grab`/`+d.spin` attachments and `d.vars` slots are all cells of
the persistent base manifold — the program image folded from the artifact's low ops prefix (§4.5) —
which the arena reads through and shadows on write; `cells"…"` walks them, with multi-hop navigation
through `$` anchors (`cells"##/+d.stdlib"` then `cells"$c/+d.vars"`). The adapter hands `std:meta`
the program-root cell the artifact loader registered — the same binding mechanism V10 uses for the
thirteen dimensions. Read functions are pure and allowed in conditions; `test_of`/`body_of` wrap raw
clause graphs as `Code<Cell>` (a `run` on them yields a `Cell` handle to the produced cell — the
honest type at an unchecked boundary), and the wrapper heads carry `+d.entry → the wrapped graph's
root op` like every head — they are session-side and unpromotable by construction, so the uniform
head convention costs nothing. The memoization is *clause-side*: a `clause +d.code → +d.step` chain
of memo **records**, each
`{+d.values → the wrapper head, +d.name → the memo-kind label ("test"/"body") — a fresh text
cell per record (the makeCell(name) precedent, vortex_stdlib.cpp:4051), since an interned
label cell could hold only one `-d.name` edge and the second memo's would evict the first's,
+d.grab → a fresh scalar key cell holding the attach cell's identifying label}` — the
identifying label is the attach cell's `+d.name` text where it carries one; a label-less attach
cell (a bare op under the op-or-head resolution rule §6.1 states) has no key to write, so its
wrapper never memo-hits and is re-minted per call — and no memo record is appended for it either,
since a keyless record could never hit — the key is *content* compared by value, never a link to
the attach cell, because the attach cell's single `-d.name` slot could hold only one memo's key
edge and the other memo would evict it; and the record's `+d.values` occupies the wrapper's
single `-d.values` slot, safe because a builtin-minted wrapper carries no emission-record anchor
on that dim. `+d.step` chains the
records only, so a wrapper's own `+d.step` never enters `validate`'s op-graph walk, and
a key-mismatching memo is unlinked at re-wrap — so repeated reflection does not mint a fresh
~6-cell head per call, and `test_of`/`body_of` memos coexist on the chain rather than evicting each
other. The memo chain is never on the agent's `d.code` rank (§6.7): clause cells are program-side,
so the transit clause keeps wrappers off the durable set — they die with the incarnation at
`resume`, matching the session-handle rule they already are.
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
(`vagent-agent-language.md` §4.3, §V12); program cells are therefore never *republished* — they ride
the shared base lineage the checkpoint's spool prefixes onto (§4.5) — so a session graft is gone at
`resume`. The same lineage model **supersedes the spec §4.4's per-arena-load wording** — "each
agent's arena loads its own copy of the compiled program … after spawn and again after every
`resume`": the program image is the persistent prefix folded into the shared base, not a per-arena
copy rebuilt per incarnation, which narrows U-A14 to "the prefix *is* the shared base". This
document amends V12 **narrowly**: state may link to a *closed fragment graph* (the
`closed` descriptor flag — a fragment whose every outside reference is a
state-side binding cell or a name-bound program symbol, so linking it drags only the fragment, its
state captures and name-records the program can re-resolve — never the program skeleton). But the
link skeleton is not enough: a fragment's operand slots and `Jump`/`Call`/`Branch`/`Choice`
targets are `int64` *content* (`makeScalarCell(int64(target))`, `compiler.cpp:516-525`;
`vortex_core.cpp:384,402`), and `promote` copies Int64 content verbatim while translating only links
and handle-kind values (`OpHandle`/`ForeignOpHandle`, `arena_manifold.cpp:1934-1954`) — a promoted
fragment's internal references would silently alias unrelated cells. **P16** is the fix, and it is
adapter-side, needing no new mechanism beyond P4's already-approved wake (the empty-mailbox arm
mints a wake turn — the
register's approved-but-unimplemented `mesh_actor` change, `vagent-agent-language.md:1670`) **plus
the named lineage-store seam** — the adapter constructs and keeps the lineage `Store` (§6.6, §10.1),
base-injected at `spawn` like `resume` already does, and `resume` takes or verifies it, owed
adapter-API surface like P4's wake, not assumed — the
`mesh_evaluator.cpp:723-735` publication precedent already does *both* halves
`isCellRelocationHandle` tests for: `setValueBits(OpHandle, target)` *and* a `d.mesh.relocation`→
`"cell"` marker edge (`foreign_op_handle.cpp:98-117`; an unmarked `OpHandle` fails `promote`
outright, `arena_manifold.cpp:1797-1813`). Fragment emission marks every emitter-minted int64 ref
cell whose `+d.refs` record carries no `+d.name` — the uniform rule of §6.6 — with the inert marker
edge plus a `+d.ref` record on the head's `+d.refs` rank; the adapter's flag-driven ceremony inside
its own `Executor` callback re-kinds each marked cell `OpHandle` at the checkpoint-terminating
turn's end, `publishValue` runs between turns on `const&`, and a head-of-next-turn pass writes
`Int64(*handleTarget)` back so the continuing incarnation executes intact (the full protocol is
§6.6's); genesis replays `d.patch` **first**, then rehydrates `Int64(*handleTarget)` per marked
cell across each `learn`-registered head's records *and* the replay-installed copies' regenerated
records — the sweep runs last so name-records rebind post-installation symbols
(`resume` overlays the folded manifold without `importValue`'s remap, `mesh_actor.cpp:260-271`).
Without P16, `learn`/`d.patch`/`d.code` are session-only and the document says so wherever
they appear. A patch entry on `d.patch` holds the ordinal (the durable serial off the *agent
cell's* `d.vars` counter, never a
rank position), target descriptor, a `+d.patch-head`
chain of recorded head *names* and a `+d.name` label; `std:agent`'s genesis re-runs each patch's
verb after resume. *Nelsonian wins — the patch list is an edit decision list, the same shape the
test fixtures use.* *Price:* O(live fragment cells) promotion ops per checkpoint, the relocation
pass O(refs) per
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
   `d.patch-head`/`d.validate`/`d.weight`/`d.synapse`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/
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
| checkpoint (`publishValue`) | **not** republished — rides the shared base lineage | promoted iff reachable (e.g. `d.code`)            | **stale**: int names a dense id        | promoted (state cells)         | Int64 promoted verbatim (`arena_manifold.cpp:1934-1936`) — stale without P16                                       |
| `resume`                    | folded back verbatim with the lineage's image prefix | rebuilt if promoted; fresh ids                    | **meaningless**: stale or aliased ints | present; re-grafted at genesis | **aliased**: names whatever cell landed at the old dense id — rehydrated by P16's genesis pass, garbage without it |

Three rows deserve emphasis. Program cells do not *republish*: `promote` walks only what the agent
cell reaches, so the skeleton never enters the promoted set — that is V12 working as intended.
What carries the image is the **store lineage itself**, and the object is named: the adapter
constructs and *keeps* the agent's lineage `Store` itself, seeded at load with the artifact's
**sealed `ops` segment** (`sealedOpsName`, `provenance.hpp:286`). The segment *bytes* are
resolved like `restorePublication`'s own loop (`publication.cpp:1138-1162` — `metainfo` →
`Scroll::ofTorrentFile` → `resolver.resolve` → `isVerified` + length check) over the adapter's
content source, because `pub.opsSegments` are torrent descriptors, not bytes; the signed-inventory
checks that loop performs are skipped at seeding — covered by signature-on-decode plus the
genesis (a) band comparand — then applied into the
fresh store by `historyFromSeal` (`publication.cpp:1093-1103` — `applyOpsSegment`, `:1030+`, is
its file-local per-segment step, gating on the `\x7fXSO\x01` magic at `:1033`; `ops.nodes`, the
native `CompactOpNode` spool, is a different serialization and fails that gate) — and then
**given its span source like any restored history**, because `historyFromSeal` is the bare fold
and installs none: `setContentSource` on the adapter's content source (the lease pinned for the
actor's life, the role `actor->sourceLease` plays at `mesh_actor.cpp:268`) — the read-path
half, without which every artifact-local span resolves to `""` — the
remap makes *no* span local, `publication.cpp:1049`,`:1086` — so `initGenesis`'s per-dim
`mintNamedDimension`→`baseDimensionNamed` miss every folded dim name and mint ephemeral twins,
and every label and name-resolution read dead-reads — plus
`bindPublishedLocalScroll(store->addScroll(from))`, `restorePublication`'s own pair at
`publication.cpp:1129-1131` — `addScroll`'s `sameContentAs` dedup making the re-add return
`from`'s fold-internal id (`store.cpp:1388-1405`; `externals[0]` on a fresh store) — the
scroll-identity half: inert on reads, but without it a later save round-trip hits
`sealPendingMetadataAsCells`' `publishedLocalScroll_ == localScroll` arm and mints
`registerScroll` ops for `self` into the history (`store.cpp:4081-4089`; its other readers are
`scrollRegistry`'s resolved-scroll set, `manifold.cpp:1503`, and the storeTables round-trip,
`store.cpp:4176`,`:4454`) — the store is then the lineage's low
prefix, *position-stable, not byte-identical*: a segment's scroll table remaps into the receiving
store's `ScrollId` space (`publication.cpp:1048-1086`, `:1030+`), so the requirement is the same
op sequence at `1..E_ops` plus resolvable scroll names, not identical stored bytes (the
`job.base` shared-store pattern, `mesh_jobs.cpp:304-318`). Artifact bytes are shared; **store
objects never are** — two agents off one artifact each hold their own `Store`, and whether they
share the artifact's `storeId`/`documentId` lineage is a *construction choice*: `documentId_` is
`Store`-ctor-or-load state (`store.hpp:133`,`:1435` — the two-arg ctor writes it,
`store.cpp:135`, and the store-tables load rewrites it on a directory open, `store.cpp:4450`;
the segment apply carries ops, never identity).
The fresh-mint arm is the mechanized one: `historyFromSeal` mints `Store()` internally with no
permascroll or `DocumentId` parameter (`publication.cpp:1097`, decl `publication.hpp:605-612`),
`applyOpsSegment` is file-local so the apply cannot be re-run around a pre-minted store, and the
`Store()` mints a fresh `UserPermascroll` (`store.cpp:124`) — so in v1 each agent takes its
own document lineage, while adopting the artifact's id —
`Store(userPermascroll, artifactDocumentId)`, `restorePublication`'s precedent
(`publication.cpp:1127-1128`) — is the **unmechanized arm**, owed as a `historyFromSeal`
overload or a `restorePublication` seed (which would perform the span-source pair *and* adopt
`pub.storeId`, but consumes a signed `Publication` rather than bare segments); when it lands it
reproduces the divergent-histories-under-one-identity exposure
`job.base` already carries (a pre-existing exposure of the pattern, not a new one). A new artifact
is a new `programRef`, so the continuation pin (`grant->programRef == continuation->programRef`,
`mesh_actor.cpp:215`) makes an upgrade respawn, never migrate mid-chain. Checkpoints append above
the prefix, and `sealLocalSpool` seals the whole spool in the checkpoint path — `publishValue`
passes the default `opsAlreadySealed = 0`, so the sealed segment covers `1..opsTotal`
(`mesh_value.cpp:227-228`, `:241`; `publication.cpp:611-617`, `:664-672`) — so the prefix's
positions `1..E_ops` ride inside every checkpoint's ops segment and fold back verbatim —
`rebuildManifold` assigns `birthOp = opIndex` (`manifold.cpp:427-442`; a persistent `CellRef` *is*
a spool position, `manifold.hpp:295`). That is why session grafts are ephemeral and `learn` needs
`d.patch`. Ref-box ints are *session handles*: `promote` mints store cells in reachability order,
not mint order (the BFS at `arena_manifold.cpp:1744-1788`, structure at
`arena_manifold.hpp:235-240`), and `resume` rebuilds an arena whose dense numbering need not match
the old one — a stale `Int` can name the wrong cell silently. And the last column is the one v1.0
missed: the same int-content mechanism is *inside* every fragment graph, so a promoted fragment
that survived reachability still executes garbage until
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
   definition-site scopes (the resolve-only pass's binding-identity stamps — one scope truth,
   §5.6); every free
   name that resolves to an enclosing *value* binding is renamed in the body AST to a fresh `cap$N`
   identifier — unlexable in source (`$` is a lexical error at `lexer.cpp:333-335` and absent from
   the identifier charset, `lexer.cpp:14-24`), and the checker *also* refuses `$`-bearing names on
   user-supplied decls, so no programmatically built `Program` can squat on the namespace either
   (`snake()` accepts `$` today, `checker.cpp:30-36` — the guard is §10.1's). The rename covers
   *value bindings only*: program-member references (`def`/method/plan/goal names) stay
   name-resolved at emission and carry `+d.refs` name-records (§6.6) — they are never
   capture-renamed. The renamed `cap$N` keeps a resolve stamp naming the *captured binding
   itself*, so the checker resolves it straight to that binding under §5.6's stamp-honoring rule;
   the `{cap$N → captured-name}` env records `expand` also inserts at the literal's position are a
   diagnostics and readability aid — `cap$N` renders in messages as the captured *name*, never
   `'cap$3'` — not the resolution channel. `cap$N` is a **pure
   binding-table alias, not a synthesized `let`**: `ExprKind::Let` lowers to `emit(initializer →
   fresh localCells_ slot)` (`compiler.cpp:705-707`, `:853-855`) — a value copy that would snapshot
   the capture where the contract is cell identity — and `validName` would refuse the name at kernel
   preflight anyway (`compiler.cpp:407-417`). Fragment emission and the merged emission resolve
   `cap$N` through `CompilerBindings` to **one
   cell: the captured binding's own cell**. For fields and member vars — the *stable* class —
   that is true cell identity and reads stay live. Frame-bound bindings — `def`/plan/handler
   parameters *and* `let` locals — are the ABI-debt class: under the §5.8 frame convention a
   local's cell is per-activation like a parameter's (the scalar kernel's static `localCells_` is
   the interim no-call shape, `compiler.cpp:853-855`), so a captured local is orphaned at `Return`
   and reads back the last-written value — the same observable rule parameters get. For that class
   there is no emission-time cell to name: the `cap$N` alias is compile-time — it is the
   capture-slot edge's *target* that is established **per-activation by frame binding** (the ABI
   debt), and `cells"$cap$N"` anchors are
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
                             binding identity      (cap$N bindings)          compute T, purity,     refs rank + recipe +
                             (scope+ordinal)
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
- The **resolve-only pass** stamps each `Identifier` with its binding's identity — scope id plus
  binding ordinal within that scope, so two bindings in one scope never share a stamp — the
  checker's env construction extracted into a shared helper it reuses, so `expand` never
  re-implements the
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
- The **checker** then types the merged tree once — honoring the resolve pass's binding-identity
  stamps
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
level; `learn`'s closedness refusal names the offending captures or name-refs — its lineage
refusal reports *no program lineage*, distinct from closedness (§6.6's fail-closed arm) — and
template-body errors report the call site with the `def`'s location noted (M13).

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
`d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.refFor`, `d.mesh.relocation`, and the
`validate` constants (the
`OpcodeKind::Linked` ceiling cell, the per-op arity table and the impure-kind set §7.2 requires —
adapter-bound data, not prelude
literals, because the enum's integers are serialized data that a later opcode would stale).

The fragment head layout (existing dimensions plus the data-minted `d.entry`, `d.refs`, `d.ref`,
`d.recipe`, `d.refFor`, `d.mesh.relocation`; `d.name` is `initGenesis`'s,
`vortex_core.cpp:61`):

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
  │                  a later +d.values write could evict the marker — link
  │                  eviction, arena_manifold.cpp:1347-1377 / M14)
  │                  +d.name→qualified name or head label ⇒ NAME-record (no marker,
  │                  never re-kinded; +d.grab→"call"/"run" role label picks the
  │                  writeback — int64(entryOp) for call-role, int64(head) for
  │                  run-role — the +d.name *shape* picks only the resolution
  │                  path: qualified or verbatim-label — §6.6)
  │                  +d.name absent           ⇒ REF-record (full P16 ceremony)
  ├─(+d.recipe)─► reify recipe chain — per-field records (link tuples + literals
  │                + capture records + ref-capture records — one per +d.refs
  │                ref-record: +d.refFor→the same slot cell its +d.ref names
  │                (dedicated dim — a first-position input slot's -d.grab is the
  │                opcode's wing edge; +d.grab here would evict it); the referent
  │                rides CONTENT, never an edge into it — internal: the slot's
  │                int64 rebinds through the old→new map (+d.values on the record
  │                may name it, one record one edge, only where -d.values is
  │                free); external: the slot's int64 / *handleTarget verbatim;
  │                program-side: +d.name→the dim name), one record per field -- §6.1 reify
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
| `symbol_named(owner: Str, name: Str): Opt<Cell>`                                   | pure                       | `Opt<Cell>`                           | a `d.vars` symbol cell on the named module — all symbols live on a module's `d.vars` (spec §5.7): goals/events as bare names, members as `Agent.member`; `owner` is the module only                |
| `clauses_of(cell: Cell): [Cell]`                                                   | pure                       | `[Cell]`                              | the `+d.clause` rank — plans of a goal, handlers of an event type                                                                                                                               |
| `test_of(clause: Cell): Opt<Code<Cell>>`                                           | pure                       | `Opt<Code<Cell>>`                     | wraps the clause's `+d.grab` graph as a `Code` handle — wrapper memoized clause-side, keyed by the current attach cell (§6.1)                                                                    |
| `body_of(clause: Cell): Opt<Code<Cell>>`                                           | pure                       | `Opt<Code<Cell>>`                     | wraps the clause's `+d.spin` graph (same session-side memoization)                                                                                                                             |
| `plan_named(qualified: Str): Opt<Cell>`                                            | pure                       | `Opt<Cell>`                           | a clause cell by its qualified `module:Agent.plan` name — matches every clause `+d.name` label verbatim — learned clauses included, by their minted `patch:<n>` spelling, never by the user alias; declared handlers carry no `+d.name` to match (§6.6, spec V15)                                                                   |
| `code_named(label: Str): Opt<Code<Cell>>`                                          | pure                       | `Opt<Code<Cell>>`                     | `d.code` rank lookup by `d.name` label, falling through on a miss to the program-image label walk — so literal heads re-resolve by the same call (§6.7); the space is *head labels only* — qualified names are `symbol_named`/`plan_named`'s space, clause labels `plan_named`'s                                                                                                                  |
| `label_of(c: Cell): Opt<Str>`                                                      | pure                       | `Opt<Str>`                            | reads a `+d.name`-bearing cell's label — a head's `+d.name` or a learned clause's `patch:<n>` (the rediscovery read the `clauses_of` idiom ends in) — the cell reaching the `Cell` parameter through `cells"…"` (`cells"$a/+d.code"` over the registered rank, `a := agent_cell()` — `d.code` hangs on the agent cell, not arena `home`, so `##` reads an empty rank; `cells"$clause/+d.spin"` for an installed copy; a clause cell from `clauses_of` directly — all three paths yield `[Cell]`, so the caller indexes/`for`s down to the single `Cell`), since the head-producing reads yield `Code<Cell>` and no `Code`→`Cell` coercion exists (M3)     |
| `validate(c: Cell): Bool`                                                          | pure                       | `Bool`                                | the well-formedness walk of §7.2 — per-op `maxCycles`-bounded, seen-set on `d.validate`                                                                                                         |
| `weight_of(cell: Cell): Opt<Float>`                                                | pure                       | `Opt<Float>`                          | a `d.weight`/`d.vars` scalar read (§8)                                                                                                                                                          |
| `value_of(c: Cell): any`                                                           | pure                       | `any`                                 | reads a `Cell`'s content — the `Code<Cell>` companion: `run` yields the produced cell, `value_of` reads its bits (§6.2)                                                                           |
| `next_rand(seed: Cell): Int`                                                       | pure                       | `Int`                                 | wrapping-safe mix over `Int` ops (32-bit halves via `DivMod`); the seed lives in a state cell (§8.6)                                                                                            |
| `run(c: Code<T>, args): T`                                                         | opaque-impure              | `T`; checker special form — `args` a list literal, per-element `any` | §6.2 — frame-per-activation call, `Return` tails only; a bare-`Code` actual yields `unknown`                                                                                                    |
| `reify(c: Code<T>): Code<T>`                                                       | impure                     | `Code<T>`                             | replays the head's `+d.recipe` — link tuples + literal contents + old→new map + regenerated `+d.refs` records and fresh per-ref-cell `d.mesh.relocation` markers + `+d.recipe` — minting a fresh copy, closed iff the source was (§6.1) |
| `graft_plan(goal: Str, name: Str, test: Code, body: Code<()>)`                     | impure                     | `()`                                  | mint a named clause cell, `+d.grab`→test head, `+d.spin`→body head, append to the goal's `d.clause`; test must be `pure`                                                                        |
| `graft_handler(event: Str, name: Str, guard: Code, body: Code<()>, priority: Int)` | impure                     | `()`                                  | same shape on the event type's `d.clause`, priority scalar on `d.vars`; guard must be `pure`                                                                                                    |
| `graft_where(clause: Cell, test: Code)`                                            | impure                     | `()`                                  | re-point the clause's `+d.grab` at the test's head; test must be `pure`                                                                                                                         |
| `graft_body(clause: Cell, body: Code<()>)`                                         | impure                     | `()`                                  | re-point the clause's `+d.spin` at the body's head                                                                                                                                              |
| `graft_method(owner: Str, name: Str, code: Code)`                                  | impure                     | `()`                                  | unlink any existing `owner.name` symbol (redefinition-as-replacement), then symbol cell on the module's `d.vars`, `+d.values`→head — callable by name; `code`'s effect-ness must match the host member (fragments are never effect members, so an effect host refuses) |
| `prune(clause: Cell)`                                                              | impure                     | `()`                                  | unlink a clause from its `d.clause` rank (predecessor→successor repair, the spec §5.1 removal idiom)                                                                                            |
| `learn_*` (each `graft_*` verb's durable twin)                                     | impure                     | `()`                                  | reify twice — graft one fresh copy, register a second fresh `reify` on `d.code` (the program-side original is never registered), record `d.patch`; a clause-installing `learn_*` mints the installed clause's `+d.name` itself as `patch:<n>`, `<n>` off the *agent cell's* `d.vars` serial — the `name` argument rides the patch as an alias (§6.1); an argument already `d.code`-registered is reused as its own registration — the second `reify` is skipped (§6.6's carve-out); closedness re-verified at accept (§6.6); **conditional on P16** |
| `unlearn(clause_or_symbol: Cell)`                                                  | impure                     | `()`                                  | undo the latest patch whose target's resolved name equals the argument's: unlink a learned clause/symbol, or re-point via a `reify` of the named prior head — none recorded for program-original attachments; a `d.patch` record argument is refused by its `-d.patch` backlink; dead patches drop with a fault record (§6.6) |
| `set_weight(cell: Cell, w: Float)`                                                 | impure                     | `()`                                  | write the `d.weight` scalar through the focus slot                                                                                                                                              |
| `sprout_synapse(from: Str, to: Str, w: Float)`                                     | impure                     | `Cell`                                | mint a junction cell on the agent's `+d.synapse` rank naming the two qualified-name labels, `+d.weight`→its own `w` scalar (§8.1)                                                              |

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
a reconstruction template — the `(from, dim, dir, to)` link tuples, each endpoint field pointing
`+d.values` at the actual endpoint cell under the same one-record-one-edge rule the ref-capture
arm below states — a claimed `-d.values` taking no edge, the endpoint then riding the field's
`int64` content for the old→new map to translate either way (an op cell is `to` of one tuple and
`from` of the next, so shared endpoints are routine) — never a census integer (checkpoint
renumbers dense ids, §4.5), the literal contents, the capture records, and the
**ref-capture records** — one per `+d.refs` ref-record, pairing the slot with its referent:
`+d.refFor → the same slot cell` the ref-record names by `+d.ref` — a **dedicated correlation
dim**, because every reusable candidate evicts live structure: a second `+d.ref` cannot correlate
them (the slot's `-d.ref` backlink is single-occupancy, M14's rule), and `+d.grab` is worse —
a *first-position* input slot's `-d.grab` is already the opcode's own wing edge (`bindInput`
writes `opcode +d.grab → slot`, `vortex_core.cpp:384-390`), so a `+d.grab` correlation evicts the
wing head and `inputsOf` reads empty (`wingOf` enters through `linked(opcode, dims_.grab, POS)`,
`vortex_core.cpp:366-369`) — the op loses its whole input wing at emission and again at every
`reify` replay. The slot's `-d.refFor` is free by construction instead, and a slot pairs with at
most one ref-record, so `-d.refFor` never contends. The referent itself is carried **as content,
never as an edge into the referent**: any such edge lands on a shared or already-occupied
backlink — a state-side referent's `-d.values` may already carry a live edge (a `d.vars`
binding's cell *is* a `+d.values` target), and two records naming one referent collide on the
same single-occupancy `-d` slot, M14 on both counts — so the shape splits by referent class.
An *internal* referent (a cell inside the head's own graph) rebinds through the old→new reify
map: the slot's `int64` names the source position and the map, which covers every copied graph
cell, translates it to the copy twin — map membership *is* the internal/external discriminator —
and the record's `+d.values` *may* name its own endpoint cell so the recipe stays
self-describing, one record, one edge, only where the endpoint's `-d.values` is unclaimed (a
shared or occupied endpoint takes no edge, the slot's content carrying it regardless). An
*external*/state-side referent rides the slot's own `int64` ref position in-session and
`*handleTarget` after promotion — the identical bits genesis (e) already writes, `promote`
translating the position — verbatim, the slot *being* the endpoint record, no edge at all.
A *program-side* referent (a `dimConstant` slot naming a folded dim — `findOrMintDimension`'s
arena-local id, `compiler.cpp:769-772`) keeps `+d.name → the referent dim's name string`,
resolved at replay through `ensureDimension`'s name arm (`arena_manifold.cpp:78-105`) — an edge
to a fresh name cell, never to the referent: a record on a `d.code`-registered head linking into
a program-skeleton cell is the `d.code`-rank/`closed` violation the invariant exists to refuse.
The frontier stays enumerable — `closed` reads each ref-capture endpoint through the paired
slot's own content (§6.6), a `+d.name`-bearing program-side record's endpoint being the named dim
rather than a cell and counting as name-resolved — and `reify`
replays it through `New`/`Link`/`Value`, building an old→new association that rewrites every copied
ref-box `int64` to the copy's cells and remaps every tuple endpoint through the same map.
**External captures are preserved verbatim** — the copy's capture slots carry the *same*
definition-site positions the source's did for external referents, and name the copy's own twins
for internal ones through the old→new map, so the copy is `closed` iff the source was. `reify`
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
take `name` parameters — under `graft_*` the argument is held on a `d.name` label verbatim, while
under a clause-installing `learn_*` (`learn_plan`, `learn_handler`; `learn_method`'s `name` is the
symbol's functional lookup name, never a mint) the clause's `+d.name` label is the verb's own mint,
`patch:<n>` — `<n>` the patch's **durable monotone serial**, drawn from the same per-agent
counter on the *agent cell's* `d.vars` rank `reify`'s `<serial>` already rides (§6.7), never the
rank position: `unlearn`
drops and replay-drops renumber the rank, and a recycled ordinal would re-mint a live `patch:<n>`
and misbind every descriptor recording it — only the verb can mint it, since the serial belongs to
the patch the same call appends — and the user `name` argument rides the patch record as an
*alias* — each scalar field its own `+d.vars → [key] +d.values → scalar` record (§8.1's shape),
keyed beside `ordinal` and `priority`, never a shared `+d.vars` slot — never the `+d.name` label
and **never a
session lookup key**: no session resolver consults the alias — the `d.patch` rediscovery join
below and `learn_plan`'s replay dedupe do (`learn_*` returns `()`, so the minted serial is never
handed back either) — and a learned clause is re-found by `clauses_of` + `label_of`
filtered to the `patch:` prefix, or `plan_named("patch:<n>")` once `<n>` is known — `<n>` read off
the patch *record's* `ordinal` field, joined from the alias by matching the `alias` key-record on
each `d.patch` member (`cells"$a/+d.patch"` — the only name→clause join; `clauses_of` +
`label_of` enumerates learned clauses without it). The join yields *every* match — aliases are
not deduped (`learn_plan`'s dedupe hits stored clause `+d.name`s only and `patch:` labels are
unreachable), so two live patches can share one alias and the caller picks by `ordinal`,
latest-wins — `label_of`
reads a clause's `+d.name` the same way it reads a head's. `patch:` is refused as a `name`
argument at the `graft_*`/`learn_*` call — reserved for the verb's minting, the same reservation
§6.7 gives it at module declaration.

`graft_method`/`learn_method` dedupe by name: an existing same-named symbol is unlinked first
(redefinition-as-replacement, since the `d.vars` name walk is first-match-wins,
`vortex_core.cpp:100-127`). `learn_plan` dedupes its *alias* verbatim against the clause
`+d.name`s **of the install goal's own `d.clause` rank** — the rank the patch records as the
attach parent, never a global scan, which could unlink a clause off a rank the patch does not
record — and the match is *any* verbatim hit: declared or grafted, qualified or unqualified,
since a grafted clause holds its `name` argument on the label verbatim (a learned clause's
`patch:<n>` label is unreachable — `patch:` is refused as a `name` argument). A hit unlinks the
first-matched clause and installs `patch:<n>` — so `learn_plan("Patrol",
"demo:Guard.cautious", …)` unlinks §9.2's declared `cautious` clause, the qualified name then
resolving to nothing (the same dead-name exposure `prune` already leaves in `tried`'s name set),
and name-preserving replacement of a declared clause goes through
`learn_where`/`learn_body`, which keep the named clause — while an alias spelling
no live `+d.name` *appends* under its own `patch:<n>`: the structural asymmetry against
`learn_method`, whose symbol's content *is* the user name and so genuinely replaces. The unlink
is **durable through the alias**: on a dedupe hit the record's `alias` field *is* the superseded
clause's `+d.name` verbatim — the dedupe match was verbatim equality — while on an append it
records the user name with nothing displaced, displaced-vs-appended staying auditable for a
*declared* victim — its `+d.name` folds back in the `1..E_ops` band for the join to find; a
session-grafted victim mints no `d.patch` record of its own, so nothing proves the alias ever
named anything and the record reads append, which is also what replay makes of it — so genesis (d)
replays each patch's *verb*, not a bare attach — `learn_plan`'s replay
first re-matches the recorded alias verbatim against the re-attached
rank's clause `+d.name`s and unlinks the hit — the alias is replay-keyed, no session resolver
consulting it — and a dedupe-displaced declared clause, which folds back at `1..E_ops`, is
displaced again rather than resurrecting beside `patch:<n>`; the replayed dedupe re-derives its
victim from the folded rank, the verbatim alias match *at replay time*, which need not be the
clause the session unlinked — a session hit on a grafted twin displaced nothing durable (grafts
never fold back), so the replay re-matches the folded rank, appending where no same-named clause
survives and displacing where one does, while a session append replays as a displace when an
intervening displacer's patch was replay-dropped on its own dead target — deterministic either
way, canonical-rank correct;
`learn_method` replays its replace for free because
`graft_method`'s unlink-first is part of the grafted verb itself (`learn_handler` carries no
dedupe of its own, so its replay simply appends). In-session, `unlearn` of the displacing patch
does **not** re-link the displaced clause — it stays dead for the session and returns at the next
genesis only if the displacing patch was unlearned first (no replay, no re-unlink).
`learn_handler` is likewise
additive — a second same-alias clause appends under its own `patch:<n>`, `priority`
differentiating the pair at selection — `plan_named` by the alias hits *neither* (zero hits,
never an ambiguous first), and `unlearn` picks each twin precisely by its minted label. The
surviving `+d.name` ambiguity is any same-named pair a verbatim `graft_*` `name` can create —
graft-on-graft, or a graft shadowing a declared clause —
where `plan_named(name)`'s first-hit and `unlearn`'s latest-name match alike can misbind; and
`unlearn`'s LIFO-by-name survives only among patches recording one *target* name — stacked
replaces on one
clause, which is undo order, not ambiguity.

Every read function is honest about what the graph can prove: `test_of` returns `Code<Cell>` because
a program-side test graph has no compiler-emitted descriptor — its produced value's type is not
statically known, so `run` on it yields a `Cell` handle to the produced cell. Upgrading that to
`Code<Int>` is a `validate`+re-descriptor step the caller requests, not an assumption.

### 6.2 `run`: a fragment is a same-cursor call

`run(c, args)` is a **checker special form** (M6): `args` is a list literal typed per-element by a
fresh `any` wildcard — heterogeneous argument lists are legal here and only here — with a
compile-time arity check against the fragment's signature when the fragment is statically known,
and a run-time descriptor arity check otherwise. It lowers to: read the head's `+d.entry` for the
entry op — the operand slot is **head-valued by construction**: the emitter writes `int64(head)`
into it, and `run` derefs the head's `+d.entry` *and* `+d.values` (the descriptor holding the
parameter cells and the `result`/`produced` cells it spills) at run time, so the slot names the
head, never the entry op; evaluate each element in order and bind it on a fresh per-activation
`#Call` frame *by parameter name, as a value* (the frame writes of `vortex_vm.cpp:854-893`, the
same mechanism method calls use — values, not ref-boxes); push the frame whose successor is the
post-`run` op; jump to the entry op.
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
(a program `def` tail) is reflected, not `run`. The same check names the **headless-`run`**
refusal: `run` of a head-less program `def` *symbol* — one whose `+d.values` names an entry op,
never a fragment head — cannot satisfy the head-valued slot convention at all, so the call is
refused at the descriptor lookup, not bound and then failed (§6.6's rebind arm hits the same
class). A `plan_named`/`symbol_named` result belongs to this class only by analogy — it can never
reach `run` at all: both builtins return `Opt<Cell>` where `run` demands `Code<T>`, and M3 bars the
`Cell`→`Code` upgrade — so the session-side surface for it is the type error, and the durable-side
one the record refusal of §6.6. `value_of(c: Cell): any` is the companion for
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
  O(length)). A bare `goal`/`event` name is self-implicit — resolved on the agent's own module's
  `d.vars` (`symbol_named` taking the module as owner, §6.7's rule); a `module:Name` spelling
  resolves cross-module on the named module's `d.vars`, the same first-`:` split §6.7 gives
  qualified names.
  `graft_plan`'s body/test signatures are the *goal's* formals — a plan fragment must
  declare (or be `Code<()>`-shaped against) the same parameter list *the goal's other clauses
  share, names included*, since selection binds the goal's arguments by name at every clause.
  `graft_handler`'s formals are the handler's bound names: the event cell under `occurrence` — a
  minted clause binds `occurrence`; the `as`-name arm applies only when `graft_where`/`graft_body`
  re-point a clause that declared one — event fields are read through the bound
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
  attach does). For `graft_method`, `owner` is the `Agent` qualifier — optionally
  `module:`-prefixed: a bare `Forager` resolves against the agent's own module, `demo:Forager`
  cross-module — and the
  symbol is minted on the resolved module's `d.vars` under the dotted `Agent.member` spelling
  (spec §5.7), the same first-`:` module split §6.7 gives `symbol_named`.
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
   *consistent* precisely because no map entry exists (their content is their kind). **Registration
   on the image bind:** `opcodeMap_` is VM-private per `WorkerContext` and holds none of the
   folded image's op cells on every fresh
   context — at the spawn-time injection as after `resume`; the
   folded image's op cells are re-registered by a `setOpcodeKind` pass over exactly the recorded
   instruction enumeration of folded cells at
   genesis (`vortex_vm.cpp:98-100`, the same `ensureDimension` window — genesis step (b), §6.6).
   Re-running `mintOpcode` is the other wrong shape; the `asInt64 ≤ Linked` value scan §6.6's
   step (b) rules out is the first: `mintOpcode` *mints* a fresh ephemeral cell
   (`vortex_vm.cpp:73-82`), so it would register twins of the program ops rather than the folded
   cells themselves, leaving every persistent op cell outside P15's membership test. A kind rewrite
   of a *never-registered* cell is not refused — but it is also harmless-by-decode
   (content-is-kind), so the guard's coverage gap is exactly the set of cells where a desync cannot
   form.

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
  "program-side" means *both* reachable from `program()` by a bounded, **outbound-following** walk
  over the program's **full structural dim vocabulary** — `d.stdlib`, `d.vars`, `d.clause`,
  `d.extends`, `+d.values`, `+d.entry`, `+d.name`, `+d.refs`, `+d.recipe`,
  `+d.grab`, `+d.spin`, `+d.step`, `d.contract`, `d.source` — `d.contract` staying in the
  program list because its `-d.contract`/`+d.contract` precondition/postcondition chains anchor on
  *op cells* (`vortex_stdlib.cpp:603`,`:628`), `d.source` because it anchors on clause cells —
  plus, on the *agent
  cell's own* legs, `+d.code`; every edge outbound
  (`+d`); the bare `d.` spellings in this list are the *rank* dims, whose membership edges run
  `rankTail +d → member` like the named `+d.` cell edges (`vortex_core.cpp:113-126`), the
  shorthand naming the dim, not a direction — `d.name` is not one: it is a single-link label dim
  (`vortex_core.cpp:61`), already covered as `+d.name` — every dim the program
  skeleton hangs on is in the walk — `d.dims` is the one named exclusion, matching the checkpoint
  rule (`promote` skips `d.dims` edges, `arena_manifold.cpp:1766`; genesis's `ensureDimension` is
  find-or-mint — `dimensionNamed` resolves base dims and the `arenaDims_` cache before any mint,
  `arena_manifold.cpp:78-105`, so a promoted dim cell re-binds rather than re-mints) — and the
  vocabulary spans **two roots**: `program()` and `agent_cell()` are distinct adapter-bound roots
  (§6.1's binding list) never `+d`-reached from each other, so the agent cell's own legs —
  `+d.code` and its rank dims (`d.patch`, `d.synapse`, `d.vars`, `d.belief` and the
  spec's other *agent-cell-anchored* ranks — `d.weight` is no rank, it is the §8.1 scalar edge
  dim on junctions and `d.vars` records; `d.contract` is not one — its
  `-d.contract`/`+d.contract` chains anchor on op cells, `vortex_stdlib.cpp:603`,`:628`, already
  enumerated by the `program()` walk) —
  are enumerated only by a second walk rooted at `agent_cell()` itself. The sideness test needs
  no such walk — nothing an agent-rooted walk reaches *through a link beyond its root* can
  classify program-side, since V12's
  state→program-skeleton link bar stops the walk at the boundary — and when `agent_cell()`
  itself sits `≤E_ops` (the exempt case below) its image legs classify by ref position like
  every other cell — and
  the walk's consumer
  is `closed`'s frontier enumeration, nothing else; the
  bound is at least skeleton depth — *and* the cell's ref is a **persistent (non-`ephemeralBit`)
  ref at or below the recorded program-image extent**. The
  discriminator is ref-space ordering, not a timestamp, and it stands on a **named store
  lineage** — and the object is named too: the program image lives as the agent store's **low ops
  prefix**, and the store that carries it is **the adapter's own**. The adapter constructs and
  *keeps* the lineage `Store` itself — the spawn-side anchor whose role §4.5 splits from the
  verify destination — seeded at spawn-time with the artifact's **sealed `ops`
  segment** (`sealedOpsName`, `provenance.hpp:286`), applied into the fresh store by
  `historyFromSeal` (`publication.cpp:1093-1103` — `applyOpsSegment`, `:1030+`, is its
  file-local per-segment step; `ops.nodes`, the native `CompactOpNode` spool, is the other
  serialization and fails its `\x7fXSO\x01` gate, `:1033`) — and then **given its span source
  like any restored history** (`restorePublication`'s hydration pair — a different operation
  from step (e)'s `OpHandle`→`Int64` *rehydration* below), since the bare fold installs none:
  `setContentSource` on the
  adapter's content source plus `bindPublishedLocalScroll(store->addScroll(from))` —
  `historyFromSeal` does not return `self`; `addScroll`'s `sameContentAs` dedup returns `from`'s
  fold-internal id (`store.cpp:1388-1405`), `restorePublication`'s own
  pair at `publication.cpp:1129-1131`, without which every artifact-local span — all `d.*` dim
  names, `+d.name`/`src:` labels, symbol names — resolves to `""`, `initGenesis` mints
  ephemeral twin dims and every name-resolution path dead-reads — *position-stable, not
  byte-identical*: a segment's scroll table remaps into the receiving store's `ScrollId` space
  (`publication.cpp:1048-1086`, `:1030+`), so the requirement is the same op sequence at
  `1..E_ops` plus resolvable scroll names, not identical stored bytes — the `job.base`
  shared-store pattern
  (`mesh_jobs.cpp:304-318`). The seeded store's identity is a construction choice named at
  §10.1's step: `documentId_` is `Store`-ctor-or-load state (`store.hpp:133`,`:1435` — the two-arg
  ctor's `store.cpp:135` or the store-tables load's `store.cpp:4450`; the segment apply carries
  ops, never identity), and the mechanized arm is fresh-mint-and-record —
  `historyFromSeal` mints `Store()` internally with no permascroll or id parameter
  (`publication.cpp:1097`; `applyOpsSegment` is file-local, so the apply cannot wrap a
  pre-minted store), each agent its own document lineage — while adopting the artifact's id —
  `Store(userPermascroll,
  artifactDocumentId)`, `restorePublication`'s precedent (`publication.cpp:1127-1128`) — is the
  *unmechanized* arm, owed as a `historyFromSeal` overload or a `restorePublication` seed
  alongside the supply-side seam (the divergent-histories exposure `job.base` already carries,
  a pre-existing exposure of the pattern, not a new one). The runtime never hands the store
  object out:
  `spawn()` creates and
  accepts no store (its signature, `mesh_actor.cpp:119-122`/`mesh_actor.hpp:132-135`; the
  `WorkerContext` mint at `:156`), `resume()` builds one internally and keeps it private
  in `Actor` (`mesh_actor.cpp:260-269`) — though it already transports a caller `Store` inside
  `ReceivedValue checkpoint` (`mesh_actor.cpp:182`, first read at `:188`'s `!checkpoint.store`
  guard, `:261`, read only for
  `userPermascrollPtr()` and dropped before `receiveValue`'s rebuild, so the owed change is the fold
  *trusting* the supplied store or comparing it, not new transport — and `checkpoint.store` is
  a `unique_ptr`, `mesh_value.hpp:98-105`, so "supply" of the adapter's *kept* store implies
  shared ownership or a second seeded product; the seam names the former) — and neither
  `checkpointView` nor the `Executor`
  signature exposes a store (`mesh_actor.hpp:102-106`, `:113-116`) — so the adapter's seeding is
  the §10.1 step that owns the store, bound **before the `WorkerContext`/`initGenesis` freeze**:
  `initGenesis` runs inside the `VortexCore` ctor (`vortex_core.cpp:14`) and adopts
  `*base->home()` and the base dims only when a base already exists (`:30-43`), and
  `initGenesis`/`home_` are private with no re-genesis door (`vortex_core.hpp:268`,`:282`,`:287`)
  — so a post-construction `attach(Space)` (`arena_manifold.cpp:274-287` installs
  `base_`/`store_` when unset) delivers the arena pointers but cannot re-run genesis, leaving
  `core.home()` and the session dims anchored to the base-less arena for the whole
  first incarnation (`opcodeMap_` is not `initGenesis`-frozen at all — it is VM state populated
  by `VortexStdLib`'s `mintOpcode`s during `WorkerContext` construction, and the *image*
  entries it lacks are what genesis step (b) re-registers). The spawn arm therefore takes
  **resume's own injection shape**: the seeded
  store folded to a `Manifold` the adapter pins `unique_ptr`-stable for the arena's life
  (`ArenaManifold` holds `base_` as a raw `const Manifold*`, `arena_manifold.hpp:752`),
  constructing the context
  through the `WorkerContext` two-arg form (`worker_context.hpp:52-54`) — `resume` itself
  passes `base` alone and installs the reader post-construction via `arena.setDefaultSpanReader`
  (`mesh_actor.cpp:271-273`); the spawn arm supplies both at construction — after
  which `initGenesis` adopts the image's home/dims identically at spawn and `resume`, and the
  asymmetry collapses to the `resume` arm, where the adapter's seeded store is only the
  **spawn-side lineage anchor** — the received checkpoint's band lives in the runtime's own
  rebuilt store — so the resume seam is **supply-or-verify against the store holding the
  received band**: under the supply arm the runtime folds the adapter's lineage store as the
  rebuild's backing store; under the verify arm the comparand is the runtime's base store's
  own `1..E_ops` band, reached through `arena.base()->store()` — the mutable reach
  (`Manifold::store()` is `const noexcept` returning non-const `Store*`, `manifold.hpp:486`;
  `ArenaManifold::store()` itself is const-only, `arena_manifold.hpp:269-271`) — the adapter's
  seeded store
  holding the *image* lineage is not the comparand's *checked* side — it supplies the reference
  band, never the publish destination: `promote`'s verbatim persistent-ref map
  (`real.emplace`, `arena_manifold.cpp:1867-1868`) would write positions into it that never
  existed there, undetectable by a comparand reading the wrong store. The
  verify comparand itself is the decoded op records in canonical post-remap form —
  (a)'s stated comparand, applied verbatim: order-vs-order, non-scroll fields verbatim and
  scroll identity by `globalKeyOf`'s arm set — and "supply" meaning the
  runtime folds the adapter's
  store rather than re-applying segments into a second one — owed adapter-API surface, a named
  seam change like P4's wake (§10.2), not silently assumed. A kept-in-step adapter *twin* —
  the seeded store advanced alongside the runtime's after resume — is **owed a segment-apply
  surface** `adopt`/`historyFromSeal` do not provide (`adoptOpRecords` takes already-decoded
  records, `historyFromSeal` mints its own store; nothing applies a received segment into a
  store the caller already holds), named as owed surface under §10.2 rather than assumed.
  Positions coincide across the
  adapter's store and the runtime's because a
  persistent `CellRef` *is* a spool position, not an object identity (`manifold.hpp:295`) — so
  the image occupies persistent positions `1..E_ops` before any state
  op is written (`noCell == 0`, `dim_vector.hpp:23`, is the only ref below them). Checkpoints
  inherit the prefix position-by-construction: `promote` appends state ops above it and
  `sealLocalSpool` seals the whole spool in the checkpoint path — `publishValue` passes the
  default `opsAlreadySealed = 0`, so the segment covers `1..opsTotal`
  (`mesh_value.cpp:227-228`, `:241`; `publication.cpp:611-617`, `:664-672`) — `1..E_ops` ships
  inside every checkpoint's ops segment and folds back verbatim — `rebuildManifold` assigns
  `birthOp = opIndex` (`manifold.cpp:427-442`). The extent `E_ops` is the image prefix length
  **in ops**, the ref-space ordering bound: a cell's persistent ref is its
  *birth-op* position (`manifold.hpp:295`, `store.hpp:677-681`), never the band's op count, and
  the image's tail ops are links and values rather than births — recorded once at load on the
  adapter's `programRef` bind record — chain-stable because the continuation pins the artifact
  (`grant->programRef == continuation->programRef`, `mesh_actor.cpp:199,215-216`) — consumed at
  genesis step (a), never re-derived (the received manifold already holds every promoted cell; a
  per-incarnation extent would absorb them all). Positions therefore classify by construction:
  program cells return at refs `≤ E_ops` on every resume, before the checkpointed state in the
  numbering and before any `learn`-replay mints one — while a flat append after state would land
  them above it and invert the test, which is exactly why the prefix is a load-time commitment
  rather than a genesis step. Every ref classifies by position: an arena mint's
  ref is `ephemeralBit | dense` (`refOf`, `arena_manifold.hpp:468-470`; `manifold.hpp:154-158`),
  never satisfying the persistent arm, so grafted, learned and other runtime cells stay
  classifiable — a `learn`ed clause sitting on a program `d.clause` rank does not become
  "program-side" (its durability rides the registered twin and patch replay, not its own
  promotion) — while a shadow
  cell names its *base* ref
  (`arena_manifold.cpp:1489-1490`), so a shadow whose base ref is persistent and pre-extent
  classifies program-side **by design**: it is the resurrected program image — and a promoted
  cell's post-extent persistent ref keeps it state-side across `resume`. Prefix maintenance is an
  **adapter invariant on publish**, not an assertion: `publishValue` runs against the store
  holding the *received* band — the runtime's base store, `arena.base()->store()` — under the
  supply arm that *is* the adapter's seeded lineage store folded as the rebuild's backing
  store; under the verify arm it is the runtime's rebuilt store, verified to carry the same
  `1..E_ops` band (a persistent `CellRef` is a spool position, so the two stores' bands
  coincide) — never the adapter's image-only seeded store itself: published state ops must
  append above the *received* band, and the seeded store's positions above `E_ops` are stale
  image-lineage, not this incarnation's — and a
  prefix-less lineage (a publish off a foreign or fresh store, an `importValue` ingress of
  ephemeral twins, a sibling manifold) reports an empty program-side set, under which the
  discriminator **fails closed**: `learn` is refused outright — an admitted `learn` would link
  the skeleton into every checkpoint under the `closed` flag, the V12 failure with the roles
  reversed — and a `programRef` that names no cell of the received manifold is a genesis bind
  refusal. Fresh-store publication is therefore the fail-closed arm **by detection**, and
  detection needs no new surface: `ArenaManifold::store()` is already public
  (`arena_manifold.hpp:269-271`, falling through to `base_->store()`), so the verify arm can
  read `arena.store()`'s band today — what the platform withholds is *enforcement*:
  `publishValue` takes any `Store&` (`mesh_value.hpp:69-74`) and nothing compares it to
  `arena.store()` (a de-facto supply shape even exists — `Manifold::store()` is `const
  noexcept` returning non-const `Store*`, `manifold.hpp:486` — so the owed seam is *sanctioned*
  supply/verification, not raw reachability). It is already the only publish path the actor
  ceremony exercises today, `mesh_actor_resume_test.cpp:125`'s fresh `Store` —
  while `promote`'s own encoding presumes the continuing store (link and handle-value ops
  alike emit non-ephemeral targets verbatim — `setLink` writes `real.at`'s output,
  `arena_manifold.cpp:2041`, and persistent refs identity-map through `real`, `:1868`,
  the same translation that ships `OpHandle` targets at `:1947-1954`,`:1992-1996` — so a
  fresh-store publish over a persistent base writes ops naming positions that never existed in
  it). "State-side" is
  link-reachable *from the agent cell* — in either direction, as `promote` walks — **without
  transiting a program-side cell** (the walk prunes at the program-side boundary — its own root
  exempt: a program-side `agent_cell()` cannot prune the durable set) and not itself
  program-side; `closed`'s state-side arm uses the same test. The pruning is load-bearing: memo
  records, reflect wrappers, installed `learn` copies and capture-linked binding cells sit behind
  program-side anchors (the clause's `+d.code` memo chain, the installed `d.clause` edge, a
  program-side capture wing) and must not promote under
  the state banner — they die with the incarnation at `resume`. Live frame cells are the
  different case: `d.stack` hangs off *cursor* cells (`vortex_vm.cpp:206`, `:865-887`), never
  `home` — the cursors sit on `home`'s `+d.cursors` rank (`vortex_vm.cpp:157-160`), and `home`
  is the session's cursor anchor: the checkpoint's own persistent `home` whenever the base has
  one — `initGenesis` adopts `*base->home()` (`vortex_core.cpp:30-36`), and the base's `home_`
  is the store's first MakeCell (`manifold.cpp:450-451`), a pre-extent persistent ref — or
  arena-minted on a base-less genesis. The frames stay out of the durable
  set because `home` is not link-*reachable from* the agent cell — the invariant is about
  *paths*, not one edge direction: no *system-minted* edge creates a link path from the agent
  cell to `home`, the
  `+d.cursors` rank or any `d.stack` cell (the membership edges run `rankTail +d → member` —
  `home`→cursor, cursor→frame, `vortex_vm.cpp:157-160`,`:206` — outbound from the chains, and
  `##` anchors are ref-box ints, invisible to the link walk either way) — and the prune is what
  excludes the frames in every case: `promote`'s BFS prunes at any non-`holdsOwn` persistent
  cell (`arena_manifold.cpp:1755-1757`), so even a persistent `home` a session `Link` managed
  to reach cannot expose the frames behind it; and were `home` itself to classify
  program-side the prune would exclude the same frames, so the conclusion holds either way.
  Frame-bound captures are refused by binding class ahead of
  the sideness test — the `closed` account below details the refusal. The walk is
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
  `{+d.values → wrapper, +d.name → memo-kind label, +d.grab → scalar key cell}`, `+d.step`-linked —
  the key rides a fresh content cell, not `+d.name`, which holds the label, and the label is a
  fresh text cell per record too, since an interned one would be evicted between records) so they
  stay program-adjacent,
  never registered, and invalidate when the clause's attach cell changes. Wrapper descriptors are
  unknown-flagged — `pure`/`closed` unset — so a pure-requiring graft refuses them; body/symbol
  grafts may install one under the narrowed rules of §6.3 (validate still runs). A wrapper is not
  `run`-nable if the wrapped graph's tail is `Halt` (§6.2).
- `unlearn` removes the head's `d.code` registration edge when the last patch naming it is undone;
  dead patches are dropped after recording a `ConditionFault`-style fault.

`learn_plan(goal, name, test, body)` (and its clause-installing twins — `learn_method` installs
no clause; its `name` is the symbol's functional lookup name, never an alias and never minted)
does the trailed writes: reify each fragment
twice (registered head, installed copy), graft the installed copy, register the registered head on
`d.code`, append a patch cell to the agent's `d.patch` rank — and mint the installed clause's
`+d.name` label itself as `patch:<n>`, `<n>` the new patch's ordinal — the durable monotone
serial drawn from the agent cell's `d.vars` counter `reify` shares, never the rank position
(§6.1's naming rule: the user
`name` is an alias on the record, never the label). Patch and installed clause carry equal-text
`patch:<n>` labels on their own `+d.name` label cells intentionally — the patch's label names
the clause it installed, and the text sharing cannot collide because `plan_named`'s space is
clause-rank cells, never patch records.
The patch carries `{ordinal, target
descriptor record, the user `name` alias, +d.patch-head→record chain, +d.name→label,
priority for learn_handler}` — the alias present only where the verb takes a `name` argument
(`learn_plan`, `learn_handler`; `learn_where`/`learn_body` take none and `learn_method`'s
`name` is never an alias) —
`d.patch-head` is a dedicated first-edge dimension rather than reusing `+d.step` or `+d.values` at
the patch, matching the record-chain convention this document uses throughout — a chain's first
edge and its step edges are different dims (`+d.refs`/`+d.step`, `+d.recipe`/`+d.step`, the memo
chain's `+d.code`/`+d.step`); the top-level ranks themselves — `d.code`, `d.patch`, `d.synapse` on
the agent cell, and each goal's/event type's `d.clause` — are *same-dim* chains per
`insertIntoRank`/`rankTail`
(`arena_manifold.cpp:1423-1439`, `cell_views.hpp:258`; the `addClause` precedent
`vortex_stdlib.cpp:4238-4241`) — and
avoiding an occupancy collision — the patch's `+d.values` already
points at the descriptor record, so a `+d.values` first edge would evict it. `learn_handler`'s
patch record carries `priority`, since handler selection orders clauses by it and a replayed graft
must restore the same ordering. The label and serial split **per verb**: every `learn_*` consumes
its `<n>` off the agent cell's `d.vars` counter and writes it into the record's `ordinal`, and
every patch record carries its own `+d.name → patch:<n>` label cell — the uniform
`label_of`/genesis-transport bookkeeping handle (`unlearn` never matches it, (d) below). The
*installed* label differs: a clause-installing verb
(`learn_plan`, `learn_handler`) mints a second, equal-text `patch:<n>` label cell on the installed
clause — two cells, never one shared, each label cell's `-d.name` backlink holding a single edge
(genesis (d)'s
re-mint rule) — while `learn_where`/`learn_body` install nothing, so the re-pointed clause keeps
its own declared or minted name and the patch's `patch:<n>` lives on the record alone, and
`learn_method`'s installed symbol keeps its functional `owner.name` content — the content *is*
the name the `d.vars` walk matches on `textOf` (`vortex_stdlib.cpp:1953`), a `+d.name` label
shadows nothing and no resolver on the symbol arm reads one — its `patch:<n>` likewise on the
record alone. Replay and `unlearn` follow the same split: `learn_where`/`learn_body` re-point the
target descriptor's resolved clause, `learn_method` re-runs its unlink-first symbol arm, and
`unlearn` matches every verb by the recorded *target* name, never by the record's own label.
The chain is `+d.step`-linked record cells, each
`+d.values`-pointing at a fresh cell holding one **registered** head's `+d.name` label — the
pristine `d.code`-resident original the patch names, never the installed copy (`learn_plan` records
registered test head then registered body head, followed by any recorded prior head's name). Heads
are recorded **by name, never by link**: a head is referenced by up to two patches (installed on
one, prior on a later replace), and a second `+d.*` link to the head cell would sever the first
record by eviction; `unlearn` and genesis replay resolve each name through `code_named`/
`plan_named` (§6.7). An `unlearn` re-point never installs the named head in place — it installs a
`reify` of the named prior head, per the same never-graft-a-registered-head rule (§6.3). The
target descriptor records the target's resolved name, and the convention has two arms: the
**qualified** `[<module>:][<Agent>.]<member>` spelling for program-original clause and member
targets (intentions' qualified names — two-part `module:Name` for goals and events, three-part
where an `Agent.` member segment applies), and the clause's **verbatim `+d.name`** —
the `patch:<n>` the learning verb minted — for learned-clause targets: a learned clause's
name lives on its
`+d.name` verbatim, so the record stores it exactly, never a canonicalized
`<module>:<Agent>.patch:<n>` the `plan_named`
match would then miss (§6.7). For graft verbs the descriptor also carries the parent the clause
attaches under — the `goal`/`event` name replay needs to re-run the attach. *Where the name is
read off* splits by verb, because the
name lives on different dims for a clause than for a method/symbol. A **clause** target
(`learn_plan`, `learn_where`, `learn_body`, `learn_handler`) resolves through `plan_named`:
`plan_named` matches the clause's verbatim `+d.name` label (§6.7). A **method or symbol** target
(`learn_method`) resolves through `symbol_named`:
the qualified spelling is walked `module_named` on the qualifier, `symbol_named` on the module
and the dotted remainder against the module's `d.vars` (§6.7's owner rule) — the argument's
symbol cell stores bare `Agent.member` content while the record carries the composed qualified
`module:Agent.member`, the same composition `unlearn`'s symbol arm must re-do — a method has no
`+d.name` label to match, so `plan_named` cannot name it. A `learn_*`
whose target has no resolvable name (an anonymous grafted clause) is refused, because a patch that
cannot be re-found at genesis is a patch that lies — and the gap reaches declared handlers too:
a kernel-emitted handler clause is anonymous under spec V15 (`on Event [guard] { }` mints no
`+d.name` — only plan clauses are named), so `learn_where`/`learn_body` on a *declared* handler
hits the same refusal — durable guard/body patching works on named plans and on learned handlers
(`graft_handler`/`learn_handler` mint named clauses), never on declared ones, which are likewise
un-*replaceable* and un-*removable* durably: dedupe-by-name cannot match a nameless clause, so
`learn_handler` on the same event is always additive — it coexists with the declared clause
under `priority` ordering rather than superseding it — `unlearn` has no recorded name to match,
and `prune` is session-only; post-resume the image folds the declared clause back while the
learned shadow replays alongside it.
`unlearn(clause_or_symbol)` *matches* by the
patch target's resolved name: the argument's clause/symbol cell resolves to its name by the
reverse read — the clause's `+d.name` label cell, the symbol cell's own content — and the
latest patch whose recorded target name equals it is undone. A **`d.patch` record argument is
refused**: its `-d.patch` backlink names it a record rather than a clause/symbol, and letting its
`patch:<n>` label through the reverse read would accidentally match the clause-arm patch that
installed the same-named clause — kind-dependence refused, not exploited. Both arms deserve a
line. The **clause arm** reads the minted `patch:<n>` off the label — under the unique mint
every learned
clause answers a *different* name, so `unlearn` picks a specific twin out of a same-alias stack
in any order, and the surviving LIFO-by-name is only the undo order of stacked patches
recording one target (`learn_where` re-points on one clause). The **symbol arm** composes the
qualified spelling: the argument's symbol cell stores bare `Agent.member` content
(`exportSymbol` mints `makeCell(name)`, `vortex_core.cpp:119-127`), while the patch recorded
the target's qualified `module:Agent.member` — literal equality fails across the prefix unless
the reverse read composes it, the owning module cell reached by the `-d.vars` walk back to the
rank head and its content supplying the `module:` prefix — the same composition
`learn_method`'s record wrote (§6.7's split; the qualified form is what `symbol_named`'s
pre-split `owner` wants). Removing a patch never
reclaims `<n>` — the agent cell's `d.vars` serial stays spent; a later `learn_*` on the same
target mints the next ordinal, and replay-dropped patches likewise leave their serials consumed.

**`closed`, correctly.** A fragment is `closed` iff **no program-side reference lacks
name-resolution**: every reference out of its own graph is one of (a) a *state-side* endpoint
however carried — a `cap$N` capture link (§5.3's capture-slot edge, decoded through `masterOf`)
or a `cells"…"` path resolved at emission whose referent rides the paired slot's `int64` content —
where state-side means link-reachable *from the agent cell* without transiting a program-side
cell and not itself program-side under the sideness test above; (b) a *name-record* on
`+d.refs`: `+d.ref → the operand's slot
cell` plus `+d.name → the callee's qualified name` — or, on a `run`-target record, the target
head's label; or (c) a program-side dim referent name-recorded on its paired *ref-capture*
record's `+d.name` — the endpoint being the named dim rather than a cell and counting as
name-resolved, the slot's own `+d.refs` record staying a REF-record (a `+d.name` there would
make it a name-record and skip the marker ceremony) — and a `cap$N` link or an unnamed slot
landing on a *program-side* cell (an artifact-initial binding cell) carries none of the three,
the refused class on every arm: arm (a) admits only state-side cells and arms (b)/(c) require
the record. The user-level contract
follows: a learnable
fragment is *self-contained* — literals, its own `code` parameters instantiated, `believes`/
`findall` reads, `cells"##…"`/`$` paths onto state-side cells, captures of *declared fields and
member vars* — whose cells hang on the agent's or member's `d.vars` and so are state-side
(`vagent-agent-language.md` §4.3) —
plus program calls bound by name. **Capturing a `let` local or a parameter makes a fragment
un-learnable** — those cells are frame-bound (§5.3), orphaned per activation, and `learn` refuses
them by binding class ahead of the sideness test: a live frame cell is not name-recordable, and
one sitting behind a program-side anchor is not state-side under the transit clause either — so
the `learn` refusal names them (§5.6's diagnostics). A capture
whose declared type is or *contains* `Cell`/`Code` (`any`, `Opt`, lists, records) — or whose cell's
content is a ref-box int naming a program-side cell — is likewise non-`closed`: a session handle
cannot be durable, so a fragment
closed over one is refused unless it is name-resolved (`d.name`/`code_named`) at emission. The
closed walk is bounded by the *enumerable frontier* — `+d.refs` records, the emission's wing-dim
`cap$N` capture-slot links, the `+d.recipe` ref-capture records whose endpoints are read off
their paired slots' content (§6.1), and the head's own structural dims; an outside edge
smuggled onto a non-op-graph dimension (`dimensionsOf` being C++-only, U-M7) evades it — as does
an edge to a
program cell reachable only through int64 content (control-flow targets are ints, invisible to the
link walk, §6.4) — and both are named §11-door
limitations, not soundness holes to claim closed. `learn` **re-verifies** closedness at accept
time — it walks those records and links checking each endpoint is state-side or name-resolved
under (b) or (c) — a `+d.refs` name-record or the paired record's `+d.name` —
because an emission flag can be forged and a `reflect`-spliced fragment can smuggle program
structure — and `run` targets inside a learnable fragment must be *name-resolved* (a `run` of an
opaque session `Code` handle is not durable either — the emitter name-records every
statically-known `run` operand on a `run`-role record: a literal operand → the head's actual
`+d.name` label — a `code"#label …"` literal's user label, else its `src:`; a `code_named(L)`
operand with `L` a string literal → `L` verbatim — a name, not a call: the emitter resolves `L`
through `code_named`'s two-space lookup at emission and writes `int64(head)` into the slot on a
hit, while an `L` it cannot resolve — a qualified spelling like `demo:Forager.f` that no head
label can carry, or a `reify:`/`d.code` label the emission set cannot yet contain — leaves the
slot **unwritten** (its observable content is `None`; there is no sentinel and the §6.2
descriptor deref fails cleanly on it) while still recording `L`, so the session `run` refuses
it at the descriptor check *until the record's genesis rebind fills the slot* (the genesis
`int64(head)` write needs
this static-slot reading — there is no per-`run` `code_named` evaluation to fail or mask it).
The slot is **emission-fixed**: an `L` unresolved at emission stays refused for the rest of the
incarnation even when a *later* `learn` registers that very label — `code_named(L)` would then
resolve, but no in-session path writes the slot; only genesis's rebind fills it. The emitter
warns at emission that the operand is unresolved and names both futures — it can fully
classify only qualified misses (head → until-resume, head-less `def` → forever), since whether
a later same-incarnation `learn` registers a verbatim label is not the emitter's to know —
because the same descriptor refusal
hides two different ones — **fail-until-resume** (the record rebinds at genesis if `L` lands on
a head) versus **fail-forever** (the patch drops as unresolved).
The qualified `L` is thereby the only route a `run`-role record has to the qualified resolution
path; when the qualified name resolves head-less (a `def` symbol) the record stays unresolvable —
the documented headless-`run` refusal, the patch dropping like an unresolved name. The corollary
narrows who can succeed: the only qualified `run`-role record that *ever* binds is one naming a
`learn_method`-installed — head-valued — symbol, so `code_named("demo:Forager.f")` spelled at a
declared `def` is a guaranteed patch-drop, never a durable spelling. There is **no
`plan_named`/`symbol_named` arm** — both builtins return `Opt<Cell>` where `run` demands `Code<T>`
and M3 bars the silent `Cell`→`Code` upgrade, so such a `run` never typechecks and nothing is
recorded — the head-less `def` symbol it would name is the headless-`run` refusal class (§6.2); a
`reflect`-born operand is a session-side wrapper
with no durable referent, and any operand reached through a non-syntactic path — a `let` alias, a
computed argument — is opaque, so an opaque or `reflect`-born `run` target in a learnable fragment
is a closedness refusal at emission). A non-`closed` fragment — e.g., a
`reflect`-wrapped program-side graph — is refused, because linking it from `d.patch` would drag the
program skeleton into every checkpoint. Such graphs can be `graft`ed for the session; making them
durable requires `reify` into a closed copy first — and `reify` of non-literal graphs is deferred
(§15), so the honest v1 rule is: **`learn` accepts `code"…"`-born fragments and their `reify`
descendants only.** The `closed` flag's provenance is the adapter's: `CompilerBinding` gains a
state-side marker bit (§10.1) so emission knows which captured bindings name checkpointed cells.

**Name-bound calls are ordinary `Call`s with an int64 operand — model (b), no new execution
machinery.** A fragment's call to a program symbol emits the same `Call` op every call emits: the
operand cell is an emitter-minted int64 slot holding `int64(calleeEntryOp)` at emission
(`vortex_vm.cpp:854-856` executes `toInt(inputs[0])`), so in-session the call executes the
emission-time binding directly. What makes it *durable* is the `+d.refs` name-record alongside:
`+d.ref → that very slot cell` plus `+d.name → the callee's qualified name`. **`+d.name` present is
the discriminator**: a record with `+d.name` is a *name-record* — it gets **no** `d.mesh.relocation`
marker and is **never** re-kinded `OpHandle`, because a program-cell target must never reach
`promote`'s relocation path (which enqueues `handleTarget` into `seen`/`frontier` —
`arena_manifold.cpp:1759-1764` — and gates on it at `:1797-1809`, either dragging the program
skeleton in or returning `nullopt` when the target is excluded). A record without `+d.name` is an
ordinary ref-record and gets the full P16 marker +
`OpHandle` + rehydrate ceremony. Within name-records, the genesis writeback keys on **the record's
slot role**, carried on the record itself — a `+d.grab → "call"`/`"run"` role label cell, the same
fresh-scalar convention the memo key uses (M8) — never derived from name shape, because the two
slot conventions differ in kind, not in spelling: a `call`-role record writes `int64(entryOp)` — a
`Call`-operand slot is entry-valued (`Call` executes `toInt(inputs[0])`, `vortex_vm.cpp:854-856`) —
while a `run`-role record writes `int64(head)`, because a `run`-target or `Code`-valued operand
(fragment-identity) slot is head-valued — `run` derefs the head's `+d.entry` *and* `+d.values` at
run time (§6.2), so an op cell written there would kill the fragment on first `run`. The
`+d.name` *shape* selects only the resolution path: the test splits on the first `:` *and requires
the first segment to resolve as a module* on `programRef`'s walk, else the whole string is a
verbatim label — and call records always carry the `module:`-qualified spelling while label
records carry `src:`/`reify:`/`#label` head-label spellings, which the reserved-prefix rule of
§6.7 keeps disjoint (`src:`/`reify:`/`patch:` can name no module — `patch:` is the *clause*-label
prefix `learn_*` mints and patch-target names use, never a head label — and a user `#label`
cannot spell `:`).
A qualified name resolves through the §6.7 split (`module_named` on the module qualifier,
`symbol_named` on the module and the dotted remainder — §6.7's owner rule) → the symbol's
`+d.values` → **apply §6.1's head/op discriminator at the hop**: the
resolved cell is the entry op when it is an op (`exportSymbol` binds `sym +d.values → entryOp`
directly, `vortex_core.cpp:119-127`), else follow its `+d.entry` once (a head) — the same
op-or-head hop wherever a call record's chain lands on a symbol, genesis and patch replay alike —
then the role decides the payload: `call` writes `int64(entryOp)` through the hop; `run` writes
`int64(head)` and therefore requires the hop to land on a head — a `run`-role record whose
qualified name resolves head-less (a program `def` symbol, whose `+d.values` names an entry op,
never a fragment head; `code_named(L)` with a qualified `L` is the only producer of a qualified
`run`-role record) is the **headless-`run`** refusal class: the record is malformed, the patch
drops like an unresolved name, and `run` itself refuses the same target at the descriptor check
(§6.2) — a `run` of a head-less program `def` symbol cannot receive the slot's head convention at
all. A *label* — `src:`,
`reify:` or a user `#label`, which is what `run`-target and fragment-identity records carry —
rebinds through the **program-image label→head lookup**: the same outbound `program()`
structural walk the sideness test runs, matching `+d.name` verbatim but only over cells in the
head layout (`+d.entry` plus the descriptor `+d.values` — the shape restriction that keeps
clause `+d.name` labels — `patch:<n>` their largest producer — and the qualified strings they
carry on the same dim from matching; symbol names need
no such guard, they live as cell *content*, never on `+d.name`), →
`writeValue(slot, int64(head))` — the head itself, no `+d.entry` hop, because the slot is
head-valued; `learn`-registered heads live state-side, outside the program
image, so the lookup falls back to `code_named`'s `d.code` walk for them, writing `int64(head)`
the same way. An unresolved name
faults/drops the patch like a dead `d.patch` record — genesis does not install a call it cannot
bind. **Divergence
clause:** a session `run` retains the *emission-time* binding, while a resumed genesis rebinds
through the durable name and therefore follows *post-resume* redefinitions — a `graft_method`
re-point between checkpoint and resume is picked up by genesis's rewrite, not by the session copy —
and a *learned* re-point replayed at genesis step (d) is picked up exactly because the `+d.refs`
sweep runs after replay (step (e) below).
§9.3's `learn_plan` works by exactly this design: the `Call` to `navigate_to` inside the learned
body `code"{ navigate_to(\"water\") }"` is a name-recorded `Call` — `+d.name →` the *callee's*
qualified name `"demo:Forager.navigate_to"`, while `"water"` is just the argument — rebound at
genesis to whatever `navigate_to` then names, which is why the
learned clause survives where an int64 callee id would not.

**P16 is the checkpoint ceremony, adapter-side — no new mechanism beyond two named seams**: P4's
already-approved wake turn (the empty-mailbox arm below mints one;
`vagent-agent-language.md:1670`, an approved but unimplemented `mesh_actor` change) **and the
lineage-store seam** §6.6's invariant stands on — the adapter constructs and keeps the lineage
`Store`, seeded at spawn *by base-injection* (the seeded store's folded manifold plus the store
as reader through `WorkerContext`'s two-arg ctor — `worker_context.hpp:52-54`; `resume` passes
the manifold alone and calls `arena.setDefaultSpanReader` after, `mesh_actor.cpp:271-273` —
before `initGenesis` freezes `home_`/dims,
`vortex_core.cpp:14`,`:29-70`; §10.1), and at `resume` supplies it as the rebuild's backing store
or verifies the rebuilt store's decoded `1..E_ops` band against the lineage's decoded reference
band — (a)'s canonical post-remap form; `spawn()`
has no store to seed and `resume` keeps its internally built one private
(`mesh_actor.cpp:119-122`,`:156`, `:260-269`) — though `resume` already transports a caller
`Store` inside `ReceivedValue checkpoint` (`:182`, first read at `:188`'s `!checkpoint.store`
guard, `:261`), so the owed change
is trusting or comparing it — owed adapter-API
surface named like P4's wake, not silently assumed. `checkpointView` yields a
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
   mark is `+d.ref` on the record plus `d.mesh.relocation` on the cell — not `+d.values`, because
   `+d.values` has
   occupancy semantics — a later descriptor write could evict a `+d.values` marker. Runtime-minted
   reference cells are the carve-out: `Linked` rebuilds its rank per execution, so a `Linked`-born
   ref box is re-minted, not marked.
1. **Request.** The adapter sets its relocate-before-checkpoint flag when the next turn is the one
   whose end calls `publishValue` — and only when `pending(pid) == 1`, so a terminating turn exists
   whose end leaves the mailbox empty (`pending` exposes `mailbox.size()`,
   `mesh_actor.cpp:659-662`; `pumpOne` skips empty-mailbox actors, `:462`, and `checkpointView`
   refuses a non-empty one, `:672`). A checkpoint decision that lands on an empty mailbox instead
   mints a P4 `wake` turn so the flag has a window (P4 is the register's approved-but-unimplemented
   `mesh_actor` change, `vagent-agent-language.md:1670` — the only *pump-side* part of this
   ceremony outside current code; the lineage-store seam §6.6 names is the other owed surface).
   **No turn — message or P4 wake — runs between the flagged turn's end and
   `publishValue`**: the ceremony claims the ordering itself — the flag's `publishValue` call is
   issued from the same `Executor` callback that ended the flagged turn, synchronously, before the
   pump's next `pumpOne` decision can dequeue anything — current `pumpOne` gives no such ordering
   on its own (it dequeues whatever is pending — `mailbox.front()`/`pop_front()` at `:467-468`),
   so the no-interleaving property is
   *proposed adapter discipline*, not an attribution to the pump (changelog v1.7): a
   ready-intention or due-timer wake queued before the flagged turn's end is held behind publish
   the same way — so the window is "immediately after the flagged turn's end", never interleaved.
   The flag is a three-state marker — *re-kind pending → restore pending → drained* — and the
   callback owns it: it clears only once publish has succeeded *and* the head-of-turn restore has
   run — the restore itself is unconditional on marked cells still in `OpHandle` kind and runs on
   every exit path, so a `publishValue` failure or refusal still restores while the flag stays
   armed for the next attempt, and `ActorDown` clears it outright (a stopped actor never re-enters
   `pumpOne`, so no restore is owed there either).
1. **End of that turn, before `publishValue`**, the `Executor` callback — on the mutable arena —
   walks each registered head's `+d.refs` ref-records and `setValueBits(OpHandle, target)`s every
   marked ref cell, skipping any already in `OpHandle` kind (idempotent; the kind check is cheap and
   the walk is `+d.refs`-bounded). The VM **never executes** `OpHandle`-kind cells — the re-kind
   happens at turn end when no op can run, and `isCellRelocationHandle`
   (`foreign_op_handle.cpp:98-117`) requires kind *and* marker edge, so no stray `OpHandle` write is
   a handle, and no handle-kind cell (`OpHandle`/`ForeignOpHandle`, `ops.hpp:154-163`) is
   executable op content — nothing checkpointed runs as a ghost op. The walk's scope is honest:
   only *registered* heads carry the durable path — an installed-but-unlearned graft's ref cells
   are never re-kinded (`run`/`graft_*` never register), so an unlearned graft may structurally
   survive a checkpoint
   with stale calls; `learn_*` is what makes a graft's references durable.
1. **`publishValue` executes between turns on `const&`, immediately after the flagged turn's
   end** — the step-2 window: no subsequent turn of either kind runs first, and "the next turn"
   below is the first turn after `publishValue` returns — `promote` relocates the handles through
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

At genesis, the persistent manifold **arrives — it is not constructed**: `receiveValue` runs
`rebuildManifold(version)` over the checkpoint's own sealed spool (`mesh_actor.cpp:260-271`
installs it as the arena's `const` base; `manifold.cpp:427-442` assigns `birthOp = opIndex`
verbatim, index-for-index), so under the store lineage above the fold already reproduces the
whole `[image 1..E_ops][state >E_ops]` band in one pass. Genesis *binds* what arrived — the
ordering is fixed, and nothing in it re-materializes an image:

- **(a) lineage bind.** Genesis asserts the received manifold's image band is the recorded
  program image on **two distinct landmarks**, because one comparand conflated them: the band
  bound is `E_ops`, the image prefix length *in ops* — while a cell's persistent ref is its
  *birth-op* position (`manifold.hpp:295`, `store.hpp:677-681`), and the image's last op is
  never a birth (`promote` emits links last, `arena_manifold.cpp:2010-2017`; `exportSymbol`
  likewise closes with the `+d.values` and rank links, `vortex_core.cpp:119-127`), so no walk
  maximum can ever equal `E_ops` and none is asserted. The bind is instead an **ops-record
  order comparand**, mandatory rather than optional: the adapter **decodes its adopted lineage
  store's own `1..E_ops` band — the same op records `adoptOpRecords` folded it into, read back
  in canonical order — and asserts the received store's decoded `1..E_ops` records equal it
  pairwise**, an order-vs-order comparison of canonical post-remap `OpRecord`/`CompactOpNode`
  fields: op kind plus every non-scroll field verbatim, plus scroll *identity* by key — never
  the raw remapped `ScrollId`, which the remap legitimately rewrites
  (`publication.cpp:1048-1086`,`:1030+`). The identity check takes `globalKeyOf`'s arm set
  (`publication.cpp:853-869`) minus its `localScroll` case, which the remap never leaves behind
  (`:1049`): a `breakMarkerScroll` span compares by `breakMarkerKey` (`publication.hpp:447`) —
  the id is legitimately sealed, `sealableOps`'s table builder skipping rather than rejecting
  it (`:985-987`), and identity-remapped (`:1050`) — an engaged external id compares
  `scrollKey(*store->scroll(id))` (`scroll.cpp:13-22`), `Store::scroll` being an
  optional-returning lookup that disengages on `localScroll` *or an id beyond `externals`*
  (`store.cpp:1461-1469`; `localScroll == 0`, `spool.hpp:19`), any other disengaged id failing
  the compare outright — a lineage differing only in which scroll a span names failing it —
  and never segment bytes
  against decoded records, a node-byte compare refusing every honest resume under the same
  remap. `adoptOpRecords`'s produces-order fold is itself deterministic
  (`store.cpp:3836-3940`), so both comparands are the post-fold order, not file order.
  The comparand is the only stated test that detects permutation and scramble *inside* the
  band — a reachable shape, since `adoptOpRecords` topologically re-sorts adopted records with
  a `produces`-hash tie-break (`store.cpp:3836-3940`), so a branched segment can land image
  cells permuted within `1..E_ops` while every positional bound still passes — and it is the
  arm that refuses a merged or re-seeded lineage: `mergeValues` promotes imported twins in
  `(contentHash, authorKey, parent)` order (`mesh_value.cpp:519-523`) into its destination
  store — the fresh-destination, no-`job.base` arm (`mesh_host.cpp:509-510`) carries no prefix
  and fails the comparand outright, while under `job.base` the merge appends above the base's
  own prefix (`:507`) and the band survives legitimately. The walk's remaining role is
  **per-cell classification, not refusal**: each cell the `program()` structural walk reaches
  — the walk rooted at the adapter-recorded program root resolved on the received fold, (c)
  re-binding it for in-arena use — tests its own persistent ref against `E_ops`: `≤ E_ops`
  classifies program-side, `> E_ops` state-side — so cells the walk legitimately reaches beyond
  the band through runtime-grown edges (an installed copy under a `d.clause` attach, a
  session-minted member under `d.vars`) are
  classified state-side, never refused; **named `>E_ops` legs hanging off the *agent cell* are
  reached only where a runtime-grown edge leads to them** — `program()` and `agent_cell()` are
  distinct adapter-bound roots never `+d`-reached from each other (§6.6's vocab names the
  two-root split), so an agent-`d.vars` binding cell hangs on the agent cell's own `±d.vars`
  rank and enters this walk only across a genuine cross-root edge — a §5.3 `cap$N` capture slot
  resolving to its binding cell through `masterOf`/`d.clone` is the surviving case, a ref-capture
  record contributing none since its referent rides slot *content* the link walk cannot follow —
  and every such leg classifies `>E_ops`/state-side regardless: nothing an agent-rooted second
  walk would add classifies program-side anyway, so none is owed — its only consumer is
  `closed`'s frontier enumeration, not this test — and
  non-image ingress is *classified*
  by the same rule rather than detected as an error: a persistent ref `> E_ops` or any
  ephemeral ref is state-side, so an image twin minted above `E_ops` is admitted and reads as
  state, while a non-image cell inside `1..E_ops` is what the comparand just excluded. The
  lineage itself is a load-time
  commitment (the artifact's sealed `ops` segment, applied into the adapter's lineage `Store` by
  `historyFromSeal` — position-stable, not byte-identical; §10.1's seeding step owns the
  store — the `job.base` pattern, `mesh_jobs.cpp:304-318`), inherited by every checkpoint
  because checkpoints append and `sealLocalSpool` seals the whole spool in the checkpoint path
  (`mesh_value.cpp:227-228`,`:241`; `publication.cpp:611-617`); a prefix-less lineage is the
  fail-closed arm above — `learn` refused (the §5.6 *no program lineage* diagnostic), and a
  `programRef` naming no cell of the received manifold is a genesis bind refusal. The arena
  then overlays the base with ephemeral session cells; the overlay adds nothing persistent.
- **(b) `ensureDimension` find-or-mint + `setOpcodeKind` re-registration** — dims resolve from
  base/cache before any mint (`arena_manifold.cpp:78-105`), no marks outstanding; and the folded
  image's op cells re-register into `opcodeMap_` through `setOpcodeKind`
  (`vortex_vm.cpp:98-100`) — over **exactly the recorded instruction enumeration**: the artifact
  records which members of its band are op cells at emission — as `produces`
  `MicroversionId`s, resolved to positions on the *received* fold (`opsSpool` keys records by
  id, `store.cpp:3840`; `indexOf`/`idOf` translate) — or, equivalently, as positions
  *in the canonical post-fold order*, which requires the stated invariant that the image's
  promote appends formed a linear `parent` chain, sealed order == fold order: `adoptOpRecords`
  is a deterministic topo-sort drained by a min-`produces` heap (`store.cpp:3836-3940`), so on
  a branched image band the sealed order and the folded order diverge and recorded positions
  would point at the wrong cells — a skew (a)'s comparand cannot catch, fold-vs-fold of the
  same segment being equal regardless of emission order — while `produces` identities are
  permutation-stable by construction. The enumeration rides
  the same `programRef` bind
  record `E_ops` rides (ops are known by construction, never discovered by value — the
  `program.instructions` precedent, `mesh_evaluator.cpp:658-663`, drives registration off the
  recorded list the same way), and the pass applies `setOpcodeKind` to the folded cells at those
  positions — the *kind* argument recorded alongside (`instruction.opcode` in the precedent) or
  read off the folded cell's own `asInt64`, safe for a *recorded* cell. A value scan is the one
  wrong shape: an `asInt64 ≤ Linked` sweep of the band would
  register every cell holding a small `int64` — `dimConstant` operand cells naming genesis-era
  dims (`compiler.cpp:769-772`) among them — putting honest data cells inside P15's refusal and
  making `Value`-writes to them fail; and an op cell reached only through `int64` content
  (a `Jump`/`Call`/`Branch`/`Choice` target) is invisible to any link walk (§6.6's own bound).
  The pass is a registration *pass* over the recorded folded cells, never a re-run of
  `mintOpcode`, which would mint ephemeral twins and register the wrong cells (§6.5). The
  dependency is an image-free map, not stale indices: `opcodeMap_` is per-`WorkerContext` and
  holds none of the folded image's op cells
  at the spawn-time image bind as after `resume`, so the pass runs at both — and the consumer
  that truly needs map *membership* is §6.5's write-guard —
  `getOpcodeKind` decodes folded ops fine through its `asInt64` fallback
  (`vortex_vm.cpp:102-114`), but the P15 refusal tests `opcodeMap_.contains(masterOf(cell))`,
  so without this pass the folded skeleton's op cells are absent from the map and surgically
  writable (the §6.5 exemption is what leaves them writable); `code` re-emission needs it
  filled too — the program image's *own* op map, keyed on the folded refs.
- **(c) `programRef → the program root` bind** by the V10 mechanism — the artifact binds its own
  program artifact so `code_named`/patch name resolution has a root to walk.
- **(d) `d.patch` replay in ordinal order** — replay does not re-record (replayed grafts mint no
  patch cells, so no **`patch:<n>` serial** is consumed — the install copy's own
  `reify:<src>:<serial>` label still mints off the same counter, §6.7). Each replayed install
  `reify`s a `d.code`-registered
  head into the installed
  copy — the copy's `+d.name` carried **verbatim off the patch record** as a **re-mint**: a fresh
  label cell written with the record's text exactly, never a re-link of the record's own label
  cell, which single occupancy would evict off the record and drop `patch:<n>` at the *second*
  genesis — two equal-text `patch:<n>` label cells therefore exist per clause-installing patch,
  one on the record and one on the installed copy (the same `patch:<n>` the
  verb minted; replay transports the label, it never re-derives `<n>` from the position the
  replayed graft lands at), so the installed clause keeps its minted name across checkpoint —
  and the replay re-enacts each patch's **verb**, install and recorded dedupe alike:
  `learn_plan`'s replay re-matches its recorded alias verbatim against the re-attached rank's
  clause `+d.name`s and re-unlinks the hit — the superseded declared clause folding back at
  `1..E_ops` is displaced again rather than resurrecting — and the victim is re-derived from
  the folded rank, the verbatim alias match *at replay time*, which need not be the clause the
  session unlinked (§6.1's faithful-and-divergent cases) — while `learn_method`'s
  unlink-first rides `graft_method`'s own verb and `learn_handler` simply re-appends —
  and the copy's ref slots derive their endpoints from the *paired donor slot's promoted
  content* at replay — the copy-inheritance that decides (e)'s position, the record supplying
  the pairing and the referent class (`+d.refFor`, the conditional `+d.values`, `+d.name`), never
  the endpoint value itself (§6.1's recorded-template rule) — **and the replay hands (e)
  its install set**: per patch, the
  minted install copy plus the `+d.refs` records `reify` regenerated on it. The hand-off is
  needed because the copies hang off program-side install sites — the `d.clause`/attachment
  anchors the publish-time walk prunes — so no traversal over registered heads alone reaches
  them; (e) iterates (d)'s install set plus the registered heads.
- **(e) `+d.refs` rehydration, last genesis pass** — a single sweep over every `learn`-registered
  head's records **and every replay-installed copy's regenerated records**: per ref-record
  `Int64(*handleTarget)` — the slot's own promoted target *is* the referent's fold position for
  internal and external endpoints alike, the endpoint riding the slot's content under §6.1's
  content-carried rule — the ref-capture record paired to it by `+d.refFor` → the same slot
  cell, `+d.name` → the dim name re-resolved through `ensureDimension`'s name arm when the
  referent is program-side (§6.1's enumeration);
  per name-record the role-split rebind above. Rehydration is needed
  because `resume` overlays the received manifold without `importValue`'s remapping
  (`mesh_actor.cpp:260-271`) — the two paths differ in kind, not just order: `importValue`
  re-mints each relocation-marked cell `OpHandle`-kind naming the *local* twin of its promoted
  target and demotes unmarked `OpHandle` cells to `None` (`mesh_value.cpp:485-501`), while
  `resume`/`genesis` receives the `OpHandle`-kind cells verbatim — handles carrying their
  *promoted* targets (`arena_manifold.cpp:1947-1951` writes `real.at(target)`) — and rehydration
  `writeValue(slot, Int64)`s each marked cell back with the promoted target. The sweep runs last
  for two reasons — coverage first, ordering second. Coverage: a replayed install's ref-box
  slots derive from the regenerated ref-capture records — a ref-record's `+d.ref` slot
  identifies *which* cell's referent is captured, while the referent itself is read off the
  paired slot's *content* (§6.1's rule: internal and state-side endpoints
  ride the slot's `int64` ref position in-session and its `*handleTarget` at (d) — the donor's
  marked ref cells still `OpHandle`-kind, (e) rehydrating last — a position, never a census
  integer — `+d.name` carrying the dim *name*
  for a program-side referent, re-resolved through `ensureDimension`'s name arm);
  at replay an *internal* referent is rebound through the old→new reify map to its copy twin,
  and an *external*/state-side referent is the fold cell at the promoted position — so `Value`-
  replaying `int64(endpoint)` writes the identical bits `*handleTarget` on the donor slot names,
  an equality that holds for the external arm only —
  the copy's ref slots arrive pre-rehydrated with no handle-kind read (`reify`'s
  `New`/`Link`/`Value` palette cannot mint handle-kind cells, and the ref arm's kind-guard makes
  it a no-op on them), while a naive replay of the donor's recorded literal is the stale-alias
  bug the endpoint derivation exists to preclude — and the copies' *name-record* slots still
  need the rebind arm; those records live on copies a head-only
  sweep cannot reach, which is why (d) hands its install set over. Ordering: a
  name-record's rebind must see **post-installation** symbols — under the naive pre-replay
  position a `learn_method`/`learn_plan` install-or-repoint replayed at (d) was invisible to it,
  so a call to a patch-replaced symbol bound the pre-replacement image binding and a call naming
  a patch-installed symbol dropped the patch — the misbind/drop class the reorder removes.
- **(f) turns.**

Two discipline rules keep the checkpoint boundary honest:

- **Replaces of program-original attachments record no prior head.** A `learn_where` on a
  kernel-emitted clause cannot keep the prior attachment on the patch — that edge would be
  state→program-skeleton, exactly the link V12 exists to exclude. The patch records the new head
  only; `unlearn` of such a patch detaches it, and genesis restores the program's own attachment
  for free because the image prefix folds the skeleton back verbatim at `1..E_ops`. (Unlearning a
  *previously learned* replace does record a prior head — a closed fragment is safe to link.)
- **Replay is monotonic but not cumulative-garbage.** Genesis walks `d.patch` in ordinal order and
  re-runs each patch's recorded verb; a patch whose target no longer resolves (renamed plan,
  pruned clause) is
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
- `d.name` labels: `code"…"` heads carry an automatic `src:<module>:<line>:<col>` label — the
  emitter's default, not an overridable property of the head: a `code"#label …"` inner-grammar
  prefix *replaces* it (the user label wins outright, it is not a suffix). A *nested* literal —
  a `code"…"` appearing inside another `code"…"` body — labels from the **outermost** enclosing
  literal's `src:` site and chains `+o:c` descending inward per level: `src:m:l:c+o:c…` where
  each `+o:` arm is the next-inner literal's offset position — the `+<off-line>:<off-col>` pair
  §5.1's diagnostics use inside its enclosing
  body — the first arm sits inside the `src:` literal's body, the last names the labelled
  literal's own spot in its immediate parent — so the outermost origin plus the full descent is
  the unique key. A literal
  cloned by expansion — `splice` substitution or M13 `def` expansion — takes its *expansion
  site's* position plus a clone ordinal, `src:<module>:<line>:<col>+n`, the label built
  site-chain → the clone's own `+o:c` arms (a clone of a nested literal keeps them) → `+n`
  appended last; `n` counts that site's clones in expansion order, deterministically (the label
  is a durable re-resolution key, so a recompile numbering clones differently would mint labels
  no checkpointed record can match) — so cloned literals cannot collide. `reify`
  labels `reify:<src>:<serial>` (`<src>` is the reified head's own label, `<serial>` the
  duplicate disambiguator). User
  `#label`s are uniqueness-checked **at emission across the merged program emission set** —
  literals, splices, M13 clones — a duplicate is a compile-time refusal, since
  labels are the re-resolution key and a duplicate would misbind (`+n` ordinals attach to `src:`
  labels only, so a `#label` inside a body expanded more than once collides and is refused —
  labelled literals are not cloneable) — and all
  three are *opaque label strings*, matched verbatim; the *user* `#label` alone cannot spell `:`
  or `"` (the `'#' Ident` grammar, §5.1). First-`:` splitting applies only to
  qualified-name lookups — `plan_named` and patch target names (`symbol_named` receives the
  qualifier pre-split as `owner`): `demo:` is treated
  as a module qualifier only when it resolves as a module on `programRef`'s walk, otherwise the
  whole string is the name — never to `code_named`, where `demo:Forager.f` is just a label.
  The label prefixes `src:`/`reify:` and the verb-minted `patch:` clause-label prefix are
  **reserved at
  module declaration**: none may name a module — a declaration carrying one is a compile-time
  refusal — so
  the shape test's module arm can never reroute a label down the qualified path.
  `reify`'s `<serial>` comes from a *durable per-agent
  counter* on the agent cell's `d.vars` rank — a module's `d.vars` is program-side and never
  holds it — not a per-incarnation one, so reified labels never collide with a promoted
  registration's after `resume` — and `patch:<n>`'s `<n>` draws the *same* counter (§6.1's mint),
  so a patch ordinal is never recycled across `unlearn`, replay-drop or resume. `code_named(label)`
  resolves label → live head per incarnation —
  the recommended durable reference, and the required one for handles received from another agent:
  a received `Code`/`Cell` is opaque data until re-resolved by label, never a ref-box to
  `run`/`graft`/`cells` against directly. The lookup space differs by head class: `code_named`
  walks `d.code` first — the `learn`-registered heads — and falls through on a miss to the
  program-image label→head walk §6.6's name-record rebind runs, where a `code"…"` literal's head
  lives (never `d.code`-registered, so the two spaces are disjoint and no rank-order tie-break
  is ever needed); that fallthrough is what gives a literal head a re-resolution spelling at
  all — and a `reify:<src>:<serial>` label is read off a live head with `label_of`, never
  constructed by hand — the head reached as a `Cell` through `cells"…"` (§6.1's row names the
  paths), since `label_of` takes `Cell` and the head-producing reads yield `Code<Cell>` (M3
  bars the coercion). A `code"…"` head living in *another* artifact's image has no
  re-resolution spelling
  at all this round — both walks cover the bound `programRef`'s lineage only, and a foreign head
  outside it takes §6.6's fail-closed arm as a cell outside the bound `programRef` lineage —
  never a silent alias; the cross-artifact spelling is deferred as U-M11.
- `symbol_named(owner, name)` / `plan_named` resolve qualified names — the grammar is
  `[<module> ':'] [<Agent> '.'] <member>`: the first `:` separates the module qualifier — `owner`
  is the module *only*, and `demo:` is treated as a module only when it resolves on `programRef`'s
  walk, otherwise the whole string is the name — and the remaining dotted prefix is part of the
  stored name itself. The two lookups walk different ranks and match **different representations**
  of the name: `symbol_named` reads the resolved module's `d.vars` (spec §5.7 — goals and events
  as bare names, agent members under the `Agent.member` spelling), where the name lives as the
  symbol cell's *content* — `exportSymbol` mints `makeCell(name)` (`vortex_core.cpp:119-127`), no
  `+d.name` edge — so the `Agent.` segment names no separate `d.vars` cell and there is no
  agent-owner arm; `plan_named` matches the qualified `module:Agent.plan` string against clause
  `+d.name` label *cells* (a plan's qualified name lives on its clause's `+d.name`, not in a
  `d.vars` symbol). Both compare the name string verbatim — it is the cell carrying it that
  differs.
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
  the serial itself drawn from the durable counter on the agent cell's `d.vars` rank, the same
  source `reify`'s `<serial>` uses (§6.7); membership is the edge existing **and**
  `value_of(tag) == serial`. Both
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
  and `plan_named` resolves). `weight_of` disengages on no record; a `set_weight` to an unkeyed
  name mints the `d.vars` record first, so a first write *seeds* rather than skipping. The write
  itself is an ordinary trailed scalar write through the focus slot
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
  the agent cell, so they promote and survive `resume`. What is *not* durable is the incarnation
  binding a label resolves to — program cells return refolded from the lineage's image prefix
  (§4.5), but session *handles* alias, so the junction stores the name, not the endpoint — a
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
`#Call` frame + spill instead of inlining.

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
      learn_plan("Feed", "near_water",   // alias on the patch; learn_* mints the
                 code"believes sees(\"food\", \"water\")",   // clause's +d.name itself —
                 code"{ navigate_to(\"water\") }")           // patch:<n> off the d.vars serial
      believe learned_rule("food_water")
    }
  }

  on Meal as m {   // it worked — strengthen
    for c in clauses_of(symbol_named("demo", "Feed")) do {
      let w := weight_of(c)
      if [w] then set_weight(c, w + 0.1) else set_weight(c, 0.1)   // else seeds the first record
    }
  }
}
```

The grown clause participates in plan selection like any plan — it is a plan. Its `+d.name` is the
verb-minted `patch:<n>` (`<n>` = the patch's ordinal off the agent cell's `d.vars` serial —
`"near_water"` is
the user *alias* recorded on the patch, §6.6's naming rule; `patch:` is refused as a user `name`
spelling, so the example passes the alias, never the label). (`navigate_to` is a
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
  resolve.{hpp,cpp}            // the resolve-only pass stamping Identifier binding identities — the
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
                               //   per-fragment arity table, static closedness,
                               //   position-mask re-verification,
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
adapter:                       // CONSTRUCTS and KEEPS the agent's lineage Store (per-agent,
                               //   never shared; spawn-side anchor — the verify destination
                               //   is the received band's store, arena.base()->store()) —
                               //   seeds it on the artifact's sealed ops
                               //   segment at load (historyFromSeal, publication.cpp:1093-1103),
                               //   then binds its span source like any restored history —
                               //   setContentSource + bindPublishedLocalScroll(
                               //   store->addScroll(from)) — historyFromSeal returns no
                               //   self; addScroll's sameContentAs dedup recovers the id
                               //   (store.cpp:1388-1405) —
                               //   restorePublication's own pair (publication.cpp:1129-1131),
                               //   without which every artifact-local span dead-reads "" —
                               //   position-stable as the lineage's low prefix (the
                               //   job.base pattern, §4.5/§6.6); documentId_ is ctor-or-load
                               //   state (store.hpp:133,:1435 — store.cpp:135 or the
                               //   directory load's :4450) and historyFromSeal mints
                               //   Store() internally (publication.cpp:1097) with no id
                               //   parameter, so the adopt-the-artifact's-id arm
                               //   (restorePublication's Store(userPermascroll, storeId)
                               //   precedent, :1127-1128) is UNMECHANIZED — owed with the
                               //   supply-side seam — binds it by BASE-INJECTION:
                               //   the folded Manifold (pinned node-stable for the arena's
                               //   life — rebuildManifold returns it by value,
                               //   store.hpp:450-457, and the context's arena holds it as a
                               //   raw const Manifold* base_, arena_manifold.hpp:752)
                               //   + the store as reader through WorkerContext's two-arg
                               //   ctor (worker_context.hpp:52-54; resume itself passes the
                               //   manifold alone and calls arena.setDefaultSpanReader after
                               //   — mesh_actor.cpp:271-273 — the spawn arm supplies both at
                               //   construction) before initGenesis freezes home_/dims
                               //   (vortex_core.cpp:14,:29-70; a post-construction attach(Space)
                               //   cannot re-deliver them; spawn() takes no store today,
                               //   mesh_actor.cpp:119-122,:156 — this step owns that gap) —
                               //   supplies the same lineage store as resume's rebuild backing,
                               //   or verifies the received band in the runtime's own rebuilt
                               //   store — arena.base()->store()'s 1..E_ops in canonical
                               //   post-remap form — and binds program
                               //   root + agent cell +
                               //   d.code/d.patch/d.weight/
                               //   d.synapse/d.entry/d.patch-head/d.validate/d.refs/d.ref/
                               //   d.recipe/d.refFor/d.mesh.relocation + the validate ceiling, arity
                               //   & impure-kind cells (V10 mechanism, no-marks path); the
                               //   programRef bind record carries the recorded image extent
                               //   and instruction enumeration genesis (a)/(b) assert (§6.6);
                               //   owns
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

Continuing vagent's register after P14. **P15 and P16 are proposed, not approved** — and P16's
adapter half rides on the lineage-store seam §6.6 names: the adapter constructs/keeps the lineage
`Store` — the spawn-side lineage anchor, never the verify destination, which is the
received-band store `arena.base()->store()` —
`spawn` binds it by **base-injection** before `initGenesis` (the `WorkerContext` two-arg
ctor, `worker_context.hpp:52-54` — `resume` passes `base` alone and calls
`arena.setDefaultSpanReader` after, `mesh_actor.cpp:271-273`; the spawn arm supplies both at
construction — supply-side, not a store parameter),
and `resume` must take or verify it (`spawn()` accepts none, `resume()` keeps its private
one, `mesh_actor.cpp:119-122`,`:260-269` — though `resume` already transports a caller `Store`
inside `ReceivedValue checkpoint`, `:182`, first read at `:188`'s `!checkpoint.store` guard,
`:261` and dropped before the
rebuild, so the owed change is *trusting*/comparing it, not new transport; `ActorCheckpointView`
hands the adapter arena/root/
continuation but no store, `mesh_actor.hpp:102-106` — while `ArenaManifold::store()` already
exposes the band for verification, `arena_manifold.hpp:269`, so only sanctioned *supply* is
owed) — owed adapter-API surface alongside P4's
approved wake change, not silently assumed. The adapter-side owed surfaces, named beside the
core-code rows below rather than assumed: (i) the spawn-side base-injection and (ii) the
`resume` take-or-verify above, (iii) a **segment-apply surface** `adopt`/`historyFromSeal` do
not provide — `adoptOpRecords` takes already-decoded records, `historyFromSeal` mints its own
`Store` internally (`publication.cpp:1097`), and nothing applies a received segment into a
store the caller already holds — owed only if the kept-in-step adapter twin of §6.6's seam is
kept (the `base()->store()` verify arm stands alone without it), (iv) P4's approved wake
(`vagent-agent-language.md:1670`), and (v) the **`documentId`-adoption arm** §10.1 names
unmechanized — `historyFromSeal` mints `Store()` internally with no id parameter
(`publication.cpp:1097`, `store.cpp:123`), so the seeded lineage's id never becomes the
artifact's own unless an overload or a `restorePublication` seed lands alongside the
supply-side seam of (ii). P17 is recorded
below the register as deferred-not-registered.

| No. | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | Where                                                                                                                                                                     | Why it cannot live in `std:meta` or the adapter                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Without it                                                                                                                                                                                                                                                                                                              | Used by         |
| --- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------- |
| P15 | Every VM write into an `opcodeMap_`-registered cell is refused — kind-immutability at the write funnel: `VortexCore::writeValue`/`splice` take a VM-installed kind predicate (`opcodeMap_.contains(masterOf(cell))`), covering `executeOpcodeBody`'s `Value`/`Splice` cases, `step()`'s memo-hit and generic output writebacks, and both `runPipeline` writes; refusal is a typed `success=false` per site. Exempt by construction: `setCursorOpcode`, `Call` frame writes (the frame-cell carve-out), `mintOpcode`'s `setContent`, every write to a fresh (unregistered) cell. Post-resume the folded image's op cells re-register by a `setOpcodeKind` pass (`vortex_vm.cpp:98-100`) over exactly the recorded instruction enumeration — repopulating the image-op entries in the per-`WorkerContext` map, never a value scan — `mintOpcode` would mint ephemeral twins, not register the folded cells (§6.5).                                                                                                                               | `vortex_core.cpp:196-254` (`writeValue`/`splice` funnel + installed predicate); call sites `vortex_vm.cpp:771-806`, `:1269-1273`, `:1287-1294`, `:337`, `:343-346`; tests | `opcodeMap_` is private VM state (`vortex_vm.hpp:186`). No Vortex opcode can see the map, and the adapter runs between turns, not between opcodes. Every non-structural op restates bound outputs' content through `writeValue` — a wing re-point (this document's whole subject) can aim an output at an op cell and rewrite its `int64` with no `Value`/`Splice` involvement; the map claims the old kind while the cell names a new one, and the trail cannot undo it because the map is not arena state. | Any write to a registered op cell silently changes what the cell *means* — a stale-kind divergence invisible to every `std:meta` guarantee, making "the graph says X" unverifiable. The only alternative would be refusing all program-side writes, which is `cells"…"`'s ordinary content write and cannot be refused. | M10, §6.5, §7.2 |
| P16 | Checkpoint-time relocation of fragment-owned int64 reference cells — an **adapter-side ceremony needing no mechanism beyond two named seams: P4's already-approved wake (`vagent-agent-language.md:1670`) plus the lineage-store seam §6.6 names — the adapter-owned lineage `Store` (spawn-side anchor; the verify destination is the received-band store, `arena.base()->store()`), base-injected at `spawn` and supplied or verified at `resume`** (`spawn` takes no store and `resume` keeps its rebuilt one private, `mesh_actor.cpp:119-122`,`:156`,`:260-269` — though `resume` already transports a caller `Store` inside `ReceivedValue checkpoint`, `mesh_actor.cpp:182`, first read at `:188`'s `!checkpoint.store` guard, `:261`, read only for `userPermascrollPtr()` and dropped before `receiveValue`'s rebuild): emission marks every fragment-owned int64 ref cell whose `+d.refs` record carries no `+d.name` with *its own* `d.mesh.relocation→"cell"` marker cell (one per ref, `mesh_evaluator.cpp:735` precedent) plus a `+d.ref` record on `+d.refs`; `+d.name` on a record makes it a name-record — no marker, never re-kinded, genesis rebinds by *record slot role*: a `call`-role record resolves the qualified `module:Owner.name` through `symbol_named`'s module-`d.vars` walk → `+d.values` → the op-or-head hop → `writeValue(slot, int64(entryOp))` (Call-operand slots are entry-valued), a `run`-role record resolves its `src:`/`reify:`/`#label` through the program-image label→head lookup → `writeValue(slot, int64(head))` — a *qualified* `run`-role `+d.name` (`code_named(L)` with a qualified `L` is the only producer) resolves through the same qualified path but must land on a head, since run-target/`Code` slots are head-valued — `run` derefs `+d.entry`/`+d.values` — and a head-less resolution (a `def` symbol) is the headless-`run` refusal, learned labels falling back to `code_named`. The mutable seam is the adapter's own `Executor` callback inside `pumpOne` (flag-driven: `pending(pid)==1` arms the window, an empty-mailbox checkpoint mints P4's wake turn; end-of-turn re-kind `Int64`→`OpHandle` on the mutable arena, `publishValue` immediately after the flagged turn's end on `const&`, head-of-next-turn `Int64` restore on every exit path); genesis rehydrates in the post-`d.patch`-replay `+d.refs` sweep — registered heads' *and* replay-installed copies' records (§6.6 step (e); resume skips `importValue`, `mesh_actor.cpp:260-271`). Acceptance: a learn → checkpoint → resume → learn → checkpoint round-trip test. Scope: fragment-owned ref cells only. | emission-side marking: `vagent/fragment.{hpp,cpp}` (kernel-glob TU, §10.1); the flag/checkpoint/genesis ceremony: the adapter's own executor TU plus P4's approved `mesh_actor` wake change and the lineage-store seam (spawn-side base-injection of the seeded store — the `WorkerContext` ctor, `worker_context.hpp:52-54` — `resume` passes `base` alone and installs the reader post-construction via `arena.setDefaultSpanReader`, `mesh_actor.cpp:271-273`; the spawn arm supplies both at construction, supply-side not a store parameter; `resume` store supply/verify); the rest reuses `foreign_op_handle`/`publishValue`/`checkpointView` as-is | `promote` copies Int64 content verbatim and translates only links and `OpHandle`/`ForeignOpHandle` values (`arena_manifold.cpp:1934-1954`, the handle-translation arm covering both); no opcode can write `OpHandle` kind (`writeValue` covers Int64/Double/Bool/None, `vortex_core.cpp:196-229`), and `resume` bypasses `importValue` — so only the adapter (which owns emission and checkpoint timing) can run the ceremony; it needs no VM change because `isCellRelocationHandle` and the promote path already exist.                                                                                                                                                                                       | Every promoted fragment's wing slots and `Jump`/`Call`/`Branch`/`Choice` targets stay stale dense ids after `resume` — silent aliasing into unrelated cells; `learn`/`d.patch`/`d.code` durability is impossible and must degrade to session-only.                                                                      | M11, §4.5, §6.6 |

P17 — `opcodeMap_`/`cursorPC_`/`arenaDims_` eviction or resync on `release` — is **deferred, not
registered**: the containment discipline (drain idiom, no bare `release` under live marks, no-marks
minting for ops and dims alike) keeps it unnecessary; a future proposal that needs mid-turn release
of op- or dim-minting marks must raise it then.

Registered outside the core register, as adapter-side surface (iii) above: the **segment-apply
surface** — a `Store`/`publication` entry point applying a received ops segment into a store the
caller already holds (`adoptOpRecords` consumes decoded records; `historyFromSeal` folds into a
`Store` it mints, `publication.cpp:1097-1102`) — owed for the kept-in-step twin only: if the
twin arm is dropped, §6.6's `arena.base()->store()` verify path stands alone and this surface is
never needed.

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
- **P16 makes `learn`'s durability real.** Given P16's adapter ceremony over the adapter-owned
  lineage `Store` — marker edges + `+d.refs`
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

- **One new core proposal plus an adapter ceremony** (P15; P16 needs no mechanism beyond P4's
  already-approved wake plus the lineage-store seam §6.6 names — adapter-owned `Store` base-injected
  at `spawn`, supplied or verified at `resume`). P15 changes
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
  per install (§7.2); `learn_*` prices its double-`reify`: two fresh copies per fragment argument —
  each an O(fragment) recipe replay — one installed, one registered, the argument itself left
  untouched (live session data, not dead);
  plus a `d.patch` cell + head-chain records + checkpoint ops for live
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
  `d.patch-head`, `d.validate`, `d.refs`, `d.ref`, `d.recipe`, `d.refFor`, `d.mesh.relocation`
  are minted as
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
- **U-M11 — Foreign literal heads from other artifacts.** A `code"…"` head living in *another*
  artifact's image — a second program loaded beside this agent's, or a library artifact grafted
  cross-store — has **no re-resolution spelling in this round**: `code_named` walks this agent's
  `d.code` first (falling through to the bound image's label walk), the program-image label→head
  walk of §6.6 covers the *bound* `programRef`'s
  lineage only, and a literal head outside it is a cell outside the bound lineage — the
  fail-closed arm, never a silent alias. Re-resolving a foreign artifact's heads needs a second
  lineage bind and a
  namespaced label space; recorded as a direction, not claimed.

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

**1.5 — Fifth review revision (r5 disposition).** Replaces the `birthOp` mint-window framing
entirely — `birthOp` is self-identity for ordinary cells and the base ref for shadows, never a
timestamp — with **ref-space ordering**: program-side is outbound `program()` reachability over
the full structural vocabulary (`d.extends` added; `d.dims` named the exclusion since `promote`
skips `d.dims` edges and genesis re-mints them) *and* a persistent non-`ephemeralBit` ref at or
below the loader-recorded program-image extent — the maximum persistent `CellRef` at program
load, recorded once, bound at genesis step (c), never re-marked; ephemeral and post-extent refs
stay state-side, shadows naming pre-extent bases classify program-side by design. Adds the
missing **state-side transit clause** — link-reachable from the agent cell without transiting a
program-side cell — with the program-side boundary the walk's pruning point (covering memo
records, wrappers, installed copies and live frames behind `home`'s anchors). Repairs `run`-target
resolution so name-records rebind **by shape**: qualified `module:Owner.name` through
`symbol_named`, `src:`/`reify:`/`patch:`/`#label` through the program-image label→head lookup,
learned heads through `code_named`; the emitter name-records every statically known `run` operand
(literal → `src:` label, `code_named`/`reflect`-born → `d.name`, opaque → closedness refusal).
Makes `symbol_named`'s owner the module *only* — all symbols live on the module's `d.vars`
(§9.3's example corrected to `symbol_named("demo", "Feed")`, no agent-owner arm). Applies the
minor batch: `importValue` demotes unmarked handles to `None` and handles carry *promoted*
targets (no `CellImport{OpHandle}`/`MeshOp` kinds); genesis (e) scoped to `learn`-registered
heads' records; the publish window runs no turn between the flagged turn's end and `publishValue`;
the flag clears only after successful publish and restore; P4's already-approved wake named as
P16's only outside dependency; `opcode_named`→`getOpcodeKind`; memo keys ride a `+d.grab` scalar
key cell not `+d.name`; the `as`-name arm applies only to re-pointed declared clauses; the scope
stamp carries binding identity (scope id + ordinal); `reify:<src>:<serial>` spelled consistently;
`rankTail` cited at `cell_views.hpp:258`; `d.clause` is per goal/event; `learn_*`'s double-`reify`
priced in §13; and the residual micro-nits.

**1.6 — Sixth review revision (r6 disposition).** Sequences the genesis ordering **image-first**:
step (a) re-materializes the program image into the persistent manifold *first and ref-preserved*
— the ops-spool indices *are* the persistent `CellRef`s (`manifold.hpp:295`), so the image's
recorded positions `0..E` return below the extent by construction — before the checkpointed state
joins the numbering and before any `learn`-replay mints a cell; a program image arriving through
`importValue` ingress (ephemeral twins) or a sibling manifold reports an empty program-side set
by design. Names the extent's carrier: a field on the adapter-side `programRef` bind record,
recorded once at load, stable across geneses of the same checkpoint, asserted never re-derived.
Splits genesis's name-record writeback **by `+d.name` shape with the slot convention** (R6-B):
qualified Call-operand records write `int64(entryOp)`; label records (`src:`/`reify:`/`#label`
run-target and fragment-identity records) write `int64(head)`, because `run` derefs `+d.entry`
*and* `+d.values` — run/`Code` operand slots are head-valued by construction (§6.2). Applies the
minor batch: `patch:` removed from the rebind shapes (a clause-name convention, not a head
label); the `reflect`-born `run` emitter-recording arm dropped — wrappers are session-side,
neither `d.code`-registered nor program-image, so an opaque or `reflect`-born `run` target in a
learnable fragment is closedness-refused; call records always `module:`-qualified, the shape test
splitting on first-`:` only when the segment resolves as a module, else verbatim label, with the
automatic prefixes `src:`/`reify:` label-reserved; M13-cloned literal labels take expansion-site
`src:` + `+n` clone ordinals; `d.stack` attributed to cursor cells, never `home` — live frames
stay off the durable set by unreachability, not pruning — and the state-side root exemption
stated (a program-side `agent_cell()` cannot prune its own durable set); literal heads re-resolve
through the program-image label→head lookup only, `code_named` staying `d.code`-only;
`ensureDimension` find-or-mint; `plan_named` matching clause `+d.name` labels; user `#label`
uniqueness enforced at emission; `closed` gains the two-arm clause (a program-side capture link
without a name-record is the refused class on both arms); and the nit bundle — the mark is
`+d.ref` on the record plus `d.mesh.relocation` on the cell, the `writeValue` cite range, the
`manifold.hpp` ref range, the glossary `closed` run-target arm, "left dead" → "left untouched",
per-record memo label freshness, label-less (bare-op) attach cells never memo-hitting, and the
installed-copy durability phrasing.

**1.7 — Seventh review revision (r7 disposition).** Re-roots the checkpoint model in a **named
store lineage**, replacing the recorded-extent construction the three converging lanes (vortex,
architect, zigzag) found unmechanized. The program image is the agent store's **low ops prefix**,
anchored at load on the `job.base` shared-base pattern (`mesh_jobs.cpp:304-318`) — the artifact's
ops segment loaded byte-identical, since positions are only stable when the prefix is — so the
image occupies persistent positions `1..E` before any state op is written. `receiveValue` already
folds the whole `[image 1..E][state >E]` band inside `rebuildManifold`
(`mesh_actor.cpp:260-271`, `manifold.cpp:427-442`), and `sealLocalSpool` seals the complete spool
(`mesh_value.cpp:227-228` calls it with `opsAlreadySealed = 0` and `:241` hands the sealed segment
to `publish`; `publication.cpp:611-617`,`:664-672`), so every checkpoint inherits the prefix
position-by-construction and
`WorkerContext` receives the folded manifold const — genesis *binds* what arrived; it cannot
sequence "image first", and the v1.6 image-first narration is withdrawn in favour of (a) lineage
bind, (b) `ensureDimension` find-or-mint + `setOpcodeKind` re-registration of the folded image's
op cells (rationale moved off "stale indices": `opcodeMap_` is empty per `WorkerContext` after
`resume`, and re-running `mintOpcode` would mint ephemeral twins, not register the folded cells),
(c) `programRef` bind, (d) `d.patch` replay, **(e) the `+d.refs` rehydrate/rebind sweep last** —
the
R7-B reorder, moved after replay so rebinds see post-installation symbols and the replay-minted
installed copies' regenerated records (copy-inheritance is the named dependency: a replayed
install's slots inherit the registered head's current contents). `E` is the image prefix length
recorded on the adapter's `programRef` bind record, chain-stable through the continuation's
`programRef` pin (`mesh_actor.cpp:199,215-216`). Prefix maintenance is an **adapter publish
invariant**, not an assertion: a prefix-less lineage (foreign ingress, sibling manifold, a
publish off a foreign store) yields an empty program-side set and the discriminator **fails
closed** — `learn` refused, never a silent state→skeleton admission; a `programRef` naming no
cell of the received manifold is a genesis bind refusal. "Program cells do not checkpoint"
becomes "not *republished* per agent — they ride the shared base lineage" throughout. Applies the
minor batch: the name-record writeback keys on the **record's slot role** (`+d.grab` "call"/"run"
label), `+d.name` shape selecting only the resolution path — a qualified `run`-role name resolves
through the qualified path yet writes `int64(head)` like a label record, and a head-less resolution
(a `def` symbol) is the named **headless-`run`** refusal; `src:`/`reify:`/`patch:` are reserved at
module declaration (the "module literally takes that name" arm deleted); `plan_named` unified as
verbatim clause `+d.name` matching, never `d.vars`; `home` after `resume` is the persistent base
home `initGenesis` adopts (`vortex_core.cpp:30-36`, `manifold.cpp:450-451`), and frames stay out
because no `+d` edge reaches *into* `home`, not because home is ephemeral; P16's promote clause
names `OpHandle`/`ForeignOpHandle` translation (`arena_manifold.cpp:1934-1954`); `d.name`
corrected to a single-link label dim, not a rank; the `#Call` convention reconciled as frame +
head-valued slot, not a `Call`-opcode operand; foreign `code"…"` heads from other artifacts named
as having no re-resolution spelling this round (new U-M11); nested literals label by innermost
`src:` plus chained `+o:c` arms with clone ordinals appended; patch target descriptors split —
clause targets via `plan_named`, method/symbol targets via `symbol_named`'s module-`d.vars` walk;
and the nit bundle — the P16 row's op-or-head hop restored, the capture-wing doubling removed
(the cells a wing anchors are the items, the wing the anchor), the dangling "pre-head convention"
phrase resolved to the §6.1 op-or-head rule, the pump-ordering claim re-scoped as proposed
adapter discipline (the ceremony issues `publishValue` synchronously from the flagging callback;
`pumpOne` supplies no ordering on its own), `src:` shorthand clarified against `#label` overrides,
the `code_named` record-key made literal-`L`-only, the label→head walk restricted to
fragment-head layout, user `#label` uniqueness scoped to the merged program emission set, the
"Both match labels verbatim" gloss split by representation (symbol names live as cell content,
clause names as `+d.name` label cells), the `extent` comparand named as the image prefix length
with the `programRef` pin as its stability source, and `0..E` corrected to `1..E`.

**1.8 — Eighth review revision (r8 disposition).** Names the publish `Store`'s owner the lineage
model was missing (R8-A): **the adapter constructs and keeps it** — `spawn()` accepts no store and
`resume()` keeps its internally built one private (`mesh_actor.cpp:156`, `:260-269`), so the seam
is owed adapter-API surface alongside P4's approved wake, stated at §6.6, §10.1's adapter step,
§10.2's preamble and P16's register row. Seeding is **position-stable, not byte-identical** —
`applyOpsSegment` remaps `ScrollId`s when the artifact's sealed ops segment lands in the agent's
store (`publication.cpp:1048-1086`,`:1030+`), so what the lineage preserves is identical op
positions and sequence, and v1.7's "byte-identical" gloss is corrected (per-agent `Store` objects
are never shared even when artifact bytes are; an artifact upgrade is a new `programRef` and a
respawn, never a mid-chain migration — spec §4.4's arena-load wording marked superseded). Fresh-
store publication becomes the fail-closed arm *by detection* — today's only publish path, the
fresh `Store` of `mesh_actor_resume_test.cpp:125`. Applies the minor batch: the `plan_named`/
`symbol_named` `run`-operand arm dropped outright — both builtins return `Opt<Cell>` where `run`
demands `Code<T>` (M3 bars the upgrade), so the session-side headless-`run` case is a type
refusal and `code_named(L)` with a qualified `L` is the only route to a qualified `run`-role
record; the patch-target descriptor gains a `patch:<n>` verbatim clause arm, and declared
handlers' anonymity (spec V15 `on Event [guard] { }` mints no `+d.name`) is stated as the durable
patching gap; genesis (a) goes positional — every `program()`-walk cell `≤ E` and walk-max `= E`,
a merged/re-seeded lineage (`mergeValues`→fresh store) failing closed, not merely an absent
prefix; (d) hands (e) its install set explicitly and (e) sweeps `learn`-registered heads *plus*
replay-installed copies; the `OpHandle`-kind-copy claim corrected — `reify`'s `New`/`Link`/`Value`
replay cannot mint handle-kind cells, so copies' ref slots self-rehydrate through inherited Int64
while their name-record slots still need the rebind arm, restated as the sweep's two reasons
(coverage, ordering); nested-literal labels corrected — base `src:` is the *outermost* site (1.7's
"innermost" reading inverted the construction) and `+o:c` arms descend inward, `+n` ordinals
stated deterministic per expansion order, a user `#label` inside a multiply-expanded body refused
as un-cloneable; `code_named` gains the `d.code`-miss → program-image-walk fallthrough (the
disjointness argument makes it unambiguous) and the `label_of` read is named the `reify:`-label
workflow. Applies the writer/nit batch: `(step (e) below)` ref corrected; `patch:` re-subjected as
the clause-name convention beside the `src:`/`reify:` label prefixes (never an emitter prefix);
the name-record lookup's head-layout restriction stated to keep *clause* `+d.name` labels out —
symbol names are content, never `+d.name`; `mailbox.front()`/`pop_front()` cited at
`mesh_actor.cpp:467-468`; `sealLocalSpool` cited at its `mesh_value.cpp:227-228`,`:241` call site
beside the `publication.cpp:611-617`,`:664-672` machinery; the `getOpcodeKind` rationale moved to
§6.5's write-guard (map *membership* is what P15's refusal and re-emission need; `asInt64` decode
already answers kind); `home` reachability stated as no link *path* in either direction; the
"name-record sweep" shorthand dropped — the sweep covers ref-records too; the `+d.refs`
diagram's genesis-rewrite slot line had its `(link eviction, §4.3)` cross-reference corrected —
eviction is §6.1/M14/`link`'s
`arena_manifold.cpp:1347-1377`, not the trail section; `run(inc, …)`'s cost spelled a `#Call`
frame plus spill (never a `Call`-opcode operand); P16's row gains the seam pair and the
qualified-run trigger; and `arena_manifold.cpp:1952-1954` widened to `:1934-1954` (the
translation arm covers both handle kinds). Chronicles belatedly the still-unrecorded edits —
v1.7's memo keyless-record sentence (§3: a label-less attach cell appends no memo record, since a
keyless record could never hit) and v1.6's `module vars`→`member vars` rename plus `cap$N` alias
rephrase.

**1.9 — Ninth review revision (r9 disposition).** Repairs genesis (a)'s landmark conflation
(R9-A): `E` was the ops-segment *length* while cell refs are birth-op *positions*
(`store.hpp:677-681`, `manifold.cpp:434`) and `promote` emits links last
(`arena_manifold.cpp:2010-2017`), so `walk-max == E` refused every honest artifact and the
un-pruned walk's `>E` reaches (`+d.code`/`d.vars`/`d.belief` state) refused every stateful one —
replaced by the two distinct landmarks: `E_ops`, the image prefix length *in ops* and the
ref-space ordering bound, and per-walk-cell *classification* (each `program()`-walk cell tests
its own ref against `E_ops`; `>E_ops` classifies state-side, never refused). Merged-lineage
detection moves to a **mandatory ops-segment content comparand** — canonical post-remap form,
op kind plus non-scroll fields — the only stated test that catches within-band permutation
(`adoptOpRecords`' topo-resort, `store.cpp:3836-3940`); the per-cell test detects non-image
ingress; and (b)'s registration scope now keys on the comparand-verified band, widened to the
spawn-time image bind since `opcodeMap_` is equally empty there. Respawns the spawn-side store
seam (R9-B): the seeded store is bound **before** the `WorkerContext`/`initGenesis` freeze by
resume's own base-injection shape (`mesh_actor.cpp:271-273`, `worker_context.hpp:52-54` — a
post-construction `attach(Space)` cannot re-deliver the `home_`/dims adopted at
`vortex_core.cpp:14`,`:29-70`), collapsing the asymmetry to `resume`-side supply-or-verify;
`ArenaManifold::store()` is already public (`arena_manifold.hpp:269-271`), so detection needs
no new surface and only sanctioned *supply* is owed; the seeded store's `documentId` handling
is stated (`store.hpp:133`,`:1435`, `restorePublication`'s `Store(userPermascroll, storeId)`
precedent at `publication.cpp:1127-1128`); and the verify comparand is named the canonical
post-remap form (op kind plus non-scroll fields — `CompactOpNode` bytes differ under the
`ScrollId` remap, `publication.cpp:1048-1086`). Restates copy pre-rehydration (R9-C): a replayed
install's ref slots derive from the regenerated ref-records' **capture endpoints** — each
*recipe* record's `+d.values` naming the referent fold cell at the promoted position, the
identical bits `*handleTarget` carries on the external arm — never the donor's "current
contents" nor a replayed stale literal,
reconciling §6.1's recorded-template rule; the "two reasons" header is relabelled
coverage/ordering. Applies the minor batch: `ops.nodes` corrected to the sealed `ops` segment
(`sealedOpsName`, `provenance.hpp:286`; `applyOpsSegment` is file-local, `historyFromSeal` the
public apply seam, `publication.cpp:1030`,`:1093-1103`, `\x7fXSO\x01` gate `:1033`); the P16-row
"spawn store parameter" reconciled to supply-side base-injection; the link-emission cite moved
off the `OpHandle` *value* arms to `setLink` plus the `real` identity map
(`arena_manifold.cpp:2041`,`:1868`); `mergeValues`' order named `(contentHash, authorKey,
parent)` (`mesh_value.cpp:519-523`) with the fresh-store destination scoped to the
no-`job.base` arm (`mesh_host.cpp:507` vs `:509-510`); `plan_named`'s row stops claiming
declared event-handler clauses (spec V15 mints them no `+d.name`); `code_named(L)`'s
unresolved-at-emission convention stated — the slot stays head-less and name-recorded, `run`
refusing at the descriptor check until the record's genesis rebind, a head-less qualified
resolution being the documented headless-`run` refusal; the anonymous-handler gap extended to
durable replace/remove (`learn_handler` coexists under `priority`, `unlearn` has no name to
match); `label_of`'s `cells"…"` acquisition path named (`Code<Cell>`→`Cell` has no coercion);
`patch:<n>`'s `<n>` pinned to the `d.patch` ordinal with `patch:` refused as a user `name` at
`graft_*`/`learn_*` emission; `learn_handler` noted additive-not-deduped; the sealed band
corrected to `1..opsTotal`; and the residual nits — the `spawn()` signature cite
(`mesh_actor.cpp:119-122`/`mesh_actor.hpp:132-135`), the `d.code`-band's `1..opsTotal` arm, the
`home` claim's weight moved to the prune (`arena_manifold.cpp:1755-1757`), the `+o:` arm's
`<off-line>:<off-col>` unit, the §5.6 *no program lineage* diagnostic, the `+d.refs`-diagram
eviction xref relabelled, `mesh_jobs.cpp:304-318`, and the `program()` walk's root named at
(a).

**1.10 — Tenth review revision (r10 disposition).** Closes the `patch:<n>` minting gap
(R10-A): `learn_*` mints the installed clause's `+d.name` itself as `patch:<n>`, `<n>` the
`d.patch` ordinal that same call creates — only the verb can mint it, since the ordinal belongs
to the patch being appended — while the user `name` rides the patch record as an *alias*
recorded alongside, never the label; §9.3's example now passes `"near_water"` as the alias, and
`patch:` stays reserved for the verb's minting everywhere user names are spelled. Identifies
genesis (b)'s op-cell set (R10-B): `setOpcodeKind` re-registers *exactly the recorded
instruction enumeration* the artifact writes at emission (the `mesh_evaluator.cpp:658-663`
precedent — ops are known by construction), never a value scan — an `asInt64 ≤ Linked` sweep
would register `dimConstant` operand cells and put honest data under P15's refusal — and never a
`mintOpcode` re-run, which mints ephemeral twins. Completes the seed ceremony (R10-C): the
adapter's lineage `Store` is given its span source like any restored history —
`setContentSource` plus `bindPublishedLocalScroll`, `restorePublication`'s own pair at
`publication.cpp:1129-1131` —
because `historyFromSeal`'s bare fold installs no span source and every artifact-local span
(dim names, `+d.name`/`src:` labels, symbol names) would dead-read `""`, `initGenesis` minting
ephemeral twin dims; the adopt-`documentId` arm is named **unmechanized** (`historyFromSeal`
mints `Store()` internally, `publication.cpp:1097`, `applyOpsSegment` file-local, `Store()`'s
own `UserPermascroll` mint at `store.cpp:124`), owed with the supply-side seam; and the
"neither exposes a store" claim is narrowed — `resume` already transports a caller `Store`
inside `ReceivedValue checkpoint` (`mesh_actor.cpp:182`, first read at `:188`'s
`!checkpoint.store` guard, `:261`, dropped
before `receiveValue`'s rebuild), so the owed change is trusting or comparing it, not new
transport. Corrects the capture endpoint (R10-D): a ref-record's `+d.ref` slot identifies *which*
cell's referent is captured while the endpoint itself rides the *recipe* record's `+d.values`;
internal referents rebind through the old→new reify map to copy twins, and `*handleTarget`
equality holds for external/capture referents only. Pins the comparand (R10-E): the adapter
decodes *its adopted lineage store's own `1..E_ops` band* — the `adoptOpRecords`-folded
`OpRecord`/`CompactOpNode` sequence in canonical order — against the received store's decoded
band pairwise, an order-vs-order comparison, never segment bytes against decoded records.
Applies the minors: `label_of`'s registered-head path re-anchored `cells"$a/+d.code"` off
`agent_cell()` (`d.code` never hangs on arena `home`; the `[Cell]`→`Cell` indexing step named),
and `code_named`'s row marks its space head-labels-only; `WorkerContext`'s real call shape
stated (ctor `worker_context.hpp:52-54` — `resume` passes `base` alone and calls
`arena.setDefaultSpanReader` after, `mesh_actor.cpp:271-273`; the spawn arm supplies both);
`opcodeMap_` removed from `initGenesis`'s freeze set (it is `VortexStdLib`-populated VM state;
`initGenesis` adopts `home_`/dims only); the walk vocabulary names its two roots — agent-cell
legs (`+d.code`, `d.contract`, `d.belief`, `d.source`) enumerate only under a second walk
rooted at `agent_cell()`; non-image ingress restated as classification, both arms named
(persistent `≤E_ops` program-side, `>E_ops`/ephemeral state-side — a `>E_ops` twin is admitted,
not detected); the unresolved-`run` refusal splits **fail-until-resume** from **fail-forever**,
the head-less slot's observable content named `None` (unwritten, no sentinel) and the slot
declared emission-fixed — a same-session `learn` registering `L` does not fill it, only
genesis's rebind does; `unlearn`'s LIFO-by-name limit among same-named learned handlers stated
(the clause-cell argument cannot pick an earlier twin out of order); and the cite/type nits —
`setLink` `arena_manifold.cpp:2041`, `real.emplace` `:1868`, `CompactOp`→`OpRecord`/
`CompactOpNode`, the `historyFromSeal` range widened to `publication.cpp:1093-1103` so the
`applyOpsSegment` call at `:1100` is inside it — applied throughout.

**1.11 — Eleventh review revision (r11 disposition).** Pins `patch:<n>`'s `<n>` to a **durable
monotone serial** off `d.vars` — the counter `reify`'s `<serial>` already rides — never the
`d.patch` rank position (R11-A): `unlearn` drops and replay-drops renumber the rank, and a
recycled ordinal would re-mint a live `patch:<n>` and misbind every descriptor recording it, so
ordinals are never reclaimed; replay transports the recorded `+d.name` verbatim — minting no
patch and re-deriving no label; and the `learn_*` dedupe consequence is stated — a *qualified*
alias matching a declared clause's stored `+d.name` unlinks it and installs `patch:<n>`, leaving
the user name dead (name-preserving replace goes through `learn_where`/`learn_body`). Corrects
the verify arm's destination (R11-B): the comparand/publish store is the one holding the
*received* band — the runtime's base store, `arena.base()->store()` (`manifold.hpp:486`) — the
adapter's seeded store demoted to its true role, the spawn-side lineage anchor; a kept-in-step
adapter twin is named as owing a segment-apply surface `adopt`/`historyFromSeal` do not provide.
States the **ref-capture record** shape explicitly (R11-C): one recipe record per `+d.refs`
ref-record — `+d.grab → the same slot cell` (a second `+d.ref` would evict the first, M14),
`+d.values → the referent endpoint` for internal/state-side referents, `+d.name → the dim name`
for program-side referents, resolved at replay through `ensureDimension`'s name arm — never a
`+d.values` edge to a program-side cell, which is the `d.code`-rank/`closed` violation. Corrects
the stale same-name claims (R11-D): learned clauses carry unique `patch:<n>` labels, so
same-named *learned* clauses cannot exist, `unlearn` selects a specific twin in any order,
`plan_named` zero-hits the alias (the alias is write-only audit metadata — rediscovery is
`clauses_of` + the `patch:` prefix or `plan_named("patch:<n>")`), and the surviving `+d.name`
ambiguity is same-named **grafted** clauses only. Applies the minors: the seed pair's stakes
split — `setContentSource` carries the dead-read/read-path stake, `bindPublishedLocalScroll`
the scroll-identity stake (`sealPendingMetadataAsCells`' `publishedLocalScroll_ == localScroll`
arm, `store.cpp:4081-4089`; `scrollRegistry`'s resolved-scroll set, `manifold.cpp:1503`);
`bindPublishedLocalScroll`'s `self` is recovered by `store->addScroll(from)`'s
idempotent re-add (`store.cpp:1388-1405`), `historyFromSeal` returning no `self`; (b)'s recorded
enumeration is pinned to `produces` `MicroversionId`s or the canonical post-fold order with the
linear-`parent`-chain invariant named — `adoptOpRecords` topo-sorts a nonlinear DAG
(`store.cpp:3836-3940`) and the fold-vs-fold comparand cannot catch permutation; `unlearn`'s
symbol arm composes the qualified `module:Agent.member` the patch recorded against the
argument's bare `Agent.member` content; `d.contract` is op-anchored (`vortex_stdlib.cpp:603`,
`:628`), the agent-leg list gains `d.patch`/`d.synapse`/`d.weight`, and the second walk's
consumer is named `closed`'s frontier enumeration; `learn_method`'s qualified-success corollary
stated (only a `learn_method`-installed head-valued symbol ever binds a qualified `run`-role
record), the qualified-verbatim dedupe made explicit (`learn_plan` on `"demo:Forager.travel"`
unlinks the declared clause; an unqualified alias appends), and the `learn_method`
replace-vs-`learn_plan` append asymmetry drawn; the seed step resolves the segment *bytes*
through `restorePublication`'s resolve loop shape (`publication.cpp:1138-1162`), signed-inventory
checks skipped at seeding; and the citation/wording nits — `mesh_actor.cpp:186`→`:188` (the
`!checkpoint.store` guard), `store.cpp:123-124`→`:123`, the "patch:
convention"/`patch:<ordinal>` spellings to `patch:<n>`, and the hydrate/rehydrate stem split
(span-source install vs `OpHandle`→`Int64` rehydration).

**1.12 — Twelfth review revision (r12 disposition).** Re-keys the ref-capture record off the
single-occupancy dims it would evict (R12-A): the slot correlation uses a dedicated `d.refFor`
dim — a first-position input slot's `-d.grab` already carries the opcode's own wing edge
(`vortex_core.cpp:384-390`), so a `+d.grab` correlation decapitates the input wing at emission
and every `reify` replay — and the referent rode a dedicated `d.captured` dim, `+d.values`
barred because a `d.vars` referent's `-d.values` is live and shared referents collided on the
single-occupancy slot (one `+d.captured` hop to the earlier record instead — the collision R13-A
then proved `d.captured` merely relocates to the shared referent's `-d.captured`, 1.13 dropping
the dim for a content-carried endpoint); `+d.name` keeps the program-side arm and the record
shape is restated in §6.1's enumeration, the head diagram and genesis (e). Makes the
`learn_plan` dedupe durable (R12-B):
genesis (d) replays each patch's *verb*, re-matching the recorded alias verbatim against the
re-attached goal rank's clause `+d.name`s and re-unlinking the hit — a dedupe-displaced
declared clause folding back at `1..E_ops` is displaced again, never resurrected beside
`patch:<n>` — while `learn_method`'s unlink-first rides `graft_method`'s own verb and
`learn_handler` re-appends; the dedupe scope is pinned to the install goal's own `d.clause`
rank, the self-contradiction corrected (an unqualified alias verbatim-matches *any* clause
`+d.name`, declared or grafted — it appends only on no live hit), and the surviving ambiguity
restated as any same-named pair a verbatim `graft_*` `name` creates, graft-on-graft or
graft-shadowing-declared. Registers the kept-in-step twin's **segment-apply surface** in §10.2
(R12-C) — `adoptOpRecords` takes decoded records, `historyFromSeal` mints its own `Store`
(`publication.cpp:1097-1102`), nothing applies a received segment into a store the caller
holds — owed for the twin only, the `base()->store()` verify arm standing alone without it.
Harmonizes the comparand to one canonical statement in genesis (a), §6.6's verify arm citing it
(R12-D): order-vs-order post-remap `OpRecord`/`CompactOpNode` fields — op kind plus non-scroll
fields verbatim plus scroll *identity* by `scrollKey`, never the raw remapped `ScrollId`, with
`Store::scroll`'s optional disengaging on `localScroll` so local ids compare as ids
(`store.cpp:1461-1469`, `spool.hpp:19`). Applies the minors: the serial pinned to the agent
cell's `d.vars` everywhere (module `d.vars` is program-side); replay `+d.name` transport is a
re-mint — a fresh label cell with verbatim text, never a re-link, which single occupancy would
evict off the record — two equal-text `patch:<n>` cells per clause-installing patch; (d)'s "no
serial consumed" scoped to `patch:<n>` (the install copy's `reify:<src>:<serial>` still mints);
per-verb `+d.name`/serial behaviour stated for `learn_where`/`learn_body`/`learn_method`; the
alias→clause join spelled as patch-rank alias field → `plan_named`; `unlearn` refuses a
`d.patch` record argument by its `-d.patch` backlink; the `≤E_ops` agent-rooted walk claim
scoped for `agent_cell()` itself program-side; `d.weight` de-ranked to the §8.1 scalar edge
dim; and the citation/wording nits — `real.at`→`real.emplace`
(`arena_manifold.cpp:1867-1868`), `store.cpp:3841`→`:3840` (the `opsSpool.contains` line),
`store.cpp:123`→`:124` (the `userPermascroll_` mint, `:125` `chronofilade_`), the last
"publish `Store`" to "lineage `Store`", "the hydration" to "the span-source pair", and the
`d.patch`-rank ordinal read pinned to the patch record's `ordinal` field.

**1.13 — Thirteenth review revision (r13 disposition).** Drops `d.captured` entirely (R13-A —
the five-lane convergent major): a shared referent holds only one `-d.captured` backlink, so
the v1.12 edge merely relocated M14's collision — ≥3 same-recipe sharers self-evict under a
star reading, "one hop" fails under a chain reading, and cross-head sharing is unencodable,
every `reify` regenerating `+d.captured →` the same external endpoint and severing the
registered recipe's edge (routine in practice — `compiler.cpp:531-532,781,796`'s shared
`cells_->zero`-class referents). The capture endpoint is now carried as *content*, never as an
edge into the referent: external/state-side endpoints ride the slot's own `int64` ref
position in-session or `*handleTarget` after promotion (what `promote` already
translates), internal referents rebind through the old→new reify map, program-side endpoints
ride the `+d.name →` dim-name arm re-resolved through `ensureDimension`, and `+d.values` on a
recipe record may name *its own* endpoint cell only where that one record owns the edge;
`d.refFor` alone correlates record→slot. Restated in §6.1's enumeration, the head diagram and
genesis (e), `d.captured` struck from every minted-dim list, and `d.refFor` restored to §10.1's
and §14's enumerations, which had dropped it. Applies the minors: the comparand's
scroll-identity arm takes `globalKeyOf`'s shape — `breakMarkerScroll` compares by
`breakMarkerKey` (legitimately sealed and identity-remapped, `publication.cpp:1050`), an
engaged external id by `scrollKey`, any other disengagement failing the compare; the
comparand's reference side is pinned to the lineage store's decoded band, the verify-arm
pronoun ambiguity resolved; the alias reads "no session resolver consults it — the replay key
and the `d.patch` rediscovery join do", names the superseded target only on a dedupe hit, and
the rediscovery join's multiplicity is stated (every match, the caller picking by `ordinal`,
latest-wins); the record label is a `label_of`/genesis-transport handle, never an `unlearn`
key; the example premise corrected to `learn_plan("Patrol","demo:Guard.cautious",…)` against
§9.2's declared clause; the replay dedupe covers the faithful *and* divergent victim cases —
a grafted-twin session hit displacing nothing durable, the replay re-matching the folded rank
to append or displace as it finds it, a replay-dropped intervening patch turning a session
append into a replay displace; the contended slot named the *label cell's*
`-d.name`; the `patch:`-shadows-`d.vars` claim removed — the `d.vars` walk matches `textOf`
content (`vortex_stdlib.cpp:1953`), a `+d.name` edge shadows nothing; agent-`d.vars` legs hang
on the agent cell's own `±d.vars` rank — reached only across a genuine cross-root edge (a §5.3
`cap$N`/`masterOf` capture link), never through a ref-capture record's slot-carried referent —
yet classify state-side, so no agent-rooted second walk is
owed; patch and installed clause carry equal-text `patch:<n>` labels on their own `+d.name`
label cells; the patch-record field list scopes `alias` to the verbs that take a `name`;
the §9.3 `on Meal` loop seeds a first weight — `set_weight` mints the keyed `d.vars` record
on first write, the `if [w] … else` arm covering the disengaged read; and §10.2's register
gains the `documentId`-adoption arm as owed surface (v) beside the segment-apply seam —
`historyFromSeal` mints `Store()` internally with no id parameter (`store.cpp:123`), the adopt
arm unmechanized until an overload or a `restorePublication` seed lands.

**1.14 — Fourteenth review revision (r14 disposition).** Restates the agent-cell-legs
reachability (R14-A — the four-lane stale-mechanism major): agent-`d.vars` cells hang on the
agent cell's own `±d.vars` rank — the agent cell the `program()` walk never `+d`-reaches —
and enter the sideness walk only across a genuine cross-root edge, the surviving case being a
§5.3 `cap$N` capture slot resolved to its binding cell through `masterOf`/`d.clone`; a
ref-capture record contributes no reach at all, its referent riding slot *content* the link
walk cannot follow, so the legs classify `>E_ops`/state-side by the two-root shape and no
agent-rooted second walk is owed. Sweeps the residual "capture edge/link" vocabulary the
content-carried endpoint stranded: the glossary `closed` row and §6.6's `closed` arms now
enumerate (a) state-side endpoints however carried — `cap$N` capture link or slot-carried
`int64` — (b) `+d.refs` name-records, and (c) program-side dim referents name-recorded on their
paired ref-capture record's `+d.name`; `closed`'s frontier lists `+d.refs` records, the wing-dim
capture-slot links and the `+d.recipe` ref-capture records read through their paired slots'
content; `reify`'s external-capture sentence reads slot positions; (d)'s hand-off names the
paired donor slot's promoted content as the endpoint source, the record supplying pairing and
class only; and the 1.13 entry's own "capture edges" phrasing is corrected. Applies the minors:
`closed`'s content-read gains the program-side carve-out — a `+d.name`-bearing record's endpoint
is the named dim, not a cell, and counts as name-resolved (the `+d.refs` name-record requirement
applying to call/run bindings, the dim slot's own record staying a REF-record); the (d)-time
endpoint read is `*handleTarget`, donor slots being `OpHandle`-kind until (e) — `int64` names
the in-session form only; the recipe link tuples' `+d.values`→endpoint obeys the same
one-record-one-edge conditional, a claimed endpoint riding the field's `int64` content for the
map to translate; displaced-vs-appended auditability is bounded to *declared* victims — a
session-grafted victim mints no `d.patch` record for the join, so the record reads append, which
is also what replay makes of it; the grafted-twin session hit keeps its replay-displace caveat —
the replay re-matches the folded rank, appending where no same-named clause survives and
displacing where one does (the 1.13 entry corrected to match); the "no session resolver consults
the alias" qualifier is mirrored into the glossary `patch` row and §6.1's replay sentence;
`documentId_` narrows to `Store`-ctor-or-load state — `store.cpp:4450` writes it on directory
load beside the two-arg ctor's `:135`; and the (v) item's `Store()` cite is corrected from the
`userPermascroll_` initializer line to the no-id signature (`store.cpp:123`).
