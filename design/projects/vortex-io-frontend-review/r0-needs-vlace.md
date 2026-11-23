# VLace stakeholder needs for Vortex I/O (pre-review)

Target: `design/vortex-io-redesign.md` v0.9. This is a source-grounded frontend-needs note for the
draft editor, not a verdict on the frozen design. Proposed syntax below is illustrative only, is not
implemented, and does not freeze grammar or ABI. VLace owns its effect typing/placement, lazy
evaluation boundaries, evaluation multiplicity, and frontend-local continuation state; the shared
Vortex service/VM owns only generic cell-based requests, suspension, outcomes, and leases. No
production ABI is preserved.

## Current VLace behavior

- VLace is a lazy functional language in the current implementation, with parse → optional normative
  Hindley–Milner check → lowering/evaluation in `Driver::run`
  (`apps/common/xanadu/vlace/driver.cpp:22-49`). The language design describes lazy-by-default
  evaluation and thunk forcing (`design/vlace-functional-language.md:84-89`); this matters because
  merely constructing an `IO.*` description is not the same moment as performing its leaf.
- Its existing IO layer is a sequential action-description encoding, not a nonblocking request API.
  `lowerIoBuiltin` lowers `pure`, `bind`, `then`, `putStr`, `putStrLn`, and `getLine` to `IO.*`
  constructors (`apps/common/xanadu/vlace/lower_io.cpp:75-83`); `lowerIoBind` forces both operands
  to WHNF while making `IO.Bind`/`IO.Then` (`lower_io.cpp:54-73`).
- `runIoOnVm` interprets one action on a host call stack. It handles `IO.Bind`/`IO.Then` with its
  own `std::vector<Continuation>` (`lower_io.cpp:96-142`), performs console writes inline
  (`:151-167`), and performs `std::getline` inline (`:168-185`). This is synchronous host I/O; the
  current continuation stack is not a resumable VM suspension record. Effect leaves fall through to
  `performEffectLeaf` and execute their generated block synchronously (`:186-212`).
- The IO boundary is partly enforced by representation, not by a frontend effect system: `IO.*`
  constructor tags are reserved and opaque to user construction because forged tags could smuggle
  effects into `runIO` (`apps/common/xanadu/vlace/ast.hpp:234-257`). The type checker assigns `IO`
  result schemes to `openLens`, gesture verbs, and `promote`
  (`apps/common/xanadu/vlace/types.cpp:170-209`), but `Driver::run` has no effect-placement pass
  (`driver.cpp:34-48`). In particular, `--no-typecheck` exists in the CLI
  (`apps/vlace/main.cpp:131-134`), so the runtime boundary remains necessary.
- The standalone runner first calls `driver.run`, detects an IO-typed result, then calls
  `driver.runIO` with stdin/stdout (`apps/vlace/main.cpp:34-57`); the REPL instead calls
  `driver.run` per line and renders the result, without performing returned IO descriptions
  (`:62-99`). The compiler/type-checker CLI likewise reports type and optionally rendered value; it
  does not perform IO (`apps/vlacec/main.cpp:38-99`). A new Pending contract therefore needs an
  explicit host pump/resume path in the runner and a defined REPL session policy, not just a changed
  leaf handler.
- Current lens bridge code already goes beyond the older design summary: `openLens` calls
  `openLensUri`, creates and pins a `ProjectionSession`, and returns roots as `Ok (List Cell)` or
  refusal as `Err` (`apps/common/xanadu/vlace/lower_effects.cpp:288-350`). Lens gesture leaves route
  session-owned cells, then call synchronous `sessionMountAt`/unmount/page/edit/commit helpers
  (`lower_effects.cpp:352-385`, `:401-430` onward). The target's “implemented IO stream/lens
  handlers” line is accurate as current sync behavior, not as evidence of asynchronous execution
  (`design/vortex-io-redesign.md:212`).
- The design-only VLace language spec calls for a typed IO boundary and makes pure description
  construction distinct from performing it (`design/vlace-functional-language.md:11-20, 34-39`). The
  lens bridge explicitly says its proposed signatures are not implemented beyond the existing
  compiler (`design/vlace-lens-bridge.md:1-6`); it treats projected cells as stable names with
  content/ranks observed when lazy reads are forced, and orders those reads against mutations via IO
  bind (`:65-73`). This forces the redesign to preserve the rule while making a pending bind
  resumable.

## VLace-specific requirements for the shared protocol

