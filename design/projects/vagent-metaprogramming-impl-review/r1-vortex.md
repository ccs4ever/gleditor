# r1 — Vortex / VM-semantics critic

**Verdict: NOT CONVERGED**

Counts: **3 BLOCKING / 6 MAJOR / 6 MINOR / 2 NIT**

Lens: does the phase order respect VM semantics? Findings answer the four
assigned questions plus what the dependency walk surfaced. Plan line refs are
`design/vagent-metaprogramming-implementation.md`; spec refs are
`design/vagent-metaprogramming.md` v1.19 (mechanism text identical to v1.18 —
the plan's `v1.18` header cite is stale by an approvals bump only, n2).

---

## BLOCKING

### B1 — Phase 5's "genesis (e)/(f) split" inverts spec ordering and mislabels the steps

**Plan :145-147:** "(e) name-record rebind for registered heads *and*
replay-installed copies; (f) `d.patch` replay after rebind — patch-installed
symbols resolve to post-replacement bindings."

**What the spec shows:** genesis runs (d) `d.patch` replay **then** (e)
`+d.refs` rehydration, and (e) is *last* for a stated reason (spec :2417-2439):
"a name-record's rebind must see **post-installation** symbols — under the
naive pre-replay position a `learn_method`/`learn_plan` install-or-repoint
replayed at (d) was invisible to it, so a call to a patch-replaced symbol bound
the pre-replacement image binding and a call naming a patch-installed symbol
dropped the patch." The plan's cited rationale ("post-replacement bindings")
*is* the spec's argument for the opposite order. The text is also
self-contradictory: (e) covers "replay-installed copies" whose records cannot
exist before (f)'s replay runs — the spec makes the hand-off explicit: "**the
replay hands (e) its install set**" (spec :2374-2380). Lettering is off too:
spec (f) is "turns" (spec :2440), not patch replay — the plan's (f) is the
spec's (d). Finally, the same work is assigned twice: phase-5 "(e)" is the
name-record arm of spec (e), while phase-6 sub-step 4 claims "genesis
rehydration by record-slot role (`call` vs `run` writeback split,
`int64(entryOp)` vs `int64(head)`)" (plan :177-181) — the *same* spec-(e)
sweep, since the role split is the name-record arm.

Followed literally, the rebind sweep runs before any replay: registered heads
bind fine but every replay-installed copy's name-record slots stay unwritten →
calls inside re-learned bodies fail at the descriptor check → the phase's own
exit (learn→re-genesis→learn) cannot pass.

**Fix:** restate as "genesis (d) `d.patch` replay in ordinal order, then (e)
`+d.refs` sweep — *name-record arm only* in this phase; the ref-record
`Int64(*handleTarget)` arm stays phase 6 with P16" — replay precedes rebind,
(d) hands its install set to (e). Drop the "(f)" label or use it for spec's
turns. Say explicitly which arm of spec-(e) phase 6 re-enters.

### B2 — Phase 5's exit "genesis re-run without transport" is untestable by construction

**Plan :151-152:** "learn→re-genesis→learn cycles in a test harness (genesis
re-run without transport)."

**What the spec/code shows:** genesis is bind-over-received-fold — the
persistent manifold *arrives*, it is not constructed (spec :2215-2220). To
exercise (d)/(e) at all, the harness needs:

1. **A folded image band `1..E_ops`** — i.e., the `historyFromSeal` seeding +
   `setContentSource` + `bindPublishedLocalScroll(store->addScroll(from))`
   span binding that the plan assigns to **phase 6, sub-step 2** (plan
   :169-172; spec :1648-1661, `publication.cpp:1093-1103`, `:1129-1131`,
   `store.cpp:1388-1405`). Without the span-source binding "every
   artifact-local span — all `d.*` dim names, `+d.name`/`src:` labels, symbol
   names — resolves to `""`" — and phase 5's whole surface is name-keyed
   (`symbol_named`/`plan_named` verbatim `textOf` matches, `patch:<n>` label
   joins, the `dim`-name arm).
2. **The agent cell's `d.patch`/`d.code`/`d.vars` state in the new fold** —
   ephemeral arena cells reach a store only through `promote`/publish (the
   checkpoint path — transport by another name — or a hand-rolled op append
   the plan does not describe).
