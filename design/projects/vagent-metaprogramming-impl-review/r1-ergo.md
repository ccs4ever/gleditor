# R1 critique — Implementer usability (ergo lens)

**Target:** `design/vagent-metaprogramming-implementation.md` (plan v1) against spec
`design/vagent-metaprogramming.md` **v1.19**, the tree at HEAD, and
`design/vagent-implementation.md`'s own status table.

**Verdict: NOT CONVERGED**

Counts: **2 BLOCKING / 5 MAJOR / 7 MINOR / 2 NIT**

Lens question: can an engineer execute this plan without re-deriving the spec? For
phases 0–2, mostly yes — the cites are good and the work items name real files. From
phase 3 onward, no: the plan assumes a runtime substrate (adapter, executor, call ABI,
agent cell, clause ranks, genesis driver) that does not exist and that no phase builds,
one genesis step is stated in inverted order, and the learn_*/durability split across
phases 4–5 is never defined.

---

## BLOCKING

### B1 — Phases 3–7 presuppose a vagent runtime that is not built by any phase and does not exist in the tree

**Plan:** the whole phase map (:17-26) and every phase-3+ bullet.

**What the plan claims:** phase 3 delivers "`run` can execute one" (:99-100);
phase 4 delivers `learn_*`/`unlearn` "at session scope" (:128-129); phase 5 delivers
genesis replay "in a test harness" (:151-152); phase 6 has "the adapter" construct and
keep the per-agent lineage `Store` (:168-169).

**What the tree actually shows:** there is no vagent adapter, executor, call ABI,
agent cell, or turn machinery.

- `build/vagent` implements only `check`. `apps/vagent/main.cpp:19-22` says so
  verbatim: "Cross-file module loading, Vortex lowering, agent execution and the REPL
  are not implemented in this iteration." The binary links only `main.cpp` +
  `VAGENT_FRONTEND_OBJS` (Makefile:688-690, :1147) — token/lexer/parser/checker/
  cell_path + vql lexer/parser + scanner_base. No kernel, no VM.
- `apps/common/xanadu/vagent/` holds 13 files: token, lexer, parser, checker,
  cell_path, compiler, ast.hpp. No adapter/executor/genesis TU anywhere under
  `apps/common/` (no `Adapter`/`adapter_*` symbols exist in the tree).
- `vagent-implementation.md`'s status table (:9-32) marks iteration 3 "remaining
  compilation open" (only the scalar kernel — `compileStatement` on expressions — is
  done, `tests/xuzz/vagent_compiler_test.cpp:18-46`) and iterations 4–9 all "Open":
  actor turns, BDI, effects, mesh, product integration.
- The spec itself names the debt external: "The `run`/`graft_*` verbs need the call
  ABI (`#Call` + frame + spill + the `d.entry` head resolution) — owed by vagent
  §11.1 itself, not new debt here" (spec :3069-3072).

**Consequences per phase, concretely:**

- Phase 3's `run(c, [args])` exit needs `#Call` frames, parameter spill, and
  `+d.entry`/`+d.values` descriptor deref at run time (spec :1372-1388) — none of
  which phases 0–2 build.
- Phase 4's `learn_plan`/`learn_handler` mutate `d.clause` ranks under goals/events
  and consume a serial off the *agent cell's* `d.vars` counter — BDI lowering
  (iteration 5) and the adapter-bound agent cell (iteration 4) do not exist.
- Phase 4's "sideness/`closed` guards" (:126) are worst. The spec's sideness test is
  a conjunction: outbound `program()`-walk reachability over the structural-dim
  vocabulary **and** a persistent non-`ephemeralBit` ref `≤ E_ops`, the extent
  recorded on the adapter's `programRef` bind record (spec :1640-1660, :1746-1748).
  The plan assigns the lineage store and that record to *phase 6*. In a store-less
  session the spec's own rule resolves to the fail-closed arm: "a prefix-less lineage
  … reports an empty program-side set, under which the discriminator **fails
  closed**: `learn` is refused outright" (:1771-1777). So a phase-4 `learn_*` either
  refuses everything (no mutation tests pass) or implements a session-scope
  approximation of sideness that the plan never defines and the spec never
  sanctions.
