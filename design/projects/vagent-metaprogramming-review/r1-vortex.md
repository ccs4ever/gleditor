# Round 1 critique (vortex role): vagent-metaprogramming.md v1.0

Scope per lane: VortexCore execution semantics, opcode-graph lowering, trail/backtracking, handle
relocation, MeshActorRuntime invariants. All line numbers verified against the working tree under
`apps/common/xanadu/`. The doc's opcode claims are largely accurate: `mintOpcode` (:73-82),
`getOpcodeKind`'s map-first decode (:102-114), `Linked` appended last (hpp:89-93), `Choice` mark
(:217), `backtrack`'s release (:234), `Cut`'s discard (:255), `New`'s already-used refusal
(:736-743), `Linked`'s seen-set and `consumeWork` (:686, :699-703), `Halt` (:1220-1225), the
`+d.spin` advance (:1333-1335), the contract phases (:1229-1239, :1306-1322), `emitStatement`
(compiler.cpp:849-872), the `cells"…"` subset (cell_path.cpp:147-182), `pureBody`
(checker.cpp:827, :861-865), `validateType` (:553-567), `PrefixedStringLiteral`
(lexer.cpp:379-393, token.hpp:22), `checkpointView`'s quiescence gates (mesh_actor.cpp:668-694),
`resume`'s `baseRef == contentHash` binding (:215-216), `compact()`'s MarksOutstanding refusal
(arena_manifold.hpp:667-679), the conditional-trailing argument (:721-730) and its cpp mirror
(:1506-1570) all check out as cited.

## Findings

### 1. BLOCKING — §4.5/§6.6/M11: a promoted fragment's intra-graph references are stale Int64s; `learn` cannot work as specified

The durability story rests on "a `+d.grab` link to the fragment head drags the closed fragment into
the checkpoint's reachable set" and genesis re-grafting it. But a fragment graph's internal
addressing is **Int64 cell content, not links**: wing slots are `makeScalarCell(int64(operand))`
(vortex_core.cpp:384, :402), control targets are `reference() = makeScalarCell(int64(target))`
(compiler.cpp:516-518, used at :525, :549, :565, :582, :623, :635, :717, :865), and `#Call` frames
store return ops as int64 value bits (vortex_vm.cpp:862-863). `promote()` walks only links for
reachability (arena_manifold.cpp:1765-1784) and translates only link endpoints plus
`OpHandle`/`ForeignOpHandle`-kind values (:1797-1828, :1937-1954, :2010-2013); Int64 cells promote
verbatim (`store.makeScalarCell(asInt64)`, :1934-1936). `mesh_value.hpp:107-108` states it outright:
"No arena ref crosses this boundary." After `resume` the rehydrated fragment keeps its link skeleton
and op kinds (self-content Int64 decodes fine through `asInt64`) but every operand slot and
jump/branch/call/choice target names an old-incarnation ephemeral ref that either misses
`arena.contains` or — worse — lands in-range of the new `slots_` and silently aliases an unrelated
cell (arena_manifold.cpp:256-267). `wingOf` decodes `asInt64(slot)` only (vortex_core.cpp:358-364),
so a re-grafted learned fragment reads garbage operands and jumps to garbage PCs — silent
mis-execution, not a clean refusal. The codebase already solved exactly this problem once, at the
artifact boundary: `executeProgram` runs the graph on local Int64 refs, then before publication
rewrites every ref cell to `ValueKind::OpHandle` "so every reference is relocated again by the
receiver" (mesh_evaluator.cpp:636-643, :671-684, :720-736). M11 never engages with that mechanism,
and the fix is not expressible in `std:meta`: no opcode can write `OpHandle` kind — `writeValue`
covers Int64/Double/Bool/None only (vortex_core.cpp:203-227) — so a relocation pass must be C++.
**Fix:** register a P16: a checkpoint-time relocation pass over each `d.patch`-reachable fragment
(Int64 ref-cells → `OpHandle` + relocation marker, the :723-735 precedent generalized), plus the
genesis-time inverse (rehydrated handle → fresh Int64 ref, since `wingOf`/control ops decode Int64
only). Alternatively narrow M11 to session grafts only and defer durability — but then §6.6, §9.2
and the plasticity pitch lose their durable half.

