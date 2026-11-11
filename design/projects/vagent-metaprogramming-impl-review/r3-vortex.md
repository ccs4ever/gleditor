# r3 — Vortex / VM-semantics critic

**Verdict: CONVERGED.** Both r2 majors landed *correctly* — the re-genesis
producer chain and the dim-record arm are mechanically right as written, every
load-bearing cite re-verified against today's tree. The new-text hunt turns up
no majors: three minors and two nits, all clause-level polish inside otherwise
verified text.

Counts: **0 BLOCKING / 0 MAJOR / 3 MINOR / 2 NIT**

Lens: same as r1–r2 — does the phase order respect VM semantics, and are the
phase-5/6 mechanics constructible on today's tree? Plan refs are
`design/vagent-metaprogramming-implementation.md` v3; spec refs are
`design/vagent-metaprogramming.md` v1.19. All code cites below re-verified
today.

---

## nM1 — re-genesis fold loop: LANDED CORRECTLY (full mechanical verification)

Plan :378-385 names the loop; :436-438 the Requires; :442-443 the ≥2-cycle
exit. Each link verified:

- **`promote(Store&, parent, arena, root)`** — `std::optional<Promoted>
  promote(xanadu::Store&, const MicroversionId&, const ArenaManifold&, CellRef,
  PromotionBudget = {})` at `zigzag/arena_manifold.hpp:845-847`; "promote() is
  the only way across" at :28. `Promoted::version` is the new head.
- **into `arena.base()->store()`** — mechanically right, and the
  "publish-destination shape" gloss is accurate: `ArenaManifold::base()` is
  `const Manifold*` (`arena_manifold.hpp:268`), `Manifold::store()` returns a
  non-const `xanadu::Store*` (`manifold.hpp:486`), and `Store::rebuildManifold`
  *sets* `store_` on the fold it returns (`store.cpp:511`, `:532`) — so the
  base fold's `->store()` is the kept lineage `Store`, which is exactly where
  the cycle must publish. Production precedent: `promoteAndAttachToStore`
  promotes `primary_.arena` into a caller-supplied `store` then
  `rebuildManifold(promoted->version)` + `getOrCreate`
  (`vortex_host.cpp:609-637`); the bound-store arm at :393-403 is the same
  promote-into-base's-store shape.
- **`Store::rebuildManifold(newHead)`** — returns `zigzag::Manifold` *by value*
  (`store.hpp:451`, `[[nodiscard]]`), so the harness holds the fold and passes
  `&fold` as `base`. Right object type for the next step.
- **fresh `WorkerContext(base, reader)`** — the two-arg ctor
  (`worker_context.hpp:52-54`) mints `arena(baseManifold)` — a *fresh*
  `ArenaManifold` overlay comes for free inside the context — plus a fresh
  `VortexVM`, whose `opcodeMap_`
  (`std::unordered_map<CellRef, OpcodeKind>`, `vortex_vm.hpp:186`) starts
  empty; `setOpcodeKind` writes it at `vortex_vm.cpp:98-100`. That is exactly
  what (b)'s re-registration needs to fill, and matches spec :2335-2336
  ("`opcodeMap_` is per-`WorkerContext` and holds none of the folded image's
  op cells"). The only imprecision is the *stated reason* for freshness —
  see m3.
- **`overlay.cpp:46-82` really is the precedent** — `declareOverlayTarget`
  mints (`makeDimension`/`makeCell`/`setLink`) then `store.rebuildManifold`
  re-folds after every mutation (:46-82), no actor anywhere; `getOrCreate`
  shows the same mint→`rebuildManifold`→move-assign idiom
  (`dimension_registry.cpp:194-217`, re-verified). The "re-pin" half of the
  plan's "re-pin loop" is the `WorkerContext` re-attach, which the precedent
  doesn't need (it keeps a local `fold` variable) — still the right cite for
  "the identical mint→rebuild→re-pin loop with no actor."
- **no `sealLocalSpool` claim — correct.** `rebuildManifold` reads the
  in-memory `opsSpool`; `promote` already appended the records. Sealing is a
  transport-format step; nothing on the local path consumes it.
- **initial band via `promote` of the compile arena or `adoptOpRecords`** —
  `adoptOpRecords` at `store.cpp:3836-3940`, the deterministic
  `produces`-heap topo-sort (spec :2246-2250 re-verified).

The only thing left abstract is `root` — see n2.

## nM2 — dim-record arm: LANDED CORRECTLY (verbatim-faithful, both sites)

- **(e) restatement** (:418-430): the dim arm's text — "`+d.name`-bearing
  `+d.recipe` records (ref-capture dim records and `dim`-field members alike)
  write `Int64(DimRef)` re-resolved through `ensureDimension`'s name arm into
  the `+d.refFor`-paired slot or the member cell itself — a filed-cell rebind,
  *never* the promoted twin `*handleTarget` names" — is a faithful compression
  of spec :2394-2398, including the "respectively" pairing (ref-capture
  record → its paired slot; `dim`-field member → the member cell itself, spec
  :2396-2397) and the promoted-twin bar (spec :2397-2398, cited).
  `ensureDimension`'s name arm is `arena_manifold.cpp:78-105` (re-verified:
  `dimensionNamed` → `arenaDims_` → mint, with the `d.dims`-rank append at
  :91-103). The not-a-no-op reasoning is right: emission writes the
  compile arena's dense `Int64(DimRef)` (spec :2369-2372), stale the moment
  the fold differs — independent of the `OpHandle` re-kind the ref arm waits
  on.
