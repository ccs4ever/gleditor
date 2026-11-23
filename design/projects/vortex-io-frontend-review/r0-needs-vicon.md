# VICON stakeholder needs (pre-review)

Target: `design/vortex-io-redesign.md` v0.9, Draft. This is a source-grounded needs note for the
editor, not a formal R1 critique. The usage sketches are proposed pseudocode, not current VICON
grammar or API. There are no production users or ABI compatibility requirements; the in-memory VICON
driver/compiler boundary may change freely. Keep generic request/cell behavior in Vortex and VICON
syntax, effect policy, and continuation ownership in the VICON compiler/driver.

## Current VICON behavior

VICON is a goal-directed generator language. An expression can produce zero or more cells; `#Choice`
records an arena mark and alternative, later `#Fail` releases that mark and resumes at the
alternative. Bounded expressions commit their choice; `every` drives an unbounded expression until
it is exhausted (`design/vicon-goal-directed-language.md:194-263`). This means a side-effecting
expression inside an `every` body can execute once per generated value, and resuming a generator is
ordinary backtracking, not a generic host retry.

The compiler emits an opcode graph. `write(e)` is currently the only host-visible VICON operation:
at the top-level produce boundary it yields the value; in a nested produce context it tees the value
through the shared `haltOp_`, then continues when resumed
(`apps/common/xanadu/vicon/compiler.cpp:793-822`). Its existing meaning is host rendering to stdout,
not external stream or file writing (`design/vicon-goal-directed-language.md:556-576`). The old
VICON lens bridge likewise describes a compile-to-opcode-graph design with a single
produce-and-suspend host channel (`design/vicon-lens-bridge.md:11-20`); its lens surface is proposed
design material, not an implementation precedent for asynchronous I/O.

The Driver starts the first activation with `goalProduce`; subsequent `resume()` calls invoke
`goalResume` at a shared fail opcode and return the program result cell. Both are currently
expressed as `optional<CellRef>`: `nullopt` represents no result/exhaustion, and `every()` loops
until that happens (`apps/common/xanadu/vicon/driver.cpp:36-63`). The stdlib accepts a run as a
produce only when the cursor stopped at the expected `haltOp`;
`apps/common/xanadu/vortex/vortex_stdlib.cpp:2523-2572` confirms that a different halt does not
currently mean “parked, await host, and continue.” The redesign therefore needs an actual Pending
execution outcome and a way for the host to resume the same cursor/activation; treating Pending as
VICON failure would unwind the very choice state needed to continue.

The current CLI is synchronous: file/stdin source is read before compile, `runAll()` collects all
rendered answers, and REPL results are printed through `every()`
(`apps/vicon/main.cpp:24-34,121-159`; `apps/common/xanadu/vicon/driver.cpp:60-72`). Async runtime
I/O should not be smuggled into this collection/render path. The program protocol should expose cell
references for capability, request, outcome, and payload; rendering stays a host-boundary
observation. The current `write` result tee does not submit or await an I/O request.

## Proposed usage examples

Both examples state placement and multiplicity only. Names such as `io.submit`, `io.await`,
`io.readRange`, `io.payload`, and `io.status` are proposed; they are not current builtins or frozen
syntax. They follow the draft's generic submit/await vocabulary and VICON sketch
(`design/vortex-io-redesign.md:448-477,495-507`).

### 1. Bounded media/file reads, one fresh request per explicit generator visit

```vicon
# cap is a host-supplied capability cell for this source generation and range budget.
every offset := 0 to 2 do {
  req := io.submit(cap, io.readRange(offset * 4096, 4096))
  seen := io.await(req)
  if io.status(seen) = "complete" then
    write(io.payload(seen))
  else
    write(io.status(seen))
}
```

