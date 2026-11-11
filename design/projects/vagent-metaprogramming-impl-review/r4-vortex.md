# r4 — Vortex / VM-semantics critic

**Verdict: NOT CONVERGED — one major.** R3-B's promote-visibility clause landed
mechanically correct in every particular (`isEphemeral`-only content ops,
`holdsOwn`-gated link-writes, mint-and-repoint, unlinks-ride-replay) — and my
three r3 minors all landed verbatim-faithful. But the same paragraph's new
*consequence* clause — "the cycle promotes once per runtime root, or under a
harness session root linked to both" — over-extends `promote`'s root past the
spec's agent-cell-only checkpoint model, inverting the load-bearing
program-side pruning that makes bare grafts die and `learn_*` records matter.

Counts: **0 BLOCKING / 1 MAJOR / 0 MINOR / 1 NIT**

Lens unchanged: does the phase order respect VM semantics, and are the
phase-5/6 mechanics constructible on today's tree? Plan refs are
`design/vagent-metaprogramming-implementation.md` v4; spec refs are
`design/vagent-metaprogramming.md` v1.19. All code cites below re-verified
today.

---

## M1 — "promotes once per runtime root" is spec-wrong: the checkpoint roots at the agent cell alone

**Plan :429-434.** "`root` is the **agent cell** (spec :375, :635-636 — program
cells never republish; they ride the folded prefix and (d) re-enacts), and
session mutations span two disjoint roots — `program()`/`agent_cell()` are
distinct adapter-bound roots never `+d`-reached from each other (spec
:2265-2266) — **so the cycle promotes once per runtime root, or under a harness
session root linked to both**". The first clause is right; the bolded
consequence is not.

**What the spec actually fixes.** The checkpoint's promote root is the agent
cell, alone, and the program-side boundary pruning is stated as *load-bearing*:

- spec :375-377 (M11): "the checkpoint promotes what is reachable from the
  agent cell … program cells are therefore never *republished* — they ride the
  shared base lineage the checkpoint's spool prefixes onto — so **a session
  graft is gone at `resume`**." The graft/learn distinction *is* this fact:
  `learn` = graft **+ a `d.patch` record** — the record is what confers
  durability, and it hangs off the agent's `d.patch` rank.
- spec :635-636: "`promote` walks only what the agent cell reaches, so the
  skeleton never enters the promoted set — that is V12 working as intended."
- spec :1791-1799, the decisive text: "'State-side' is link-reachable *from
  the agent cell* — in either direction, as `promote` walks — **without
  transiting a program-side cell** … **The pruning is load-bearing**: memo
  records, reflect wrappers, **installed `learn` copies** and capture-linked
  binding cells sit behind program-side anchors … **and must not promote under
  the state banner — they die with the incarnation at `resume`**."
- Nothing durable hangs program-side: weights live "only on a junction's
  `d.weight` or on `d.vars` records of the *agent* — never … directly on a
  clause cell" (spec :2647-2652); junctions/labels/weights are "reachable from
  the agent cell, so they promote" (spec :2684).

**What a program-root or spanning-root promote does instead.** The reachability
walk expands only `isEphemeral` and `holdsOwn` (shadowed) cells
(`arena_manifold.cpp:1755`), so promoting `program()` carries exactly the
*modified* program subgraph: shadowed anchors' link-writes emit
(`holdsOwn` gate, `:2021`) and every ephemeral cell behind them mints. That is
precisely the forbidden class:

- **Bare grafts go durable** — an unrecorded `graft_*` install is
  program-anchored session state; under program-root promote it persists,
  inverting M11's "gone at `resume`" and making the `d.patch` record
  decorative.
- **Asymmetric half-durability** — graft-*adds* persist while prune-*removes*
  never do (a removed base edge is never un-emitted, the very fact the plan
  states two clauses earlier). The program side becomes add-only durable —
  worse than either consistent pole.
- **`learn_*` installs double-carried** — once via promote (forbidden per
  spec :1797-1799), once via (d) replay's re-materialization each genesis; the
  dedupe clause churns identities rather than the clean replay-only model.