3. **The `programRef` bind record carrying E_ops + the instruction
   enumeration** — without it the sideness test "fails closed: `learn` is
   refused outright" (spec :1771-1776) and the *second* learn in the cycle
   cannot run. That record has no phase home (M1).
4. **The genesis driver itself** — the orchestrated (a)-(f) pass is new
   adapter code; `initGenesis` only adopts `home_`/dims and has no re-genesis
   door (`vortex_core.cpp:14`,`:27-45`; `vortex_core.hpp:268`,`:282`,`:287`;
   spec :1694). No phase owns it (M1).

The only literal no-transport reading — re-running the replay/rebind routines
over the same live arena — exercises no fold, no renumbering, no
`handleTarget`, and is not what "genesis" names; it tests replay logic only.

**Fix:** either pull the store-seed + span-bind + bind-record subset forward
into phase 5's dependencies, or restate the exit honestly as a same-arena
replay driver and say which semantics it does *not* cover (fold order,
`E_ops` classification, promoted positions).

### B3 — Phase 3 ships `std:meta` members whose code home and call gate are phase-4 work

**Plan :99-104 vs :123-124:** phase 3 delivers `run(c, [args])` special form,
`value_of`, resolvers (`label_of`, `clauses_of`, `code_named`, `plan_named`,
`symbol_named`), and `reflect`/`reify`; phase 4 delivers "`std:meta` prelude
sources — Vortex-level routines linked like `std:agent`; checker
import-allowlist + builtin member-signature table entries."

**What the spec/code shows:** every phase-3 member *is* a `std:meta` member —
Vortex/vagent prelude source (spec :1048-1053) — i.e., the routines' *code
home* is the phase-4 prelude bullet. And the call gate is import-conditional:
the allowlist (`checker.cpp:2061-2067` — `std:agent`/`std:goal` only, verified)
refuses `import std:meta` today; `run`/`value_of` stay legal as member names
only "when `std:meta` is imported" and a user `def run` is refused only under
the import (spec :725-736) — so even `run`'s checker special form can't be
exercised through `check()` without the allowlist + signature table
(`checker.cpp:765-779`, verified). Phase 3's exit — "`run(code"…")` executes;
resolver tests incl. qualified-name and headless-`run` refusal" (plan
:106-108) — has no door through the language as written.

**Fix:** split the scaffolding, not the membership: phase 3 lands the
`std:meta` allowlist entry, the signature-table entries and prelude-source
skeleton for exactly the members it ships (`run`, `value_of`, resolvers,
`reflect`, `reify`, plus `program()`/`agent_cell()`/`module_named` if they
ride the same table); phase 4 appends the verb entries and routines. If the
intent is that phase-3 tests drive the routines engine-level (the
`vagent_compiler_test` harness style), say so — and say what *isn't* covered
until phase 4.

---

## MAJOR

### M1 — The genesis driver and the `programRef`/E_ops/instruction-enumeration bind record have no phase

**Plan :121-122, :139-149, :161-186.** Genesis (a)-(f) is a sequenced adapter
pass — comparand, `ensureDimension`+`setOpcodeKind`, `programRef` bind,
`d.patch` replay, `+d.refs` rehydration — yet no phase owns the orchestrator.
The plan scatters its parts: the `setOpcodeKind` pass inside phase 4's P15
bullet (":121-122"), replay+rebind in phase 5, the comparand in phase-6
sub-step 2, rehydration in sub-step 4 — and the **`programRef` bind record
that "carries the recorded image extent and instruction enumeration genesis
(a)/(b) assert"** (spec :3057-3058) appears nowhere. Worse, the *recording* is
emission-side (spec :2311-2326: the artifact records its op cells at emission
as `produces` `MicroversionId`s or canonical post-fold positions — the
`program.instructions` precedent, `mesh_evaluator.cpp:658-663`, verified), and
phase 4's own P15 bullet consumes "the recorded instruction enumeration"
without any phase producing it. Note also the enumeration must exist at
*spawn*, not just resume: `opcodeMap_` is per-`WorkerContext` and holds none
of the folded image's ops on every fresh context (spec :1551-1556,
:2335-2343) — a spawned agent's program ops are unprotected by P15 until the
pass runs.