1. **Keep the generic runtime language-neutral.** VLace type/effect policy, syntax, lazy thunk
   forcing, typed conversion from generic Outcome cells, and bind continuation state belong in VLace
   compiler/adapter code. The redesign's compiler/adapter boundary says exactly this
   (`design/vortex-io-redesign.md:330-337`). Do not put `IO`, a VLace AST node, purity analysis, or
   its handler policy into Vortex core/VM.
1. **Define what `perform` means under laziness.** The proposed trace
   `perform (submit cap (Read 0 4096)) >>= await` (`design/vortex-io-redesign.md:504`) needs a
   precise admission point. Forcing an ordinary pure thunk, constructing a request-spec, rendering
   an `IO` value, or comparing it must not submit. Only the explicit `perform`/runner boundary (or
   an equivalent statically checked operation) may admit; each executed perform site is fresh, while
   resuming an await retains its request identity.
1. **Order the IO bind across Pending.** VLace needs a host-visible suspension object that preserves
   the unexecuted suffix of `IO.Bind`/`IO.Then`, forced prefix values, thunk/memo state, and the
   currently awaited request. On resume it continues after the same leaf/outcome; it cannot replay a
   leaf or lose the pending action. Pending is neither `Result Err`, empty output, nor language
   failure. Specify whether the session state is tied to one driver run, how callers
   resume/cancel/abandon it, and when dropping it releases VLace references and payload leases.
1. **Preserve laziness and effect multiplicity.** `>>=`/`>>` are sequencing forms and ordinary pure
   expressions may be shared/memoized; the system must distinguish a saved request value from
   reevaluation of a submit action. A thunk containing an explicit effect must not execute
   accidentally when a pure consumer forces it, and a second explicit execution must be a fresh
   operation. Static effect checks should reject ambiguous effectful use in pure positions while
   keeping runtime guards for `--no-typecheck`.
1. **Expose typed outcomes as data.** Completion values should let VLace pattern-match success, EOF,
   partial bytes plus status/progress, rejection, backend error, cancellation/uncertainty,
   overflow/gap, and unknown outcome. EOF is normal input termination, distinct from a successful
   empty payload. Do not collapse external terminal outcomes into a thrown `EvalError` or
   `Err String` if that erases partial progress and retry uncertainty. Keep compile/type/evaluation
   errors distinct from operation Outcome.
1. **Keep payloads and capabilities as resolver-aware cells.** VLace needs pure, lazy range
   slicing/indexing over transient payload cells without reading an entire media file into a
   `String`. Copying a cell/value should retain the lease according to generic ownership rules;
   normal rendering and scalar/text decoding cannot silently capture bytes.
   Capability/request/effect-result values crossing the program boundary remain cells; native
   descriptors/buffers stay private.
1. **Map lens transactions to asynchronous operations without breaking lens semantics.**
   Open/page/refresh/commit probes and commit I/O must run off evaluation/UI threads. VLace must
   retain existing session roots/identity, dirty edit visibility, source-version conflict/refusal,
   and explicit commit/revert behavior. A commit outcome needs applied/partial/uncertain
   information; a failed commit cannot silently clear edits. `lensEdit` is intent within the
   declarative session until explicit commit; it is not equivalent to sending a stream write.
1. **Make cleanup scope explicit.** Dropping a pending VLace program or failing while evaluating a
   bind suffix must release language continuation refs, while an admitted external request stays
   service-owned/observable through its terminal-retention window. Scope-exit cleanup cannot pretend
   it cancelled/undid a sent write. The current Driver creates a fresh compiler/session arena for
   each run (`apps/common/xanadu/vlace/lower_io.cpp:284-291`), but the REPL is per-line and does not
   hold a resumable IO driver state; redesign should state whether/when that changes.

## Proposed VLace examples (not current syntax/API)

### 1. Submit once, suspend in bind, match typed outcome

```haskell
-- Proposed pseudocode: no such perform/submit/await syntax is implemented.
readChunk cap offset =
  perform (submit cap (ReadRange offset 4096)) >>= \req ->
  await req >>= \seen ->
  case seen of
    Pending -> suspendHere req
    Complete bytes -> pure (Chunk bytes)
    Partial bytes eof progress -> pure (PartialChunk bytes eof progress)
    Eof -> pure EndOfInput
    Failed kind progress -> pure (ReadFailure kind progress)
    Cancelled progress uncertain -> pure (ReadCancelled progress uncertain)
    Overflow gap -> pure (ReadGap gap)
    Unknown -> pure NeedsReconciliation
```

