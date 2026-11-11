# r4 — system-architecture review (phase ordering and deliverability)

**Verdict:** NOT CONVERGED — one major inside the new R3-B text. The
promote-visible-class half landed code-perfect; the `root` half prescribes a
program-side fold the spec's dedupe cannot see, and the fix is again one
clause. Trajectory holds: r1 ~12 structural → r2 2 → r3 2 → r4 1.

**Counts:** 0 BLOCKING / 1 MAJOR / 2 MINOR / 2 NIT

Lens unchanged. Plan under review: `design/vagent-metaprogramming-implementation.md` v4.
Spec refs `design/vagent-metaprogramming.md` v1.19.

## Directed verification — R3-A (`splice` call-by-value marshalling): **CLOSED**

Landed verbatim at plan :159-163: "each argument binds a fresh `let` local at
the expanded body's head, evaluated once in argument order — the
`{ let p0 := count; p0 + 1 }` shape §9.1 spells — with parameter references
re-pointed at the synthesized bindings; M13's non-`Code` args marshal
identically, and the synthesized `let` is the design's only one — `cap$N`
remains a pure alias". Spec verified: :903-908 (call-by-value, fresh `let`
locals, "each argument evaluated once, in argument order", M13's non-`Code`
arguments "through fresh `let` locals at the inlined body's head" while `Code`
arguments stay statically-known), :2797 (the `p0` shape verbatim in §9.1),
:839-840 ("`cap$N` is a **pure binding-table alias, not a synthesized
`let`**" — so the synthesized `let` *is* the design's only one), :930-932
(dead-`let` erasure, which the same bullet prices). The ruling's three named
facts — marshalling, once-only, order — plus the impure-actual multi-eval
rationale are all present and correct.

## Directed verification — R3-B (promote-visible class + root): **MAJOR DEFECT in the root half**

The visibility half (plan :421-429) is code-exact:

- "content ops only for `isEphemeral` cells" — `arena_manifold.cpp:1867-1870`:
  `if (!isEphemeral(arena)) { real.emplace(arena, arena); continue; }` — a
  shadowed base cell maps to itself and mints nothing. ✓
- "link-writes on `holdsOwn` cells" — `:2021`:
  `if (!from.holdsOwn(arena) || from.isProxy(arena)) continue;` —
  `holdsOwn` = has a `slots_` entry = ephemeral or shadowed (`:270-272`);
  `:2035-2039` skips edges the base already carries, so only *changed* shadow
  edges emit. ✓
- "a removed base edge is never un-emitted" — the `:2020-2053` loop walks the
  arena's *current* `dimensionsOf` and only ever calls `setLink`; a removed
  edge is simply unvisited; there is no unlink emission. ✓
- Mint-and-repoint for the `d.vars` counter (fresh ephemeral + repoint, never
  in-place `writeValue` → fold-2 `patch:<n>` ABA) and `set_weight`'s
  `+d.weight` writes; removals ride "(d) replay or tombstone-form records" —
  the spec's own form for `unlearn`'s drop ("dead patches are dropped after
  recording a `ConditionFault`-style fault", spec :1848). Consistent. ✓
- `root` named: "**agent cell** (spec :375, :635-636)" — verified: :375-376
  "the checkpoint promotes what is reachable from the agent cell"; :635-636
  "`promote` walks only what the agent cell reaches". ✓

But the conclusion the paragraph draws is wrong — see M-1.

## Minor bundle — applied, spot-verified

`pending(pid)==0` sampled inside the callback vs `==1` flag-time sole-queued
(:540-544; `pop_front` at `mesh_actor.cpp:468` precedes the `executor_` call
at `:474-476`, spec :2164-2165/:3110) ✓; the `d.dims`-rank guard restored with
the `home`-gated append cite (:388-390; `arena_manifold.cpp:92-103` ✓,
`dimensions()` base∪`arenaDims_` at `:34-46` ✓, interned name at `:87-88` ✓);
phase-6 ref arm's `+d.name` exemption (:535-538; spec :2390-2396 verbatim —
the dim arm owns both record kinds) ✓; filing re-enumerates per cycle
(:397-400) ✓; "are"→"include" plus the seven added mints (:405-409 —
`dim_cap.cpp:83-85` `d.actor`/`d.ticket`/`d.signature` ✓,
`command_transaction.cpp:16-18` `d.target`/`d.action`/`d.reply` ✓,
`sql_promotion.cpp:140` `d.promoted` ✓); writer's two `+d.name`-bearing record
kinds (:236-238; spec :1964-1967/:2393-2395) ✓; driver home
`genesis.{hpp,cpp}` under `vagent/` (:412-414; spec :3110 "the adapter's own
executor TU" verbatim) ✓. Cite fixes verified: `arena_manifold.hpp:844-846`
(signature at :844-846, reachable-subgraph contract at :833-838) ✓;
`Makefile:698` alone ✓; `:2290-2292` added to the (a) cite ✓;
`store.cpp:3836-4018` (`adoptOpRecords` spans :3836-4018 exactly) ✓;
`vagent_compiler_test.cpp:22-53`, `Makefile:1330-1334`
(`test/ux-journey-plans` prereq) ✓; §2 glossary, ~25 sites ×2, "nineteen
names" (verified: 17 `mintNamedDimension` calls `d.grab`…`d.role` + `home`
:35 + `d.dims` :44 = 19, `vortex_core.cpp:50-69`/:35/:44) ✓;
WorkerContext-over-rebuilt-fold, `vlace/prelude.hpp` sibling-file precedent
(exists, `apps/common/xanadu/vlace/prelude.hpp`), "functional lookup name",
the `int64` ref-box class in `closed`, and the trailing `→ resume`
(:553-554) — all landed. The meta residual set is in: prefix productions
(:103-105, spec :960-961), the `$cap$N`-anchor interim (:293-294), splice-only
`run`/`graft` (:267-268, spec :928-929), `static`-in-emitted-body ownership
(:231-233, spec :968-972), descriptor `+d.step`-off-`+d.grab` vocabulary
(:225-226, spec :914-916), `static`'s result-read/`literal()` half (:197-199,
spec :952-955). **Note:** the plan kept `or_parallel.cpp:181-184` and was
right to — the four `ensureDimension` mints are exactly :181-184; the
disposition's ":182-185" drift note was itself off by one.