- Phase 5's exit "learn→re-genesis→learn cycles in a test harness" needs a genesis
  driver executing steps (a)–(e) and a lineage store to fold — the plan assigns the
  lineage-store ceremony to phase 6 sub-step 2 (:168-172) and dim filing to
  sub-step 3 (:173-176). Phase 5 cannot ship its exit before phase 6's machinery.

**Suggested fix:** add a **Prerequisites** section mapping each phase to the
`vagent-implementation.md` iterations it needs (3: call ABI + verified artifact load;
4–5: adapter + agent cell + `d.clause` ranks + BDI lowering; 5–6: genesis driver +
lineage store), and give each phase a `Requires:` line distinguishing "this plan
builds" from "vagent §11.1 owes". If the intent is instead that phases 3–6 build a
*test-owned mini-adapter* (a gtest harness that owns a lineage `Store` and drives
genesis passes without the actor runtime), say so explicitly and list the harness as
a work item — today the only line gesturing at it is phase 5's parenthetical
"(genesis re-run without transport)".

### B2 — Phase 5 states the genesis replay/rebind order inverted, with the wrong step letters

**Plan:** :144-146 — "Genesis (e)/(f) split: (e) name-record rebind for registered
heads *and* replay-installed copies; (f) `d.patch` replay after rebind —
patch-installed symbols resolve to post-replacement bindings."

**What the spec actually says:** `d.patch` replay is step **(d)**, run in ordinal
order, and it *hands its install set to* (e) — "the replay hands (e) its install
set … (e) iterates (d)'s install set plus the registered heads" (spec :2346-2380).
The `+d.refs` rehydration/rebind sweep is step **(e)**, "last genesis pass"
(:2381). Step **(f)** is "turns" (:2440) — not replay. The spec states the ordering
rationale explicitly and it is the opposite of the plan's: a name-record rebind run
*before* replay "bound the pre-replacement image binding and a call naming a
patch-installed symbol dropped the patch — the misbind/drop class the reorder
removes" (:2435-2439).

So the plan (i) inverts the two passes' dependency — replay must precede the rebind
sweep, not follow it; (ii) mislabels replay as "(f)", stealing the letter the spec
gives "turns"; and (iii) garbles the rationale: "patch-installed symbols resolve to
post-replacement bindings" is only true *because* rebind runs after replay sees the
post-installation symbols. An engineer implementing the plan's letter builds the
misbind/drop class the spec ordered genesis to remove.

**Suggested fix:** rewrite as two bullets matching spec :2344-2440 — "(d) `d.patch`
replay in ordinal order: verb re-enactment, verbatim alias re-match, install-set
hand-off to (e); (e) `+d.refs` rehydration last, sweeping registered heads **and**
replay-installed copies' regenerated records — name-record rebinds must see
post-installation symbols."

---

## MAJOR

### M1 — Phase 3 consumes `std:meta` machinery the plan schedules in phase 4, and the `std:meta` runtime binding has no phase home at all

**Plan:** :100-104 (phase 3 work list) vs :123-124 (phase 4).

**Claim vs. spec:** phase 3's work items and exit need `label_of`, `clauses_of`,
`code_named`, `plan_named`, `symbol_named`, `reflect`, `reify` — every one a
`std:meta` member (spec :162, :1148-1161) reachable in vagent source only through
`import std:meta`, which needs the checker allowlist (`checker.cpp:2061-2067`,
verified — currently accepts only `std:agent`/`std:goal`), the builtin
member-signature table (`:765-779`, verified), and prelude linkage "like
`std:agent`". The plan puts all of that in phase 4. Phase 3's "resolver tests" exit
is therefore unimplementable as scheduled — the resolvers can't even be imported.

Separately, `std:meta`'s *adapter-supplied bindings* — the program root cell, the
agent's own cell, the data-minted dims (`d.code`…`d.mesh.relocation`), and the
`validate` constants (Linked ceiling cell, per-op arity table, impure-kind set) —
are required by `validate`, `learn_*`, and `cells"…"` path resolution at *run* time
(spec :1053-1060, :2583-2592). No phase lists binding them; phase 4 covers only the
compile-time surface (allowlist + signature table).

**Suggested fix:** move `import std:meta` (allowlist + signature table + prelude
linkage) into phase 3 alongside the resolvers, and add an explicit work item — most
naturally in phase 4 beside the prelude sources — for the V10-style constant
binding surface that hands `program()`, `agent_cell()`, the dim constants and the
three `validate` constant cells to `std:meta`.

