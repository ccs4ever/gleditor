# Impl-plan round 2 disposition — v2 → v3

Tally: 2/7 converged outright (meta, writer); codebase substance-converged at 0
majors. Every r1 ruling verified closed — the repartition held. Remaining:
**exactly two majors**, both multi-lane, both narrow clause-level omissions in
v2's own new text.

| lane      | verdict        | B | M | m  |
| --------- | -------------- | - | - | -- |
| architect | NOT CONVERGED  | 0 | 1 | 5  |
| codebase  | (substance)    | 0 | 0 | 3  |
| ergo      | NOT CONVERGED  | 0 | 1 | 3  |
| meta      | CONVERGED      | 0 | 0 | 6  |
| vortex    | NOT CONVERGED  | 0 | 2 | 5  |
| writer    | CONVERGED      | 0 | 0 | 3  |
| zigzag    | NOT CONVERGED  | 0 | 1 | 5  |

## R2-A — genesis (e)'s third arm unassigned (zigzag M2-1 + architect M-1 + vortex nM2, three lanes)

Spec :2395-2406 gives the rehydration sweep **three** per-record arms; v2 names
two. The dropped arm: `+d.name`-bearing `+d.recipe`/dim-ref-capture records
write `Int64(DimRef)` re-resolved through `ensureDimension`'s name arm — at
**every** genesis, because emission-time `DimRef`s are stale compile-arena ids
(spec :2397-2398 forbids the promoted twin). It is neither the name-record
role-split rebind (phase 5) nor the deferred `Int64(*handleTarget)` arm
(phase 6/P16), and unlike the ref arm it is *not* a no-op pre-P16. The plan's
phase-3 schema even names the channel "consumed by phase 5's (e) rebind"
(:194) — a consumer never delivered. Same exception missing from (d)'s
donor-slot derivation (:2369-2372).

**Ruling:** phase-5's (e) bullet gains the third arm explicitly —
dim-record → `Int64(DimRef)` via name-keyed `ensureDimension`, never the
promoted value — and (d)'s derivation notes the same exception. One clause;
the mechanism already exists in the spec.

## R2-B — the "re-" in re-genesis has no producer (ergo M1 + vortex nM1, two lanes)

Phase-5's exit iterates "learn→re-genesis→learn" but no bullet produces the
fold the *second* genesis binds: session mutations live in the ops-less
`ArenaManifold` overlay; the only path into a fold is `promote(Store&, parent,
arena, root)` (`arena_manifold.hpp:845-847`) → `Store::rebuildManifold` →
re-attach → the (a)–(e) driver — never named. `overlay.cpp:46-82` runs the
identical loop with no actor; everything exists and is kernel-simulable —
it's an unassigned work item, r1-B2 one level down. The "Requires: a local
`Store` only" line also undercounts (the fold needs the overlay→store hop).

**Ruling:** phase-5's driver bullet names the re-genesis loop explicitly —
promote → rebuild → re-attach → re-run (a)–(e) — and the Requires line lists
the harness objects (`promote`, `rebuildManifold`, fresh `WorkerContext`).

## Minor bundle (apply all)

- (d)'s replay rules: state "replay does not re-record" — no `d.patch` cells,
  no `patch:<n>` serial consumed — while `reify:<src>:<serial>` *does* draw
  off the shared `d.vars` counter at replay (spec :2346-2350, :2504-2509;
  vortex m1, zigzag m2-2, codebase m3); and the donor-slot promoted-content
  endpoint derivation (:2364-2372).
- `CompilerBinding` "state-side marker bit" — no such bit exists
  (`compiler.hpp:15-18` is `{cell, type}`); either schedule it or restate the
  `closed` flag's carrier as the per-fragment tables (writer m-2, vortex).
- Filing list narrowing: say which bind-set members file as *names* vs bind
  as *cells* — program root/agent cell are non-fileable (zigzag, writer m-3).
- (b) bullet: split `ensureDimension` (dim-name resolution) from
  `setOpcodeKind` re-registration (op enumeration) — two arms, not one scope
  (zigzag m2-1, writer m-1); keep (c) to the programRef→root hop only.
- Cite drifts: `store.cpp:1388-1405` → the `bindPublishedLocalScroll`/
  `setContentSource` pair lives at :1418/:1425 (ergo m1); Makefile filter-out
  is :699 not :698 (vortex n); `zigzag_test` at :1351 (codebase n);
  `d.op-label`/`or_parallel` members need their code cites appended
  (`mesh_value.cpp:473`, `or_parallel.cpp:181-184`, codebase m1).
- `fragment.{hpp,cpp}` placement: §10.1's tree puts it under `vagent/`
  (spec :2944); the plan's own filter-out rationale requires it (codebase m2,
  zigzag).
- `learn_method`'s recorded name is "never an alias" (spec :1851-1853) — the
  record field's label must not call it one; it's what `unlearn` matches
  (architect m-2).
- `run`/`reify` are opaque-impure/impure per spec — don't label them "read
  group" (ergo n2); the member-surface split is read-vs-mutation by
  *function*, not purity.
- Phase-6 internal order: the ref-record arm (step 2) depends on P16 flags
  (step 3) — reorder or note (architect m-4).
- Phase-3 exit: name the minimal call convention's coverage floor —
  frame-bound capture classes define the refusal rules (ergo m3,
  architect m-3); and "no-op without promote" → the arm is a no-op because
  nothing mints `OpHandle` pre-P16 (vortex).
- `check()` public-API ripple: ~10 `vagent_checker_test` call sites bypass
  resolve/expand — state the env-walk fallback rule that keeps the suite
  green (meta F2); pin the skeleton-refusal's scope (program-wide,
  body-recursive, F1); `closed` computes off outside-reference
  classification, not the capture set (F3); name the descriptor's full field
  schema beside `closed` (F4).
- Requires tightenings: (iv) wake is tree-local-testable, `pending(pid)==1`
  is a proxy for mailbox-empty-at-turn-end, emit-side filing is owed
  substrate (architect m-5); `test_of`/`body_of`'s clause-side memo record
  chain (spec :311-331) priced in phase 3 (architect m-1).
- Nits: step (i) passes `Store` where the ctor takes `Manifold*`+reader —
  tighten wording (vortex); "`Call` op" vs spec's "not a `Call`-opcode
  operand" (architect n); "descriptor re-point" misnomer (architect n);
  prelude skeleton's home unnamed (architect n); seam list's conditional-
  last ordering noted (ergo n1); "kernel tables" → Emitter's tables
  (codebase n); `vortex_core.cpp:14` initGenesis cite (architect n).
