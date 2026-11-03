# vagent metaprogramming — phased implementation plan

Companion to `design/vagent-metaprogramming.md` (v1.18, converged at review round
18). That document is the *what* and *why*; this is the *order*. Phases are
dependency-sorted so each ships a usable capability and leaves the tree green.
Section numbers (`§`) refer to the design doc; `P15`/`P16`/`P17` to its §10.2
register; `U-M*` to its §15 deferred list.

Ordering logic: nothing runtime-mutating lands before the frontend can represent
fragments; nothing durable lands before mutation exists; nothing checkpoint-
surviving lands before the lineage-store and filing seams exist. P15 lands inside
the mutation phase — it is the precondition for trusting opcode claims once
program cells become writable surfaces.

## Phase map

| Phase | Capability delivered                            | Risk | Depends on |
| ----- | ----------------------------------------------- | ---- | ---------- |
| 0     | Frontend refactor extractions (no new semantics) | low  | —          |
| 1     | `code"…"` literal + `Code<T>` type surface      | low  | 0          |
| 2     | `expand` stage: `splice`, `static` (compile-time)| med  | 1          |
| 3     | Fragment emission: fragments become real cells   | med  | 2          |
| 4     | Runtime mutation + P15 + guards (session scope)  | high | 3          |
| 5     | Durability: `patch:<n>` serials, dedupe, genesis | med  | 4          |
| 6     | Checkpoint/resume: P16 + lineage store + filing  | high | 5          |
| 7     | Plasticity exercises: §8 weights, §9 examples    | low  | 6          |

---

## Phase 0 — frontend extractions (pure refactor, no behavior change)

**Goal:** pull the machinery `expand` needs out of `checker.cpp` so the new stage
shares it instead of duplicating it.

- `resolve.{hpp,cpp}` — extract the checker's env walk into a resolve-only pass
  stamping `Identifier` binding identities on `Expr`; `cloneExpr`/`cloneDecl`
  preserve the stamp (§5.6).
- `ast_clone.{hpp,cpp}` — `cloneExpr`/`cloneProgram` deep copy covering every
  `ExprPtr` field (children/scope/recipients/deadline/guard/recovery/parallel
  names); `Decl`-granularity for M13.
- `frontend.{hpp,cpp}` — the driver sequencing parse → resolve → expand → check;
  owns invocation order and per-literal resolve seeding (§10.1).
- Makefile: add all three to `VAGENT_FRONTEND_SRCS` (the list is explicit — the
  `cell_path.cpp` omission bug is precedent for forgetting).

**Exit:** tree builds, all existing tests green, `vagent check` unchanged.

## Phase 1 — `code"…"` literal and the `Code<T>` type

**Goal:** fragments parse, check, and carry types — without yet emitting runtime
objects. Compile-time surface only.

