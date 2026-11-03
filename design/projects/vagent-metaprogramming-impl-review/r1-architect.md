# r1 — system-architecture review (phase ordering and deliverability)

**Verdict:** NOT CONVERGED

**Counts:** 3 BLOCKING / 3 MAJOR / 4 MINOR / 2 NIT

Lens: phase ordering and deliverability only — does the dependency column tell the
truth, can each phase ship green and leave a coherent product, are the phase-6
sub-steps internally ordered right, and is any phase-boundary item missing.
Mechanism-level correctness of the spec itself is out of scope per the brief.

Plan under review: `design/vagent-metaprogramming-implementation.md` (line refs
below are to that file). Spec refs are `design/vagent-metaprogramming.md` v1.19.

Verified load-bearing cites (all clean): `worker_context.hpp:52-54` is the
two-arg `WorkerContext(const Manifold*, const SpanReader*)` ctor; `mesh_actor.cpp`
`:182`/`:188`/`:261`/`:271-273` are exactly the `resume` signature, the
`!checkpoint.store` guard, the `userPermascrollPtr()` read, and the
base-only `WorkerContext` construction + post-construction `setDefaultSpanReader`;
`vortex_core.cpp:50-69` mints 17 `mintNamedDimension` cells + the `home`/`d.dims`
pair = "19 system dims" ✓; `publication.cpp:1093-1103` is `historyFromSeal`,
which mints `Store()` internally at `:1097` ✓; `mesh_actor.cpp:473-477` is the
`executor_(…, *actor->context)` invocation inside `pumpOne` ✓; `parser.cpp:848-855`
is the `PrefixedStringLiteral` gate with the `"cells"` prefix check ✓;
`checker.cpp:2061-2063` is the import allowlist (`std:agent`/`std:goal`) ✓;
`compiler.hpp:23-26` is `ScalarCompilerLimits` ✓; `checker.hpp:12-27` is
`CheckLimits` ✓; `VAGENT_FRONTEND_SRCS` (Makefile:688-690) is an explicit list —
the `COMMON_XANADU_SRCS` glob (Makefile:643) covers only `XUDU_CORE_OBJS`, so
`fragment.cpp`'s "kernel-glob, no edit" claim is true and every frontend TU
*does* need an explicit add for the `vagent` binary and `vagent_frontend_test` —
the `cell_path.cpp` precedent (0bd47bc1) is real.

What the plan gets right, before the findings: phases 0–2 are genuinely
dependency-clean and independently shippable on today's tree (frontend exists;
`expand` needs `resolve`'s stamps; `static` needs the scalar kernel — all
present); the phase-6 macro-order (seams → lineage ceremony → P16 ceremony) is
the right shape; the §10.2 register is fully covered (P15, P16, all four named
adapter seams, the conditional segment-apply surface correctly qualified, P17
correctly excluded); the §15 deferred list is complete; P16's acceptance test
matches the spec's verbatim.

---

## BLOCKING

### B-1 — Phase 3 consumes `std:meta` machinery the plan itself assigns to phase 4

**Plan lines 99–108 vs 123–124.**

