# Impl-plan round 3 disposition — v3 → v4

Tally: **5/7 converged at 0 majors** (architect, vortex, ergo, writer,
codebase-substance). The two remaining majors are single-clause omissions in
v3's new text — the trajectory is healthy (r1 ~12 structural, r2 2 clause-level,
r3 2 clause-level).

## R3-A — `splice`'s call-by-value marshalling unstated (meta M1)

Phase 2's bullet reduces `splice` to "substitution via `cloneExpr`" but never
states §5.4's marshalling: **a fresh `let` local per argument at the expanded
body's head, evaluated once in argument order** — the `{ let p0 := count;
p0 + 1 }` shape §9.1's own exit requires. It's the design's only synthesized
`let` (cap$N is explicitly not one); a textual-substitution misread yields
observable multi-eval semantics on impure actuals. **Ruling:** one clause in
phase 2's splice item naming the `let`-per-arg marshalling + once-only
evaluation + argument order.

## R3-B — `promote` visibility overclaimed for the re-genesis loop (zigzag M3-1)

Phase 5's loop says "each cycle promotes them into the fold" — wrong about
`promote` for two mutation classes the plan itself prices. `promote` emits
content ops only for `isEphemeral` cells — shadowed base cells get no content
op (`arena_manifold.cpp:1867-1870`) — so an in-place `d.vars` counter bump
rewinds at fold 2 (patch:<n> ABA, breaking the durable-semantics bullet) and
`set_weight`'s junction-arm value write drops silently. Base-edge unlinks
never un-emit (:2020-2053) — they survive only via (d) replay. **Ruling:**
one clause naming the promote-visible class — ephemeral mints + link-writes;
counters take mint-and-repoint form (each bump a fresh ephemeral + repoint);
removals ride (d) replay or tombstones. Architect's m-1 pairs here:
`promote`'s `root` argument is load-bearing — session mutations span two roots
(agent cell vs program-side anchors, spec :2265-2266) — name it (the agent
cell per spec :375/:635-636, or a spanning session root).

## Minor bundle (apply all)

- **vortex m1** — `pending(pid)==1` is the *break* case, not the arm
  predicate: the message is popped before the executor runs
  (`mesh_actor.cpp:468` vs :475-477), so mailbox-empty-at-turn-end ⟺
  `pending==0`; `==1` is the self-send case. Name the sampling point.
- **vortex m2** — restore spec's "never the arena-local `d.dims` rank" guard
  in the filing enumeration (rank-append gated on `dimDims && home`,
  `arena_manifold.cpp:92-103`); names come off each dim cell's interned
  content, `dimensions()` yields `DimRef`s.
- **zigzag m3-2** — phase-6's `Int64(*handleTarget)` arm omits the `+d.name`
  exemption (spec :2394-2396); a literal reading double-writes dim-capture
  records with the forbidden promoted-twin value (:2397-2398).
- **zigzag m3-3** — state whether filing re-runs per fold cycle; note
  genesis re-mints churn the same orphan-per-fold path (the enumeration's
  own base∪`arenaDims_` source shrinks the residual).
- **codebase m1** — "today's unfiled session mints are X" is falsified as
  exhaustive: `dim_cap.cpp:83-85`, `command_transaction.cpp:16-18`,
  `sql_promotion.cpp:140` add seven more `ensureDimension` mints. "are" →
  "include".
- **architect m-2 + vortex n** — "nothing mints `OpHandle` pre-P16" is
  code-false (`mesh_evaluator.cpp:733` mints them today, `Store::makeOpHandle`
  ×6); the deferral's true reason is palette/kind-guard (spec :2430-2432) —
  and the arm must stay kind-guarded post-replay.
- **architect m-3** — (e)'s name-record arm should name its resolvers
  (`symbol_named`'s module-`d.vars` walk for `call`-role, label→head for
  `run`-role, spec :3110).
- **writer m-1** — phase-3's emission contract reserves `+d.name`-on-
  `+d.recipe` to the `dim` field's channel, but (e)'s dim arm consumes two
  record kinds (ref-capture dim records *and* `dim`-field members, spec
  :1964-1967/:2393-2395) — enumerate both.
- **ergo m-1** — the (a)–(e) driver's TU home is unnamed; every other
  artifact names one (spec :3110 puts ceremony in the adapter's executor TU).
- Cite drifts: `arena_manifold.hpp:845-847`→`:844-846` (architect);
  `Makefile:698-699`→`:698` alone (codebase/vortex); `or_parallel.cpp`→
  `:182-185` (zigzag); `store.cpp:3836-3940`→`:3836-4018` under-range
  (codebase); `vagent_compiler_test.cpp:18-46`→`:22-53`; `test/ux-journey-
  plans` prereq `Makefile:1330-1334`; "§6.1's glossary"→§2; "~10"→~25 test
  sites (architect); "user alias" residue at :433; `closed` summary dropped
  the int64-ref-box class; "nineteen cells"→"nineteen names" (ergo n);
  `WorkerContext(base,reader)` → clarify base is the rebuilt fold (ergo n);
  `vagent/prelude/` is forward placement — word it as convention (codebase);
  meta's m1-m6 residual set (prefix productions, `$cap$N`-anchor-on-frame-
  bound interim, splice-only run/graft arms, `static`-in-emitted-body
  ownership, descriptor parameter-rank edge vocabulary, `static`'s
  result-read/`literal()` half); typo "recipients/ deadline" :76.
