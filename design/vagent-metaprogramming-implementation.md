# vagent metaprogramming — phased implementation plan

Companion to `design/vagent-metaprogramming.md` (v1.19 — the r18-converged text plus the recorded
P15/P16 approvals). That document is the *what* and *why*; this is the *order*. Phases are
dependency-sorted so each ships a usable capability and leaves the tree green. Section numbers (`§`)
refer to the design doc; `P15`/`P16`/`P17` to its §10.2 register; `U-M*` to its §15 deferred list.
v2 repartitioned per the round-1 review (`r1-disposition.md` under
`design/projects/vagent-metaprogramming-impl-review/`); v3 applies the round-2 disposition
(`r2-disposition.md`, same directory) — genesis (e)'s dim-record arm, the re-genesis loop's
producer, and a minor bundle.

Ordering logic: nothing runtime-mutating lands before the frontend can represent fragments; nothing
durable lands before mutation exists; the phase-5↔6 boundary is *transport*, not *genesis* —
everything a local `Store` can host (lineage seeding, span-source binding, dim filing, the (a)–(e)
driver) sits in phase 5, and phase 6 keeps only the wire-crossing seams plus P16's flag windows. P15
lands inside the mutation phase: *sanctioned* mutation is what makes opcode-kind claims load-bearing
— `std:meta`'s guarantees and `validate` read kind truth, while raw `cells`+`Value` writes could
always forge it (documented-unguarded, §11) — so phase 3's `reflect`/`reify`/`run` ship under the
same pre-existing caveat, and phase 4's guards are what first make claims trustworthy.

## Preconditions — owed substrate this plan does not build

The spec's status line is explicit: vagent's "source frontend and the scalar lowering kernel exist;
the actor executor does not" — and `vagent-implementation.md`'s table shows iterations 3–9 open.
Four pieces of that substrate are consumed here and produced nowhere:

- **Call ABI** (vagent §5.8/§11.1 — `#Call` + frame + spill + the `+d.entry` head-hop attach
  convention): the VM-side frame mechanism exists (`vortex_vm.cpp:854-893`); the compiler-side
  per-activation binding does not — `localCells_` is the interim no-call shape
  (`compiler.cpp:853-855`). Phase 3 prices a fragment-local minimal convention as its own work item;
  the full §11.1 discipline stays owed.
- **The vagent adapter / actor executor** (iterations 4+): no `Executor` callback inside `pumpOne`,
  no adapter TU, no bound agent cell or `d.clause` ranks exist — everything the spec calls "the
  adapter" is greenfield. Phases 5–6 name harness substitutes where an exit can run without it.
- **V10's adapter dim-binding mechanism** (`vagent-agent-language.md` V10): the pattern `std:meta`'s
  bind set and genesis (b)'s `ensureDimension` sweep reuse — no-marks minting through
  `ensureDimension`.
- **The linked-prelude path** (iteration 3): `std:meta` is "linked like `std:agent`", and no
  linkable `std:agent` prelude source exists yet — member *checking* is testable once the checker
  tables land; member *execution through a linked prelude* is not, so phases 3–4 drive the routines
  engine-level.

Also owed — and *owned* here as a phase-6 work item: **P4's approved `wake(pid)`**
(`vagent-agent-language.md:1670`), unimplemented — `mesh_actor` has `pending(pid)`
(`mesh_actor.cpp:659`) but no `wake`, and `ActorExecution::message` is a non-optional `ActorMessage`
(`mesh_actor.hpp:81`).