- **(d)'s donor-slot exception** (:413-416): "the `+d.name`-bearing dim
  record excepted, where the record's `+d.name` *is* the endpoint source and
  the slot takes `Int64` of the resolved `DimRef`, not the donor's promoted
  twin" — verbatim against spec :2369-2372. The R2-A ruling's second clause
  landed.
- **The phase-3 schema's dangling consumer is closed** (:218-219): "+d.name on
  a +d.recipe member reserved to the dim field's dim-name channel — consumed
  by phase 4's `closed` re-verification and phase 5's (e) rebind" — both
  consumers now exist (spec :158's `closed` glossary names the dim-field
  member record; (e) delivers the rebind).
- **Exit coverage** (:445-446): "a `dim`-field recipe record re-resolved to
  `Int64(DimRef)` off the filed name post-fold" — the reify-through-the-name-
  channel case I asked for.
- **The ref arm's deferral is now correctly attributed** (:427-430): "a no-op
  here because nothing mints `OpHandle`-kind cells pre-P16 (`writeValue`
  covers Int64/Double/Bool/None only, `vortex_core.cpp:196-229`)" — verified:
  `writeValue`'s four arms are Double/Int64/Bool/None(:196-229), `splice` at
  :251 completes the :196-254 funnel range; `promote` *translates* existing
  handle values, it doesn't mint them. The m2 misattribution is gone — and
  the arm moved inside step 3's ceremony (:477-479), so the r2 ordering note
  (ref arm depending on P16 flags from a later step) is resolved too.

## No-re-record rule + the rest of the r2 bundle: landed

- **(d) "Replay does not re-record"** (:407-410) — verbatim against spec
  :2346-2350; the `reify:<src>:<serial>`-still-mints-off-`d.vars` clause is
  spec :2504-2509 (re-verified: the counter is the agent cell's `d.vars`,
  shared by `patch:<n>`).
- **Verbatim re-mint + second-genesis eviction** (:410-413) — verbatim against
  spec :2351-2354, and the ≥2-re-geneses exit now carries its own rationale
  (:442-443).
- **(a) comparand** (:387-394) — pairwise band-vs-band, `globalKeyOf` arm set
  minus `localScroll` (`breakMarkerKey` `publication.hpp:447`, `scrollKey`
  `scroll.cpp:13-22`, disengaged-id failure), spec :2228-2243 re-verified; the
  single-store degeneracy and the second "received" store fix (m3) landed,
  and the exit's permuted-band refusal case is spec :2248-2251's permutation
  detection. `pending`/`checkpointView` empty-mailbox discipline —
  `mesh_actor.cpp:659-662` re-verified.
