# R2 disposition — design/vagent-metaprogramming.md v1.1

All seven r2 critics returned NOT CONVERGED: writer (1B/4M/12m), vortex (1B/5M/9m), codebase
(1B/2M/4m), architect (1B/5M/7m), ergo (1B/6M/3m), zigzag (2B/4M/10m), meta (2B/9M/6m). Every r1
finding was ruled resolved or resolved-in-shape by its lane — v1.1's failures are in the *new*
machinery. Six lanes independently converged on the same four root issues: P16's ceremony is
incomplete, `d.code` registration reopens V12, the `d.validate` seen-set is backwards, and the
`cap$N` carrier contradicts itself. Verified load-bearing claims against the tree:
`isCellRelocationHandle` requires OpHandle kind *and* the `d.mesh.relocation→"cell"` edge
(`foreign_op_handle.cpp:109-117`); `checkpointView` hands the live arena and the actor resumes on
it (`mesh_actor.hpp:102-106`, `mesh_actor.cpp:668-694`); `promote`'s BFS discovers both edge
directions (`arena_manifold.cpp:1782-1783`); `link` evicts same-dim-same-direction neighbours
(`:1347-1377`); `validName` refuses `$` at kernel preflight (`compiler.cpp:407-417`); `let`
lowers to a fresh-cell `copy` (`:705-707`,`:853-855`); `writeValue` resolves `masterOf`
(`vortex_core.cpp:202`); `resume` overlays without `importValue` (`mesh_actor.cpp:260-271`).

## Rulings

### R2-A. P16 must be re-specified — accepted (vortex-1/2/12/14, zigzag-2/5/6/13, architect-1/2/5/7, meta-2/9, writer-N5, codebase — convergent)

1. **Full relocation ceremony.** Each marked ref cell gets both halves of the precedent:
   `setValueBits(OpHandle, target)` *and* a `d.mesh.relocation`-dim edge to one shared
   `"cell"`-texted marker (minted once per pass). The marker edge is emitted at fragment-emission
   time (inert while the cell is Int64) so the checkpoint pass is a pure re-kind.
2. **Live-arena restore.** After `publishValue` returns, the pass re-walks each `d.code`
   head's `+d.refs` rank and writes `setValueBits(Int64, *handleTarget)` — mechanical, no
   association table, idempotent (skip already-Int64 cells). The marker edge stays in place.
