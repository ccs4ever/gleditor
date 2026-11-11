# Round-4 disposition — orchestrator rulings on v1.3 findings

All seven lanes returned NOT CONVERGED: **0 blocking, 8 majors, ~50 minors**. Every r3 finding is
resolved in substance; the r4 findings are precision defects in the new mechanisms plus residual
wording. Rulings below; "adopted" means the critic's proposed fix lands, amended where stated.

## Majors

**R4-A — sideness test dim enumeration (vortex-1, zigzag-3, codebase-1; architect-6 related).
Adopted, with codebase's refinement.** The named-dim list cannot reach the skeleton it exists to
classify (`+d.values`/`+d.entry`/`+d.code`/`+d.name` absent — program heads classify state-side,
permissive direction). But naive extension also misclassifies grafted runtime cells on `+d.clause`
ranks. **Ruling:** program-side = reachable from `program()` over the *full* structural dim
vocabulary (`d.stdlib`, `d.vars`, `d.clause`, `+d.values`, `+d.code`, `+d.entry`, `+d.name`,
`d.name`, `+d.refs`, `+d.recipe`, `+d.grab`, `+d.spin`, `+d.step`, `d.contract`, `d.belief`,
`d.source`) **and** minted within the program's own mint window — `birthOp` ≤ the program-image
boundary the genesis `programRef` bind marks (`arena_manifold.cpp:1798` already records birthOp).
Grafted/learned runtime cells exceed the window and stay classifiable. `closed`'s state-side arm
inherits the same test. Add architect-6's door clause: int64-referenced endpoints (control-flow
targets) are invisible to any link walk — §11-door, not claimed soundness.

**R4-B — `:`-label parse breaks `code_named` (zigzag-2, ergo-F3, meta-4, writer R4-5). Adopted.**
`code_named`/`+d.name` labels match **verbatim** — `src:`/`reify:`/`patch:`/user `#label`s are
opaque. The first-`:` split applies *only* to qualified-name lookups (`plan_named`,
`symbol_named`-style, patch target names) and only when the first segment resolves as a module.
The no-`:`/`"` ban scopes to user `#label`s. Drop the "rank-first match"/twin sentence —
`code_named` walks `d.code` only; the program-image twin is unreachable by construction.

**R4-C — genesis `+d.values` hop assumes a head (zigzag-1, architect-5, codebase-7). Adopted.**
`exportSymbol` binds `sym +d.values → entryOp` directly; apply §6.1's existing discriminator at the
hop: resolved cell is the entry op when it is an op, else follow `+d.entry` once. Applies to the
name-record chain and patch replay.

**R4-D — P16 step 1 marks every ref cell (zigzag-4, ergo-F10, writer R4-3). Adopted.** Step 1
scopes to ref cells **whose `+d.refs` record carries no `+d.name`** — the discriminator governs
both halves.

**R4-E — `cap$N` stamp fate (meta-1). Ruled: option (b).** The renamed `cap$N` keeps a stamp
naming the *captured binding itself*; the checker resolves it straight to that binding. The env
records `expand` invents are diagnostics/readability aid, not the resolution channel — meta-4's
liveness question dissolves. Companion (meta-7): `cap$N` renames **value bindings** only;
program-member refs (`def`/method/plan/goal names) stay name-resolved and carry `+d.refs`
name-records — never capture-renamed. Companion (meta-2): frame-bound captures are the ABI-debt
class — alias/capture-slot established per-activation by frame binding; `cells"$cap$N"` anchors
are refused on frame-bound captures (compile-time resolution), supported for stable captures only.

**R4-F — `graft_handler` formals (ergo-F1). Adopted.** Formals are the handler's bound names:
the event under `occurrence` or the `as`-name; fields read through the bound event
(`occurrence.f`), never bound as formals — matching spec :1205/:2153 and the R3-F3
`(name, type)`-pair rule.

**R4-G — empty-mailbox flag window + phases (architect-1). Adopted.** Flag fires only when
`pending(pid) == 1`; an empty-mailbox checkpoint decision mints a P4 `wake` turn so the flag has
a window. Two-phase state: re-kind pending → restore pending → drained; restore mirrors the
re-kind's guard — "restores `Int64(*handleTarget)` on every marked cell **still in `OpHandle`
kind**" (unmatched restore is a no-op); flag-clear on publish failure and `ActorDown` named.

**R4-H — §9 declarations malformed (writer R4-1). Adopted.** `goal Patrol() perform`,
`goal Feed() perform`, `goal Wait(w: Int) perform`; hoist `predicate` decls to top level;
`event WithinBudget { n: Int }`. Changelog line corrected to not claim example fixes.

## Minor batch — all adopted (no lane conflict)

