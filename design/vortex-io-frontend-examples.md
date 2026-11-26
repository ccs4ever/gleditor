# Vortex I/O frontend examples

These six examples expand §10 of the [implementation plan](vortex-io-implementation-plan.md). They
are proposed source-surface sketches, not syntax accepted by today's compilers and not a production
API. The implementation plan is authoritative for semantics and names. A frontend bridge must add
the syntax and runner behavior described there before an example can run.

## Shared host fixture and notation

For each example, assume the embedding host has opened an endpoint asynchronously, checked the
endpoint's reported operation and consistency guarantees, and supplied a managed capability cell
named `ReadCap` (or the direction-specific capability named in the example). The capability grants
only the operation and bounded byte count used below. Source programs receive that cell as an
ordinary immutable input; they do not construct native endpoints, descriptors, capability authority,
or private IDs. `read_bytes`, `read`, `submit`, `await`, and the tagged patterns shown below are
proposed frontend vocabulary from plan §10, not current builtins.

Every `submit` result is the managed sum `Admitted{request}` or `Refused{outcome}`. `await` is only
valid on an admitted request. A pending await is a runner disposition, never a language value or a
terminal outcome: the host parks the same activation and re-awaits the same request when notified.
Only an outcome whose tag carries a payload permits payload access. Empty payload is still distinct
from EOF. Refusal, EOF, partial progress, failure, cancellation, timeout, overflow and unknown
external effect remain inspectable outcomes; none means “no row”, Prolog failure, an empty VPL slot,
or successful close.

The examples do not invent language-level capture or shutdown calls. If an embedding application
chooses to preserve a returned transient payload, its host may explicitly pass the selected range to
`CaptureCoordinator::prepare`, `appendRequest`, and `commit` (plan §2); those are host APIs, not
source forms specified for any of the six frontends. No automatic capture occurs. Likewise, host
teardown retires its waiter, requests cancellation only when policy chooses it, and keeps leases and
request records until backend release is known; cancellation is not proof that work stopped.

## VQL: script effect outside query evaluation

The host binds `ReadCap` and `Offset` before the script starts. The plan's `EffectClause` runs once
at its script action site. The block below is proposed sequencing and match syntax: current VQL uses
`weave` for effects and has no sequential `match` arm form. The M7 grammar work must add this
script-only form outside FLWOR row traversal, filters, projections, and pure rendering, so the
script cannot accidentally multiply I/O by the number of rows.

```text
effect {
  let $submission := submit(ReadCap, read_bytes(Offset, 4096));
  let $seen := match $submission {
    admitted($request) => await($request),
    refused($outcome) => refused($outcome)
  };
  match $seen {
    refused($full_rejection) => retain($full_rejection),
    outcome(complete($payload, $progress), $full) =>
      retain_with_payload($full, $payload, $progress),
    outcome(partial($payload, $cause, $progress), $full) =>
      retain_with_payload($full, $payload, $progress),
    outcome(end_of_stream($progress), $full) => retain($full),
    outcome(failed($reason, $maybe_payload, $progress), $full) =>
      retain_with_optional_payload($full, $maybe_payload, $progress),
    outcome(cancelled($maybe_payload, $progress, $uncertain), $full) =>
      retain_with_optional_payload($full, $maybe_payload, $progress, $uncertain),
    outcome(timed_out($maybe_payload, $progress, $uncertain), $full) =>
      retain_with_optional_payload($full, $maybe_payload, $progress, $uncertain),
    outcome(overflow($gap, $progress), $full) => retain($full),
    outcome(message_too_large($required, $disposition, $progress), $full) => retain($full),
    outcome(stale($expected, $observed), $full) => retain($full),
    outcome(unknown($last_known), $full) => retain($full)
  }
}
```

The `outcome(tagged_fields, full)` wrapper in this proposed match makes the original managed
`OutcomeCell` available in every arm; `retain*` and `record_*` names stand for application-provided
pure transformations, not VQL builtins. Only `Complete`, `Partial`, and failure-like variants with
an actual optional payload permit payload access. Every branch keeps the whole typed outcome,
including progress, uncertainty, gap, oversize disposition, and generation fields. The runner
retains the effect activation at Pending and re-awaits its existing request; it does not repeat the
submit. A refusal takes the `refused` arm and starts no work. Querying/rendering the staged result
remains pure. A payload may be explicitly captured by the host after inspecting its source/range;
the VQL snippet does not make it permanent.

