# VQL stakeholder needs for Vortex I/O

This is a frontend-needs note for the draft editor, not a verdict on the frozen design. It records
current VQL behavior separately from proposed syntax. No production ABI compatibility is required:
VQL syntax, compiler lowering, and adapter surfaces may change freely. New Vortex core/VM behavior
must remain frontend-neutral; VQL owns syntax, static effect placement, multiplicity checks, and
lowering to the shared cell/capability protocol.

## Current VQL facts

- VQL is relational over cells, but its present implementation is not uniformly pure: bindings and
  paths can execute effects. `evaluateBindings` evaluates a `for` input path once, then evaluates
  remaining bindings/where/action per row; `executeEffectClause` explicitly iterates generators and
  executes a nested effect per result (`apps/common/xanadu/vql/vql_engine.cpp:1691-1805`). A side
  effect hidden in a generator, predicate, or row expression therefore has data-dependent
  multiplicity and may be skipped by short-circuiting. The existing lens bridge identifies this
  hazard and requires mutations only at exactly-once statement positions
  (`design/vql-lens-bridge.md:101-157`).
- Boolean evaluation short-circuits AND/OR (`vql_engine.cpp:1650-1684`); submissions cannot be value
  expressions, predicates, filters, joins, or render operations. Require a compiler diagnostic and
  keep a runtime guard as backstop.
- `QueryExecutor::execute` currently returns `expected<vector<CellRef>, QueryExecutionError>`,
  compiles to Vortex, and has a cycle budget (`apps/common/xanadu/vql/execution.hpp:24-48`). That
  synchronous, all-or-nothing return is not a pending-result contract. Compiled VQL already runs on
  the VM (`execution.cpp:128-156`).
- Lens open/gesture support is a design bridge, not a complete async API. It proposes generator
  anchors for open, step functions for gestures, and session state across REPL queries
  (`design/vql-lens-bridge.md:48-99`). Preserve useful concepts—source generators, row binding,
  explicit effects, cell results—but no ABI or blocking open/commit assumption.

## VQL requirements for the shared protocol

1. **Multiplicity is explicit and statically checked.** Source enumeration is a pure read over an
   available view. `submit` is an explicit action, never a generator/step automatically repeated per
   row. Intentional N-row work must make N submissions visible and define deterministic admission
   order, bounded concurrency, per-row outcomes, and cancellation after partial admission.
1. **Separate reads from effects.** Open, page fetch, refresh, file/media range read, and
   database/network/process operation each produce a named retained request. `await` observes that
   request only; retry or re-evaluated submit mints a fresh request. Lens edit/commit preserve
   transaction validation and typed refusal; commits are not hidden in row traversal or rendering.
1. **Ordinary cells carry all public values.** Requests, endpoint/capability occurrences, progress,
   typed outcomes, payload ranges, and result rows are addressable cells in the query's composite
   arena. Multi-store results use the coordinator's composed arena. Native buffers, C++ result
   structs, and serialized JSON stay private.
1. **Lifecycle remains navigable.** VQL needs open, page, dirty-state inspection, edit,
   commit/revert, and release. Dynamic sources need explicit refresh/page requests and typed
   gaps/overflow. All open/page/commit work is asynchronous and follows request identity/lifetime
   rules.
1. **Binary/media ranges work without whole-file reads.** A result can hold resolver-aware bytes and
   request a bounded range without text decoding. Slices preserve source identity/generation and a
   lease. A path/URI is only a locator; mutable reads need version observation. Explicit capture
   alone makes transient bytes permanent.
1. **Terminal status is inspectable.** EOF is a typed normal terminal result, distinct from empty
   payload, no row, pending, or logical failure. Rejection, backend error, cancellation,
   overflow/gap, partial payload/progress, and unknown external outcome remain in typed outcome
   cells. Query/compiler errors remain separate.
1. **Retry identity is stable and explicit.** Request references survive VQL backtracking and
   session resumption. Re-await cannot consume another source item or resend bytes. Reaching submit
   again creates a new request, even after backtracking. Retrying an unknown effect is explicit and
   exposes duplicate risk. Pure queries are deterministic for fixed input snapshots and request
   outcomes.
1. **Bound and release resources.** Abandoning a query releases solver refs and unpublished payload
   leases. Admitted requests remain service-owned until terminal/cancelled and observable for the
   retention period. Wait cancellation differs from request cancellation; request cancellation
   reports actual progress and uncertainty. Backpressure is visible; no hidden spill is assumed.

## Proposed VQL usage sketches (not existing syntax/API)

The names below state frontend intent only. They are not existing syntax, opcode ordinals, or a
proposed C++ ABI. The VQL compiler/adapter lowers them to the shared protocol.

```vql
-- Proposed: pure source enumeration, one explicit open and page request.
let $src := source("file:/var/log/app.log");
let $open := submit $src.open();
let $opened := await $open;
let $page := submit $opened.page(after: $cursor, limit: 256);
let $page_result := await $page;
return $page_result.rows;
```

`source`, `submit`, and `await` are proposed. Open and page are distinct submissions; the runner can
suspend without blocking, and re-awaiting does not repeat the page read. `limit` is a request bound,
not a fixed global ceiling.

```vql
-- Proposed: per-row submissions are explicit and produce typed outcomes.
for $file in $files
submit $file.read(range: [0, 4096]) as $request
await $request as $outcome
return { $file, $outcome };
```

This differs from reads hidden in a generator. Static checking must define source-order admission,
serial await versus an explicit bounded batch, and cancellation after some admissions succeed. Rows
retain partial outcomes, including earlier successful reads.

## Generic operation trace and resource semantics

1. Owner evaluates submit, validates capabilities, snapshots an immutable native plan, reserves
   request/result/payload budget, and admits a fresh request cell or returns typed refusal. Workers
   receive no arena references.
1. Worker performs external work and posts progress/completion with service request identity,
   endpoint generation, byte ranges, and typed status—never `CellRef` or VQL continuation state.
1. Owner turns completion into ordinary cells. Await either returns Pending/suspends or observes the
   retained result; it never replays submission.
1. Backtracking rewinds bindings and speculative result construction, not an external request or
   receipt. Reaching submit again creates a new identity. Pure generators/predicates can be
   reevaluated without I/O.
1. Cancelling await stops that wait only. Cancelling a request asks the service to stop; its outcome
   reports progress and partial/unknown status. Reclaim request/payload leases only after terminal
   retention, VQL references, and consumer leases permit it. Capture is the explicit persistence
   boundary.

## Questions for the VQL adapter and draft editor

- What statement/action syntax marks submit as exactly-once-per-evaluation while making intentional
  N-row submission and bounded concurrency straightforward?
- How can a relation retain successful partial rows alongside a later transport failure without
  disguising the failure as a missing row?
- How does `vquery` pump Pending completions and preserve request references across its fresh
  `VQLEngine` per query, while keeping query/session cleanup explicit?
- Which dynamic-source event and file/media range forms belong in generic endpoint/request cells
  versus VQL's source/lens vocabulary?
- For N requests admitted in array order but completed out of order, how does the shared result
  preserve per-element correlation and input shape? VPL raised this cross-frontend question; VQL
  needs the same guarantee for batched row operations.
