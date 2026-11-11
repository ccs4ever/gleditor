# r4-writer — document-coherence critique of `design/vagent-metaprogramming-implementation.md` (v4)

Verdict: **CONVERGED**. Counts: **0 BLOCKING / 0 MAJOR / 1 MINOR / 2 NIT** (one carried).

Every r3 item verified landed — both majors (R3-A, R3-B), my minor + both nits, and the entire
minor bundle including the meta m1–m6 residual set, checked clause-by-clause against v4 and
re-verified against spec v1.19 and the tree. Residual is one under-covering cite in phase 2's new
refusal-edge enumeration plus one near-miss cite range — both coordinate errors, not content
errors — and the standing mdformat wraps. `mdl -i` clean (exit 0); `mdformat --check --wrap 100`
clean (exit 0). Every `file:line` cite touched or added in v4 spot-checked clean — including two
places where the disposition's suggested drift-fix was itself slightly off and v4 kept the correct
coordinate (`or_parallel.cpp:181-184` — the four `ensureDimension` mints are exactly :181-184).

## Per-finding verification (r3 → v4)

### My r3 items — all CLOSED

- **m-1 (two-kind `+d.name` enumeration) — CLOSED** (:236-238). The emission contract now reads
  "`+d.name` on a `+d.recipe` record reserved to the dim-name channel — on `dim`-field members and
  on `+d.refFor`-paired ref-capture dim records alike (spec :1964-1967, :2393-2395)" — the
  suggested wording near verbatim, matching (e)'s consumer enumeration ("ref-capture dim records
  and `dim`-field members alike", :473-475) and the spec's dual usage (:2393-2396 verified
  verbatim: "the `dim`-field member itself, or the slot's `+d.refFor`-paired ref-capture record —
  exempt from this arm").
- **n-1 — CLOSED** (:468). "(e) **`+d.refs` rehydration — last pass; the sweep's three arms, two
  run here**" — the deferral is fronted.
- **n-2 — CLOSED** (:419-421). The fresh `WorkerContext` is now "fresh because the context's
  `arena` pins `base` at construction and the bundle is non-movable; the fresh VM's empty
  `opcodeMap_` is what (b) then re-fills" — re-attach onto the rebuilt fold is the reason, the
  map-refill a downstream consequence. Verified against `worker_context.hpp:52-54` (two-arg
  `const Manifold *` + `SpanReader` ctor, `arena(baseManifold)` at :53, deleted copy/move at
  :57-58).
- **n-3 (carried) — persists, one instance fixed.** The `recipients/ deadline` typo-wrap at old
  :76 is repaired (:77 now `recipients`/`deadline`/`guard`/`recovery` inline). Five mdformat
  slash-wraps remain: `maxExpansionDepth`/ `maxSplices` (:133), `consume`/ `terminate` (:165),
  `+d.refs`/ `+d.recipe` (:234), `d.recipe`/ `d.refFor` (:278), `learn_where`/ `learn_body`
  (:332). mdformat-produced, CI-clean, cosmetic.

### r3 majors — both CLOSED