### 2. MAJOR — §4.2/M9/§6.4: `cursorPC_` is the authoritative PC and is *not* trailed; the "PC is a cell value, so it is trailed" claim misstates the mechanism

`getCursorOpcode` consults `cursorPC_` first and falls back to the cursor cell's bits only when the
map misses (vortex_vm.cpp:116-128) — the map is the authority; the cell is the persistence backing.
`backtrack()` restores the PC not because the write was trailed but because it explicitly calls
`setCursorOpcode(*cursor, cp.altOp)` (:238-243), which rewrites *both*. A bare
`arena.release(m)` — exactly the "release(m0) undo" containment M9 and §6.4 invoke for runaway
mutation — restores the cursor cell's bits but leaves `cursorPC_` holding the post-mutation PC, and
the map wins: the cursor resumes mid-mutation, on an op that may have been truncated (decodes to
Nop → silent "Finished" while the cell bits say otherwise) or on a surviving op that resumes as if
never rewound. This is the identical untrailed-VM-cache-vs-arena-state hazard the doc correctly
identifies for `opcodeMap_` and prices as P15 — but the PC instance is missed, and the doc's own
containment story depends on it. vortex-distribution.md:112 likewise treats `cursorPC_` as
per-process scheduler state, not arena content. **Fix:** spec the containment as the drain idiom
the stdlib already uses — `while (choiceDepth() > base && backtrack(cursor))`
(vortex_stdlib.cpp:3800-3802, :3841-3843), which rewinds PC, stack top and marks in LIFO order —
and state explicitly that bare `arena.release` must never be called on a cursor's arena mid-turn
without a following `setCursorOpcode` (or register a P16 `vm`-side resync that rebuilds `cursorPC_`
from cursor cells after a host-driven release).

### 3. MAJOR — §6.4/M9: `release(m0)` with outstanding inner marks corrupts the mark machinery; containment needs an ordering rule

`release(m)` truncates to `m`'s recorded extents but pops only the *top* floor and decrements
`outstandingMarks_` once (arena_manifold.cpp:1695-1698); "marks are assumed well nested"
(arena_manifold.hpp:773). A bare `release(m0)` while `Choice`-pushed marks remain leaves
`choiceStack_` holding `ChoicePoint`s whose marks name extents beyond the new sizes: the next
`backtrack` calls `release(cp.mark)` with `m.trailSize > trail_.size()` /
`m.cellCount > slots_.size()`, and `trail_.resize`/`slots_.resize` *grow* the vectors with
default-constructed entries (:1657-1673) — ghost slots and garbage restores, silent corruption.
Separately, the doc's "dies at `maxCycles` and becomes a hard failure" glosses that work-budget
exhaustion is a `throw std::runtime_error` (arena_manifold.hpp:420-426, fired from `consumeWork` in
`Linked`/`New`/mint paths), not an `ExecutionResult` — it escapes `step()` uncaught and lands in the
executor's `catch (const std::exception&)` (mesh_actor.cpp:473-479 → `ActorDown`), outside any
result-code path the doc describes. **Fix:** state the ordering: on hard failure, drain choice
points via `backtrack` (per finding 2), *then* `release(m0)` — never `release` over live marks —
and name `consumeWork`'s exception channel in §6.4's failure taxonomy so the adapter spec catches
the right thing.

### 4. MAJOR — §6.5/§10.2 (P15): the proposal misses `step()`'s output write-back paths, the largest write surface to op cells

