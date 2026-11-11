# R2 critique — Implementer usability (ergo lens)

**Target:** `design/vagent-metaprogramming-implementation.md` **v2** (commit `441578e4`)
against spec `design/vagent-metaprogramming.md` v1.19 and the tree at HEAD.

**Verdict: NOT CONVERGED** — narrowly. Every r1 finding of mine closed, and the new
cites spot-checked are accurate save one drift. One MAJOR recipe hole remains inside
phase 5's exit (the "re-" in re-genesis is never mechanised), plus three MINORs and
two NITs. Nothing here reopens a spec ruling.

Counts: **0 BLOCKING / 1 MAJOR / 3 MINOR / 2 NIT**

## R1 per-item verification

| r1 item | Status | v2 evidence |
| ------- | ------ | ----------- |
| B1 substrate/preconditions | **closed** | Preconditions section (:20-48) names call ABI, adapter/executor, V10 dim-binding, linked-prelude path, and P4 wake as owed-and-owned-in-phase-6; every phase now carries a **Requires** line splitting kernel-simulable vs adapter-gated — checked below for honesty. |
| B2 genesis order inverted | **closed** | Phase 5 :323-350 now runs (d) `d.patch` replay → (e) `+d.refs` rehydration **last**, states the R7-B rationale verbatim (:347-348), and marks (f)=turns out of scope (:324). |
| M1 `std:meta` split | **closed** | Phase 3 :203-215 carries allowlist + signature rows + prelude skeleton + the full read surface (`module_named`/`test_of`/`body_of`/`program`/`agent_cell` all present); mutation verbs land in phase 4 :261-267. |
| M2 learn-boundary | **closed** | Session mint/record/register + `unlearn` read-side in phase 4 :275-284 (two-`reify`, `d.code`, `d.patch` append, `patch:<n>` mint off `d.vars`); monotone serial/no-ABA/alias/two-copy durable invariants in phase 5 :351-354 — matches the M-B ruling. |
| test homes named | **closed** | Phase 1 :128-131, phase 3 exit :243-245 (`tests/xuzz/vagent_meta_test.cpp` under the `xuzz_test` glob), cross-cutting :434-443 names the `vagent_runtime_test` filter-out trap and the `test/vagent-*` targets (all verified present in the Makefile). |
| runtime-test infra item | **closed** | Resolved by routing, not by a new suite: engine tests ride the `tests/xuzz/` glob, frontend tests `VAGENT_FRONTEND_TEST_SRCS` or the same glob — no Makefile fix is needed, so the item's substance (honest homes + `make test` coverage) is met. |
| `module_named`/`test_of`/`body_of` | **closed** | phase 3 :207-208. |
| `main.cpp` rewiring | **closed** | phase 1 :125-127 — `checkFile` drives the `frontend` pipeline, output contract preserved. |
| M4 leftovers (`staticFuel`, `learn_where`/`learn_body`, reflect surface) | **closed** | `staticFuel`→`ScalarCompilerLimits` :167-168 (confirmed absent from `compiler.hpp:22-25` today); all five `learn_*` :263-264; `set_weight`'s three-arm dispatch :265-266; dead-`let` erasure :148-150; reserved names both halves (:122-124 phase 1, :228-231 phase 3 — `src:`/`reify:`/`patch:` module-declaration refusal, spec :2500-2504 verified). |
| m1 19-vs-17 dims | **closed** | phase 5 :313-315 — "**17 system dims** (`vortex_core.cpp:50-69`) plus the `home`/`d.dims` genesis pair beside (`:35`,`:44`) — nineteen cells all told"; I counted exactly 17 `mintNamedDimension` calls at :50-69. |
| m2 Makefile follow-through | **closed** | `code_path.cpp`/`expand.cpp` join `VAGENT_FRONTEND_SRCS` in the phase that creates them (:82-84, :106, :113-115); test-file list named. |
| m3 logging categories | **closed** | cross-cutting :444-447 names `vagent.expand`, `vortex.meta`, `vortex.genesis`, `xanadu.adapter`. |
| m4 §9.4 instrumentation | **closed** | phase 2 exit :175-180 — §9.1 end-to-end under ScalarHarness, §9.4 expand+check via a test accessor, `static` baseline refusals kernel-linked. |
| m5 P15 spawn+resume arms | **closed** | phase 4 :256-260 — registration at *every* fresh-context bind, spawn *and* resume; "registered here, exercised at (b) — phase 5". |
| m6 phase-6 sub-step testability | **closed (mooted)** | the "independently testable" claim is gone; phase 6 is honestly "fully adapter-gated" (:398), so phase-end exits are the only kind possible. |
| m7 phase-5 risk | **closed** | rated `high` (:59). |
| n1 v1.18 cite | **closed** | :3 reads v1.19 (spec header confirms). |
| n2 phase-6 `1.` markers | **closed** | kept `1.` (MD029/mdformat) but made the **named steps** the citable units with an explicit note (:372-373): "Adapter API seams", "Genesis hook wiring", "P16 ceremony". Prompt question answered: **yes, citable** — internal refs use names and roman numerals ("(ii)'s take-or-verify"), no step is cited by number anywhere. |