- **Serial provenance** (vortex-2, zigzag-5, architect-4, codebase-6): tag = fresh `Int64` scalar
  cell holding the pass serial; serial drawn from the durable per-agent `d.vars` counter (the
  `reify` serial's source); the read names `value_of`, not `textOf`.
- **Kind-skip uniformity** (vortex-4, architect-1c): all three P16 passes skip cells already in
  target kind before `asInt64`/`handleTarget`.
- **Release guard** (vortex-5, codebase-5): `choiceDepth()==0 && outstandingMarks()==1` — m0
  itself. **Checkpoint aside** (architect-7): `checkpointView` requires `outstandingMarks()==0`;
  the hydration-floor `==1` regime is a parallel-worker artifact, name it or drop the aside.
- **"Int64-stale" wording** (vortex-3, meta-5, architect-3, writer R4-17): marked cells arrive
  `OpHandle`-kind with promoted targets; unmarked int64 cells (name-record slots) arrive stale.
- **Duplicated paragraph** (ergo-F4, meta-9, architect-8, writer R4-9): delete :932-936, fold the
  `isCellRelocationHandle` cite into :923-930.
- **Unlearned-graft `+d.refs` boundary** (codebase-2): state it — only `learn_*` makes refs
  durable; unlearned grafts may survive a checkpoint structurally with stale calls.
- **`navigate_to` example** (codebase-3, writer R4-16): recast — the name-recorded callee is
  `demo:Forager.navigate_to`; `"water"` is the argument.
- **`closed` content arm** (meta-3): "declared type is or *contains* `Cell`/`Code` (`any`, `Opt`,
  lists, records) — or whose content is a ref-box int naming a program-side cell".
- **`run`-target name-records** (meta-6): `+d.name` labels rebind through `code_named`; the
  emitter name-records every statically-known `run` operand; an opaque `run` target in a
  learnable fragment is a closedness refusal.
- **`symbol_named` arity** (ergo-F9, writer R4-6, zigzag-9): state the qualified-name →
  (owner, name) split once (first-`:` module, then agent.member) and use it at the genesis chain.
- **Template-def CBV** (meta-8): non-`Code` args bind call-by-value like `splice`; `Code` args are
  statically-known bindings, never re-evaluated.
- **`frontend.cpp`** (architect-2): add to the `VAGENT_FRONTEND_SRCS` edit; add `ast.hpp` to the
  inventory (`ExprKind::{Code,Splice,Static}` + stamp field).
- **Memo cell shape** (zigzag-7): record `{+d.values → wrapper head, +d.name → attach-cell key}`;
  `+d.step` chains records only — wrapper `+d.step` stays out of `validate`'s walk; stale-memo
  unlinked at re-wrap.
- **Rank convention** (zigzag-6): `d.code`/`d.patch`/`d.synapse` are same-dim chains per
  `insertIntoRank`/`rankTail`; the first-edge/chain split is the list convention only.
- **§8.2 residual + §8.1 rationale** (zigzag-8, writer R4-12): "a weight record naming the clause
  is its strength"; junction scalars are distinct because `link` gives `W` one `-d.weight` slot.
- **`reify` marker re-mint** (architect-9): the row names regenerated records *and* fresh
  per-ref-cell markers.
- **Re-learn carve-out** (vortex-7, zigzag-9, writer R4-4): a registered-head argument is reused —
  second `reify` skipped, `insertIntoRank` no-ops; only that arg-form dedupes.
- **`Code` arity** (writer R4-2, ergo-F6): 0-or-1 at :177 too; EBNF `Type ::= 'Code' ['<' Type '>']`;
  "when `c`'s type carries it" → "when the fragment is statically known" (meta-9).
- **Splice-only position** (ergo-F8, meta-10): refused in evaluated positions; `expand` erases
  dead `let` bindings left by full substitution, or the checker refuses them.
- **Stale refs/sentences** (writer R4-7/8/10/11/13/14/15, meta-10): `run_checked` `(§14)` → U-M10;
  blank line before `### 6.7`; merge the stranded `run`-spills sentence; `value_of` row joins the
  pure half; §6.1 binding list gains the impure-kind set; §7.3 drops resolved items; genesis
  ordering lists `opcodeMap_` re-registration.
- **Nit bundle** (writer R4-18, meta-10, vortex-6/8, codebase-4): `Cell`→`Code` where the run
  target is meant; "re-kindled"→"re-kinded"; drop the `(R2-B)` body cite; `code_named` walks
  `d.code` only; "`Agent.plan`-style" → three-part; `checkpointView` cite = `mesh_actor.cpp:668-694`;
  enqueue cite `arena_manifold.cpp:1759-1764`; `lexer.cpp:14-24`; appendix ordering; M11's seam
  description defers to §6.6; P16 "Where" splits kernel `fragment.cpp` vs adapter executor TU;
  `static(param)` in an emitted fragment errors at emission; `def run`/`def value_of` refusal
  under `import std:meta` stated; duplicate-label refusal anchored at §6.7; `graft(candidate)`
  pseudocode qualified; identifier-legality clause tightened.

## Round status

v1.4 to follow. Expected residual: wording-level minors at most — all mechanism rulings are
localized to text already written.
