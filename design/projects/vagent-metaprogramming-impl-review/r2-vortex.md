# r2 — Vortex / VM-semantics critic

**Verdict: NOT CONVERGED — one revision short.** Every r1 finding closed (all
three BLOCKINGs, all six MAJORs, all minors); two *new* MAJORs found in the v2
text, both in phase 5, both cheap to fix.

Counts: **0 BLOCKING / 2 MAJOR / 5 MINOR / 2 NIT**

Lens: same as r1 — does the phase order respect VM semantics, and is the new
phase-5 "local `Store`" genesis actually mechanically constructible on today's
tree? Answer: yes, with two unassigned arms. Plan refs are
`design/vagent-metaprogramming-implementation.md` v2; spec refs are
`design/vagent-metaprogramming.md` v1.19. All code cites below re-verified
against `apps/common/xanadu/` today.

---

## r1 findings — disposition check (all closed)

- **B1 (genesis order) — CLOSED.** Phase 5 :336-350 now runs (d) `d.patch`
  replay in ordinal order → (e) `+d.refs` rehydration **last**, with "(f) is
  turns and stays out of scope" (:324-325). The (d)→(e) install-set hand-off is
  stated (:343-344), the `patch:<n>` verbatim re-mint is spec-faithful
  (:341-343 — spec :2354-2360 carries the identical single-occupancy/
  second-genesis argument), and the (e) split is now honest: name-record
  (role-split) arm in phase 5, `Int64(*handleTarget)` ref arm deferred to
  phase 6 step 2 (:386-387). Verified against spec :2346-2440 — the reorder
  rationale ("post-installation symbols", spec :2435-2439) is correctly cited.
- **B2 (testable fold) — CLOSED in substance; residual = new M1 below.**
  Phase 5 now owns lineage-store construction, `historyFromSeal` seeding
  (`publication.cpp:1093-1103`, `Store()` minted at :1097 — verified), the
  `setContentSource`/`bindPublishedLocalScroll`/`addScroll` span-source triple
  (`publication.cpp:1129-1131` — the `restorePublication` precedent, verified),
  dim filing, and the (a)–(e) driver. The full local fold chain exists today:
  `promote(Store&, MicroversionId parent, const ArenaManifold&, CellRef root,
  PromotionBudget)` is a free function (`arena_manifold.hpp:845-847`, verified),
  `Store::rebuildManifold(head)` folds to a `Manifold` (used non-actor at
  `overlay.cpp:46-82`), `DimensionRegistry::getOrCreate` shows the
  mint→rebuild→reinject idiom (`dimension_registry.cpp:194-217` — move-assigns
  the rebuilt fold, verified), and the pin is `WorkerContext(const Manifold*,
  const SpanReader*)` (`worker_context.hpp:52-54`, verified). No actor, no
  transport anywhere in the loop.
- **B3 (`std:meta` split) — CLOSED.** Phase 3 :203-215 carries the allowlist
  entry, signature-table rows and prelude skeleton *plus* the read group;
  phase 4 :261-267 takes the mutation verbs. Engine-level testing vs
  linked-prelude is honestly marked (:206, :236-238).