P15 guards `executeOpcodeBody`'s `Value`/`Splice` cases and `runPipeline`'s
`writeValue(paramCell)`, but `step()` writes `newOutputs[i]` into `outCells[i]` for every
non-structural op (vortex_vm.cpp:1288-1294) and the memo-hit path does the same (:1269-1273), both
through `core_.writeValue` with no target screening. An op cell bound as another op's output wing —
unreachable in today's compiler output discipline, immediately reachable the moment grafted graphs
bind outputs, which is this document's whole subject — gets its `int64` kind rewritten by any
`Add`/`Eq`/`Value`-read, bypassing P15 entirely. A second placement problem: `getOpcodeKind` is
`VortexVM`-private (vortex_vm.hpp:119, :186) while `writeValue` is `VortexCore`'s
(vortex_core.cpp:196-229), so the refusal cannot literally live inside the shared write helper
without a supplied predicate or moved state — the P15 "Where" cell should say this honestly. (The
same screen is also needed conceptually for host-side `writeValue` calls such as
vortex_stdlib.cpp:2225/:2243's parameter writes, though those are C++-caller discipline per §6.5.3.)
**Fix:** amend P15's `Where` to cover all four VM write sites (`Value` case, `Splice` case,
`runPipeline` :337/:345, `step` :1271/:1293) — or better, specify one VM-side
`writeValueGuarded(target,…)` wrapper used by all of them, with the op-kind lookup where the map is
visible.

### 5. MAJOR — §6.1/§6.3/§8.1: `Cell`-parameterized verbs cannot name their targets; the handle→referent indirection is missing machinery, not notation

