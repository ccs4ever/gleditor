# VQL shape dispatch: the run-time half of V5 (shapes as cells)

Status: proposal, nothing built, no core or stdlib change made. Branch `shapedispatch`, based on the
`q-shapepred` tip `f713bd50` (marked shape handles, shapes refused as values, shapes followed
through definitions). Author's note on method: every claim about code cites `path:line` at that tip;
no measurement is quoted because none was taken (§2.5 says which to take).

Sources read: `.logs/w1/D4b-spike.md` (the V5 card and the representation it assumes),
`design/projects/vm-restoration-coordinator-plan.md` (D4, gates K to P),
`design/projects/vql-412-completion-gates.md` (C4), `design/vql-implementation-status.md` (§4.8,
§4.12), and the code under `apps/common/xanadu/vql/` and `apps/common/xanadu/vortex/`.

## 0. Where this sits

D4(b) moved views into arena cells with run-time dispatch (gates N to P). The spike's own sentence
for shapes is the starting point: the rules as cells are "cheap", the dispatch is "not"
(`D4b-spike.md` l.~70). Today a shape is syntax: `registerShape` mints a one-cell marked handle and
stores the expanded `ShapeLiteral` in the compile-time table `shapeHandles_`
(`compile_shape_value.cpp:93-105`); the file header says the VM never reads the handle
(`compile_shape_value.cpp:1-8`). Gate K ruled that table "a deliberate exception ... to be moved to
cells later", and gate M and O repeat that each move of shapes or lenses needs its own approval
(`vm-restoration-coordinator-plan.md` l.~510, ~565). This document is that approval request for the
dispatch half only. It does not ask for lenses (V6), which §4 sequences after it.

## 1. What run-time dispatch must enable, and what it lifts

A shape handle that exists only at compile time forces every shape to be reducible to a literal rule
list before the graph is emitted. Three things cannot be, and each has a named refusal today.