What it claims: phase 3 delivers `run` ("`run(c, [args])` special form +
`value_of` in the checker"), the resolvers `label_of`/`clauses_of`/`code_named`/
`plan_named`/`symbol_named`, and `reflect`/`reify`; its exit is "resolver tests
incl. qualified-name and headless-`run` refusal". Phase 4 then delivers
"`std:meta` prelude sources — Vortex-level routines linked like `std:agent`;
checker import-allowlist + builtin member-signature table entries".

What the spec shows: every one of those names is a `std:meta` member, "called
bare after `import std:meta`" (§6.1, spec :1048–1052). A source program can only
name them once the import allowlist gains `std:meta` (`checker.cpp:2061-2063`,
verified) *and* the builtin member-signature table gains its entries — both of
which the plan puts in phase 4. `run` itself is the `std:meta` checker special
form; the headless-`run` refusal lives inside its descriptor check. In phase 3
as written, `import std:meta` is refused and `run`/`label_of`/etc. have no
signature-table entries, so the entire phase-3 exit is unreachable through the
language surface. The dependency is inverted inside the plan's own map.

Suggested fix: split the package at the plan's own read/write boundary — phase 3
delivers the `std:meta` package shell (import allowlist + member-signature
table + prelude linkage) and the *read* group (`program`/`agent_cell`/
`module_named`/`symbol_named`/`clauses_of`/`test_of`/`body_of`/`plan_named`/
`code_named`/`label_of`/`validate`/`value_of`/`weight_of`/`next_rand`) plus
`reify` and `run`; phase 4 adds the mutation verbs on top. One-line edit to the
phase boundary, zero new machinery.

### B-2 — The vagent actor substrate is a hard precondition named nowhere

**Plan lines 9–13, 17–26 (phase map), 88–199 (phases 3–7).**

What it claims: the "Depends on" column is fully internal — phase 3 depends on
2, phase 5 on 4, phase 6 on 5. Each phase "ships green" on the tree the
previous phase leaves.

What the spec and the tree show: the spec's own status line says vagent's
"source frontend and the scalar lowering kernel exist; the actor executor does
not" (spec :6), and `vagent-implementation.md` shows the substrate open:
iteration 3 (call ABI, linked `std:agent` prelude, complete lowering) partially
open, iteration 4 (actor turns) open, iterations 5–7 open. This plan's exits
presuppose that substrate at three distinct depths:

- **Call ABI** — §10.1: "the `run`/`graft_*` verbs need the call ABI (`#Call` +
  frame + spill + the `d.entry` head resolution) — owed by vagent §11.1 itself,
  not new debt here" (spec :3069); §6.2: `run` "owes the call ABI … a consumer
  of it, not a new one" (:1388); §6.1: the `+d.entry` head-hop is "the attach
  convention the call ABI owes" (:1143). Phase 3's `run` is kernel-simulable
  (the `Call` frame-write mechanism exists at `vortex_vm.cpp:854-893`), but only
  if the plan consciously absorbs the consumer side of the convention — which
  should be said, since the spec assigns the convention itself to vagent.
- **Linked-prelude path** — `std:meta` is "linked like `std:agent`"; the
  linked-prelude artifact path is itself open vagent debt (iteration 3), so
  resolver *checking* is testable in phase 3 but resolver *execution* through a
  linked prelude is not.
- **Actor turns + adapter** — phase 4's "mutation under execution" is
  turn-boundary semantics; phase 5's genesis replay is the adapter's genesis
  hook; phase 6's entire P16 ceremony runs inside "the adapter's own `Executor`
  callback inside `pumpOne`" (spec :2129) — a vagent adapter that does not
  exist (`apps/common/xanadu/vagent/` holds frontend + kernel only; the
  mesh-level `Executor` hook exists at `mesh_actor.hpp:115` but nothing supplies
  it). Phase 7's `§9.2`/`§9.3`-green exit needs the full executor — the spec's
  own scope note says only §9.1 is inside today's scalar subset (:2905-2907).

The plan does name P4's approved wake in phase 6 — it acknowledges *that*
external seam while staying silent about the far larger substrate the seam
hangs on. As written, phases 3, 5, 6 and 7 cannot ship their stated exits on
the tree this plan builds; the exits are untestable by construction until
external machinery lands.

Suggested fix: add a "Preconditions" section (or "Phase −1") stating: phases
0–2 ship on today's tree; phases 3+ gate on vagent iterations 3–4+ — the call
ABI including the `d.entry` attach convention, the linked-prelude path, actor
turns and the vagent adapter with its V10 bindings and genesis hook. Then mark
each phase's external requirements in the Depends column and annotate which
exits are kernel-simulable (run semantics, drain/marks disciplines) versus
adapter-gated (genesis, checkpoint, §9.2–9.3).

### B-3 — Phase 5's re-genesis exit needs machinery the plan assigns to phase 6 (or to no phase)

**Plan lines 11, 24–25, 134–153 vs 155–186.**

