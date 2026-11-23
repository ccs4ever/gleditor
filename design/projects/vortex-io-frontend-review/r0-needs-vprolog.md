# VProlog stakeholder needs (pre-review)

Target: `design/vortex-io-redesign.md` v0.9, a working draft. This is a source-grounded needs note,
not a formal R1 critique or frozen syntax/API. Proposed snippets below are illustrative. There are
no production users or ABI constraints; generic in-memory protocol design may change freely. Vortex
core/VM stays language-neutral, and VProlog owns its goal semantics, legal effect positions, and
mapping of typed outcomes into logic-level behavior.

## Current VProlog/Vlog facts

- The current source tree compiles Prolog queries to VM frames: `Compiler::solve` delegates to
  `stdlib_.solveQuery` (`apps/common/xanadu/vprolog/compiler.cpp:243-250`), and
  `VortexStdLib::solve` calls `logic::compileAndRunQuery`
  (`apps/common/xanadu/vortex/logic_solve.cpp:79-93`). The logic compiler emits clause alternatives,
  calls, unification, and returns into the VM; backtracking is the VM's `Fail` and arena rollback
  (`apps/common/xanadu/vortex/logic_compile_core.cpp:1-20`). This differs from the older
  recursive-resolver description at the top of `design/vprolog-lens-bridge.md`; do not base the new
  IO continuation design on that historical seam.
- Vlog terms are ordinary cells. Variables are cells on `d.vars`, bindings splice `d.clone` ranks,
  and arena mark/release undoes them; failed unification may leave partial bindings for the caller's
  release (`apps/common/xanadu/zigzag/vlog.hpp:11-20, 34-42`; `vlog.cpp:88-141`). A pending await
  must preserve its request identity outside that reversible binding image so rewind cannot replay
  or erase the external operation.
- Prolog search is nondeterministic. Clause/goal alternatives can revisit a goal after backtracking;
  explicit submission at such a site is a fresh request each time. A request variable that is
  unbound by rewind must not destroy the service's admitted operation, and if the program can still
  name that request through another live branch it must re-await the same observation.
- Current `vprolog-lens-bridge.md` remains useful as a frontend model for handles, generator
  predicates, and backtracking cleanup. It documents Int64 cell handles and predicates that
  enumerate by mark/unify/recurse/release (`design/vprolog-lens-bridge.md:44-60, 400-445`), but
  those are lens proposals, not the shared IO API. Its `lens_*` refusal-as-failure convention must
  not be copied for transport outcomes: the v0.9 generic contract correctly keeps typed terminal
  outcomes distinct from logical failure (`design/vortex-io-redesign.md:246-250`).
- The general Vortex IO draft says the vprolog seam is mark/release and calls for resumable async
  execution (`design/vortex-io-redesign.md:205-223`), proposes
  `submit(Cap, read(...), Req), await(Req, Outcome)` outside speculative search unless declared
  otherwise (`:499-507`), and requires Pending to park the VM and preserve frames/output
  bindings/choice state (`:519-545`). Because current Prolog is already lowered to VM frames, this
  is implementable at the shared VM suspension boundary; the Prolog compiler/adapter must still make
  effect multiplicity and Prolog-level outcome recovery explicit.

## VProlog requirements for the shared protocol

1. **Submission semantics respect search.** A `submit` predicate is the only admission point. The
   bridge must state which goal positions may contain it. It must reject or explicitly mark
   submissions reachable under nondeterministic generators, negation-as-failure, or speculative
   alternatives unless the program opts into that behavior. If an allowed branch revisits the submit
   goal, it admits a new operation. Reaching `await` again with the same bound request observes the
   same retained request and never replays submit.
1. **Pending is neither success nor failure.** A pending request parks the same Prolog activation at
   the await site. It must not bind an outcome, count as a solution, trigger ordinary
   failure/backtracking, or consume resolver/VM work budget while parked. Resume continues from
   await with the same request identity and preserved frame/choice/trail state.
1. **Logical variable/cell boundary stays honest.** Public request, capability, payload, progress,
   and outcome values are ordinary cells. Logical variables may unify with opaque cell
   handles/outcome terms, but native descriptors, pointers, request records, buffers, and
   continuation objects never enter terms or Vortex cells. The frontend validates tags, arena
   incarnation, and authority before using an opaque handle; integer-looking cells are not proof of
   a live capability/request.
1. **Transport error is data until the program chooses recovery.** Logical failure means no
   proof/solution; EOF is a normal stream terminal; rejection, backend failure, cancellation,
   overflow/gap, partial transfer, and unknown external completion are typed outcomes. A clause
   fallback may inspect a typed outcome, but IO failure must not silently become predicate failure
   or be masked by `\\+`, since that turns transport state into a false logical conclusion. Syntax
   for matching/refusal is frontend-owned and remains proposed.