`perform`, `submit`, `await`, constructors and `suspendHere` above are proposed names only. The
essential split is: pure construction of the action is lazy and inert; the driver executes one
explicit submit leaf; Pending retains that exact request and the VLace bind continuation; resumption
observes the same request. A fresh re-execution of the perform site creates a fresh identity.

### 2. Lens edit followed by explicit asynchronous commit

```haskell
-- Proposed pseudocode; existing lens verbs are currently synchronous IO leaves.
editAndCommit sessionCell memberCell newText =
  perform (lensEdit sessionCell memberCell newText) >>= \editResult ->
  case editResult of
    Err refusal -> pure (EditRejected refusal)
    Ok Unit ->
      perform (lensCommit sessionCell) >>= \commitResult ->
      case commitResult of
        Pending -> suspendCommit sessionCell
        Ok applied -> pure (Committed applied)
        Err conflict -> pure (CommitRefused conflict)
```

Existing names/signatures are not asserted by this proposal: today `lensEdit` is
`Cell -> String -> IO (Result String Unit)` and `lensCommit` is `Cell -> IO (Result String Int)`
(`apps/common/xanadu/vlace/types.cpp:185-209`), and the effect handlers perform synchronous session
work (`lower_effects.cpp:276-280`, `:401-430`). The future API may change freely to distinguish
Pending and richer status. The example asks that edit remain staged lens intent, that commit be a
distinct explicit operation, and that a conflict/partial/unknown outcome preserve dirty state for
inspection/reconciliation.

## Generic request, outcome, and lifetime trace as VLace sees it

1. Pure lazy evaluation constructs capability, request-spec, and action cells. Forcing these values,
   inspecting an already-materialized Outcome, or rendering them admits no work.
1. At the designated explicit perform boundary, the adapter forces only the operands required to
   freeze an immutable native plan; it validates authority and reserves request/result/payload
   budget. Admission returns a fresh request cell or typed refusal before external work begins.
1. The generic service executes off the evaluation/UI thread. It owns native handles and immutable
   payload leases; no worker receives a VLace thunk, compiler, arena reference, or bind
   continuation. It emits bounded progress and exactly one terminal status plus partial
   payload/progress as applicable.
1. Await either returns the retained terminal Outcome or Pending. Pending parks VLace continuation
   state and returns to the host pump; it does not spin or consume cycle budget. Resuming the parked
   driver continues at await, not at submit. Re-evaluating the explicit submit action creates
   another request.
1. Terminal outcome is represented in cells and can be handled by normal VLace pattern matching.
   Pending does not masquerade as a terminal constructor; EOF is distinct from empty bytes; partial
   output/status remains observable for cancellation/failure.
1. Arena rewind/failure/abandonment may discard local result bindings and VLace continuation
   references. It cannot undo admitted I/O. Request/payload leases remain valid while service, saved
   continuations, cells, decoders, or capture own them; they are reclaimed after all owners release
   and backend users acknowledge release. Explicit capture is separate from read/render.

## Open questions for the editor / VLace adapter

- Is `perform` a distinct syntax with static checking, a runner-only execution boundary for all
  `IO`, or both? How is the boundary available to `vlace` REPL and embedding hosts without making
  ordinary `Driver::run` perform effects?
- Does a parked IO continuation retain the VM cursor or a VLace-owned bind stack/thunk graph? What
  survives replacement/destruction of Driver/Compiler, and how does the REPL pump a Pending run
  without blocking its input loop?
- How does `IO (Outcome a)` interact with the existing `IO (Result String a)` signatures for lens
  operations, and where are language-specific conversions kept so generic core stores only
  outcome/status cells?
- Can a lazily mapped list contain many submit actions? If yes, exactly which evaluation action
  admits each one and how is multiplicity bounded; if no, what diagnostic prevents force/render from
  unexpectedly dispatching a stream of effects?
- How do lens edits and commit attempts interact with ordinary effect submission identity? Lens edit
  is reversible dirty session state before commit; commit has versioned transaction semantics and
  may be uncertain after backend work. It must not inherit stream write retry rules that discard
  transaction validation.
- How are file and media range payload values exposed so pure lazy slicing does not pin unbounded
  bytes, decode as text, or silently capture permanent content?

No source/runtime changes are requested by this note.

## Additional VLace needs: buffering, framing, and scoped close

The common brief also asks frontends to cover unbuffered, line, fixed-buffer, and tokenized I/O. For
VLace, these are pure transformations over bounded payload cells plus explicit source/sink
operations; the generic service must not gain a VLace tokenizer, text decoder, or grammar AST.

