# R1 — API designer review

Target: `design/vortex-io-implementation-plan.md`  
Target SHA-256: `f619801374ad6b81638e719014243cbccc354fdfee5ca9c1579b43b93bc80093`  
Review scope: public contracts across layers, type/signature completeness, ownership and state consistency.  
Verdict: **RETURN FOR REVISION**

## Findings

### MAJOR — Native request identity crosses a boundary declared private

**Evidence:** §4 (lines 186–192) says native `RequestId` is private and the managed request cell is the program handle. §5 then declares `PendingAwait { RequestId request; ... }` (line 214); §6 puts `RequestId` in `IoPlan` and `IoCompletion` (lines 256–257). The plan does not state that these types are service-private, while the VM/host completion handoff explicitly uses them.

**Why it matters:** There are now two incompatible request identity surfaces: cell/incarnation for service and language callers, versus raw `RequestId` for pending execution and adapter completion. This affects stale-completion validation and prevents the cell graph from being the sole source of logical request identity as claimed.

**Required fix:** Choose and state the boundary. Keep numeric/native IDs in private service/backend messages only; carry an owner-validated `RequestCell` (cell plus incarnation) or an explicitly host-private opaque token through VM pending state. Define precisely which functions may observe the private token and how it is resolved to the managed request cell.

### MAJOR — Open cannot satisfy the declared asynchronous contract

**Evidence:** §3.1 includes `Open` in the operation vocabulary (line 90). §4 says opens must not block the evaluation/UI thread (lines 164–168), but `IoService::open(const OpenSpec&) -> OpenResult` is the only open API (line 174), and `OpenResult` is never defined. The signature does not show admission, request identity, Pending, or later endpoint publication. The plan elsewhere requires cell-based requests and async completion.

**Why it matters:** A first endpoint cannot be created through the otherwise specified capability-based `submit` path, and no defined result lets a caller suspend while DNS/open/handshake work completes. A synchronous return also leaves it unclear whether `open` is a prohibited blocking exception.

**Required fix:** Define a bootstrap open contract: validation/refusal, admitted open request, Pending/await behavior, terminal open outcome and owner-side endpoint/capability publication. State that open work follows the same worker, budget, cancellation, generation and continuation rules as other operations.

### MAJOR — Cancellation has incompatible return and completion semantics

**Evidence:** §4 declares `cancelRequest(RequestCell) -> CancelResult` and says it is idempotent/best effort (lines 178–180), while §10's shared frontend primitives declare `io.cancel(req) -> Outcome` (see §10, around lines 440–445). §10.1 also describes cancellation as an outcome-bearing asynchronous operation. The API does not define `CancelResult` or explain whether cancellation returns an immediate request-accepted status or the eventual terminal outcome.

**Why it matters:** Cancellation may race with transfer and can remain uncertain. An immediate `Outcome` cannot truthfully represent eventual progress, while treating the call as terminal loses the later receipt. Different language bridges could implement materially different semantics.

**Required fix:** Separate `CancelRequestResult` (e.g. accepted/already-terminal/refused, with the original request handle) from observation of the original request's eventual `Cancelled`/other terminal outcome. Define the behavior for cancel-before-start, cancel-after-completion, repeated cancel and late completion. Make all frontend signatures match this contract; keep waiter cancellation a distinct operation.

### MAJOR — Capture’s asynchronous lifecycle has no usable await/result protocol

**Evidence:** §3.1 inventories `CaptureCoordinator::prepare/append/commit/recover`; §8.2 declares `append(CaptureId)` asynchronous but returns the underspecified synchronous `CaptureResult` (lines 355–362). The operation vocabulary also lists `Capture` (line 91), and §10's shared frontend surface says `io.capture(payloadRange) -> CaptureResult` (near line 440). No type defines Prepared/Pending/Committed/Refused states, the append request handle, terminal result, or how async durable acknowledgement reaches `commit`.

**Why it matters:** The capture path must cross an async fsync/permascroll boundary and preserve staged selection and leases. As written, a caller cannot know whether `append` is still pending, may call `commit` too early, or how to recover the same capture without duplicate append. This is a public API gap, not merely the journal implementation choice.