New cites verified: `historyFromSeal`/`Store()` no-id (`publication.cpp:1094-1103`,
`make_unique<Store>()` at :1097 — plan says :1093-1103/:1097, close enough), the
`setContentSource`+`bindPublishedLocalScroll` pair (`publication.cpp:1129-1131` — inside
`restorePublication`, correctly cited as the pattern), `WorkerContext` two-arg ctor
(`worker_context.hpp:52-54`), `ActorExecution::message` non-optional (`mesh_actor.hpp:81`),
`pending` (`mesh_actor.cpp:659`), the `executor_` callback (`mesh_actor.cpp:474-476`),
`setOpcodeKind` (`vortex_vm.cpp:98-100`), `OpcodeKind::Call` frame mechanics
(`vortex_vm.cpp:854-893`), `localCells_` (`compiler.cpp:853-855`), `const Expr*`-keyed
kernel tables (`compiler.cpp:884-891`), `findOrMintDimension` via `dimConstant`
(`compiler.cpp:769-772`), `getOrCreate`'s `rebuildManifold` fold
(`dimension_registry.cpp:194-217`), `ensureDimension`/`arenaDims_`
(`arena_manifold.cpp:78-105`), thirteen V10 dims (`vagent-agent-language.md` V10 —
counted 13), P4 wake approval (`vagent-agent-language.md:1670`), Makefile lines
643/649/688-690/692/698/1147/1348-1350 — all accurate. Also verified the plan's
novel claims against the spec: the `patch:<n>` verbatim re-mint (spec :1274-1278 —
the "≥2 re-geneses" exit requirement at :360-361 is a genuinely good catch), the
`+d.entry` head-hop (spec :1141-1144), the `globalKeyOf` arm-set comparand
(spec :2235-2243), `learn_method`'s unlink-first (spec :2363-2364).

## Requires-line honesty audit (prompt question)

- Phase 0/1 "frontend only" — accurate.
- Phase 2 — honest: names that `vagent check` links no kernel (Makefile:1147) so
  `static` needs a kernel-linked harness.
- Phase 3 — honest: `run`, resolvers, `reify` flagged engine-level; linked-prelude
  execution and real `program()`/`agent_cell()` binds flagged adapter-gated. The
  bind set is harness-supplied via `ensureDimension` (exists today,
  `arena_manifold.cpp:78-105`) — the V10 *pattern* is owed substrate, the mechanism
  is not, so "harness-supplied" is constructible.