What it claims: phase 5 ships durability — `patch:<n>` serials, dedupe, genesis
(e)/(f) — with exit "learn→re-genesis→learn cycles in a test harness (genesis
re-run without transport)". Phase 6 then delivers the lineage store ceremony,
the adopted-band comparand and dim filing.

What the spec shows: genesis is one fixed sequence — (a) lineage bind/comparand
→ (b) `ensureDimension` find-or-mint + `setOpcodeKind` re-registration → (c)
`programRef`→root bind → (d) `d.patch` replay → (e) `+d.refs` rehydration → (f)
turns (spec :2222–2440). The replay and rebind steps the plan puts in phase 5
are the *last* steps of a chain whose first three need: a seeded lineage
`Store` the image folds back out of (`historyFromSeal` + the span-source pair —
plan's own phase-6 sub-step 2), the (a) comparand (sub-step 2), the dim filing
(sub-step 3), and the `programRef` record + (c) bind (owned by no phase — see
M-2). The dedupe property phase 5 exists to deliver is only exercisable when
the skeleton folds back — "the superseded declared clause folding back at
`1..E_ops` is displaced again" (spec :2360–2361). Without a real fold there is
nothing for replay to displace and nothing for a name-record rebind to rebind
against; a hand-rolled image can't reproduce `birthOp = opIndex`
position-stability. And the plan's own ordering rule — "nothing
checkpoint-surviving lands before the lineage-store and filing seams exist"
(line 11) — is violated by its own phase map: `d.patch`/`d.code` durability
*is* checkpoint survival, and it lands in phase 5, before the lineage store and
filing it requires.

Suggested fix: cut the boundary at *transport*, not at *genesis*. The lineage
`Store`, its `historyFromSeal` seeding, the span-source pair, dim filing, the
`programRef` record, and genesis passes (a)–(e) are all transport-free — a
`Store` is a local object and a re-genesis harness is exactly
seed→fold→genesis→session→re-fold. Put them with durability in phase 5 (the
harness then tests the real passes, not a reimplementation). Phase 6 keeps what
genuinely needs the transport boundary: the spawn base-injection seam, `resume`
supply/verify, the `documentId` arm, the conditional segment-apply surface,
P4's wake, and the P16 flag/re-kind/restore ceremony plus the real round-trip.

---

## MAJOR

### M-1 — Phase 5's "genesis (e)/(f) split" mislabels the spec's steps and states the R7-B order backwards

**Plan lines 144–146.**

What it claims: "Genesis (e)/(f) split: (e) name-record rebind for registered
heads *and* replay-installed copies; (f) `d.patch` replay after rebind —
patch-installed symbols resolve to post-replacement bindings."

What the spec shows: the patch replay is step **(d)**, not (f) — (f) is
"turns" (spec :2344, :2440). The ordering is pinned: (d) replay "hands (e) its
install set", and the (e) rebind/rehydration sweep runs *last* "for two reasons
— coverage first, ordering second", because "a name-record's rebind must see
post-installation symbols" (:2373–2380, :2435–2439; the R7-B reorder, changelog
:3552–3556). The plan's text literally sequences the replay *after* the rebind —
the inverse of the mandated order, reintroducing exactly the misbind/drop class
R7-B removed — and contradicts its own stated goal, since "patch-installed
symbols resolve to post-replacement bindings" *requires* replay before rebind.

Suggested fix: restate as "genesis (d) `d.patch` replay in ordinal order —
replayed installs hand (e) their install set; (e) `+d.refs`/name-record rebind
sweeps registered heads plus replay-installed copies' regenerated records last;
(f) turns." The labels matter — they are the only map an implementer has from
this bullet to §6.6.

### M-2 — Spec-required work items owned by no phase

**Plan lines 94–108 (phase 3), 116–124 (phase 4), 138–149 (phase 5), 160–181
(phase 6); spec §10.1/:3057–3058, §6.6/:2222–2345.**

Four items in §10.1/§6.6 land in no phase:

