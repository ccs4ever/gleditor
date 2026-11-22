# Xanadu & ZigZag design document map

Annotated map of `design/` — where the architectural reasoning lives. A design document can describe
more than is built; check the code before relying on proposed behaviour.

- [`store-slice-convergence.md`](../../../../design/store-slice-convergence.md) — the active plan: a
  cell is an operation and a slice is a replay product. Fourteen rulings (R1–R14) with prices, the
  migration (complete through `ArenaManifold`, the ephemeral arena), open questions U1–U3. Read
  before touching `CompactOpNode`, `Manifold`, or the zigzag sync path. R8 (view movement does not
  mutate a visited store; a separate activity store records completed reader visits), R11 and R12
  are the ones most often needed.
- [`view-system.md`](../../../../design/view-system.md) — proposal, unbuilt: xuzz's pluggable View
  system over the store. Slice views (stretch vanishing, all-dim walk, dimensional pack) over a
  `ViewManifold` with cell-based bindings and an O(1) toss; page views (base, stacked vanishing);
  scenes and panes; and where all of it lives. Read before adding a view or touching
  `ViewAxisBinding`, `ZigzagVisualizer` or `xanadu::Views`.
- [`view-system-implementation-plan.md`](../../../../design/view-system-implementation-plan.md) —
  plan, unbuilt: the order of work for the two documents around it — spikes, tracks, milestones,
  gates, the visual direction and its checks. Its §2 lists corrections found against the code that
  the spec and the rendering plan have not yet absorbed; read it with them.
- [`projects/start-view-project.md`](../../../../design/projects/start-view-project.md) — the prompt
  that starts the view project in a fresh session, with the owner's decisions; the reviews it was
  planned from are in `design/projects/view-reviews/`.
- For world-space rendering (`design/world-space-rendering-plan.md`), see the
  [`rendering-and-text`](../../rendering-and-text/SKILL.md) skill.
- [`structure-hyperop-vision.md`](../../../../design/structure-hyperop-vision.md) — what else
  Structure can carry, grounded: the wire defect it documents, and a prerequisite-ordered proposal
  list.
- [`vortex-hyperstructural-runtime.md`](../../../../design/vortex-hyperstructural-runtime.md),
  [`vql-query-language.md`](../../../../design/vql-query-language.md),
  [`vpl-array-language.md`](../../../../design/vpl-array-language.md) — the runtime and the two
  front-end languages over the manifold. All three are implemented under `apps/common/xanadu/`; each
  doc carries a reconciliation section against the convergence and a versioned change history.
- [`vlog-logic-extension.md`](../../../../design/vlog-logic-extension.md) — Vlog: unification and
  backtracking inside Vortex. §4–§5 (arena, choice points, trail, `unify()`) are built; §6–§7
  (clause selection and a solver written *in Vortex*) are not and are gated on U1. `vprolog` runs on
  the VM's C++ logic opcodes instead.
- [`vicon-goal-directed-language.md`](../../../../design/vicon-goal-directed-language.md) — vicon:
  an Icon-shaped goal-directed language whose generators are Vlog's choice points. Asks the C++ core
  for nothing (§8.1) because `#Choice`/`#Fail`/`#Cut` over the `ArenaManifold` already exist, and
  all new standard-library code lives in a `std:goal` package written in Vortex.
- [`vlace-functional-language.md`](../../../../design/vlace-functional-language.md) — vlace: a
  text-based, purely functional (ML/Haskell-shaped) language whose purity is R8's arena regime —
  evaluation mints no operation, so referential transparency holds, and the append-only spool plus
  `d.clone` are its persistent data structures. Keeps new standard-library code in a `std:fun`
  package written in Vortex.
- [`vagent-agent-language.md`](../../../../design/vagent-agent-language.md) — vagent: an
  agent-oriented (SARL-shaped) sixth front end over `ArenaManifold` and Spool-Mesh distributed
  actors (`vortex/mesh_*`); checker and single-node runner (`build/vagent`) are built (see
  [`docs/vagent-user-guide.md`](../../../../docs/vagent-user-guide.md) and
  [`vagent-implementation.md`](../../../../design/vagent-implementation.md)).
- [`enfilade/`](../../../../design/enfilade/) — `enfilade-discussion.md` (the survey and the 4.96 ns
  breakeven), `enfilade-rank-indexing.md` (the U1 proposal; §3 says why crums are not cells), and
  one note per built enfilade (spanfilade, chronofilade, holefilade, layoutfilade, arrayfilade).
- [`osmic-microversioning-and-dag.md`](../../../../design/osmic-microversioning-and-dag.md) —
  hypertime naming.
- [`system-xanadocs-customization-and-metasystem.md`](../../../../design/system-xanadocs-customization-and-metasystem.md)
  — the settings model; §6.2 is the live ten-dimension geometry, §4.1 preset sharing is unbuilt.
- [`xudu-zigzag-unified-hypermedia-bridge.md`](../../../../design/xudu-zigzag-unified-hypermedia-bridge.md)
  and
  [`xudu-zigzag-hypermedia-bridge-next-stage.md`](../../../../design/xudu-zigzag-hypermedia-bridge-next-stage.md)
  — the bridge (`UniversalLinkEnd`, `BridgeCoordinator`, Spanfilade as canonical discovery); the
  federated cross-store link source is explicitly deferred.