3. **Home: the adapter, not beside `publishValue`.** Everything the pass needs is public arena
   API; `publishValue`'s const ref cannot host it anyway. P16 is re-scoped in the register: an
   adapter-side pre-checkpoint pass + a genesis post-resume pass; the only kernel-side duty is
   `+d.refs` emission (already §10.1's fragment-emitter item).
4. **Genesis ordering stated:** arena overlay → `ensureDimension` pass → `+d.refs` rehydration
   (`Int64(*handleTarget)` per marked cell — resume does *not* run `importValue`'s remap) →
   `d.patch` replay → turns.
5. **Mark completeness — uniform rule:** every `makeScalarCell(int64(ref))` the emitter mints:
   both wings' slot cells, `reference()` cells, dim-constant slots (`dimConstant` at
   `compiler.cpp:769-772`). Runtime-minted ref cells are unmarkable — stated carve-out: `Linked`
   output boxes are safe because `Linked` rebuilds its rank per execution.
6. **Marks on a dedicated dim:** record `+d.ref → refcell` (new dim in the adapter set), not
   `+d.values` — the occupancy convention reserves `-d.values` and a later `+d.values` write
   would evict the mark silently.
7. **`reify` regenerates `+d.refs` and `+d.recipe` for the copy**, endpoints through the old→new
   map. Recipe link tuples encode `from`/`to` as *links* (record `+d.values`-pointing at the
   cells) so relocation handles them — never census ints (stale at load, before any checkpoint).
   External captures preserved verbatim; `reify` is "closed iff the source was"; v1 replays the
   recorded emission template — current-graph copy remains U-M7.
8. **Pass spec wording:** "each `d.code`-registered head's `+d.refs` rank" (patch records hold
   names, not links).

### R2-B. `d.code` registration must never reach a program cell — accepted (zigzag-1, architect-6, writer-N4)

**Invariant:** no cell on the agent's `d.code` rank may carry an edge in either direction into a
program cell. Concretely:

- `run`/`graft_*` never register. `learn_*` grafts a `reify` (session-side installed copy, dies
  at resume) and registers the *pristine uninstalled original* under the patch's recorded name.
  `unlearn`'s re-point likewise installs a reify of the named prior head. `graft_*` of a
  `d.code`-resident head reifies first (or refuses — reviser picks).
- Reflect wrappers move off `d.code`: memoized session-side, keyed `clause +d.code → wrapper`
  (a clause-side edge — clause cells aren't state-reachable, so wrappers aren't promoted and die
  with the program at resume, matching §6.7's own session-handle rule). Wrapper heads name their
  graphs by ref-box int, not `+d.entry`, for the same reason.
- `unlearn` removes the head's `d.code` registration; patches still naming a deregistered head
  drop after a fault record at genesis.

### R2-C. `d.validate` seen-set direction — accepted (writer-N1, vortex-3, codebase-1, architect-8)

`visited +d.validate → marker`; membership `linked(visited, d.validate, POS) == marker`. One
mark slot per visited cell, N backlinks on the marker for free, stale marks name a previous
pass's marker and fail equality. (Zigzag's r1-verdict line approving the mechanism reviewed the
intended direction; the written direction is the broken one — code evidence decides.)
Additionally: `validate`'s walk is bounded to the op-graph dims (`d.spin`, `d.step`, `±d.grab`
wings, `d.entry`, int-ref targets) so it cannot cross a head's occupancy backlink into the
program (zigzag-15); and one clause noting the marks are trailed non-semantic writes inside a
`pure` builtin (vortex-15).

### R2-D. `cap$N` carrier — accepted, rewritten (meta-1/3/4/5/6/8, architect-3/4, vortex-7, ergo-F3, writer-N11)

1. **No synthesized `let`.** `cap$N` is a pure binding-table alias: the checker records
   `{cap$N : type-of-w}` in its env *at the literal's position*; fragment emission and the merged
   emission resolve `cap$N` via `CompilerBindings` to one cell — the captured binding's own cell
   for stable cells (fields, module vars, let-locals — true cell identity, live reads); frame
   parameters are ABI-debt with the observable rule written down. Captured cells are referenced
   by links so `promote` relocates them. This also fixes: `validName`'s `$` refusal is moot (no
   AST node is emitted), the snapshot-vs-alias contradiction, and head-of-block ordering.
2. **One global counter per `expand` run**, shared across all literals and substituted bodies;
   checker refuses `$`-bearing names on user-supplied decls.
3. **`cells"…"` `$` anchors:** `expand` rewrites `CellsPath` anchor variable names through the
   same rename map (the body is re-parsed at expansion anyway) — capability kept; the refusal
   alternative is refused in §11.
