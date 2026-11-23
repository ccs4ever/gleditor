# R1 frontend language review

- Reviewed target: `design/vortex-io-implementation-plan.md`
- Target SHA-256: `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`
- Scope: callable surface, grammar, value/result types, errors, ownership,
  lowering, and rollout for VQL, VPL, VProlog, VLace, VIcon, and VAgent.
- Result: **3 MAJOR, 0 BLOCKING, 1 MINOR — REVISION REQUIRED.** The ownership
  architecture is sound and language-neutral in principle, but the claimed
  first-bridge forms do not yet specify how an accepted request or typed
  refusal is represented and consumed consistently. Several of the forms
  cannot be expressed by the current frontend grammar, and the VAgent example
  does not correspond to its staged effect-call model.

## Findings

### F1 — MAJOR: Common language-visible submit result is not defined

Section 10 gives the shared form as
`io.submit(cap, op) -> Result<Request, AdmissionError>` and says admission
refusal is a value, never a missing cell. But §3.2 says outcomes are tagged
cells, §4's C++ API returns `Admitted{RequestCell}` or `Refused{Rejected}`,
and the per-language forms mostly bind a request directly (`let r = ...`, VPL
`Reqs`, Prolog `io_submit/3`, VIcon `req := ...`). None specifies how each
language distinguishes an admitted request from refusal, what type a language
variable holds, or how refusal avoids becoming ordinary failure/no-row.
`await` also accepts `req` without saying whether it accepts a refusal value.

This is not only surface polish: a caller cannot safely compose or lower the
shown code without choosing whether submit returns a request-or-error sum,
an outcome cell, two output arguments, or a host-side exception. The current
types also need to make explicit that the language sees a managed request
cell/opaque request value, never native `RequestId`. The `PendingAwait` in §5
may use native IDs internally, but it must remain strictly host/VM-private.

**Concrete fix:** choose one managed graph value contract and apply it to all
six bridges. For example, submit yields a tagged `Admitted{requestCell}` or
`Refused{outcomeCell}` cell; request/outcome cells are normal non-persistable
managed values. State each frontend's pattern/term/match syntax for that sum,
and require `await` to accept only `Admitted`'s request cell. Add one
cross-language trace proving refusal is a value and never a Prolog failure,
VQL empty result, or VPL dropped slot. Keep any RequestId and continuation
tokens in host/service routing only.

Evidence: plan §§3.2–4 and §10; VQL's public executor returns `expected<vector<CellRef>, QueryError>`
(`apps/common/xanadu/vql/execution.hpp`); VPL returns an `ExecutionResult` with
a `VplView` (`apps/common/xanadu/vpl/execution.hpp`); VProlog's query model
uses `CompiledQuery` and solution vectors (`apps/common/xanadu/vprolog/compiler.hpp`).
All need an explicit adapter result mapping rather than C++ `Result` syntax
being presumed language-visible.

### F2 — MAJOR: VQL/VPL/VProlog/VIcon suspension and lowering contracts need callable outcomes

The table names an async await surface but leaves its evaluation result type
and continuation carrier implicit. This causes different concrete gaps:

- **VQL:** The source already has `ScriptBody`, `ExecutionBlock`, `ActionClause`
  and `EffectClause`, with `weave` as the effect action
  (`vql/ast.hpp:469-532`, `vql/parser.cpp:1280-1394`). The proposed
  `let r = ...; await r` is not the existing script grammar (script bindings
  use `let $r := ...`; effects are `weave`, and values are query expressions).
  A `VQL_IO_EFFECT_CONTEXT` diagnostic and "command/perform positions" are
  therefore not grounded in an existing context. Also `QueryExecutor::execute`
  returns a terminal vector, not a parked activation; the plan says retain
  row/request association without specifying a VQL-specific resume result or
  where the yielded `Pending` appears. The source v0.11 plan explicitly
  prohibits row-generation effects, so this should be a script action lowering,
  not a query expression or row.
- **VPL:** `submit¨ Caps Specs` is consonant with VPL's existing each adverb
  and shape-aware `VplView`, but neither its result shape nor mixed
  `Pending`/per-slot `Refused` handling is defined. The plan says refusal
  stays at the input position while also saying `await¨ Reqs`; it must state
  whether `Reqs` is a shape-preserving sum view and what `await¨` does to a
  refused position. `Executor::execute/run` currently returns a completed
  `ExecutionResult` (`vpl/execution.hpp`), so the host result that parks and
  later resumes that same activation is missing. Do not encode Pending as an
  `ExecutionError` or a terminal slot.
- **VProlog:** `io_submit/3` and `io_await/2` fit compound-goal syntax, but no
  result argument communicates typed admission refusal; failure of the
  predicate is ordinary Prolog failure. The compiler currently compiles a
  query to goals/variables and `solve()` returns `LogicSolution` rows
  (`vprolog/compiler.hpp`). A top-level-only effect context needs a specified
  query grammar/driver gate; arbitrary predicate clauses can otherwise call
  it under choice. A pending result must park the compiled VM activation and
  preserve Prolog bindings/trail, while remaining distinct from logical
  failure.
- **VIcon:** Its proposed calls fit the generic `ECall` AST, but `resume()`
  currently returns `optional<CellRef>` where `nullopt` is Icon failure
  (`vicon/driver.hpp`). The plan must define a distinct driver result for
  Pending, and how a typed submit refusal is matched without treating it as
  generator failure. Keep `write(value)` as host-yield, as the plan correctly
  does; stdout sink admission/outcome needs the same typed contract.

