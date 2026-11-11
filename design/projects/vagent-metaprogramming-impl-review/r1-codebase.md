# r1 — codebase-completeness critic (inventory coverage and citation truth)

**Verdict: NOT CONVERGED**

**Counts:** 0 BLOCKING / 7 MAJOR / 5 MINOR / 2 NIT

Scope of this pass: every §10.1 new-code row vs. the plan's work items; §10.2's
register (P15 call sites, the five owed adapter seams (i)–(v)); the plan's
load-bearing `file:line` cites against the tree at HEAD; the
`VAGENT_FRONTEND_SRCS`-vs-kernel-glob claim for each new TU.

Citation audit summary up front: every file:line cite the plan repeats was
checked against the tree and is **accurate** — `worker_context.hpp:52-54`
(two-arg `WorkerContext(baseManifold, reader)`), `mesh_actor.cpp:182` (`resume`
signature transporting `ReceivedValue checkpoint`), `:188` (`!checkpoint.store`
guard), `:261` (`checkpoint.store->userPermascrollPtr()` inside `receiveValue`),
`publication.cpp:1097` (`historyFromSeal` mints `Store()` with no id),
`store.cpp:123` (`Store::Store()` default ctor), `publication.cpp:1093-1103`,
`publication.cpp:1129-1131` (`setContentSource`/`addScroll`/
`bindPublishedLocalScroll`), `store.cpp:1388-1405` (`addScroll` `sameContentAs`
dedup), `vortex_core.cpp:196-254` (`writeValue` body + `splice` delegating to it
at :254), all five P15 call sites (`vortex_vm.cpp:771-806` = `executeOpcodeBody`'s
`Value`/`Splice` cases, `:1269-1273` = memo-hit writeback, `:1287-1294` = generic
output writeback, `:337` + `:343-346` = both `runPipeline` writes) and `:98-100`
(`setOpcodeKind`), `arena_manifold.cpp:34-46` (`dimensions()` base∪`arenaDims_`)
and `:91-103` (`d.dims` rank append only when `dimDims && home`, i.e. under a
base-supplied home), `vagent-agent-language.md` V10 (thirteen dims,
:224-235, and its one-cell-per-name checkpoint/resume test) and `:1670` (P4 row).
`Makefile:643` is the `apps/common/xanadu` recursive glob, `:688-690` the
explicit `VAGENT_FRONTEND_SRCS` list, `:695` `VAGENT_SYNTAX_OBJS` — all as the
spec's Makefile row states. The problems are what the plan *doesn't* say.

## MAJOR

### M1 — `staticFuel`/`ScalarCompilerLimits` dropped entirely

- **Plan ref:** Phase 1 limits bullet (line 62); Phase 2 `static` bullet
  (lines 79-82).
- **Claim:** Phase 1 threads `CheckLimits::maxExpansionDepth`/`maxSplices`;
  Phase 2 adds `static` evaluation ("no-mark vm run, cursor unlink, post-run
  baseline check").
- **Code/spec:** §10.1's closing note (spec :3070-3072) requires *two* limit
  changes: "a `staticFuel` entry in `ScalarCompilerLimits`
  (`compiler.hpp:23-26`) **plus** the two expansion budgets in `CheckLimits`
  (`checker.hpp:12-27`)". `ScalarCompilerLimits` today is exactly
  `{maxNodes, maxDepth}` (compiler.hpp:23-26); the spec's `static` semantics run
  `vm_.run(cursor, staticFuel)` (spec :233-234, :952). No phase names
  `staticFuel`.
- **Fix:** add "`ScalarCompilerLimits::staticFuel` (compiler.hpp:23-26)" to the
  Phase 2 `static` bullet beside the baseline check, mirroring the Phase 1
  `CheckLimits` line.

### M2 — `frontend` driver gets no call site; `vagent check` never runs resolve/expand

- **Plan ref:** Phase 0 `frontend.{hpp,cpp}` bullet (lines 41-42) and exit
  "`vagent check` unchanged" (line 46); Phase 1 exit "`vagent check`
  accepts/rejects `code"…"`" (line 64).
- **Claim:** the parse→resolve→expand→check driver is built in Phase 0; the
  binary's `check` is the acceptance surface in Phase 1.
- **Code/spec:** `apps/vagent/main.cpp:46-53` calls `Parser::parseProgram` then
  `xanadu::vagent::check(*program)` directly — no driver in between, and
  `check()` (checker.hpp:28) takes an already-parsed `Program`, so it cannot
  invoke `expand` internally. §10.1's `apps/vagent` row ("no new commands;
  `check` gains `code"…"` coverage", spec :2965) is only deliverable if
  `checkFile` (or a `check`-level entry) is rewired onto the frontend driver —
  otherwise splices stay unexpanded (residual-splice check unreachable),
  `static` never evaluates, and `expand`-side diagnostics never reach the
  binary the exits name. No phase names the rewire.
- **Fix:** add an explicit work item — Phase 0 exit can keep "`vagent check`
  unchanged," but Phase 2 must rewire `checkFile` to the driver (or state that
  `check()` gains a `checkSource` wrapper) so the `apps/vagent` row of §10.1 is
  actually delivered.

