# Round 2 critique (vortex role): vagent-metaprogramming.md v1.1

Lane: VortexCore/VM execution semantics, opcode graphs, trail/backtracking, handle
relocation, promotion, MeshActorRuntime invariants. All claims re-verified against the
working tree under `apps/common/xanadu/`; sibling docs `vagent-agent-language.md` and the
r1 review/disposition. v1.1's code citations are accurate wherever checked:
`getOpcodeKind` map-first + `asInt64` fallback (vortex_vm.cpp:102-113), `setCursorOpcode`
map+cell write and `noCell` throw (:131-145), `Call`/`Return` frame mechanics
(:854-893), `Halt` non-pop (:1220-1225), `step` writebacks (:1269-1273, :1287-1294),
`runPipeline` writes (:337, :343-346), `writeValue`/`splice` funnel
(vortex_core.cpp:195-255), `bindInput`/`bindOutput`/`wingOf` (:354-414),
`resolveCell` (vortex_vm.cpp:50-67), the `mesh_evaluator.cpp:720-736` publication
pass, `isCellRelocationHandle` (foreign_op_handle.cpp:98-117,
constants foreign_op_handle.hpp:24-25), `promote`'s link/`OpHandle` handling and
Int64-verbatim copy (arena_manifold.cpp:1755-1810, :1863-1951), `importValue`'s
relocation fixup (mesh_value.cpp:390-509), `publishValueImpl`'s promote call
(mesh_value.cpp:168-171), `checkpointView` (mesh_actor.cpp:668-694;
mesh_actor.hpp:102-106), `release`/`compact` (arena_manifold.cpp:1596-1731),
`emitStatement`'s `Halt` tail (compiler.cpp:849-872), `pureBody`'s cells rule
(checker.cpp:861-865).

## R1-item verdicts

1. **r1-1 (BLOCKING, intra-graph Int64 refs die at promote)** — *resolved in shape,
   defective in mechanism.* P16 is registered (§10.2), `learn`/`d.patch`/`d.code`
   durability is made conditional on it (§6.6), §4.5 gained the
   "intra-graph reference ints" row, and the `+d.refs` mark rank plus
   genesis rehydration match the ruling. But the mechanism as written misses the
   recognition edge `promote` actually tests (finding 1) and omits the live-arena
   restore (finding 2). Not dischargable as converged.
2. **r1-2 (MAJOR, `cursorPC_` untrailed; bare `release(m0)` containment)** —
   *resolved.* §6.4 item 1 and M9 state the drain idiom verbatim
   (`while (choiceDepth() > base && backtrack(cursor))`,
   `vortex_stdlib.cpp:3800-3802, :3841-3843`), explain that `cursorPC_` is the
   authority, and forbid bare release; P17 is deferred-not-registered with the
   reason stated. Residual imprecision in the release guard: finding 9.
3. **r1-3 (MAJOR, `release` over live marks; `consumeWork` channel)** —
   *resolved.* §6.4 item 2 names `consumeWork`'s `throw` → executor `catch` →
   `ActorDown` path (arena_manifold.hpp:420-426; mesh_actor.cpp:473-479) and the
   well-nested-marks corruption is described correctly
   (arena_manifold.cpp:1695-1698).
4. **r1-4 (MAJOR, P15 over/under-cover)** — *resolved.* P15 is now an
   `opcodeMap_`-membership predicate at the `writeValue`/`splice` funnel covering
   all six VM write sites, with the fresh-cell exemption — the ruling's preferred
   shape. Residuals: refusal cannot surface as the promised `ExecutionResult`
   (finding 5); exempt-by-construction bypasses and the `masterOf` question
   (finding 10).
5. **r1-5 (MAJOR, `Cell`-parameterized verbs cannot name targets)** — *resolved.*
   The focus-slot idiom is specified in §6.1/M7 with the correct `resolveCell`
   evidence; multi-input ops inherit slot ordering from `bindInput`'s
   `+d.step` chaining (vortex_core.cpp:380-396) — worth one clause saying the verb
   replicates that chaining order, but the mechanism is sound.
6. **r1-6 (MAJOR, `run` not re-entrant on bound-output `New`)** — *partially
   resolved.* `validate` refuses bound-output `New`s in `run`-reachable graphs
   (§6.2/§7.2), alternatives priced in §11 — but grafted bodies/methods are also
   repeatedly executed and fall outside the stated scope (finding 4).