- `parser.cpp`: `code` in the `PrefixedStringLiteral` gate (§5.1, :848-855
  precedent from `cells"…"`); `Primary`/`Type` productions; `parseCodeBody`
  (#label/CodeParams header, inner-offset diagnostics, `#` stripped pre-lex).
- `ast.hpp`: `ExprKind::{Code,Splice,Static}` + the scope-stamp field.
- `code_path.{hpp,cpp}` — narrowing + `src:m:l:c+o:c` diagnostic mapping
  (mirrors `cell_path`).
- `checker.cpp`: `Code` scalar + 0-or-1 arity chain (arity 0 = wildcard; bare
  `Code` = parameterized wildcard); fragment table keyed by `SourceLocation`;
  purity flags; per-fragment arity table; `$`-bearing user-name refusal;
  `CheckLimits::maxExpansionDepth`/`maxSplices` counters threaded.

**Exit:** `vagent check` accepts/rejects `code"…"` programs per §5/§7.1; parser
and checker unit tests; diagnostics map to inner-literal offsets.

## Phase 2 — the `expand` stage: `splice` and `static`

**Goal:** compile-time metaprogramming works end-to-end — quasiquote, splice
substitution, staged evaluation — with zero runtime mutation. This is the
"compile time" half of the original ask, independently shippable.

- `expand.{hpp,cpp}` — the Program → Program stage (§5.6): code-body parse,
  scope walk over resolve's stamps, `cap$N` aliasing + env records, `$`-anchor
  rewriting, splice substitution, `Code`-splice `def` expansion, residual-splice
  check.
- `checker.cpp`: `static` closedness; position-mask re-verification; stamp-
  honoring (resolve owns name truth).
- `compiler.cpp`: merged-AST lowering; `static` evaluation — no-mark vm run,
  cursor unlink, post-run `choiceDepth`/`outstandingMarks` baseline check;
  `cap$N` `CompilerBindings` emission (cap$N lookups bypass `binding()`'s
  unvalued render check, read `externalReferences_` directly — §5.3).

**Exit:** §9.1 and §9.4 worked examples expand correctly; hygiene tests
(§5.3); expansion-depth/splice-limit refusals; `static` baseline-violation
refusals.

## Phase 3 — fragment emission: code becomes cells

**Goal:** a `code"…"` literal lowers to a real fragment object in the arena —
head + descriptor + recipe — and `run` can execute one. Runtime structure, still
session-scope.

- `fragment.{hpp,cpp}` — `compileFragment`: head cell, descriptor, `+d.refs`
  mark rank, `+d.recipe` emission around the kernel (§6.1, §10.1). Kernel-glob
  TU — no Makefile edit.
- `compiler.cpp`: `CompilerBinding` gains the state-side marker bit (the
  `closed` flag's provenance).
- `run` (§6.2): fragment-as-same-cursor-call; `run(c, [args])` special form +
  `value_of` in the checker.
- Resolvers: `label_of`, `clauses_of`, `code_named`, `plan_named`,
  `symbol_named` (module-`d.vars` walk → `+d.values` → op-or-head hop);
  `reflect`/`reify` over literal-born fragments via `+d.recipe` (U-M7's
  foreign-graph case stays deferred).

**Exit:** `run(code"…")` executes; fragment shape tests (`+d.refs`/`+d.recipe`
records verified by a test walker); resolver tests incl. qualified-name and
headless-`run` refusal.

## Phase 4 — runtime mutation and P15 (session scope)

**Goal:** `graft`/`prune`/`learn_*`/`unlearn` mutate the program graph in-session,
with the guards that make mutation trustworthy. *Before this phase lands, opcode
claims are unverifiable* — P15 is co-requisite, not optional polish.

- **P15** (§10.2): VM-installed kind predicate
  (`opcodeMap_.contains(masterOf(cell))`) at the `writeValue`/`splice` funnel —
  `vortex_core.cpp:196-254`; call sites `vortex_vm.cpp:771-806`, `:1269-1273`,
  `:1287-1294`, `:337`, `:343-346`; typed `success=false` refusals; carve-outs
  (`setCursorOpcode`, Call frames, `mintOpcode`, fresh cells); post-resume
  re-registration by `setOpcodeKind` over the recorded instruction enumeration —
  never a value scan (`vortex_vm.cpp:98-100`).
- `std:meta` prelude sources — Vortex-level routines linked like `std:agent`;
  checker import-allowlist + builtin member-signature table entries.
- `graft`/`prune` typed attachment points (§6.3); mutation-under-execution and
  under-marks rules (§6.4); sideness/`closed` guards; `run`-target rebind
  dead-end refusal.
- `learn_plan`/`learn_method`/`learn_handler`/`unlearn` at session scope —
  alias minting, priority append for handlers, method replace/unlink ordering.

**Exit:** mutation tests incl. guard refusals; a P15 write-refusal test per
funnel site; `reflect` after mutation reports true kinds.

## Phase 5 — durability: serials, dedupe, genesis replay

**Goal:** mutations survive *genesis replay* — durable identity and ordering —
still without checkpoint transport.

- `patch:<n>` verb-minted durable labels: monotone serial from the agent cell's
  `d.vars` counter, never rank positions; serials not recycled after `unlearn`
  (no-ABA); user alias separate (§6.6).
- Verb-replayed dedupe at genesis so a `learn_*` can unlink its declared/grafted
  match again after resume; `learn_method` replace-then-unlink ordering.
- Genesis (e)/(f) split: (e) name-record rebind for registered heads *and*
  replay-installed copies; (f) `d.patch` replay after rebind — patch-installed
  symbols resolve to post-replacement bindings.
- `d.code` separation; `closed` taxonomy with the `+d.name` carve-out and
  program-side carve-outs; capture-slot re-clone (`New`/`Link`/`Value` palette,
  never `Clone`).

**Exit:** learn→re-genesis→learn cycles in a test harness (genesis re-run
without transport); dedupe/no-ABA tests; anonymous declared handlers remain the
documented capability limitation.

## Phase 6 — checkpoint/resume: P16, lineage store, dim filing

**Goal:** learned mutations survive a real checkpoint→resume. The largest phase;
the design's hardest mechanics live here. Land in the listed sub-order — each
sub-step is independently testable.

1. **Adapter API seams** (owed supply-side surface, §10.2): spawn-side
   base-injection of the lineage `Store` through the `WorkerContext` two-arg
   ctor (`worker_context.hpp:52-54`); `resume` take-or-verify of the transported
   `Store` (`mesh_actor.cpp:182`,`:188`,`:261`); the `documentId`-adoption arm
   (`historyFromSeal` mints `Store()` with no id, `publication.cpp:1097`,
   `store.cpp:123`); the segment-apply surface — required only if the
   kept-in-step twin arm is kept.
1. **Lineage store ceremony** (§6.6): adapter constructs/keeps the per-agent
   `Store`; `historyFromSeal` seeding (`publication.cpp:1093-1103`) plus
   `setContentSource` + `bindPublishedLocalScroll(store->addScroll(from))`
   (`publication.cpp:1129-1131`, `store.cpp:1388-1405`); canonical adopted-band
   comparand, scroll-identity-aware via `scrollKey`.
1. **Dim filing** (r18-fixed): name-table enumeration + explicit vocabulary —
   19 system dims (`vortex_core.cpp:50-69`), 13 V10 `std:agent` dims with
   sweep-before-filing ordering, `d.store-refs`, `dimConstant` mints —
   file-before-pin, file-before-first-resolution.
1. **P16 ceremony** (§10.2): emission marking (`+d.refs` inventory incl.
   `+d.recipe` fields; `d.mesh.relocation→"cell"` markers), flag-driven
   re-kind/restore windows (`pending(pid)==1`, P4's approved wake), genesis
   rehydration by record-slot role (`call` vs `run` writeback split,
   `int64(entryOp)` vs `int64(head)`).

**Exit:** the P16 acceptance test — learn → checkpoint → resume → learn →
checkpoint round-trip; comparand-refusal test on permuted lineage; orphan-twin
test asserting exactly one cell per dim name after resume (the V10 test the
language spec already describes).

## Phase 7 — plasticity surface exercises

**Goal:** prove the design's stated purpose on the built machinery — no new
mechanism, all tests.

- §8.1 weight/synapse cells, `set_weight` arms incl. the junction arm.
- §8.3 Hebbian growth cycles (§9.3 worked example end-to-end).
- §8.2 plan-library-as-synaptic-structure; §8.5 program-search-is-cell-search.
- Full §9 worked-example suite as integration tests.

**Exit:** §9.2/§9.3 run green; a learned-weight schedule survives
checkpoint→resume (uses phase 5+6 machinery).

## Explicitly out of scope (all phases)

- Everything in §15 (`U-M1`–`U-M11`): metacircular compiler, remote `Code`
  mobility, autodiff, learned plan selection, AST-as-cells, suspending
  fragments, generic foreign-graph `reify`, `std:*` graft policy, `specialize`,
  `run_checked`, foreign-artifact heads.
- `P17` (`opcodeMap_`/`cursorPC_`/`arenaDims_` eviction-resync): deferred-not-
  registered; the containment discipline keeps it unnecessary.
- Anonymous declared handlers (capability limitation, documented).

## Cross-cutting requirements

- Every phase: `make -j$(nproc) test` green; new tests sit with their code
  (`tests/xuzz/` for engine-level, `tests/lib/` only for generic library
  behavior); headless env per AGENTS.md.
- Citation discipline: the design doc's `file:line` claims were verified at
  v1.18; implementation must re-verify each as code drifts — treat every cite
  as a starting coordinate, not gospel.
- Each phase is one or more logical commits; the §10.2 register entries (P15,
  P16, owed adapter seams) map to their own commits for reviewability.