### M3 — `VAGENT_FRONTEND_SRCS` wiring named for 3 of 5 new TUs

- **Plan ref:** Phase 0 Makefile bullet (lines 43-44) vs. Phase 1
  `code_path.{hpp,cpp}` (lines 57-58) and Phase 2 `expand.{hpp,cpp}` (line 73)
  bullets, which carry no Makefile statement.
- **Claim:** "add all three to `VAGENT_FRONTEND_SRCS` (the list is explicit —
  the `cell_path.cpp` omission bug is precedent for forgetting)."
- **Code/spec:** §10.1's Makefile row (spec :3061-3063) lists **five** files:
  `expand.cpp, resolve.cpp, ast_clone.cpp, code_path.cpp, frontend.cpp`.
  `VAGENT_FRONTEND_SRCS` (Makefile:688-690) is the explicit list the `vagent`
  binary (`Makefile:1147`), `vagent_frontend_test` (:1151) and
  `fuzz_vagent_frontend` (:1054-1057) link; the `:643` glob only covers
  `XUDU_CORE_OBJS`/`xuzz_test`. The `cell_path.cpp` bug the plan cites is real —
  commit `0bd47bc1` is precisely the fix that added it to this list. If
  `code_path.cpp` is referenced by parser/checker for inner-offset diagnostics
  (the Phase 1 exit requires exactly that), the `vagent` binary fails to link —
  the same silent-missing-TU failure mode the plan warns about.
- **Fix:** state the list edit in each phase that lands a frontend TU:
  `code_path.cpp` in Phase 1, `expand.cpp` in Phase 2 (or fold all five into
  the Phase 0 Makefile bullet).

### M4 — Genesis step mislabeled and replay/rebind order inverted

- **Plan ref:** Phase 5 bullet (lines 144-146): "Genesis (e)/(f) split: (e)
  name-record rebind for registered heads *and* replay-installed copies; (f)
  `d.patch` replay after rebind — patch-installed symbols resolve to
  post-replacement bindings."
- **Claim:** `d.patch` replay is step (f) and runs *after* the rebind.
- **Code/spec:** spec §6.6 letters the steps (a) lineage bind, (b)
  `ensureDimension`+`setOpcodeKind`, (c) `programRef`→root bind, **(d)
  `d.patch` replay in ordinal order**, (e) `+d.refs` rehydration *last pass*
  (the name-record rebind lives inside it), **(f) turns** (spec :2346, :2381,
  :2440). The ordering is load-bearing and the *opposite* of the plan's:
  replay at (d) must precede the rebind because "a name-record's rebind must
  see **post-installation** symbols — under the naive pre-replay position a
  `learn_method`/`learn_plan` install-or-repoint replayed at (d) was invisible
  to it" (spec :2435-2439). "Patch-installed symbols resolve to
  post-replacement bindings" is the *purpose of rebind-after-replay*, not a
  property of replay-after-rebind — as written the plan reintroduces the
  misbind/drop class the spec's reorder removes.
- **Fix:** rewrite as "(d) `d.patch` replay in ordinal order precedes (e)'s
  rehydration sweep; the name-record rebind arm inside (e) covers registered
  heads *and* replay-installed copies ((d) hands (e) its install set), so
  rebind sees post-replacement bindings."

### M5 — `learn_where`/`learn_body` dropped from the `learn_*` enumeration

- **Plan ref:** Phase 4 bullet (lines 128-129): "`learn_plan`/`learn_method`/
  `learn_handler`/`unlearn` at session scope."
- **Claim:** three of the five `learn_*` twins are scheduled.
- **Code/spec:** §6.1's verb table (spec :1168-1174) makes each `graft_*` verb
  durable via a `learn_*` twin — `learn_plan`, `learn_handler`, `learn_where`,
  `learn_body`, `learn_method`. §6.6 spells out the where/body replay arms
  ("`learn_where`/`learn_body` re-point the target descriptor's resolved
  clause", spec :1891-1892) and the plan's own Phase 7 exit runs §9.2, the
  self-tightening-`where` example whose mechanism is `learn_where`
  (spec :1184). Phase 5's dedupe/replay items name only `learn_method`.
- **Fix:** enumerate all five `learn_*` twins in Phase 4 (or write
  "`learn_*` — all five `graft_*` twins"); the where/body pair also need their
  replay arms named in Phase 5's dedupe bullet.

### M6 — Adapter seam (iv), P4's `wake(pid)`, referenced but never scheduled

- **Plan ref:** Phase 6 step 1 (lines 161-167) lists four seams — (i) spawn-side
  base-injection, (ii) `resume` take-or-verify, (v) `documentId`-adoption,
  (iii) segment-apply — and step 4 mentions "(pending(pid)==1, P4's approved
  wake)" (line 179) only as a mechanism parenthetical.
