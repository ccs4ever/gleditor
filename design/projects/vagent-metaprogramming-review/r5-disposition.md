# Round-5 disposition — orchestrator rulings on v1.4 findings

Two lanes CONVERGED (ergo, architect); five returned NOT CONVERGED: **0 blocking, 5 majors,
~18 minors + nits**. Every r4 finding resolved in substance; r5's majors are all localized to the
R4-A/R4-B machinery. The `birthOp` window ruling was verified by the orchestrator against the tree
before ruling.

## Majors

**R5-A — `birthOp` is not a timestamp (vortex-1, zigzag-1; architect-4 concurs; codebase verifies
the field mechanics). Adopted, reframed.** `arena_manifold.cpp:1183-1186` states it directly: an
arena cell has no birth operation — `birthOp` carries the cell's **own ref** (`ephemeralBit|dense`);
a shadow's `birthOp` is the *base cell's* ref (`:1489-1490`), which does not order against a fresh
boundary. **Ruling:** drop `birthOp` from the test. The discriminator is *ref-space ordering*: a
cell is program-side iff reachable from `program()` over the full structural vocabulary **and**
its ref is a persistent (non-`ephemeralBit`) ref **at or below the recorded program-image extent** —
the maximum persistent `CellRef` minted at program load, recorded by the loader and bound by
genesis step (c). Ephemeral arena refs always test above the boundary (grafted/learned cells);
shadow cells carry their base ref but are freshly-minted overlay members — the base ref they name
is pre-extent *and* persistent, so a promoted graft's shadow classifies program-side **by design**
(it is the resurrected program image). Non-goal: the per-incarnation extent vortex warns against —
the boundary is recorded once at load and never re-marked per genesis.

**R5-B — `run`-target name-records dead-end (meta-major). Adopted.** `code_named` walks `d.code`
only; statically-known `run` operands are program-side literal heads — never registered. Ruling:
a name-record's `+d.name` resolves by *shape*: a qualified `module:Owner.name` rebinds through
`symbol_named` (R4-C's head/op discriminator); a `src:`/`reify:`/`patch:`/user `#label` rebinds
through the **program-image label→head lookup** — the same `program()` structural walk as the
sideness test, matching fragment descriptors' `+d.name` labels — falling back to `code_named` for
learned heads. The emitter name-records every statically-known `run` operand (literal → its
`src:` label; `code_named`/`reflect`-born → its `d.name`); an opaque target stays a closedness
refusal.

**R5-C — `symbol_named` owner domain (writer-R5-1, ergo-R5-F1). Adopted.** All symbols live on
the **module's** `d.vars` (spec §5.7) — goals/events/agents alike; "the owning `Agent`'s
`d.vars`" has no referent. Owner = the module name (bare module string) or the
`module:Agent.member` qualified form split at first-`:` then last `.`. §9.3's example becomes
`symbol_named("demo","Feed")`; state the agent-alias convention (an `Agent` segment resolves its
module) or drop the agent arm entirely — rule: drop it, one spelling.

**R5-D — state-side transit clause missing (zigzag-2). Adopted.** "State-side = link-reachable
from the agent cell *without transiting a program-side cell* and not itself program-side" —
memo records, wrappers, and installed `learn` copies sit behind program-side anchors and must not
promote under the state banner (they die with the skeleton as specified).

## Minor batch — all adopted

- **"Pre-promotion targets" inverts promote** (vortex-2, meta, architect-3, codebase-2): handles
  carry *promoted* targets (`:1947-1952` writes `real.at(target)`).
- **`CellImport{OpHandle}` / `MeshOp` fabricated kinds** (codebase-1/3): remove both; marked cells
  stay `OpHandle`-kind post-import (`mesh_value.cpp:494-501`), unmarked demote to `None`.
- **Genesis (e) rehydration walk unscoped** (vortex-3): scope to `learn`-registered heads'
  records — consistent with the unlearned-graft stale-call boundary.
- **Wake-era scheduling seam** (architect-1): the publish window is "immediately after the flagged
  turn's end, before any subsequent turn runs" — wake turns are enqueued by the publish path
  itself, never interleaved; reconcile "cleared on publish failure" with restore-on-exit (flag
  clears only after successful publish *and* restore).
- **P4 dependency** (architect-2): name it in the register row — the empty-mailbox arm depends on
  P4's approved-but-unimplemented `mesh_actor` change; soften "no new core mechanism" to
  "no new mechanism beyond P4's already-approved wake".
- **`rankTail` cite** (codebase-4): → `cell_views.hpp:258`.
- **`reify:<serial>` spelling** (ergo-R5-F3, writer-R5-3, zigzag): canonical `reify:<src>:<serial>`.
- **`opcode_named` dangling** (writer-R5-2): `opcodeMap_` re-registration step names the correct
  identifier.
- **`d.extends` + `d.dims` vocabulary** (writer-R5-4, zigzag): add `d.extends` to the structural
  list; keep `d.dims` excluded (promote skips it — name the exclusion explicitly).
- **Memo `+d.name` key eviction** (zigzag): the attach-cell key rides the record's `+d.values`
  slot or a fresh content cell, not `+d.name` (which holds the label) — state which.
- **`as`-name referent** (ergo-R5-F2): `graft_handler`'s `as`-name arm applies only when grafting
  *onto* a declared clause that has one; a minted clause binds `occurrence`.
- **Scope-id payload wording** (meta): the stamp carries a binding identity token (scope id +
  binding ordinal), not a bare scope id — one clause.
- **Walk direction / live frame cells** (meta): the `program()` walk is outbound-following;
  state-side's root is the agent cell — live frame cells behind program-side anchors fall under
  R5-D's transit clause.
- **Micro-nits** (writer/codebase/meta bundles): apply all — d.clause spelling, label-shape
  vocabulary consistency, residual cite touch-ups.

## Round status

v1.5 to follow. The defect surface keeps contracting to the newest text — convergence expected
next round.