- **R3-A `splice` call-by-value — CLOSED** (:158-163). "splice substitution via `cloneExpr`
  **call-by-value** (§5.4): each argument binds a fresh `let` local at the expanded body's head,
  evaluated once in argument order — the `{ let p0 := count; p0 + 1 }` shape §9.1 spells — with
  parameter references re-pointed at the synthesized bindings; M13's non-`Code` args marshal
  identically, and the synthesized `let` is the design's only one — `cap$N` remains a pure alias."
  Verified verbatim against spec :904-905 ("binds each argument to a fresh `let` local at the head
  of the expanded body — each argument evaluated once, in argument order") and :2797 ("expands to
  `{ let p0 := count; p0 + 1 }` at expand").
- **R3-B promote-visible class + root — CLOSED** (:421-434). "The **promote-visible mutation
  class**" clause names the full mechanics — content ops only for `isEphemeral` cells, link-writes
  only on `holdsOwn` cells, no content op for shadowed base cells, base-edge removals never
  un-emitted (`arena_manifold.cpp:1867-1870`, `:2020-2053` — both verified) — then prices the
  discipline: counters/scalars mint-and-repoint ("never an in-place `writeValue`, which would
  rewind `patch:<n>` at fold 2 into exactly the ABA the durable bullet forbids"),
  `set_weight`'s `+d.weight` rides it, unlinks ride (d) replay or tombstones. `root` is named:
  the **agent cell** (spec :375, :635-636 — both verified: "checkpoint promotes what is reachable
  from the agent cell" / "promote walks only what the agent cell reaches"), with the two-root
  split stated and resolved (spec :2265-2266 verified verbatim). I traced `promote` end to end:
  the root is exempt from the base-cell traversal skip (`current != root`, :1755), `holdsOwn` =
  has a dense slot in the arena (:270-272), touched base cells slot in and their session-minted
  edges emit — so the mint-and-repoint discipline the plan prescribes is mechanically sufficient,
  and the clause is *correct about the code*, not merely plausible.

### Minor bundle — all landed

`pending(pid)` sampling points — message pops at `mesh_actor.cpp:468` before `executor_` runs at
:473-477, `pending` = `mailbox.size()` (:659-662) — `==0` at turn end vs `==1` as the flag-time
queue-depth test, self-send veto via the empty re-check (:540-544, spec :2165-2169 verified);
"never the arena-local `d.dims` rank" guard restored with the `dimDims && home` gate
(:388-390, `arena_manifold.cpp:92-103` verified — append at :101 gated at :92); phase-6's
`Int64(*handleTarget)` arm exempts "`+d.name`-bearing records the dim arm owns" (:536-537, spec
:2394-2396 verified); "Filing re-enumerates per cycle … before each `rebuildManifold` …
idempotent by name" with the (b)/(e) re-mint coverage (:397-400); "unfiled session mints
**include**" plus the seven new mints — `dim_cap.cpp:83-85` (`d.actor`/`d.ticket`/`d.signature`),
`command_transaction.cpp:16-18` (`d.target`/`d.action`/`d.reply`), `sql_promotion.cpp:140`
(`d.promoted`), all verified line-exact (:405-409); the `OpHandle` deferral rewritten to the true
palette/kind-guard reason — "`OpHandle` cells do exist today, `mesh_evaluator.cpp:733`, just never
in a fragment ref slot", stays kind-guarded post-replay (:481-486 — :733 `setValueBits(…,
OpHandle, …)` and :735 marker precedent both verified); (e)'s name-record arm names its resolvers
(`symbol_named`'s module-`d.vars` walk → `+d.values` → `int64(entryOp)`; label→head →
`int64(head)`, :469-473, spec :3110 verified); the driver's TU home named — `genesis.{hpp,cpp}`
under `vagent/`, glob-picked like `fragment.cpp`, invoked by phase 6's adapter executor TU
(:412-414, spec :3110 verified — "the adapter's own executor TU"); meta's m1–m6 residual set —
prefix productions (`static` `Unary`-prefixed mandatory operand, spec :960-961 verified), the
`$cap$N` single-activation/`bindings_` interim (`compiler.cpp:475-483` — the
`bindings_.find`/`UnknownName` anchor lookup verified at :476-479), splice-only `run`/`graft`
refusal arms (§5.4 :924-934, landed at :172 and :267-268), `static`-in-emitted-body ownership
(spec :968-972 verified), descriptor parameter-rank edge vocabulary (spec :914-917 verified
verbatim), `static`'s result-read/`literal()` half (spec :952-955 verified); plus `run`'s
spill-*and-restore* (spec :262-264 verified) and the `(a)` comparand's `:2290-2292` extension
(prefix-less/`programRef`-nonexistent refusals verified).

Cite drifts all landed: `arena_manifold.hpp:844-846` (:417 — signature spans exactly :844-846),
`Makefile:698` alone (:313, :591 — `VAGENT_RUNTIME_CORE_OBJS` filter-out verified at :698),
`store.cpp:3836-4018` (:436 — `adoptOpRecords` body verified to end exactly at :4018),
`vagent_compiler_test.cpp:22-53` (:211), `Makefile:1330-1334` `test/ux-journey-plans` (:593-594 —
target+prereq verified), "§2 glossary" (:189, spec :158), "~25" `check(` sites (:129, :183 —
`vagent_checker_test.cpp` holds 25 `check(` calls, 20 of exact `check(*program` shape; "~" makes
it honest), "recorded target name (user alias or `learn_method`'s functional lookup name)"
replacing the bare "user alias" (:489-490), `closed` regains the `cells"…"`-path ref-box `int64`
class (:187-188), "nineteen names" (:394), "`WorkerContext` over the rebuilt fold" (:419),
"convention: `vagent/prelude/`" (:253 — `vlace/prelude.hpp` verified to exist). The one cite the
disposition suggested changing that v4 correctly *kept*: `or_parallel.cpp:181-184` — the
suggested `:182-185` would itself have been wrong; the four `ensureDimension` mints sit at
:181-184 exactly.

## New findings (only what is new or still wrong)

### MINOR

- **m-1 — phase 2's refusal-edge enumeration under-cites `spec :464-470`** (:167). The
  parenthetical lists five edges — `recursive-template`, the `return`/`yield`/`consume`/
  `terminate` control-flow bar, the `effect`/`native`/`capacity`/`graft_method` bar, opaque `Code`
  actuals, and `splice c := 3` — under one trailing cite. The cited range covers only the middle
  two (opaque actuals :463-465, effect bar :468-470); `recursive-template` lives at :456-458 and
  the control-flow bar at :459-461, both just outside, and `splice c := 3` — "a captured name
  expands to a `cap$N` *binding*, not a re-assignable slot" — is at :784-786, nowhere near. The
  plan's *statement* of every edge is correct; the coordinate sends an implementer to the wrong
  section for the last-named edge and misses the first two by a few lines. Fix: cite
  `spec :456-470` for the four `Code`-splice-`def` edges and append `:784-786` for the cap$N
  re-assign rule.

### NIT

- **n-1** (:199): the `static` post-run baseline check cites `spec :952-955` — that range covers
  the `result`-cell read / cursor unlink / `literal()` / no-mark sequence (and does contain the
  `compiler.cpp:108-110`-invariant rationale at :955), but the named `choiceDepth() != 0 or
  outstandingMarks() != 0` refusal is :959-960. Extending the cite to :952-960 picks it up.
  Near-miss, not a miss.
- **n-2 (carried):** the five standing mdformat slash-wraps at :133, :165, :234, :278, :332.

## Consistency checks run clean

- **Phase map vs bodies:** all eight rows match their sections; deps linear 0→…→7; phase-5's
  "(a)–(e) driver" matches the lettered body, which now names its `genesis.{hpp,cpp}` home.
- **Post-addition contradictions:** none found. The promote-class clause composes cleanly with
  everything around it — "each cycle promotes them into the fold" is immediately bounded by the
  class, the `d.vars` mint-and-repoint matches phase 4's "the verb **mints `patch:<n>`** off the
  agent cell's `d.vars` counter" (:349) and (d)'s "each install copy's own `reify:<src>:<serial>`
  label still mints off the shared `d.vars` counter" (:458-459), and the ABA rationale points
  exactly at the durable bullet's "never recycled … (no-ABA)" (:487-489). The two-root promote
  ("once per runtime root, or under a harness session root linked to both", :432-434) reads
  consistently against the agent-cell root naming and (d)'s program-side re-enactment — sealed
  program cells ride the prefix, session mints promote, replay re-establishes topology.
- **(e) ↔ phase 6 arm split:** "the sweep's three arms, two run here" (:468) ↔ "the deferred
  ref-record arm … lands here" (:534-536) — the count fronted in (e) matches the phase-6 landing;
  the `+d.name` exemption is correctly scoped to the *writeback* arm while the marker mint stays
  "per ref cell" (:532-534), matching spec P16's "the member still marked" split exactly.
- **Pending rewrite:** internally consistent (pop :468 → `executor_` :473-477 → `==0` at turn
  end, `==1` flag-time sole-queued, self-send veto via end-of-turn empty re-check) and consistent
  with spec :2165-2169's flag-at-`pending==1` rule and `checkpointView`'s non-empty refusal
  (`mesh_actor.cpp:671-674` verified).
- **Requires vs exits:** phase 5's Requires names `promote`/`rebuildManifold`/fresh
  `WorkerContext` and prices the class those run under; its exit covers both run-here arms
  (§9.3 rebind, `dim`-field `Int64(DimRef)`), the (a) comparand refusal, ≥2 re-geneses and the
  orphan-twin test — nothing demanded that the phase's machinery can't produce. Phase 6's
  adapter-gated Requires matches its acceptance-test exit; (iv)'s mesh-testable exception is
  stated identically in Requires and the step text. Phase-6's exit ("learn → checkpoint → resume
  → learn → checkpoint → resume") is a superset of spec P16's stated acceptance (which ends at
  the second checkpoint) — the added final resume is what makes the second restore observable;
  a strengthening, not a misread.
- **Phase-6 sub-order note:** "mint/file → fold → pin/inject → first-resolution" (:512) is the
  suffix of phase-5's "seed → mint/file → fold → pin/inject → first-resolution" (:371) — the
  resume/spawn lineages re-run it behind (i)/(ii); consistent, seed already done.
- **Terminology:** `d.vars` counter, `patch:<n>`, `reify:<src>:<serial>` mint semantics identical
  across phases 3/4/5; "bind set", "emission contract", "promote-visible", "mint-and-repoint",
  "filed-cell rebind" used uniformly; "the dim arm" in phase 6 back-references (e)'s named
  "dim-record arm" without drift.
- **Format gates:** `mdl -i` exit 0; `mdformat --check --wrap 100` exit 0.

## Top-3

1. **m-1** — phase 2's five-edge refusal enumeration cites `spec :464-470`, which covers only
   two of the named edges; `splice c := 3` lives at :784-786 and `recursive-template`/
   control-flow at :456-461. Cite `:456-470` + `:784-786`.
2. **n-1** — the `static` baseline-check cite `spec :952-955` stops four lines short of the
   `choiceDepth`/`outstandingMarks` refusal at :959-960.
3. **n-2 (carried)** — the five standing mdformat slash-wraps; the `recipients/ deadline`
   typo instance is fixed.