- **Claim:** the owed adapter-API surface is the four items of step 1.
- **Code/spec:** §10.2 enumerates **five** owed surfaces — "(iv) P4's approved
  wake (`vagent-agent-language.md:1670`)" (spec :3099-3100) — and it is
  unimplemented: `mesh_actor` has `pending(pid)` (mesh_actor.cpp:659) but no
  `wake`, and `ActorExecution::message` is a non-optional `ActorMessage`
  (mesh_actor.hpp:81) — the register row states the change is `wake(pid)` plus
  `message`→`std::optional` "touching the existing mesh executor and its
  tests". The flag-driven re-kind/restore windows of step 4 ride on it.
- **Fix:** list (iv) in step 1 as its own work item — implement `wake(pid)` +
  the `ActorExecution::message` optionality + executor/test updates — keeping
  the conditional note on (iii).

### M7 — `programRef` bind record and the spawn-side bind set never named

- **Plan ref:** Phase 6 steps 2-4 (lines 168-181).
- **Claim:** the adapter work is lineage-store ceremony + dim filing + P16
  ceremony.
- **Code/spec:** three §10.1/§6.6 work items are absent: (a) the **`programRef`
  bind record** that "carries the recorded image extent and instruction
  enumeration genesis (a)/(b) assert" (spec :3057-3058, :1746-1747,
  :2321-2325) — the plan's Phase 4 P15 bullet consumes "the recorded
  instruction enumeration" and step 2's comparand consumes `E_ops`, but no item
  *produces* the record; (b) the **V10-mechanism bind set** — "program root +
  agent cell + `d.code`/`d.patch`/`d.weight`/`d.synapse`/`d.entry`/
  `d.patch-head`/`d.validate`/`d.refs`/`d.ref`/`d.recipe`/`d.refFor`/
  `d.mesh.relocation` + the validate ceiling, arity & impure-kind cells"
  (spec :3003-3007, restated §6.1 :1054-1058) — which also means step 3's
  "explicit vocabulary" omits the "spawn-seed bind set" arm of the spec's fixed
  filing list (spec :119-130 names it explicitly beside name-table
  enumeration); (c) genesis step **(c) `programRef`→program-root bind** the
  resolvers need for `code_named`/patch name resolution (spec :2344-2345).
- **Fix:** add a Phase 6 sub-step naming the `programRef` bind record (extent +
  instruction enumeration recorded at load) and the adapter bind sweep
  (program root, agent cell, the `d.*` std:meta vocabulary + validate
  constants), and add the bind set to step 3's filed vocabulary.

## MINOR

### m1 — "19 system dims" cite under-explains the count

- **Plan ref:** Phase 6 step 3 (line 174): "19 system dims
  (`vortex_core.cpp:50-69`)".
- **Code/spec:** `vortex_core.cpp:50-69` mints **17** dims
  (`d.grab`…`d.role`); 19 = those plus the `home`/`d.dims` genesis pair at
  `:35,:44` — the spec's parenthetical ("`d.grab`…`d.role`, `:50-69`, the
  `home`/`d.dims` genesis pair beside, `:35,:44`), nineteen cells all told"
  (spec :120-122). Count right, cite incomplete.
- **Fix:** add `:35,:44` to the parenthetical.

### m2 — Test-binary wiring unaddressed

- **Plan ref:** cross-cutting bullet (lines 213-215).
- **Code/spec:** `VAGENT_FRONTEND_TEST_SRCS` (Makefile:692) is an explicit list
  like the source list — new frontend unit tests (e.g. a
  `vagent_expand_test.cpp`) land in `xuzz_test` automatically via the
  `tests/xuzz` glob but not in `vagent_frontend_test`; `VAGENT_SYNTAX_OBJS`
  (:695) correctly stays unchanged (spec :3064-3066). Given the plan itself
  invokes the explicit-list precedent, one line naming which binary each
  phase's unit tests join is warranted.
- **Fix:** state "frontend unit tests join `xuzz_test` via the glob, or add to
  `VAGENT_FRONTEND_TEST_SRCS` for the dedicated binary."

### m3 — Phase 3 `run` exit leans on the §11.1 call ABI without naming the harness

- **Plan ref:** Phase 3 `run` bullet (lines 99-100) and exit "`run(code"…")`
  executes" (line 106).