A buffer adapter must state whether bytes are only staged locally or have crossed the external
boundary. Repeated append/encode calls that mutate a purely functional buffer should not send
anything. A buffered submission freezes one immutable chunk and mints one operation identity at the
explicit submit point. Re-evaluating that submit is fresh work; resuming/awaiting or inspecting the
resulting buffer/request cannot resend. `flush` means transfer to the backend and can therefore
report partial/unknown progress; `sync`/durable acknowledgement is a separate backend-specific
promise and may be refused where the backend cannot provide it. Aborting an unsubmitted buffer is
local cleanup; aborting after submission requests cancellation but cannot retract transferred bytes.

Line/token adapters need bounded maximum line/token sizes and explicit overflow behavior. They must
retain read-ahead bytes beyond the delimiter for the next consumer, with source-generation/range
identity and bounded leases. Text decoding across chunks must define incremental decoder state and
malformed/truncated-sequence outcomes; chunk boundaries may split a code point or token. Render,
list traversal, thunk forcing for display, and pure pattern matching must neither fetch more bytes
nor flush a buffer. A scoped writer/reader helper must keep asynchronous close/flush state through
Pending and report close failure; exceptional exit chooses an explicit abort/cancel policy rather
than silently treating cleanup as successful flush.

```haskell
-- Proposed pseudocode only: none of these constructors/functions are current VLace APIs.
withBufferedWriter sink $ \writer ->
  appendText writer "header\n" >>= \writer1 ->
  appendText writer body >>= \writer2 ->
  perform (flush writer2) >>= \flushState ->
  case flushState of
    Pending -> suspendFlush writer2
    Flushed transferred -> pure (Written transferred)
    Partial transferred uncertain -> pure (WriteUncertain transferred uncertain)
    Failed kind transferred -> pure (WriteFailed kind transferred)
-- A successful scope close may itself await backend close/flush acknowledgement;
-- an abandoned/failed scope must explicitly abort and report any partial transfer.
```

The names above are proposal sketches, not standard library names. VLace may implement line/token
framing as ordinary pure functions over leased byte chunks and adapter-local decoder state. The
shared protocol supplies bounded range/payload cells, service requests, status/outcomes, and leases;
it does not supply language-specific framing semantics. For a VAgent composition, a staged
post-commit write intent and a VLace explicit stream flush remain distinct policies even if both
eventually use the same service: VAgent does not submit until commit/durable admission, while VLace
submission is at its explicit perform site. The existing target requires preserving that VAgent
boundary (`design/vortex-io-redesign.md:262-270`). The VAgent reviewer confirms that actor intents
must freeze semantic bytes/order before commit, then transfer a bounded immutable lease to durable
post-commit admission; see the dialogue below.

## Dialogue with VAgent stakeholder

VAgent answered the question about durable post-commit admission: the actor intent freezes semantic
bytes and endpoint order before commit can expose it; after commit, the adapter transfers a bounded
immutable snapshot/lease into durable admission, records attempt metadata, then permits service
work. The ledger need not persist transient bytes, so loss after admission can still be `Unknown`.
This separates VAgent's reversible staged intent and post-commit admission from VLace's explicit
`perform`/`flush` point, while both use the same generic service. I accept the distinction: semantic
freeze belongs to VLace's explicit perform evaluation for a stream write, whereas actor staging must
already define immutable meaning/order before turn commit. Shared core should model request
identity/outcome/lease without deciding either frontend's policy. The exchange clarified that
“freeze” has two meanings here: fix bytes/order, then transfer lease and durably admit.

VAgent asked how a lazy immutable buffer distinguishes `flush` from `close` when the bind suffix is
abandoned after flush admission. My answer: keep the buffer as a pure immutable value of bounded
staged chunks. `perform (flush buffer)` freezes the chunk and creates one request; await/resume
never re-submits. `close` is a separate ordered endpoint-lifecycle effect; `abort` is an explicit
discard/cancel policy. If the continuation is abandoned after admission, its local references may be
released, but the admitted request's frozen lease and partial/unknown outcome remain service-owned;
abandonment cannot imply close, retry, or rollback. A graceful scoped writer must preserve
frontend-local finalizer state through Pending/abandonment and issue ordered close/abort work;
ordinary GC cannot block or imply successful close. A graceful close follows a terminal flush
result/barrier, and flush uncertainty must remain visible in the close path. This sharpens my
earlier request that scoped cleanup retain asynchronous state and report close failure.
