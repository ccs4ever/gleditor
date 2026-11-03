# r3 — codebase-completeness critic (inventory coverage and citation truth)

**Verdict: substance-CONVERGED** (R2-A and R2-B landed verbatim with verified cites; all r2
minors applied, one partially; every new v3 cite checked against code and spec — one MINOR
plus nits remain)

**Counts:** 0 BLOCKING / 0 MAJOR / 1 MINOR / 7 NIT

Scope of this pass: did R2-A/R2-B land correctly; did my r2 minors land; do v3's *new*
`file:line` cites land; does anything in the v3 deltas misstate the actual code.

## Disposition of R2 majors — both landed

- **R2-A (genesis (e)'s third arm).** Phase 5 (e) now reads "three record arms": the
  name-record role-split rebind (`call`-role `int64(entryOp)` vs `run`-role `int64(head)` —
  spec :1085-1087 verified), the **dim-record arm** — `+d.name`-bearing `+d.recipe` records
  write `Int64(DimRef)` re-resolved through `ensureDimension`'s name arm into the
  `+d.refFor`-paired slot or the member cell, *never* the promoted twin `*handleTarget`
  (spec :2395-2406 verified verbatim, esp. :2397-2398), and the ref-record arm deferred to
  phase 6 with the no-op rationale ("nothing mints `OpHandle`-kind cells pre-P16",
  `vortex_core.cpp:196-229` — writeValue arms verified: Double/Int64/Bool/None, no OpHandle).
  (d) gained the matching donor-slot exception (spec :2364-2372 verified). ✓
- **R2-B (re-genesis loop producer).** The phase-5 driver bullet now names the loop:
  `promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847` — signature verified
  :844-846) into `arena.base()->store()` (`base()` at `arena_manifold.hpp:268`,
  `Manifold::store()` at `manifold.hpp:486`; `rebuildManifold` sets `store_` at
  `store.cpp:511`/`:532`, so the expression is live) → `Store::rebuildManifold` over the new
  head → re-attach through a fresh `WorkerContext(base, reader)` (`worker_context.hpp:52-54`
  — takes `const Manifold *`+`SpanReader *`, verified) → re-run (a)–(e). Requires lists
  `promote`/`rebuildManifold`/fresh `WorkerContext`; `overlay.cpp:46-82` verified as the
  mint→rebuild loop (`rebuildManifold` :46, :53, :64, :70, :76, :82). ✓

## My r2 minors — disposition

| r2 | Status in v3 |
| -- | ------------ |
| m1 `d.op-label` cites | **Landed** — "today's unfiled session mints are `d.op-label` at `mesh_value.cpp:473` and `or_parallel`'s four — `d.or`/`d.branch`/`d.frontier`/`d.submission` at `or_parallel.cpp:181-184`". All six cites verified exact. But the enumeration is incomplete — see m1 below. |
| m2 `fragment` placement | **Landed** — "Kernel-glob TU under `apps/common/xanadu/vagent/` — §10.1's tree (spec :2944) — no Makefile edit (Makefile:643); the `vagent/` placement is load-bearing". Both cites verified. |
| m3 `reify:<src>:<serial>` at replay | **Landed** — (d): "Replay does not re-record — replayed grafts mint no `d.patch` cells and consume no `patch:<n>` serial, though each install copy's own `reify:<src>:<serial>` label still mints off the shared `d.vars` counter (spec :2346-2350, :2504-2509)". Both cites verified verbatim. |
| n1 `Makefile:1348-1351` | **Landed** (cross-cutting now cites :1348-1351; `zigzag_test` at :1351 verified). See n4 for the "only" wrinkle. |
| n2 `CompilerBinding` bit | **Landed** — phase 2 now schedules it: "the `CompilerBinding` **state-side marker bit** §10.1 adds for `closed`'s provenance (spec :2958-2959 — the struct is `{cell, type}` today, `compiler.hpp:15-18`; the bit is new work here, not an existing field)". Spec :2958-2959 verified; struct is `{cell, type}` at :15-18. |
| n3 (c) overlabel | **Partial** — (c) still reads "`programRef` → program root bind (spec :2344-2345), plus the bind set's non-dim members — agent cell, `validate` constants". The spec's lettered (c) is the `programRef`→root hop only. Now explicitly labeled as additions rather than silently folded, and (c) is "by the V10 mechanism" — the members *are* V10-mechanism binds — so the grouping is defensible. Carried as n7. |
| n4 "kernel tables" → Emitter | **Landed** — "the `Emitter`'s per-`compileStatement` tables key on `const Expr *` (`compiler.cpp:884-891`)". Verified: Emitter is constructed per statement (`return Emitter(...).compile(...)` at :906), tables at :885-891 all `const Expr*`-keyed. |