My r3 items: m-2 closed (palette/kind-guard rationale verbatim at :481-486,
spec :2430-2432; the `OpHandle`-exists parenthetical now honest at
`mesh_evaluator.cpp:733`); m-3 closed (both resolvers named at :470-473 —
`symbol_named`'s module-`d.vars` walk → `+d.values` → op-or-head hop →
`int64(entryOp)` for `call`-role, program-image label→head → `int64(head)`
for `run`-role, spec :3110 verbatim); all nine nits applied.

Re-verified beyond the delta: `dimension_registry.cpp:194-217`
(getOrCreate's rebuilt fold = the injected base), `pending`/`checkpointView`
(`mesh_actor.cpp:659-662`/`:672`), the twelve apparatus dims and thirteen V10
dims (spec :1055-1057/:3021-3022), `compiler.cpp:769-772` (the `dimConstant`
lambda), `compiler.cpp:885-891` (`localReferences_` keyed `const Expr *` —
the spec's own cite), `vortex_vm.cpp` funnel sites, `worker_context.hpp:52-54`,
`publication.cpp:1093-1103`/`:1129-1131`, `vortex_core.cpp:14`/`:29-70`.
Phase ordering still has no used-before-built; every exit is checkable in the
harness its Requires names.

---

## MAJOR

### M-1 — "promotes once per runtime root, or under a harness session root linked to both" folds the installed copies the spec says never fold

**Plan :430-434; spec :1344, :635-636, :1310-1315, :1323-1324, :2265-2266.**

The clause correctly states `root` is the agent cell and that the two roots
are never `+d`-reached — then concludes "so the cycle promotes once per
runtime root, or under a harness session root linked to both". Either reading
folds the program-side reachable subgraph, and that subgraph contains the
**replay-installed copies** (`d.clause`-attach clause copies, module-`d.vars`
method copies — reached by the `program()` walk, spec :2261). Folding them is
wrong twice over:

1. **The spec forbids it.** "grafts never fold back" (spec :1344) is the
   load-bearing premise of the whole (d)-replay/dedupe design — installed
   copies are program-side precisely so the agent-cell-rooted `promote`
   (:635-636) never reaches them; their durability channel is (d) replay
   *by construction* — that is what the `d.patch` records are for.
2. **Dedupe cannot clean it up.** At the next genesis the replayed
   `learn_plan` re-matches the recorded *alias* verbatim against clause
   `+d.name`s (:1338-1340) — and `patch:` is refused as a `name` argument
   (:1313-1315), so a folded `patch:<n>`-labelled copy is **unreachable to
   the match** (:1323-1324). The replay displaces the declared victim again
   and installs a fresh twin beside the folded one: one extra live
   `patch:<n>` clause per re-genesis — double firing at selection,
   `unlearn`'s label-precise twin pick gone ambiguous (:1356-1357), and the
   durable-semantics bullet's own invariants broken. (`learn_method` alone
   escapes — its dedupe is content-name-keyed, :1317-1318 — which is why the
   hazard is specific to the clause-installing verbs.) Session `graft_*`
   copies would wrongly persist too, corrupting the session-vs-durable
   boundary the tests measure.

The two-root split (spec :2265-2266) is *why* program-side state cannot ride
`promote` — not a second promote target. The only program-rooted `promote`
with a legitimate role is the **one-time initial-image seed**, which the plan
already covers separately at :435-436 (`promote` of the compile arena or
`adoptOpRecords`), before any installed copy exists.

**Fix:** replace the conclusion — keep "`root` is the agent cell" and the
two-root fact, then: "so the cycle's `promote` is agent-cell-rooted only;
program-side session mutations persist *through (d) replay* by design
(grafts never fold back, spec :1344/:635-636) — promoting the program root,
or a spanning session root, would fold installed copies beside their replayed
twins, and `patch:<n>` labels are unreachable to the alias-keyed dedupe
(spec :1310-1324)". The disposition's "spanning session root" alternative was
under-analyzed; the plan picked the harmful option.

## MINOR

### m-1 — M13 refusal-edge cite under-ranges and misses one arm

**Plan :166-167.** "(its refusal edges: the named `recursive-template`
`DiagnosticCode`, the control-flow bar … the `effect`/`native`/`capacity`/
`graft_method` bar, opaque `Code` actuals, and `splice c := 3` — cap$N is not
re-assignable, spec :464-470)". The cite covers the opaque-actuals and
membership bars (:463-470) but `recursive-template` is :456-459 and the
control-flow bar :460-462 — both outside — and `splice c := 3`/the
non-re-assignable `cap$N` slot is :784-786, entirely elsewhere. Fix:
"spec :456-472, :784-786".