**Concrete fix:** for each bridge give one complete source snippet from
admission through refusal handling, await-Pending, terminal outcome matching,
and payload gating. Specify a frontend-owned continuation/result wrapper
around the parked activation (VQL script/query state, VPL activation and
shape, Prolog frame/substitution/trail, VIcon generator cursor). The host
resumes the same request and continuation; it does not run the source submit
again. This can be implemented without language-specific branches in
VortexCore/VM.

Evidence: source APIs cited above; VIcon `driver.hpp` documents `nullopt` as
ordinary failure; VQL parser has explicit statement/effect grammar while the
proposed example uses a different let/action shape. VPL's source already
supports each-adverb lowering, but no pending executor API.

### F3 — MAJOR: VLace and VAgent target forms are not operationally specified

**VLace.** The target `perform (submit cap (ReadBytes off max)) >>= await`
does not say which part constructs an `IO` value, which part admits the
request, or what type `perform` and `await` return. Current `lower_io.cpp`
constructs opaque `IO.Pure/Bind/Then/PutStr/PutStrLn/GetLine` terms, and
`runIoOnVm` executes leaves synchronously against `istream`/`ostream`; there
is no `perform` language primitive or parked driver result. Vlace is
lazy-by-need, so admission cannot happen by forcing a thunk during ordinary
evaluation/rendering. Make `perform` the runner boundary that returns a
frontend execution result (`Pending` or a terminal IO value), and define
whether `submit` is an action constructor or a primitive reached only during
perform. Its terminal payload/error constructors must be non-forgeable in
the same way current `IO.*` tags are.

**VAgent.** The table's `stage effect Write{writer, bytes: freeze(buffer),
after:lastWrite}` is not the current VAgent surface. The parser supports
effect signatures/native skills and `perform` goal declarations; checker
rules permit effect calls as a bounded handler action or a suspending plan
statement (`vagent/parser.cpp:439-452, 532-537`; `vagent/checker.cpp:1274-1290`).
The executor's `effectcall` path stages a typed outbox entry and correlated
`EffectDone`/`EffectFailed` wake (`vagent/executor.cpp:1260-1325`); the adapter
drain invokes `NativeBody` with mutable `WorkerContext`
(`vagent/adapter.hpp`). Thus the target is an architectural pseudo-record,
not a callable VAgent program. It also leaves unclear whether `request` in
`on EffectDone{request,outcome}` is a VAgent binding or native RequestId.

**Concrete fix:** for VLace specify the AST operation/result constructors,
runner method and lazy forcing rule; retain explicit perform-only admission
and represent Pending separately from a value-level terminal `Outcome`. For
VAgent use a source-shaped effect signature/call and the existing actor
`EffectDone`/`EffectFailed` correlation vocabulary; state that actor code
sees a language-level correlation/outcome cell, never RequestId. Define the
owner snapshot → durable journal acknowledgement → service admission → later
owner-turn reply path, with no mutable `WorkerContext` crossing to workers.
Local-runner `perform` must be identified as a separate host mode.

### F4 — MINOR: Direct Vortex baseline appears as a seventh frontend row and is malformed

Section 10 calls this the six frontend bridge contract, then adds a
`Direct Vortex` row. This is useful as a host/protocol conformance trace, not
a seventh language. Its Markdown code span contains an unescaped `|`, which
splits the table cell; it also conflates `Pending` with outcome in a way that
the source v0.11 and §1 distinguish.

**Concrete fix:** label this as the shared-host baseline outside the six
language count and write the result as `Pending or OutcomeCell` (without a
table-delimiting pipe). Keep Pending nonterminal.

## Cross-cutting positive evidence and scope

- Section 1's no-production-ABI statement correctly frees implementation
  signatures while preserving on-disk and `CompactOpNode` invariants.
- Sections 4–5 keep native buffers, endpoint descriptors and RequestId out
  of program cells, and prohibit frontend-specific Core/VM branches. Retain
  that boundary; frontend parsers, type/effect checks, result mapping and
  continuation state belong in the six adapters.
- VPL's array shape, deterministic admission position, and completion
  reordering rule are well grounded in the array language. VIcon's explicit
  distinction between yield and stdout sink is correct. VLace's pure action
  construction and explicit driver boundary are the right lazy-language
  constraint. VAgent's post-commit journal and refusal-before-proof rules are
  essential and source-backed.
- Staged scope needs one clarification: §2 and §10 say all six interfaces
  ship or are explicitly unsupported, but §10 marks VQL `submit_each` as a
  later gate and VPL frame/message modes unsupported. Say whether those
  omitted multiplicities/modes are typed unsupported in M7 and name the exact
  diagnostics/results. The plan's rollout can remain staged by frontend so
  there is no blocking fallback; no advertised path may continue synchronous
  host I/O.

## Tally and verdict

| Severity | Count |
| --- | ---: |
| Blocking | 0 |
| Major | 3 |
| Minor | 1 |

**Verdict: REVISION REQUIRED.** Resolve F1–F3 before treating §10 as an
implementation contract. The architecture is implementable without ABI
compatibility work, but callers need concrete managed sum values, grammar,
diagnostics and parked-execution results to lower these forms safely.

## Review limits

Read the frozen plan at the recorded SHA, the source v0.11 design, and the
named frontend parser/AST/executor/adapter boundaries. No runtime tests were
run and no implementation files were changed.