As built (M7-VQL and M7-VQL-b, `vql-query-language.md` §4.13), the script reads an outcome with path
functions instead of the proposed `outcome(...)` match: `effect { ... } as $r` names it and
`io_tag($r)`, `io_payload($r)` (only where the status gate exposes bytes), `io_bytes($r)`,
`io_error($r)` and `io_detail($r)` read its fields. `submit(...) within 250 ms`, a detached read
(`admitted($q) => $q`), `effect { await($h) }`, `effect { cancel($h) }` and
`effect { open("path") } as $cap` complete the surface; every terminal tag, `TimedOut` and
`Cancelled` included, is a value.

## VPL: shape-preserving request and outcome arrays

The host supplies `Caps` and `Specs` as equally shaped VPL views. Shape agreement is checked before
the first submission. The example uses the plan's each operator and keeps every refusal and result
at its original position even when backend completions arrive out of order.

```apl
Submissions ← submit¨ Caps Specs
Outcomes    ← await¨ Submissions
```

The outcome view has the same shape as `Submissions`: admitted positions become terminal outcomes;
refused positions pass through unchanged as submission results, separate from `IoOutcome`. Admission
is deterministic left-to-right under the configured bounded concurrency; shape is checked before the
first admission. `await¨` parks the VPL activation at its first pending position. The runner
re-awaits that same request after notification, then scans left-to-right for the next pending
position. Other submitted requests continue independently; their completion order never changes
array position. A fresh execution of `submit¨` is a new effect and may create new requests.

```text
match Outcomes with the same shape:
  Refused(fullRejection) at p => retain full submission refusal at p
  Complete(payload, progress, full) at p => decode payload; retain full at p
  Partial(payload, cause, progress, full) at p => inspect; decode supplied bytes only; retain full
  EndOfStream(progress, full) at p => retain full at p
  Failed(reason, optionalPayload, progress, full) at p => retain full; read payload only if present
  Cancelled(optionalPayload, progress, uncertain, full) at p => retain full
  TimedOut(optionalPayload, progress, uncertain, full) at p => retain full
  Overflow(gap, progress, full) at p => retain full at p
  MessageTooLarge(required, disposition, progress, full) at p => retain full at p
  Stale(expected, observed, full) at p => retain full at p
  Unknown(lastKnownProgress, full) at p => retain full at p
```

The match notation is explanatory pseudocode for the VPL bridge, not committed grammar. Each arm
retains the original outcome at its position; failure-like tags preserve their optional payload and
progress fields. A shape mismatch yields a frontend diagnostic before admission, never truncation or
recycling. Arithmetic and rendering of `Outcomes` are pure. The host retires the active waiter when
the activation is abandoned; request cancellation is a separate explicit host choice. Unsupported
message/frame modes are typed refusals, and no payload is dereferenced for a payload-less tag.

## VProlog/Vlog: top-level effect goal with explicit outcome binding

The embedding host injects `ReadCap` as a managed capability cell binding in the query environment;
the atom `ReadCap` in source is only the variable name and never authority. This effect is allowed
only as an explicit top-level query goal, outside user clauses and speculative alternatives.
Ordinary predicates remain pure. The following is the plan's proposed query-driver form; the
recognized sum match and `->` lowering must be added as one deterministic driver branch, not a
speculative Prolog alternative:

```prolog
io_submit(ReadCap, read(0, 4096), Submission),
( Submission = admitted(Req) -> io_await(Req, Seen)
; Submission = refused(Why), Seen = refused(Why)
),
IoResult = Seen.
```

The query result returns the bound `IoResult` term with the solution; the driver does not call a
possibly failing user predicate to interpret it. `refused(RejectedOutcomeCell)` retains the complete
pre-admission refusal and is not logical failure. For an admitted request, `IoResult` is the
unchanged tagged outcome cell. Its proposed source match is total over the read result set:

```text
Complete{payload, progress} => payload may be read; retain full outcome
EndOfStream{progress} => retain full outcome
Partial{payload, cause, progress} => payload may be read; retain full outcome
Failed{reason, optionalPayload, progress} => retain full; read payload only if present
Cancelled{optionalPayload, progress, uncertain} => retain full
TimedOut{optionalPayload, progress, uncertain} => retain full
Overflow{gap, progress} => retain full outcome
MessageTooLarge{required, disposition, progress} => retain full outcome
Stale{expected, observed} => retain full outcome
Unknown{lastKnownProgress} => retain full outcome
```

This post-query match is a proposed VProlog result presentation form, not an ordinary goal or
current compiler feature. Each arm retains the original full value; transport status never turns
into absence of a proof.