1. **The `programRef` bind record's contents** — the recorded image extent
   `E_ops` and the recorded instruction enumeration (`produces`
   `MicroversionId`s or canonical post-fold positions), written at emission
   (spec :3057–3058, :2311–2326). Consumed by the (a) comparand, the (b)
   registration pass, the sideness test's `≤E_ops` bound (:1643) and `learn`'s
   fail-closed lineage arm (:2290–2292). Belongs in phase 3's emission work —
   it is emission-side recording beside the `+d.refs`/`+d.recipe` emission the
   plan already lists.
2. **Genesis (b)'s `setOpcodeKind` re-registration pass** — the spec runs it at
   *every* fresh-context image bind, "at the spawn-time image bind as after
   `resume`" (:1551–1556, :2335–2337), because without it "the folded
   skeleton's op cells are absent from the map and surgically writable"
   (:2339–2343) — P15's membership test covers only `mintOpcode`-registered
   cells, which in a folded world is the stdlib, not the program. The plan puts
   "post-resume re-registration" inside phase 4's P15 bullet where it can
   neither be implemented nor tested (no fold exists yet) and never names it in
   the genesis phase — sub-step 3 covers only (b)'s dim half. It is needed at
   *spawn*, not merely post-resume, and it consumes item 1.
3. **Genesis (c), `programRef → program root` bind, and the adapter bind set**
   — the V10-mechanism bind of program root + agent cell + the twelve new
   data-minted dims (`d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/
   `d.patch-head`/`d.validate`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/
   `d.mesh.relocation`) + the `validate` ceiling, arity and impure-kind cells
   (spec :1053–1060, :3003–3007). `program()`/`agent_cell()` are `std:meta`'s
   roots from phase 3 onward; the minted names are what phase-6 sub-step 3's
   filing enumerates — but no phase owns minting/binding them in the first
   place.
4. **The rest of the `std:meta` surface** — `learn_where`/`learn_body` (the
   `graft_where`/`graft_body` durable twins — §9.2, the very example phase 7's
   exit cites, calls `learn_where`), `set_weight`, `sprout_synapse`,
   `weight_of`, `validate`, `program()`, `agent_cell()`, `module_named()`,
   `test_of`, `body_of` and the clause-side memo wrappers. Phase 4 names only
   `learn_plan`/`learn_method`/`learn_handler`/`unlearn`; phase 7 claims "no
   new mechanism, all tests" while its §9.3 exit needs `set_weight`/`weight_of`
   and its §9.2 exit needs `learn_where`.
5. **`staticFuel`** — `static` "needs a `staticFuel` entry in
   `ScalarCompilerLimits` (`compiler.hpp:23-26`)" (spec :3070–3071); phase 2's
   `vm_.run(cursor, staticFuel)` consumes it. Unassigned.

Suggested fix: assign (1) and (3)'s record to phase 3's emission bullet; (2)
and (3)'s bind to the genesis phase (phase 5 under the B-3 re-cut); (4) to the
`std:meta` split in B-1; (5) to phase 2.

### M-3 — Phase 6's listed sub-order files dims after the ceremony that pins the fold

**Plan lines 158–181; spec :2996–3040, :2294–2309.**

What it claims: "Land in the listed sub-order" — 1. adapter seams, 2. lineage
store ceremony (construct/keep/seed/comparand), 3. dim filing, 4. P16.

What the spec shows: the minted-dim vocabulary must be filed on the kept
lineage `Store`'s persistent `d.dims` rank **before the folded base is
pinned** — `getOrCreate` move-assigns its `Manifold&`, so the rebuilt fold the
filing returns *is* the injected base; "a filing after the pin lands outside
the sealed fold — the base image was folded before the rank append" and the
arena mints orphan twins (:3030–3040). The pin moment is the
`WorkerContext`-construction-with-injected-fold that sub-steps 1 and 2 build
toward. Implementing sub-step 2 completely and then sub-step 3 is the
file-after-pin hazard the spec's ordering pin exists to prevent. (The
hazard is fold-order, per the R18-A re-attribution, not name-table
visibility — so "the names are in the table by then" is no defense.)

Suggested fix: state the window — the true order is seam → seed the store →
file the minted-dim vocabulary → fold+inject (pin) → comparand machinery →
P16. Either split sub-step 2 into "seed" and "inject" with filing between, or
annotate that sub-step 3 lands inside sub-step 2's pre-pin window. Sub-steps
1→2 and 4-last are otherwise right.

---

## MINOR

### m-1 — P15's phase-4 rationale is imprecise about the boundary it gates

**Plan lines 11–13, 113–114, 120–122.**

"P15 … is the precondition for trusting opcode claims once program cells
become writable surfaces." Program cells are already writable surfaces —
`cells"…"` + `Value` writes exist today and are documented as unguarded (spec
:3134). P15 coincides not with a new writability boundary but with *sanctioned*
mutation: `std:meta`'s guarantees and `validate` are what need kinds to be
truthful. Consequence worth one line: phase 3's `reflect`/`reify`/`run` ship
under the same pre-existing unguarded-write caveat — the plan's framing that
opcode claims become unverifiable only at phase 4 understates what phase 3
already ships. (The separate scope problem — the bullet advertising
"post-resume re-registration" that can't exist in phase 4 — is M-2.2.)

### m-2 — "`learn_method` replace-then-unlink ordering" reads backwards

**Plan lines 129, 143.**

The spec is unlink-first: "`learn_method`'s unlink-first rides `graft_method`'s
own verb" (:2363–2364); `graft_method` "unlink[s] any existing `owner.name`
symbol (redefinition-as-replacement), then symbol cell on the module's
`d.vars`" (:1172). "Replace-then-unlink" literally instructs install-first —
ambiguous at best, wrong at worst. Say "unlink-then-install (replace)".

### m-3 — Makefile adds missing for two of the five new frontend TUs

**Plan lines 43–44 vs spec :3061–3063.**

§10.1 has all five files join `VAGENT_FRONTEND_SRCS` (explicit list,
Makefile:688–690 — verified); the plan states the Makefile edit only in phase
0 for its three. `code_path.cpp` (phase 1) and `expand.cpp` (phase 2) get no
wiring bullet — and without it the `vagent` binary and `vagent_frontend_test`
won't link them (the glob only covers `XUDU_CORE_OBJS`), so phases 1–2's
`vagent check`-flavored exits fail on a stale binary. The plan flags this exact
failure mode ("the `cell_path.cpp` omission bug is precedent") then applies the
reminder to only three of five files. Repeat the add line in each phase's
bullets.

### m-4 — Phase 1 lists the fragment table as checker work; the table is emitted by `expand` in phase 2

**Plan lines 59–62 vs spec :1019–1023.**

"It emits the merged `Program` — plus the fragment table, keyed by
`SourceLocation`" — the table is *produced* by `expand` (phase 2) and consumed
by `check` (the signature change is phase-appropriate in phase 1). Minor
sequencing ambiguity: as written a phase-1 implementer could expect populated
fragment records before `expand` exists. One clarifying word ("declare the
table + `check` signature now; `expand` populates it in phase 2") suffices.

---

## NIT

### n-1 — Stale spec-version cite

**Plan line 3.** "Companion to `design/vagent-metaprogramming.md` (v1.18 …)" —
the file is v1.19. Harmless (v1.19 is an approvals-only delta, no mechanism
change), but the plan's premise that P15/P16 and the seams *are approved* rests
on that version; bump the cite.

### n-2 — Limitation note inside a phase-5 Exit line

**Plan line 153.** "anonymous declared handlers remain the documented
capability limitation" is a scope statement, not a checkable criterion, and
already sits in the out-of-scope section (line 209). Move or drop from Exit.

---

## Cross-cutting note for the dispositions

B-1 and B-3 are the same defect at two boundaries — a later phase consuming
machinery the plan places earlier, or an earlier phase's exit consuming
machinery the plan places later. The unifying fix is a per-phase "needs from
outside this plan / from later phases" audit of every Exit line, plus the
preconditions section of B-2. Nothing found here reopens a spec ruling; every
fix above is re-sequencing or naming of already-approved work.