4. **Scope fidelity:** pipeline is parse → resolve-only pass (stamps each `Identifier` with its
   binding's scope id — one scope truth, no duplicated env rules) → `expand` → check. The doc
   names this pass instead of "a scope-tracking walk".
5. **Fragment table keyed by `SourceLocation`/ordinal** stamped on the `Code` node by `expand`
   (clones share the original's record); the record's body AST is itself re-cloned per emission
   (kernel tables key on `const Expr *`).
6. **Static knowledge is transitive and post-substitution**: a `let`-bound name is statically
   known iff its initializer is (literal, statically-known identifier, substituted actual);
   `expand` performs the analysis; `var`-bound literals get an explicit refusal line; inline
   literals admitted (writer-N6 fixed accordingly).

### R2-E. `closed` covers all outside references; learnable fragments bind calls by name — accepted (ergo-F1, writer-N5, vortex-6)

Option (b) of the finding, consistent with the doc's own V11 story (durable references are links
or names, never ints): fragment emission records Call/event targets on `+d.refs` as **name-refs**
— record carries the callee's qualified name; the checkpoint pass does not OpHandle-rewrite
name-refs (their targets are program-side, unpromoted by design); genesis re-resolves the name
through `symbol_named` and rewrites the ref cell to the fresh target — the same machinery patch
head-name resolution already uses. `closed` is redefined: every outside reference is either a
state-side cell (link/captured binding) or a name-bound program symbol. `learn` **re-verifies**
closedness at accept time (walk the fragment's link frontier + `+d.refs` targets — emission-time
flags are forgeable by post-emission surgery). The §9.3 example stays valid: `navigate_to`
resolves by name. §9.2's `learn_where` is unaffected.

### R2-F. `run`'s surface — accepted (ergo-F2, writer-N2, codebase-2, meta-10, vortex-8, architect-12, codebase-3/5)

`run` is a checker special form: `run(c: Code<T>, args)` takes a list literal whose elements are
individually typed (per-element `any` wildcard — a parameterized-wildcard note, codebase-5), each
evaluated and bound on the `#Call` frame by parameter name per §5.8 — values, not ref-boxes;
frame-bound per activation (recursion-safe); descriptor param cells stay signature data.
Compile-time arity/type check when the fragment is statically known; `run` on `Code<()>` yields
`()`. `Code` joins `builtinTypes` (architect-10). The descriptor records link the `result` and
`produced` *cells* (not only type names) so `run` can reach them — added to §6.1's layout; they
need no `+d.refs` marks (no int content). A `value_of(c: Cell): any` checker-special reader is
named for `Code<Cell>` results (ergo-F9e).

### R2-G. P15 residual corrections — accepted (writer-N3, meta-16, vortex-10/5, vortex-9)

Predicate tests `opcodeMap_.contains(masterOf(cell))` — a `Clone` of a registered op writes the
master's bits through the funnel. The funnel's refusal must be distinguishable: `writeValue`
returns a typed refusal (expected/optional-reason), `Value`/`Splice` bodies map it to a failed
output, the four writeback sites to `success=false ExecutionResult`. Exempt-by-construction
writes named: `setCursorOpcode`, `Call` frame writes, `mintOpcode`'s own `setContent`. The
`release` guard is "innermost outstanding mark" (`outstandingMarks() == 1` under cp-only marking)
— `checkpointView` already checks it.

### R2-H. Remaining majors — accepted

- **Bound-output `New` (vortex-4):** `validate`'s refusal covers any repeatedly-executable
  fragment — every `graft_*` target, not only `run`-reachable; wrapped program graphs correctly
  fail when containing `%`; `New` refusal covers both append and non-append bound output
  (writer-N14).
- **Reflect wrappers (meta-11):** descriptor fields are `unknown`; grafts requiring `pure`
  refuse unknown-flagged wrappers; §6.6's graft-for-session promise narrows to body/symbol
  grafts; wrapped graphs aren't `run`nable (`Halt` tails — vortex-13, say so).
- **Template model (meta-7/8, ergo-F6):** trigger on *use* (the `def` splices a `Code`
  parameter), not signature; template `def`s cannot be recursive directly or transitively;
  diagnostic ownership — call-site loc for context checks, def-site for body errors; trigger
  scope = parameter whose declared type is `Code<…>` (nested `Code` opaque); member coverage
  stated; `return`/`yield`/`consume`/`terminate` refused in template bodies; per-site checking
  stated; refactor consequence noted.
- **Labels (zigzag-3, ergo-F4, meta-15):** `reify` mints `reify:<src>:<serial>`; registration
  refuses duplicate labels; genesis replay performs graft steps without re-recording patches.
- **Occupancy (meta-12, zigzag-10):** test is *any* install-dim backlink; only attach verbs may
  write head `-d.grab`/`-d.spin`/`-d.values` — the reservation is stated.
- **Head-vs-op discrimination (zigzag-9):** `+d.entry`-presence is the test (heads carry non-op
  content anyway; state one).
- **§4.4 contradiction (zigzag-7):** heads only; op roots only in graphs predating the
  convention.

### R2-I. Minors — accepted bundle (all lanes; folded into v1.2)

`unlearn`: clause-or-symbol param, matching = resolved target name, deregistration rule,
empty-`+d.grab` window noted (zigzag-8/14, writer-N13). `cloneExpr` copies every `ExprPtr` field;
`cloneDecl` at `Decl` granularity for M13 (architect-9). Makefile §10.1: name which explicit list
each TU joins; `VAGENT_SYNTAX_OBJS` unchanged iff the `code` case stores raw text for `expand`
(architect-11). `d.patch-head` justification clause (architect-13). `static`: post-eval
`choiceDepth`/`marks` baseline as compile error; residue never reaches the sealed artifact;
`let` literal closedness vs `var` exclusion; bare-operand parse error; per-site re-eval priced
(meta-13, ergo-F9h). `static`'s splice bookkeeping (meta-14). `run` named in M6's allow-list
(meta-17); opaque `run` compile-impure with `run_checked` candidate (ergo-F9i). `graft_where`/
`graft_handler` formals for `graft_plan` are the goal's; grafted tests cannot bind `?`-vars
(ergo-F9c); fragments always non-effect → `graft_method` onto `effect` refuses (ergo-F9d).
`reify` "fresh copy, closed iff source was" (meta-9, ergo-F9g). Diagnostics paragraph
(ergo-F7): `cap$N` renders as the captured name; site-vs-fragment attribution; clone position
retention; `learn` refusal names offending refs; nested `+o:c` chains; template errors point at
call site. Deref-after-consume contract (ergo-F8): installed fragments still `run`/`reify`;
backtracked graft frees the `Code`; per-dim occupancy list; literal-evaluates-same-head example.
Synapse endpoint domain stated (zigzag-16); weights only on junction `d.weight` or agent
`d.vars`; clause-correlated weights key the clause name (zigzag-4). Wrapper memo invalidation on
attach-cell re-point (zigzag-11). `d.weight` edge-vs-bits disambiguation (zigzag-4). §5.1:543
nested example fixed (writer-N6); undeclared decls across §8.3/§9 declared or elided-note
(writer-N7); reflect-family wording (N8); §10.1 dim list completed (N9); "of these, only `code`"
(N10); `infer` "const-in, judgment-out" (N11); descriptor param cells chain `+d.step` off
`+d.grab` in declaration order (N12); citation wobbles N15 + codebase-4's ten cites; patch
glossary wording (N16); "once per bound-output writeback" (N17); `plan_named` three-part
qualification (codebase-6); §11 grounding cite (codebase-7); `run` inside `code"…"` named
(meta-17); `value_of` named (above); `learn_plan` patch chains both heads via `+d.step` already
covered.

### REFUSED — none this round. Every finding verified against the tree and accepted.

## Proposal register updates for v1.2

- **P15** — predicate on `masterOf(target)`; typed refusal surfaced per site; exempt-by-
  construction writes named.
- **P16** — re-scoped: adapter-side pre-checkpoint pass (OpHandle re-kind + relocation-marker
  edge per marked ref cell) + post-publish restore pass + genesis rehydration pass with stated
  ordering; kernel-side duty is only `+d.refs`/`d.ref` emission at fragment emission. No new
  core mechanism — the register row should reflect that the pass needs no core change.
- **P17** — stays deferred-not-registered.

## Revision order for v1.2

P16 re-spec (R2-A) and the `d.code` reachability invariant (R2-B) first — they reshape §6.6/§6.7
and the register; then `closed`-by-name (R2-E) which rewrites `learn`'s contract and §9.3; then
the `cap$N`/expand rewrite (R2-D, §5.3/§5.6); `run`'s signature (R2-F); validate seen-set (R2-C);
P15 residuals (R2-G); then R2-H and the minor bundle. Bump to v1.2 with a changelog row.

VERDICT: NOT CONVERGED — v1.2 required; round 3 critics re-review.