If await is Pending, the query runner retains the compiled query, VM cursor, bindings, and Vlog
trail and resumes that same activation by awaiting `Req` again. It does not rerun `io_submit`. A new
query or a top-level effect site reached again as a fresh query action can submit a new request. The
compiler rejects effect goals inside clauses or speculative choice with `PROLOG_IO_EFFECT_CONTEXT`;
this is a placement diagnostic, not a failed proof. Cut, logical failure, and query disposal release
local roots but do not undo already performed external work. The host retains leases until backend
release and captures a payload only through its explicit capture path.

## VLace: lazy description, explicit performance

The embedding fixture supplies `ReadEndpoint` and its host-owned `ReadCap` capability cell.
Constructing the planned `IO.ReadBytes` description is pure and inert. The runner performs it only
at the explicit async driver boundary:

```haskell
readAction = readBytes ReadEndpoint 0 4096
runIOAsync readAction
```

`readBytes` and `runIOAsync` are the plan's target spellings, not current VLace syntax or methods.
The example-level lowering binds this action to the shared protocol: when `runIOAsync` performs it,
the runner submits `read_bytes(0, 4096)` using the host's `ReadCap`. A pre-admission refusal
completes with the original `Refused{OutcomeCell}` sum. On admission the runner awaits that same
request. Pending returns `Parked{IOContinuation, RequestCell}`; resumption continues that
continuation and re-awaits the retained request, never re-performing the action. A new explicit
`runIOAsync` call is a new submission. The current synchronous `runIO` does not implement this path.

`Done(IOValue)` contains either the original `Refused{OutcomeCell}` sum or the unchanged terminal
`OutcomeCell`; it never collapses either to a string, exception, or empty result. The proposed
post-performance match is total for reads:

```text
Refused{fullRejectedOutcome} => retain the full refusal
Complete{payload, progress} => payload may be read; retain full outcome
EndOfStream{progress} => retain full outcome
Partial{payload, cause, progress} => payload may be read; retain full outcome
Failed{reason, optionalPayload, progress} => retain full; read payload only if present
Cancelled{optionalPayload, progress, uncertain} => retain full
TimedOut{optionalPayload, progress, uncertain} => retain full
Overflow{gap, progress} => retain full outcome
MessageTooLarge{required, disposition, progress} => retain full outcome
Stale{expected, observed} => retain full outcome
Unknown{lastKnownProgress} => retain full outcome
```

The match is example-level target notation, not current VLace grammar. Each branch preserves the
full tagged value and its operation-specific fields. Existing `pure`, `bind`, `then`, `putStr`, and
`getLine` expressions are not thereby asynchronous. Until the host trampoline is migrated, the plan
requires `VLACE_ASYNC_UNAVAILABLE`, not blocking `getline` as a fallback. If output is added, write,
flush, and graceful close are separate submitted operations: await the same flush request after
Pending; submit close only after successful flush; inspect close's own outcome. An explicitly chosen
abort is also separate and its outcome must be inspected. Dropping the lazy value or retiring a
waiter neither cancels nor closes the admitted request; its request and lease remain owned until
backend release. The host may separately request cancellation under policy.

## VIcon: distinguish yielded values from parked execution

The embedding host injects `ReadCap` as a managed capability cell binding; source cannot construct
authority. In current VIcon `write(value)` yields a value to the host; it does not write bytes. The
proposed source path separates submission from outcome inspection:

```text
Submission = submit(ReadCap, read_bytes(0, 4096))
Seen = match Submission {
  admitted(Req) => await(Req),
  refused(fullRejectedOutcome) => refused(fullRejectedOutcome)
}
Result = match Seen {
  refused(fullRejectedOutcome) => retain(fullRejectedOutcome),
  outcome(Complete{payload, progress}, full) => read_payload(payload); retain(full),
  outcome(EndOfStream{progress}, full) => retain(full),
  outcome(Partial{payload, cause, progress}, full) => read_payload(payload); retain(full),
  outcome(Failed{reason, optionalPayload, progress}, full) =>
    read_if_present(optionalPayload); retain(full),
  outcome(Cancelled{optionalPayload, progress, uncertain}, full) =>
    read_if_present(optionalPayload); retain(full),
  outcome(TimedOut{optionalPayload, progress, uncertain}, full) =>
    read_if_present(optionalPayload); retain(full),
  outcome(Overflow{gap, progress}, full) => retain(full),
  outcome(MessageTooLarge{required, disposition, progress}, full) => retain(full),
  outcome(Stale{expected, observed}, full) => retain(full),
  outcome(Unknown{lastKnownProgress}, full) => retain(full)
}
```