**Fix:** name the genesis driver (the (a)-(f) sequencer at spawn *and*
resume) and the bind-record work — the E_ops extent + op-cell enumeration
recorded at emission, carried on the programRef bind — as explicit items.
The recording belongs beside phase 3's emission; the driver beside phase 5's
genesis steps (or as the first sub-step of phase 6 if B2 pulls the seed
forward).

### M2 — The adapter's `std:meta` spawn binding (program root, agent cell, `d.*` dims, validate constants) has no phase

**No plan home.** §10.1's adapter bullet (spec :3002-3007) makes the adapter
bind — via V10's no-marks mechanism — the program root cell, the agent's own
cell, `d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/`d.patch-head`/
`d.validate`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/`d.mesh.relocation`, and
the validate constant cells. The plan maps the adapter bullet wholesale to
phase 6's store-ceremony sub-steps, but the *binding* half is a phase-3
precondition: `code_named` walks the agent cell's `d.code` rank,
`plan_named`/`symbol_named`/`module_named`/`clauses_of` walk the program
root's `d.vars`/`d.clause`, `reify`'s `reify:<src>:<serial>` mints off the
agent cell's `d.vars` counter (spec :2505-2509); phase 4's `learn_*` writes
`d.patch` records and `graft_handler` writes `priority` on `d.vars`; and
`compileFragment`'s `+d.entry`/`+d.refs`/`+d.recipe`/`+d.ref`/`+d.refFor`
edges need those dims to exist in the *compile* arena at emission and in the
runtime arena for `reify` (see M6 for why their existence is load-bearing,
not cosmetic). The phase-3 exit's resolver tests cannot resolve anything
without roots to walk.

**Fix:** name the spawn-side binding step explicitly — either as a phase-3
item (harness-supplied roots + dims are legitimate for engine tests, but say
it) or as a named precondition on the adapter baseline (M5). State which
dims are minted at bind vs. filed at filing.

### M3 — P16's "emission marking" is phase-3 emission work misplaced into phase 6's adapter ceremony

**Plan :94-96 vs :177-181.** Sub-step 4 lists "emission marking (`+d.refs`
inventory incl. `+d.recipe` fields; `d.mesh.relocation→"cell"` markers)" as
P16 work — but the record schema is written by `compileFragment` (phase 3's
`fragment.cpp`) and regenerated by `reify` (phase 3's routine, whose spec
contract already includes "regenerated `+d.refs` records and fresh
per-ref-cell `d.mesh.relocation` markers", spec :1167). The schema — the
`+d.name` name-record discriminator, the `+d.grab→"call"/"run"` role labels,
the `+d.refFor` correlation — is consumed *before* phase 6: phase 4's
`closed` re-verification reads it (spec :1996-2000) and phase 5's name-record
rebind keys on it (B1). The only genuinely phase-6 piece is the marker-edge
mint — inert session-side, and gated on the `d.mesh.relocation` dim's
existence (M2). Meanwhile phase 3's bullet says only "`+d.refs` mark rank"
while its exit checks "`+d.refs`/`+d.recipe` records" — the schema's phase
ownership is ambiguous as written: an implementer can emit a bare rank in
phase 3 and discover in phase 5 that name-records, role labels and `refFor`
pairings were never emitted — or that `reify` regenerates a different schema
than emission writes.

**Fix:** phase 3 states the full record schema — name-record vs ref-record
discriminator, `call`/`run` role labels, `+d.refFor` pairing — as its
emission contract; phase 6's sub-step is explicitly a *revisit of phase-3
files* (`fragment.cpp` + the `reify` source) adding only the marker-edge
mint.

### M4 — `run`'s exit presumes the §5.8/§11.1 call ABI that the current kernel does not emit

**Plan :99-100, :106.** `run` lowers to the same-cursor `#Call` + frame +
name-keyed arg binding + spill convention (`vortex_vm.cpp:854-893`, verified:
`Call` executes `toInt(inputs[0])`, pushes a `makeCell` frame on `+d.stack`,
`setCursorOpcode` jumps). The current scalar kernel mints no `Call`/`Return`
ops at all — its op vocabulary is Jump/Value/Branch/Cut/Choice/ChoiceDepth/
Neg/And/Eq/Linked/New/Fail/Halt, with `localCells_` a static per-compile map
(compiler.cpp:853-891, verified) — "the interim no-call shape" (spec
:848-849). The spec concedes the ABI is "owed by vagent §11.1 itself, not new
debt here" (spec :3069-3070) — but the plan neither names that precondition
nor prices a minimal version (the fragment-local convention: `Call` op +
frame `d.vars`/`d.values` binding + `result`/`produced` spill + the
`+d.entry` head hop — "the attach convention the call ABI owes", spec
:1141-1144). Phase 3's exit "`run(code"…")` executes" cannot pass on a kernel
that cannot emit a call.