### M2 — The phase 4 → 5 boundary for `learn_*` emission vs. durability is undefined

**Plan:** :128-129 vs :139-149.

This is the exact question an implementer will hit on day one of phase 4, and the
plan doesn't answer it:

- `unlearn` is a phase-4 verb, but it "matches every verb by the recorded *target*
  name" on `d.patch` records (spec :1893, :1934-1939) — so `d.patch` records must
  already exist in phase 4.
- `patch:<n>` minting is listed under *phase 5* (:139-141). Yet learned clauses are
  re-found by resolvers only through their minted `patch:<n>` `+d.name`
  (`plan_named`, spec :1159, :1882-1886). Under the plan as written, a phase-4
  learned clause is either nameless — invisible to the `plan_named`/`label_of`
  resolvers phase 3 just delivered — or `patch:<n>` is minted early, in which case
  the phase-5 bullet misstates what lands there.
- `d.code` separation is a phase-5 bullet (:147). But §6.6's registration rule is
  what forces `learn`'s two-`reify` shape — registered head + installed copy
  (spec :1599-1604). If phase 4 doesn't register, its `learn_*` mint one copy;
  phase 5 then rewrites phase 4's verb internals. Unstated coupling either way.
- Phase 5's "capture-slot re-clone (`New`/`Link`/`Value` palette, never `Clone`)"
  (:148-149) is `reify`'s copy palette — but `reify` itself lands in phase 3
  (:103-104). Which part of the palette is phase-3 session work vs. phase-5 durable
  work is never said.

**Suggested fix:** state the split explicitly — e.g. "phase 4 mints the full patch
record, the `d.patch` rank entry and `patch:<n>` labels (the serial counter is
session-live anyway); phase 5 adds the *durability* properties: no-ABA across
genesis, `d.code` registration + the two-copy rule, replay dedupe" — or whatever
the intended split is, so phase 4's tests assert the right record shape.

### M3 — `apps/vagent/main.cpp` is never rewired to the new pipeline, so the `vagent check`-phrased exits test a stale driver

**Plan:** :41-46 (phase 0 adds `frontend.cpp`), :64 (phase 1 exit on `vagent
check`), :84 (phase 2 exit).

`checkFile` calls `Parser(source).parseProgram()` then `check(*program)` directly
(main.cpp:46-53). Phase 0 adds `frontend.cpp` "the driver sequencing parse →
resolve → expand → check", but no phase re-points `checkFile` at it. When phase 2
lands, `check` also gains the fragment-table parameter — a signature change
(spec :1022-1023) — which the existing call site cannot supply. Result: without a
main.cpp edit, `vagent check` runs the phase-0 pipeline forever, and every
"`vagent check` accepts/rejects" exit phrasing (:46, :64-65) measures the wrong
thing.

**Suggested fix:** add to phase 0 or phase 2: "`apps/vagent/main.cpp`: `checkFile`
drives the frontend pipeline (`frontend::` entry point), preserving the
`path:diagnostic` output contract."

### M4 — Missing §10.1 inventory items: `staticFuel`, `learn_where`/`learn_body`, and half the reflect surface

**Plan:** phases 2-4 work lists (:73-86, :99-104, :126-129).

- **`staticFuel`** — spec :3070-3072: "`static` needs a `staticFuel` entry in
  `ScalarCompilerLimits` (`compiler.hpp:23-26`) plus the two expansion budgets in
  `CheckLimits`." The plan threads `maxExpansionDepth`/`maxSplices` (:62) but never
  mentions `staticFuel`; without it phase 2's "no-mark vm run" for `static`
  (:79-80) has no fuel bound and a divergent `static` loops the compile.
- **`learn_where` / `learn_body`** — the spec's `learn_*` inventory is five verbs
  (spec :1850-1893); phase 4 names four (:128-129). `learn_where` is what §9.2's
  worked example calls (:2825) — the very example phase 7's exit runs (:198).
  The clause-re-point verbs are also the simplest members of the family (no
  clause minting), which makes their omission more likely a transcription gap
  than a scoping choice — either way it's unstated.
- **Reflect surface** — spec :162 lists `program`, `agent_cell`, `module_named`,
  `symbol_named`, `clauses_of`, `test_of`, `body_of`, `plan_named`, `code_named`,
  `label_of`. Phase 3 names five (:101-103). `module_named` is required by
  `plan_named`'s own qualified-path walk (spec :1917-1922); `test_of`/`body_of`
  mint the memoized reflect wrappers (spec :1835-1846) that §8.5's program-search
  and §9.3-style traversal rely on.

