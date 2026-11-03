# r4 — codebase-completeness critic (inventory coverage and citation truth)

**Verdict: CONVERGED** (R3-A and R3-B landed with every new cite verified against code and spec;
my r3 MINOR landed in full — "are"→"include" plus all seven appended cites, each exact; all r3
nits applied; one new MINOR: the `splice c := 3` refusal edge's spec cite points at :464-470 but
the rule lives at :785)

**Counts:** 0 BLOCKING / 0 MAJOR / 1 MINOR / 5 NIT

Scope of this pass: did R3-A/R3-B land correctly; did my r3 m1 and the minor bundle land; do
v4's *new* `file:line` and `spec :line` cites land; does any v4 delta misstate the code.

## Disposition of R3 majors — both landed

- **R3-A (`splice` call-by-value marshalling).** Phase 2's expand bullet now states it
  verbatim: "each argument binds a fresh `let` local at the expanded body's head, evaluated
  once in argument order — the `{ let p0 := count; p0 + 1 }` shape §9.1 spells — with
  parameter references re-pointed at the synthesized bindings; M13's non-`Code` args marshal
  identically, and the synthesized `let` is the design's only one — `cap$N` remains a pure
  alias (§5.3 — textual substitution would yield observable multi-eval on impure actuals)".
  Verified: spec §5.4 :903-907 is the same text ("binds each argument to a fresh `let` local
  at the head of the expanded body — each argument evaluated once, in argument order");
  §9.1's `let p0 := count; p0 + 1` is spec :2797 verbatim; cap$N-as-alias-not-let at spec
  :219/:840/:3405. All three required clauses (let-per-arg, once-only, argument order)
  present. ✓
- **R3-B (promote-visible class + `root`).** The re-genesis loop now bounds the cycle:
  "`promote` emits content ops only for `isEphemeral` cells and link-writes on `holdsOwn`
  cells — a shadowed base cell gets no content op, and a removed base edge is never
  un-emitted (`arena_manifold.cpp:1867-1870`, `:2020-2053`)" — verified: the
  `!isEphemeral → map-to-self, mint nothing` arm is :1867-1870 exact (doc comment
  :1864-1866 says it outright); the link loop gates on `!from.holdsOwn(arena)` at :2021 and
  only `setLink`s edges present in the overlay — additive only, nothing un-emits a removed
  base edge (:2020-2053 exact). Mint-and-repoint counters ("the `d.vars` serial counter
  bumps a *fresh ephemeral* ... never an in-place `writeValue`, which would rewind
  `patch:<n>` at fold 2 into exactly the ABA the durable bullet forbids"),
  (d)-replay/tombstone unlinks, `root` = **agent cell** (spec :375 "the checkpoint promotes
  what is reachable from the agent cell", :635-636 "`promote` walks only what the agent cell
  reaches"), two disjoint roots (spec :2265-2266 "distinct adapter-bound roots never
  `+d`-reached from each other" verbatim), once-per-root or session-root promote
  (`arena_manifold.hpp:837-846` — see n2). Architect's m-1 pairing landed. ✓

## My r3 MINOR and nits — all landed

- **r3-m1 (session-mint enumeration).** "are" → "include" and all seven appended with exact
  cites: `dim_cap.cpp:83-85` (`d.actor`/`d.ticket`/`d.signature` at :83/:84/:85 through
  `arena.ensureDimension`), `command_transaction.cpp:16-18` (`d.target`/`d.action`/`d.reply`
  at :16/:17/:18 through `manifold.ensureDimension`), `sql_promotion.cpp:140`
  (`d.promoted` through `staging_.ensureDimension`). The full twelve-member class is now
  enumerated. ✓
- **Nits:** `vagent_compiler_test.cpp:22-53` (ScalarHarness :22-53 exact — `compile` :28-34,
  `run` :36-46, `produced` :48-52 all inside); `Makefile:698` (single filter-out line);
  `store.cpp:3836-4018` (`adoptOpRecords` body ends :4018, `sealPendingMetadataAsCells`
  opens :4020 — exact); `Makefile:1330-1334` (`test/ux-journey-plans` target :1330, two
  python steps :1331-1332, prerequisite edge :1334) and `:1348-1351` (test rule :1348 +
  three binary invocations :1349-1351) — both land, "only" dropped; "the §2 glossary"
  (:189); prelude as "convention:" with `vlace/prelude.hpp` sibling-file precedent (file
  exists, sits beside `vlace/compiler.cpp`); "`recipients`/`deadline`" typo fixed (:77);
  "~25" both sites (25 total `check(` call sites in `vagent_checker_test.cpp`, ~19 of the
  exact `check(*program)` form — "~" holds).
- **Minor bundle spot-checks (other critics', all landed):** `pending(pid)==1` reframed as
  the flag-time sole-queued test with `pending(pid)==0` at turn end sampled inside the
  callback — `mailbox.pop_front()` at `mesh_actor.cpp:468`, `executor_` invocation
  :474-476 inside the cited :473-477; the `d.dims`-rank guard restored (:387-390 —
  `dimDims && home` at `arena_manifold.cpp:91-103`); phase-6 `+d.name`-bearing-record
  exemption on the `Int64(*handleTarget)` arm (spec :2394-2396 verbatim — "exempt from this
  arm"); per-cycle re-filing (:397-400 — `Store::makeDimension` appends to the persistent
  `d.dims` rank via `setLink`, store.cpp:1087-1088); the OpHandle deferral rewritten
  correctly — "`OpHandle` cells do exist today, `mesh_evaluator.cpp:733`" (`setValueBits`
  `ValueKind::OpHandle` at :733; the palette claim at spec :2430-2432 lands); resolvers
  named (spec :3110 is the P16 table row — the `call`-role `symbol_named`-walk and
  `run`-role label→head text plus "the adapter's own executor TU" all sit on that line);
  two dim-record kinds (spec :1964-1967 + :2393-2395 verbatim); `genesis.{hpp,cpp}` TU
  named; `worker_context.hpp:52-54` — `arena(baseManifold)` pins base at construction,
  moves deleted :57-60; `compiler.cpp:475-483` — the `bindings_.find`+`UnknownName` refusal
  :476-479 inside the Variable-anchor branch; meta's residual six all land (prefix
  productions spec :960-961, `bindings_` interim, splice-only arms §5.4 :924-934,
  `static(param)` spec :968-972 — :971-972 verbatim, descriptor `+d.step`-off-`+d.grab`
  chaining spec :914-917 verbatim, `static` result-read/`literal()` — see n1).
- **One disposition suggestion correctly *not* applied:** "`or_parallel.cpp`→`:182-185`" —
  the four `ensureDimension` calls sit at `or_parallel.cpp:181-184` (dimOr_ :181,
  dimBranch_ :182, dimFrontier_ :183, dimSubmission_ :184); v4 kept my verified :181-184.
  The suggested range was itself the drift.

## MINOR

### m1 — `spec :464-470` does not contain the `splice c := 3` edge it is cited for

- **Plan ref:** phase 2, expand bullet (lines 164-167): refusal edges "… the
  `effect`/`native`/`capacity`/`graft_method` bar, opaque `Code` actuals, and `splice c := 3`
  — cap$N is not re-assignable, spec :464-470".
- **Spec:** the cap$N rule is at spec :785 — "a captured name expands to a `cap$N`
  *binding*, not a re-assignable slot, so `splice c := 3` is a check error". The cited
  range covers the opaque-`Code`-actuals bullet (:463-465) and the
  `effect`/`native`/`capacity`/`graft_method` bar (:468-470); the control-flow bar
  (:460-462) also sits just outside it. Every named edge except the last lands in range —
  and the last is the one the cite trails.
- **Fix:** repoint the parenthetical to `spec :785` (or `spec :460-470, :785` if it is
  meant to cover the whole edge list). The refusal itself is correctly stated and ungated —
  MINOR not MAJOR.

## NIT

1. **`spec :952-955` vs the baseline check** — the `choiceDepth`/`outstandingMarks`
   refusal clause is spec :959-961; :952-955 is the kernel-lower bullet (`vm_.run(cursor,
   staticFuel)` :952-954, `CellValue` read :953, `literal()` :954-955, no-mark :955), which
   does cover the sentence's earlier claims. Adjacent-range miss on the last-named item.
2. **`arena_manifold.hpp:837-846`** — "promote folds only its `root`'s reachable subgraph"
   is anchored by the `@param root` doc at :833-836 ("Only what is reachable from it along
   any dimension is promoted"); :837-839's `@return` does name the reachable subgraph and
   the root-not-`from` refusal, so the cite lands partially — the tighter range is
   :833-846.
3. **"appends only under a resolvable `home`"** (:389) — the code gate is `dimDims &&
   home` (`arena_manifold.cpp:91-93`); the d.dims conjunct is dropped. The spec's own
   comment says "under a base-supplied home" (:3015-3016) — inherited shorthand either way.
4. **"nineteen names all told"** (:394) — `home` is `makeCell("home")`
   (`vortex_core.cpp:35`), a plain genesis cell, not a dimension: it heads the `d.dims`
   rank without joining it, and nothing resolves it through `dimensionNamed`. The filed
   dim-name count is eighteen (17 + `d.dims`). The spec's own filing list groups "the
   home/d.dims genesis pair" the same way (spec :3019-3021), so the plan is spec-faithful —
   noted only, in case an implementer reads the count as "19 d.dims rank entries".
5. **Carried r3-n7** — (c) still groups `agent cell`/`validate` constants beside the
   `programRef`→root hop (`:450-452`); spec :2344-2345 letters only the hop. Still labeled
   as additions, still defensible.

## Coverage matrix — unchanged, all rows closed

Every §10.1 row remains mapped; §10.2's P15/P16 and seams (i)–(v) all present; U-M1–U-M11
stays out; no r3-covered item was dropped. The v4 deltas (marshalling clause, promote
class, driver TU, (e) resolvers, `+d.name` exemption, pending-sampling reframe, per-cycle
filing, the seven session mints) are all spec- and code-verified.