- **programRef record — CLOSED.** Phase 3 :198-202 emits `E_ops` + the
  instruction enumeration beside `compileFragment`'s marking; consumers named
  (phase-4 P15, phase-5 (a)/(b), `learn`'s `≤E_ops` bound).
- **Genesis driver — CLOSED.** Phase 5 :323-325 names the (a)–(e) orchestrated
  pass as §1's priced `std:agent` genesis hook, harness-invoked in phase 5,
  production-wired in phase 6 step 2.
- **Spawn-time bind set — CLOSED.** Phase 3 :216-223 names the harness-supplied
  bind set: program root, `agent_cell()`, the twelve apparatus dims
  (`d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/`d.patch-head`/`d.validate`/
  `d.refs`/`d.ref`/`d.recipe`/`d.refFor`/`d.mesh.relocation`), `validate`
  constants — verbatim against spec :1053-1060 and §10.1's list (:3002-3007).
- **P16 marking split — CLOSED.** Phase 3 :191-195 states the full record
  schema as its emission contract (name/ref discriminator, `call`/`run` roles,
  `+d.refFor`, the `+d.name`-on-`+d.recipe` dim channel); phase 6 step 3
  (:388-391) is explicitly a *revisit* minting one `d.mesh.relocation→"cell"`
  marker per ref cell.
- **Call-ABI judgment — CLOSED.** Phase 3 :224-227 prices the minimal
  convention; precondition :26-30 names the owed §11.1 remainder. **Verified
  VM-real:** `Call` pushes a `makeCell` return frame on `+d.stack`
  (`vortex_vm.cpp:853-876`), `Return` pops it (:878-893), `Bind` binds
  `+d.vars`/`+d.values` name-keyed (:894+), the cursor advances on `+d.spin`
  (:1331-1335) so the Call op's own `+d.spin` successor is the return point,
  and the `+d.entry` head-hop convention is spec :1141-1143 as cited.
- **Adapter substrate — CLOSED.** The preconditions section (:20-48) names all
  four owed pieces plus per-phase **Requires** kernel-simulable/adapter-gated
  marks. `ActorExecution::message` non-optional at `mesh_actor.hpp:81`
  (verified), `WorkerContext()` mint at `mesh_actor.cpp:156` (verified).
- **No-marks dim-minting — CLOSED.** Phase 3 :232-234 states the
  already-filed-or-refused discipline for `reify`'s `ensureDimension` arm.
- **m1-m6, n1-n2 — all closed.** `staticFuel` in phase 2 (:167-168);
  `code_path.cpp`/`expand.cpp` Makefile edits land in phase 1 (:106, :113-115);
  the stamp ordering note (:86-89); named refusal for surviving `Code`/`Splice`
  (:163-165); `patch:<n>` mint correctly placed in phase 4 with the verb-mints/
  alias-rides-record split (:275-284, spec :1850-1893 verified); `check(program,
  table)` lands with its producer + `main.cpp:46-53` rewiring (:118, :125-127);
  "17 system dims + the `home`/`d.dims` genesis pair — nineteen cells all told"
  (:313-315); v1.19 cite (:3).

---

## New MAJOR

### nM1 — Phase 5's exit names seed and driver but not the *cycle*: the per-round `promote`→refold producer is unassigned

**Plan :296-364.** The exit is "learn→**re-genesis**→learn cycles — ≥2
re-geneses". The bullets name the seed (`historyFromSeal`), the span bind, the
filing and the driver — but nothing produces the fold the *second* genesis
binds. Learned `d.patch` records are ephemeral arena cells; the only path into
any fold is `promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847`
— "promote() is the only way", :28). So each cycle mechanically requires
`promote` into the kept lineage `Store` → `rebuildManifold` → fresh
`WorkerContext` → driver (a)–(e), and the *initial* image band likewise needs a
producer (`promote` of the compile arena, or `adoptOpRecords`/`historyFromSeal`
of sealed segments — `adoptOpRecords` is the deterministic topo-sort fold,
`store.cpp:3836-3940`). All pieces exist and are kernel-simulable —
`overlay.cpp:46-82` drives the identical mint→rebuild→re-pin loop with no
actor — but the phase's own mechanics list, which is otherwise scrupulous about
naming the machinery (it names `getOrCreate`'s fold-is-base behavior only in
the goal line :298-300), never assigns the checkpoint step the exit iterates on.
This is r1-B2 one level down: the machinery moved into phase 5's reach, the
named work item did not follow.

**Fix:** one bullet — "the cycle step: `promote(store, head, arena, root)` →
`rebuildManifold(newHead)` → re-attach (`WorkerContext` two-arg) → driver;
a same-`Store` cycle needs no `sealLocalSpool` (op records and local spans ride
verbatim); the initial image band comes from `promote` of the compile arena or
`adoptOpRecords`". Also name `getOrCreate`/`Store::makeDimension` as the filing
mechanism (spec :3020-3026 names it; the plan cites :194-217 only for the
fold-is-base property).

### nM2 — The phase-5/6 split of spec-(e) drops the sweep's *third* arm: `+d.name`-bearing `+d.recipe`/dim records' `Int64(DimRef)` writeback

**Plan :345-350 vs spec :2381-2410.** Spec-(e)'s single sweep handles **three**
record classes: plain ref-records → `Int64(*handleTarget)`; `+d.name`-bearing
`+d.refs` name-records → the `call`/`run` role-split rebind; **and**
"`+d.name`-bearing `+d.recipe` record — the ref-capture dim record or the
`dim`-field record alike — instead writes `Int64` of the `DimRef`
`ensureDimension`'s name arm re-resolves (`arena_manifold.cpp:78-105`), into
the record's `+d.refFor`-paired slot or the member cell itself" (spec
:2395-2406 — "the `+d.recipe` `+d.name`-bearing records read for the dim arm
below"). The plan assigns arm one to phase 6 (:349-350, :386-387) and arm two
to phase 5 (:345-348); arm three is assigned nowhere — it is neither a
`call`/`run` role rebind nor a `*handleTarget` writeback.

The arm is load-bearing at *every* genesis including the local test: emission
writes the `dim`-field member's cell as `Int64(DimRef)` of the **compile
arena's** dense id ("the `dim`-field member's own cell taking the same
`Int64(DimRef)`, the member *being* its field cell" — spec :2369-2372) — a
stale `ephemeralBit|dense` value in any fold; and unlike arm one it is **not**
a no-op pre-P16, because the stale-ness is from emission, not transport (no
`OpHandle` re-kind needed for the bug — the Int64 is wrong the moment the fold
differs). `reify` replaying a recipe whose `dim` field still names the compile
arena links/mints against a dead `DimRef` — silent misbind inside the phase-5
exit's own coverage, and nothing in either phase's exit is certain to exercise
it (the §9.3 rebind case exercises the *symbol* arm).

**Fix:** assign the dim-record arm explicitly — it belongs in phase 5 (it needs
no P16: it is a name-resolution rewrite, run every genesis) — restate (e) as
"three record arms: name-record role-split and `+d.name`-bearing dim records'
`Int64(DimRef)` here; the `*handleTarget` ref arm in phase 6" — and add an
exit case reifying a recipe whose `dim` field resolves through the name
channel post-fold.

---

## New MINOR

- **m1 — (d) drops two spec replay rules.** Plan :336-344 transposes the
  per-verb re-enactment but omits (i) "**replay does not re-record**" —
  replayed grafts mint no patch cells and consume no `patch:<n>` serial; only
  the install copy's `reify:<src>:<serial>` label mints off the shared counter
  (spec :2346-2350). An implementer re-running phase-4 `learn_*` code verbatim
  doubles `d.patch` per genesis and burns the no-ABA serial. And (ii) the
  copy's ref-slot endpoint derivation — slots derive from the *paired donor
  slot's promoted content*, never the donor's recorded literal (the
  stale-alias preclusion, spec :2364-2372). Both belong in (d)'s contract.
- **m2 — "a no-op without promote" misattributes.** Plan :349-350: `promote`
  *does* run in the phase-5 cycle (nM1 — it is the only way records reach the
  fold). The ref arm is a no-op because nothing mints `OpHandle`-kind cells
  before P16: `writeValue` covers Int64/Double/Bool/None only
  (`vortex_core.cpp:196-229`, verified), `promote` only *translates* existing
  `OpHandle`/`ForeignOpHandle` values (`arena_manifold.cpp:1934-1954`), and the
  Int64→`OpHandle` re-kind is the phase-6 ceremony. Say "a no-op without the
  marker mint + re-kind".
- **m3 — (a)'s comparand gets no phase-5 coverage though locally testable.**
  In the single-`Store` cycle the comparand self-compares and can never fail;
  but a two-store harness is fully local (lineage `Store` + a second
  `historyFromSeal`/`adoptOpRecords` "received" store — merged/foreign band or
  missing prefix for the refusal arms, spec :2249-2258, :1771-1776). Phase 5's
  exit exercises (d)/(e) and filing but nothing of (a); the only comparand
  test sits in phase 6's exit (:401-402). A refusal case is phase-5's to own.
- **m4 — phase-6 (i) passes a `Store` where the ctor takes a `Manifold`.**
  Plan :377-379 "base-injection of the seeded and filed lineage `Store`
  through the `WorkerContext` two-arg ctor (`worker_context.hpp:52-54`)" — the
  ctor signature is `(const Manifold* baseManifold, const SpanReader* reader)`.
  The spec itself hedges "supply-side, not a store parameter" (:3080-3086);
  the plan dropped the hedge. Say "the `Manifold` folded over the seeded and
  filed lineage `Store` — plus the `Store` as `SpanReader`".
- **m5 — phase-3's "`CompilerBinding` state-side marker bit" doesn't exist.**
  Plan :189-190: `CompilerBinding` is `{CellRef cell; Type type;}`
  (`compiler.hpp:15-18`, verified) — no bit. The `closed` flag's provenance is
  the capture set's cell classification, computed per binding at check time;
  wherever the flag is *stored* between phase-2 computation and phase-3
  descriptor emission (binding field, fragment-table record) is unowned. Name
  the carrier.

## New NIT

- **n1 — Makefile:698 is the `VAGENT_RUNTIME_TEST_OBJS` list; the filter-out
  is :699** (`VAGENT_RUNTIME_CORE_OBJS := $(filter-out …)`). Adjacent lines;
  cite :698-699.
- **n2 — `(spec :2397-2402, d.op-label/or_parallel the known members)`** —
  :2397-2402 names no members; `d.op-label`'s session mint is
  `mesh_value.cpp:473` (`ensureDimension("d.op-label")`, verified),
  `or_parallel`'s is the parallel-import path (`or_parallel.cpp:349`, spec
  :1522). The substance is right; the cite conflates the residual text with
  the `== 1`-marks aside.

---

## Assigned hunt questions — direct answers

1. **Is the phase-5 "local `Store`" genesis test mechanically constructible on
   today's tree?** **Yes** — and no transport anywhere is needed.
   `historyFromSeal` takes sealed segment strings and mints `Store()` with no
   id (`publication.cpp:1093-1103`; for a same-store harness `adoptOpRecords`
   on decoded records does the same fold, `store.cpp:3836-3940`). The span
   bind is `setContentSource`+`addScroll`+`bindPublishedLocalScroll`
   (`publication.cpp:1129-1131`). Filing is `getOrCreate`→`makeDimension`→
   `rebuildManifold` move-assigned back (`dimension_registry.cpp:194-217`).
   The fold is `store.rebuildManifold(head)` — the same call `receiveValue`
   uses at resume — and the pin is `WorkerContext(&manifold, &reader)`
   (`worker_context.hpp:52-54`; resume itself mints the no-arg context then
   `setDefaultSpanReader`, `mesh_actor.cpp:271-273`). The missing-but-existing
   piece is the per-cycle `promote` (nM1); `checkpointView`'s quiescence gates
   (`mesh_actor.cpp:668-694`) are production discipline, not a harness
   requirement — a test is quiescent by construction.
2. **Is phase-3's minimal call convention VM-real?** **Yes.** `Call`
   (`vortex_vm.cpp:853-876`), `Return` (:878-893), `Bind` (:894+), `+d.stack`
   frames, `+d.spin` threading (:1331-1335), `+d.entry` head hop — all present;
   only the compiler-side emission (which ops to mint, arg/spill convention)
   is new, and it is priced.
3. **Phase-6's four steps vs the actual seams?** **Complete and correctly
   conditional.** Step 1 covers all five owed seams (i)–(v) — spawn-side
   base-injection, resume take-or-verify (`mesh_actor.cpp:182`,`:188`,`:261`,
   `:271-273` verified), P4 `wake` + `ActorExecution::message`→`std::optional`
   (`mesh_actor.hpp:81`), `documentId` arm (`publication.cpp:1097`,
   `store.cpp:123`), and the conditional segment-apply surface — plus step 2's
   genesis wiring + ref-record arm and step 3's marker mint + flag windows +
   no-interleave discipline (spec :2165-2183 verified verbatim). The `1.`
   renumbering is acknowledged in-text (:372-373).

## Spot-check status

New v2 cites verified clean: `vortex_core.cpp:50-69` = 17 `mintNamedDimension`
calls + `home`/`d.dims` pair; `mesh_actor.cpp:659-662` `pending`; `:668-694`
`checkpointView` gates; `executor_` callback inside `pumpOne` at ~:472-477;
`compiler.cpp:500-502` preflight refusal; `checker.cpp:2069` `builtinTypes`;
`checker.hpp:30` `check` public; `Makefile:643/:649/:688-690/:692/:698-699/
:1147/:1348-1350`; `tests/xuzz/vagent_compiler_test.cpp:18-46` ScalarHarness;
`tests/samples/vagent/mini_*.vagent` present; spec :1141-1144, :1740-1776,
:1850-1893, :2171-2183, :2346-2440, :2496-2510, :2903-2907, :3002-3026,
:3057-3058 all as the plan reads them.
