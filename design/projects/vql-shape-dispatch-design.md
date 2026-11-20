# VQL shape dispatch: the run-time half of V5 (shapes as cells)

Status: proposal, nothing built, no core or stdlib change made; the owner ruled on P1 to P5 on
2026-11-20 (§6), so the opcode, setting, vagent and stdlib items below are approved to build, and
two spikes (S1, S2) and two witness cards (W1, W2) are ordered (§7). Q5 is analysed in §8 and awaits
the owner. Branch `shapedispatch`, based on the `q-shapepred` tip `f713bd50` (marked shape handles,
shapes refused as values, shapes followed through definitions). Author's note on method: every claim
about code cites `path:line` at that tip; no measurement is quoted because none was taken (§2.5 says
which to take).

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
owner has since approved P1 to P5 (§6); the paragraph below is kept as the argument those approvals
rest on, and §4.3 carries the order of work. The reasoning is by exclusion, from §2.2:

- The wildcard family needs link enumeration and `project` needs a variable edge count. Neither is
  expressible over existing opcodes (§2.3 a, b). A recommendation that left them refused would lift
  only the easy half of "run-time-source shapes" and keep the q-shapepred refusal for `project`.
- (c) adds a *reader of rule cells* over machinery that already exists in C++, rather than a new
  algorithm, and answers nested targets without the frame copy cost.
- (d) keeps every statically known shape on today's sealable, exact path, so gates K to P are
  untouched.

The spike (S1, no approval, read/test only) builds the (a) interpreter for non-exact predicates and
measures it, as V0 did. With P1 approved, S1 no longer decides whether the opcode exists; it decides
what `std:shape` (P5) contains besides thin wrappers, and whether the (a) graph is worth keeping as
the oracle the differential test (§4.1) compares P1 against. The smaller fallback, **(d)+(b)** with
a named refusal for the wildcard family, is no longer on the table for predicates: the owner chose
the full feature.

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

Each was a separate owner decision. All five were approved on 2026-11-20 (§6); the text below is the
request as it was put, with the approval noted on each item.

**P1. Opcode `ShapeHolds(candidate, shape) -> Bool`, appended after `EpochOfCell`. Core. Approved.**
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

**P2. Opcode `ShapeProject(stream, shape) -> stream`, appended after P1. Core. Approved; whether
anything consumes it is S2's decision (§7.1).** Justification: `project` of a shape with unknown
rule count needs a variable edge selection; `ProjectTopology` cannot take one
(`vortex_streams.cpp:728`) and must not change (§2.4 item 5). The op folds the rules to an
`EdgeSelection` (the fold in `compile_semantic_shape.cpp:49-70`) and calls the same `copyTopology`.
Same family as `ProjectTopology`: impure, mints cells each run, refused at seal by name like it.
Costs as P1 plus the `ProjectTopology`-by-name refusal wording extended to `ShapeProject` at
`mesh_evaluator.cpp:383-386`. Estimate: M. Dropping P2 and keeping `project` of a run-time shape
refused was the alternative (Q3); S2 now measures before that is decided.