- Phase 4 — honest: under-execution guard arm flagged adapter-gated. The sideness
  conjunction now has its inputs (phase-3 bind record's `E_ops`) so `learn` isn't
  stuck fail-closed — the fail-closed arm is itself an exit case (:292).
- Phase 5 — "a local Store only" is *almost* honest: see M1, the one object-and-step
  shortfall.
- Phase 6 — "fully adapter-gated; (iv) implemented here, not assumed" — honest and
  the correct P4 disposition.
- Phase 7 — honestly gated on the full actor executor.

## Findings

### M1 — phase 5's exit needs a promote→refold→re-pin recipe that no step names; "a local Store only" is one object short (MAJOR)

**Plan:** phase 5 ordering :297-300 ("seed → mint/file → fold → pin/inject →
first-resolution"), Requires :356 ("phases 3–4's emission contract, bind set and
session records; a local Store only"), exit :360-364 ("learn→re-genesis→learn
cycles — ≥2 re-geneses").

**What the spec and tree show:** session mutations live in the `ArenaManifold`
overlay — dense vectors, `ephemeralBit|dense` refs, **no ops appended**
(`arena_manifold.hpp:14-15`). The folded `1..E_ops`/`>E_ops` band a re-genesis
reads comes only from `promote` appending the arena state into the store's spool —
the free function `zigzag::promote(Store&, parent, arena, root)`
(`arena_manifold.hpp:845`) — then `Store::rebuildManifold` refolding
(`store.hpp:451`). That is exactly what `publishValue` wraps, and the spec says the
publish destination is `arena.base()->store()` (spec :1765). The *other* candidate
path — the kept-in-step twin — is named **owed and absent** by both spec
(:1728-1733) and the plan's own phase-6 (iii) (:383-384), so it cannot be the
phase-5 mechanism.

The plan's stated ordering therefore specifies **first genesis only**. The exit's
core operation — the "re-" in re-genesis — requires: `promote(session arena → the
lineage/base store)` → `rebuildManifold(new head)` → fresh `WorkerContext` over the
fold → (a)–(e) → next `learn`. None of that is named; (d)'s install set exists only
if the session's `d.patch` records made it into the spool via promote, otherwise
re-genesis folds the un-mutated image and the exit tests nothing. Compounding it,
(b)'s `setOpcodeKind` writes `opcodeMap_`, which lives on `VortexVM` inside a
`WorkerContext` (`vortex_vm.hpp:186`) — so "a local Store only" undercounts the
harness's objects by the Manifold fold plus context pair (the two-arg ctor is cited
only in phase 6, :377).

**Suggested fix:** one sentence in the genesis-driver bullet: "the harness's
re-genesis = `promote` the session arena into the lineage/base store
(`arena.base()->store()` — the supply-arm shape), `rebuildManifold`, a fresh
`WorkerContext(base, reader)`, then (a)–(e) — the (e) ref-record arm stays dark
until P16 since no P16 flag ceremony runs here"; and Requires gains "…plus
`promote`/`rebuildManifold`/`WorkerContext` (all in-tree)". Kernel-simulable, so
the claim survives — it just isn't stated.

### m1 — `store.cpp:1388-1405` cites the wrong function (MINOR)

**Plan:** :306-308 — "`setContentSource` + `bindPublishedLocalScroll(store->addScroll(from))`
(`publication.cpp:1129-1131`, `store.cpp:1388-1405`)".

The `store.cpp` range covers `Store::addScroll` (:1388-1403). The claim's load-bearing
half — `bindPublishedLocalScroll`, whose absence produces the `""` dead-reads — is at
`store.cpp:1418`; `setContentSource` at `:1425`. The only new-cite drift I found in v2.
Fix: `store.cpp:1418-1427` (or cite both).

### m2 — "the validate cells" inside the filing list conflates bound constants with filed dim names (MINOR)

**Plan:** phase 5 :312-313 — filing enumerates "the phase-3 bind set above (the
apparatus dims and the validate cells)". Phase 3 :222-223 delegates the split:
"state which names are minted at bind vs. filed at filing."

The `validate` constants are **adapter-bound data cells**, not dim names
(spec :1053-1060 — "adapter-bound data, not prelude literals"); genesis (c)
re-binds them (:334-335). Whether they also carry filed `d.dims` names is a real
question the plan half-answers (they're listed under filing) and half-delegates
(the "state which names" imperative). An implementer can reconcile it — file the
twelve apparatus dims, re-bind the constants — but the wording invites filing
non-dim cells. Fix: "…the twelve apparatus dims (the `validate` constants are
re-bound at (c), not filed)" or state the constants' filing explicitly.

### m3 — the minimal call convention's exit is self-referential (MINOR)

**Plan:** phase 3 :225-227 prices the convention and instructs "state the
arg-binding coverage the convention delivers"; exit :240 — "`run(code"…")`
executes (nullary plus the convention's arg coverage)".

Whatever gets implemented defines what gets tested — no floor is named, so a
nullary-only convention passes the exit as written while the spec's `run(c, args)`
takes a list literal (spec table, `args` per-element `any`). Fix: name the floor
in the work item, e.g. "nullary plus positional args bound name-keyed on the
frame's `d.vars`/`d.values`" — then the exit cites a contract, not an
implementation detail.

### n1 — phase-6 step 1 lists seams (i)(ii)(iv)(v)(iii) (NIT)

:375-384. Deliberate — (iii) is conditional-last ("owed only if the kept-in-step
twin arm is kept") — but reads as a transcription typo until the parenthetical.
One clause ("(iii) last: conditional") would preempt. The named-steps device
otherwise works.

### n2 — `run`/`reify` under the "read group" heading (NIT)

Phase 3's bullet title "scaffolding + the read group" includes `run`
(spec: opaque-impure) and `reify` (impure) — inherited from the r1 disposition's
own grouping. "Non-mutation surface" is the truer label; harmless either way.

## Cross-cutting note (not counted)

v2's honesty improvements are real: the P16 "conditional on P16" spec line is
honoured by keeping the ref-record arm and marker mint in phase 6; "illegal-but-
uncheckable" body calls are flagged as a named interim limitation (:160-162); and
the "≥2 re-geneses" exit weaponises spec :1274-1278's second-genesis eviction into
a test. The plan now reads as buildable through phase 4; the M1 recipe is the last
unsized step in 5.