**Required fix:** Define capture state and typed operations: prepare returns a capture handle or refusal; append returns an admitted request/Pending handle; commit accepts only a terminal successful append and returns a durable receipt or typed failure; recover has explicit reconciliation outcomes. Specify whether capture is a service op or a coordinator layered over IoService and remove the duplicate/ambiguous entry point.

### MAJOR — Outcome schema does not fully define operation and malformed-input results

**Evidence:** §3.2 defines `Complete` with an optional payload for every operation and places `Rejected` inside `IoOutcome` while commenting it is pre-admission only (lines 123–139). §4 separately makes rejection a `SubmitResult::Refused`. The frontend contract uses `Result<Request, AdmissionError>` and `Pending | Outcome` (§10), so there are three overlapping representations. Framing requires malformed/truncated handling (§7.1, lines 299–302), but the closed variant set has no decode/malformed tags and `IoErrorInfo`/`IoTerminal` are not defined. `Complete{payload:nullopt}` also does not explain how a successful write/flush differs from an absent read payload.

**Why it matters:** Adapters and languages cannot reliably map all backend outcomes or tell whether a result is valid for the requested operation. A generic error payload does not freeze the promised typed malformed-input contract, and placing admission refusal in terminal outcomes invites callers to mistake rejection for an admitted request completion.

**Required fix:** Make admission refusal a distinct pre-admission result only. Define an exhaustive typed error/cause schema including unsupported, malformed/truncated decode, cursor conflict, timeout, profile refusal, and persistence refusal; specify payload/progress presence by operation and distinguish successful write/flush/close results from reads. Align service, VM, and frontend result aliases to that one schema.

### MINOR — Buffer policy signature and framing contract are not one frozen type

**Evidence:** §3.1 defines `BufferPolicy` variants with `Frame{codec,...}`, cursor policy and oversize disposition (lines 84–88). §7.1 instead describes delimiter/fixed-length/message framing plus encoding and malformed/truncated policy (lines 299–302). No signature connects `BufferPolicy` to `ReadFrame`, and “Refuse” is listed as an oversize policy without stating whether that refusal is pre-admission or a terminal result after consuming bytes.

**Why it matters:** Adapters can interpret identical frame requests differently, particularly around delimiter/encoding split and whether an oversized frame was consumed.

**Required fix:** Define the exact frame-mode fields and lifecycle: supported delimiter/length/message framing, encoding/decode policy, maximum/read-ahead, cursor ownership, and pre-admission refusal versus post-read `MessageTooLarge`/overflow including consumption disposition.

### MINOR — Cell wrapper and allocation-incarnation ownership are only partially declared

**Evidence:** §3.2 declares owner-only `RequestCell` and `CapabilityCell`, while later APIs use `EndpointCell`, `WaiterCell`, `OutcomeCell`, `ContinuationId`, `CaptureId`, and `IoHost`/`IoExecutionProfile` without defining their cell/native split or constructors. The schema table names tags and dimensions but does not specify the payload field/link representation for progress/outcome values.

**Why it matters:** The incarnation check is central to preventing stale references after arena slot reuse. Leaving some wrappers undefined makes it unclear which IDs are forgeable cell data and which are owner-validated native handles.

**Required fix:** Add a compact type inventory defining all cell wrappers as `{CellRef, AllocationIncarnation}` owner-only values or clearly private IDs, plus the mint/validate boundary and the authoritative cell fields for state/outcome/progress.

## Tally

- Blocking: 0
- Major: 5
- Minor: 2
- Nit: 0

## Verdict rationale

The plan substantially resolves the redesign's earlier open questions and contains useful concrete types, schemas, and frontend surfaces. The remaining major findings are internal contract contradictions or missing end-to-end signatures for open, cancellation, capture, and outcomes. Those affect every adapter/VM/frontend implementation and cannot be left for milestone spikes to decide. Resolve them in the plan before implementation starts.