**P3. Settings entry for the shape ceilings (D5). Config, VQL/system settings. Approved.** Moves the
4096 and 128 constants to `defaultSettingSpecs()` (`AGENTS.md`: "Adding a setting means adding a
`defaultSettingSpecs()` entry, which is easy to skip") so the compiler and P1/P2 read one value.
Estimate: S. Prerequisite of P1.

**P4. vagent arity rows for P1/P2. Approved (overrides the recommendation below).** Separate pass,
as VAGENT-ARITY was for `ProjectTopology` and `VqlMaskProbe`; without it vagent refuses both by
ordinal, which is the CELLINT precedent and costs nothing. I had recommended no rows now, because
vagent has no shape values to pass; the owner's ruling makes vagent a caller of `std:shape` (P5).
Estimate: S.

**P5. `std:shape` stdlib module. Approved.** I had listed it as not requested; the owner has asked
for it. Its place is after the opcodes (§4.3): it is the Vortex-callable face of P1/P2 plus whatever
S1 proves worth keeping, so it cannot precede the ordinals it wraps. Governance applies: it reuses
existing stdlib routines (the rank iteration view dispatch uses, `D4b-spike.md` l.~100, and the
witness check built from `CellOfInt`/`EpochOfCell`) rather than re-deriving them, and the card lists
which routines it reuses; any routine it adds must say why an existing one does not serve
(`.claude/rules/architectural_governance.md` §4). Built through `VortexStdLib::bootstrap`
(`vortex_stdlib.cpp:100-135`) like the other modules, and callable from vagent once P4's rows exist.

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

### 4.3 Sequencing, and relative to lenses (V6)

1. Data half (D1 to D5) and P3 (approved; P3 is a prerequisite of P1).
1. In parallel, none needing further approval: **S1** (the (a) interpreter under a test namespace),
   **S2** (§7.1, what run-time `project`/`apply_shape` costs), the §2.5 probe, and **W1** (§7.2,
   views' witness; independent of everything here).
1. **P1**, with **W2** (§7.3) as its own card landing in the same series, then the differential test
   of §4.1 against S1's graph.
1. **P2**, only if S2's decision rule (§7.1) says run-time `project`/`apply_shape` is worth
   supporting. P2 is approved, but building an opcode whose only consumer is undecided would invert
   the order the owner chose; if S2 says no, P2 stays approved-but-unbuilt and `project` of a
   run-time shape refuses by name.
1. **P4** (arity rows for P1, and for P2 if built), then **P5** (`std:shape`). `std:shape` sits
   after the opcodes because it wraps them, and after S1 because S1 decides whether any of the (a)
   graph survives into it; it does not sit before P1, since a module with no ordinal to call would
   be (a) rebuilt, the case S1 exists to price.
1. Lenses (V6) after this. Reason: V6's blocker is lifetime (a closure descriptor dies with the
   program, `D4b-spike.md` l.~90, `arena_copy_out.cpp:74-83`), not dispatch, and it needs its own
   core change (descriptor assembly outside the program area). Shapes need none of that (D2, and §8
   on why it matters). Doing V6 first would not unblock this design; doing this first narrows the
   compile-time table to lenses alone and lets the `handleKindOf` site list
   (`compile_shape_value.cpp:122-141`) shrink to one remaining special case. The ast.hpp exception
   comment (`ast.hpp:563-570`) is narrowed again at that point.

## 5. Open questions for the owner

Status after the 2026-11-20 rulings (§6): Q1 moot, Q3 ordered to a spike, Q4 answered yes and
carded, Q5 analysed in §8 and awaiting the owner, Q7 settled; Q2 and Q6 remain open. The original
wording follows where it still carries information.

Q1. **Predicate scope.** *Moot.* P1 is approved, so the wildcard family and run-time shapes in
predicates are in scope together; the question of shipping only the non-exact subset no longer
arises. (The case against the subset stands as the reason: a user who writes `?{*}` cannot tell why
a run-time shape differs from a literal one.)

Q2. **Fuel for P1.** *Open.* A C++ rule walk is one VM step to the scheduler. Should P1 charge per
rule (accurate, but a rule list of 4096 costs 4096 units) or one unit (CellOfInt's discipline, but
then a hostile shape cell is an unmetered loop)? Recommendation: per rule, with the ceiling from P3,
which the owner has approved and which makes the per-rule charge bounded. P3 narrows this question
but does not answer it, since the choice is about unit price, not the bound.

Q3. **`project` and `apply_shape` of a run-time shape.** *Undecided pending cost; spike S2 (§7.1)
ordered.* Detached copies are already mesh-impure and refused at seal. `apply_shape` also needs a
mutation-time reader, which this document has not priced; it is the largest unscoped item, and S2
exists to price it.

Q4. **Witness on shapes.** *Yes; carded as W1 (views) and W2 (shapes), §7.2 and §7.3.* D3 says every
consumer checks the epoch witness. A shape handle copied by value without its witnesses is the same
unchecked hole gate O recorded for views.

Q5. **Computed targets at mint time (D2).** *Analysed in §8; the decision is the owner's.*

Q6. **Where a shape cell lives.** *Open.* Session cells survive in the session arena and refer to a
foreign-store shape only by an Int64 box (the view rule, `D4b-spike.md` l.~120). Should a shape be
allowed to span arenas by promotion to a store (`promote`), or stay session-local until a separate
card? Staying local keeps D3's witness sufficient. Q5's recommendation (§8) assumes session-local.

Q7. **vagent.** *Settled by the approval of P4:* the arity rows are in scope, the CELLINT precedent
(refused by ordinal) is not followed.

## 6. Owner rulings, 2026-11-20

The owner ruled on the design as follows. Each ruling is recorded with what it changes in this
document and what it costs, since a ruling that adds work should say so.

| item | ruling                 | what it changes                                                                                                             |
| ---- | ---------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| P1   | approved               | `ShapeHolds` is built; (c) is the run-time predicate half, not a candidate. Q1 falls away.                                  |
| P2   | approved               | `ShapeProject` is approved as an opcode; whether it has a consumer rides on S2 (§4.3 item 4).                               |
| P3   | approved               | The 4096 and 128 ceilings become a `defaultSettingSpecs()` entry; prerequisite of P1.                                       |
| P4   | approved               | vagent arity rows for P1/P2: reverses my recommendation, adds an S pass and makes vagent a caller of shape ops. Settles Q7. |
| P5   | approved               | `std:shape` is in scope, after the opcodes (§4.3 item 5), reusing existing stdlib per governance.                           |
| Q1   | moot                   | Follows from P1.                                                                                                            |
| Q3   | undecided pending cost | Spike S2 (§7.1) ordered. Nothing in P2's implementation starts before its decision rule is applied.                         |
| Q4   | yes                    | Own cards W1 and W2 (§7.2, §7.3), not folded into the shape cards, so each is accepted or rejected on its own evidence.     |
| Q5   | analysis requested     | §8, with a recommendation and no decision.                                                                                  |
| Q2   | open                   | P3 bounds the charge but does not choose the unit price.                                                                    |
| Q6   | open                   | §8 assumes session-local shapes.                                                                                            |
| Q7   | settled by P4          | See above.                                                                                                                  |

Net price of the rulings: five approved items (three core-adjacent, one config, one stdlib), two
spikes, two witness cards. Nothing in them changes the static path (D4): a program whose shapes are
all static stays byte-identical, which is the acceptance gate the later cards inherit.

## 7. Spike and card definitions ordered by the rulings

### 7.1 Spike S2: what run-time `project` and `apply_shape` cost (read and test only)

*Question (Q3).* Is it worth letting a run-time shape feed `project` and `apply_shape`, given that
both are detached or arena-mutating operations today lowered only from a literal? Not "can it be
done": P2's fold plus `copyTopology` plainly can for `project`; the unpriced part is `apply_shape`'s
mutation-time reader, which has no opcode and no cell-reading path.

*What is measured.* Three paths over the same corpus and subjects:

1. **Baseline: static `ProjectTopology`** as `compile_semantic_shape.cpp:77-90` lowers it, edge
   pairs folded at compile time. For `apply_shape`, the existing literal path of the S1 semantic
   builtin (`compile_semantic_shape.cpp:127-131`).
1. **Prototype run-time `project`:** a test-only reader that walks the rule rank on `d.vql.rule`,
   folds to an `EdgeSelection` as `compile_semantic_shape.cpp:49-70` does, and calls `copyTopology`
   (`vortex_streams.cpp:710-775`). It is the body P2 would have, without the opcode, the ordinal or
   the export edits.
1. **Prototype run-time `apply_shape`:** the same fold, applied at the point where `apply_shape`
   validates assignments and then masks (`vql-query-language.md` §4.8.3). The spike must identify
   and name that mutation-time reader in code before measuring it; if no existing reader can take a
   rule cell without a new mutation opcode, that finding is the result for this path and the spike
   stops there rather than prototyping a core change.

*Per path, report:* ops executed and fuel per call (the V0 discipline, `D4b-spike.md` card V0); wall
time per call; cells minted and arena bytes per call for `project` (which mints each run) and links
changed for `apply_shape`; rollback behaviour under `mark()`/`release()` for `apply_shape` (an arena
edit must undo by truncation); the fold's share of the whole call, separated from the copy or
mutation cost the static path pays too. The comparison that matters is the rule-reading overhead
against the cost the operation already pays, not the absolute time.

*Workload.* The shared corpus of §2.5 (flat presence rules, targets, nested depth 1, 8, 64,
wildcard-led, 4096 rules), crossed with subject streams that vary in cardinality and cell valence,
since `copyTopology` cost scales with the reachable topology and the fold cost with the rule count
and these must be told apart. Projection-only masks (`project` accepts masks only, §4.8.3) for the
`project` path; the assignment-bearing subset for `apply_shape`. Headless, under `make`, per the
headless rule.

*Tool.* A probe beside `tools/vql-rank-benchmark.cpp` (or a mode of it) rather than a new harness
style; governance rule 3 requires the data to come from a probe tool, not a hand timing. The corpus
and subjects are generated by the probe, not fixtures, so no fixture regeneration is involved.

*Decision rule, fixed before the run so the result cannot pick its own threshold.*

1. **Correctness gate (not a trade):** each prototype's result must be structurally identical to the
   static path over the whole corpus, by the differential comparison of §4.1. One disagreement ends
   the evaluation for that path.
1. **Cost test:** the owner states, before the probe is read, the acceptable ratio of run-time
   rule-reading overhead to the operation's own cost. This document does not choose it, and no
   figure appears here. If the measured overhead is within it for `project`, the answer to Q3 for
   `project` is yes and P2 proceeds (§4.3 item 4); otherwise `project` of a run-time shape refuses
   by name and P2 stays unbuilt.
1. **`apply_shape` is decided separately,** because its reader may need a new mutation opcode. If a
   reader exists without one, the cost test above applies. If it needs one, that is a new core
   approval request with S2's numbers attached, not something S2 or the P-items authorise.
1. A hot path that allocates where the static path does not (governance rule 3) is a finding the
   owner weighs, not an automatic veto, since `project` mints cells by construction.

### 7.2 Card W1: close the copied-by-value witness hole for views

*Problem.* Gate O records it as open: "a handle copied by value without its witnesses is unchecked"
(`vm-restoration-coordinator-plan.md` l.~570); `D4b-spike.md` l.~156-159 gives the mechanism (an
integer naming a cell passes `contains()` after number reuse) and notes the same-arena
different-engine case works only because the marker is found. The gate-O reviewer argued no
reachable program needs an in-graph epoch read; the owner's ruling is that the hole is closed
anyway.

*Scope.* Views only: the consumers `viewOfStream` and `planViewStep` (`compile_view_steps.cpp`)
verify the handle's `d.vql.epoch` witness in-graph (`EpochOfCell`, already in the enum) and refuse
by name when it is absent or stale, so a handle copied without the witness rank refuses instead of
reading whatever sits at that number. VQL-only: no new opcode, no layout change.

*Price.* The view dispatch is pinned at about 57 ops per step site (pin 60,
`vm-restoration-coordinator-plan.md` l.~565); the check adds a read and a branch per site, so the
pin must be re-measured and re-pinned in the same commit, not assumed. Programs that read the
witness use `EpochOfCell`, so they are mesh-impure; views over run-time sources already are, so the
sealed-artifact surface does not change. Estimate: M (compiler emission plus the refusal tests and
the re-pin).

*Acceptance.* A handle minted, then copied by value (integer round-trip through `IntOfCell`/
`CellOfInt`, and a cell copy that omits the rank), refuses by name at the step; a released-and-
reused cell number refuses; a legitimately minted handle runs with no behaviour change; the op-count
pin reports the new figure.

### 7.3 Card W2: the same for shapes

*Scope.* The shape half of D3. The handle wrap step writes `d.vql.epoch` on every shape handle
(`view_cells.cpp:22`, as gate P did for views), and every run-time reader requires the rank:
`ShapeHolds` and `ShapeProject` refuse by name on a handle that lacks it, and a nested target's
shape cell is checked at each level so a witness-less inner shape cannot be smuggled under a valid
outer one.

*Why its own card.* P1's text already says it fails by name on a stale witness, so a reader that
checks one is part of P1. W2 is the part P1 does not cover: that the witness is *required* (absent
means refuse, not treat-as-unwitnessed), that the wrap step writes it for every handle including
those minted by compile-time `registerShape` (`compile_shape_value.cpp:93`), and the negative tests.
Keeping it separate lets W1 land first and lets W2 be accepted on tests that do not depend on P1's
fold being right.

*Price.* Estimate: S to M. Depends on P1's opcode existing for the opcode-side check; the wrap-step
half can land before it. Static shapes are unaffected (D4). Acceptance mirrors W1: copied-by-value
handle, reused number, absent rank and nested witness-less inner shape each refuse by name.

## 8. Q5: computed shape targets read as capture-by-value at mint

The owner asked for the implications of reading `{+d.tag: $n + 1}` as capture by value at the moment
the shape is minted, and for the source of that reading. The decision is the owner's; §8.6 gives a
recommendation.

### 8.1 The source, quoted, and what it supports

**The spec is silent on when a computed shape target is evaluated.** The reading is this document's
own assumption (D2, §2.1, written for the design; line 63 of this file), and the earlier wording of
Q5 said the spec "is not settled by it". Every source I was pointed to, quoted exactly:

- `design/vql-query-language.md:387`, the grammar, which admits an expression and says nothing about
  time: `DimensionShapeRule ::= "!"? ( "+" | "-" | "~" )? DimensionIdentifier ( ":" ValueExpr )?`.
- `design/vql-query-language.md:984-985`, §4.8.2, the only prose on a target: "If a value constraint
  or variable binding is specified (`+d.tag: "mp4"` or `+d.tag: $t`), the neighbor cell along that
  dimension matches the value or binds to `$t`." This is a *test* description. It shows `$t` as a
  binder (an output of the match), which is the opposite direction from a computed read, and gives
  no evaluation time for either.
- `design/vql-query-language.md:666`: "a missing shape target does not match." This constrains the
  empty-result case below but not the timing.
- `design/vql-query-language.md:2218` (§4.12 status): "Non-closed formal defaults, computed shape
  targets, ordered projections, historical pins and creation at that recursive boundary remain
  outside this subset". `design/projects/vql-412-completion-gates.md:70` (C4): "non-closed defaults,
  computed shape targets and historical/projection forms remain". Both say *open*, not *how*.
- `design/vql-implementation-status.md` §4.8 (l.~16-18): "arena-local first-class shape handles". It
  does not speak to targets. `design/vql-implementation-status.md` has no §4.12 sentence on computed
  shape targets beyond the C4 repeat.
- `.logs/w1/D4b-spike.md:48`: "Computed rules stay refused (section b, "computed")." That is a
  refusal, written about views' rules, and it too is silent on timing.
- `design/projects/vm-restoration-coordinator-plan.md`: no sentence on computed shape targets. It
  records only that shape values live in a compile-time table "a deliberate exception ... to be
  moved to cells later" (l.~508).

So: **the spec neither supports nor contradicts capture by value; it is silent.** The present
compiler is also silent in code: `compile_shape_predicate.cpp:80-84` refuses everything but a
scalar-literal or nested-literal target, so no run of the system has ever fixed a timing.

The nearest precedents, by analogy and not by text, lean towards by-value for first-class values:
"Decorated functor captures freeze their body across rebinding" (`vql-query-language.md:2286`) and
"Captures freeze ordered membership at creation ... and retain live cell identity"
(`vql-implementation-status.md:937`). Note the second sentence: even there, *membership* freezes
while cell identity stays live. That distinction matters in §8.2.

### 8.2 Implications of capture by value

- **Stale, not live, by construction.** After `let $s := {+d.tag: $n + 1}`, rebinding `$n` or
  editing a cell `$n` was read from leaves `$s` unchanged. That matches how the compiler already
  treats a `let` of a first-class value and the "freeze across rebinding" precedent, and it keeps a
  shape referentially transparent, which is the purity vlace's design leans on
  (`design/vlace-functional-language.md`). The price is that a user who expects `$s` to track `$n`
  is surprised; this should be stated in §4.8, not discovered.
- **Per evaluation of the literal, not per program.** A literal inside a function body or `for`
  re-mints each time it is evaluated, so "frozen" means frozen for that evaluation. A loop of N
  iterations mints N shapes (one scalar target cell per computed rule each, plus the rule cells).
- **The value is copied, not the cell.** D2 says "the resulting cell is stored as the target". If
  `$n` is an existing cell (`$x/value`), storing that cell makes the target a live reference to
  something that can change or be released: capture by identity, which is by reference. The reading
  is only by-value if mint copies the scalar into a fresh cell (`value()` "snapshots its operands",
  `vql-implementation-status.md` §4.2/§7.4, is the precedent). Typed scalars come from canonical
  text by design, so the copy keeps comparing as `VqlCompare` Equal does ("`2.0 = 2` is true").
- **Cardinality and absence.** A computed target is a stream. Capture needs exactly one value.
  Several is a refusal naming `one`, consistent with the scalar-cardinality guidance beside
  `vql-query-language.md:660`. Zero is the case line 666 decides: the rule never matches. D2's
  "absent target" (presence-only rule) must therefore be distinct from "computed, empty, never
  matches": an edge-presence flag (cells and dimensions, not a reserved value), or a rule of a
  distinct kind. Conflating them silently turns "no tag with this value" into "any tag".
- **Binders are not computed targets.** `+d.tag: $t` with `$t` unbound is a binder (spec l.985). A
  binder cannot be evaluated at mint; it needs the match, which is run time per candidate. Capture
  by value is the reading for a *read* of a bound expression only. Which of the two a `$name` is
  must be decided by scope, as today's `{+d.tag: $t}` handling will need to when C4 lifts the
  refusal. This is a separate ruling the owner should expect to make.
- **Hypertime, R8, stores.** The target cell is minted in the session arena, which mints no
  operation (R8: arena cells carry bits only, `store-slice-convergence.md`), so a shape mint mutates
  no visited store. A version-pinned read in the target (`x@(v1)`) is evaluated at the pin and
  frozen. `apply_shape` mutates the arena neighbourhood only; the assignment value is fixed at mint,
  so applying twice assigns the same value (idempotent), where a live target would not.
- **Session persistence and epoch witnesses.** The copied target cell is a child of the shape's rule
  rank, so it shares the handle's lifetime and is covered by the one witness (D3, W2). Capture by
  reference would need a witness *per referenced cell per rule*. `clearVariables()` and arena
  release drop shape and targets together. A shape that crosses arenas by `promote` (Q6) must map
  its target cells too; staying session-local is simpler and is assumed here.
- **Closures and lenses (V6).** By-value avoids the closure-lifetime problem: a closure descriptor
  dies with the program (`D4b-spike.md` l.~90, `arena_copy_out.cpp:74-83`), and a lens callback with
  captures is refused. A by-value shape needs no descriptor, so it does not block on V6 and V6 does
  not block it. This is the main reason the design leans this way.
- **Mesh purity and sealed artifacts.** A literal-target shape lowers as today and seals
  byte-identically (D4). A computed target adds ordinary graph ops that evaluate the expression and
  one mint; that mint makes the program impure like any cell-minting op, but the evaluation itself
  introduces no opcode. The static lowering of a computed target in an immediately used predicate
  (`?{+d.tag: $n + 1}`) can compile as `ProbeLink` plus `VqlCompare` against the computed operand
  with no shape cell, which is sealable. For that static form to agree with the run-time form, a
  shape held in a `let` must evaluate its target at the `let` into a hidden binding and read that
  later, not re-run the expression at each use after `$n` was rebound. The differential test (§4.1)
  must cover this pair.
- **Cost.** One expression evaluation and one scalar-cell mint per computed rule per evaluation of
  the literal; zero extra reads at use, since the target is a plain rule cell `ShapeHolds` already
  compares. No figure is claimed. The §2.5 probe should add a computed-target row so the by-value
  and by-reference costs below are measured, not argued.

### 8.3 The alternative: capture by reference, or re-evaluate at use

Each computed rule would hold an expression, and the target would be evaluated each time the shape
is applied or tested. What it requires:

- **A code value per rule**, a closure with its captured environment. That is precisely V6's blocker
  (descriptor dies with the program; assembly outside the program area is a core change). Shapes
  would then wait on V6, reversing §4.3's order.
- **A way for the reader to run code.** `ShapeHolds` is a C++ opcode; it would need to re-enter the
  VM for each computed rule, or the predicate would move back to the graph interpreter (a)/(b) with
  `ApplyClosure`, giving up the wildcard family (§2.3 a) for any shape with a computed target.
- **Live environment lookups** with witnesses on every captured binding, since a program's registers
  die when the program is released (`programRetained_`, O2).
- **Evaluation count and ordering:** O(candidates x expression cost) unless hoisted, and a result
  that depends on when the test runs, so memoisation keys and sealed artifacts cannot be sound, and
  differential testing against the static path needs the evaluation moment pinned anyway.
- It *would* give the "live" reading some users expect, and make a shape a true thunk. A first-class
  live value is better expressed as an explicit closure the user writes (`$f`) and applies, which
  spec §4.12's closure work already provides, than as hidden laziness inside a shape literal.

### 8.4 What each choice needs recorded

By value needs a sentence in §4.8.1/§4.8.2 (the timing, the one-value rule, empty never matches,
binders excluded), the fresh-copy rule, and the distinct empty-target representation. By reference
needs V6 first. Neither is in the spec today, so either needs the owner's spec ruling; this document
does not edit the spec.

### 8.5 Open points this analysis does not close

The binder-versus-read rule for `$name` in a target (§8.2), the hidden-binding rule for a `let`-held
static shape, and Q6 (cross-arena shapes). Each is a ruling or a small design, not a measurement.

### 8.6 Recommendation

Adopt capture by value at mint, with four conditions: the target is a fresh scalar copy (never the
source cell), exactly one value (several refuses, naming `one`), an empty result is a distinct
"never matches" rule (spec l.666), and unbound `$name` stays a binder outside this ruling. Write the
timing into spec §4.8.2 so it stops being an assumption of this design. Reasons, in order: the spec
is silent so nothing contradicts it; it keeps shapes off V6's critical path; it keeps one witness
per shape and static-path parity; and the live reading remains available through an explicit
closure. The decision is the owner's.