### m-2 — "the rank repoints at" misnames the repoint edge

**Plan :425-426.** "`d.vars` scalar fields" are keyed
`+d.vars → [key] +d.values → scalar` records (spec :1300, §8.1); what
repoints is the *record's* `+d.values` edge — the rank membership is
untouched. "the `d.vars` serial counter bumps a fresh ephemeral the record's
`+d.values` edge repoints at" (or "whose holding record repoints at").

## NIT

- **n-1 — `mesh_actor.cpp:473-477` over-ranges the `executor_` call**
  (:541). The invocation is :474-476 inside the :473-479 `try`.
- **n-2 — "(d) re-enacts" ellipsis** (:430) — "they ride the folded prefix
  and (d) re-enacts" parses only by charity; "(d) re-enactment carries them".

## Cross-cutting note for the disposition

The R3-B mechanics landed correctly — promote's emit classes are stated
exactly where the code gates them, and every durable mutation kind got the
right channel. The defect is the last half-sentence, where the ruling's own
"or a spanning session root" alternative was taken at face value; under the
spec's model it folds installed copies no dedupe can see. One clause —
agent-cell-rooted promote, program-side persistence owned by (d) — closes
it. Everything else in v4 verified clean against spec and code; with M-1
applied the plan is implementable end-to-end.