Each phase carries a **Requires** line marking its exit kernel-simulable (provable in
`xuzz_test`-linked harnesses on today's tree) or adapter-gated (waiting on the substrate above).

## Phase map

| Phase | Capability delivered                                             | Risk | Depends on |
| ----- | ---------------------------------------------------------------- | ---- | ---------- |
| 0     | Frontend refactor extractions (no new semantics)                 | low  | —          |
| 1     | `code"…"` literal + `Code<T>` + skeleton expand + driver rewired | low  | 0          |
| 2     | Full `expand`: `splice`, `static`, `closed` flags                | med  | 1          |
| 3     | Fragment emission + `std:meta` non-mutation surface + bind set   | med  | 2          |
| 4     | Runtime mutation + P15 + session `learn_*`/`d.patch` records     | high | 3          |
| 5     | Durability: lineage seed, dim filing, genesis (a)–(e) driver     | high | 4          |
| 6     | Transport seams + P16 ceremony                                   | high | 5          |
| 7     | Plasticity exercises: §8 weights, §9 examples                    | low  | 6          |

______________________________________________________________________

## Phase 0 — frontend extractions (pure refactor, no behavior change)

**Goal:** pull the machinery `expand` needs out of `checker.cpp` so the new stage shares it instead
of duplicating it.

- `resolve.{hpp,cpp}` — extract the checker's env walk into a resolve-only pass stamping binding
  identities on `Expr`; `cloneExpr`/`cloneDecl` preserve the stamp (§5.6). The stamp carries
  scope-id on every `Expr`, plus binding-ordinal on `Identifier`s — `expand` seeds a body subtree
  from its literal's `Expr`, so the scope-id cannot live on `Identifier` alone.
- `ast_clone.{hpp,cpp}` — `cloneExpr`/`cloneDecl`/`cloneProgram` deep copy covering every `Expr`
  field — the `ExprPtr` children (scope/recipients/ deadline/guard/recovery/parallel names) and the
  value fields (`text`, `names`, `annotation`, `binding`, `loc`; `loc` is what keeps `+o:c`
  inner-offset diagnostics alive through substitution). `Decl`-granularity for M13.
- `frontend.{hpp,cpp}` — the driver sequencing parse → resolve → expand → check; owns invocation
  order and per-literal resolve seeding (§5.6, §10.1). It sequences an `expand` seam phase 1 fills
  with the skeleton and phase 2 with the real stage — the seam is named so phase 0's driver is not
  later seen as a stub that forgot a stage.
- Makefile: add all three to `VAGENT_FRONTEND_SRCS` (the list is explicit, Makefile:688-690 — the
  `cell_path.cpp` omission fix, commit `0bd47bc1`, is precedent for forgetting). `code_path.cpp` and
  `expand.cpp` join the list in the phases that create them, stated per-phase.

Ordering note: the `Expr` scope-stamp field is formally added by phase 1's `ast.hpp` item, but phase
0's resolve stamps it and ast_clone preserves it — the field is additive, so landing it early with
its consumers is harmless; the phase boundary stays because `ExprKind::{Code,Splice,Static}` is what
the field exists to serve.

**Requires:** today's tree only; frontend work, no kernel.

**Exit:** tree builds, all existing tests green, `vagent check` unchanged.

## Phase 1 — `code"…"` literal, `Code<T>`, skeleton expand, driver rewiring

**Goal:** fragments parse, bodies parse, the fragment table exists, and the binary runs the new
pipeline — with `splice`/`static` uniformly refused. Compile-time surface only.

- `parser.cpp`: `code` in the `PrefixedStringLiteral` gate (:848-855 precedent from `cells"…"`);
  `Primary`/`Type` productions; `parseCodeBody` (#label/CodeParams header, inner-offset diagnostics,
  `#` stripped pre-lex).
- `ast.hpp`: `ExprKind::{Code,Splice,Static}` + the `Expr` scope-stamp field (additive — see phase
  0's ordering note).
- `code_path.{hpp,cpp}` — narrowing + `src:m:l:c+o:c` diagnostic mapping (mirrors `cell_path`);
  joins `VAGENT_FRONTEND_SRCS` (Makefile:688-690, the same explicit-list edit as phase 0). Keep
  `parseCodeBody`'s diagnostics offset-only so the reporter does the `+o:c` mapping — otherwise
  `VAGENT_SYNTAX_OBJS` (a third explicit list, Makefile:695 — token/lexer/parser only) needs the
  same edit for `vagent`'s syntax binary to link.
- `expand.{hpp,cpp}` — **skeleton stage** (§5.6's shape, minimal content): per-`Code`-literal
  `parseCodeBody` invocation, per-literal resolve seeding off the literal's stamped scope, and
  **fragment-table emission** — keyed by `SourceLocation`/ordinal stamped on the `Code` node (§5.6;
  the `/ordinal` is what lets `cloneExpr`'d literal copies share the original's record). The
  skeleton walks the merged program *and* each literal body recursively — any `Splice`/`Static` node
  anywhere is a named `DiagnosticCode` refusal at expand, so `check` never meets either kind and
  needs no arm; nested literals emit records through the same recursion. Any of the new `ExprKind`s
  reaching `compileStatement` hits preflight's "construct is not supported" refusal
  (`compiler.cpp:500-502`) — the interim contract, so a `vagent check` acceptance never implies
  compilability. Joins `VAGENT_FRONTEND_SRCS` — the file exists from this phase, so the list edit
  lands here, not in phase 2.
- `checker.cpp`: `Code` scalar + 0-or-1 arity chain (arity 0 = wildcard; bare `Code` = parameterized
  wildcard); `Code` joins the `builtinTypes` shadow set (`checker.cpp:2069`); `check` gains the
  fragment-table parameter — `check(program, table)` lands *with its producer*; the signature change
  ripples to `vagent_checker_test.cpp`'s ~10 `check(*program)` call sites (the suite bypasses
  resolve/expand — a defaulted parameter, or a test-file edit beside `main.cpp`'s); the `Code` arm
  looks the record up and checks the body under the checker's own env walk (still the scope truth
  until phase 2's stamp-honoring) to compute `T`, purity and arity (§5.2, §7.1);
  `CheckLimits::maxExpansionDepth`/ `maxSplices` threaded counters (`checker.hpp:12-27`);
  `$`-bearing user-name refusal; §5.1's reserved-name refusals — `def`/`method`/
  `let`/`var`/parameter declarations named `splice` or `static`, which must land in the same phase
  as the parser change or a pre-existing `def splice` silently re-parses as the prefix form.
- `apps/vagent/main.cpp:46-53` rewiring — `checkFile` drives the frontend pipeline (the `frontend`
  entry point) instead of calling `Parser` + `check()` inline, preserving the `path:diagnostic`
  output contract; otherwise every `vagent check`-phrased exit measures a stale driver.
- **Test infrastructure**: frontend unit tests land under `tests/xuzz/` (globbed into `xuzz_test`,
  Makefile:649) or are added to `VAGENT_FRONTEND_TEST_SRCS` (explicit list, Makefile:692) for
  `vagent_frontend_test`; `tests/samples/vagent/mini_*.vagent` is the existing `vagent check` corpus
  and the natural home for `code"…"` positive/negative samples.

**Requires:** today's tree only; frontend, no kernel.

**Exit:** `vagent check` accepts/rejects `code"…"` programs per §5/§7.1 through the rewired driver;
`splice`/`static` refusal tests; reserved-name refusal tests; diagnostics map to inner-literal
offsets; fragment-table records asserted through a test accessor.

## Phase 2 — the full `expand` stage: `splice`, `static`, `closed`

**Goal:** compile-time metaprogramming works end-to-end — quasiquote, splice substitution, staged
evaluation — with zero runtime mutation. This is the "compile time" half of the original ask,
independently shippable.

- `expand.{hpp,cpp}` — the real stage's composition semantics (§5.6): scope walk over resolve's
  stamps, `cap$N` aliasing + env records, `$`-anchor rewriting, splice substitution via `cloneExpr`,
  `Code`-splice `def` expansion (its refusal edges: the named `recursive-template` `DiagnosticCode`,
  the control-flow bar in template bodies — `return`/`yield`/`consume`/ `terminate`, and the
  `effect`/`native`/`capacity`/`graft_method` bar), residual-splice check, the statically-known
  judgment (transitive, post-substitution, §5.2) — and **dead-`let` erasure** (§5.4): a `let` left
  dead by full substitution is erased, or refused where erasure is unsafe; mandatory for splice-only
  literals and for §9.1/§9.4's `let x := code"…"` residue. The fragment table is already emitted by
  phase 1's skeleton; this phase extends it to clones and splices and re-clones each record's body
  per emission — the `Emitter`'s per-`compileStatement` tables key on `const Expr *`
  (`compiler.cpp:884-891`), so a second emission of the same nodes would double-register
  `localReferences_`.
- `checker.cpp`: `static`'s checker-side refusal set, all four §5.5 arms — the `pureBody` impurity
  bar, the arena-reading *form* bar (`cells"…"` any mode, `believes`, `findall`, `run`, the implicit
  `self`/`parent`/`inner`/`outer`/`occurrence` names), the literal-expressible result-type rule
  (`Int`/`Float`/`Str`/`Bool`/`()` — a `Cell`/`Code` result is a stale-handle refusal), and
  closedness; position-mask re-verification; stamp-honoring (resolve owns name truth) — the
  un-stamped-input rule resolves to **env-walk fallback**: `check()` is a public API
  (`checker.hpp:30`) whose ~10 existing `vagent_checker_test.cpp` call sites never run resolve, so
  refusing un-stamped `Identifier`s breaks the suite — stamps are honored where present, the env
  walk covers their absence; a `Code` node with no table record is a named refusal. **Per-fragment
  arity/purity/`closed` tables** (§5.6) — `closed` is computed here off the per-fragment
  *outside-reference classification* `expand` leaves behind: `cap$N` captures and name-resolvable
  program refs both count *toward* `closed`; a program-side ref carrying neither is the refused
  class that clears it (§6.1's glossary, spec :158). The flag lands in these tables —
  `compileFragment` reads it into the descriptor in phase 3; `learn` re-verifies it in phase 4. Body
  `run(`/resolver calls stay illegal-but-uncheckable until phase 3's member surface — refuse them as
  unknown names, a noted interim limitation, not a regression.
- `compiler.cpp`: merged-AST lowering — a surviving `ExprKind::Code` or `Splice` (a `let`-bound
  literal not fully substituted, a `code` value passed to `run`) gets a named compile refusal, never
  an unhandled-kind path; `static` evaluation — no-mark `vm_.run(cursor, staticFuel)`, cursor
  unlink, post-run `choiceDepth`/`outstandingMarks` baseline check (`compiler.cpp:108-110`
  precedent); **`staticFuel` joins `ScalarCompilerLimits`** (`compiler.hpp:23-26`, spec :3070-3072 —
  the sibling of phase 1's `CheckLimits` budgets; without it a divergent `static` loops the
  compile); `cap$N` `CompilerBindings` emission (cap$N lookups bypass `binding()`'s unvalued render
  check and read `externalReferences_` directly — §5.3, `compiler.cpp:160-181`), including the
  `CompilerBinding` **state-side marker bit** §10.1 adds for `closed`'s provenance (spec :2958-2959
  — the struct is `{cell, type}` today, `compiler.hpp:15-18`; the bit is new work here, not an
  existing field).

**Requires:** the scalar kernel only; kernel-simulable in `xuzz_test`-linked harnesses (`static`
runs the VM, so `vagent check` cannot exercise it — the binary links no kernel, Makefile:1147).

**Exit:** §9.1 expands → checks → lowers → runs under the ScalarHarness shape
(`tests/xuzz/vagent_compiler_test.cpp:18-46`); §9.4 expand+check only — its `plan`/`where`/`emit`
are outside today's scalar subset (spec :2905-2907), so assert the merged AST/fragment table via a
test accessor; hygiene tests (§5.3 — merged-code `cap$N` resolution; capture-liveness and dead-frame
rules need phase-3 `run`); expansion-depth/splice-limit refusals; `static` refusals per §5.5 arm —
impurity, arena-reading form, non-literal result type, closedness — plus baseline-violation refusals
in a kernel-linked test.

## Phase 3 — fragment emission + the `std:meta` non-mutation surface + bind set

**Goal:** a `code"…"` literal lowers to a real fragment object in the arena — head + descriptor +
recipe — and `std:meta`'s non-mutation half is reachable through `import std:meta`. Runtime
structure, still session scope.

- `fragment.{hpp,cpp}` — `compileFragment`: head cell, descriptor — the signature-as-cells: ordered
  `(name, type)` parameter cells (count = arity), the result type name, the `pure`/`closed` flags,
  `effect`-ness, and the `result`/`produced` records `run` derefs through `+d.values` (§6.1, spec
  :914-920, :1377-1384); it carries phase 2's computed `closed` flag off the per-fragment tables,
  with the `CompilerBinding` marker bit phase 2 adds as its provenance — `+d.refs` mark rank,
  `+d.recipe` emission around the kernel (§6.1, §10.1). The **`+d.refs`/ `+d.recipe` record schema
  is this phase's emission contract**, stated in full: the `+d.name`-on-record name-record vs
  ref-record discriminator, the `+d.grab→"call"/"run"` role labels, the `+d.refFor` slot pairing,
  `+d.name` on a `+d.recipe` member reserved to the `dim` field's dim-name channel — consumed by
  phase 4's `closed` re-verification and phase 5's (e) rebind, and regenerated by `reify` to the
  same schema. Capture-slot wing emission (§5.3: the operand wraps the binding cell on its wing,
  decoded via `masterOf(slot)`). Kernel-glob TU under `apps/common/xanadu/vagent/` — §10.1's tree
  (spec :2944) — no Makefile edit (Makefile:643); the `vagent/` placement is load-bearing, since the
  `vagent_runtime_test` filter-out removes only that directory.
- **The `programRef` bind record's emission half** — the recorded image extent `E_ops` plus the
  recorded instruction enumeration (`produces` `MicroversionId`s or canonical post-fold positions,
  spec :3057-3058, :2311-2326), written at emission beside `compileFragment`'s marking; consumed by
  phase 4's P15 registration pass, phase 5's (a) comparand and (b) re-registration, and `learn`'s
  `≤E_ops` sideness bound.
- **`std:meta` scaffolding + the non-mutation surface** — the spec's *read group* proper is the pure
  top half; `run` (opaque-impure) and `reify` (impure) ride the same side of the *functional* split
  — checker import-allowlist gains `std:meta` (`checker.cpp:2061-2067`); the builtin
  member-signature table gains the read rows (`checker.cpp:765-779`); a prelude-source skeleton in
  the `std:agent` style (prelude sources live beside the frontend — `vagent/prelude/`; nothing loads
  them yet — execution through a *linked* prelude awaits vagent iteration 3, so this phase's tests
  drive the routines engine-level). Members: `program`, `agent_cell`, `module_named`,
  `symbol_named`, `clauses_of`, `test_of`, `body_of`, `plan_named`, `code_named`, `label_of` (the
  `reflect` family, §6.1/§6.7 — the qualified-name space, first-`:` split, `code_named`'s two-space
  rule); `test_of`/`body_of` memoize clause-side — the `clause +d.code → +d.step` record chain, each
  record carrying `+d.values →` the wrapper head, `+d.name →` the memo-kind label (a fresh text cell
  per record — an interned label could hold only one `-d.name` edge), and `+d.grab →` a fresh
  content-compared scalar key cell; a label-less attach cell gets no record at all (spec :311-331 —
  unmemoized wrappers leak a ~6-cell head per call); `validate` (the §7.2 per-op `maxCycles`-bounded
  walk on `d.validate` — `graft`/`learn`'s accept gate); `weight_of`, `value_of`, `next_rand`;
  `run(c, [args])` as the checker special form **under the import** — unimported, a user `def run`
  still resolves to the user's decl; `reify` over literal-born fragments replaying `+d.recipe` — its
  `reify:<src>:<serial>` label mints off the agent cell's `d.vars` counter, the same counter
  `patch:<n>` draws (spec :2504-2509), and its capture-slot re-clone palette is
  `New`/`Link`/`Value`, never `Clone` (U-M7's foreign-graph case stays deferred). The `run`-target
  head-vs-op discriminator — resolve an op directly, follow `+d.entry` once on a head (spec
  :1141-1144) — is a read rule and lands here with the readers, not with the verbs.
- **The adapter bind set, harness-supplied in this phase** — `std:meta`'s runtime roots: the program
  root cell, `agent_cell()`, the twelve data-minted apparatus dims
  `d.code`/`d.patch`/`d.weight`/`d.synapse`/
  `d.entry`/`d.patch-head`/`d.validate`/`d.refs`/`d.ref`/`d.recipe`/ `d.refFor`/`d.mesh.relocation`,
  and the `validate` constants (Linked ceiling, per-op arity table, impure-kind set — §6.1
  :1053-1060), minted through the V10 mechanism on the no-marks path. Resolvers walk these roots, so
  the phase's tests supply them; phase 5's filing owns their persistent form — the twelve apparatus
  *dim names* file there, while the bound non-dim members (program root, agent cell, `validate`
  constants) bind at genesis (c) and never file.
- **`run`'s minimal call convention** — priced work: the `#Call` frame convention (the frame writes
  of `vortex_vm.cpp:854-893`) — the operand slot stays head-valued, never a `Call`-opcode operand
  (`Call` executes `toInt(inputs[0])`, an entry-valued slot that cannot name a head — spec :259-262)
  — `d.vars`/`d.values` name-keyed binding + `result`/`produced` spill + the `+d.entry` head hop,
  kernel-simulable under the harness. The coverage floor is nullary plus positional args bound
  name-keyed on the frame; the statement of *which binding classes are frame-bound* lands before the
  capture-slot refusal rules — `def`/plan/handler parameters *and* `let` locals are frame-bound
  under §5.8 (spec :847-854), and frame-bound captures are the class the `cap$N` anchor rules refuse
  (§5.3). §11.1's full frame discipline remains owed.
- Reserved-name refusals, this phase's half: `run`/`value_of` declarations and bindings refused
  *when `std:meta` is imported* (§5.1 — unimported, a user `def run` stays legal); the
  `src:`/`reify:`/`patch:` reserved-prefix refusal at module declaration (§6.7 :2500-2504 — those
  label spaces are minted by this phase and phase 4).
- **No-marks dim-minting discipline** — `reify`'s `ensureDimension` name arm may only resolve
  already-filed/bound names; a would-be mint under live marks is a named refusal (`arenaDims_` is an
  untrailed map, `arena_manifold.cpp:78-105` — the P17-class desync §6.5 rules out).

**Requires:** kernel-linked harness (the ScalarHarness shape); bind set harness-supplied; minimal
call convention per above. Kernel-simulable: `run`, resolvers, `reify` engine-level. Adapter-gated:
`std:meta` executed through a linked prelude, real `program()`/`agent_cell()` binds.

**Exit:** `run(code"…")` executes — nullary plus positional args bound name-keyed on the frame, the
convention's coverage floor; fragment shape tests asserting the full record schema (name-record vs
ref-record, role labels, `+d.refFor`); resolver tests incl. qualified-name and headless-`run`
refusal (engine-level, not through a linked prelude); `import std:meta` accepted and non-mutation
calls check; a `reify` mint-under-marks refusal test. Test home: `tests/xuzz/vagent_meta_test.cpp`
under `xuzz_test` — `vagent_runtime_test` filter-outs all `vagent/` objects (Makefile:698-699) and
cannot link this code.

## Phase 4 — runtime mutation, P15, and the session `learn_*` records

**Goal:** the mutation verbs change the program graph in-session, with the guards that make mutation
trustworthy — and the durable records the verbs write *at learn time* exist here, since `unlearn`
and the resolvers read them immediately.

- **P15** (§10.2): VM-installed kind predicate (`opcodeMap_.contains(masterOf(cell))`) at the
  `writeValue`/`splice` funnel — `vortex_core.cpp:196-254`; call sites `vortex_vm.cpp:771-806`,
  `:1269-1273`, `:1287-1294`, `:337`, `:343-346`; typed `success=false` refusals; carve-outs
  (`setCursorOpcode`, `Call` frame writes, `mintOpcode`, fresh cells). The companion **registration
  pass** — `setOpcodeKind` over exactly the recorded instruction enumeration, never a value scan
  (`vortex_vm.cpp:98-100`) — runs at *every* fresh-context image bind, **at spawn as after
  `resume`**: `opcodeMap_` is per-`WorkerContext` and holds none of the folded image's ops at either
  (spec :2335-2343). Registered by P15 here; exercised at genesis (b) — phase 5.
- `std:meta` mutation surface: signature-table rows and prelude routines for
  `graft_plan`/`graft_handler`/`graft_where`/`graft_body`/`graft_method`, `prune`,
  `learn_plan`/`learn_method`/`learn_handler`/`learn_where`/ `learn_body` — all five `graft_*`
  durable twins (§6.1 :1168-1174; §9.2's worked example calls `learn_where`) — `unlearn`,
  `set_weight` with its full dispatch (junction arm writes its own `+d.weight` scalar; clause arm
  mints the keyed `d.vars` record on first write; a `+d.name`-less clause refused — §8.1),
  `sprout_synapse`.
- `graft`/`prune` typed attachment points (§6.3); mutation-under-execution and under-marks rules
  (§6.4); **sideness/`closed` guards** — the closed taxonomy's conjunction (outbound
  `program()`-walk reachability over the structural-dim vocabulary *and* a persistent ref `≤ E_ops`
  on the phase-3 bind record, with the `+d.name` carve-out and the program-side carve-outs),
  re-verified at `learn` accept — fail-closed where no lineage/bind record exists (the *no program
  lineage* diagnostic, §5.6's §6.6 arm); rebind refusals, named by class: §6.6's emission-fixed
  unwritten-slot refusal (fail-until-resume vs fail-forever).
- **Session `learn_*`/`unlearn` machinery** — at learn time, in-session (§6.6 :1850-1893): two
  `reify`s (registered head + installed copy — the two-copy rule, with an already-registered
  argument reused under §6.6's carve-out); `d.code` registration of the fresh uninstalled `reify`;
  the `d.patch` record append carrying the ordinal, target descriptor, the recorded target name,
  `+d.patch-head` record chain, `+d.name→patch:<n>` label and `learn_handler`'s `priority` — the
  verb **mints `patch:<n>`** off the agent cell's `d.vars` counter while the recorded target name
  rides the record un-minted: the user alias for clause-installing verbs, `learn_method`'s
  functional lookup name — *never an alias*, and what `unlearn`'s read side matches (spec
  :1851-1853); `unlearn`'s read side (latest patch matching the recorded *target* name; a
  `d.patch`-record argument refused by its `-d.patch` backlink). `learn_method` is
  unlink-then-install — redefinition-as-replacement rides `graft_method`'s own verb (spec :1172,
  :2363-2364). The `learn_*` rows are spec'd "conditional on P16" (spec :91, :156, :1174) — honored
  by the split itself: the session machinery lands here; the transport-dependent durability it
  serves waits for phase 6's P16 ceremony, so nothing here falsely inverts into transport work.

**Requires:** phase 3's bind set, emission contract and minimal call convention. Kernel-simulable:
session mutation, the write-funnel refusals, the mark/trail arms of the guards. Adapter-gated: the
under-*execution* (turn-boundary) arm of §6.4 — actor turns do not exist yet.

**Exit:** mutation tests incl. guard refusals; a P15 write-refusal test per funnel site; `reflect`
after mutation reports true kinds; `learn_*`→`unlearn` round-trips asserting the `d.patch`/`d.code`/
`patch:<n>` record shape; the fail-closed `learn` refusal with no lineage.

## Phase 5 — durability: lineage store, filing, genesis (a)–(e)

**Goal:** mutations survive *genesis replay* — durable identity and ordering — over everything a
local `Store` can host; no wire transport. The ordering discipline on the spawn and resume lineages
is **seed → mint/file → fold → pin/inject → first-resolution** — `getOrCreate`'s rebuilt fold *is*
the injected base (`dimension_registry.cpp:194-217`), so a filing after the pin lands outside the
sealed fold and the arena mints orphan twins (spec :3031-3039). The emit lineage takes the
compressed form — file before first resolution; its freeze is the compile arena's `initGenesis`
itself.

- **Lineage-store construction + seeding** (§10.1 — a `Store` is a local object, so none of this
  needs transport): the adapter (harness here) constructs and keeps the per-agent `Store` — the
  `job.base` pattern (`mesh_jobs.cpp:304-318`) — seeds it on the artifact's sealed ops via
  `historyFromSeal` (`publication.cpp:1093-1103`, minting `Store()` with no id at :1097) and binds
  its span source — `setContentSource` + `bindPublishedLocalScroll(store->addScroll(from))`
  (`publication.cpp:1129-1131`; the three bodies at `store.cpp:1388-1425` — the span-source pair
  itself sits at :1418/:1425); without the pair every artifact-local span name dead-reads `""`.
- **Dim filing** — the minted-dim vocabulary on the kept lineage `Store`'s persistent `d.dims` rank
  before the folded base is pinned, through `getOrCreate`/`Store::makeDimension` (spec :3020-3026 —
  the same mechanism `:194-217` runs), enumerated off the arena's recorded **name table** (the
  `ensureDimension`-written, `dimensionNamed`-read surface, base∪`arenaDims_`,
  `arena_manifold.cpp:34-46`) plus the fixed list — filing is name-table entries, not cells: **the
  bind set's name-bearing members** — the twelve apparatus dims — file here, while its bound non-dim
  members (program root, agent cell, `validate` constants) bind at (c) and never file; the **17
  system dims** `initGenesis` mints (`d.grab`…`d.role`, `vortex_core.cpp:50-69`) plus the
  `home`/`d.dims` genesis pair beside (`:35`,`:44`) — nineteen cells all told; the thirteen V10
  `std:agent` dims the adapter `ensureDimension`-mints *before* filing runs
  (`vagent-agent-language.md` V10); `attach()`'s `d.store-refs` (`arena_manifold.cpp:289-290`);
  every `dimConstant`/`findOrMintDimension` mint (`compiler.cpp:769-772`). The emit lineage obeys
  the twin rule — the artifact's own image emission files the same vocabulary before *its* compile
  arena's `initGenesis` (`vortex_core.cpp:14` — the ctor's call; the body at `:29-70`) — an
  owed-substrate note, the artifact-emission pipeline being outside this plan. Coverage is names
  whose edges can fold; an unfiled session-minted dim accretes one `dimensionNamed`-invisible orphan
  per checkpoint — the honest residual (spec :2397-2402; today's unfiled session mints are
  `d.op-label` at `mesh_value.cpp:473` and `or_parallel`'s four —
  `d.or`/`d.branch`/`d.frontier`/`d.submission` at `or_parallel.cpp:181-184`).