This syntax and `outcome(tag, full)` wrapper are proposed, not current VIcon grammar. The full
managed terminal cell remains available in every branch; payload reads are gated by presence and
status. At Pending, the runner returns `Parked` with the same generator cursor and frontend token.
Wakeup resumes that generator and re-awaits the same request. It does not repeat Submit. A
backtracked visit that reaches the Submit site again creates a fresh request; backtracking does not
undo an admitted or completed external effect. The proposed driver result distinguishes
`Yielded(Cell)`, `Parked(Token)`, `Failed(Error)`, and `Finished`; it does not overload current
`nullopt` (which means the generator ran dry).

Rendering an already available cell and `write(value)` do not submit I/O. If the runner chooses to
write yielded output to stdout, that sink uses its own write request and flush fence; Pending
re-awaits the same request, and a failed/partial/unknown flush cannot be treated as success or
silently followed by close. Teardown retires the waiter and follows host cancellation/release
policy.

## VAgent: committed effects and later owner events

This example uses the actor profile so the accepted post-commit path is explicit. The fixture
installs a native `Writer` skill whose provider is bound to a host-issued `WriterCap`; that binding
limits endpoint, rights, and byte bounds for every call. The source never constructs or widens the
capability. The declaration and `Bytes` type below are proposed target scaffolding, not current
VAgent source or a worker ABI. As in the current effect contract, `write` returns `Unit`; the source
statement stages the intent and does not bind an I/O receipt. Plan §10's
`let result := Writer.write(bytes)` shorthand binds only that language-level `Unit`. The name
`result` does not denote the later `OutcomeCell`; the correlated owner event below is the outcome
channel.

```text
capacity Writer {
  effect def write(bytes: Bytes): Unit within 1s
}

native skill WriterHost implements Writer
goal SendReport(bytes: Bytes) perform
agent Reporter {
  uses Writer install WriterHost()
  plan send_report for SendReport(bytes) {
    write(bytes)
  }
}
```

The actor's `ActorTurn` deliberation stages a typed, immutable effect intent; it never directly
submits or awaits I/O. A failed or rewound turn dispatches nothing. After commit, the adapter writes
and flushes the bounded durable attempt record off-thread, waits for the owner's acknowledgement,
and only then submits under the `DurableEffect` profile. Journal exhaustion or missing
acknowledgement refuses before external work. The native provider freezes the bounded bytes and uses
`WriterCap`; mutable `WorkerContext` never goes to an I/O worker.

The later correlated owner event is the only outcome consumer in this example. Proposed host-side
adapter glue maps every admitted request to `EffectDone{intent, outcome: full OutcomeCell}` when it
reaches a terminal state; `Done` means finished, not successful. The event carries the original full
value: `Complete{progress}`, `Partial{cause, progress}`,
`Failed{reason, optionalPayload, progress}`, `Cancelled{optionalPayload, progress, uncertain}`,
`TimedOut{optionalPayload, progress, uncertain}`, `Conflict{expected, observed, progress}`, or
`Unknown{lastKnownProgress}`. These are the legal write outcomes; write results have no payload. A
pre-admission capability/profile/journal refusal is preserved separately as
`EffectFailed{intent, refusal: full Rejected OutcomeCell}`. This event field mapping is proposed
adapter behavior, not the current VAgent contract: current `EffectFailed` flows into the parked
statement's `else` ladder with a textual cause, so it cannot yet deliver the full refusal cell.
Current VAgent has no source-level outcome accessor; a host-side event consumer must retain/inspect
the full cell until a future VAgent bridge explicitly exposes it. The source statement's `Unit`
result is not that cell.

An actor invocation is refused with `VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN` before service admission;
the actor path above is the staged durable intent exception, not an await inside the atomic turn.
Unknown after dispatch is never retried automatically. An explicit later effect is a new intent and
requires policy authorization. Write, flush, and graceful close each have separate identities and
outcomes, and dependent close follows only a successful flush. Abandonment does not undo sent bytes;
host teardown retires waiters and retains active requests/leases until backend release.

## Review boundary

These examples clarify how the frozen target contract could appear at each frontend. They do not
resolve missing source-level capability constructors, operation constructors, outcome accessors, or
source syntax for capture; the host fixture deliberately supplies capabilities as inputs. They also
do not make today's synchronous compiler/runner APIs resumable. VLace's action-to-common
Submit/Await mapping and VAgent's event field mapping above are companion-local proposed lowering
contracts, since §10 names their target carriers without defining those field-level mappings. Source
patterns and event presentation notation are illustrative grammar additions, not current syntax.
These are implementation tasks for the M7 bridge and its dependent host/VM milestones, not
assumptions hidden in these examples.