Each actual evaluation of `io.submit` is a fresh request. `io.await(req)` may suspend, but its
resumption observes this same request and continues after the await; it must not jump back through
submit. When `every` later backtracks and reaches submit again for another offset, that is a new
effect and needs a new request identity. `write` here is only illustrative output; a payload cell
should use bounded typed range/slice access, not implicit whole-file materialization or conversion
to text. A mutable path must carry its source generation/read-observation and consistency guarantee.
The payload remains transient unless an explicit capture is evaluated.

### 2. Repeated external writes make Icon re-drive visible

```vicon
# Every value intentionally sends a separate chunk. This is not a retry loop hidden in await.
every n := 1 to 3 do {
  req := io.submit(sendCap, io.writeChunk(chunkFor(n)))
  outcome := io.await(req)
  write(io.status(outcome))
}
```

This sends three distinct chunks/request identities if all three generator visits occur. If a branch
or generator is re-driven through this submit site, it submits a fresh effect again; the previous
send is not undone by `#Fail`. A pending await resumes on the same request. Partial progress,
cancellation, rejection, transport error, and `Unknown` must remain typed outcome information rather
than being collapsed to ordinary VICON failure or EOF. A retry after an uncertain write is another
explicit submit with a fresh effect identity, and must remain a program decision.

## Generic protocol trace as VICON should observe it

1. The host supplies endpoint/capability cells. VICON constructs request-spec cells and performs
   lens navigation, local arithmetic, predicates, and rendering without external work.
1. At an explicit `io.submit` action, the owner validates the capability/generation and reserves
   bounded request/result/payload capacity. Success yields a request cell with a stable identity;
   refusal is a typed result and starts no work. A repeated execution of this submit site admits a
   new request, even after backtracking.
1. Service work runs off evaluation/UI threads. Results are published as outcome and payload cells;
   a bounded range read can expose partial bytes and terminal status together. File/media adapters
   report the consistency they can actually prove. Lens operations use the same request protocol for
   open/page/refresh/commit/content resolution; traversal or `render` must not submit implicitly.
1. At `io.await(req)`, a ready request exposes its retained outcome. If it is Pending, the host
   parks the VICON activation with its cursor, choice stack, active await continuation, and needed
   cell leases intact. Pumping completion resumes that exact await against the same request.
   Backtracking may later return to a submit site; that distinct control-flow visit is fresh work.
1. Await cancellation releases/stops that waiter; explicit request cancellation asks the service to
   stop and reports actual progress/uncertainty. Abandoning a VICON activation must unregister its
   waiter and release frontend-owned continuation/cell leases, but must not silently imply that
   admitted external work was cancelled or undone. Request/result retention and any explicit
   cancellation policy belong to the shared service contract.
1. Capture is a separate explicit effect that pins selected payload ranges and publishes a durable
   mapping only after successful append. Neither lens traversal, await, payload inspection, nor
   VICON `write` captures bytes.

## VICON-specific requirements and questions

- Add a distinct Driver/VM result for produced value, generator exhaustion, parked Pending, and
  execution failure. Current `optional<CellRef>` cannot encode all four. The host must be able to
  pump and resume an activation without recursively running the VM or converting Pending to `#Fail`.
- Keep the suspended cursor, choice frames/arena marks, bindings, guards, and exact await
  continuation in VICON-owned state. Generic Pending should report parking/request readiness, not
  carry Icon-specific choice or trail state. This is parallel to VProlog retaining its resolver
  continuation locally, despite their different control machines.
- Make the source-level effect boundary explicit. Existing VICON has no effect/type checker, so the
  bridge/compiler must specify what expression positions may submit, how `every`, alternation,
  loops, procedure calls, and backtracking affect multiplicity, and where diagnostics are possible.
  Do not ban goal-directed control wholesale: intentional per-result submission must be expressible
  and its repeated effects obvious.
- Keep request identity usable while a choice mark rewinds ordinary cell bindings. Backtracking
  cannot revoke external work; it must not invalidate the service's ability to finish/account for
  the request or turn an uncertain write into implicit retry. Define the retained request/payload
  lease and explicit release behavior independently of an arena-local temporary result binding.