- **(b) two-arm split** (:395-399) — `ensureDimension` name-resolution arm vs
  `setOpcodeKind` enumeration arm, "both run at every fresh-context image bind
  (spawn *and* resume)" = spec :2335-2337.
- **Filing list narrowed** (:357-375) — the twelve apparatus *dim names* file;
  program root/agent cell/`validate` constants bind at (c) and never file;
  `d.op-label` at `mesh_value.cpp:473` and `or_parallel`'s four
  (`d.or`/`d.branch`/`d.frontier`/`d.submission` at `or_parallel.cpp:181-184`)
  — both cites re-verified.
- **(c)** (:400-402) — spec :2344-2345 verbatim; the "apparatus dims' names
  already resolved through (b)" gloss is consistent with (b)'s find arm.
- **Ordering line** (:343-348) — "seed → mint/file → fold → pin/inject →
  first-resolution", `getOrCreate`'s rebuilt fold *is* the injected base —
  verbatim against spec :2294-2300/:3031-3039; "the emit lineage ... its
  freeze is the compile arena's `initGenesis` itself" is consistent with
  `vortex_core.cpp:14` (ctor calls `initGenesis()`) and the body at :29-70
  (17 `mintNamedDimension` + `home` at :35 + `d.dims` at :44 — nineteen).
- **`CompilerBinding` bit** (:189-191) — now "the bit is new work here, not an
  existing field"; `{cell, type}` at `compiler.hpp:15-18` and spec :2958-2959
  ("CompilerBinding gains a state-side marker bit") both verified — the fix
  took the "schedule it" arm of the ruling.
- **Phase-6 (i)** (:460-462) — "`Manifold` folded over the seeded *and filed*
  lineage `Store` — plus the `Store` as `SpanReader` — through the
  `WorkerContext` two-arg ctor ... which takes `const Manifold *`, not a
  `Store`" — verified (`Store` is-a `SpanReader`;
  `mesh_actor.cpp:270-273` passes `checkpoint->manifold.get()` + store as
  reader on the resume path).
- **`learn_*` names** (:322-326) — `patch:<n>` minted off `d.vars`; recorded
  target name rides un-minted; `learn_method`'s is "the functional lookup
  name — *never an alias*" — spec :1851-1853 verbatim.
- **`run`/`reify` group label** (:229-231) — "the *read group* proper is the
  pure top half; `run` (opaque-impure) and `reify` (impure) ride the same side
  of the *functional* split" — the mislabel is gone.
- **`fragment.{hpp,cpp}` under `vagent/`** (:221-223) — Makefile:643 glob +
  :698 filter-out re-verified.
- **Marker sharing** (:476-477) — "sharing impossible — `link` evicts the
  first edge; `mesh_evaluator.cpp:735` precedent" — verified *correct*:
  `link` is one mutual edge (`arena_manifold.cpp:1347-1377` — setting
  refB→M clears M's `-dir` neighbor refA), so a shared marker cell would
  evict the first ref's edge, per spec :1121-1124.
- **`Call` operand claim** (:261-262) — "the operand slot stays head-valued,
  never a `Call`-opcode operand (`Call` executes `toInt(inputs[0])` ... spec
  :259-262)" — verified: `vortex_vm.cpp:856` is `toInt(inputs[0])`; frame
  writes :859-875, Return :878-893 — inside the cited :854-893.
- **`test/vagent-*` targets** (:533-534) — `test/vagent-frontend`,
  `test/vagent-syntax`, `test/vagent-runtime`, `test/vagent-compiler` all
  exist (Makefile:1158-1173).

---

## New MINOR

### m1 — `pending(pid)==1` reads as the arm predicate; on the code it is the *break* case

**Plan :482-483.** "arm the window on mailbox-empty-at-turn-end (spec
:2165-2169; `pending(pid)==1` is the queue-depth proxy — a flagged turn that
self-sends breaks it)". Inside the `Executor` callback — where the plan puts
the ceremony — the in-flight message was already popped
(`mailbox.pop_front()` at `mesh_actor.cpp:468`, `executor_` invoked at
:475-477), so `pending(pid)` (`mailbox.size()`, :659-662) counts *remaining*
queue depth: mailbox-empty ⟺ **`pending(pid)==0`**, and `==1` is precisely the
self-send/queued-successor case the same parenthetical calls the break. Read
as the arm-time predicate, `==1` arms the window in exactly the forbidden
case and never in the correct one.