1. **Backtracking and cut clean local state without claiming external undo.** Cut, failure,
   exceptions, query abandonment, and arena destruction release branch-local bindings and payload
   leases / waiter registrations exactly once. They do not claim to retract admitted requests or
   sent bytes. Separate wait cancellation from request cancellation; request cancellation is
   explicit and reports progress/uncertainty. Retained service outcomes remain bounded and follow
   the shared request-retention/lease policy after a branch disappears.
1. **Lens and content interaction remains cell-based and source-aware.** Prolog may use
   `lens_open`/`lens_children` style generators to query projected cells, and explicit submit/await
   to request pages or payload ranges. Merely enumerating, unifying, rendering, or resolving a lens
   result does not submit IO. Mutable file/media paths are locators; range results identify observed
   source generation and lease, and capture is explicit. Large binary/media reads use bounded
   ranges, not whole-file conversion to a Prolog string.
1. **Stream modes are explicit, with framing policy separated.** The cell request API should expose
   byte-level unbuffered reads, line reads with a bound, fixed-size reads, and a generic
   framed/token-like mode where the backend supports it. Prolog/DCG grammar tokenization is a pure
   frontend operation over captured bytes; generic byte transport must not encode Prolog grammar or
   parser state. Oversized lines/tokens produce typed bound/overflow outcomes, not unbounded
   buffering; byte decoding and partial/multibyte boundaries are explicit.
1. **Interactive ordering is observable.** A prompt write is not necessarily visible when merely
   queued. Before asking for interactive input, Prolog can explicitly submit and await a
   flush/visibility operation (or an output request whose terminal contract includes flush). Output
   request identity, bytes actually accepted, cancellation/close, and uncertainty stay observable.
   Backtracking cannot implicitly flush, discard, duplicate, or undo output.
1. **No Prolog-specific solver semantics enter generic Vortex.** Generic capability,
   submit/admission, Pending, await, outcome, payload, cancellation, and suspension rules are shared
   cells/runtime mechanics. Prolog compiler/adapter owns whether and where an effect predicate is
   legal, goal position multiplicity, pending-to-continuation mapping, logical-variable bindings,
   and typed outcome branching. The core/VM does not gain Prolog rule state, solver policy, `\+`
   semantics, DCG/token grammar, or Prolog-named request opcodes.

## Two proposed Prolog examples

These examples are frontend sketches, not existing VProlog predicates or grammar commitments.
`io_submit`, `io_await`, `io_outcome`, and `io_payload` are proposed names for lowering to the
generic cell protocol.

### 1. Bounded interactive input and pure DCG parsing

```prolog
% Proposed: one explicit bounded line request, awaited without resubmission.
read_command(Cap, Command) :-
    io_submit(Cap, read_line(max_bytes(4096)), Req),
    io_await(Req, Outcome),
    io_outcome(Outcome, complete(Payload)),
    io_payload(Payload, Bytes),
    phrase(command(Command), Bytes).

% Proposed frontend grammar only; this DCG is pure over captured bytes.
command(status) --> "status", newline.
command(open(Path)) --> "open ", path_bytes(Path), newline.
```

If `io_await` is Pending, the runtime parks this same goal activation. Its eventual result either
supplies one completed line or a typed terminal state; reevaluating `io_submit` is new work. DCG
parsing, lens queries, and display only inspect materialized cells. EOF and an empty line must be
distinguishable.

### 2. Flush the prompt before submitting an input read

```prolog
% Proposed: wait for the prompt's flush completion before requesting a line.
prompt_and_read(Cap, Prompt, Line) :-
    io_submit(Cap, write_bytes(Prompt), WriteReq),
    io_await(WriteReq, WriteResult),
    io_outcome(WriteResult, complete(_)),
    io_submit(Cap, flush, FlushReq),
    io_await(FlushReq, FlushResult),
    io_outcome(FlushResult, complete(_)),
    io_submit(Cap, read_line(max_bytes(4096)), ReadReq),
    io_await(ReadReq, ReadResult),
    io_outcome(ReadResult, complete(LinePayload)),
    io_payload(LinePayload, Line).
```

For real buffering, the API may define `flush` as a separate request or a write-completion
guarantee, but that guarantee must be explicit. A failure/cancellation between steps leaves earlier
external progress intact and inspectable; later steps are not silently run. In speculative code this
sequence requires an explicit placement policy because each submit is an irreversible external
operation.