**Suggested fix:** add all four items to their phases' work lists (staticFuel →
phase 2; `learn_where`/`learn_body` → phase 4; `module_named`/`test_of`/`body_of` +
`program`/`agent_cell` → phase 3's resolver list).

### M5 — No runtime harness is named for any post-frontend exit, and `vagent_runtime_test` is a link-time trap

**Plan:** exits at :106-108, :131-132, :151-153, :183-186; cross-cutting :213-215.

`vagent` gains no `run` subcommand — spec :2965 ("`apps/vagent`: no new commands;
`check` gains code"…" coverage") and main.cpp confirms none exists. Every phase-3+
exit is therefore gtest-only, but the plan never says which binary, and the
Makefile layout has one real footgun:

- `tests/xuzz/*_test.cpp` is globbed into `xuzz_test` (`XUZZ_TEST_SRCS :=
  find tests/xuzz`, Makefile:649) which links `XUDU_CORE_OBJS` — the whole
  `apps/common/xanadu` tree including the vagent dir (Makefile:643-645, :1000).
  This is the only auto-discovered home that can link `fragment.cpp`, the compiler
  and the VM.
- `vagent_runtime_test` *excludes* all of `apps/common/xanadu/vagent`
  (`VAGENT_RUNTIME_CORE_OBJS` filter-out, Makefile:698 — "This suite isolates
  shared VM prerequisites"). A metaprogramming test placed there cannot link
  `fragment`/`compiler`/`expand` code at all.
- `vagent_compiler_test` links exactly one file (Makefile:1170); new files need an
  explicit rule edit.
- `make test` runs `xuzz_test` but never `vagent_*_test` (Makefile:1348-1350);
  `test/vagent-*` are separate targets.
- The `ScalarHarness` precedent (tests/xuzz/vagent_compiler_test.cpp:18-46 —
  arena + VortexCore + VortexVM + Compiler, compileStatement + spawnCursor/run)
  and `tests/xuzz/mesh_actor_resume_test.cpp` are the two existing harness shapes
  worth naming.

**Suggested fix:** name the home per phase in each exit line (e.g.
"`tests/xuzz/vagent_meta_test.cpp`, run under `xuzz_test` via the glob"), state
explicitly that `vagent_runtime_test` is off-limits by construction, and note that
`make test` covers the new tests only through `xuzz_test`.

---

## MINOR

### m1 — "19 system dims (`vortex_core.cpp:50-69`)" repeats the exact mislabel r18 corrected

**Plan:** :174-175. The cited range contains **17** `mintNamedDimension` calls
(`d.grab`…`d.role`, verified :50-69); "nineteen" is cells-all-told *including* the
`home`/`d.dims` genesis pair at :35,:44 — the spec's corrected phrasing is "`d.grab`…`d.role`
off `mintNamedDimension` (`vortex_core.cpp:50-69`), the `home`/`d.dims` genesis pair
beside (`:35,:44`), nineteen cells all told" (spec :120-123; the r18 changelog entry
records this exact mislabel being fixed). An implementer enumerating "the 19" at
:50-69 will count 17 and re-derive. Fix: "17 system dims at :50-69 plus the
home/d.dims pair at :35,:44 — 19 genesis cells all told".

### m2 — Makefile follow-through is stated only for phase 0

Phase 0 carries the `VAGENT_FRONTEND_SRCS` reminder (:43-44), but `code_path.cpp`
(phase 1, :57) and `expand.cpp` (phase 2, :73) join the same explicit list
(spec :3061-3066) with no per-phase reminder — a link error in `vagent`/
`vagent_frontend_test` is the failure mode (the XUDU_CORE glob picks them up for
`xuzz_test`, masking the omission there). Likewise new frontend test files need
explicit `VAGENT_FRONTEND_TEST_SRCS` adds (Makefile:692) to run under
`vagent_frontend_test`, and the 23-file `tests/samples/vagent/mini_*.vagent`
corpus — the existing `vagent check` vehicle — is never mentioned as the natural
home for `code"…"` positive/negative samples in phases 1-2.

### m3 — No logging/debug convention for the new passes

AGENTS.md mandates named spdlog categories over ad-hoc output. The plan never names
any, yet the hardest phases are exactly the ones that will need `SPDLOG_LEVEL`-
gated diagnosis: expand's substitution/`cap$N` renames, P15's per-site refusals,
`d.patch` replay, dim filing, and P16's re-kind/restore windows. Suggest the plan
name categories up front (e.g. `vagent.expand`, `vortex.meta`, `vortex.genesis`,
`xanadu.adapter`) so each phase doesn't invent its own.

### m4 — Phase 2's "§9.4 expands correctly" exit is under-instrumented

§9.4's program uses `plan`/`where`/`emit` — outside today's scalar kernel subset:
"only §9.1 is inside today's scalar subset" (spec :2905-2907). `static` *evaluation*
is kernel-side (compiler.cpp; `vagent check` never links the kernel). So "§9.4
expands correctly" is only observable as expand-stage *output* — the merged
`Program`/fragment table — and the plan names no inspection hook. Same for
"static baseline-violation refusals" (:85-86): the baseline check runs in the
kernel (`choiceDepth`/`outstandingMarks` post-run, :80-81), so it needs a
kernel-linked test, not `vagent check`. Fix: scope the exit as "§9.1 end-to-end
(expand→check→lower→run under ScalarHarness); §9.4 expand+check only, asserting
the merged AST/fragment table via a test accessor."

### m5 — Phase 4's P15 bullet bundles a phase-6-untestable requirement

"post-resume re-registration by `setOpcodeKind` over the recorded instruction
enumeration — never a value scan (`vortex_vm.cpp:98-100`)" (:120-122) is genesis
step (b) — it cannot be exercised until resume machinery exists (phase 6). Listed
bare under phase 4's P15 work items, an implementer may try to test it early.
Fix: mark it "(registered by P15, exercised at genesis (b) — phase 6)" or move it
to phase 6's bullet list where step (b) lives.

### m6 — Phase 6's "high" rating is honest, but "each sub-step is independently testable" is asserted, not operationalized

The claim (:159) is the phase's safety argument, yet the only exits are three
phase-end tests (:183-186). Per-sub-step exits would make it true: seams → spawn
constructs `WorkerContext` through the two-arg ctor with base+reader bound before
`initGenesis`; lineage ceremony → seeded store folds the band and `d.*`/label
names resolve (no "" dead-reads); dim filing → `dimensionNamed`-visible singletons
after a fold (the session-scope cousin of the orphan-twin test); P16 → the
round-trip. Alternatively split the phase (6a seams+lineage+filing, 6b P16).

### m7 — Phase 5's "med" understates its subtlety

Verb re-enactment at replay, verbatim alias re-matching against a re-folded rank,
the re-minted `patch:<n>` label pair, and the (d)→(e) install-set hand-off
(spec :2346-2380) is more intricate than anything in phase 2's "med" expand stage.
Not a correctness defect, but the risk column is the implementer's only
difficulty signal — suggest "high", or note that the subtlety concentrates in the
(d)/(e) hand-off.

---

## NIT

### n1 — Stale spec-version cite

Plan :3 says "v1.18, converged at review round 18"; the reference spec is **v1.19**
(header :5). The 1.18→1.19 delta is approval-status only (P15/P16 → approved,
which the plan honors), so harmless — but the cite should read v1.19.

### n2 — Phase 6 sub-steps all numbered `1.`

Plan :161, :168, :173, :177 all open with `1.` — renders as 1-4 in markdown but
reads confusingly in raw diff review, and references like "Phase 6 sub-step 2"
(used above) have no stable printed numbers to cite.

---

## Cross-cutting observation (not counted)

On exit-criteria granularity generally: the plan is at its best where it names the
test *class* and the funnel sites (`a P15 write-refusal test per funnel site` with
the sites enumerated — good), and at its worst where the exit names machinery that
doesn't exist yet ("run executes", "learn→re-genesis→learn", the P16 round-trip —
all fine as *goals*, untestable without B1's substrate). For "guard refusals" in
phase 4 specifically, the enumerable set the implementer should self-check against
is: §6.3's graft refusals (signature mismatch, effect-member, bound-output `New`,
M14 double-install), §6.4's drain-idiom/`consumeWork` discipline, sideness/`closed`
arms (a)-(c) — which in turn needs M1's `closed` flag and B1's lineage anchor to
mean anything.