7. **r1-7 (MAJOR, `unlearn` prior-head re-violates V12)** — *resolved.* §6.6's
   "replaces of program-original attachments record no prior head" plus the
   genesis-restores-for-free argument is exactly the ruling.
8. **r1-8 (MINOR, `Return` tails)** — *resolved.* §7.2 requires `Return` for
   run-reachable graphs and correctly limits `Halt` to cursor-entry graphs
   (vortex_vm.cpp:1220-1225). Residual for reflect-wrapped program graphs:
   finding 13.
9. **r1-9 (MINOR, `next_rand` `Mul` overflow)** — *resolved.* §6.1/§8.6 spec
   32-bit halves via `DivMod` (the opcode exists) or a modulus LCG.
10. **r1-10 (MINOR, `static` closedness)** — *resolved.* §5.5 forbids `cells"…"`
    in any mode plus `believes`/`findall`/`run`/host calls, restricts results to
    literal-expressible scalars, and names `staticFuel`.
11. **r1-11 (MINOR, `reify` internal refs)** — *resolved.* The recipe (census +
    link tuples + literals + old→new map over copied ref-boxes) is specified in
    §6.1; `Clone`'s `masterOf` aliasing is the right refusal reason. Residual:
    `+d.refs` marks must retarget to the copy (finding 12).
12. **r1-12 (MINOR, foreign `Code` mis-cited; failure paths conflated)** —
    *resolved.* §4.5/§6.7 now state the dense-range-only `resolveCell` check and
    cross-agent aliasing honestly (`d.name` re-resolution rule); §6.2 separates
    soft non-production from a hard `Fail` that backtracks through the `run`.

## New findings

### 1. BLOCKING — §10.2 (P16) / M11 / §6.6: the OpHandle rewrite omits the `d.mesh.relocation→"cell"` edge `promote` actually tests

`isCellRelocationHandle` requires *both* `ValueKind::OpHandle` and a posward edge on
the literal dimension text `d.mesh.relocation` to a cell whose text is `cell`
(foreign_op_handle.cpp:98-117; constants foreign_op_handle.hpp:24-25). P16's pass
rewrites kind only, and the `+d.refs` records point *at* the ref cell on `+d.values`
(§6.1 head diagram, :739) — on the ref cell that is a `-d.values` (negward) edge on a
different dim, which can never satisfy the check. The consequence is not silent
staleness but outright refusal: in `promote`, an ephemeral `OpHandle` cell whose
target is ephemeral and which is *not* relocation-marked hits
`return std::nullopt` (arena_manifold.cpp:1797-1810, specifically :1802-1805), so
`publishValueImpl` fails with `"mesh value: promotion refused"`
(mesh_value.cpp:168-171). As specified, the first checkpoint containing any
`d.code`-registered fragment fails — `learn` produces an un-checkpointable arena.
The `mesh_evaluator.cpp:723-735` precedent the row cites does attach the edge
(`builder.link(cell, relocation, arena.makeCell("cell"))` at :735) — it was dropped
from the P16 description.

Verified surrounding substrate (so the fix is small): the `+d.refs` rank itself is
link-reachable from the head and promotes with the fragment; genesis can read it
through base fall-through (`linked`/`dimensionsOf` delegate to `base_`,
arena_manifold.cpp:868-869, :958-961), `shadow` copies base edges whole
(:1495-1499), and `importValue`'s fixup (:494-501) writes `OpHandle` bits naming the
*local* target — i.e., import keeps relocation cells `OpHandle`-kind, so rehydration
to `Int64` is needed on every delivery path, which matches the doc's genesis step.

Also in this row: "the same old→new association `promote`/`importValue` already
computes" is not quite the mechanism — `promote` returns `Promoted{.version,.cells}`
(a BFS list, not a map, arena_manifold.cpp:1858-1861) and the store-side handle bits
hold *store* refs (:1948-1951). The workable primitive is `handleTarget` per marked
cell (arena_manifold.cpp:1090-1098): `setValueBits(Int64, handleTarget(cell))` at
genesis under the genesis mark.