The charitable reading — `pending(pid)==1` sampled at *flag* time, where the
flagged message sole-queued predicts its end leaves the mailbox empty — is
coherent, but then the end-of-turn `mailbox.empty()` re-check is still owed
(the self-send lands between the two samples), and the plan names neither
sampling point. The operative clause ("mailbox-empty-at-turn-end", spec
:2165-2166 — "`checkpointView` refuses a non-empty one", :672) is right;
only the proxy token is wrong-or-unspecified.

**Fix:** name the sample point — "`mailbox.empty()` at the flagged turn's end
inside the callback" (or: `pending(pid)==1` is the *flag-time* queue-depth
test — flagged message sole-queued — with the end-of-turn empty check the
veto a self-send trips).

### m2 — the filing enumeration drops spec's "never the arena-local `d.dims` rank" guard

**Plan :359-361 vs spec :2303-2306.** The plan names the enumeration surface
correctly — "the `ensureDimension`-written, `dimensionNamed`-read surface,
base∪`arenaDims_`, `arena_manifold.cpp:34-46`" — but the spec adds "never the
arena-local `d.dims` rank, which appends only under a base-supplied `home`,
`:91-103`", and the plan drops it. Verified: `ensureDimension`'s `d.dims`
append is gated `if (dimDims && home)` (`arena_manifold.cpp:92-103`), so dims
minted without a resolvable `home` never reach the rank — an implementer
enumerating the rank instead of `dimensions()` misses exactly the
session-minted class the filing exists to cover. Adjacent imprecision in the
same clause: `dimensions()` (:34-46) yields `DimRef`s, not names — the name
is each dim cell's interned content (`makeCell(span)` at :87-88) — and the
`arenaDims_` name→ref map is private (:756), so filing-by-name wants a
name-yielding enumeration (over `dimensions()` + cell text, or a new
accessor).

**Fix:** append the exclusion and the name source — "enumerated off
`dimensions()`' base∪`arenaDims_` (names read off each dim cell's interned
content) — never the arena-local `d.dims` rank, which appends only under a
resolvable `home`".

### m3 — "fresh because `setOpcodeKind` writes `opcodeMap_`" is true but the wrong causality