**The miscite.** spec :2265-2266's two-roots vocabulary belongs to (a)'s
sideness-classification walk ("named `>E_ops` legs … enter this walk"),
corroborated by spec :2258 ("the walk rooted at the adapter-recorded program
root" — the comparand's second *walk*, not a second *promote*) and :1627-1632
("the agent cell's own legs … are enumerated only by a second walk rooted at
`agent_cell()`"). It is a fact about what the ≤E_ops classifier enumerates,
not a license to promote the program root.

**Note on the ruling applied.** R3-B's paired architect-m-1 offered "(the
agent cell per spec :375/:635-636, or a spanning session root)". The plan took
the second arm — but the option was premised on "session mutations span two
roots", which is true of the classification walk and false of durable-state
reachability. As a VM-semantics statement the clause stands wrong regardless
of provenance: program-side session mutation persists *only* as (d) replay —
that asymmetry is the design, not an oversight the harness must route around.

**Fix.** Strike the per-root/spanning alternative and state the boundary as
the mechanism it is: "`root` is the agent cell (spec :375, :635-636); the
program-side boundary pruning is the durability model — bare grafts and prunes
die at the fold, `learn_*` installs re-materialize through (d) replay, and
nothing durable hangs program-side (weights/junctions/records all ride the
agent's own ranks, spec :2647-2652, :2684, :1791-1799). The two-roots
vocabulary (spec :2265-2266) is (a)'s classification walk, not a second
promote."

---

## Verified — R3-B's promote-visibility mechanics: stated correctly

Plan :421-429, each claim against today's tree:

- **"`promote` emits content ops only for `isEphemeral` cells"**
  (`arena_manifold.cpp:1867-1870`) — exact: non-ephemeral cells hit
  `real.emplace(arena, arena); continue;` inside the `mintOrder` loop. The
  only `setValue` emissions are the cyclic-`OpHandle` deferred arm
  (:1984-2003), which is gated `isEphemeral` at :1799. A shadowed base cell
  gets no content op — correct.
- **"link-writes on `holdsOwn` cells"** (`:2020-2053`) — exact: the loop is
  `order`-driven with `if (!from.holdsOwn(arena) || from.isProxy(arena))
  continue` at :2021; `holdsOwn` = `denseOf(ref).has_value()` (:270-272) =
  ephemeral ∪ shadowed-base (a `link()` shadows its endpoints, :1338-1342).
  Posward-side only (:2027), `real`-translated endpoints, and the
  base-identical dedup at :2035-2039 (all three of arena/dim/pos
  non-ephemeral + `base()->linked` match → skip). Accurate compression.
- **"a removed base edge is never un-emitted"** — verified structurally:
  `unlink` exists only as arena sugar over `link(..., nullopt)`
  (`arena_manifold.hpp:616`); `Store` has no unlink op at all; a removed edge
  is absent from `dimensionsOf` and emits nothing. Corollary the plan's
  mint-and-repoint form silently relies on and gets right: a *re-point*
  persists because the emitted `setLink` op displaces the stale edge at fold
  time (`Manifold::apply` SetLink clears `displacedUs`/`displacedIt`,
  `manifold.cpp:509-518`) — the old edge's severance rides the new op, never
  an unlink.
- **"the `d.vars` serial counter bumps a *fresh ephemeral* the rank repoints
  at, never an in-place `writeValue`, which would rewind `patch:<n>` at fold
  2"** — mechanically exact: post-fold the counter is a base cell; an
  in-place bump shadows content that no content op carries, so the fold
  serves the stale value and the next mint reuses the serial — the ABA the
  durable bullet forbids. (In-place writes on a *still-ephemeral* counter
  fold fine — the blanket "never" is the safe discipline, not a literal
  necessity pre-first-fold; correct as stated for the durable class.)
- **"`set_weight`'s `+d.weight` value writes ride the same discipline"** —
  consistent: the scalar cells sit agent-side (spec :2647-2652), reachable
  from the promote root, so mint-and-repoint carries them.
- **"base-edge unlinks (`learn_plan`'s supersede, `unlearn`'s record drop)
  survive only via (d) replay or tombstone-form records"** — consistent with
  both the op set (no unlink) and the spec's replay channel.
- **"into `arena.base()->store()` — the publish-destination shape"** — right:
  `base()` → `const Manifold*` (`arena_manifold.hpp:268`), `store()` →
  non-const `Store*` (`manifold.hpp:486`), `rebuildManifold` sets `store_`
  on the fold it returns (`store.cpp:511`,`:532`), so the base's `->store()`
  is the kept lineage `Store` — matching spec :3110's "the verify destination
  is the received-band store, `arena.base()->store()`".
- **"anything unreached drops silently"** — true: `promote` refuses only on
  budget/unknown-root/handle-target failures; unreached session cells simply
  mint no ops (`:1744-1788`).
- **`arena_manifold.hpp:844-846`** for the `promote` signature — drift-fixed,
  correct.

## Verified — my three r3 minors: all landed correctly

- **m1 (`pending` predicate) — landed.** Plan :539-544 now names both sample
  points: "mailbox-empty-at-turn-end is `pending(pid)==0` sampled inside the
  callback; `pending(pid)==1` is the *flag-time* queue-depth test — flagged
  message sole-queued — with the end-of-turn empty re-check the veto a
  self-send trips". Verified: `mailbox.pop_front()` at `mesh_actor.cpp:468`,
  the `executor_` call at :474-476 (cited :473-477), `pending` =
  `mailbox.size()` at :659-662. This is also the spec's own convention —
  spec :3110's P16 row says "`pending(pid)==1` arms the window" (flag-time)
  and spec :2165-2169 "whose end leaves the mailbox empty" (turn-end veto).
- **m2 (`d.dims`-rank guard) — landed.** Plan :386-390: "enumerated off
  `dimensions()`' base∪`arenaDims_` surface — names read off each dim cell's
  interned content, `dimensions()` yielding `DimRef`s
  (`arena_manifold.cpp:34-46`, `:87-88`) — *never* the arena-local `d.dims`
  rank, which appends only under a resolvable `home` (`:92-103`)". Verified:
  `dimensions()` at :34-46 returns `std::vector<DimRef>` over base∪
  `arenaDims_`; the name is the dim cell's interned content
  (`intern(name)`/`makeCell(span)` at :87-88); the rank append is gated
  `dimDims && home` at :91-103.
- **m3 (fresh-`WorkerContext` causality) — landed.** Plan :419-421: "fresh
  because the context's `arena` pins `base` at construction and the bundle is
  non-movable; the fresh VM's empty `opcodeMap_` is what (b) then re-fills".
  Verified: `arena(baseManifold)` in the ctor init list
  (`worker_context.hpp:53-55`), copy/move deleted :57-60. The causality is
  now the right way round.

## Verified — the rest of the r3 bundle in my territory

- **zigzag m3-2** (:535-537): the ref arm now reads "per-ref-record
  `Int64(*handleTarget)` writebacks *minus the `+d.name`-bearing records the
  dim arm owns* (spec :2394-2396 — a literal per-record reading would
  double-write them with the forbidden promoted twin)" — matches spec
  :2391-2398 verbatim (`dim`-field member and `+d.refFor`-paired dim record
  "exempt from this arm", "never the promoted twin `*handleTarget` names").
- **zigzag m3-3** (:397-400): "Filing re-enumerates per cycle … filing only
  what the rank lacks … each `makeDimension` is a permanent rank append, so
  the sweep is idempotent by name" — mechanically sound: the persistent
  `d.dims` rank is append-only (`Store::makeDimension`'s tail-append sugar,
  `store.cpp:1050`/`:1088`), so a per-cycle sweep files last cycle's (b)/(e)
  re-mints before they can orphan again.
- **codebase m1** (:404-409): "today's unfiled session mints include …" —
  all seven new cites verified: `dim_cap.cpp:83-85`
  (`d.actor`/`d.ticket`/`d.signature`), `command_transaction.cpp:16-18`
  (`d.target`/`d.action`/`d.reply`), `sql_promotion.cpp:140` (`d.promoted`).
- **architect m-2 + my n1(iii)** (:481-486): the ref arm's no-op reason is
  now scoped correctly — "emission writes `Int64` ref slots and `reify`'s
  `New`/`Link`/`Value` palette cannot mint handle-kind cells … (`OpHandle`
  cells do exist today, `mesh_evaluator.cpp:733`, just never in a fragment
  ref slot); it must *stay* kind-guarded post-replay" — verified
  (`setValueBits(OpHandle)` at :733-735; `writeValue`'s four arms at
  `vortex_core.cpp:196-229`).
- **architect m-3** (:469-473): both resolvers named — `symbol_named`'s
  module-`d.vars` walk → `+d.values` → op-or-head hop → `int64(entryOp)` for
  `call`-role, program-image label→head lookup → `int64(head)` for `run`-role
  — verbatim against spec :3110's P16 row.
- **writer m-1** (:236-238 + :473-475): both record kinds enumerated —
  "`dim`-field members and on `+d.refFor`-paired ref-capture dim records
  alike (spec :1964-1967, :2393-2395)".
- **ergo m-1** (:412-415): driver TU named — `genesis.{hpp,cpp}` under
  `vagent/`, "phase 6's adapter executor TU invokes the same artifact"
  (spec :3110).
- **Cite drifts**: `arena_manifold.hpp:844-846` ✓; `Makefile:698` ✓ (the
  `VAGENT_RUNTIME_CORE_OBJS` filter-out line itself);
  `store.cpp:3836-4018` ✓ (`adoptOpRecords`'s exact extent);
  `vagent_compiler_test.cpp:22-53` ✓; `test/ux-journey-plans`
  `Makefile:1330-1334` ✓; §2 glossary/spec :158 ✓; ~25 sites (:129, :183) ✓;
  "nineteen names" (:394) ✓ — 17 `mintNamedDimension` calls at
  `vortex_core.cpp:50-69` + `home` :35 + `d.dims` :44; "WorkerContext …
  over the rebuilt fold" (:419) ✓; `vagent/prelude/` as "convention:" (:253)
  ✓; `recipients`/`deadline` typo fixed (:77) ✓; closed's int64-ref-box
  class restored (:187-189) ✓; `or_parallel.cpp:181-184` — **plan kept the
  right range** (`dimOr_` :181 … `dimSubmission_` :184); the disposition's
  suggested :182-185 was itself off by one.
- **meta residuals in my check-zone**: splice call-by-value (R3-A) at
  :157-163 — `let`-per-arg, once-only, in-order, verbatim vs §5.4/:924-934;
  `static(param)`-in-emitted-body emission error (:231-233); `$cap$N`
  anchor-on-frame-bound interim named (:292-294, `compiler.cpp:475-483`'s
  `bindings_` lookup verified); splice-only `run`/`graft` refusal + evaluated-
  position bar (:267-268, :170-173 vs spec :924-934); `static`
  `Unary`-mandatory-operand (:104-105 vs spec :960-961); parameter-rank
  `+d.step`-off-`+d.grab` edge vocabulary (:224-226); `static`'s result-read
  + `literal()` + `choiceDepth`/`outstandingMarks` baseline
  (:196-199, `compiler.cpp:108-110` verified) and `staticFuel`→
  `ScalarCompilerLimits` (:200-201, `compiler.hpp:23-26` verified).

## New NIT

- **n1 — `arena_manifold.hpp:837-846` under-ranges the cited claim**
  (:433-434). "promote folds only its `root`'s reachable subgraph" is the
  `@param root` doc text at :833-836 ("Only what is reachable from it along
  any dimension is promoted"); the cited :837-846 opens on the `@return`
  clause and ends on the signature. Cite :833-846 — and the paired spec cite
  :635-636 is one line late on the same text ("Program cells do not
  *republish*: `promote` walks only what the agent / cell reaches" spans
  :634-635).

---

## Spot-check status

Newly verified for v4: `arena_manifold.cpp:1755` (walk expands only
ephemeral ∪ `holdsOwn` — the precise reach of a program-root promote),
`:1782-1783` (both-direction `discover`), `:1867-1870`, `:1984-2003`,
`:2020-2053` (incl. :2035-2039 dedup), `:34-46`, `:78-105`, `:270-272`,
`:1322-1379` (link shadows both ends + displaced neighbors);
`manifold.cpp:462-527` (fold-time SetLink displacement :509-518 — why a
repoint persists where an unlink cannot); `arena_manifold.hpp:616`
(unlink = arena sugar only; no `Store` unlink exists), `:833-846`;
`mesh_actor.cpp:468`,`:474-476`,`:659-662`,`:270-273`;
`mesh_actor.hpp:81`; `worker_context.hpp:53-60`;
`vortex_core.cpp:35`,`:44`,`:50-69`; `mesh_value.cpp:473`;
`or_parallel.cpp:181-184`; `dim_cap.cpp:83-85`;
`command_transaction.cpp:16-18`; `sql_promotion.cpp:140`;
`mesh_evaluator.cpp:733-735`; `compiler.cpp:108-110`,`:160-181`,`:475-483`;
`compiler.hpp:15-18`,`:23-26`; `store.cpp:3836-4018`,`:1050`,`:1088`;
`vortex_vm.cpp:98`; `Makefile:695`,`:698`,`:1330-1334`,`:1348-1351`; spec
:158, :259-264, :375-377, :633-636, :924-934, :960-961, :1053-1060,
:1141-1144, :1172, :1174-1176, :1295-1304, :1627-1632, :1791-1799,
:1848-1858, :2165-2183, :2228-2266, :2346-2354, :2364-2372, :2391-2406,
:2500-2509, :2647-2652, :2684-2687, :2905-2907, :3110 (P16 row verbatim).