Unverified corner worth one test: a generation-2 fragment whose marked ref cell
names a *base-manifold* (already-checkpointed) cell hits `promote`'s non-ephemeral
arm, which requires `store.getCompactOp(target)` to succeed
(arena_manifold.cpp:1809) — whether resumed-manifold refs resolve in the local store
is not established here.

**Fix:** the pass must also `link(refCell, "d.mesh.relocation"-dim, POS, shared
"cell" marker)` when flipping kinds (mint the marker once; the edge is inert while
the cell is `Int64`, so it can equally be emitted at fragment-emission time). Restate
the genesis step as `handleTarget`-driven `Int64` rehydration, and note the pass is
idempotent (skip cells already `OpHandle`) for repeated checkpoints and for ref
cells named on more than one head's `+d.refs` rank after surgery.

### 2. MAJOR — §10.2 (P16) / §6.6: no live-arena restore — the continuing incarnation keeps OpHandle ref cells

`checkpointView` hands `const ArenaManifold&` — the actor's *live* arena
(mesh_actor.hpp:102-106); checkpointing is non-destructive by design (the only
refusals are quiescence gates, mesh_actor.cpp:668-694) and `pumpOne` keeps running
the actor afterward. The doc specifies rehydration only at "genesis/graft-time", so
after a successful publish the marked ref cells remain `OpHandle`-kind in the arena
that keeps executing. On the next execution of that fragment: `wingOf`'s
`asInt64(slot)` fails → operand resolves to `masterOf(slot)`, the slot *itself*
(vortex_core.cpp:356-365) — inputs silently bind wrong cells; and `Jump`/`Call`/
`Branch`/`Choice` targets decode `toInt(render(slot))` → `render` returns `textOf`
for `OpHandle` (vortex_core.cpp:294-311) → `toInt` → 0 (vortex_vm.cpp:38-48,
:839-873) → `setCursorOpcode(cursor, 0)` throws `invalid_argument` (:131-135) →
escapes `step` → executor `catch` → `ActorDown` (mesh_actor.cpp:473-479, :484-501).
Either the adapter intends "checkpoint implies retire-to-`resume`" — which the doc
never states, and which would make `learn`ed fragments un-runnable in a
checkpointed-but-running actor — or the pass needs a third step.

**Fix:** after `promote` succeeds, walk the same `+d.refs` marks and restore each
cell's kind to `Int64` — the OpHandle bits already hold the arena-side target
(`setValueBits(cell, OpHandle, CellRef)` at mesh_evaluator.cpp:733-734), so the
restore is a pure re-kind, `handleTarget` optional. Alternatively state the retire
policy explicitly and record that a checkpointed-but-live actor may not `run`
learned fragments — the honest version of the limitation.

### 3. MAJOR — §7.2: the `d.validate` seen-set is written in the evictable direction

The spec says "`marker +d.validate → visited`, membership tested as
`visited -d.validate == marker`" (:1068-1071). `link` maintains exactly one posward
neighbour per dimension — linking `marker +d.validate → visited₂` evicts `visited₁`'s
backlink (arena_manifold.cpp:1349-1377) — so the "seen-set" holds at most one cell.
Any cyclic `Jump`/`Call`/`Choice` target graph re-walks forever and `validate`
terminates only by burning its own per-op budget — it reports by dying, not by
returning `false`. The evicts-stale-marks benefit the doc cites still holds in the
correct direction.

**Fix:** `visited +d.validate → marker`; membership `visited +d.validate == marker`;
each visited cell carries at most one mark edge so marking is idempotent and the
next pass's fresh marker naturally supersedes stale ones.

### 4. MAJOR — §6.2/§7.2: the bound-output-`New` refusal covers only "`run`-reachable" graphs; grafted bodies are repeatedly executable too

The single-shot hazard r1-6 identified is not `run`-specific: a grafted plan body,
handler body, or method executes once per plan selection / event / call. A body
containing `cells"…%"`-born `New` ops binds the output cell at first execution
(compiler.cpp:793-799 — `created` temporaries are bound per *emission*, not per
activation) and `New` refuses the already-linked bound output on every later
activation (vortex_vm.cpp:736-743) — a `learn`ed plan that fires twice faults inside
plan selection, with the only containment being fuel + `ActorDown`. `where` tests
are `pure` and so cannot contain `New` at all under the impure-kind scan — bodies
are the exposure.