**Fix:** state explicitly whether phase 3 delivers a minimal frame/spill
lowering for `run` (priced — it is real emission work) or is gated on the
§11.1 ABI landing first (then say so in "Depends on").

### M5 — The adapter/artifact substrate the plan's later phases stand on does not exist in this tree

**Plan :123-124, :161-186.** Everything the spec calls "the adapter" is
greenfield: `apps/vagent/` is `check`-only (`main.cpp:14`, 91 lines, verified
— parse + `check()` and nothing else); `apps/common/xanadu/vagent/` has no
`program.{hpp,cpp}` sealed-artifact format, no `agent_executor`, no
`host_bindings` — all owed §11.1 work; there is no `std:agent` prelude
*source* in the tree (the allowlist/signature entries exist,
`checker.cpp:770-777`,`:2061-2067`, but no linkable Vortex source), so "linked
like `std:agent`" (plan :123) names a mechanism that does not exist;
`programRef` exists only as a generic grant/continuation field
(`mesh_actor.cpp:199`,`:215-216`). The generic mesh machinery the plan cites
does exist (`spawn`/`resume`/`pumpOne`/`checkpointView`/`publishValue`/
`promote`/`receiveValue`/`rebuildManifold`/`historyFromSeal`/`adoptOpRecords`
— verified throughout) — what is absent is the *vagent-side* consumer of it.

Consequence: phases 1-3 can ship green against `WorkerContext`+`Compiler`
harnesses, but phases 4-6's deliverables — prelude linkage, the spawn
binding, the lineage-store ceremony, genesis, the P16 flag windows "inside
the `Executor` callback" — presuppose an executor that runs a vagent
artifact. Phase 6's exit (learn→checkpoint→resume→learn→checkpoint) is a
spawn/resume round-trip over a real artifact.

**Fix:** state the §11.1 baseline (artifact format + agent_executor + a
linkable-prelude mechanism) as a named precondition for phases 4-6, or scope
those phases' exits to named harness substitutes.

### M6 — `reify`'s `ensureDimension` replay can write `arenaDims_` under live marks — the no-marks minting discipline is unassigned

**Plan :104 vs spec :1581-1586.** `reify` replays `+d.recipe` `dim` fields
through `ensureDimension`'s name arm (spec :1214-1216, :1252-1259). Verified:
`ensureDimension`'s mint arm writes `arenaDims_[name]` — an untrailed C++ map
(`arena_manifold.cpp:78-105`) — while `reify` is impure and runs inside turn
marks. §6.5's rule "dimensions are minted only on the no-marks genesis path"
is the P17-class desync (release truncates arena cells, never `arenaDims_`);
a `reify` under a live choice point hitting the mint arm leaves the map
naming a dead/reused ref. This is the one real place phase-3 code touches
the discipline the plan leaves implicit — the write-funnel guards of phase 4
do not cover it (it is a map, not a `writeValue`). The mitigation is exactly
M2+M5's binding completeness: filed/bound dim names resolve through
`dimensionNamed`'s find arm and never reach the mint.

**Fix:** name the discipline where `reify` lands — e.g., phase 3 states
"`reify` may only resolve already-filed/bound dim names; a would-be mint
under marks is a named refusal" — or record it as an accepted residual with
the filing guarantee (phase 6 sub-step 3) as the named mitigation.

---