- Distinguish an ordinary generator failure/exhaustion from a typed I/O `Eof`, `Partial`,
  `Cancelled`, `Rejected`, `Failed`, `Overflow/Gap`, or `Unknown` outcome. VICON may explicitly
  branch/fail after inspecting a status; the adapter must not map these statuses to no result.
- Lens open, pages, refresh, commit, and media/file reads must use async request/await. Pure
  traversal and `io.render` observe already-materialized cells. Large media should use bounded
  ranges and explicit seek requests; never promise snapshot coherence for mutable paths unless the
  adapter can prove it. Capture is explicit.
- Define cleanup for normal exhaustion, explicit stop/cancel, a new REPL `start`, compile failure,
  and Driver destruction. Release VICON cursor/choice/await state and unregister a parked waiter; do
  not silently cancel the underlying effect merely because an Icon branch/activation is dropped.
  Respect bounded service retention so orphaned requests cannot retain memory without limit.
- No ABI shim is needed: the `Driver::resume()` return type and compiler/runtime seams may be
  redesigned. Keep any generic VM/service mechanism language-neutral and keep VICON syntax and
  continuation policy in its compiler/driver.

## Dialogue with VProlog frontend reviewer

VProlog asked whether the shared service should own request identity/outcome/payload only while the
frontend retains its own resolver continuation, trail, choices, and request bindings. I agree: the
VICON Driver must retain its cursor and choice stack and resume at the await continuation; generic
Pending must not encode either language's control state. I added the cross-cutting consequence that
abandonment unregisters the waiter and frees frontend-owned leases, but does not silently cancel
service-owned work. VProlog agreed that request-cell references may be released on activation
abandonment while bounded service-owned work and terminal retention remain governed by the common
policy; explicit request cancellation is a separate policy/action. This confirms, rather than
changes, my requirement for explicit waiter-versus-request cleanup semantics.

## Source anchors

- `design/vicon-goal-directed-language.md:194-263,556-576` — generator, choice/re-drive, reversible
  state, bounded-vs-unbounded evaluation, and current write/render example.
- `apps/common/xanadu/vicon/compiler.cpp:793-822` — the `write` tee and its yield/resume path.
- `apps/common/xanadu/vicon/driver.cpp:36-72` — current produce/resume/exhaustion API and eager
  `every`/`runAll` behavior.
- `apps/common/xanadu/vortex/vortex_stdlib.cpp:2523-2572` — `goalProduce`/`goalResume` require the
  designated yield halt and otherwise unwind.
- `design/vicon-lens-bridge.md:11-20,46-62,418-465` — compiled host boundary, current-surface
  inventory, and explicitly proposed lens surface.
- `design/vortex-io-redesign.md:52-59,205-250,448-493,495-516,519-535` — settled shared
  effect/outcome rules, illustrative syntax, and Pending/resumption requirements.
- `design/projects/vortex-io-frontend-review/r0-needs-vql.md` and `r0-needs-vpl.md` — sibling
  multiplicity and request-correlation questions. Their shared request protocol is consistent with
  VICON retaining its own goal-directed continuation.

No source or runtime changes are requested by this note.

## Addendum: buffering, framing, and streams

The owner requires the shared protocol to expose unbuffered, line, fixed-buffer, and tokenized
modes, with no synchronous or unlimited-buffer assumption. These modes describe service-side
framing/retention; VICON owns language-level token grammar and any generator cursor over the
materialized framed data. The current VICON REPL reads a complete line synchronously with
`std::getline` (`apps/vicon/main.cpp:53-56`); command input is not yet a Vortex service stream. The
draft already requires bounded retention/backpressure and distinguishes stream EOF from zero-length
datagrams (`design/vortex-io-redesign.md:55,488-493,513-516`), but its VICON row has no concrete
framing or flush example.

### 3. Proposed prompt, line input, and tokenized consumption