`graft_where(clause: Cell, …)`, `set_weight(cell: Cell, …)`, `sprout_synapse(from,to: Cell)` and
friends take ref-boxes whose `int64` names the target. But `Link`/`Break`/`New`/`Value` resolve
their subject through `resolveCell(inCells, inputs, i)` which returns the *operand cell itself* —
a bound ref-box resolves to the box, never the referent (vortex_vm.cpp:50-67; the wing decode at
vortex_core.cpp:358-364 turns a slot's int64 into the operand it names). The only opcode that reads
through a box's int is `Linked`'s `follow` input (vortex_vm.cpp:666-672) — traversal, not mutation
— and `cells"…%"` anchored on a `$` binding is refused outright (compiler.cpp:484-486), so the
high-level path cannot write through a handle either. §8.1's "`w := w + lr * s` is an ordinary
trailed write" and §8.3's "the update is `Value` on a scalar cell" are false as written: `:=`
writes a binding cell, and a `Value` op targeting a `Cell` handle writes the *box*. Every verb in
the §6.1 table needs the same plumbing — read the box's int64, `Value`-write it into the structural
op's input slot, then execute — an indirection idiom that appears nowhere in the document and that
makes "the surgery is `Link`/`New`/`Value`/`Break`/`Splice`/`Linked` — all present" (§11) true of
primitives but not of the composition. **Fix:** name the convention in §6.1 (e.g., a per-verb
"focus slot" preamble: `Value`-write the handle's referent int into a scratch wing slot bound to
the structural op), or propose a `Deref`-style operand mechanism as a P16; also restate §8.1/§8.3's
"ordinary write" claims to name the actual op sequence.

### 6. MAJOR — M6/§6.2/§8.5: `run` is not re-entrant on creation-bearing graphs — the spill restores values, not links

"Recursion through `run` is permitted" and §8.5's evaluate-mutate-score loop assume repeated
execution of one fragment graph. But a fragment containing `cells"…%"` creations lowers to `New`
with a *statically bound* output cell (compiler.cpp:793-799, :798), and `New` refuses to execute
when that bound output already carries a link on the target dimension (vortex_vm.cpp:736-743:
"Compiled creation output was already used"). The §5.8 spill discipline the doc leans on restores
cell *values* — it cannot unlink the bound output. A committed first `run` therefore makes every
later `run` of the same fragment fail; `body_of`/`test_of` on program clauses containing creations
inherit the same single-shot property (r10-vortex.md already flagged the coupling living in the
purity rule, not the op). **Fix:** say it in M6/§6.2 — either `run`/`graft` refuse fragments whose
graphs contain bound-output `New`s (checkable by `validate`), or the fragment emission must mint
creation outputs per activation (a different `New` calling convention — no bound output, dynamic
`newCell`), or the frame restore must restore link state too; priced per option.

### 7. MAJOR — §6.6/§9.2: `unlearn`'s recorded prior head re-violates the V12 amendment for replaces of program-original attachments

Patch cells for replaces "keep the prior head on a `+d.step` chain" so `unlearn` restores it. For
`learn_where` on a *program-original* clause — the doc's own §9.2 example (`Guard.cautious`) — the
prior head is program skeleton: a state-side patch cell linking it is precisely the state→program
edge M11's narrow V12 amendment exists to exclude ("never the program skeleton"), and `promote()`
will drag that program subgraph into every checkpoint. The clause is only safe when the prior head
is itself a closed fragment (i.e., unlearning a *previously learned* attachment). **Fix:** restrict
`+d.step` prior-head recording to closed-fragment heads; for replaces of program-original
attachments record no head — genesis restores the original for free because `programRef` reloads it
— and keep same-incarnation undo state on a session-only, non-`d.patch` structure that is never
reached by the checkpoint walk.

### 8. MINOR — §7.2: `validate` should require `Return` tails for runnable graphs, not "Return/Halt"

The spec'd check "+d.spin chains terminate at `Return`/`Halt`" accepts the one tail shape that
wedges a caller: a `Halt` executed via `#Call` parks the cursor on the halt op (vortex_vm.cpp:
1220-1225) without popping the frame `#Call` pushed (:859-871) — the post-`run` continuation never
resumes and the next `#Return` pops the wrong frame. `run`-reachable graphs need `Return` tails;
`Halt` is correct only for cursor-entry graphs like `emitStatement`'s.

### 9. MINOR — §8.6: `next_rand`'s "SplitMix-style mix over Int ops" hits the overflow-checked `Mul`

`Mul` returns an overflow `ExecutionResult` on `__builtin_mul_overflow` (vortex_vm.cpp:457-463);
SplitMix64's golden-gamma multiply overflows int64 for essentially every non-trivial seed, so the
verb as characterized faults instead of mixing. Spec a wrapping-safe construction — 32-bit halves
reassembled via `DivMod`, or a modulus-form LCG — and say so.

### 10. MINOR — §5.5/M5: `static`'s closedness rule should name the `cells"…"` and non-scalar-result cases

`pureBody` admits non-mutating `cells` paths (checker.cpp:861-865), so `static cells"##/+d.vars"`
reads the partially-built compile arena — deterministic per compile but reading compiler-internal
state the "closed over literals/statics" rule doesn't obviously intend to allow; and a `CellList`
or `Cell` result has no literal form to emit ("the value becomes a `literal()`" only covers scalar
`CellValue`). Say `static` refuses `cells"…"` and non-scalar result types.

### 11. MINOR — §6.1: `reify` must rewrite internal Int64 refs to the copy's cells

"Compile-assisted" copy of a literal-born fragment: copying the cells copies their Int64 contents
verbatim, so the copy's wing slots and control targets still name the *original's* cells — a shared
operand is mutated through both copies and the "closed copy" is not closed-independent. The reify
recipe needs an old→new map pass over ref-bearing cells (positionally identifiable: wing slots
under `+d.grab`/`+d.step`, control-target inputs of `Jump`/`Branch`/`Call`/`Choice`). One sentence
prices it; today it reads as free.

### 12. MINOR — §12/§6.2: the "inert foreign `Code`" mechanism is mis-cited, and `run`'s two failure paths are conflated

"`resolveCell` yields `nullopt` for foreign cells" is true for `Link`/`Break`/`Value` targets but
`Call`/`Jump`/`Branch` never contains-check their target (vortex_vm.cpp:839-841, :848-850,
:856-873): a foreign ref is `setCursorOpcode`'d and decodes `Nop` → `linked` fails → `Finished`,
leaving `produced` unset → the statement fails. The outcome ("inert") is right; the mechanism is
different. Related: a fragment hitting the shared `Fail` op backtracks into the *caller's* choice
structure past the `run` op entirely (the saved `stackFrame` relink at :240-243 is what makes the
unwind sound) — distinct from the "completes with `produced` false" path §6.2 describes; one
sentence should separate hard-Fail propagation from soft non-production.

VERDICT: NOT CONVERGED