## Generic request/outcome trace as VProlog should observe it

1. Host supplies a capability cell; pure Prolog terms describe operation/mode/bounds. Lens lookup,
   term construction, unification, rendering, and DCG parsing do not admit IO.
1. At a statically permitted `io_submit` goal, the frontend snapshots the request terms into an
   immutable plan and asks the shared service for admission. Admission returns a request cell or
   typed refusal. Each execution of this goal is a fresh identity, including after backtracking.
1. Worker performs IO off the evaluation/UI thread and posts typed progress/completion keyed by
   service identity and endpoint generation. It holds no Vlog variables, Vortex arena refs, Prolog
   frame, or DCG continuation.
1. `io_await(Req, Outcome)` either exposes `Pending` as a parked activation or publishes the same
   retained terminal observation as ordinary cells. It never jumps to `io_submit`. Rewind may remove
   the Prolog binding and local payload cells; a retained request observation can be rematerialized
   if a live program reference still names it.
1. `Complete`, `Eof`, `Partial`, `Rejected`, `Failed`, `Cancelled`, `Overflow/Gap`, and `Unknown`
   are typed outcome terms/cells. The program explicitly decides whether a given outcome yields a
   solution, tries a recovery clause, or fails logically. `Pending` is not an outcome interpreted as
   logical failure.
1. Backtracking/cut/error/abandonment release trail-owned cells, continuation and waiter roots, and
   transient leases with incarnation-safe ownership. They do not claim external IO was rolled back.
   Await-wait cancellation and request cancellation remain distinct; only an explicit request cancel
   can ask the backend to stop.
1. Read payload through resolver-aware cell ranges under an explicit bound and lease. Mutable source
   generation, partial bytes, decode boundaries, and capture are explicit; only explicit capture
   makes transient content permanent.

## Dialogue with VIcon reviewer

VIcon agrees that generic Pending should carry only a language-neutral parked signal/request
identity, while the frontend activation owns its cursor, choice stack, await site, and bindings.
Applied to VProlog, its adapter/runtime activation must preserve compiled goal position, VM
choice/frame state, and the Prolog trail while parked; no Prolog-specific continuation state belongs
in generic core/VM. This exchange confirms the ownership boundary above.

On abandonment, we agree that stopping the waiter is distinct from cancelling admitted IO. Branch
cut/query drop may unregister waiter roots and release local leases; request execution and bounded
terminal retention follow shared service policy unless explicit request cancellation is issued.
Cancellation policy must avoid orphan retention, but must not silently claim an effect was undone.
This sharpens requirement 5; it does not change the core/frontend boundary.

## Questions for convergence

- Which goal contexts permit submit: top-level/conjunction only by default, explicit once/staged
  forms for speculative search, or an opt-in declaration? How are recursive loops and disjunction
  multiplicity diagnosed?
- How does pending resume work through the current Prolog-to-VM lowering without duplicating submit
  or treating Pending as a solution/failure? What continuation identity survives materialized result
  rewind?
- How does Prolog pattern-match typed request/outcome cells while distinguishing logical failure
  from IO status, including use under negation-as-failure?
- Which request/output completion semantics guarantee a visible prompt before input, and how do
  partial writes, cancellation, close, and flush failures surface?
- Does the shared request mode vocabulary include line/fixed/unbuffered/framed transport bounds
  while leaving Prolog DCG grammar and token semantics in the frontend?
- How are requests/outcomes/payload leases reclaimed after cut, all solutions exhausted, exceptions,
  REPL cancellation, or abandoned query, without resending or silently cancelling external work?
- What range/version contract can file/media lenses honestly offer for mutable sources, random
  access, decoding, and explicit capture?

### Input cursor/framing point from VIcon dialogue

VIcon asked how stream read-ahead and token leftovers behave under choice release. VProlog's rule is
that the external input cursor is not trailed: by default, submit stays outside speculative search;
an explicit opt-in search read consumes bytes once even if its branch later fails. Re-awaiting a
still-live request can inspect the same retained bytes, while reaching submit again requests the
next sequence. A bounded line/fixed/framed read may read ahead, but the service's endpoint buffer
must retain the unread suffix under its own lease so the next request starts after the returned
frame, not after the extra read. Cut/query abandonment may release local waiter and decoded/token
views; it cannot rewind consumed input or discard endpoint-owned unread bytes without an explicit
close/cancel policy. Generic transport owns bounded byte framing and carry-over; Prolog DCG/token
grammar remains pure frontend work over delivered byte cells. This exchange sharpened the stream
lifecycle requirement without moving Prolog grammar or choice state into generic Vortex.