**Plan :382-383.** "re-attach through a fresh `WorkerContext(base, reader)`
(`worker_context.hpp:52-54` — fresh because (b)'s `setOpcodeKind` writes
`opcodeMap_` on the context's `VortexVM`)". The empty map is why (b) must
*re-run* on the new context; it is not why the context must be *fresh* —
that is forced regardless: `arena(baseManifold)` is set at construction
(`worker_context.hpp:54`), `base_` has no setter, and the type is
non-copyable/non-movable by design (:57-60 — members hold references into
earlier members). As written it invites "clear `opcodeMap_` and reuse the
context", which is impossible — the arena would still overlay the *old*
fold.

**Fix:** "fresh — the context's `arena` pins `base` at construction and the
bundle is non-movable; the fresh VM's empty `opcodeMap_` is what (b)
re-fills".

---

## New NIT

- **n1 — (e) header "three record arms"** (:418): the header counts the spec
  sweep's three arms, but only two run in phase 5 — the ref arm's deferral
  arrives two clauses later (:427). Say "the sweep's three arms: name-record
  role-split and dim-record here; the `*handleTarget` ref arm in phase 6".
  Same bullet's "a filed-cell rebind"/"off the filed rank" understates the
  unfiled sub-case — spec :2401-2402 re-mints unfiled names (the (b) bullet
  covers it; a "(re-mint for unfiled)" tail would close it). And "nothing
  mints `OpHandle`-kind cells pre-P16" is loose as a blanket claim —
  `mesh_evaluator.cpp:733-735` mints `OpHandle` in artifact emission today;
  scoped to the *record* context it's right.
- **n2 — two stale/under-specified tokens.** (i) `Makefile:698-699` drifted
  again: today the `VAGENT_RUNTIME_CORE_OBJS` filter-out is :698 (the
  `VAGENT_RUNTIME_TEST_OBJS` list :696, its comment :697), and :699 is
  `SPIKE_SRCS` — cite :696-698. (ii) `promote`'s `root` is left abstract
  (:380) — the spec's checkpoint root is the agent cell ("`promote` walks
  only what the agent cell reaches", spec :375/:635-636); one word pins it.

---

## Spot-check status

All r2-verified cites re-checked clean on today's tree. Newly verified for
v3: `store.hpp:451` + `store.cpp:524-540` (`rebuildManifold` by-value,
`setStore` at :511/:532); `manifold.hpp:486` (`store()` non-const);
`arena_manifold.hpp:268` + `arena_manifold.cpp:48-74` (`base()`,
`dimensionNamed` base→`arenaDims_`); `worker_context.hpp:52-66` (ctor-fixed
`arena(baseManifold)`, owned `ArenaManifold`, non-movable);
`vortex_vm.hpp:118/:186`, `vortex_vm.cpp:98-100` (`setOpcodeKind`/
`opcodeMap_`); `vortex_vm.cpp:853-893` (Call frame :854-876, Return
:878-893, `toInt(inputs[0])` at :856); `vortex_core.cpp:14` (ctor calls
`initGenesis`), :29-70 body, :196-229 `writeValue`, :251 `splice`;
`dimension_registry.cpp:194-217` (mint→rebuild→move-assign);
`publication.cpp:1093-1103`/`std::make_unique<Store>()` at :1097,
:1129-1131 (`setContentSource`/`addScroll`/`bindPublishedLocalScroll`);
`store.cpp:1418` (`bindPublishedLocalScroll` body), :1425
(`setContentSource` body), :3836 (`adoptOpRecords`);
`arena_manifold.cpp:78-105` (`ensureDimension` name arm, `d.dims` append
gated on `dimDims && home` at :92-103), :289-290 (`attach()`'s
`d.stores`/`d.store-refs`), :1347-1377 (mutual-link eviction);
`compiler.cpp:769-772` (`dimConstant`/`findOrMintDimension`), :500-502
(preflight refusal), :853-855 (`localCells_`), :884-891 (Emitter's
`const Expr*`-keyed tables); `compiler.hpp:15-18` (`{cell, type}`),
:23-26 (`ScalarCompilerLimits`); `checker.cpp:2061-2067` (allowlist),
:765-779 (member table), :2069 (`builtinTypes`); `mesh_actor.cpp:156`
(no-arg `WorkerContext` at spawn — the seam (i) gap), :182/:188/:261
(resume take-or-verify), :270-273 (resume pin + reader), :468/:475-477
(pop-then-`executor_`), :659-662 (`pending`); `mesh_jobs.cpp:304-318`
(`job.base`); `mesh_evaluator.cpp:723-736` (per-reference marker mint);
`mesh_value.cpp:473` (`d.op-label`), `or_parallel.cpp:181-184` (the four);
`publication.hpp:447` (`breakMarkerKey`), `scroll.cpp:13-22` (`scrollKey`);
`Makefile:643/:649/:688-690/:692/:695/:696-699/:1147/:1150-1173/:1348-1351`;
`tests/xuzz/vagent_compiler_test.cpp:22-46` (ScalarHarness),
`vagent_checker_test.cpp` (7 bare `check(*program)` + ~5 with
`CheckLimits` args ≈ the "~10" claim); spec :158, :259-262, :311-331,
:847-854, :914-920, :1053-1060, :1141-1144, :1168-1174, :1850-1893,
:2165-2183, :2228-2265, :2294-2345, :2346-2440, :2500-2510, :2905-2907,
:2944-2959, :3002-3039, :3057-3058, :3070-3072 — all as the plan reads
them.