**Fix:** widen §7.2's rule to "any repeatedly-executable fragment" — every `graft_*`
target — or state explicitly that grafted bodies execute under the same
`#Call`/frame discipline and thereby count as run-reachable. Note that
`test_of`/`body_of` wrappers of program-side graphs will (correctly) fail this
check when the program body contains `%`-creations; say so rather than letting an
implementer read `run`/`graft` on wrapped graphs as permitted.

### 5. MAJOR — §6.5 item 2 / §10.2 (P15): the funnel cannot produce the promised `success=false ExecutionResult`

`VortexCore::writeValue` returns `std::optional`; `splice` delegates to it
(vortex_core.cpp:195-255). A refusal therefore surfaces as `nullopt` — which
`executeOpcodeBody`'s `Value`/`Splice` cases map to a `noCell` *output*
(vortex_vm.cpp:783-790, :799-806), while `step()`'s memo-hit and generic writebacks
and both `runPipeline` writes *discard the return entirely* (:337, :343-346,
:1269-1273, :1287-1294). A refused output write is thus silent: the op reports
success and the output cell keeps its stale value — categorically different from
the doc's "A refusal is a `success=false` `ExecutionResult` the adapter classifies
like the numeric contract faults" (:947-948). The safety property (kind
immutability) holds; the diagnostic contract does not exist as specified.

**Fix:** pick the contract and spec it. Either (a) the funnel returns a refusal
reason (`std::expected`) and each site maps it — `Value`/`Splice` bodies to a
failed/noCell output, the four writeback sites to `success=false ExecutionResult` —
or (b) silent refusal is the documented semantics, in which case say a grafted
graph that aims an output at an op cell sees stale outputs and is caught only at
`validate`-time, not at fault-time.

### 6. MAJOR — §6.3 / §6.6 / M11: `learn` trusts the emission-time `closed` flag, which post-emission surgery can falsify

The `closed` flag records provenance *at emission* (`CompilerBinding` state-side
marker bit, §6.6 :1001-1003). A `closed` fragment can later be re-pointed at
program cells by the raw `cells"…"`/`Link`/`Value` surgery this document adds —
P15 guards kind rewrites, not links — after which `learn` appends it to `d.patch`
and drags the program skeleton into every checkpoint: the V12 violation §6.6's
"What `learn` refuses" paragraph exists to prevent, reached through the document's
own machinery. M11's *Price* mentions "a `std:meta` closure check before `learn`
accepts a fragment" (:314), but the `graft_*` check list (:871-875) and the
`learn_*` row (:779) say only "requires the fragment's `closed` flag" — an
implementer can build the flag read and never the re-verification.

**Fix:** state that `learn` re-verifies closedness at accept time: walk the
fragment's link frontier plus its `+d.refs` targets — every outside reference must
resolve to a state-side cell (the marks give the ref-cell list for free; the
runtime needs the arena-side equivalent of the emission marker — a state-root
reachability test the adapter supplies). One line; the marks and the descriptor's
signature rank already make it walkable.

### 7. MINOR — §5.3: the synthesized `let cap$N := <name>` is a snapshot under kernel `let` emission

`ExprKind::Let` lowers to `emit(child, localCell, …)` — the child's *value* is
written into the fresh local cell (compiler.cpp:705-707). As written, the
synthesized let copies the captured cell's value at literal position, contradicting
the same section's "binds the definition-site cell" (:597) and "captures the cell,
not a snapshot" (:612). The `CompilerBindings` entry `{cap$N → {cell, type}}` is
what preserves cell identity (the `externalReferences_`/`resolved` path at
compiler.cpp:571-576, :755-758).

**Fix:** say the synthesized `let` is expand-stage scope bookkeeping (its only job
is blocking later shadows) and that `cap$N` resolves through `CompilerBindings`,
never through ordinary local emission — or specify that the let aliases the
binding cell.

### 8. MINOR — §5.4/§6.2: `run`'s `args: [Cell]` vs the `[3, flag]` examples, and where args land