```vicon
# Proposed host stream cells, framing records, and calls; not current VICON syntax.
writeReq := io.submit(stdoutCap, io.writeBytes("name? "))
flushReq := io.submit(stdoutCap, io.flush())
flushSeen := io.await(flushReq)       # same ordered endpoint: prompt visible before input wait
readReq := io.submit(stdinCap, io.readFrame(mode = Line, maxBytes = 4096))
lineSeen := io.await(readReq)
```

The host must flush prompt output before parking for input. Submission/admission is not equivalent
to external transfer completion; flush and close are explicit operations with their own typed
outcomes, and cancellation can report bytes already transferred. Endpoint ordering must ensure a
flush admitted after a prompt write fences that earlier write, while each request retains its own
identity/progress. A line read needs a declared bound and explicit overlong-line behavior (for
example, partial frame plus `Overflow`/continuation, or typed refusal); it cannot silently allocate
until newline. `Eof` is distinct from an empty line, and binary zero is ordinary payload, not EOF.

If the VICON program tokenizes the returned frame, tokenization can be a local pure generator over
retained bytes:

```vicon
# `tokens` walks an already-materialized line/frame and retains its own cursor state.
every token := tokens(lineSeen.payload) do {
  write(token)
}
```

This is proposed pseudocode. If a frame contains `alpha beta` and the first visit yields `alpha`, a
Pending suspension or later backtracking must not throw away `beta` or fetch another frame. The same
request observation can be revisited; a new `submit(Read...)` consumes subsequent source data. If a
choice is abandoned/cut, the language-level tokenizer cursor may be released, but the service-owned
read observation and request accounting follow bounded retention/lease policy; no backtrack claims
the external stream was rewound. VICON can expose its token grammar while the shared stream policy
owns chunking/framing modes, bounds, overflow signaling, and endpoint ordering.

For fixed-buffer and unbuffered reads, VICON should see requested/actual lengths and partial
progress. Fixed buffers need defined short-read and `EOF+bytes` outcomes; unbuffered mode must still
reserve completion/control capacity. Tokenized mode needs a maximum token/frame size, a specified
overlong policy, and leftover handling. The protocol must distinguish `Eof`, an empty line/frame, a
zero-byte successful read when that is meaningful, binary `0x00`, and partial bytes plus terminal
status. These are transport outcomes, not ordinary generator failure.

### Dialogue update: VProlog continuation and stream state

VProlog's reviewer agrees that generic Pending carries no language control stack. For VICON, this
means the Driver preserves its cursor/choice marks, current request, and token/frame cursor across
Pending. A token remainder from an already-materialized frame is frontend-local and backtracks with
VICON's choice state; the admitted read and its observation are not rolled back or resubmitted. The
VProlog reviewer also agrees activation abandonment releases frontend-owned request references and
unregisters its waiter, while service work and bounded terminal retention remain governed by shared
policy; explicit request cancellation is separate. Applying this to streams means cut or abandonment
can stop the consumer/waiter but cannot silently discard evidence of transferred bytes or imply that
input was unread. This adds stream-specific state to the frontend continuation, but does not change
the shared Pending-versus-control-state boundary.

One host-side detail is already present in the CLI: the REPL prompt uses `std::flush` before
`std::getline` (`apps/vicon/main.cpp:45-56`). An asynchronous stream migration should preserve this
ordering explicitly as a write/flush-before-read sequence, not assume that the old synchronous
prompt behavior follows automatically.

### Shared stream position clarified with VProlog

VProlog further clarified the read-ahead rule: if one request delivers a line/frame but the backend
prefetched bytes beyond its boundary, those unread bytes remain in the endpoint buffer lease. The
next read request starts after the delivered frame, not after the hidden prefetch. A branch can
re-await the still-live request and inspect the same delivered bytes; it cannot promise transparent
rewind of the external stream. VICON's token generator may rewind its own cursor over the delivered
frame under choice backtracking, but shared endpoint framing must preserve any unread suffix and
bound its retention. This confirms the division: generic transport owns byte framing/carry and
bounded buffers; frontend grammar owns pure token interpretation.