- **Code/spec:** §10.1's note discharges the call ABI ("`#Call` + frame +
  spill + the `d.entry` head resolution") to "vagent §11.1 itself, not new debt
  here" (spec :3069-3070) — and vagent's own status records the actor executor
  as unbuilt. VM-level `Call` frame machinery exists (`vortex_vm.cpp:858-866`),
  so the exit is testable at the op level, but the plan never says whether
  `run` is proven through a `std:meta` prelude harness (itself unlinked today —
  "Vortex lowering and actor execution are not yet built") or a direct
  cursor/Call gtest.
- **Fix:** name the test harness for the Phase 3 exit and note the §11.1
  precondition for the user-level spelling.

### m4 — `reflect`/`validate` surfaces under-enumerated

- **Plan ref:** Phase 3 resolver bullet (lines 101-104); Phase 4 (lines 123-129).
- **Code/spec:** the reflect family is ten functions — `program`, `agent_cell`,
  `module_named`, `symbol_named`, `clauses_of`, `test_of`, `body_of`,
  `plan_named`, `code_named`, `label_of` (spec :162); the plan names five and
  leaves `module_named`/`test_of`/`body_of`/`program`/`agent_cell` implicit.
  `validate(c)` — the §7.2 per-op `maxCycles`-bounded walk riding `d.validate`,
  consumed by every `graft`/`learn` accept — is named nowhere.
- **Fix:** name `module_named`, `test_of`, `body_of`, `program`, `agent_cell`
  in the Phase 3 resolver bullet and `validate` in Phase 4's guards bullet.

### m5 — Stale spec-version reference

- **Plan ref:** header (line 3): "Companion to `design/vagent-metaprogramming.md`
  (v1.18, converged at review round 18)"; cross-cutting (line 217) "verified at
  v1.18".
- **Code/spec:** the spec is **v1.19** (:5) — the revision that records the
  P15/P16 approvals the plan's ordering logic leans on ("P15 lands inside the
  mutation phase", line 12).
- **Fix:** v1.19 in both places.

## NIT

### n1 — `CompilerBinding` marker bit is a header change filed under `compiler.cpp`

- **Plan ref:** Phase 3 bullet (line 97). `CompilerBinding` lives at
  `compiler.hpp:15-18`; §10.1 likewise nests it under "compiler.cpp additions",
  so the plan mirrors the spec — but the implementer edits the header (same
  file as `staticFuel`, which argues for a single Phase 3 `compiler.hpp` line
  covering both).

### n2 — "Kernel-glob TU" claim for `fragment.{hpp,cpp}` is right but elliptical

- **Plan ref:** Phase 3 (lines 94-96). Correct per `Makefile:643` — but only if
  the file lands under `apps/common/xanadu/` (the §10.1 tree puts it in
  `vagent/`); worth saying the directory explicitly since `vagent_runtime_test`
  filters *out* everything under `vagent/` (`Makefile:698`), so its fragment
  tests belong in `xuzz_test`.

## Coverage matrix (§10.1 → plan)

| §10.1 row | Plan phase | Status |
| --- | --- | --- |
| `parser.cpp` additions | 1 | covered (cite :848-855 verified) |
| `expand.{hpp,cpp}` | 2 | covered; Makefile join unstated (M3) |
| `resolve.{hpp,cpp}` | 0 | covered incl. Makefile |
| `frontend.{hpp,cpp}` | 0 | covered; no call site (M2) |
| `ast.hpp` record-row | 1 | covered |
| `ast_clone.{hpp,cpp}` | 0 | covered incl. Makefile |
| `fragment.{hpp,cpp}` | 3 | covered; kernel-glob claim verified (:643) |
| `checker.cpp` additions | 1/2/3/4 | all 12 items mapped (arity, fragment table, purity, arity table, `$` refusal →1; `static` closedness, position-mask, stamp-honoring →2; `run`/`value_of` →3; allowlist+builtins →4) |
| `compiler.cpp` additions | 2/3 | covered; `staticFuel` dropped (M1) |
| `code_path.{hpp,cpp}` | 1 | covered; Makefile join unstated (M3) |
| `stdmeta` prelude | 4 | covered (verb enumeration gaps: M5, m4) |
| `apps/vagent` check coverage | 1 (exit) | mechanism absent (M2) |
| adapter (lineage store, filing, binds, P16) | 6 | mostly covered; programRef record + bind set + genesis (c) absent (M7), seam (iv) unworked (M6), bind set missing from filing vocabulary (M7) |
| Makefile row | 0/1/2 | 3 of 5 TUs (M3); `VAGENT_SYNTAX_OBJS` no-op correctly implied (m2) |

§10.2: P15 → Phase 4 (all five call-site cites verified); P16 → Phase 6 step 4;
seams (i)(ii)(v)(iii) → Phase 6 step 1, (iv) referenced-not-scheduled (M6);
P17 → out-of-scope, consistent with spec.