The surface signature is `run(c: Code<T>, args: [Cell]): T` but `run(c, [3, flag])`
(§5.4) passes values; the checker-special marshalling — each argument's value cell
bound on the `#Call` frame's `d.vars` by parameter name, matching the §5.8
discipline — needs one sentence. Related: state that args are *frame-bound* (hence
per-activation and recursion-safe), not written into the descriptor's static
`+d.grab` parameter cells (:738), which no spill covers — the descriptor cells are
signature data for `validate`.

### 9. MINOR — §6.4: `choiceDepth() == 0` is the wrong release guard

"only when `choiceDepth() == 0` may a `release(m0)` run" (:915) misses non-`Choice`
marks: a host `arena.mark()` (or a native that forgets to release) leaves
`outstandingMarks_ > 0` at zero choice depth, and `release(m0)` over them corrupts
identically — `release` pops one floor while marks are assumed well-nested
(arena_manifold.cpp:1695-1698; arena_manifold.hpp:773).

**Fix:** the guard is "`m0` is the innermost outstanding mark" — under the
cp-only-marking rule, `outstandingMarks() == 1`. `checkpointView` already checks
the right predicate (mesh_actor.cpp:672-675).

### 10. MINOR — §6.5/§10.2 (P15): "every VM write" is loose, and the predicate must test `masterOf(cell)`

Three VM-internal writes bypass `writeValue` by construction and all land on
structurally non-registered cells: `setCursorOpcode`'s `setValueBits`
(vortex_vm.cpp:139-143, cursor cells are never registered), `Call`'s frame write
(:860-863, fresh cell), `mintOpcode`'s own `setContent` (:73-81, registration
itself). The doc already exempts fresh cells; name these three as
exempt-by-construction so the funnel claim is literally true. Separately,
`writeValue` writes the *clone master* (`masterOf` at vortex_core.cpp:202); a clone
of a registered op cell (reachable through the same `Clone`-op surgery) would slip
past a raw `opcodeMap_.contains(cell)` test and write the master — the installed
predicate must test `masterOf(cell)`, one clause.

### 11. MINOR — §7.2: "the emitted arity table" for per-op wing arity has no named source

Per-`OpcodeKind` expected wing counts exist nowhere in the tree — each op body
checks `inputs.size()` ad hoc (e.g., vortex_vm.cpp:898 for `Bind`); the fragment
arity table the checker emits is the *parameter* arity for descriptors. Name the
per-kind table's home: an adapter-bound cell like the ceiling/impure-kind set, or
constants inside `validate`.

### 12. MINOR — §6.1: `reify` must retarget the `+d.refs` marks onto the copy

If the recipe's link tuples replay the `+d.refs` rank records (they should, for the
copy to be durable), their `+d.values→refcell` endpoints must pass through the same
old→new map — otherwise the copy's marks point at the *original's* ref cells
(double-rewriting them at checkpoint while the copy's own go unmarked and promote
stale). One sentence: the `+d.refs` rank is replayed like every other rank and its
endpoints remapped.

### 13. MINOR — §6.1/§7.2: `test_of`/`body_of` wrappers sit on `Halt`-tailed program graphs

Program-side clause graphs terminate at `Halt` (compiler.cpp:861-863, :870) —
correct for cursor-entry execution but refused by `validate`'s run-reachability
rule, so `run` on a `reflect`-wrapped program test either fails validation (the
honest answer — such a wrapper genuinely is not `run`nable) or requires the call
ABI to emit `Return` tails for clause graphs. §6.1's "`run` on them yields a
`Cell`" (:815-818) should say which.

### 14. MINOR — §10.2 (P16) / §6.6: "`d.code`/`d.patch`-reachable heads" is imprecise

Patch records hold head *names* in record cells, never links to heads (§6.6 :968-971
— correctly, per the eviction argument), so only `d.code` registration makes a head
checkpoint-reachable; `d.patch` reaches nothing headward. Harmless, but the pass
spec should read "each `d.code`-registered head's `+d.refs` rank".

### 15. MINOR — §6.1/§7.2: `validate` is `pure` yet writes mark links

`validate`'s `d.validate` marks are ordinary trailed arena writes inside a
`pure`-declared builtin — consistent with positional purity (restored on backtrack,
stale marks evicted by the next pass's fresh marker), but a house reader will ask;
one clause noting the marks are trailed, non-semantic arena writes closes it.

VERDICT: NOT CONVERGED
