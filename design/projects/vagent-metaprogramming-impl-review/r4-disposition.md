# Impl-plan round 4 disposition — v4 → v5

Tally: **4/7 converged at 0 majors** (ergo, meta, writer, codebase); three lanes
hold **the same major** (architect M-1, zigzag M4-1, vortex M1 — fully
convergent). Trajectory: r1 ~12 structural → r2 2 → r3 2 → r4 1, each round's
finding smaller and inside the previous round's own new text.

## R4-A — `promote`'s root is the agent cell alone (three lanes)

V4's re-genesis loop says "the cycle promotes once per runtime root, or under a
harness session root linked to both" — the spanning-root alternative my own
r3-disposition option text suggested, and all three lanes independently proved
it spec-wrong. A program-rooted or spanning `promote` republishes the program-
side subgraph: bare grafts become durable (spec :375-377 — "a session graft is
gone at `resume`" is the boundary that makes `learn` = graft + record a
distinction), graft-adds carry without prune-removes (unlinks never un-emit),
and replay-installed copies double under (d), whose alias re-match cannot see a
promoted `patch:<n>` label (spec :1322-1324). The cited spec :2265-2266 is
(a)'s classification-walk vocabulary, not a promote license; all durable state
already hangs on the agent cell's ranks (`d.code`/`d.patch`/`d.synapse`/
`d.vars`, spec :1870-1871/:2465).

**Ruling:** program-side is *never* a promote root; the root set is the agent
cell plus other *state-side* anchors only. The only program-rooted promote is
the one-time image seed (already named separately). Ergo's m-1 is the same
defect — one edit fixes both.

## Minor bundle (applied in v5)

- `d.vars` repoint precision: the keying record's `+d.values` edge repoints,
  not "the rank" (spec :1300 — architect m-2).
- `unlearn` drop classes: mid-rank record drops are repoints (link-writes,
  promote fine — `unlink` = `link(…,nullopt)`, `arena_manifold.hpp:616-618`);
  the tail drop is a true removal → tombstone only (zigzag m4-2); `learn_plan`'s
  supersede was mis-paired as the removal example.
- Residual re-scope (zigzag m4-1): with per-cycle filing landed, the residual
  is one orphan per unfiled session-minted name at its *first* fold — the
  first-use edge split (`store.cpp:1066` `B` vs arena `A`, `discover`→promote
  mints `A'`, `arena_manifold.cpp:1781`/`:2030-2042`) — not per-checkpoint
  accrual.
- "nineteen names" → nineteen cells, eighteen filed names (`home` is a plain
  genesis cell — codebase n); `dimDims && home` conjunct restored (:91-93).
- Cites: `arena_manifold.hpp:833-836` (vortex n1 — was :837-846); spec :634-635
  (was :635-636); M13 edges `spec :456-470` + `splice c := 3` at `:784-786`
  (writer m-1, meta m8 — it's §5.2's all-splices "never a place" rule, not
  M13-scoped); `static` baseline `:952-960` (writer n-1, codebase).
- `'splice' Postfix` + peeling rule (spec :736-738) in phase 1's productions —
  without it `splice f(args)`'s arglist can't re-attach (meta m7).
- `run`/`graft` refusal-arm promise split: `run` arm in phase 3, `graft` arm in
  phase 4 where the verbs live (meta m9).
