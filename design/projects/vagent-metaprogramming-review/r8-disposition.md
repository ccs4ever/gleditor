# Round-8 disposition — orchestrator rulings on v1.7 findings

Six lanes converged in substance (vortex, zigzag, codebase, ergo, meta, writer-at-minor-level);
the lineage chain verified end-to-end (`job.base` → `base->store` `mesh_jobs.cpp:304-318`;
`sealLocalSpool` `opsAlreadySealed=0` seals `[1..opCount]` `publication.cpp:611-617`/
`mesh_value.cpp:227-228`; `birthOp=opIndex` `manifold.cpp:434`). **0 blocking, 1 major, ~13
minors + nits** — all in the lineage cluster's newest text.

## Major

**R8-A — the publish-store invariant names an object the API doesn't expose (architect).
Adopted.** `spawn()` creates/accepts no store (`mesh_actor.cpp:156`); `resume()` builds it
internally and keeps it private in `Actor` (`:260-269`); `checkpointView`/`Executor` expose only
the arena — so "publishValue runs against the store backing the arena's base manifold" references
nothing the adapter can reach, and today's only publish path (fresh `Store`,
`mesh_actor_resume_test.cpp:125`) is exactly the doc's fail-closed arm. **Ruling:** name the
adapter's store ownership explicitly — the adapter constructs and *keeps* the publish `Store`
itself, seeded with the artifact's ops segment at spawn-time (§10.1's seeding step owns this);
at `resume` the adapter supplies the same lineage store to the replay path (the owed adapter API
surface — `resume` must take or verify the lineage store, a named seam change like P4's wake,
not silently assumed). Fresh-store publication is then the fail-closed arm *by detection*, not
by construction. Companion minors folded in: the artifact's seedable ops-segment form and owner
are named ("byte-identical" → position-stable, since scrollIds remap); spec §4.4's arena-load
wording is marked superseded-by-lineage.

## Minor batch — all adopted

- **`plan_named`/`symbol_named` `run` operands are dead text** (vortex-m, meta-2, ergo R8-F1 —
  three lanes): both builtins return `Opt<Cell>`, `run` demands `Code<T>`, and `Cell`→`Code` is
  barred. Drop the qualified-name `run`-operand arm entirely — `run` operands are literal or
  `code_named`-sourced only; a `run` of a `plan_named`/`symbol_named` result is the headless-`run`
  refusal class (session-side) and its record arm is removed, not rebinding-fixed.
- **`patch:<n>` doesn't fit the qualified-name descriptor shape** (ergo R8-F2, writer R8-3):
  patch targets name a *clause*, stored verbatim on clause `d.name` — the descriptor's
  `<module>:<Agent>.<member>` convention gains a clause-target arm (`patch:<n>` verbatim).
  Anonymous kernel handlers (spec V15) hit the anonymous-target refusal — state the gap.
- **Genesis (a) positional assertion** (zigzag-m1): the high-water check is stated positionally —
  every `program()`-walk cell must test `≤ E` and the walk-max must equal `E`; a merged/re-seeded
  lineage (`mergeValues`→fresh store) fails closed, not just absent-prefix.
- **(e) sweep traversal** (zigzag-m2): (d) hands (e) its install set — the sweep covers the
  `learn`-registered heads plus the copies replay just installed, named explicitly.
- **Nested-literal label direction** (meta-3): base `src:` is the *outermost* site; `+o` arms
  descend into nesting.
- **`OpHandle`-kind copy impossible** (meta-1): `reify`'s `New`/`Link`/`Value` replay can't mint
  `OpHandle` cells — copies' ref slots self-rehydrate via inherited content; fix the phrase.
- **Writer minors** (R8-1..4): step-ref mislabel at :1571; `patch:` out of "emitter's automatic
  prefixes" (it stays clause-label); descriptor qualified-name shape gains the clause arm;
  "symbols are content-named, no `+d.name`" — fix the "qualified symbol names on the same dim"
  phrasing.
- **Nit bundles**: `mailbox.front()`/`pop_front()` dequeue cite (`:467-468`); `sealLocalSpool`
  cite gains `mesh_value.cpp:227-228`,`:241`; `getOpcodeKind` rationale → §6.5's write-guard;
  `home` walk direction; changelog `:1952-1954` narrowing; "name-record sweep" shorthand;
  copy-inheritance "two reasons"; `(link eviction, §4.3)` xref; `Call`+spill at :2031 — apply all.

## Round status

v1.8 to follow. The lineage cluster has now been probed three levels deep and each fix held;
remaining majors are locus refinements, not new architecture.