| capability                                                                                                                        | refusal it lifts                                                                                                                                                                                                   | cite                                                                                                                                                              |
| --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| shape source known only at run time (a shape in a `let` of a computed stream, from a host binding, or from another store's cells) | "shape value '$x' is not statically known: it is bound to cells the compiler did not record as shapes" and "... a shape handle the compiler has no record of (the session was cleared, its cell was released ...)" | `compile_shape_value.cpp:146-151`, `:165-169`                                                                                                                     |
| computed shape targets, `{+d.tag: $n + 1}`                                                                                        | "compiled shape predicates support only scalar literal or nested literal targets"; the C4 row lists "computed shape targets" as open                                                                               | `compile_shape_predicate.cpp:80-84`; `vql-412-completion-gates.md` C4 row                                                                                         |
| a shape used as a value: operand, argument, collection member, result element                                                     | "a shape value cannot be used as an operand of '...': it is syntax resolved while the query is compiled, not a value the graph computes with" (the q-shapepred refusal), and the `refuseShapeCells` twin           | `compiler.cpp:1253-1266`, `:1221-1235`; called at `compile_call.cpp:209`, `compile_steps.cpp:175`, `compiler.cpp:795`, `:1535`, `compile_shape_predicate.cpp:135` |
| a shape rule that is a pivot or a virtual dimension inside a predicate                                                            | "compiled shape predicates do not support pivot or virtual rules"                                                                                                                                                  | `compile_shape_predicate.cpp:56-59`                                                                                                                               |
| `project` / `apply_shape` with a shape that is not a literal after expansion                                                      | "project requires a shape literal, a shape variable or a call of a function whose body is a shape literal"                                                                                                         | `compile_semantic_shape.cpp:108-111`                                                                                                                              |
| shape-returning functions whose parameters are typed, defaulted, variadic or whose body is computed                               | "shape function '...' has a typed, defaulted, variadic or destructured parameter ..."                                                                                                                              | `compile_shape_value.cpp:241-249`                                                                                                                                 |

Not all rows are in scope. §3 decides which this design lifts and which stay refused. The first
three are the capability; the last three are listed so a reader does not mistake them for
consequences of dispatch (pivot and virtual rules in `project`/`apply_shape` are the S1 semantic
builtins' own scope, `compile_semantic_shape.cpp:127-131`; the function-parameter refusal is about
checking contracts while resolving, and a run-time shape does not change it).

The static path is not the problem and must not regress: validation bounds (depth 128, expanded size
4096, `compile_shape_predicate.cpp:22-98`), the folded mask (`:343-364`) and `ProjectTopology`'s
folded edge pairs (`compile_semantic_shape.cpp:77-90`) are exact, sealable (except `project`) and
need no run-time reading of rules.

## 2. What the data half must provide, and the options over it

### 2.1 Assumed landed (the contract this design depends on)

D1. **Rules as cells on `d.vql.rule`.** A shape cell heads a rank of rule cells in source order, the
same representation as a view (`view_cells.cpp:12-27`, `viewRuleDimension` at `view_cells.hpp:38`),
with the spike's kinds extended: `dim` (a dimension rule), `wildcard` (everything or nothing),
`nested` (a nested shape: one `d.vql.nested` edge to a shape cell), and `pivot`/`virtual` only if §3
admits them. Dimension name, direction and the `!`/`~` flags are cells or edge presence exactly as
`d.vql.to`, `d.vql.neg`, `d.vql.bidir`, `d.vql.dir` already are for views (`D4b-spike.md` l.40-47).
Source order is part of the data: both the mask fold (`compile_shape_predicate.cpp:343-364`) and the
project fold (`compile_semantic_shape.cpp:49-70`) are order-dependent.

D2. **Targets are values, resolved at mint time.** A rule's target is one of: absent, a scalar cell
(`d.vql.target`, compared with `VqlCompare` Equal, `compile_shape_predicate.cpp:407-415`), or a
shape cell. A computed target (`{+d.tag: $n + 1}`) is *evaluated where the shape literal is
evaluated*, in the scope that has `$n`, and the resulting cell is stored as the target. This is
capture by value, which is why the dispatch half needs no closures: the lens problem (a descriptor
that dies with the program, `D4b-spike.md` l.~80-90) does not arise for shapes. The compiler change
is VQL-only and belongs to the data half.

D3. **A shape handle is a shape cell with a marker and an epoch witness**, as a view handle is: the
marker dimension already exists (`shapeMarkerDimension = "d.vql.shape"`, `view_cells.hpp:37`;
`carriesShapeMarker`, `view_cells.cpp:60-69`), and the witness rank `d.vql.epoch`
(`view_cells.cpp:22`; written by the wrap step, gate P). A handle copied by value without its
witness is unchecked today for views (gate O "open"); the shape half must not widen that hole, so
every run-time consumer verifies the witness (§2.3 states how).

D4. **Static lowering unchanged.** `compileShapePredicateImpl`, `compileProject`,
`compileSemanticShapeCall` keep taking a `ShapeLiteral`. The compile-time table survives for what
the compiler *can* resolve statically (a literal, a variable bound to one), so a known shape is
still expanded and lowered with no run-time reading. D1 fills the cells *in addition* when a handle
must outlive the compile (session `let`, result, run-time source), mirroring how `registerShape`
already creates a handle cell for every shape (`compile_shape_value.cpp:93`).

D5. **Rule-count and depth ceilings are data-visible**: the `maxCompiledShapeProjectionRules` (4096,
`compiler.hpp:49`) and nesting depth 128 constants are the compiler's today. A run-time reader needs
them as settings (governance rule 2: no naked magic numbers in algorithms), so D5 asks that they
move to a `system://settings` entry (or the VQL engine's existing limits struct) and are read by the
dispatch half. Without D5 the run-time reader would hard-code a second copy.

If any of D1 to D5 does not land first, §3 does not apply.

### 2.2 What a run-time interpreter must compute

Two consumers, with different needs.

*Predicate* (`candidate ? {rules}`): for each rule in source order, a presence probe or a
target-matching probe in one or both signs, a recursive call for a nested target, and, for the
wildcard-led forms, a *mask probe* ("would this mask remove a link of the candidate?",
`VqlMaskProbe`, `compile_shape_predicate.cpp:368-385`, `:512-524`). The folded mask is a map from
dimension to two bits, rebuilt by a left-to-right walk in which a wildcard clears it (`:343-364`).

*Project* (`project(stream, shape)`): fold the rules to an edge selection (everything flag, added
edges, removed edges) and hand it to `copyTopology` (`vortex_streams.cpp:710-775`).

What the existing opcodes can and cannot take at run time, read from the code:

- `ProbeLink` takes the dimension and the sign as ordinary input cells, so a run-time dimension is
  expressible (`compile_shape_predicate.cpp:372-377` passes them as constant cells, which the VM
  reads like any other operand). It is exported with the dimension operand relocated
  (`program_export.cpp:218`) and is mesh-pure (`mesh_evaluator.cpp:204`).
- `VqlMaskProbe` and `ProjectTopology` read their edge operands from cells too
  (`vortex_streams.cpp:726-740` uses `inputValue`), but **the number of edge pairs is the opcode's
  operand count**, fixed when the instruction is emitted (`pairs = inputs.size() - headerInputs`,
  `:728`). A shape of unknown rule count cannot instantiate them. Padding to a maximum is refused
  below.
- No existing opcode enumerates a cell's links. The compiler says so itself ("which enumerates links
  that a fixed list of ProbeLink dimensions cannot", `compile_shape_predicate.cpp:339-341`). `{}`,
  `{*}` and `{!*, ...}` need exactly that.
- A routine called through `emitCallRoutine` that "never calls another step" can use fixed slots
  (`compile_view_steps.cpp:90-100`); a nested shape *does* call itself, so it needs real frames. The
  frame opcodes are open for whole-table copies (`GroupBegin`, `GroupNext`, `RestoreBindings`,
  `vm-restoration-coordinator-plan.md` l.~503) and long recursion is where that cost is quadratic.

### 2.3 Options

All four are priced on the same axes. "Reuse" is existing stdlib or compiler machinery. "C++" is
what must be written outside Vortex and why. "Opcode" covers append-only ordinals, the ceiling
scalar, export, mesh purity and vagent.

**(a) A `std:shape` Vortex package over existing opcodes only.**

- Shape of it: `VortexStdLib::bootstrap` weaves modules into the manifold along `+d.stdlib`
  (`vortex_stdlib.cpp:100-135`); a module is a set of entry ops built by a C++ `build*Module`
  function that mints the instruction graph (`vortex_stdlib.cpp:495-508`, `buildGoalModule` at
  `:129`). So "a Vortex package" still means C++ that assembles a graph; the only real difference
  from (b) is *where* the graph lives (once per `VortexCore` instead of once per compiled program).
- Reuse: `ProbeLink`, `VqlCompare`, `BranchFalseCell`, `Or`/`Not`, `RankBegin`/iteration over the
  rule rank (the same mechanism view dispatch uses, `D4b-spike.md` l.~100), `InvokeFrame` for
  recursion, `CellOfInt`/`IntOfCell`/`EpochOfCell` for the witness check.
- C++: the graph assembler. Cannot be avoided; there is no Vortex source form for stdlib bodies
  today (the `.vagent` files under `tests/samples/vagent/` are samples, and vagent refuses the
  cell-number opcodes by ordinal, `vagent/meta.cpp:152-157`).
- Capability: predicates over **non-exact** shapes (no wildcard, so `exact` is false and no mask
  probe is needed: `compile_shape_predicate.cpp:343-348`). It cannot do `{}`, `{*}`, `{!*...}` (no
  link enumeration), and it cannot do `project` (fixed pair count; no edge-list opcode). Those would
  refuse by name at run time when a wildcard rule cell is read.
- Opcode: none new. The routine uses `CellOfInt` and `EpochOfCell`, so any program that calls it is
  mesh-impure and refused at seal and export for a constant operand (`program_export.cpp:233-246`,
  `mesh_evaluator.cpp:270-288`), as run-time views already are. vagent refuses the cell-number
  opcodes by ordinal; the module is not vagent-callable (unchanged).
- Performance: view dispatch is the prior art, about 57 ops per step site pinned at 60
  (`vm-restoration-coordinator-plan.md` l.~565). A predicate walks a rank with a probe and a branch
  or two per rule, plus a frame per nested level. No figure is claimed; §5.1 is the spike that
  produces one.
- Sealed artifacts: unaffected. A sealed artifact that contains only static shapes lowers exactly as
  today and keeps refusing `ProjectTopology` by name at seal and at decode
  (`mesh_evaluator.cpp:383-386`; test pinned in gate K text). A program that calls `std:shape` is
  new and is refused for the cell-number opcodes it contains.

**(b) A Vortex graph emitted by the compiler once per program.**

- This is the view precedent: `viewRoutines` emits fold, wrap and resolve once per program as
  subroutines and each step site writes fixed slots and calls them (`compile_view_steps.cpp:90-100`,
  `:380`, `:463`). Reuse is total (`emitOp`, `emitCallRoutine`).
- C++: the emitter lives in the compiler (VQL-only: no core approval, as V1 to V3 were). It is the
  same instruction set as (a), so (a)'s capability limits apply identically: no `{*}`-family
  predicate, no `project`.
- Price against (a): the graph is copied into every program that needs it (program size, compile
  cost), and it is mesh-visible as program instructions rather than a module call. Advantage: it
  needs no stdlib change and the owner has already approved this pattern for views.
- Recursion: fixed slots cannot serve a self-calling routine, so nested targets would go through
  frame ops, with the open copy cost above; or nested depth would be a compile-time fixed unrolling,
  which brings back a depth ceiling that users hit. A ceiling users work around is refused by
  standing preference (`feedback_no_ceilings`).

**(c) New opcodes that read the rule cells.**

- Two opcodes appended after `EpochOfCell` (`vortex_vm.hpp` enum tail; the enum is append-only
  because exported programs and mesh artifacts store opcode integers, `vortex_vm.hpp:~217` comment):
  `ShapeHolds(candidate, shape) -> Bool` and `ShapeProject(stream, shape) -> stream`.
- Why not Vortex: link enumeration for the wildcard family and the detached-copy mint
  (`copyTopology`) are engine primitives already in C++ (`VqlMaskProbe`, `ProjectTopology`); the
  variable rule count defeats the existing fixed-arity forms (§2.2); recursion into nested targets
  is a C++ call, not a frame, which sidesteps the open frame-copy cost. The new code reuses
  `planMaskRemovals` (the planner `VqlMaskLinks` and `VqlMaskProbe` already share,
  `vortex_vm.cpp:1480`) and `copyTopology`, so it adds a *reader of rule cells*, not a new mask
  algorithm.
- Opcode edits, from the precedents: enum + `executeShapeHolds`/`executeShapeProject` dispatch
  (`vortex_vm.cpp:~2912`); the parameter-pipeline and condition refusal lists
  (`vortex_vm.cpp:2018-2032`, `:2180-2190`); `program_export.cpp` handling and `mesh_evaluator.cpp`
  purity (`:255-288`: both read the evaluating arena, so impure, like `VqlMaskProbe`); the vagent
  ceiling scalar, which `meta.cpp:152` ties to "the enum's last ordinal", so it moves; and the
  decision whether to add arity rows, which gate N made a separate approval
  (`vm-restoration-coordinator-plan.md` l.~543, VAGENT-ARITY; the CELLINT precedent left them
  refused by ordinal).
- Mesh: both are mesh-impure, refused at seal and export. `ShapeProject` joins `ProjectTopology` in
  the "fresh cells every run" class (`mesh_evaluator.cpp:259-262`). Both name their shape operand by
  cell, so a constant operand is refused as `CellOfInt` is (a cell number is not relocatable).
- Performance: one dispatch per test or projection; rule walking happens in C++, so the per-rule
  graph cost of (a) and (b) is absent. Cost moved to correctness: the fold now exists twice (the
  compiler's static fold and the VM's reader), so they can drift. Mitigation is a differential test
  (§4), not trust.
- Sealed artifacts: unaffected for static shapes; new programs are refused as above.

**(d) Hybrid: static lowering when the shape is known, dispatch otherwise.**

- Not a fourth mechanism but the *selection rule*: at each consumer site the compiler calls
  `resolveShapeValue` (`compile_shape_value.cpp:300-313`); when it returns a literal, lower as today
  (exact, sealable); when it would refuse "not statically known", emit the run-time form instead.
  Which run-time form is the choice among (a), (b), (c) and (d) is orthogonal to them.
- Price: two code paths per consumer, with the static one as the oracle for the dynamic one. That is
  a feature for testing and a cost for maintenance; views already pay it (the D4b plan kept
  `ComputedViewRules` beside the cell-based path, gate O).
- It preserves the sealed-artifact rule exactly: a program whose shapes are all static is
  byte-identical to today's output, so existing sealed artifacts, memo keys and the
  `ProjectTopology`-by-name refusal are untouched.

### 2.4 Refused alternatives

1. **Pad `VqlMaskProbe` / `ProjectTopology` to a maximum arity** with unused pairs. Needs a "no
   edge" operand value, which is a sentinel outside the wire format (`AGENTS.md` code style;
   `feedback_sentinel_boundary`), and 4096 padded pairs per site. Refused.
1. **Have the compiler re-lower on every run** (recompile the query once the shape's cells are
   known). Closed by the program-release design: the compiled program is released after a run
   (`vm-restoration-coordinator-plan.md` O2) and a second compile defeats memoisation and sealed
   artifacts alike. Refused.
1. **Make `ShapeLiteral` data the VM reads** by serialising the AST into a cell. The spike already
   refuses this for lenses ("syntax, never data the VM reads", `ast.hpp:553-560`) and it hides
   structure in a blob, against the cells-and-dimensions rule. Refused.
1. **Evaluate computed targets lazily at the test site.** Needs the expression's code at run time,
   which is the closure-lifetime problem that sends lenses to V6. D2's capture-by-value avoids it.
   Refused for shapes.
1. **Change `ProjectTopology`'s operand layout** to accept a rank head. Exported programs and
   `AGENTS.md`'s append-only rule make an existing opcode's operand meaning load-bearing
   (`vagent/meta.cpp:175-180` pairs stride). A new opcode costs one ordinal; a changed one breaks
   stored programs. Refused.

### 2.5 Measurements to take before the choice is final

Mirroring V0 (`D4b-spike.md` card V0: report op count and fuel per step), and the M3 lookup
experiments (`ed09f5b7`, `384bc24f`): one probe, run headless under `make`, over a shared shape
corpus (flat presence rules; target rules; nested depth 1, 8, 64; wildcard-led; 4096 rules), for the
same candidates:

- static lowering (baseline);
- the (a)/(b) graph interpreter (only if S1 below builds it);
- the (c) opcode.

Report ops executed and fuel per candidate test, wall time per call, program size in cells, and the
frame-copy count for nested depth (the open counted tables). No number appears in this document
until that probe has run.

## 3. Recommendation

**Adopt (d) with (c) as the run-time half, and spike (a) first for the cases it can cover.** The
reasoning is by exclusion, from §2.2:

- The wildcard family needs link enumeration and `project` needs a variable edge count. Neither is
  expressible over existing opcodes (§2.3 a, b). A recommendation that left them refused would lift
  only the easy half of "run-time-source shapes" and keep the q-shapepred refusal for `project`.
- (c) adds a *reader of rule cells* over machinery that already exists in C++, rather than a new
  algorithm, and answers nested targets without the frame copy cost.
- (d) keeps every statically known shape on today's sealable, exact path, so gates K to P are
  untouched.

The spike (S1, no approval, read/test only) builds the (a) interpreter for non-exact predicates and
measures it, as V0 did. If its cost per rule is acceptable and the owner prefers fewer opcodes, the
fallback is **(d)+(b)** for non-exact predicates and a named refusal for the wildcard family and for
`project` of a run-time shape; this is a smaller feature, stated honestly, not a silent subset. If
S1 is not run, the recommendation stands on the capability argument alone.

### 3.1 What this lifts and what stays refused

Lifted by (d)+(c): run-time-source shapes (§1 row 1), computed targets (row 2, via D2), shapes as
operands/arguments/collection members where a *handle cell* is what the graph holds (row 3, with the
witness check), `project`/`apply_shape` of a run-time shape (row 5, `project` only; see Q3). Stay
refused: pivot and virtual rules inside predicates (row 4: the static path refuses them and so does
the reader, by name); typed or defaulted shape-function parameters (row 6); a shape used
*arithmetically* ("an operand of `+`") remains a type error even when it is a cell, since a shape
has no value-level operators. The q-shapepred message is therefore narrowed, not deleted: it says "a
shape is a value", but the operator still refuses by name.

### 3.2 Numbered approvals

Each is a separate owner decision. None is taken by this document.

**P1. Opcode `ShapeHolds(candidate, shape) -> Bool`, appended after `EpochOfCell`. Core.**
Justification: the in-graph interpreter cannot enumerate links (§2.2), and the existing
`VqlMaskProbe` has a compile-time pair count. The op reads the rule rank on `d.vql.rule`, folds it
with the same left-to-right rules as `compile_shape_predicate.cpp:343-364`, probes each rule's
signed link, matches scalar targets with the same comparison as `VqlCompare` Equal, recurses for a
nested target with the depth and size ceilings from D5, and answers the exact/`{*}` forms through
`planMaskRemovals`. Pure read; one unit of work per rule is charged (see below). Fails by name on a
cell that carries no shape marker, a handle whose witness is stale, or a depth over the ceiling.
Costs: enum ordinal, dispatch, parameter-pipeline and condition refusal lists, `program_export`
handling (shape operand named by cell, a constant operand refused), `mesh_evaluator` classification
(impure), `vagent` ceiling scalar. Estimate: M. Work accounting: it must charge fuel per rule
walked, or the op is an unmetered loop; the V12 precedent charges per step and CellOfInt "one unit
of work" (`vortex_vm.hpp` CellOfInt comment), so this is the same discipline at a larger unit.

**P2. Opcode `ShapeProject(stream, shape) -> stream`, appended after P1. Core.** Justification:
`project` of a shape with unknown rule count needs a variable edge selection; `ProjectTopology`
cannot take one (`vortex_streams.cpp:728`) and must not change (§2.4 item 5). The op folds the rules
to an `EdgeSelection` (the fold in `compile_semantic_shape.cpp:49-70`) and calls the same
`copyTopology`. Same family as `ProjectTopology`: impure, mints cells each run, refused at seal by
name like it. Costs as P1 plus the `ProjectTopology`-by-name refusal wording extended to
`ShapeProject` at `mesh_evaluator.cpp:383-386`. Estimate: M. Optional: drop P2 and keep `project` of
a run-time shape refused (Q3).

**P3. Settings entry for the shape ceilings (D5). Config, VQL/system settings.** Moves the 4096 and
128 constants to `defaultSettingSpecs()` (`AGENTS.md`: "Adding a setting means adding a
`defaultSettingSpecs()` entry, which is easy to skip") so the compiler and P1/P2 read one value.
Estimate: S. Prerequisite of P1.

**P4. vagent arity rows for P1/P2.** Separate pass, as VAGENT-ARITY was for `ProjectTopology` and
`VqlMaskProbe`; without it vagent refuses both by ordinal, which is the CELLINT precedent and costs
nothing. Recommend: do not add rows now; vagent has no shape values to pass. Estimate: S if wanted.

**P5. `std:shape` stdlib module.** Not requested. Listed because the card says "`std:` package": (a)
would be that module, and S1 builds it only as a spike under a test namespace, not in `bootstrap()`.
If the owner later wants shapes callable from Vortex/vagent programs, that is its own stdlib
approval then.

Compiler-side changes (D2's mint-time target evaluation; the run-time branch at each consumer site;
narrowing `refuseShapeCells`) are VQL-only and, by the V1 to V3 precedent, need no core approval.

## 4. Test and acceptance plan, and sequencing

### 4.1 Tests (all headless, `xuzz_test`, per the headless rule)

1. **Differential, the core guard.** For a corpus of shapes S and candidate cells c, the static path
   (`compileShapePredicateImpl` over the literal) and the run-time path (the same shape minted into
   cells, run through P1) must agree on every pair. Same for `project`: static `ProjectTopology`
   against P2, compared by `verifyAgainstFullRebuild`-free structural equality of the detached copy.
   Corpus: flat, `!`, `~`, both signs, target scalar, nested depth 1/8/128, wildcard-led, empty
   `{}`, `{*}`, `{!*, +d.a}`, duplicate rules, rule orders that differ only by a wildcard.
1. **The refusals.** Each remaining refusal in §3.1 pinned by message; a stale witness, a handle
   from another engine, a cell with no marker, a cyclic nested rank, over-ceiling depth and rule
   count all fail by name, not with a wrong answer.
1. **Capability.** Computed target (`$n` captured at mint); shape in a collection then applied;
   shape from a run-time stream; session `let` shape reused in a second query after the first
   program was released (the `programRetained_`-empty check the view tests use); `clearVariables()`
   then reuse.
1. **Sealing.** A static-shape program seals and decodes byte-identically before and after; a
   program containing P1 refuses at seal and at decode naming `ShapeHolds`; `mesh::evaluate` of VQL
   source with a run-time shape runs, as it does for `project` (pinned by
   `ProjectTopologyOpcodeTest.MeshEvaluateOfVqlSourceRunsProject`).
1. **Opcode plumbing.** The three mirrored edits that gate N and O tests already pin for
   `CellOfInt`: ordinal stability (an old stored program still decodes), vagent refusal by ordinal,
   export refusal for a constant operand, the 43-opcode allow-list the vagent table keeps.
1. **UX validation.** Run the `ux-validation` skill's journeys over a shape written in a session,
   saved, resumed and applied; a shape is a first-class value only if the REPL shows it as one.

### 4.2 Acceptance

- Differential corpus agreement: 100 percent, any single disagreement blocks.
- `xuzz_test` shows no newly failing test against the gate baseline (11 Evidence/Subscription
  failures at gate P).
- The S1 and §2.5 probe reports are attached to the PR; no performance claim is made without them
  (governance rule 3).
- `make format-check` and `make lint` pass; `make tidy` is clean on touched lines.

### 4.3 Sequencing relative to lenses (V6)

1. Data half (D1 to D5) and P3.
1. S1 spike and the §2.5 probe. Decision point: (c) or the (a)/(b) fallback.
1. P1, then P2 if approved.
1. Lenses (V6) after this. Reason: V6's blocker is lifetime (a closure descriptor dies with the
   program, `D4b-spike.md` l.~90, `arena_copy_out.cpp:74-83`), not dispatch, and it needs its own
   core change (descriptor assembly outside the program area). Shapes need none of that (D2). Doing
   V6 first would not unblock this design; doing this first narrows the compile-time table to lenses
   alone and lets the `handleKindOf` site list (`compile_shape_value.cpp:122-141`) shrink to one
   remaining special case. The ast.hpp exception comment (`ast.hpp:563-570`) is narrowed again at
   that point.

## 5. Open questions for the owner

Q1. **Predicate scope.** Is a run-time shape that is only non-exact (probes, targets, nested; the
(a)/(b) subset) worth shipping alone, with `{}`/`{*}`/`{!*}` and `project` refused by name at run
time? It needs no opcode, but it lifts the capability only partly. My recommendation is no, because
a user who writes `?{*}` cannot tell why a run-time shape differs from a literal one.

Q2. **Fuel for P1.** A C++ rule walk is one VM step to the scheduler. Should P1 charge per rule
(accurate, but a rule list of 4096 costs 4096 units) or one unit (CellOfInt's discipline, but then a
hostile shape cell is an unmetered loop)? Recommendation: per rule, with the ceiling from P3.

Q3. **`project` of a run-time shape (P2).** Detached copies are already mesh-impure and refused at
seal. Is a second opcode for them worth it, or should `project` stay literal-only and run-time
shapes feed predicates and `apply_shape` only? `apply_shape` also needs a mutation-time reader,
which this document has not priced; it is the largest unscoped item.

Q4. **Witness on shapes.** D3 says every consumer checks the epoch witness. A shape handle that is
copied by value without its witnesses is the same unchecked hole gate O recorded for views. Do you
want the shape half to close it (witnesses in the cell as a required rank, reader refuses without)
rather than inherit it?

Q5. **Computed targets at mint time (D2).** Capture by value means `{+d.tag: $n}` freezes `$n` when
the shape value is made, not when it is applied. That is the usual reading of a first-class value,
but the spec's §4.8.2 wording on "a value constraint or a binding" is not settled by it. Confirm the
capture-by-value reading, or require the owner's spec ruling first.

Q6. **Where a shape cell lives.** Session cells survive in the session arena and refer to a
foreign-store shape only by an Int64 box (the view rule, `D4b-spike.md` l.~120). Should a shape be
allowed to span arenas by promotion to a store (`promote`), or stay session-local until a separate
card? Staying local keeps D3's witness sufficient.

Q7. **vagent.** Do you want P4 now, or the CELLINT precedent (refused by ordinal until a vagent
shape value exists)?