## MINOR

- **m1 — `staticFuel` unlisted.** §10.1 requires a `staticFuel` entry in
  `ScalarCompilerLimits` (`compiler.hpp:23-26`, verified: `maxNodes`/
  `maxDepth` only). Phase 2's `static` bullet names the run, not the limit
  (plan :79-81). `VortexVM::run(CellRef, std::size_t, std::stop_token)`
  (`vortex_vm.hpp:162`) takes it directly.
- **m2 — Makefile wiring is split but only partially stated.** Phase 0 adds
  "all three" (resolve/ast_clone/frontend) to `VAGENT_FRONTEND_SRCS`
  (Makefile:688-690, verified — explicit list, the `cell_path` precedent is
  real), but `code_path.cpp` lands in phase 1 and `expand.cpp` in phase 2
  with no Makefile bullet in either phase. (`fragment.cpp` is correctly
  globbed — `COMMON_XANADU_SRCS` at Makefile:643; verified.)
- **m3 — Phase-0 `cloneExpr` "preserve the stamp" precedes the stamp.**
  `ast.hpp`'s scope-stamp field is a phase-1 bullet (plan :56); phase 0's
  clone cannot preserve a field that does not exist, nor cover
  `ExprKind::{Code,Splice,Static}` — a phase-1 revisit of `ast_clone` is
  needed and unstated.
- **m4 — Phase-2 lowering needs a named refusal for surviving
  `ExprKind::Code`/`Splice`.** A `let`-bound literal not fully substituted
  (or a `code` value passed to `run`) reaches the merged AST before
  `compileFragment` exists; the kernel must refuse it as a compile error,
  not hit an unhandled-kind path.
- **m5 — `patch:<n>` minting is phase-4, not phase-5.** The session `learn_*`
  mints the installed clause's `patch:<n>` `+d.name` at install and consumes
  the agent-cell `d.vars` serial then (spec :1854-1856, :1882-1885); phase
  5's "verb-minted durable labels" bullet (plan :139-141) reads as if the
  mint begins there — phase 5 owns only the serial's durability/no-ABA.
  Phase 4's "alias minting" (plan :128-129) is also backwards: the verb
  mints `patch:<n>`; the user `name` rides the patch record as an un-minted
  alias.
- **m6 — `check()` signature change has unlisted callers.** §5.6 gives
  `check` a fragment-table parameter (spec :1022-1023); `vagent check`'s
  call site (`apps/vagent/main.cpp:53`) and any other callers need updating
  — a one-line fix that belongs to a named phase (phase 1 or 2).

## NITS

- **n1 — plan :176 "19 system dims (`vortex_core.cpp:50-69`)".** The range
  holds 17 `mintNamedDimension` calls (verified: `d.grab`…`d.role` at
  :50-69); 19 counts `home` (:35) + `d.dims` (:44). The spec's own cite is
  ":50-69 … :35,:44" — say "17 dims + the genesis pair" or cite :35-69. This
  enumeration is exactly where r18's dim-filing bug lived, so the cite
  should be precise.