- **The genesis (a)–(e) driver** — §1's priced `std:agent` genesis hook: the orchestrated pass,
  harness-invoked in this phase, wired to the real spawn/resume paths in phase 6; (f) is turns and
  stays out of scope here. The harness's **re-genesis loop** is named machinery: session mutations
  live in the `ArenaManifold` overlay and append no ops, so each cycle promotes them into the fold —
  `promote(Store&, parent, arena, root)` (`arena_manifold.hpp:845-847`, into `arena.base()->store()`
  — the publish-destination shape) → `Store::rebuildManifold` over the new head → re-attach through
  a fresh `WorkerContext(base, reader)` (`worker_context.hpp:52-54` — fresh because (b)'s
  `setOpcodeKind` writes `opcodeMap_` on the context's `VortexVM`) → re-run (a)–(e). A same-`Store`
  cycle needs no `sealLocalSpool` — op records and local spans ride verbatim; the initial image band
  comes from `promote` of the compile arena or `adoptOpRecords` (`store.cpp:3836-3940`). In spec
  order:
  - (a) **lineage bind** — the ops-record order comparand over the decoded `1..E_ops` band (every
    *non-scroll* post-fold `OpRecord` field verbatim, post-remap, plus scroll *identity* by key —
    `globalKeyOf`'s arm set minus `localScroll`: `breakMarkerKey` for sealed break-marker spans,
    `scrollKey` for engaged externals, any other disengaged id failing outright, spec :2235-2243);
    per-cell `≤E_ops`/`>E_ops` classification; fail-closed on a prefix-less lineage. The comparand
    is *pairwise* — adopted lineage band vs received band (spec :2228-2232); a single-store fixture
    degenerates it, so its refusal arms exercise a second `historyFromSeal`/`adoptOpRecords`
    "received" store (foreign band or missing prefix, spec :2249-2258).
  - (b) **two arms, not one scope**: `ensureDimension` find-or-mint over the dim *names* (filed name
    → canonical cell, unfiled → re-mint — spec :2294-2299, :2401-2402 — the V10 sweep included), and
    `setOpcodeKind` re-registration over the phase-3 recorded *instruction enumeration* (op cells,
    `vortex_vm.cpp:98-100`) — both run at every fresh-context image bind (spawn *and* resume); the
    phase-4-registered P15 machinery is what the second fills.
  - (c) **`programRef` → program root bind** (spec :2344-2345), plus the bind set's non-dim members
    — agent cell, `validate` constants; the apparatus dims' names already resolved through (b)'s
    `ensureDimension` arm.
  - (d) **`d.patch` replay in ordinal order** — verb re-enactment: `learn_plan`'s verbatim alias
    re-match against the re-folded rank (displacing the superseded declared clause again),
    `learn_method`'s unlink-first, `learn_handler`'s re-append with `priority`,
    `learn_where`/`learn_body`'s clause-edge re-point (`+d.grab`/`+d.spin` at the new head).
    **Replay does not re-record** — replayed grafts mint no `d.patch` cells and consume no
    `patch:<n>` serial, though each install copy's own `reify:<src>:<serial>` label still mints off
    the shared `d.vars` counter (spec :2346-2350, :2504-2509). Dedupe so a `learn_*` unlinks its
    declared/grafted match again on re-genesis; the installed copy's `patch:<n>` label is a
    **verbatim re-mint** — a fresh label cell written with the record's text, never a re-link of the
    record's own label cell, whose `-d.name` occupancy would evict and drop the label at the
    *second* genesis. The copy's ref slots derive endpoints from the *paired donor slot's promoted
    content* — the `+d.name`-bearing dim record excepted, where the record's `+d.name` *is* the
    endpoint source and the slot takes `Int64` of the resolved `DimRef`, not the donor's promoted
    twin (spec :2364-2372). Replay hands (e) its install set — the copies hang off program-side
    anchors no head-only traversal reaches.
  - (e) **`+d.refs` rehydration — last pass, three record arms** — the name-record role-split rebind
    (`call`-role `int64(entryOp)` vs `run`-role `int64(head)`) over registered heads *and*
    replay-installed copies' regenerated records; and the **dim-record arm** — `+d.name`-bearing
    `+d.recipe` records (ref-capture dim records and `dim`-field members alike) write
    `Int64(DimRef)` re-resolved through `ensureDimension`'s name arm into the `+d.refFor`-paired
    slot or the member cell itself — a filed-cell rebind, *never* the promoted twin `*handleTarget`
    names (spec :2395-2406, esp. :2397-2398). Emission-time `DimRef`s are stale compile-arena ids,
    so unlike the ref arm this is *not* a no-op pre-P16 — it runs at every genesis off the filed
    rank. It runs after (d) precisely so name-record rebinds see post-installation symbols (the R7-B
    ordering, spec :2435-2439). The ref-record arm — per-record `Int64(*handleTarget)` writebacks —
    stays phase 6 with P16: a no-op here because nothing mints `OpHandle`-kind cells pre-P16
    (`writeValue` covers Int64/Double/Bool/None only, `vortex_core.cpp:196-229`); the re-kind is
    phase 6's marker ceremony.
- **Durable semantics** — the properties phase 4's records must exhibit across genesis: `patch:<n>`
  monotone serial off the agent cell's `d.vars` counter — never rank positions, never recycled after
  `unlearn` or a replay-drop (no-ABA); the user alias separate and un-minted; `d.code`'s two-copy
  rule and the program-side-original-never-registered invariant.

**Requires:** phases 3–4's emission contract, bind set and session records; a local `Store` plus
`promote`, `rebuildManifold` and a fresh `WorkerContext` per cycle — all in-tree, and
`overlay.cpp:46-82` runs the identical mint→rebuild→re-pin loop with no actor executor.
Kernel-simulable: the whole exit through a test-owned genesis driver. Adapter-gated: the production
hook (the driver invoked on real spawn/resume paths) — phase 6 wires it.

**Exit:** learn→re-genesis→learn cycles — **≥2 re-geneses**, since a re-linking (rather than
re-minting) implementation only drops `patch:<n>` at the second fold; dedupe/no-ABA tests; a rebind
against a patch-installed symbol (the §9.3 shape — a learned clause calling a learned method, the
exact misbind (e)-last exists to remove); a `dim`-field recipe record re-resolved to `Int64(DimRef)`
off the filed name post-fold; an (a)-arm comparand refusal on a permuted "received" store; an
orphan-twin test asserting one cell per filed dim name after a fold — the V10 one-cell-per-name
test's shape extended to the apparatus dims.

## Phase 6 — transport seams + the P16 ceremony

**Goal:** learned mutations survive a real checkpoint→resume. Only what crosses the wire lives here.
Land in the listed sub-order — it continues phase 5's invariant: the filing phase 5 ran on the
minted vocabulary and the fold it returned are the inputs the *pin* (seams step) consumes, and the
*first-resolution* is the genesis wiring step — mint/file → fold → pin/inject → first-resolution, on
the spawn lineage and, behind (ii)'s take-or-verify, on the resume lineage alike. (The named steps
are the citable units; the repo's list style renders every marker `1.`.)

1. **Adapter API seams** (the owed supply-side surface, §10.2's (i)–(v)): (i) spawn-side
   base-injection of the `Manifold` folded over the seeded *and filed* lineage `Store` — plus the
   `Store` as `SpanReader` — through the `WorkerContext` two-arg ctor (`worker_context.hpp:52-54`,
   which takes `const Manifold *`, not a `Store`) before `initGenesis` — **the pin on the spawn
   lineage**, consuming the fold phase 5's filing returned; (ii) `resume` take-or-verify of the
   transported `Store` (`mesh_actor.cpp:182`,`:188`,`:261` — the pin on the resume lineage is the
   rebuild behind it); (iv) **P4's approved wake implemented** — `wake(pid)` plus
   `ActorExecution::message`→`std::optional` (`mesh_actor.hpp:81`, `vagent-agent-language.md:1670`)
   with mesh executor/test updates — implementable and testable on today's tree at mesh level; only
   its *invocation* from the checkpoint ceremony is adapter-gated; (v) the `documentId`-adoption arm
   (`historyFromSeal` mints `Store()` with no id, `publication.cpp:1097`, `store.cpp:123`); (iii)
   the segment-apply surface — listed last because it is conditional: owed only if the kept-in-step
   twin arm is kept (the `base()->store()` verify arm stands alone).
1. **Genesis hook wiring** — the phase-5 (a)–(e) driver invoked on the real spawn and resume paths
   before turns.
1. **P16 ceremony** (§10.2): **marker-edge mint** — the record schema is phase-3's emission
   contract; this step revisits `fragment.cpp` and the `reify` source to mint one fresh
   `d.mesh.relocation→"cell"` marker *per ref cell* (sharing impossible — `link` evicts the first
   edge; `mesh_evaluator.cpp:735` precedent), including `reify`'s regenerated records; the deferred
   ref-record arm of spec-(e) lands here inside the same driver — per-ref-record
   `Int64(*handleTarget)` writebacks, the `OpHandle`→`Int64` restore — the arm that genuinely
   requires this step's flag machinery; the flag-driven re-kind/restore windows inside the adapter's
   own `Executor` callback in `pumpOne` (`mesh_actor.cpp:473-477`) — arm the window on
   mailbox-empty-at-turn-end (spec :2165-2169; `pending(pid)==1` is the queue-depth proxy — a
   flagged turn that self-sends breaks it), an empty-mailbox checkpoint mints a P4 wake turn, and
   **no turn of either kind runs between the flagged turn's end and `publishValue`** (issued
   synchronously from the same callback — the no-interleave discipline, spec :2171-2179); the
   three-state flag and the kind-guarded restore on every exit path.

**Requires:** the actor executor + adapter `Executor` callback — adapter-gated, except (iv)'s
`wake`/`std::optional` mesh change, which is implemented and mesh-testable here and only consumed by
the ceremony later.

**Exit:** the P16 acceptance test — learn → checkpoint → resume → learn → checkpoint round-trip;
comparand-refusal test on permuted lineage; orphan-twin test asserting exactly one cell per filed
dim name after resume (the V10 test extended to the bind set).

## Phase 7 — plasticity surface exercises

**Goal:** prove the design's stated purpose on the built machinery — no new mechanism, all tests.

- §8.1 weight/synapse cells, `set_weight` arms incl. the junction arm.
- §8.3 Hebbian growth cycles (§9.3 worked example end-to-end).
- §8.2 plan-library-as-synaptic-structure; §8.5 program-search-is-cell-search.
- Full §9 worked-example suite as integration tests — §9.2 exercises `learn_where` (phase 4); §9.3
  `learn_plan` + `set_weight`.

**Requires:** the full actor executor (vagent iterations 4–5+) — §9.2/§9.3 sit outside today's
scalar subset; adapter-gated.

**Exit:** §9.2/§9.3 run green; a learned-weight schedule survives checkpoint→resume (uses phase 5+6
machinery).

## Explicitly out of scope (all phases)

- Everything in §15 (`U-M1`–`U-M11`): metacircular compiler, remote `Code` mobility, autodiff,
  learned plan selection, AST-as-cells, suspending fragments, generic foreign-graph `reify`, `std:*`
  graft policy, `specialize`, `run_checked`, foreign-artifact heads.
- `P17` (`opcodeMap_`/`cursorPC_`/`arenaDims_` eviction-resync): deferred-not-registered — the
  containment discipline (the drain idiom, no bare `release` under live marks, no-marks minting for
  ops and dims alike) keeps it unnecessary.
- Anonymous declared handlers (capability limitation, documented in §6.6).

## Cross-cutting requirements

- Every phase: `make -j$(nproc) test` green; headless env per AGENTS.md.
- **Test homes, honestly named**: `tests/xuzz/*_test.cpp` globs into `xuzz_test` (Makefile:649),
  which links the whole `apps/common/xanadu` tree — the only auto-discovered kernel-linked home, and
  where all engine-level metaprogramming tests sit. Frontend unit tests join
  `VAGENT_FRONTEND_TEST_SRCS` (explicit list, Makefile:692) for `vagent_frontend_test`, or land in
  the same `tests/xuzz/` glob. `vagent_runtime_test` **filter-outs everything under
  `apps/common/xanadu/vagent/`** (Makefile:698-699 — it isolates shared VM prerequisites) and cannot
  link `fragment`/`compiler`/`expand` at all: no metaprogramming test belongs there. `make test`
  runs `gleditor_test`, `xuzz_test`, `zigzag_test` only (Makefile:1348-1351) — phase exits run under
  `make test` exactly insofar as they live in `xuzz_test`; the `vagent_*_test` binaries run through
  their own `test/vagent-*` targets.
- Logging: name the spdlog categories up front — `vagent.expand` for substitution/`cap$N` renames,
  `vortex.meta` for P15's per-site refusals, `vortex.genesis` for replay/filing, `xanadu.adapter`
  for the P16 windows — so each phase adds `SPDLOG_LEVEL`-gated diagnostics instead of inventing
  conventions (AGENTS.md).
- Citation discipline: the design doc's `file:line` claims were verified at v1.19; implementation
  must re-verify each as code drifts — treat every cite as a starting coordinate, not gospel.
- Each phase is one or more logical commits; the §10.2 register entries (P15, P16, the five owed
  adapter seams (i)–(v)) map to their own commits for reviewability.