## New-cite audit — everything lands

Code cites verified this round (all exact or correctly ranged):

- `arena_manifold.hpp:845-847` (promote signature), `worker_context.hpp:52-54`
  (two-arg ctor, `const Manifold*`), `overlay.cpp:46-82`, `mesh_jobs.cpp:304-318`
  (the `job.base`/`empty` store split :304-318 exact), `vortex_core.cpp:196-229`
  (writeValue arms) and `:196-254` (funnel through splice :251-255), `:14`/`:29-70`/`:35`/
  `:44`/`:50-69` (ctor→initGenesis, `home` :35, `d.dims` :44, 17 system dims :50-69 —
  d.grab→d.role, nineteen cells confirmed), `arena_manifold.cpp:34-46`
  (`dimensions()` = base∪`arenaDims_`), `:78-105` (ensureDimension, `arenaDims_` write :89),
  `:289-290` (`attach()`'s `d.stores`/`d.store-refs`), `dimension_registry.cpp:194-217`
  (`getOrCreate` rebuild :207 + move-assign :214), `store.cpp:123` (no-id `Store()`),
  `:1388-1425` with the span-source pair at `:1418`/`:1425` exact, `:3836-3940`
  (`adoptOpRecords` — see n3), `publication.cpp:1093-1103` with `make_unique<Store>()` at
  :1097, `:1129-1131` (setContentSource+addScroll+bindPublishedLocalScroll),
  `mesh_actor.cpp:182`/`:188`/`:261` (resume, `!checkpoint.store` guard, `receiveValue`
  verify), `:473-477` (`executor_` inside `pumpOne` :460), `:659` (`pending`→`mailbox.size()`
  :659-662; `checkpointView`'s non-empty refusal :672), `mesh_actor.hpp:81`
  (non-optional `message`), `mesh_evaluator.cpp:735` (fresh `makeCell("cell")` per ref),
  `mesh_value.cpp:473` (`ensureDimension("d.op-label")`), `or_parallel.cpp:181-184`
  (four mints), `vortex_vm.cpp:98-100` (`setOpcodeKind`), `:337`/`:343-346`/`:771-806`/
  `:1269-1273`/`:1287-1294` (all five P15 writeback sites), `:854-893` (Call `toInt(inputs[0])`
  :856 + frame push; Return `+d.stack` pop :878-893), `compiler.cpp:884-891`/`:160-181`/
  `:769-772`/`:853-855`/`:500-502`/`:108-110`, `compiler.hpp:15-18`/`:23-26`,
  `checker.cpp:2061-2067`/`:2069`/`:765-779`, `checker.hpp:12-27`/`:30`,
  `parser.cpp:848-855`, `main.cpp:46-53`, `vagent-agent-language.md:1670` (P4 row),
  Makefile `:643`/`:649`/`:688-690`/`:692`/`:695`/`:698-699` (see n2)/`:1147`/`:1348-1351`.
- `Store : public SpanReader` (`store.hpp:128`) — the "Store as `SpanReader`" claim in
  seam (i) is true; `ArenaManifold::base()`/`Manifold::store()` exist as used.

Spec cites verified verbatim — every one in the doc:

:91, :156, :158 (see n5), :259-262 (`Call` `toInt(inputs[0])`, head-valued slot), :311-331
(memo records: `clause +d.code→+d.step` chain, `+d.values`→wrapper, fresh `+d.name` text
cell, `+d.grab` content-compared key, label-less→no record, ~6-cell leak — all five
details present), :847-854 (`def`/plan/handler params *and* `let` locals frame-bound),
:914-920/:1377-1384 (descriptor signature + `+d.values` deref), :1053-1060 (bind set),
:1141-1144 (op-direct/head-`+d.entry`-once discriminator), :1168-1174 (five graft_* +
learn_* row), :1172 (graft_method unlink-first), :1174 (conditional on P16), :1850-1893
(session machinery), :1851-1853 ("never an alias"), :2165-2169/:2171-2179 (P16 windows,
`pending(pid)==1`, no-interleave, synchronous publish), :2228-2232 (pairwise comparand),
:2235-2243 (globalKeyOf-minus-localScroll arm set), :2249-2258 (permutation detection +
no-prefix refusal), :2294-2299, :2311-2326 (produces-ids vs post-fold positions),
:2335-2343 (`opcodeMap_` per-WorkerContext, spawn+resume), :2344-2345, :2346-2350
(replay-no-rerecord), :2363-2364/:2364-2372 (verb re-enactment + donor-slot exception),
:2395-2406/:2397-2398/:2397-2402/:2401-2402 (dim-record arm), :2435-2439 (post-installation
ordering), :2500-2504 (reserved prefixes at module declaration), :2504-2509 (shared
`d.vars` counter), :2905-2907 (§9.1 only scalar), :2944 (fragment under vagent/ in §10.1
tree — "kernel-side, picked up by the existing source glob"), :2958-2959,
:3020-3026/:3031-3039 (file-before-pin), :3057-3058 (programRef record comment — lands),
:3070-3072 (staticFuel). Also re-verified the clause re-point dims: `+d.grab`(test/where)/
`+d.spin`(body) per spec :160/:1170-1171, matching (d)'s new parenthetical.

Twelve apparatus dims counted against spec :1055-1056 — list matches, twelve. The sixteen
non-mutation members still tally (10 reflect + `validate`/`weight_of`/`value_of`/
`next_rand`/`run`/`reify`).

## MINOR

### m1 — "today's unfiled session mints are" is falsified as exhaustive

- **Plan ref:** Phase 5 dim-filing bullet (lines 373-375): "the honest residual
  (spec :2397-2402; today's unfiled session mints are `d.op-label` at `mesh_value.cpp:473`
  and `or_parallel`'s four — `d.or`/`d.branch`/`d.frontier`/`d.submission` at
  `or_parallel.cpp:181-184`)".
- **Code:** those six cites land, but `ensureDimension` has seven more ArenaManifold
  session-mint sites in the tree: `dim_cap.cpp:83-85` (`d.actor`/`d.ticket`/`d.signature`),
  `command_transaction.cpp:16-18` (`d.target`/`d.action`/`d.reply`), `sql_promotion.cpp:140`
  (`d.promoted`) — all minted through `ArenaManifold::ensureDimension`, same
  accrete-an-orphan-if-unfiled class as the named five. As written ("are"), the
  parenthetical asserts a 5-member class; grep finds 12.
- **Fix:** "are" → "include" — or append the seven if the residual class is meant
  exhaustive. No work item is gated by the list (filing is name-table-driven either way),
  which is why this is MINOR not MAJOR.

## NIT

1. **`vagent_compiler_test.cpp:18-46` under-ranges the harness** — `struct ScalarHarness`
   is :22-53 (`compile` :28-34, `run` :36-46, `produced` :48-52); the cite covers neither
   the header nor `produced`.
2. **`Makefile:698-699` overshoots** — the `vagent/` filter-out is a single line at :698
   (:699 is `SPIKE_SRCS`). Harmless range; the disposition's ":699" was itself the drift.
3. **`store.cpp:3836-3940` under-ranges `adoptOpRecords`** — the body runs to :4018
   (tail sync :4001-4017). Same range the spec itself cites (:2247/:2317), so inherited —
   noted only.
4. **"`make test` runs `gleditor_test`, `xuzz_test`, `zigzag_test` only"** — `make test`
   also runs the `test/ux-journey-plans` prerequisite (Makefile:1330-1334 — two python
   steps) before the three binaries. The substance (metaprogramming exits run under
   `make test` only via `xuzz_test`) is undisturbed.
5. **"§6.1's glossary, spec :158"** — the glossary is §2 (opens :141); §6.1 opens :1046.
   Line cite lands; the §-label doesn't.
6. **"prelude sources live beside the frontend — `vagent/prelude/`"** — the directory does
   not exist and no prelude source exists anywhere (the preconditions already say so);
   the placement is a forward decision worded as existing convention.
7. **Residual r2-n3** — phase 5 (c) keeps `agent cell`/`validate` constants beside the
   `programRef`→root hop; spec :2344-2345 letters only the hop. Defensible ((c) is "by the
   V10 mechanism" and the members are V10 binds) but a strictly-lettered acceptance reads
   (c) narrower. Also a stray space typo at line 76: "recipients/ deadline".

## Coverage matrix — unchanged from r2, all rows still closed

Every §10.1 row remains mapped (parser/expand/resolve/frontend/ast.hpp/ast_clone/
code_path/fragment/checker/compiler/stdmeta/apps-vagent/adapter/Makefile); §10.2's P15,
P16 and seams (i)–(v) all present with correct cites; U-M1–U-M11 stays out. No r2-covered
item was dropped by the repartition — the re-genesis producer and (e)'s third arm filled
the only structural gaps.