- **n2 — plan :3 cites spec "v1.18"; reference is v1.19.** v1.19 is an
  approvals-recording bump only (spec :4119-4124: P15/P16 approved, "no
  mechanism text changes") — immaterial but the plan should name the
  version it sequences.

---

## Direct answers to the assigned lens questions

1. **Phase-2 `static` no-mark machinery — available pre-adapter. Confirmed,
   no inversion.** `Emitter`/`Compiler` already own `core_`/`vm_`
   (`compiler.hpp:47-49,70`); `VortexVM::run` (`vortex_vm.hpp:162`),
   `vm_.choiceDepth()` (`:134`) and
   `core_.arena().outstandingMarks()` (`arena_manifold.hpp:659-661`) are
   already exercised at `compiler.cpp:108-110`; `spawnCursor` mints onto
   `home`'s `+d.cursors` (`vortex_vm.cpp:148-161`); eval ops register via
   `vm_.mintOpcode` — the allowed compile-time map writer (spec :1581-1586).
   Nothing adapter-side is touched. (Caveat is m1's missing `staticFuel`.)

2. **Does phase 3 need phase-4 guards to not corrupt state? — No, for the
   write funnel.** Every runtime write phase 3 performs is
   P15-exempt-by-construction: `run`'s `#Call` frame cells are per-activation
   `makeCell` (the named frame-cell carve-out, `vortex_vm.cpp:860`);
   `reify`'s palette is `New`/`Link`/`Value` on fresh unregistered cells;
   `reflect` wrappers are links; `run`'s head/`+d.entry` deref is reads.
   The raw `cells`+`Value`-onto-op-cell hole predates this plan (spec §11
   documents it unguarded). The one genuine cross-phase VM-semantics touch
   is not the funnel but M6's `arenaDims_` map-write under marks — a
   discipline, not a guard, and the plan assigns it nowhere.

3. **Phase 5 "genesis re-run without transport" — cannot be exercised as
   stated** → B2 (and B1 for the order the phase prescribes).

4. **P16 emission marking listed last — yes, misplaced** → M3: the
   `+d.refs` record schema is phase-3 emission contract consumed by phases
   4-5; only the marker-edge mint is phase-6 work, and both live in
   phase-3's files (`fragment.cpp` + the `reify` prelude routine).

## Citation spot-check (all verified accurate against `apps/common/xanadu/`)

`parser.cpp:848-855` (`!= "cells"` gate); `compiler.cpp:108-110`
(choiceDepth/outstandingMarks), `:160-181` (binding() render check +
`externalReferences_`), `:475-486` (anchor resolution + `cannot create`),
`:705-712` (`Let` lowering), `:769-772` (`dimConstant` →
`findOrMintDimension`), `:853-891` (`localCells_`, `const Expr*`-keyed
tables); `compiler.hpp:23-26` (`ScalarCompilerLimits`); `checker.hpp:12-27`
(`CheckLimits`); `checker.cpp:30-36` (`snake` accepts `$`), `:555-562`
(arity arm), `:765-779`,`:827`,`:2061-2067`; `vortex_core.cpp:14`,`:27-45`,
`:50-69`,`:100-127` (`findModule`/`exportSymbol`),`:196-254`
(`writeValue`/`splice` — `splice` at :251); `vortex_vm.cpp:50-67`
(`resolveCell`),`:73-82` (`mintOpcode`),`:98-105` (`setOpcodeKind`/
`getOpcodeKind`),`:136-145` (`setCursorOpcode`),`:148-161` (`spawnCursor`),
`:238-243` (backtrack PC restore),`:337`,`:343-346`,`:736-743` (`New`
bound-output refusal),`:771-806`,`:816-825`,`:854-893`,`:1220-1225`
(`Halt`),`:1269-1294`; `worker_context.hpp:52-54`; `mesh_actor.cpp:119-156`
(spawn, `WorkerContext()` mint),`:182`,`:188`,`:261`,`:271-273` (resume
store transport + two-step base/reader install),`:460-490` (`pumpOne` skip +
executor + `ActorDown` catch),`:659-662` (`pending`),`:668-694`
(`checkpointView` quiescence gates); `publication.cpp:1093-1103`,`:1097`
(`historyFromSeal` mints `Store()`, no id),`:1127-1131`
(`restorePublication` id + `setContentSource`/`bindPublishedLocalScroll`);
`store.cpp:1388-1405` (`addScroll` `sameContentAs`); `arena_manifold.cpp:34-46`
(`dimensions()` = base∪`arenaDims_`),`:55-76` (`dimensionNamed` base-first),
`:78-105` (`ensureDimension` mint + conditional `d.dims` append at :91-103),
`:274-292` (`attach` installs `base_`/`store_` + `d.store-refs`),`:1009-1012`
(`cloneMaster` = NEG `rankEnd`),`:1347-1377` (link eviction);
`arena_manifold.hpp:269-271` (`store()` const, base fall-through),`:648`,
`:652`,`:659-661`,`:773` ("marks are assumed well nested");
`mesh_evaluator.cpp:658-663` (`program.instructions` → `setOpcodeKind`),
`:735` (per-reference marker mint in loop); `vortex_stdlib.cpp:3800-3802`,
`:3841-3843` (drain idiom); `Makefile:643`,`:688-698`.
