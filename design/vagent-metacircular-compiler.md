# The metacircular compiler (U-M1): runtime compilation for vagent

|            |                                                             |
| ---------- | ----------------------------------------------------------- |
| status     | design draft for review — nothing here is approved yet      |
| supersedes | nothing; elaborates U-M1 in `vagent-metaprogramming.md`     |
| audience   | compiler, Vortex-runtime, and system-architecture reviewers |

Unqualified `§N` refers to `vagent-metaprogramming.md`; this document's own sections are marked "§N
here".

## 1 What U-M1 asks for

One framing the doc inherits without hiding it: the emission contract it must reproduce encodes an
*address-arithmetic layer* atop the space — operands travel as `int64` content, never links
(`compiler.cpp:1076-1089`), which is precisely why the P16 relocation machinery exists. Reproducing
the contract propagates that layer to runtime-minted code — correct (deviation is a parallel
runtime), but whether operands can ever be link-carried is a separate contract-reform project this
design does not fold in silently.

`vagent-metaprogramming.md` defers one sentence under U-M1:

> **U-M1 — A metacircular compiler.** `code"…"` fragments are compile-time-born; a
> vagent-compiler-in-vagent would allow runtime source→graph. Deferred: it needs the AST-cells
> representation refused in §11, plus the whole frontend re-expressed in Vortex. Direction: if it
> ever lands, it must lower through the same kernel discipline, not a second emitter.

Three properties in that sentence decide what this document may propose:

1. **Runtime source→graph** is the goal — a program produced while an agent runs must end up as an
   invocable fragment object, indistinguishable from a lowering-time one.
1. **The frontend moves into the language** — lexer, parser, expander, checker, emitter as
   vagent/Vortex artifacts, not a second C++ pipeline.
1. **"The same kernel discipline, not a second emitter"** — the emitted graph must satisfy the exact
   emission contract `Emitter`/`compileFragment` produce today: the wrapper `emitWrapped`
   (`produced=false → prologue → ChoiceDepth → Choice → body → Cut → produced=true → tail`,
   `compiler.cpp:1831-1847`; tail `Halt` for statements at `:1858`, `Return` for fragment bodies at
   `:1889`), the head/descriptor/refs/recipe schema (`fragment.cpp:244-430`), and the
   `RunCall`/`RunResume` binding convention (`vortex_vm.cpp:947-1152`). Anything less is a parallel
   runtime, not a metacircular compiler.

Everything else — whether the AST lives as cells, which stage self-hosts first, how output is
installed — is this document's to decide.

## 2 What the current pipeline already provides

The pipeline today, with the stages a runtime compiler must reproduce or replace:

| stage          | where                            | runtime-reachable? |
| -------------- | -------------------------------- | ------------------ |
| lex/parse      | `lexer.cpp`, `parser.cpp`        | no — C++ only      |
| resolve/expand | `resolve.cpp`, `expand.cpp`      | no — C++ only      |
| check          | `checker.cpp` (~2.8k lines)      | no — C++ only      |
| scalar emit    | `compiler.cpp` `Emitter`         | no — C++ only      |
| fragment emit  | `fragment.cpp` `compileFragment` | no — C++ only      |
| decl lowering  | `decls.cpp` `lowerProgram`       | no — C++ only      |
| **execution**  | `RunCall`/`RunResume` ops        | **yes (in-lang.)** |
| (engine-level) | `meta.cpp` `runFragment`         | engine-only        |

Neither `compileFragment` nor `compileStatement` is reachable from running code: no opcode, host
verb, capacity, or reflection path reaches them.

What *is* reachable or reusable:

- **`cells"…"`** — the `Linked`/`New`/`Value` surface: one signed-dim traversal, posward `%`
  creations, and ref-box reads (`emitCells`, `compiler.cpp:1703-1823`; `cell_path.cpp:17-143`).
  Three limits matter: creation literals mint **string content only** (`compiler.cpp:1773-1778`),
  computed creation values are refused (`cell_path.cpp:134-138`), and
  `Link`/`Break`/`Splice`/`Clone` are never emitted by any vagent arm. So op cells — int64 content —
  are **not** directly mintable through the literal surface: an int64 arrives from any op's
  bound-output writeback (`x := 25+25` yields a cell holding int64 50 — parent §4's own example), or
  as an argument (Code-typed args are int64 ref-boxes at runtime, and `Linked`/`Find` materialize
  them). The bootstrap is therefore a one-time pair of genesis-minted fragments —
  `writer(target, intSource)` (one `Value` op) and `linker(from, dim, dir, to)` (one `Link` op) —
  hand-assembled in C++ like the ~190 stdlib precedents and executed through `runFragment`; all of
  `Link`/`Break`/`New`/`Value`/`Splice` decode ≤ `FieldSet` and run unregistered. Under the
  emitter's mint-order discipline plus that bootstrap, every byte of the emission contract is
  reachable (§3.2 here, Q7 — resolved to option (c)).
- **`run(c, args)`** — the invocation half: `RunCall` binds name-keyed args on the cursor's
  `+d.vars`, `RunResume` reads `produced`/`result` records (`vortex_vm.cpp:947-1152`). The contract
  runtime-compiled output must satisfy.
- **Engine-level, reusable** — `reify` (deep copier driven by the source head's `+d.recipe`,
  `meta.cpp:1427+`), `validate` (§7.2's bounded per-op walk; the `kArity`/`kImpure` bind-set tables
  mint the emission contract *as data*, `meta.cpp:102-180`, content-driven, no registration needed —
  the natural oracle), `graft_*`/`learn_*`/`d.patch` (clause installation and durable identity,
  `meta.cpp:1961+, 2184-2303`; genesis replay `genesis.cpp:247-344`). Checker signature rows exist
  for all of `std:meta` (`checker.cpp:970-1020`), but **no emit arm and no `runHostStep` dispatch
  reaches any of them** — the `Meta` capacity has no installed provider, so all are engine-only
  today. That emit-arm gap is the smallest shippable increment and everything later depends on it.
- **Hand-assembly precedent** — `vortex_stdlib.cpp`'s ~190 `mintOpcode` sites: every `std:*` routine
  is already "compiled" by cell construction in C++. U-M1 generalizes that to a language-driven
  assembler.

### The emission contract, enumerated

"The same kernel discipline" is this list — a runtime emitter must reproduce all of it, not just the
op shapes:

- **Wing shape** — inputs hang off `op +d.grab` POS chained `+d.step`, outputs off `+d.grab` NEG
  (`vortex_core.cpp:381-441`; `wing()` `compiler.cpp:1108-1121`). Every operand is a **fresh int64
  slot cell** naming the referent — operands travel as content, never as links (`bindIn`/`bindOut`,
  `compiler.cpp:1076-1089`).
- **Prologue order** — the RESOLVE chain emits in reverse declaration order so `$p0` resolves first
  (`compiler.cpp:1890-1895`).
- **Descriptor record order is load-bearing but asymmetric** — `descriptorField`
  (`meta.cpp:658-661`) reads the *first* `+d.vars` match by `textOf`, while `RunResume`
  (`vortex_vm.cpp:1119-1134`) iterates the whole rank and the *last* match wins — so `result`,
  `produced`, `pure`, `closed`, `arity` records precede param records (`fragment.cpp:282-301`), and
  a param named `result` (not reserved by `reservedName`) shadows the channel under RunResume while
  engine-side reads still see it: the two readers diverge on a shadowed descriptor, and
  canonical-first ordering cannot satisfy both — the durable fix is reserving descriptor keys as
  param names (or making `RunResume` first-match).
- **The descriptor's param chain** — params hang off the descriptor's `+d.grab`→`+d.step` rank in
  declaration order, each a `+d.name` + `+d.values`→typeName record (`fragment.cpp:303-317`): the
  shape `RunCall`'s name-keyed binding (`vortex_vm.cpp:1009-1026`, including the `$owner` skip) and
  `parameterCells` (`meta.cpp:665-674`) read arity and names from.
- **Recipe tuples are name-keyed on the dim** — a link tuple on an unfiled dimension refuses at
  emission (`fragment.cpp:353-361`); the `+d.entry` tuple is required for self-contained copies
  (`:382-387`).
- **Coverage is total** — every minted cell reaches the recipe or a ref-capture record, or reify
  aliases it to the source graph (`compiler.cpp:1137-1140`); the `own_` bookkeeping is required
  output-side state.
- **Type-name strings are `typeName()`-verbatim** — graft body arms compare signature strings
  (`meta.cpp:1920,1928,1988,1993-1994`); a partial checker can mint `"()"`/`"Cell"` honestly only if
  it also controls which graft arms its output may target.
- **cap$N slots** — `+d.clone`-rank append on the binding plus the ref-capture record
  `+d.refFor`→slot `+d.values`→slot (`compiler.cpp:1097-1107`; `fragment.cpp:412-420`; replay
  `meta.cpp:1564-1575`).
- **Labels are durable identity** — `registerHead` refuses duplicate labels (`meta.cpp:2206-2215`);
  `reservedName` covers `patch:`; minted labels draw off the durable serial counter like
  `reify:<src>:<serial>` (`meta.cpp:1709-1713`).
- **`+d.refs`/`+d.recipe` chain shape** — first record on head `+d.refs`/ `+d.recipe`, subsequent on
  `+d.step`; REF-records carry `d.mesh.relocation→"cell"` markers, NAME-records carry
  `+d.name`+`+d.grab` role (`fragment.cpp:86-111`).
- **The `run`-role convention** — a head-valued cell on the **`Call`** op's `+d.values` edge
  carrying the `"run"`-role name record for genesis re-resolution (`emitRun`,
  `compiler.cpp:1973-1978`); `RunCall` ops carry none — `emitMemberCall` binds its member symbol by
  bare `reference()` position (`compiler.cpp:2056`), so only statically-known callsites carry the
  name channel.

Four defects this design inherits and should fix, not route around, plus one correction to an
earlier draft's claim:

- **`validate` cannot read real emitter output at all** — operands are two-hop: the wing slot holds
  `int64(refBox)`, the ref-box holds `int64(referent)`, and the VM does the second hop at execute
  (`vortex_vm.cpp:1278-1284`; minted by `reference()`+`bindIn`, `compiler.cpp:1077-1083,1164-1169`).
  `validate` resolves only the first hop then demands `isOpCell` on the ref-box — a position int64,
  not an opcode (`meta.cpp:576-582,601-617`). Every `emitWrapped` body carries a `Choice` and
  `Jump`s, so `validate` fails on *all* real emission — and `graft_*`, which gates on
  `validate(runReachable)`, is latently broken on `code"…"` fragments today (no test path exercises
  it — the engine-only surface hid it). A second same-class gap: `validate` under `runReachable`
  refuses bound-output `New` (`meta.cpp:566-569`), which `emitCells` emits for every `cells"…"`
  creation (`compiler.cpp:1764-1769`). Until the walk is kind-gated two-hop (for each op kind, which
  wing slots are op-refs — `Jump`/`Call`/`Choice` slot 0, `Branch` slot 1, `RunCall` slot 2 only,
  `RunResume` slot 1 — resolve `slot → operand-cell → int64(referent)`, then `isOpCell` the
  referent), "testable against `validate`" fails for anything beyond a straight-line spin chain.
- **Filtered `Linked` arity mismatch** — `emitCells` emits 6-in `Linked` ops for `[{+d.f}]` filter
  paths (`compiler.cpp:1747-1757`) but `kArity` mints `{Linked,4,1}` (`meta.cpp:121`) and `validate`
  demands exact equality (`meta.cpp:563-565`): any `code"…"` containing a filtered cells path fails
  `validate` today. The oracle false-negatives a construct the C++ emitter produces — fix the table
  before an in-language compiler trusts it.
- **`validate` under-approximates op-reachability** — its control-target switch (`meta.cpp:601-620`)
  enqueues `Jump`/`Call`/`Choice`/`Branch` targets only; a `RunCall`'s fail target (input 2) and a
  `RunResume`'s (input 1) hit the default and are never checked or walked. So `validate` passes a
  `RunCall` whose fail slot names a non-op, and a `register` enumeration built on this walk verbatim
  registers a body's `RunCall`/ `RunResume` but leaves their fail arms unregistered — the exact
  silent-Nop position the lattice above describes. The walk must be extended — the enumeration
  contract is "every operand slot whose content names an op cell," not the four control ops' targets
  — which is a fix to `validate` itself, not a workaround in `register`.
- **Not a defect: `emitMemberCall`'s `.at()` is guarded by construction** — preflight refuses any
  `Identifier` callee absent from `methods` before emission (`compiler.cpp:866-877`), so the lookup
  at `:2053` cannot miss. `reify(c)`/`graft_*(…)` calls refuse cleanly there with a `Diagnostic` —
  the real gap is only that no verb emit arms exist (stage 1). Verb arms still dispatch
  name-vs-method through fallible lookup as house style.

And one oracle caveat: `validate` tags every visited op on `+d.validate` with a serial cell
(`meta.cpp:528-538`) — residue on the validated graph that folds into checkpoints. Harmless, but a
tool the compiler runs routinely should have it documented.

### The registration asymmetry (the load-bearing constraint)

Cell content and VM registration are different things:

- `VortexVM::getOpcodeKind` (`vortex_vm.cpp:190-202`) checks `opcodeMap_` first, then falls back to
  int64 content decode **bounded at `OpcodeKind::FieldSet`**. Runtime-minted ops execute
  unregistered only for kinds ≤ `FieldSet`; `RunCall`/`RunResume` (the newest ordinals) never
  content-decode.
- `RunCall`'s head resolution requires `opcodeMap_.find` on the `+d.entry` chain — **map membership,
  not content** (`vortex_vm.cpp:984,988`). So `run(box)` inside program code can never invoke a
  runtime-minted fragment, even one whose ops are all ≤ `FieldSet`.
- `MetaRuntime::isOpCell` is content-based against `linkedCeiling`, minted as
  `OpcodeKind::RunResume` — the enum's last ordinal (`meta.cpp:93-94,313-321`). So `runFragment`
  *recognizes* a runtime-minted graph across the whole enum and executes it — provided the body's
  nested ops stay ≤ `FieldSet`.
- Registration is effectively C++-only (`registerProgramImage` → `registerProgramOps`, genesis-(b)
  re-registration over the image `ops` enumeration). The one runtime exception *was* a defect:
  `runFragment`'s landing pad registered a `Halt` under whatever mark state capcalls run in — fixed
  by minting it unregistered (`meta.cpp:757`), since `Halt` ≤ `FieldSet` decodes identically by
  content. The `register` verb below must be the **only** post-genesis map-writing surface for
  *program-produced* cells — deliberate pre-mark host mints (the executor's `fail_step`,
  `executor.cpp:252`; mesh artifact `setOpcodeKind`, `mesh_evaluator.cpp:667`) are the carved-out
  exceptions.
- Unregistered mints get no P15 write-protection and no post-fold re-registration — and an
  unregistered `>FieldSet` op doesn't refuse, it decodes as `Nop` and the cursor advances `+d.spin`
  (`vortex_vm.cpp:1659-1660`): **silent wrong answers, not faults**. The degradation is
  position-dependent: Nop in the spine, `invalidChoice` fault in a `Choice` alt, vacuous `true` in a
  contract clause (`evaluateCondition`), silent no-op in `runPipeline` — so pre-registration
  execution has no single honest contract; `register`-then-`validate`-then-run is the ordering
  discipline. That hole is live today — reified copies mint their op twins with
  `makeCell`+`writeValue`, never `setOpcodeKind` (`meta.cpp:1470-1537`), so a `learn_*` of a
  `run`-containing fragment installs a copy whose `RunCall` Nops.
- `opcodeMap_` is per-`WorkerContext` (`vortex_vm.hpp:269`) — a session insert dies at genesis fold
  and never reaches `or_parallel` workers. Durable registration therefore can't be a map insert
  alone (see D3).

**Doc/code drift to correct as part of this work**: `vagent-metaprogramming.md:543-546` claims the
fallback "is how deserialized and runtime-minted op cells execute without registration" — true for
the `MetaRuntime` walk (ceiling `RunResume`) but not at `VortexVM::step` (ceiling `FieldSet`, plus
the `RunCall` map-membership requirement). The spec also cites a stale fallback location
(`vortex_vm.cpp:107-112`, now `:190-202`) and a stale ceiling (`[0, Linked]`, now `FieldSet`) — and
`meta.cpp:557`'s refusal text still reads `"op int64 outside [0, Linked]"` though the ceiling has
been `RunResume` since `linkedCeiling` was raised. Sibling spec cites
(`vagent-metaprogramming.md:535,537-538,540,551,555,557`) and the `#BIND` comment at `meta.cpp:765`
(`vortex_vm.cpp:896-943`, now `:1337-1362`) are stale in the same sweep.

## 3 The three separable blockers

U-M1 decomposes into three problems that can land independently:

### 3.1 Program representation

A runtime compiler needs something to traverse between text and graph. §11 refused AST-as-cells *as
the base representation* — "the opcode graph is already the executable truth" — while U-M5 keeps it
viable "as a view (reconstructed source) rather than the truth". Options:

- **(R1) Cells-encoded AST as intermediate only.** An AST schema of ordinary cells (kind tag +
  children rank + leaf values), minted by the parser stage, consumed by check/emit, never escaping
  into the sealed artifact. Honors the §11 refusal — the AST is pipeline residue, like the compile
  arena's — while giving every stage something inspectable.
- **(R2) Direct data→opcode emission.** Skip the AST: a builder surface mints heads/descriptors from
  structured input (terms, cells lists), the way `learnCopies`/`registerHead`/`mintPatch` already
  assemble objects in C++. A builder, not a compiler — but it is *the* shared kernel: U-M9's
  `specialize`, U-M2's receive side, and the in-language emitter's own output stage all produce
  heads the same way. D1 stages it fourth.
- **(R3) Host-service compile.** The C++ frontend stays C++; a runtime ceremony feeds it source text
  and grafts the result. Delivers "runtime source→graph" without metacircularity — and exercises the
  install surface before any in-language stage exists. D1 stages it third.

**Proposed**: R1 for the metacircular path (stage 5's input), R2 as the builder milestone (stage 4),
R3 as the host ceremony (stage 3) — the full staging is D1.

### 3.2 Emission and installation surface

Even with a frontend, output today has nowhere to go:

- Op minting via `cells"…"` works but is capped at `FieldSet` decode, and `run()` callsites
  additionally need `opcodeMap_` membership on `+d.entry`.
- `reify`/`graft_*`/`learn_*`/`validate` are checker-accepted but have no emit arm — no in-language
  way to install a head into an agent, a clause onto a spine, a patch into the durable EDL, or to
  *verify* output from the language.
- `learnGate` verifies declared closedness (`verifyClosed` walks `+d.refs`/`+d.recipe` records,
  `meta.cpp:2700-2823`) but **never runs `validate`** — a malformed minted graph with clean records
  passes `learn_*` and replays every genesis (`meta.cpp:1887-1900`). Install of runtime-produced
  input needs the `validate` walk as a hard gate.
- Output has no `pure`/`closed` provenance: the descriptor records are data- mintable, so
  self-certification vs engine re-verification (`verifyClosed`, `learnable`,
  `meta.cpp:2177-2186, 2700+`) is an open policy question.
- `+d.refs`/`+d.recipe` emission is required for anything durable or reifiable; skipping it yields
  session-only fragments (D5 keeps recipe in v1 scope — the boundary option is rejected, listed for
  the record).

Options for the registration gap:

- **(E1) One atomic `register` verb, returning the `Code` handle.** The milestone needs *both*
  registrations — kind (`opcodeMap_`) and code (`d.code` rank) — and the only existing path to the
  second is `registerHead` inside `learn_*`, whose own parameter is `Code`: a minted head can never
  become `Code`-typed, so `run(head)` is a *compile-time* type error (`checker.cpp:1178-1181`), M3
  bars the silent `Cell`→`Code` upgrade, and no stage of the design could form the milestone
  program. Splitting the acts also leaves the ordering trap ("a `reify` between them forfeits
  kind-registration") as a protocol no type can express. So:

  `register(h: Cell): Opt<Code<Cell>>` — one `MetaRuntime` verb (beside `registerHead`, where
  `+d.entry`/`d.code` knowledge lives) that atomically: enumerates the op set, `validate`s,
  kind-registers the enumeration, appends the head to the kind-registered-heads carrier (the durable
  write — drain-time for deferred requests, and the pending marker carries the head *cell*, not a
  label, since the enumeration is re-derived), files the head on `d.code`, and returns the handle
  `code_named` vends — the returned `Code<Cell>` is provably the live one (`reify` twins never carry
  it). The batch form is a **distinct name** — `register_all(hs: [Cell]): Opt<[Code<Cell>]>` — since
  verb signatures resolve name-keyed with no overloading and no union types
  (`checker.cpp:966-1020`); the alternatives are a checker special arm (the `run`/`reify` `metaForm`
  precedent, `checker.cpp:1255-1264`) or an `any` param that silently gives up the compile-time
  refusal this design exists for, so the separate name is the honest shape. All-or-nothing per
  registration unit, the fault record attributing the refusing element by label — and
  `register`/`register_all` **require a `+d.name` label**: an unlabeled head's tombstone would
  collapse to the shared `fault:` key (`varsRecord` last-write-wins), so unlabeled input refuses at
  request time. Impure; reserved under `import std:meta` (the `run`/`value_of` precedent — the
  reserved-name arm covers `registration` and `dim_named` too, or a member `def` of either silently
  shadows a builtin). Refusal is `Opt`-empty plus a statement-failure; the *named* missing-callee
  cause rides a `fault:<label>` `d.vars` record on the agent cell — the `dropPatch` tombstone
  convention verbatim — since the host-step channel drops text and program-observable causes are a
  fixed vocabulary (`executor.cpp:2751`). Refusal splits by phase under universal deferral: the
  synchronous `Opt`-empty can only fire on request-time-checkable bits (`!isOpCell(head)`, missing
  label, the `vagent.max_pending_registers` queue cap); every shape/callee failure discovered at
  drain is a tombstone, not a refusal. One idiom consequence: `run(register(h))` is a compile-time
  type error — the operand is `Opt<…>`, and `Opt` unwraps only on a bound read
  (bound-name/field/index, `checker.cpp:96-100`); the idiom is bind-then-run, *next turn*.

  `Code<Cell>` does not reach every install verb: `compatible` is **invariant** in `T` — the
  bare-`Code` wildcard arm fires only on unparameterized expected types (`checker.cpp:665-674`), and
  the body-position params declare `Code<()>` (`bodyCode`, `checker.cpp:1000-1011`), so a
  `register`/`code_named`/`body_of`-vended handle type-refuses `graft_body`/`learn_body` and the
  `body` slots of plans/handlers — `graft_where`/`graft_method`/`learn_method` and `test` slots
  (bare `Code`) still take it. The fix is parent §11.1's descriptor-checked-upgrade pattern extended
  one step: `as_body(c: Code<Cell>): Opt<Code<()>>` (pure) — a descriptor-internal check (descriptor
  records present, `resultType == "()"`, the arity record consistent with the param chain — the
  `meta.cpp:730-732` refusal class); the clause-formal compare can't run here (no clause argument)
  and stays where `matchAttach`/`graft_plan` re-verify it at install (`meta.cpp:1912-1994`), so the
  narrow adds no soundness hole. Not a blanket `code_as`, since type arguments can't parameterize a
  verb's return and `Code<()>` is the only invariant param v1's install surface declares (`bodyCode`
  at `checker.cpp:1002,1006,1011`; `test`/`guard`, `graft_where`, `graft_method`/`learn_method` are
  all bare-`Code` wildcards). Same bound-read rule as `register` — `graft_body(c, as_body(h))`
  type-refuses on the `Opt` operand; bind first.

  Contract: (i) `!isOpCell(head)` — an int64-content head is misclassified as an op cell
  (`graphRoot` returns the head itself as its own root, so the `+d.entry` chain is never walked;
  `runFragment` refuses it outright); (ii) the enumeration is the full op-reachable set — `+d.spin`
  chain, wing-operand-reachable ops (fail arms, branch targets), `+d.entry` termini — derived by the
  `validate`-shaped walk *repaired to the emission convention*: kind-gated two-hop operand
  resolution (which wing slots are op-refs per op kind — `Jump`/`Call`/`Choice` slot 0, `Branch`
  slot 1, `RunCall` slot 2 only, `RunResume` slot 1 — resolve
  `slot → operand-cell → int64(referent)`, then `isOpCell` the referent), plus the missing
  `RunCall`/`RunResume` fail targets (see the inherited defects, §2 here). Never caller-supplied — a
  caller-staged rank is a P15-freeze smuggling surface. Two op-bearing regions stay out of v1 scope:
  `±d.contract` clause op-graphs and param-cell `+d.spin` pipeline heads — the stdlib mints both
  (`vortex_stdlib.cpp`), the vagent emitter never does — and `register` **detects and refuses**
  (Opt-empty + `fault:`) rather than under-enumerating: a reified stdlib head *does* carry contract
  clause graphs (`preconditionsOf`/`postconditionsOf` anchor them), and an unregistered `>FieldSet`
  clause op would Nop inside `evaluateCondition` — the silent-`true` cell of the degradation
  lattice; (iii) `validate` runs internally and all-or-nothing — `registerProgramOps` commits
  partially on the first bad cell (`vortex_vm.cpp:96-123`), so the enumeration is fully verified
  *before* any map write, since a registered `RunCall` beside an unregistered `RunResume` silently
  writes the caller's output into nothing; (iv) transitively, each statically-known
  `run`-role/`call`-role referent's entry terminus must be registered **or in the same registration
  unit** — a self-recursive fragment's referent terminus is inside its own set — or the refusal
  names the missing callee; dynamic `run()` referents are unverifiable at `register` time and fail
  closed at the call; (v) under live marks, `register` doesn't refuse — it records a **trailed
  pending-registration** marker the engine drains when `outstandingMarks()==0`, re-deriving the
  enumeration at drain time (trailed graph edits between request and drain can stale a request-time
  enumeration, and `registerProgramOps`'s partial commit means the verify-before-write rule holds
  there too), since `opcodeMap_` is untrailed state whose entries would survive the release of the
  cells they claim (the P17 class, with `cursorPC_`/`memoizedOps_` in the same family — the rule is
  *no untrailed VM state mutation under marks*, not just no op registration). Registration is
  idempotent and monotone in v1 — no unregister path; an `unlearn`'d head's carrier entry simply
  pins, same as the head's other durable reachability; a re-`register` on a carrier-listed head
  skips the walk via membership check.

  **Deferral is the universal path, not the marks edge case.** The executor holds ≥2 marks before
  user code (base Choice + `m0`, `executor.cpp:256-257`) and every fragment body adds its wrapper
  `Choice` mark — `outstandingMarks()==0` is unreachable during a turn and reached exactly once at
  post-commit (`executor.cpp:3024-3029`). So `register` in a program *always* returns
  accepted-pending; `run` on the returned `Code<Cell>` fails closed (unregistered `+d.entry`
  terminus → `failJump`, `produced=false`) — **indistinguishable** from computing nothing until the
  drain lands at turn commit. Callable at T+1 earliest; the mark- bounded search loop of parent §8.5
  cannot score a compiled candidate mid-search. Caller discipline and observability: drain sites are
  *enumerated host points* — executor post-commit (after `discard(m0)`) and engine-level
  `runFragment`/`runBox` returning to marks==0 — never an arena-level on-0 hook (`evictOldestMark`
  can reach 0 mid-execution inside a victim VM; `vortex_stdlib`'s solver takes transient marks per
  candidate); the drain is non-reentrant (requests during drain queue to the next point) and records
  `fault:<label>` on failure (the `genesis.cpp:219-228` `dropPatch` precedent — `m0` is already
  discarded when it runs, so a broken graph is a recorded fault, not a rolled-back turn); `register`
  inside genesis (marks==0) applies synchronously — a third code path, how (d)-installed twins get
  registered; and a observability is a *status* verb, not a Bool: `registration(h: Cell): Str`
  returns `absent`|`pending`|`committed` (pure, callable in conditions). `committed` is the durable
  fact — carrier-listed **or program-image registered** (`code"…"`/image heads are registered by
  `registerProgramImage`'s `ops` enumeration, a space fact the verb reads, not the map; an
  already-`committed` head passed to `register` returns its handle idempotently — no walk, no
  tombstone). Where `RunCall`'s map read diverges from `committed` is the **restore window** —
  post-fold, pre-(b½)-rebind, the carrier is durable but the per-context index is unbuilt — plus
  session-minted map entries with no space backing (`fail_step`, `mesh_evaluator.cpp:667`), which
  report `absent`; in the pending window map and carrier agree (both absent — `pending` is the third
  state precisely because a Bool couldn't express it). The durable half needs no verb at all — the
  carrier rank is `Linked`-walkable. At drain, E1-iv's callee check consults **pending ∪ committed
  within the same drain unit**, and the drain is two-pass with **per-head isolation**:
  enumerate+validate every pending request against committed ∪ *same-unit heads that themselves
  validated*, then commit the survivors — a same-unit callee that faults its own check faults its
  dependents transitively (the tombstone names it), while independent heads commit; committing A
  with an absent callee would be a committed-but-broken registration, which is worse than the fault.
  The carrier write *is* the commit act — a drain-faulted head's entry is never appended, so
  `registration` reads `absent`, retry is not a no-op, and the fault record lands as the
  `fault:<label>` `d.vars` tombstone keyed by the requested head's label.

- **(E2) Lift the `FieldSet` decode ceiling** and drop `RunCall`'s map- membership requirement —
  content-decode the whole enum. Rejected: it collapses the registered/unregistered distinction the
  rest of the system (P15, genesis (b), `RunCall`'s admission check, the deserialization boundary)
  is built on — every data cell holding an int64 opcode becomes executable, descriptor records
  (arity=2 reads as `Mul`, pure=1 as `Sub`) become ops, and "protect what" under P15 becomes
  ill-posed. For the record, E2 is the *topologically* purer option — the ceiling is a trust
  boundary, not a structural one — and `RunCall`'s map check is a derived guard, not a primitive
  (the `+d.entry` chain already asserts the terminus is an op; content-decoding the terminus the way
  `graphRoot` does would shrink registration to step-time `>FieldSet` decode). E1 keeps the honest
  boundary and reframes it: `register` is *graduation into the program's immutability class* — the
  moment content becomes load-bearing kind it freezes (P15), which is why `opcodeMap_` is really an
  immutability registry and why `E2`'s any-int64-executes world can't checkpoint-kind cells in
  ancestry.

- **(E3) Restrict v1 to ≤ `FieldSet` bodies + engine-level execution** — no nested `run` inside
  runtime-compiled fragments; invoked via `MetaRuntime` (capcall-shape), not `run()`.
  Status-quo-compatible, smallest delta, but leaves `run()` unreachable for runtime output.

**Proposed**: E1 — registration is the honest boundary; it keeps the registered/unregistered
distinction the rest of the system (P15, genesis (b), `RunCall`) already depends on. Two corollaries
the reviewers surfaced:

- **A VM map is an index, never a fact.** `opcodeMap_`, `cursorPC_`, `memoizedOps_` are all the same
  species — derived per-context indices over space truth, exactly what the spanfilade transclusion
  index is to links. Registration is therefore durable *data*: a *kind-registered heads* carrier on
  the agent cell (a filed dim name beside `d.code`, distinct from `registerHead`'s code-registration
  — the naming collision is real), plus a genesis pass and `bindProgramOpcodes` hook that re-walk
  each registered head's op set and re-apply `registerProgramOps`. Placement is not free:
  `learnCopies` mints installed twins during genesis (d) (`genesis.cpp:257-261`, collected into
  `out.installedCopies` at `:580`), so a carrier sweep before (d) leaves those twins unregistered
  for a whole session — the pass folds into **(e)'s existing `installedCopies` sweep**
  (`genesis.cpp:593-609`), or `replayPatch` invokes `register` per copy (marks==0 at genesis →
  applies immediately). The sweep walks **one union seen-set** across all carrier heads — per-head
  walks re-walk shared callees fan-in times; per-head fault isolation attaches to entry resolution,
  not the walk — keeping it O(total registered ops) per incarnation, the same shape as today's (b)
  over the image. The worker hook gets only `(VortexCore&, VortexVM&)` and runs *per spark* — a
  fresh arena+VM each time (`or_parallel.cpp:345-358`) — so the host closes over roots **and
  memoizes one enumeration per incarnation**: persistent `CellRef`s are valid in every worker arena
  sharing the base, so the hook re-binds a cached `std::vector<CellRef>` (O(set) map inserts) rather
  than re-walking per spark. Replay is *restore, not re-verification*: the pass does not re-gate
  E1-iv — a verified set is restored verbatim, so a faulted callee can't permanently strand
  dependent heads the way re-gating under failure-isolation would. Worker visibility: `workerArena`
  overlays the persistent base only, so session-minted registered ops are invisible to sparks — v1
  states `or_parallel` reaches durable-registered graphs only (or spark export checks coverage).
  Faulted heads re-walk and re-fault every incarnation forever — a `dropPatch`-style eviction after
  N failed replays keeps the carrier honest. The image record's int64 `ops` enumeration can't be
  reused for two compounding reasons: bare `makeScalarCell` members carry none of the three
  relocation-mark classes `restateRefSlots` rewrites (`lineage.cpp:159-203`), so a session-minted
  position stales through `promote()` unrewritten — and program-side shadows under the image record
  are excluded from durable roots entirely (`lineage.cpp:504-509`, "verb-replayed writes genesis
  re-derives"), so a runtime append never reaches the fold either way. Re-deriving each head's op
  set by walk is what makes re-registration position-stable and gives per-head failure isolation — a
  malformed registered head records a fault and genesis continues rather than sinking the whole
  replay.
- **`register` composes with `run`'s second surface.** `run`'s operand must be `Code`-typed, which
  for a minted head means `code_named`-style membership on the `d.code` rank
  (`MetaRuntime::registerHead`, today reachable only inside `learn_*`). Kind-registration (this
  bullet) and code-registration (the head handle) are separate acts naming the **same head** —
  folded into one atomic verb (E1) precisely so the ordering is unexpressible; `reify` twins still
  carry no registration, so `run(reify(c))` fails closed — a `Code` value can be dead-invisibly,
  worth the one sentence it gets here.

**Fuel and bounds.** Every verb-side walk bills `consumeWork` per visited cell (the `RunCall`
arg-loop precedent): today `meta.cpp` has *zero* `consumeWork` calls — `validate`,
`verifyClosed`/`programCells` (an O(program-vocabulary) BFS per gate), `reify`'s replay are
unbilled, and `traversalBound` is the whole arena, a refuse-runaway bound not a budget. Pins:
`consumeWork` per visited cell in the enumeration/`validate`/ `verifyClosed`/`reify` walks; a
per-call enumeration cap (`vagent.max_register_ops`); a pending-registration queue cap
(`vagent.max_pending_registers`) with drain work charged to the next turn's grant (post-commit is
consumeWork-free, `executor.cpp:3031-3032`); `vagent.max_registered_heads` bounding the O(K)
dup-label/carrier scans; fused enumeration+`validate` in **one** traversal with an in-memory
seen-set (not `validate`'s per-op tag mints — the walk doesn't mutate the graph); `programCells`
cached per incarnation (bind-set-stable). Genesis fold and spark binds are never fuel-billed — they
are spawn/resume/spark latency, stated honestly.

**The bootstrap pair's surface and cost.** `writer(target, intSource)` and
`linker(from, dim, dir, to)` are image-side-minted at genesis carrying full head+descriptor
apparatus. Signatures: `dim` must be the dim *cell* (an int64 position through `run`'s marshaling),
not a name — `dim_named(name: Str): Opt<Cell>` (pure) is needed for programs to obtain one; `dir` is
Int-sign. Failure asymmetry: `writer`'s `Value` op on a registered target is a hard
`kindImmutabilityFault` (`ActorDown`); `linker` fails soft (`noCell`). Their labels mint under a
reserved prefix `code_named` cannot vend (`patch:`/`reify:`/`unlearn:` precedent) — filed as
ordinary `d.code` heads they would be a public unguarded `Link`/`Value` channel bypassing every
`graftable` gate (parent §8.5 calls raw surgery "§11's unguarded door"). Cost honesty: each call is
a full `runFragment` (~15-25 cells, 40-80 cycles overhead), so emission through them is ~200-400
cycles per produced op — a 100-op body eats most of a 100k-cycle turn. The real emission surface is
therefore a **batched join verb** — one host step consuming a rank of link/write tuples — with the
pair kept as the bootstrap seed it was designed to be. The tuples carry endpoints as **links**
(record `+d.values`/`+d.grab` edges to the cells), not int64 positions — the pair's int64 addressing
is excused by the emission contract it reproduces, but a new verb has no legacy contract and would
propagate the address-arithmetic layer by choice; the verb shares the pair's gate (`import std:meta`
\+ reserved name — same unguarded-channel class).

### 3.3 The frontend

The largest cost. `eval(string)` is explicitly refused (`vagent-metaprogramming.md:3131`), and
vagent's string surface (`Trim`/`ToLower`/`ToUpper`/`MakeTerm`/`Deref`) is far too thin for a lexer.
In dependency order:

1. **Emitter-in-language** — cells→ops is mechanical; the descriptor schema is data. Testable
   against `validate` (content-driven oracle, no registration needed for verification). One string
   dependency arrives earlier than "parser last" implies: head labels like `reify:<src>:<serial>`
   (`meta.cpp:1709-1713`) need int→text that vagent lacks — either mint a minimal int→text builtin
   or restructure the label as a name record + serial-int member. Decide at the builder stage, not
   the parser stage.
1. **Checker-in-language** — name resolution, types, `pure`/`closed`/`arity`, owner-field rules. The
   verification moat; can initially delegate to `validate` + engine `verifyClosed` at install.
1. **Lexer/parser-in-language** — needs a string/char surface that doesn't exist today; genuinely
   the last thing to self-host.

A runtime compile also runs inside the turn's work budget (`consumeWork`/`maxCycles`): compilation
is priced like any other program work, not a free host-side operation — the fuel story belongs in
whichever stage's interface a caller invokes.

## 4 Decisions this document proposes

**D1 — Definition of done.** U-M1 lands when a fragment body produced at run time — from a
cells-encoded AST built by an in-language frontend — is registered, grafted into an agent, invoked
through `run()` and `capcall`, validated by `validate`, and durable across a genesis fold. Two
honesty criteria keep the staging from stranding self-hosting: the stage-5 emitter must reproduce
the bootstrap `writer`/`linker` pair and a lowering-time corpus **validated-equal** (the C++ emitter
demoted to oracle), and the terminal test is the quine — the in-language compiler compiling its own
source into output indistinguishable from its own image; anything less is a demo beside a C++
substrate. The equality oracle is named, or the criterion is unfalsifiable: **graph isomorphism
modulo serial-keyed labels** — same op-kind sequence, same dims and edge topology, descriptor/param
records equal modulo the serial suffixes minted labels embed (`reify:<src>:<n>`, `patch:<n>`);
`validate` checks shape only, so the corpus comparator is its own small walk. Staging (earlier
stages are independently shippable and unconditionally useful): (1) emit arms + `runHostStep`
dispatch for the `std:meta` surface — `graft_*`/`learn_*`/`validate`/`reify` become reachable
in-language, an owed surface independent of this design; (2) the atomic `register` verb of §3.2 here
with its durable carrier, pending-drain machinery, and `registration`/`dim_named`/`as_body` verbs;
(3) host-service compile — the C++ frontend driven through a *host-side* ceremony (the VProlog
`assert`/`consult` precedent: source compiles at the boundary, the program never evals — resolving
§5's refusal), whose interface is pinned identical to the in-language pipeline's (source cells →
head + descriptor + recipe) so the C++ service is a drop-in, not a substrate; (4) the bootstrap
`writer`/`linker` fragment pair plus a builder surface — the pair is image-side-minted at genesis
(both ops ≤ `FieldSet`, so they run unregistered via `runFragment` or content-decode), carries full
head+descriptor apparatus for `runFragment`'s contract, labels reserved against `code_named` vending
(surface spec in §3.2 here); it lands before the builder because descriptor minting needs int64
writes (`arity`/serial fields — `typeName` records are text cells), not only stage 5's op ordinals —
the builder then does head/descriptor minting from structured input, shared with U-M9 `specialize`
and U-M2's receive side; (5) emitter-in-language; (6) checker-in-language; (7) lexer/parser last.
The R1 cells-AST is stage 5's input; R2 is stage 4's builder.

**Proposal register** — existing-code changes this design requires:

- **P1 — `validate` repaired to the emission convention** (`meta.cpp`): kind-gated two-hop operand
  resolution (op-ref slots per kind, referent `isOpCell` check), `RunCall`/`RunResume` fail-target
  coverage, bound-output `New` admitted under `runReachable`, filtered-`Linked` 6-in arity in
  `kArity` (`meta.cpp:121`), and an in-memory seen-set instead of per-op tag mints. Cannot live in
  the caller — `graft_*` and `register` both gate on it. Fallback: none; the oracle is wrong without
  it (empirically pinned by `ValidateReadsEmissionShapeControlTargets`).
- **P2 — `descriptorField`-vs-`RunResume` ordering contract** (`vortex_vm.cpp` or `checker.cpp`):
  reserve the five channel keys as param names (`reservedName`) or make `RunResume` first-match.
  Cheap either way; the divergence is a latent silent-produced defect.
- **P3 — `consumeWork` billing in MetaRuntime walks** (`meta.cpp`): per visited cell in
  `validate`/`verifyClosed`/`programCells`/`reify`, plus the `vagent.max_*` settings the fuel
  section names.
- **P4 — `register` verb + pending-drain machinery** (`meta.cpp`, `executor.cpp`, `decls.cpp`,
  `checker.cpp`): emit arm + `runHostStep` dispatch, trailed pending markers, enumerated two-pass
  drain sites, `fault:<label>` `d.vars` tombstones, atomic `Opt<Code<Cell>>` return via
  `registerHead`, the `registration` status verb (pure), `dim_named(name: Str): Opt<Cell>`,
  `as_body` descriptor-checked narrow, reserved-name arms in the checker for all four new verbs.
- **P5 — `+d.lineage` edge at `reify`** (`meta.cpp`): twin → source link for provenance-by-link,
  `d.lineage` filed on `d.dims`; `-d.lineage` enumerates all-time twins — dead copies keep edges
  until reclamation, so aliveness is the reader's filter.
- **P6 — Genesis carrier pass** (`genesis.cpp`): the kind-registered-heads sweep folded into (e)'s
  `installedCopies` loop with a union seen-set, plus memoized per-incarnation enumeration in the
  `bindProgramOpcodes` closure (`or_parallel.cpp`).
- **P7 — Bootstrap `writer`/`linker` pair + batched join verb** (`genesis.cpp`/`vortex_stdlib.cpp`
  mint site, `meta.cpp`): two hand-assembled ≤`FieldSet` fragments with reserved labels, and a host
  step consuming a tuple rank.
- **P8 — `learnGate` `validate` arm** (`meta.cpp`): hard gate for non-`register`-mediated install
  input, beside the existing `headShape` + `verifyClosed`.

**D2 — Representation.** R1: a cells-encoded AST exists only *inside* the compiler artifact — minted
by its parser, consumed by its emitter, never part of output. The §11 refusal stands for the base
representation; the AST is compile residue exactly like today's C++ AST. The kind vocabulary is
filed *data* — minted cells on a filed rank, the `kArity`/`kImpure` bind-set precedent — so
intermediate forms are introspectable by any walker, and the stage-4 builder consumes (a subset of)
the same schema: one program-form intermediate, not an AST for the compiler plus a second IR for the
builder.

**D3 — Registration.** E1 per §3.2 here, *including the durable carrier*: a kind-registered-heads
rank beside `d.code` (named apart from `registerHead`'s code-registration), a genesis-(b½) pass and
`bindProgramOpcodes` coverage that re-derive each registered head's op set by the `validate`-shaped
walk and re-apply `registerProgramOps`. Under live marks `register` defers rather than refuses — a
trailed pending-registration marker the engine drains at `outstandingMarks()==0` (the
untrailed-state rule covers `opcodeMap_`, `cursorPC_`, `memoizedOps_` — the landing-pad `Halt` was
the one pre-existing violation, now unregistered). Unregistered mints stay legal as data (≤
`FieldSet` content decode) — same distinction as lowering-time code — but executing them
pre-registration inherits the position-dependent degradation lattice of §2 here, so `register`
precedes any in-language invocation. Naming: "kind-registration" (`opcodeMap_`) vs
"code-registration" (`d.code` rank); both are needed for the `run()` milestone (D1). One instance
subtlety: kind-registration is *per cell, not per fragment* — `learnCopies` installs a reified twin
re-minted at every genesis replay (`meta.cpp:2188-2204`), so the (b½) carrier must record *installed
head instances* (or replay must re-invoke `register` per copy); registering the source head does not
cover the installed twin's op cells.

**D4 — Provenance.** No self-certification: a runtime-produced head's descriptor `pure`/`closed`
records are *claims*. Graft is already honest — `graftable` runs
`validate(runReachable, expectPure)` over the actual ops, so a lying `pure` flag fails
(`meta.cpp:1874-1881, 570-571`). The gap is `learn_*`: `learnGate` runs `headShape` + `verifyClosed`
(declared refs/recipe only) and never `validate` (`meta.cpp:1887-1900`) — so install of
runtime-produced input must add the `validate` walk as a hard gate (inside `register` per D3, and a
`learnGate` arm for non-`register`-mediated input); and since the installed artifact is a re-minted
twin, the gate isn't only `validate` — the install path must also route the copy through `register`
per D3's instance rule. `run()` doesn't consult the records — consistent with today's descriptor
semantics. Provenance rides links, not name-strings: `reify` mints fully independent twins and the
one back-pointing edge (`newHead +d.entry → head`) is overwritten by recipe replay —
`reify:<src>:<serial>` is provenance by *name*, so nothing lets a walker ask a source head what
twins exist. A `+d.lineage`/`d.reified-from` edge at reify (`twin → source`, `d.lineage` filed on
`d.dims` per R12; `-d.lineage` enumerates *all-time* twins — dead copies keep edges until
reclamation, so aliveness is the reader's filter) is P5 in the register. Twins re-mint their own
`+d.recipe` per copy — required, since recipes encode position-keyed tuples that the twin's
positions remap.

**D5 — Output scope.** v1 emits fragment objects (head + descriptor + body + recipe) of scalar
bodies only — the existing `Emitter` subset minus `Code`, `static`, member `def`, and `run`.
`code"…"`, `static`, member `def` calls, and `run` inside runtime-compiled bodies are explicitly out
of v1: each nests the compiler recursively, and `RunCall` in output additionally waits on the per-op
registration coverage of D3 (the head is never in `opcodeMap_` — the *set* is what's registered, so
a body `RunCall` needs its own membership plus the callee's entry-chain terminus, §3.2 here). The
exclusion is the *emitter's* contract — `register` itself does not police it (it registers
`RunCall`/`RunResume` fine per E1-iv); the in-language emitter/builder names the refusal
(`Opt`-empty + diagnostics record) for out-of-scope input.

**D6 — Arena discipline.** Runtime compilation runs on the agent's own arena through `cells"…"`; a
second arena is refused (parent §11's second-arena ruling; cross-arena handles are the §4.5
stale-handle class). `cells"…"` mints need no mark rule of their own — they are trailed arena writes
that `release` truncates, and nothing they mint enters `opcodeMap_`, so the `compiler.cpp:122-124`
hazard cannot arise on the mint side. It arises on the `register` side — a registration under a live
mark recreates that poisoning — and on the dim side (`meta.cpp:69-72`); the mark-window rule
therefore lives in D3's surface. One caveat on the arena-residue analogy: a cells-AST is arena
garbage where the C++ AST is heap-freed — and honestly bounded: `compact()` never reclaims cells (it
tightens link/content runs only), so committed-turn garbage is **session-persistent until
`promote`**, dropped only at the next incarnation's reachability walk — the grant `maxCells` is the
only bound today, and the fuel section's per-compile cell budget is priced accordingly.

**D7 — Type story.** Runtime-compiled fragments honestly type as `Code<Cell>` exactly — the type
`test_of`/`body_of`/`code_named` already return and the handle `register` vends; bare `Code` is only
a parameter wildcard (on which `run` yields `unknown`, `checker.cpp:1223-1226`), and `T` is
checker-computed which a v1 in-language checker may not derive. `run`'s dynamic arm is already
descriptor-driven and type-agnostic (`RunCall` reads only arity and `+d.name` records — never type
strings). Two honesty caveats on `Code<Cell>`: `run` under it yields `Cell` statically, and result
marshaling is descriptor-driven not `T`-driven (`refTypedResult`, `executor.cpp:695-716`) — a
fragment returning `Int`/`()` yields a `Cell`-typed value holding a non-position, so the v1 emitter
owes its caller a convention (read `result`'s value, never its position) and `run_checked` is where
the `Cell`→`Int` narrowing belongs; and `Code<Cell>` cannot reach `Code<()>`-invariant params —
`as_body` is the checked narrow (§3.2 here). Call sites needing `Code<Int>` pin it at their own
boundary (`run_checked`, U-M10, is the natural pure-context wrapper — **open question Q6**).

## 5 What stays out

- `eval(string)` — still refused; runtime compilation consumes AST cells or builder input, never raw
  text (until the parser stage self-hosts, at which point the *compiler* parses — still no eval).
- A second emitter — per U-M1's own direction, output must satisfy the exact emission contract; a
  parallel "convenient" op-minting surface would fork the discipline.
- Suspending fragments (U-M6), remote `Code` mobility (U-M2), autodiff (U-M3), source quoting
  (U-M5), learned plan selection (U-M4) — all downstream; listed so their dependencies on this
  design are visible.

## 6 Open questions for review

Review resolved Q4 and Q5 into D3's contract (per-op graph-derived registration; no map mutation
under live marks) — they remain listed for the record.

- **Q1 — Phasing boundary.** D1 now stages the owed `std:meta` emit-arm surface and the `register`
  verb first, host-service compile third — each stage is independently useful, and the frontend
  genuinely comes last. Is a host- service compile worth shipping as a milestone at all, or does the
  builder surface (stage 4) reach runtime source→graph sooner via structured input? Review
  consensus: ship stages 1–2 unconditionally, let metacircularity earn its keep.
- **Q2 — AST schema ownership.** Who defines the cells-encoded AST — the emitter (private contract),
  or a shared `design/` schema document so `validate`-like tooling and a separately-staged checker
  can inspect intermediate forms? Review consensus: shared, versioned kind tags, spec- document
  weight not yet warranted.
- **Q3 — Install path.** First consumer of runtime-compiled heads: `graft_body`/ `graft_where`
  (least machinery — signature match + `validate` + consumption), `learn_*` (adds recipe →
  `verifyClosed` → `d.code` → `mintPatch` → genesis replay), or a `spawn`-able carrier (needs P16
  mobility)? Review consensus: `graft_*` first, `learn_*` second, spawn last — with the `as_body`
  hop for the body-position verbs: `graft_body` takes `Code<()>`, so `let b := as_body(h)` precedes
  `graft_body(c, b)`; `graft_where`/`graft_method` take the `Code<Cell>` handle directly.
- **Q4 — Nested `run` in output — resolved.** Registration is per-op over the full op-reachable set
  (spin chain, wing-operand-reachable ops, `+d.entry` termini); "the head's own registration" is a
  category error — heads are never in `opcodeMap_`. A body `RunCall` needs its own membership plus
  the callee's terminus membership (E1-iv names the missing callee). Kept out of v1 per D5; the
  `Call`+`Bind`+`Return` callsite shape (`Call` ≤ `FieldSet`, blind jump, manual vars binding) is
  the documented escape hatch for a statically-known callee, at the price of skipping descriptor
  checks.
- **Q5 — Marks window — resolved as deferred application.** `register` under live marks records a
  trailed pending-registration marker; the engine drains when `outstandingMarks()==0` (D3). The rule
  is *no untrailed VM state mutation under marks* — `opcodeMap_`, `cursorPC_`, `memoizedOps_`,
  cursor spawns alike; compile-*minting* under a mark is safe (trailed, dies with the window) but
  ephemeral. The rule's one prior violation — a registered landing-pad `Halt` — is already minted
  unregistered (`meta.cpp:757`), `Halt` ≤ `FieldSet` decoding by content.
- **Q6 — Trust boundary.** `run` on a runtime-compiled `Code<Cell>` — accepted as today (the dynamic
  arm is already descriptor-driven and action-gated; registration *is* the trust ceremony), or gated
  behind `run_checked`/capability? Review consensus: accept as today; `run_checked` is the
  pure-context wrapper when U-M10 lands.
- **Q7 — The join problem — resolved to (c), the bootstrap fragment.** `New` mints
  `subject +d → fresh` only, and `cells"…"` creation literals mint string content only — so
  existing→existing joins *and* int64 op content both route through one mechanism: a genesis-minted
  `writer`/`linker` fragment pair (one `Value` op, one `Link` op; both ≤ `FieldSet`, executed via
  `runFragment`) invoked with ref-box/int64 args. Options (a)/(b) are rejected on the doc's own §1
  rule — tail duplication and `Jump` indirections deviate from the emission contract a runtime
  fragment must be indistinguishable under.

## 7 Relationship to the §15 deferral list

This document addresses U-M1 only. U-M2's receive side *is* this design's ceremony — a received
graph needs exactly enumerate → `validate` → kind-register → graft, and the durable
kind-registered-heads carrier *is* the portable code identity mobility wants: a `(lineage, label)`
pair is exactly the address a transclusion of a fragment resolves — code mobility and any future
xanadoc-transclusion of fragments become a consequence, not an accident. U-M9 (`specialize`) needs
the same builder surface D1 stages fourth. U-M5 is informed (AST-as-cells as intermediate only — the
view it wants is a byproduct). U-M6/U-M3/U-M4 remain unaddressed — orthogonal, not enabled. The
staging order in §4 D1 here deliberately lands the `std:meta` emit-arm surface first: it is the
missing dispatch for `reify`, `validate`, `graft_*`, and `learn_*` — machinery already implemented
engine-side and accepted by the checker but unreachable from the language. One substrate note the
doc owes: `std:meta`'s body today is C++ `MetaRuntime` methods, not the Vortex-routine prelude M7
promised — the same "language is its own substrate" rule this design invokes for the frontend
eventually applies to the verbs it installs through (`validate` — a `Linked`-walk + arity predicates
— and `reify` are the natural first re-expressions once the string surface exists).
