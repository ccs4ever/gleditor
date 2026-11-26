# Vortex I/O v0.11 implementation plan

| Field           | Value                                                                                                                               |
| --------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Source contract | [`vortex-io-redesign.md`](vortex-io-redesign.md), v0.11, SHA-256 `7a4a0d94ea6e0c36f45bad608f0df1c943d8e63c6304d3fb984620ed660e0e07` |
| Status          | Implementation-ready design; planned API, not production ABI                                                                        |
| Scope           | Shared asynchronous I/O protocol, transient payload ownership, capture, adapters, and six frontend bridges                          |
| Excludes        | Runtime implementation, general legacy VM cleanup, Micropayment, spill storage, codec overhaul                                      |

This plan resolves v0.11's open API and sequencing choices so implementation can proceed without
reopening architecture. All signatures below are proposed C++23 design signatures; names may be
adjusted in code review, but the ownership, refusal, and ordering semantics are normative. There is
no production ABI to preserve. Existing store format/version rules, `CompactOpNode`'s 64-byte
size/alignment, and behavior outside I/O remain in force. Nothing here permits worker access to an
arena, frontend policy in VortexCore/VortexVM, or automatic capture/retry.

For complete proposed source-surface usage sketches across the six frontends, see
[`vortex-io-frontend-examples.md`](vortex-io-frontend-examples.md). Those examples clarify the
frontend contracts below; they are not current compiler syntax or an implemented API.

## 1. Decisions and invariants

1. **One owner, many workers.** The evaluation host owns `ArenaManifold`, VortexCore, VortexVM,
   language activations, cell minting, and publication. `IoService` owns native endpoints, requests,
   buffers, queues, and backend state. Workers receive immutable `BackendCommand`, `PayloadLease`,
   cancellation state and service-private request identity only. They never receive `CellRef`, arena
   references, VM callbacks, or string views into arena storage. Completion messages contain no
   solver references.
1. **Pending is a host result.** `Await` produces `Ready` or `Pending(continuationId, waiterCell)`;
   it is never logical success/failure and does not consume VM cycle budget while parked. A host
   retains the same activation and request and resumes that await after notification. Resume never
   re-runs Submit. Re-executing Submit, including after backtracking, admits a fresh operation.
1. **Cell values are the program boundary.** Endpoints, capability occurrences, requests, payload
   ranges, progress and typed outcomes are inspectable cells. Native descriptors and pointers remain
   private. Payload bytes resolve and slice through one resolver-aware value path; there is no
   side-door byte-stream API exposed to programs.
1. **Typed terminal states.** Empty payload, EOF, Pending, logical failure, cancellation, overflow,
   unsupported operation, stale generation, partial progress and unknown external effect are
   distinct. Every adapter must return a typed refusal before admission when it cannot meet a
   requested guarantee. Outcomes never become implicit Prolog failure or VQL no-row.
1. **Bounded ownership.** There is no spill backend in the first implementation. Admission reserves
   request metadata, terminal/control capacity, and declared payload bounds before work starts.
   Pausable sources pause; unpausable sources report bounded coalesced gaps/overflow. Unresponsive
   native work remains charged and blocks replacement admission rather than growing without bound.
1. **External effects are not reversible.** Cancel is a request, not proof of stopping. Outputs
   report accepted/transferred counts and uncertainty. Actor/VAgent and Evidence policy remains
   above the service. No automatic resend follows restart or replay.
1. **Source identity is not a locator or hash.** Transient ranges identify a source generation and
   occurrence/range. Equal bytes do not merge identity. File paths/inodes are locators; each read is
   an immutable retained observation with explicit consistency evidence. Permanent identity is
   introduced only by explicit capture into the author's permascroll.

## 2. Settled API and ownership inventory

| Layer              | Final planned API / symbols                                                                                                | Authority and lifetime                                                                            | Acceptance gate                                                                          |
| ------------------ | -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| Payload resolution | `PayloadRef`, `PayloadRange`, `PayloadResolver::read/slice/retain`, `PayloadLease`; `ArenaManifold::incarnationOf/matches` | Cell-visible range; leases pin immutable chunks; native resolver owns storage                     | M0 resolver and stale-range tests                                                        |
| Cell schema        | `d.io.*` dimensions; `IoCellTag`; `IoOutcomeTag`                                                                           | Topology and tags in the graph; request, endpoint, capability and capture state native (§3.1)     | M0 topology/persistence-refusal tests                                                    |
| Service            | `IoService::admitOpen/submit/poll/await/cancelWaiter/cancelRequest/close/pump/shutdown`                                    | Host owns service; request record survives arena marks while semantically owned or backend-active | M1 delayed backend/lifecycle tests                                                       |
| VM boundary        | `ExecutionResult`, `PendingAwait`, `VortexVM::step(cursor)/run(cursor)/resumeAwait(cursor, continuation)`                  | VM owns continuation; host owns park/wakeup registration; no front-end branches                   | M2 mark/backtrack/resume tests                                                           |
| Profile/host       | `IoExecutionProfile`, `IoHost::pump/submit/await`                                                                          | Immutable profile per run; actor restrictions checked before admission                            | M3 actor proof/replay tests                                                              |
| Lens adapter       | `IoAdapter::prepare/execute/publish`, `IoService::registerAdapter`, staged `ProjectionSession` publication                 | Lens owner snapshots plan; service performs; arena owner publishes                                | M4 source-version/conflict tests                                                         |
| File/media         | `FileSourceAdapter`, `ReadObservation`, `MediaRange`, `DecodeLease`                                                        | Read observation and decoder outputs lease independently                                          | M5 mutation/large-offset/media bounds tests                                              |
| Capture            | `CaptureCoordinator::prepare/appendRequest/commit/recover`; versioned journal                                              | Per-user capture owner; permascroll permanent; journal bounded metadata                           | M6 overlap/crash-injection tests                                                         |
| Frontends          | Six bridge APIs in §10                                                                                                     | Frontend owns syntax, static checks, multiplicity and local recovery; common service owns effects | M7a common harness; `M7-<lang>` per-language lowering, diagnostic tests and expert gates |
| Configuration      | `IoBudgetSnapshot`, `system://settings` keys in §11                                                                        | Immutable snapshot per service generation                                                         | M8 config schema/default/notes and pressure tests                                        |

## 3. Common cell schema and protocol

### 3.1 Dimensions and graph

Mint these system dimensions once through the standard dimension registry: `d.io.kind`,
`d.io.state`, `d.io.endpoint`, `d.io.capability`, `d.io.request`, `d.io.payload`, `d.io.progress`,
`d.io.outcome`, `d.io.waiter`, `d.io.occurrence`, `d.io.queue`, `d.io.clone`, `d.io.source`,
`d.io.range`, `d.io.provenance`, and `d.io.lease`. Each request field uses a private occurrence cell
so ZigZag's one-neighbor-per-direction rule is preserved. Shared endpoint/capability canonical cells
are never direct fan-in targets: request occurrence → posward clone view → canonical managed cell.
Queue order is a reciprocal rank of request occurrences. The M0 spike must demonstrate two requests
sharing endpoint and capability, then traverse both directions without overwriting a link.

`IoCellTag` is a closed tagged union stored in normal typed cell content, not magic integer
sentinels: `Endpoint`, `Capability`, `Request`, `Payload`, `Progress`, `Outcome`, `ReadObservation`,
`CaptureReceipt`, and `Waiter`. Outcome values carry `OutcomeTag` (`Rejected`, `Complete`,
`EndOfStream`, `Partial`, `Failed`, `Cancelled`, `TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`,
`Conflict`, `Unknown`). Tags identify schema; fields remain graph links. `RequestId`, `EndpointId`,
`CapabilityId`, `SourceGeneration`, `WaiterId`, `ContinuationId`, `AllocationIncarnation`,
`LeaseId`, and `CaptureId` are private opaque identifiers, never accepted from parsed cell text as
authority.

| Cell value       | Target graph fields (not met; see the reconciliation below)                                                    | Native sidecar role                                         |
| ---------------- | -------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| Endpoint         | kind, generation, state, supported operation/consistency capabilities, read/write queue occurrences            | OS/backend handle, current adapter binding                  |
| Capability       | endpoint occurrence, endpoint generation, rights, byte/concurrency limits, consistency requirements            | Revocation/authority proof; never reconstructed from fields |
| Request          | operation, immutable arguments, capability occurrence, endpoint generation, state, progress, outcome, deadline | Scheduling/index data and native cancellation token         |
| Waiter           | request occurrence, owner/cursor registration, continuation reference, active state                            | Wake queue token resolved to one owner continuation         |
| Payload          | source generation/occurrence, offset, length, provenance, lease association                                    | Chunk pointers and physical lease reference                 |
| Outcome/Progress | outcome tag, typed error/cause, progress units, optional payload/result occurrence                             | Native outcome record; the cells are a view of it           |
| Capture          | selected source intervals, map reservation, append request occurrence, phase                                   | Journal sequence and recovery index                         |

`OpenedEndpoint` contains only owner-created endpoint/capability cell references.
`CaptureAppendReceipt`, `ProbeValue`, and `CommitReceipt` contain typed managed fields.

Reconciliation with the M1–M6 code (review r1, finding zigzag 4): the table's "authoritative graph
fields" column is the target schema, and the code does not meet it. What the graph holds today is
the topology and the tags: endpoint, capability and request identity, the endpoint queue rank, each
request's endpoint and capability clone views, the outcome link, payload/source/progress/range
occurrences, a capture's request and receipt links, and the closed tag of each cell. What lives only
in native tables is a request's operation, frozen arguments, state and deadline (`IoService`), an
endpoint's kind, generation and state (`IoService`/adapters), a capability's rights, limits and
consistency masks (the open authorizer and the request decoder), and a capture's source intervals,
map reservation and phase (`PendingCapture`, `translations_`, the journal). The text fields cells do
carry (`rights`, `consistency`, progress counts, a receipt's `captureId`/`appendDigest`) are
non-authoritative mirrors for inspection that nothing parses back. The price of the departure is
deliberate: a request's arguments are a frozen, cell-free `CellRequest` because workers must never
see cells (§1.1); a capability is authority that "is never reconstructed from fields" (the table's
own sidecar column); and recoverable capture state is the fsynced journal, not the arena, which
`ArenaManifold` keeps runtime-only and refuses to persist. So the sentence that logical links and
tags are canonical and native indices can be rebuilt from them is **withdrawn** for requests,
endpoints, capabilities and captures: the cells are an inspectable, owner-thread view of native
state, and a lost service is recovered only through the journals. Making request state and
capability limits graph-authoritative is a milestone-sized change (scalar cells on `d.io.state` and
`d.io.lease`, a decoder that reads them back) and is not done.

`BufferPolicy` is a typed request field: `Raw{maxBytes, readAhead=0}`, `Fixed{length}`,
`Line{delimiter, maxBytes, maxReadAhead, OversizePolicy, EncodingPolicy}`,
`Frame{mode, maxBytes, maxReadAhead, oversize, encoding}`, where mode is delimiter, length prefix
(width and endianness), or an adapter frame codec, or `Message{maxBytes, OversizePolicy}`. Oversize
policy is `RetainRemainder`, `DrainAndReportGap`, or `Refuse`; a backend must declare support.
Cursor policy is `Sequential`, `AtOffset{uint64_t}`, or `Reset`; frame read-ahead is charged and
endpoint-owned.

The initial operation vocabulary is `Open`, `CaptureAppend`, `ReadBytes`, `ReadFrame`,
`ReceiveMessage`, `WriteBytes`, `SendMessage`, `FlushAfter`, `Sync`, `CloseGracefully`, `Abort`,
`Seek`, `Probe`, `CommitBatch`, `ResolveContent`, and `DecodeRange`. Capture preparation and commit
are coordinator operations; only byte append is dispatched as an IoService operation. Each backend
declares supported operation and consistency capabilities. Unsupported operations are typed refusals
before admission.

### 3.2 Payload types and outcomes

```cpp
struct PayloadRef { SourceId source; SourceGeneration generation; OccurrenceId occurrence; };
struct PayloadRange { PayloadRef origin; std::uint64_t offset; std::uint64_t length; };
struct PayloadLease { PayloadRange range; LeaseId lease; };
struct RequestCell { CellRef cell; AllocationIncarnation incarnation; }; // owner-only wrapper
struct CapabilityCell { CellRef cell; AllocationIncarnation incarnation; }; // owner-only wrapper
struct IoBudgetSnapshot;
class PayloadResolver {
public:
  std::expected<PayloadLease, IoError> retain(PayloadRange);
  std::expected<PayloadLease, IoError> slice(const PayloadLease&, std::uint64_t, std::uint64_t);
  std::expected<OwnedBytes, IoError> read(const PayloadLease&, std::uint64_t, std::size_t);
  std::expected<PayloadLease, IoError> resolveDurable(PrimediaSpan);
  std::expected<PayloadLease, IoError> resolveScratch(ScratchRange, ArenaOwnerToken);
  std::expected<PayloadLease, IoError> resolveTransient(PayloadRange);
  std::expected<PayloadLease, IoError> resolveProxy(ProxyRange);
  void release(LeaseId) noexcept;
};
```

Ranges use checked `uint64_t` arithmetic, are binary-safe, and are immutable while leased. A slice
preserves origin interval identity; a byte-changing transform creates a derived source occurrence
with provenance. Durable permascroll spans, arena scratch, transient chunks, and remote/proxy ranges
resolve by a tagged resolver dispatch. Scratch is never used as a transient-scroll sentinel. A range
that cannot resolve or is not serializable returns a typed `Unresolved` / `TransientNotPersistable`
error; serializing display text does not preserve a lease or capability.

`IoOutcome` is a tagged value:

```cpp
enum class IoErrorCode { Unsupported, InvalidRequest, InvalidCapability, Stale, Conflict,
  DeadlineInvalid, ProfileRefusal, BudgetExceeded, MalformedFrame, MalformedEncoding,
  TruncatedEncoding, CursorModeConflict, PersistenceRefusal, TransientNotPersistable,
  Unresolved, BackendFailure, SchedulerUnavailable, ConfigUnavailable };
struct IoErrorInfo { IoErrorCode code; std::uint32_t stableDetail{}; };
using IoError = IoErrorInfo;
enum class IoPhase { Preparing, Queued, Running, Cancelling, Terminal };
struct IoProgress { std::uint64_t acceptedBytes{}, transferredBytes{}, consumedBytes{};
  std::optional<std::uint64_t> sourceSequence; IoPhase phase; };
enum class PartialCause { EndOfStream, Cancelled, TimedOut, Overflow, BackendFailure };
enum class OversizeDisposition { Retained, Consumed, Unknown };
struct Generation { std::uint64_t value; };
struct GapRange { std::uint64_t firstSequence, lastSequence; };
struct OpenedEndpoint { CellRef endpoint; CellRef initialCapability; };
// M4 reduced this to monostate|OpenedEndpoint: produced bytes and receipt
// details travel through the PayloadResolver channel and the outcome's
// payload field instead of dedicated result variants (see M4 notes).
using IoResultValue = std::variant<std::monostate, OpenedEndpoint>;
struct Complete { std::optional<PayloadRange> payload; IoResultValue value; IoProgress progress; };
struct EndOfStream { IoProgress progress; };
struct Partial { PayloadRange payload; PartialCause cause; IoProgress progress; };
struct Failed { IoErrorInfo reason; std::optional<PayloadRange> payload; IoProgress progress; };
struct Cancelled { std::optional<PayloadRange> payload; IoProgress progress; bool uncertain; };
struct TimedOut { std::optional<PayloadRange> payload; IoProgress progress; bool uncertain; };
struct Overflow { GapRange gap; IoProgress progress; };
struct MessageTooLarge { std::uint64_t required; OversizeDisposition disposition; IoProgress progress; };
struct Stale { Generation expected, observed; };
struct Conflict { Generation expected, observed; IoProgress progress; };
struct Unknown { IoProgress lastKnown; };
using IoOutcome = std::variant<Complete, EndOfStream, Partial, Failed, Cancelled, TimedOut,
                              Overflow, MessageTooLarge, Stale, Conflict, Unknown>;
```

| Operation result                 | Allowed terminal variants                                                                                                  | Payload rule                                                                                                                                            |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Read/frame/message/decode        | `Complete`, `EndOfStream`, `Partial`, `Failed`, `Cancelled`, `TimedOut`, `Overflow`, `MessageTooLarge`, `Stale`, `Unknown` | `Complete` may be zero length; `EndOfStream` has none; partial/error/cancel has bytes only when retained.                                               |
| Write/send/flush/sync/close      | `Complete`, `Partial`, `Failed`, `Cancelled`, `TimedOut`, `Conflict`, `Unknown`                                            | No payload; accepted/transferred counts and uncertainty are required when meaningful.                                                                   |
| Open/probe/commit/capture append | `Complete`, `Failed`, `Cancelled`, `TimedOut`, `Stale`, `Conflict`, `Unknown`                                              | `Complete` carries typed result fields (e.g. endpoint/capability or capture append receipt), not byte payload unless operation explicitly returns data. |

`Complete` may carry zero bytes. `EndOfStream` is distinct. `Partial` carries bytes and an explicit
terminal cause. `Unknown` never authorizes resend. `Cancelled` reports actual known progress plus an
uncertainty flag. Oversized message outcome reports whether the message was consumed or retained;
adapters unable to state this refuse message mode. Every admitted request has exactly one terminal
transition; progress is monotonic and may coalesce, but completion and payload ownership may not be
dropped.

`EndOfStream` is the only EOF spelling. Admission refusal is not an `IoOutcome` variant: it is
`SubmitResult::Refused{OutcomeCell}` containing the `Rejected` outcome tag and an `IoErrorInfo`
field; no request was admitted. `IoErrorCode` is the closed enum above; no exception text or logical
failure is protocol state. `IoProgress` carries monotonic `acceptedBytes`, `transferredBytes`,
`consumedBytes`, optional source sequence and phase. Payload is present only for `Complete`
read/decode results or `Partial`/payload-bearing `Failed`, `Cancelled` and `TimedOut`; successful
write/flush/close `Complete` has no payload and carries counts. Message receive never returns
`EndOfStream`; an empty message is `Complete` with a zero-length payload. Invalid schema,
capability, bounds or unsupported guarantees reject before admission. Malformed bytes, short frames,
and decoder errors after reads are terminal outcomes and preserve any retained payload/progress.

All cell wrappers (`EndpointCell`, `CapabilityCell`, `RequestCell`, `WaiterCell`, `OutcomeCell`,
`PayloadCell`, `CaptureCell`) are owner-only `{CellRef ref; AllocationIncarnation incarnation;}`.
`ContinuationId`, `LeaseId`, `RequestId`, `EndpointId`, `WaiterId`, and `CaptureId` are private
monotonic opaque IDs; no such ID is encoded as an authority-bearing cell integer. Arena minting
returns `MintedCell{CellRef, AllocationIncarnation}`. `ArenaManifold::incarnationOf(ref)` returns
optional incarnation for a live allocation and `matches(ref, incarnation)` validates it; ephemeral
allocation incarnation is never reused, including after mark release. Base-manifold/proxy refs use a
tagged origin identity from their owning store/session and proxy generation. Overwrite preserves
allocation incarnation but the lease sidecar follows the replaced value; release destroys that
incarnation. Arena destruction invalidates all ephemeral incarnations and releases owner leases.
Cell links and tags carry topology and schema; request, endpoint, capability and capture state is
native and the cells are its inspectable view (§3.1 reconciliation). Native tables are validated
against incarnation, never rebuilt from cells.

The language-visible submission value is a managed tagged sum cell: `IoSubmissionValue::Admitted`
links to a `RequestCell`, or `IoSubmissionValue::Refused` links to an `OutcomeCell`. These spellings
are language patterns/terms over the cell graph, never a C++ exception or a missing cell. `await`
accepts only `Admitted.request`; a `Refused` input produces a typed `InvalidRequest` outcome without
service access. VPL retains this sum at each original shape position; VQL returns it as the effect
result; Prolog unifies a tagged value instead of failing; VIcon/VLace return it as an ordinary typed
IO value.

### 3.3 Capability and request graph

A capability is minted only by the endpoint owner after policy validation. It contains private
managed authority, endpoint generation, allowed operations, maximum bytes/concurrency, and
consistency requirements. Copying a managed capability retains the same authority. Reconstructed
topology, IDs, formatted text, or serialized descriptions cannot mint it. Capabilities never imply
retention of traffic history.

A request contains immutable operation parameters, frozen output bytes or bounded output lease,
endpoint generation, capability occurrence, request identity, deadline, and its state/result links.
Admission is rejected before native work if authority, generation, declared maximum, terminal
reserve, or any budget reservation fails. Output freezing completes before a worker can observe it.

## 4. `IoService`, adapter dispatch, and storage API

`apps/common/xanadu/vortex/io_service.{hpp,cpp}` owns admission, budgets, native identities,
endpoint queues, scheduling, cancellation and completion routing. It registers operation adapters
with `registerAdapter(AdapterKey, IoAdapter&)`; endpoint kind plus operation selects exactly one
adapter. The service invokes `prepare` on the owner, schedules the immutable `BackendCommand` on its
reactor or bounded blocking pool, and returns worker completion to its owner pump. There is no
separate lens worker path. The registered `IoAdapter` performs bounded source-specific preparation,
native worker execution and owner publication. Opens, DNS, probes, DB calls and journal writes use
this same path. No caller-runs, nested event loop, or future wrapping a blocking call qualifies.

```cpp
struct SubmitResult { std::variant<Admitted, Refused> value; };
struct Admitted { RequestCell request; };
struct Refused { OutcomeCell outcome; }; // contains Rejected tag; no request allocated
struct AwaitResult { std::variant<Ready, Pending> value; };
struct Ready { OutcomeCell outcome; };
struct Pending { ContinuationId continuation; WaiterCell waiter; };
struct OwnerWaiterRegistration { CellRef cursor; AllocationIncarnation cursorIncarnation;
  ContinuationId continuation; };
enum class WaiterCancelState { Removed, AlreadyComplete, Refused };
struct WaiterCancelResult { WaiterCancelState state; std::optional<OutcomeCell> refusal; };
enum class RequestCancelState { Accepted, AlreadyTerminal, Refused };
struct RequestCancelResult { RequestCancelState state; RequestCell request; std::optional<OutcomeCell> refusal; };

struct BackendCommand;
struct NativeCompletion;
struct IoAdapter;
struct CompletionSink;
struct BackendReleaseAck;
struct BackendResult;
class IoCellStore;
struct AdmissionCost { std::size_t commandBytes, terminalEntries, completionBytes; };
struct SchedulerReservation { PrivateRequestToken token; std::size_t reservedBytes; };
enum class EnqueueResult { Queued, ReservationInvariantViolated };
enum class CancelQueuedResult { RemovedBeforeStart, AlreadyRunning, AlreadyTerminal };
struct BackendContext {
  std::stop_token stop; // scheduler requests this on cancellation
  WorkerPayloadResolver& payloads;
  BackendBudget budget;
};
class CompletionSink {
public:
  virtual void progress(PrivateRequestToken, IoProgress) noexcept = 0;
  virtual void terminalCandidate(PrivateRequestToken, BackendResult,
                                 std::optional<PayloadLease>) noexcept = 0;
  virtual void releaseAcknowledged(BackendReleaseAck) noexcept = 0;
  virtual ~CompletionSink() = default;
};
class IoScheduler {
public:
  virtual std::expected<SchedulerReservation, IoErrorInfo> reserve(AdmissionCost) = 0;
  virtual EnqueueResult enqueue(SchedulerReservation, BackendCommand, IoAdapter&,
                                CompletionSink&) noexcept = 0;
  virtual void releaseUnused(SchedulerReservation) noexcept = 0;
  virtual CancelQueuedResult cancel(PrivateRequestToken) noexcept = 0;
  virtual std::size_t pump(std::size_t completionBudget) = 0; // owner-only delivery
  virtual ShutdownResult requestStop(ShutdownMode) = 0;
  virtual ~IoScheduler() = default;
};

class IoService {
public:
  IoService(IoBudgetSnapshot, std::unique_ptr<IoScheduler>, PayloadResolver&, IoCellStore&);
  void registerAdapter(AdapterKey, IoAdapter&); // stable host-owned adapter lifetime
  SubmitResult admitOpen(OpenSpec);             // same request lifecycle; no endpoint yet
  SubmitResult submit(CapabilityCell, CellRef requestSpec);
  PollResult poll(RequestCell) const;
  AwaitResult await(RequestCell, OwnerWaiterRegistration); // atomically returns Ready or mints waiter
  WaiterCancelResult cancelWaiter(WaiterCell);
  void retireWaiter(WaiterCell); // owner acknowledgement after resume; idempotent
  RequestCancelResult cancelRequest(RequestCell);
  SubmitResult closeEndpoint(EndpointCell, CloseMode);
  std::size_t pump(std::size_t completionBudget);
  ShutdownResult requestShutdown(ShutdownMode);
};

struct BackendCommand { PrivateRequestToken token; PreparedOperation operation; };
struct BackendReleaseAck { PrivateReleaseToken token; }; // minted only after all native users release
struct NativeCompletion { PrivateRequestToken token; NativeStage stage; BackendResult result;
  std::optional<PayloadLease> payload; BackendReleaseAck released; };
struct OwnerPublication { IoOutcome outcome; std::optional<ProjectionDelta> delta; };
struct IoAdapter {
  virtual std::expected<PreparedOperation, IoErrorInfo>
  prepare(const CellRequest&, OwnerContext&) = 0;
  virtual void execute(const BackendCommand&, BackendContext&, CompletionSink&) noexcept = 0;
  virtual std::expected<OwnerPublication, IoErrorInfo>
  publish(const NativeCompletion&, OwnerContext&) = 0;
};
class IoCellStore {
public:
  virtual std::expected<void, IoErrorInfo> authorizeOpen(const OpenSpec&) = 0;
  virtual std::expected<CellRequest, IoErrorInfo>
  freezeRequest(CapabilityCell, OwnerCell) = 0;
  virtual std::expected<CellRequest, IoErrorInfo>
  freezeClose(EndpointCell, CloseMode) = 0;
  virtual std::expected<RequestCell, IoErrorInfo> mintRequest(const CellRequest&) = 0;
  virtual OutcomeCell mintRefusal(IoErrorInfo) = 0;
  virtual std::expected<OutcomeCell, IoErrorInfo>
  publishTerminal(RequestCell, const IoOutcome&, const IoProgress&,
                  const std::optional<PayloadLease>&) = 0;
  virtual bool isOutcomeCurrent(OutcomeCell) const noexcept { return true; }
  virtual std::optional<OutcomeCell> outcomeFor(const RequestCell) = 0;
  virtual std::expected<OpenedEndpoint, IoErrorInfo>
  mintOpenedEndpoint(const OpenSpec&) = 0;
  virtual std::expected<WaiterCell, IoErrorInfo>
  mintWaiter(RequestCell, OwnerWaiterRegistration) = 0;
  virtual void retireWaiter(WaiterCell) noexcept = 0;
};
```

`OpenSpec` freezes
`{adapterKey, locator, optional auth capability, requested rights, consistency, deadline}` on the
owner. Admission order is strict: validate/freeze owner inputs; reserve scheduler queue bytes, one
terminal slot, and completion metadata; mint/link the request and mark it admitted; then call
nonblocking `enqueue`. A refusal before reservation starts no work. After reservation, enqueue
cannot block; if it reports `ReservationInvariantViolated`, the service uses the reserved completion
slot to publish a terminal `Failed{SchedulerUnavailable}` for the already-admitted request, attaches
a synthetic release acknowledgement because no backend began, and performs no backend call. That
request identity remains observable. `admitOpen` returns `SubmitResult::Admitted{RequestCell}` or
`Refused{OutcomeCell}`. An admitted Open request has no endpoint/capability authority until its
terminal successful `Complete` is materialized on the owner; failed/cancelled opens release native
handles after backend acknowledgement. Its exact same waiter/Pending/cancellation/budget path
applies. `submit` likewise admits a fresh request every call. `BackendCommand` and
`NativeCompletion` are service-private cell-free types; their private request token is never exposed
to VM or language values. `prepare` validates and freezes bounded arguments only. `execute` gets no
`OwnerContext`, arena, CellRef or frontend callback. `publish` runs on owner, checks
generations/incarnations, mints outcome/endpoint cells and atomically stages projection state.

`SchedulerReservation` is acquired before `Admitted` is visible and covers bounded queue storage,
one terminal completion slot, progress metadata and declared command/payload staging charges. The
scheduler's `enqueue` is nonblocking. Its reservation-invariant failure is a service-generated
terminal `Failed{SchedulerUnavailable}` from the reserved slot, not a dropped request or retry.
Workers get `BackendContext` with a stop token, bounded worker payload resolver and immutable
backend budget; no arena, callbacks or frontend activation. `cancelRequest` signals the stop token
and asks the scheduler to remove queued work. If removed before start, the service supplies a
`Cancelled` terminal candidate and release acknowledgement using the reserved slot. If already
running, the adapter observes cancellation and supplies a terminal candidate when it knows the
result; the scheduler supplies release acknowledgement only after OS handles, buffers and kernel
registrations are no longer in use. The service never synthesizes cancellation for running work
before that acknowledgement.

`IoCellStore` is the owner-only arena bridge held by IoService. Its mint methods create the managed
request/outcome/endpoint/capability cells (topology and tags; §3.1 says which state stays native);
waiters are owner-only transient cells. `mintWaiter(RequestCell, OwnerWaiterRegistration)` and
`retireWaiter(WaiterCell)` are owner-only. `IoService::await` serializes with owner `pump`, checks
terminal state and registers/mints within one critical section; duplicate
`(request incarnation, cursor incarnation, continuation)` registration is idempotent. A completion
candidate queued by a worker is not terminal until owner pump applies it, so a waiter registered
first is woken by that pump; if pump applied terminal state first, Await returns Ready without
minting a waiter. There is no completion-before-registration lost-wakeup window.

`CompletionSink::progress` is thread-safe and coalesces to one bounded latest-progress record per
request. It cannot consume terminal reserve. `terminalCandidate` stores one worker result plus its
payload lease; `releaseAcknowledged` is a separate scheduler-supervisor signal after descriptors,
mappings, kernel registrations and buffer users are released. The sink buffers whichever arrives
first and creates one `NativeCompletion` in the reserved terminal slot only after both exist. A
duplicate candidate or acknowledgement is a backend contract violation logged and ignored; the first
of each remains canonical. For queued work canceled before start, the scheduler provides both the
`Cancelled` candidate and release acknowledgement without launching the adapter. Owner `pump` drains
progress/terminal records, validates identity/generation, publishes one terminal outcome, wakes
registered waiters, and releases scheduler reservation only after terminal handling and release
acknowledgement. Until then, native buffers and request metadata remain charged.

If a worker exits abnormally, the scheduler supervisor cancels/closes backend resources, posts an
`Unknown` terminal candidate, then issues release acknowledgement. If cleanup cannot be confirmed,
the work remains charged and later admissions backpressure. Threads/resources are not replaced
without bound. `IoAdapter::execute` is `noexcept`; implementations catch backend exceptions,
complete cleanup, and report `BackendFailure` through the reserved sink rather than unwinding across
the worker boundary.

`SubmitResult` is `Admitted{RequestCell}` or `Refused{OutcomeCell}`. `AwaitResult` is
`Ready{OutcomeCell}` or `Pending{ContinuationId, WaiterCell}`. The host supplies
`OwnerWaiterRegistration`; it does not mint a waiter cell. `WaiterCancelResult` reports `Removed`,
`AlreadyComplete`, or `Refused`; it never changes the request. `RequestCancelResult` reports
`Accepted` (cancellation requested), `AlreadyTerminal`, or `Refused`, plus the original request
cell; eventual progress and uncertainty are observed by awaiting that request. Waiter cancellation
removes only that waiter; request cancellation is idempotent and cannot retract sent bytes.
Cancel-before-start removes queued work if possible; cancel-after-terminal returns
`AlreadyTerminal`; late completion updates the original request record only. `closeEndpoint` returns
its own admitted request and uses the same awaitable outcome path. On successful Open completion,
`publish` mints `EndpointCell` and the requested `CapabilityCell`; the outcome links to these
values. Failed or cancelled open publishes no authority and releases native handles after backend
acknowledgement. Shutdown stops admissions and requests cancellation, then owner polling reports
remaining backend-owned resources; it never joins a running worker on the evaluation/UI thread.

Service indexes are keyed by private request token and validated cell incarnation. Request records
remain outside solver marks while any semantic owner or native user exists. Last-owner abandonment
removes unstarted work or requests cancellation; bounded tombstone and leases remain until backend
acknowledgement. Endpoint queues are FIFO per direction. `FlushAfter` names the admitted writes it
fences; it does not depend on completion order and does not imply durable sync or peer receipt. Read
and write queues are independent for full duplex. Independent operations may complete out of order.
Deadlines are absolute steady-clock deadlines in the admitted request; expiration produces typed
`TimedOut` outcome with progress, requests backend cancellation if specified, and retains identity
for late reconciliation.

## 5. VM/core/continuation contract

`VortexCore` remains arena structure primitives and contains no external I/O scheduler or frontend
policy. `VortexVM` keeps its existing per-cursor execution model. `step(cursor)` remains one opcode;
`run(cursor, maxCycles, stopToken)` remains bounded execution with the current stop/cut behavior.
I/O Pending is a separate disposition and never overloads stop-token cancellation or logical
failure.

```cpp
enum class StepDisposition { Progress, Yielded, Pending, Halted, Failed };
struct PendingAwait { ContinuationId continuation; WaiterCell waiter; }; // no RequestId
struct ExecutionResult {
  StepDisposition disposition;
  std::optional<PendingAwait> pending;
  std::optional<VmError> error;
};
ExecutionResult VortexVM::step(CellRef cursor);
ExecutionResult VortexVM::run(CellRef cursor, std::size_t maxCycles, std::stop_token);
ExecutionResult VortexVM::resumeAwait(CellRef cursor, ContinuationId, const IoOutcome&);
```

The await opcode receives the cell reference from `IoSubmissionValue::Admitted` and asks the arena
to resolve its `RequestCell` sidecar and validate `AllocationIncarnation`. The VM creates the
`ContinuationId`; `IoHost` sends cursor/incarnation/continuation to `IoService::await`, which under
the owner serialization boundary checks terminal state and registers the waiter as one operation.
Service completion state changes only during owner `pump`: if a worker finished before registration
but the pump has not delivered it, the await registers and that next pump wakes it; if pump already
applied it, await returns `Ready`. There is no completion-before-registration lost-wakeup window. If
terminal, owner materializes an `OutcomeCell` and continues. If pending, the VM records `cursor`,
PC/frame/choice state, frontend continuation token and waiter identity under a fresh owner-private
`ContinuationId`, then returns `Pending`. The frontend adapter stores its activation by
`(cursor incarnation, continuation)`; the host completion pump resolves the waiter to exactly one
`(cursor, continuation)` registration and resumes that cursor only; one request may have multiple
waiters and one cursor may have multiple parked awaits, each with its own continuation. Resume
continues the same await; it cannot jump to the Submit site. Parked execution retains output
bindings, guards, choices, activation state and frontend-owned activation; no VM cycles accrue until
resumed. Completion never mutates VM state from a worker.

On backtracking, request records and results remain outside the arena mark. Outcome cells and
bindings are trailed ordinary arena state; re-await after rewind rematerializes the retained
observation. Arena mint returns `MintedCell{ref, incarnation}`; `incarnationOf(ref)` and
`matches(ref, incarnation)` are owner-only APIs. Each ephemeral allocation has unique incarnation
even when a released dense index is reused. Proxies/base-manifold refs include source-session
identity and proxy generation. Save/restore trail hooks retain prior leases; overwrite
transfers/releases value leases; release/discard/cut, frame teardown, exceptions, proxy retirement
and arena destruction release only after owners are gone. No sidecar may validate `contains(ref)`
alone.

`IoHost::submitCell(CapabilityCell, CellRef requestSpec) -> SubmitResult`,
`admitOpen(OpenSpec) -> SubmitResult`,
`awaitCell(RequestCell, CellRef cursor, AllocationIncarnation, ContinuationId) -> AwaitResult`,
`retireWaiter(WaiterCell)`, `cancelWaiter(WaiterCell) -> WaiterCancelResult`,
`cancelRequest(RequestCell) -> RequestCancelResult`, and `pumpIo(completionBudget) -> size_t` are
owner-thread host APIs. Await/Pending is language-neutral. Frontend adapters retain their own
activation and map the continuation to/from the generic cursor.

## 6. Lens and adapter publication

`IoAdapter` in §4 is the sole dispatch interface used by IoService. `ProjectionLens` remains the
source identity/projection abstraction; `ProjectionSession` remains owner-thread identity mapping
and source publication. The service selects an `IoAdapter` by `AdapterKey{sourceKind, operation}`.
Each source endpoint binds to one adapter registration/generation. Router/Union adapters may
delegate a prepared immutable operation, but preserve original request identity, source identity and
cancellation; they do not resubmit.

`prepare(const CellRequest&, OwnerContext&)` validates capability, source generation and request
fields, freezes bounded native arguments, and returns `PreparedOperation` or a typed pre-admission
refusal. `execute(const BackendCommand&, BackendContext&, CompletionSink&)` runs off-owner, emits
one `NativeCompletion` with private service token and native staged data, and never sees cells.
`publish(const NativeCompletion&, OwnerContext&)` validates the original request, endpoint/source
generation and cell incarnations, then returns owner-side outcome/projection deltas. IoService
creates managed result cells and applies staged deltas atomically under an arena mark. Stale stages
become typed terminal outcomes; no source mutation happens beneath a saved choice.

Existing `ProjectionSession` synchronous edit/refresh semantics remain available to non-I/O users.
Its async methods are `prepareRefresh()`, `prepareCommit()` (both owner-only, bounded), service
submission, then `publishRefresh(OutcomeCell)` / `publishCommit(OutcomeCell)` on owner. Source
version, transaction, conflict and dirty-edit rules remain lens-owned. `pollChanged` and watchers
are only hints; probe I/O is an admitted service operation. `CommandMailbox` is migrated from inline
handler invocation to request cells and later owner publication. `NetStreamRing` becomes
service-owned leased transient payload with backpressure. Adapter rollout: stream/process/network,
filesystem observations, SQL/config transactions, Git object/worktree distinction, log/journal, then
composite and swarm/control.

## 7. Files, framing, messages, buffering, media

### 7.1 Streams and source observations

`ReadBytes(offset,maxBytes)` is random access or sequential according to an explicit cursor policy.
`ReadFrame(mode,maxBytes)` may read ahead, but unread suffix bytes remain leased by the endpoint for
the next frame. Mixing raw read, frame read, seek and reset must advance one declared source cursor
or return `CursorModeConflict`; it may not silently skip suffixes. A byte stream returns
`EndOfStream`; a message endpoint returns one whole message (including a valid zero-length message)
and never synthesizes stream EOF.

`WriteBytes` freezes one buffer version at submit. `FlushAfter` fences previously admitted writes.
`CloseGracefully` drains and reports; `Abort` cancels and reports unsent/partial/unknown bytes.
Every submit, flush, close and abort has its own request and outcome. A dependent operation is
submitted only after its prerequisite terminal outcome meets policy. Host adapters expose buffering
as a pure bounded value before submission and service buffering as separately accounted admitted
requests.

Framing/decoding uses explicit adapter policy: delimiter/fixed-length/message, max frame, oversize
drain/retain choice, decode encoding and malformed/truncated policy. There is no generic lexer or
frontend tokenizer. Chunk boundaries may split delimiters/code points; retained decoder state is
bounded and separately leased. Unsupported cursor or overflow semantics are refused. Full duplex
allows read progress while output is backpressured if the backend supports it, with independent
cancellation/accounting.

### 7.2 Mutable files

`FileSourceAdapter::open` returns a locator plus declared capabilities, never an immutable identity.
Each successful live read produces
`ReadObservation{observationId, sourceGeneration, range, consistency}` whose exact retained bytes
remain readable while leased. Each observation has a distinct origin unless it is the same
backend-proven immutable snapshot or same observation. `stat` and watches do not prove atomicity. A
strict snapshot/CAS request is refused if backend cannot meet it. Queued generation-bound requests
return `Stale`, `Conflict`, or `Unknown` when appropriate. After uncertain write, invalidate live
generation/cache and require revalidation; prior observations remain readable. Git object IDs may
identify immutable Git objects; working-tree paths remain live sources.

### 7.3 Rich media

Media access is `ReadBytes` with checked 64-bit byte offsets and bounded lengths. `DecodeRange` is
an adapter request, never a media-specific VM opcode. Encoded source, decoded frame/sample, index
and GPU surface have separate leases and budget classes. Seek/cancel releases derived work only
after native acknowledgement. Time-to-byte, keyframe selection, codec/container dependencies and
decoding live in adapters. Selected-range capture registers only those bytes; self-contained
playable capture includes explicit dependency closure and can refuse. No operation implicitly loads
or captures a whole file.

## 8. Transient storage and capture durability

### 8.1 Chunk store

Use immutable bounded chunks as the initial transient backing. Logical address is
`(SourceId, SourceGeneration, OccurrenceId, offset, length)`; physical chunk allocation is private.
A lease pins whole physical allocation, charged at physical size even for a tiny slice. Chunks
recycle after all cell, continuation, trail, capture, cache, kernel and backend users release them.
An unpausable source reserves a terminal/gap record and emits coalesced `GapRange` on loss. There is
no ring overwrite of pinned data and no spill tier.

Charge aggregate bytes and entries for ingress, frozen output, staging, payload chunks,
fragmentation, requests, continuations, proxies, completion queue, capture map, decoder output,
indexes, GPU surfaces, and kernel/native leases. Service settings are snapshotted per generation.
Ordinary payload cannot consume terminal/control reserve. Admission refuses before exceeding any
cap. Backpressure and refusal are valid outcomes, never silent data loss.

### 8.2 Capture interface and format

Capture is a two-resource transaction: append selected transient bytes to `UserPermascroll`, then
commit translations and Structure references. It is never implicit during read/render. The capture
owner freezes selection and leases, computes interval overlaps against existing mappings, appends
only unmapped suffixes, durably records append identity and translation intent, then publishes
Structure references and finalizes the mapping. If recovery cannot prove ordering, overlapping
capture is refused pending reconciliation; orphan append bytes may remain unreachable but are never
guessed into identity.

```cpp
struct CapturePrepared { CaptureCell capture; };
struct CaptureAppend { SubmitResult admission; }; // admitted request is awaited normally
struct CaptureCommit { CaptureReceiptCell receipt; };
struct CaptureRecovery { CaptureCell capture; RecoveryState state; };
class CaptureCoordinator {
public:
  CapturePrepareResult prepare(std::span<const PayloadRange>, CapturePolicy);
  CaptureAppend appendRequest(CaptureCell); // delegates to IoService CaptureAppend op
  CaptureCommitResult commit(CaptureCell, OutcomeCell successfulAppend);
  CaptureRecoveryResult recover();
};
```

`prepare` reserves bounded interval-map/journal capacity and freezes source leases, otherwise
refuses without append. `appendRequest` returns the common
`Admitted{RequestCell}`/`Refused{OutcomeCell}` sum; it is not a second scheduler. Await/Pending and
cancellation use ordinary service APIs. `commit` accepts only a successful terminal append outcome
and publishes translated Structure spans plus a receipt under one owner mark. Failed, partial,
unknown or cancelled append cannot publish translations. `CapturePrepareResult` is
`Prepared{CaptureCell}` or `Refused{OutcomeCell}`. `CaptureAppend` wraps common `SubmitResult`;
successful append is observed through normal Await. `CaptureCommitResult` is
`Committed{CaptureReceiptCell}` or `Refused{OutcomeCell}`. Before publishing, `commit` follows the
append request occurrence stored in this exact `CaptureCell`, validates its allocation incarnation,
and verifies that `successfulAppend` links back to that same request and is its terminal successful
outcome. An outcome from another capture, stale request incarnation, refusal, or nonterminal request
is rejected as `InvalidRequest`; no translation or Structure operation is published. Recovery
returns `Reconciled{CaptureCell}`, `NeedsReconciliation{CaptureCell}` (overlap capture refused), or
typed format/I/O refusal; it never resubmits an append. Capture is explicit and a language must
pattern-match its managed result value.

Translation is a bounded interval map from source-generation intervals to permanent spans. Overlap
reuses exact translated addresses (e.g. \[0,10) then \[5,15) appends \[10,15)); adjacency coalesces
only when both translations are contiguous. Hashes validate integrity only. Translation entries do
not pin transient bytes. Local capture does not invent remote signature/provenance.

The new private durable capture journal is `capture.journal`, signature `\x89XUDCAP`, version 2,
length-delimited records with record kind, sequence, CaptureId, source interval, permascroll span,
append digest, and checksum. Records are append-only; unknown signature/version, checksum failure,
truncated record or sequence gap is a typed refusal naming the number/error. It stores metadata
only, not transient bytes. A journal entry is fsynced before Structure publication; completion
record follows publication. Recovery reconciles journal and permascroll high-water mark and never
silently duplicates an ambiguous overlap. This does not change `ops.nodes`, `store.tables` v4,
`CompactOpNode`, or store primedia rules. Any future on-disk record shape change bumps this journal
version and deletes the old reader per repository policy.

## 9. Runtime profiles and actor proof

`IoExecutionProfile` is a generic immutable host contract: `Interactive`, `Batch`, `ActorTurn`, or
`DurableEffect`. It carries legal suspension/submission flags, budgets, durability requirements and
completion delivery mode. Core/VM do not inspect profile names. `IoHost` applies profile policy
before service admission and emits typed `ProfileRefusal`.

An actor turn is atomic: direct `Submit`/`Await` and any yielding VAgent handler are refused before
admission; runtime repeats check. Reversible VAgent effect intents contain immutable bytes/order,
actor generation, committed turn identity and ordinal. Failed/rewound turn stages nothing. After
commit, dispatcher writes and flushes a durable attempt record off-thread; only after owner receives
acknowledgement can IoService work begin. Journal queue/bytes/flush count against budgets. Durable
journal full/unavailable refuses before external work. Completion returns as a later actor message
with generation, request/correlation and typed outcome, no CellRef and no serialized VM
continuation.

Actor ledger format: `actor-io.journal`, signature `\x89XUDACT`, version 1, append-only bounded
length-delimited records for stable intent identity, endpoint generation, request identity,
admission, receipt/unknown result and checkpoint causal floor. Recovery never resends an attempt.
Missing, unknown or unreconciled proof refuses actor restore/replay by typed outcome. Restore while
admission or work unresolved returns `NotQuiescent`. Crash after durable admission with no
conclusive receipt returns `Unknown`. Ledger records retire only after checkpoint causal floor and
acknowledgements permit retirement. An old-generation completion reconciles and releases only its
original request. Evidence remains separately governed by exact-intent grants, durable pre-I/O
admission, receipts, revocation and explicit duplication acknowledgement; this ledger does not
weaken those rules.

## 10. Frontend bridge contracts

The language-visible `submit` result is always one managed tagged sum cell: `Admitted{request}` or
`Refused{outcome}`. Every frontend binds and pattern-matches this sum before calling await. `await`
accepts only an admitted request; a refusal remains a value and cannot become logical failure, an
empty query result or a dropped array slot. Terminal values are distinct from the execution result
`Pending{continuation}`. Each frontend parks its own activation and returns a runner-visible parked
result; after host notification it resumes that activation and re-awaits the same request. Native
`RequestId`, `WaiterId` and `ContinuationId` never enter language values.

Where current grammar/API cannot express the target, the feature is gated off with a specified
diagnostic/result; no synchronous fallback is allowed on an advertised async path. Each complete
trace below shows admission refusal, Pending and terminal status gating.

Each frontend runner exposes a separate
`FrontendRunResult = Completed{value} | Parked{FrontendToken} | Failed{FrontendDiagnostic}`.
`FrontendToken` is frontend-owned host state and carries its suspended activation, request cell and
incarnation, waiter cell, continuation ID, and optional position. This token is never a language
value. `IoHost` wakeup routes it to the owning runner, which resumes the same activation. Admission
refusal remains a `Refused` sum value. An await evaluates to either `Pending` (park, retain and
re-await the same request) or a terminal `OutcomeCell`. For VPL, vectorized await yields a
shape-preserving sum at every position; refused positions pass through unchanged. Its parked carrier
retains `{VplView submissions, VplView outcomes, nextPosition}` and scans positions left-to-right
with at most one active waiter per VPL activation: it parks on the first pending position, while all
other admitted requests continue independently and retain outcomes in their original request cells.
On wake it re-awaits only that position, retires its waiter, and continues left-to-right; it skips
already-terminal/refused positions and parks again at the next pending position. Canceling the
activation cancels its active waiter only; request cancellation is a separate explicit action.
Completion order never changes shape or position. No language maps `Pending` to terminal or refusal
to logical failure.

| Frontend                                                                | First implementation surface and complete path                                                                                                                                                                                                                                         | Result carrier, placement and refusal behavior                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ----------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| VQL / vquery, vqueryc                                                   | Planned `EffectClause` form: `let $r := submit($cap, read_bytes($off,$max)); match $r { admitted($req) => await($req), refused($err) => $err }`. Add the binding/match arm to script grammar only; do not add to row/query expressions.                                                | Extend `ExecutionBlock` result with `Completed{rows}` or `Parked{scriptActivation, effectAction, requestCell}`. Runner resumes the exact script effect: `Pending` returns `Parked{FrontendToken}` and the wake path re-awaits; `Complete(payload)` alone permits payload access, while `EndOfStream`, `Failed`, `Cancelled`, `TimedOut`, and `Unknown` are script outcome values, never `QueryError`/empty rows. Submit action site runs once per reached script action; no per-row fanout in M7 (`VQL_IO_QUERY_CONTEXT` until explicit later gate).                                                                                                                                                                                                                                                                                |
| VPL / vpl, vplc                                                         | `R ← submit¨ Caps Specs`; `Out ← await¨ R`. `R` has same input shape with `Admitted(req)` or `Refused(err)` at every position. `await¨` passes refusals through unchanged.                                                                                                             | Add `ExecutionResult` alternative `Parked{activation, submissions, outcomes, nextPosition, requestCell, waiterCell}` separate from `ExecutionError`; runner retains the same `Executor` activation and shape. One active waiter parks at the first pending position; other requests continue and retain results. On wake, re-await only that request, then continue scanning; refusals and terminal positions stay in place. Equal-shape inputs required before any submit; admission deterministic left-to-right with bounded concurrency; completion order does not reorder. Message/frame modes return `IO_UNSUPPORTED_MODE` until their adapter gate.                                                                                                                                                                           |
| VProlog / Vlog                                                          | Planned top-level query effect form: `io_submit(Cap, read(Off, Max), Submission), ( Submission = admitted(Req) -> io_await(Req, Outcome) ; Outcome = Submission )`. Extend query-driver grammar/lowering to recognize this explicit effect goal; ordinary user clauses cannot call it. | Extend the query runner result to `Completed{solutions}`, `Parked{FrontendToken}`, or `Failed{PrologDiagnostic}`. The parked activation retains the `CompiledQuery`, VM cursor, current substitutions/bindings, and Vlog trail/choice state; on wake it resumes that same activation and re-awaits the same `Req`, never re-running submit. Admission refusal binds the tagged `refused(OutcomeCell)` term to `Outcome`; it is not predicate failure. Await accepts only `admitted(Req)`; Pending is runner state, distinct from exhausted search or logical failure. Permit effects only as an explicit top-level query goal outside clauses and speculative choice/backtracking; reject other placements with `PROLOG_IO_EFFECT_CONTEXT`. Terminal outcome tags remain values and payload access is gated by the terminal status. |
| VLace                                                                   | Planned pure action constructor: `readBytes endpoint offset max` builds a lazy IO value; runner invokes `Driver::runIOAsync(action)`. Its internal `IO.ReadBytes` node is admitted only when the runner performs it.                                                                   | `IoRunResult = Done(IOValue) or Parked{IOContinuation, RequestCell}`; Pending is runner state, not an IO constructor. Resume same action continuation and re-await. Keep existing `pure/bind/then/putStr/getLine` terms; migrate their host trampoline. `VLACE_ASYNC_UNAVAILABLE` until a path is migrated; no blocking `getline` fallback.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| VIcon                                                                   | `match submit(cap, read_bytes(off,max)) { admitted(req) => await(req), refused(err) => err }`; existing `write(value)` remains host yield.                                                                                                                                             | Replace `Driver::resume() -> optional<CellRef>` with `DriverResult = Yielded{CellRef} or Parked{generatorCursor, FrontendToken} or Failed{IconError} or Finished`; `nullopt` remains Icon failure only in old API. Pending returns `Parked`; wake resumes the same generator and request. Only payload-bearing terminal variants permit byte-cell access; EOF and typed errors stay distinct. Stdout is a separate explicit host write/flush chain.                                                                                                                                                                                                                                                                                                                                                                                 |
| VAgent                                                                  | Source-shaped target: `capacity Writer { effect def write(bytes: Bytes): Unit within 1s }`; plan body uses `let result := Writer.write(bytes)` as an effect suspension statement. Existing plan effect-call/outbox syntax is retained.                                                 | Existing executor stages the typed outbox after deliberation; after commit adapter snapshots immutable args and durably journals before IoService admission. Existing `EffectDone`/`EffectFailed` correlation carries managed outcome value in a later owner event; `EffectFailed` is an event with typed refusal/failure data, not goal failure. No mutable `NativeBody(WorkerContext&)` goes to workers. Actor suspension is refused with `VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN`; local runner uses separate interactive profile.                                                                                                                                                                                                                                                                                                    |
| Direct Vortex (shared-host baseline; not counted among the six bridges) | `Submit(capCell, opCell) -> Admitted(requestCell) or Refused(outcomeCell)`; `Await(requestCell) -> Pending(continuation) or OutcomeCell`.                                                                                                                                              | Generic cell protocol and runner `Parked` path; tests prove no frontend branches in Core/VM and no raw IDs in cells.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |

For every frontend, malformed request/capability/bounds reject before service admission. Pending
carries no payload; caller retains the request and re-awaits. `Complete` read/decode results expose
a payload; `EndOfStream` has none; `Partial`, `Failed` or `Cancelled` expose payload only when
present and preserve progress. Payload dereference before checking the terminal tag is invalid and
diagnosed where statically possible. `EndOfStream` is the sole EOF tag. Renderers, VQL query
clauses/projections, Prolog pure predicates, VPL arithmetic and VIcon presentation cannot
submit/cancel/capture. Static checks are frontend-owned; service profile/capability checks are
backstops.

## 11. Configuration and budgets

`IoBudgetSnapshot` is immutable for one service generation and resolved from host configuration.
Budget fields: aggregate transient payload bytes; frozen output bytes; native/backend bytes; request
and continuation count; completion/control entries; retained metadata bytes; capture translation
entries/bytes; per-turn owner work units; decoder output bytes; media/GPU leases; durable journal
bytes/entries; terminal reserve bytes/entries; per-endpoint concurrency. The service has no implicit
numeric fallback: construction requires one fully populated immutable snapshot. Host resolution uses
schema defaults from `system://settings`; if config resolution fails, service startup refuses with
`ConfigUnavailable` instead of selecting hidden ceilings. M8 measures representative workloads and
selects those schema defaults before release. Owner-side plan/freezing work is charged to the
per-turn budget; first release returns pre-admission `BudgetExceeded` when that budget is exhausted
rather than creating a second preparation continuation. Unsupported operation or consistency
capability returns pre-admission `Rejected{Unsupported}` with zero backend calls.

User-adjustable values live in `system://settings`, with matching `defaultSettingSpecs()` entry,
`d.schemas`, schema/purpose system xanadoc and linked user-notes page. Keys use `io.*` namespace and
units in schema; defaults/ranges/refusal behavior are explicit. Host resolves one snapshot before
service construction; changing configuration creates a new generation and cannot change limits of
admitted work. Invariants such as offset width and file descriptor constraints are named typed
constants. No budget is silently duplicated in service code.

## 12. Durable formats and migration

No existing store format changes. No bytes are added to `ops.nodes`, `store.tables`, permascroll, or
Structure operations for transient I/O. Transient handles/capabilities/requests are non-persistable;
store serialization of them returns typed refusal. Explicit capture serializes only durable
references through existing permascroll and Structure mechanisms. Actor and capture metadata use
separate versioned journals specified above. Unknown versions refuse by signature/version number; no
empty fallback or compatibility shim.

Migration order keeps existing non-I/O semantics until the replacement passes its gate. Preserve
synchronous `ProjectionSession` behavior where used by existing non-I/O clients; introduce
asynchronous staging APIs, then migrate one adapter at a time. Existing compute worker pool remains
compute-only. No promise wrapping a caller-thread handler. Keep frontend-specific recovery in its
compiler/adapter. Legacy Vortex frontend opcode cleanup is explicitly independent and must not
expand this work.

## 13. Milestones, dependencies, and acceptance

| Milestone                             | Work and dependency                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | Exit evidence                                                                                                                                                                                                                                                                                                                                                                                                           |
| ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| M0 — schema/storage spike             | Settle `d.io.*`, graph occurrences/clones, tagged resolver, allocation-incarnation hooks and lease ownership. No worker yet.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           | Two requests share endpoint/capability and queue; all links traverse; binary payload slice works; transient serialization refuses; mark/release/reuse/exception tests prove leases and stale references.                                                                                                                                                                                                                |
| M1 — service core                     | Add IoService, registered adapter dispatch, asynchronous Open, budgets, worker/reactor seam, typed outcomes, endpoint ordering, cancellation, cleanup and delayed fake backend. Depends M0.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Tests prove no owner callback on worker, no caller-runs, reservation refusal before admission, enqueue failure becomes terminal SchedulerUnavailable, sink terminal is once-only, release acknowledgement gates reclamation, terminal reserve/backpressure, duplex ordering, partial/unknown/cancel and bounded queue.                                                                                                  |
| M2 — VM continuation                  | Add per-cursor Await/Pending/resume host stepping, waiter-to-cursor routing and arena incarnation/lease hooks. Depends M1.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | Outstanding marks and choices survive park; same request is re-awaited after resume/backtrack; no cycle burn while parked; stale completion cannot touch reused cell; before/after-pump waiter registration has no lost wakeup; duplicate await/retire is idempotent; atomic publication.                                                                                                                               |
| M3 — profiles/actor                   | Generic profile gate, VAgent intent staging, durable journal, Evidence adapter preservation. Depends M1–M2.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Crash injection around journal flush/dispatch/receipt/checkpoint; unknown refuses replay, no silent resend, actor completion is later message, full journal refuses before service admission.                                                                                                                                                                                                                           |
| M4 — lenses/adapters                  | Registered IoAdapter staged source plan, stream/process/net, command mailbox, then DB/config/Git/log. Depends M2.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      | Source generation/conflict tests, ProjectionSession transaction semantics retained, Router/Union no duplicate submission, endpoint direction ordering.                                                                                                                                                                                                                                                                  |
| M5 — file/media                       | ReadObservation, live/stable capability contracts, 64-bit range reads, bounded decoder/cache leases. Depends M1/M4.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    | Replacement/truncation/in-place mutation/external writer tests; offsets above 4 GiB; no path-only cache reuse; bounds and cancellation cleanup; unsupported atomicity refuses.                                                                                                                                                                                                                                          |
| M6 — capture                          | Per-user overlap map, capture coordinator and v3 journal. Depends M0/M1/M5.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | \[0,10) then \[5,15) reuses overlap; discontiguous split; mismatched append request/outcome refuses publication; failures injected after each durable step; restart reconciles; no hash identity; orphan bytes handled explicitly.                                                                                                                                                                                      |
| M7a — frontend common bridge features | Language-neutral pieces every frontend shares: managed `IoSubmissionValue` `Admitted`/`Refused` tagged-sum cell and its pattern accessors; generic `FrontendRunResult = Completed or Parked{FrontendToken} or Failed{FrontendDiagnostic}` carrier; `FrontendToken` (suspended activation, request cell and incarnation, waiter cell, continuation ID, optional position) as host state, never a language value; `IoHost` wakeup routing of a token to its owning runner; shared terminal-status gating for payload access (`Complete` exposes payload, `EndOfStream` none, `Partial`/`Failed`/`Cancelled`/`TimedOut` only when present); shared diagnostic-code registry for the frontend refusals in §10 and §14; Direct Vortex shared-host baseline conformance (Submit/Await cell protocol, runner `Parked` path); reusable conformance test harness every language milestone runs. Depends M2; adapter gates only via the M1 delayed fake backend. | Direct-host conformance suite passes, proving no frontend branches in Core/VM and no raw IDs in cells. Harness proves same-request Pending resume, refusal remains a value, terminal outcomes are not logical failure, and no raw `RequestId`/`WaiterId`/`ContinuationId` in language-visible values.                                                                                                                   |
| M7-VQL                                | `EffectClause` `let $r := submit(...); match $r { admitted => await, refused => $err }` in script grammar only; `ExecutionBlock` result gains `Completed{rows}`/`Parked{scriptActivation, effectAction, requestCell}`; submit runs once per reached script action; row/query placement refused with `VQL_IO_QUERY_CONTEXT`. Depends M7a; real `read_bytes`/file endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Script-action multiplicity (no per-row fanout), same-request Pending resume, terminal outcomes are script values never `QueryError`/empty rows, pure query clauses/projections, vquery/vqueryc CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by a VQL expert reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vql/`.                 |
| M7-VPL                                | `R ← submit¨ Caps Specs`; `Out ← await¨ R` shape-preserving sums; `ExecutionResult` gains `Parked{activation, submissions, outcomes, nextPosition, requestCell, waiterCell}` separate from `ExecutionError`; one left-to-right waiter per activation; equal shapes required before submit; message/frame modes refused with `IO_UNSUPPORTED_MODE`. Depends M7a; real endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Out-of-order completion with one left-to-right waiter, per-slot refusal passes through, shape mismatch refused before submit, same-request Pending resume, pure arithmetic, vpl/vplc CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by a VPL expert reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vpl/`.                           |
| M7-VProlog                            | Top-level `io_submit(Cap, read(Off, Max), Submission)` / `io_await(Req, Outcome)` query goal in query-driver grammar; runner result `Completed{solutions}`/`Parked{FrontendToken}`/`Failed{PrologDiagnostic}`; parked activation keeps `CompiledQuery`, VM cursor, bindings and Vlog trail/choice state; clause or speculative placement refused with `PROLOG_IO_EFFECT_CONTEXT`. Depends M7a; real endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             | Multiplicity/backtracking tests (resume never re-runs submit), refusal binds `refused(OutcomeCell)` not predicate failure, Pending distinct from exhausted search, pure predicates, vprolog CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by a Prolog expert reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vprolog/`.             |
| M7-VLace                              | Pure `readBytes endpoint offset max` lazy IO constructor run by `Driver::runIOAsync`; `IoRunResult = Done(IOValue) or Parked{IOContinuation, RequestCell}`; existing `pure/bind/then/putStr/getLine` trampoline migrated; unmigrated paths refused with `VLACE_ASYNC_UNAVAILABLE`, no blocking `getline` fallback. Depends M7a; real endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | Same-action-continuation Pending resume, IO value admitted only when performed, Pending never an IO constructor, pure evaluation submits nothing, CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by a VLace expert reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vlace/`.                                                          |
| M7-VIcon                              | `match submit(cap, read_bytes(off,max)) { ... }`; `Driver::resume()` replaced by `DriverResult = Yielded{CellRef} or Parked{generatorCursor, FrontendToken} or Failed{IconError} or Finished`; stdout an explicit host write/flush chain. Depends M7a; real endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     | Generator multiplicity/backtracking with same-request Pending resume, Icon failure distinct from typed errors and EOF, payload-bearing variants alone expose bytes, pure presentation, CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by a Icon expert reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vicon/`.                      |
| M7-VAgent                             | `capacity Writer { effect def write(...) within 1s }` effect suspension statement; outbox staged after deliberation, immutable args journaled before admission; `EffectDone`/`EffectFailed` correlation carries managed outcome in a later owner event; actor suspension refused with `VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN`; local runner on interactive profile. Depends M7a and M3; real endpoints need the M4/M5 gate, otherwise fake backend.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | `EffectFailed` is an event not goal failure, no mutable `NativeBody(WorkerContext&)` on workers, actor completion is a later message, same-request Pending resume in local runner, CLI host I/O audited. Passes the M7a conformance harness. Gate: approved by the `vagent-expert` skill as language reviewer and a Vortex expert reviewer; reports under `design/projects/vortex-io-implementation-review/m7-vagent/`. |
| M8 — budgets/config/release           | Measure, set system config schema/defaults, docs and final migration. Depends M1–M6, M7a and all `M7-<lang>` evidence.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Pressure experiments report each charge class, system setting docs/schema/default/notes linked, all configured values resolved from one generation snapshot.                                                                                                                                                                                                                                                            |

Ordering: M1 → M2 → {M3, M4, M7a}; M3, M4 and M7a may proceed in parallel once M2 lands. M5 follows
M4 and M6 follows M5. The language milestones proceed in parallel after M7a; each is blocked from
completion until both its language expert reviewer and the Vortex expert reviewer approve.

M4 implementation notes and deliberate deferrals:

- Byte output never rides `IoResultValue` (which is `std::monostate` or `OpenedEndpoint` only):
  adapters publish produced bytes through `publishTransientBytes`, place the retained `PayloadRange`
  in the terminal outcome's payload field, and hand the `PayloadLease` to
  `CompletionSink::terminalCandidate`. Owner-side consumers resolve the range through
  `PayloadResolver` (or `outcomeCellPayloadRange` on the outcome cell).
- `ProjectionSession::publishRefresh`/`publishCommit` trust the caller to hand them the outcome
  their staged request produced; binding the outcome cell back to the staged spec cell needs
  outcome→request→specification readers the I/O cell layer does not yet expose, so cross-session
  outcome mix-ups are a caller-contract matter for M4 (revisit if a second staged consumer lands).
- `CommandMailboxAdapter` carries one bounded reply per command as a payload; streaming or
  multi-message command replies are not framed and stay deferred to the file/media milestone that
  needs them.
- `ProcessIoAdapter` cannot kill a `popen` child mid-read (`context.stop` is honored between `fread`
  chunks); `NetIoAdapter` serializes per fd because concurrent ops would interleave on one stream;
  the lens adapters' fetch generation is an adapter-local watermark, not the underlying store's own
  change detection.

M4 known gaps carried forward (recorded 2026-11-24, review fix round):

- `NetStreamRing`, `JournalLens` and `SwarmLens` have no `IoAdapter` yet; only the staged lens
  adapter, the file/process/net/stream sources, the union/router wrappers and
  `CommandMailboxAdapter` landed.
- `OwnerPublication` carries `IoOutcome` only: the `ProjectionDelta` half of the publish contract is
  unimplemented, so a lens commit's arena-side delta is not published through the outcome cell.
- `CommandMailboxAdapter` exists but its call sites have not migrated to request cells and owner
  publication; only the single bounded reply is framed (previous bullet).
- `retireRequest` has no production caller wired yet: await-driven requests retire only through it,
  so `requests_`/`requestByCell_` grow monotonically until shutdown without it (the same gap
  `ensureTerminalOutcome`'s re-mint-on-refusal note in `io_service.cpp` shares).
- `ensureTerminalOutcome` re-mints a refusal cell on every poll/await while `publishTerminal` keeps
  refusing, instead of caching one; request retirement bounds the growth but the re-mint itself is
  deliberate, not redesigned.
- `OpenSpec.deadline` is frozen on the request but nothing enforces it; no adapter or scheduler path
  reads it, so expiry never produces `TimedOut`.
- `UnionSourceAdapter` members must present disjoint staged-key spaces for concurrently in-flight
  requests: `abandonStaged` sees only the spec cell, so two live same-key specs on different members
  misroute the earlier abandon onto the other's live staged entry (documented on the class;
  `StagedLensSourceAdapter` members mint unique cells and satisfy it).

M5 implementation notes and deliberate deferrals:

- `ReadObservation` is a real minted cell (`IoCellTag::ReadObservation`), not adapter-local
  metadata: `FileSourceAdapter::execute` publishes the retained bytes first, then records the
  observation against the actual bytes-read extent, and `ArenaIoCellStore::publishTerminal` mints
  the cell off the outcome's `d.io.source` field when a `Complete` carries `ReadObservationResult`,
  binding the same `PayloadRange` through its own owner lease. `outcomeCellReadObservation` is the
  owner-side reader.
- File endpoints are served by `FileEndpointAdapter` (`fileOpenAdapterKey`/`fileDataAdapterKey`,
  `d.io.kind` "file"): `admitOpen` runs `::open` inside `execute` on the scheduler worker (§7.2's
  off-owner-thread rule) and adopts the descriptor in a `FileSourceAdapter` registered under a
  per-open `FileOpenId` (never per path: reopening a replaced path is a second, independent open),
  so every later op addresses the opened inode (`FileSourceIdentity::OpenedInode`, declared on
  `FileOpenCapabilities`). `makeFileRequestDecoder` freezes spec-cell text into `CellRequest`s;
  `stageAt` keys are the minted spec cells' refs (`StageKey`, the width of `OwnerCell`) and a live
  duplicate is refused. Each open's `FileOpenId` is recorded under the open request's private token
  when the descriptor is adopted, and the generic `IoAdapter::openSettled(token, opened)` hook
  (called by the service once the outcome mints the endpoint, or with nullopt when none was minted,
  which drops the open) binds it to the `EndpointCell`; `openIdOf(endpoint)` replaces the old
  `openIdsFor(locator).back()` guess, so concurrent opens of one path need no ordering.
  `IoService::closeEndpoint` reaches `release(FileOpenId)` through `makeFileCloseDecoder` (a Control
  request under `fileCloseAdapterKey`, frozen against the endpoint's initial capability) whose
  execute releases the open; the service already runs it after the endpoint's in-flight work, and
  in-flight operations hold the file by `shared_ptr`, so the descriptor closes when the last one
  lets go. A second close is refused `Conflict` by the service tombstone and `Stale` by the adapter
  once the binding is gone.
- Live/stable consistency is typed vocabulary, not adapter-private: `SourceConsistency` lives in
  `io_cells.hpp`, `ioConsistencyLive`/`ioConsistencyStable` mask
  `OpenSpec`/`OpenGrant::consistency`, and `FileSourceAdapter` offers `Live` only — a `Stable`
  request is refused before admission because a plain POSIX file proves no snapshot/CAS.
- §7.1's framing/message/cursor vocabulary is deferred wholesale until the consumer milestone that
  needs it: `BufferPolicy` (`Raw`/`Fixed`/`Line`/`Message`), `OversizePolicy`, `EncodingPolicy`, the
  `Sequential`/`AtOffset`/`Reset` cursor modes, and the `ReadFrame`/`ReceiveMessage`/
  `SendMessage`/`FlushAfter`/`Seek`/`DecodeRange` operations have no adapter or cell-schema
  representation yet. File/media M5 ships `ReadBytes`/`WriteBytes`-equivalent transfers only; §14's
  "unsupported framing/message semantics" refusal stands until then.
- `FileSourceAdapter`'s `DecodeLease` pool (`acquireDecodeLease`/`releaseDecodeLease`,
  `maxDecodeLeases` bound) is a placeholder accounting seam: no decoder or derived-product cache
  exists behind it, so leases record source range + generation but retain no decoded bytes. It
  exists so the lease/budget class is settled before a real decoder lands.
- `FileGeneration` (device, inode, size, mtime seconds and nanoseconds, compared field for field)
  detects an uncertain write but cannot see a same-size, same-timestamp in-place rewrite — proven by
  `SameSizeRewriteWithRestoredMtimeSharesTheGeneration`. That blind spot is why `Stable`/CAS is
  refused rather than synthesized from the generation value. The `uint64_t` the protocol carries is
  an id the adapter interns per exact value (bounded, oldest evicted; an evicted value that returns
  gets a new id, so an old expectation can only fail). A read samples the generation before and
  after the transfer and answers `Conflict` when they differ, publishing no bytes and recording no
  observation; `setAfterTransferHook` is the test seam for that window.

M6 implementation notes and deliberate deferrals:

- `CaptureCoordinator` (`capture_coordinator.{hpp,cpp}`) owns the per-user interval translation map
  (per-origin non-overlapping `Committed`/`Pending`/`Pinned` slots keyed by source offset), the
  `capture.journal` and a `CaptureAppendAdapter` (`captureAppendAdapterKey`, source kind 5). The
  append is an ordinary `IoService` request: `appendRequest` returns the common `SubmitResult`, and
  the receipt (the allocated `PrimediaSpan`) is held by the adapter and correlated by `commit`,
  never carried on the outcome's payload channel.
- Overlap is computed per origin as uncovered gaps; `[0,10)` then `[5,15)` appends only the
  `[10,15)` suffix, a fully covered selection appends zero bytes, and a discontiguous selection
  becomes separate pieces inside one append. Adjacency coalesces only when source and destination
  are both contiguous. The map key is the logical `PayloadRef` (kind/source/generation/occurrence);
  the BLAKE3 `appendDigest` in the journal is an integrity field only, and two identical byte
  strings from different sources never share a translation.
- `commit` follows the append request occurrence stored in the exact `CaptureCell`, checks its
  allocation incarnation, and requires `successfulAppend` to be linked from that request as a
  terminal `Complete`; another capture's outcome, a stale incarnation, a nonterminal request or a
  non-`Complete` outcome returns `Refused` with no journal record, cell or translation published.
- Journal records are `Append` (intent, written and fsynced by the append's own worker turn before
  the sink sees a byte, from version 4), `Appended` (span plus full translation intent, fsynced
  before the receipt cell is minted), `Committed` (after Structure publication and the Store save),
  `Aborted` (with an uncertain flag), from version 2 `Reconciled` (the owner's promote-or-release
  decision for a pinned capture) and, from version 3, `Identity`, always the first record: the
  journal's own random 64-bit identity plus the highest `CaptureId` minted so far. Versions 1 to 3
  are refused by number. `recover()` rebuilds translations from `Appended`/`Committed` (durable
  origins only; see the review-r1 bullets below), pins the source intervals of open intents and
  uncertain aborts (`NeedsReconciliation`, overlapping prepare then refuses with `Conflict`), and
  reports `orphanBytes` as the sum of append lengths that may have landed without a proven commit.
  It never resubmits. Corruption, bad signature/version and sequence gaps are `PersistenceRefusal`
  naming the 1-based record; a torn tail is cut by the constructor.
- Journal frame sizes are exported from `capture_journal.hpp` (`kCaptureJournal*Bytes`,
  `captureJournalFramedBytes`), and the coordinator derives its reservations from them; nothing
  hardcodes the format. Fallback bounds (`maxIntervals`, `maxAppendBytes`, `maxPieces`,
  `maxPendingCaptures`, `maxMapEntries`, `journalMaxBytes`, `completionBoundBytes`) live in
  `capture_constants.hpp` with rationale; M8 moves the configurable ones into `system://settings`
  and these become the schema defaults.
- Durability order (M6 review): `commit` calls `CaptureByteSink::sync()` (for the permascroll,
  `UserPermascroll::flush()`: write the tail to the active segment file and fsync) BEFORE it writes
  the `Appended` record, and refuses with `PersistenceRefusal` when the sink cannot sync.
  `recover()` reconciles every `Appended`/`Committed` span against
  `CaptureByteSink::highWaterBytes()` (the permascroll's size at restart): a span in another scroll,
  of the wrong length, or ending past the high-water mark is never a translation; it is pinned, its
  bytes counted as orphans, and reported as `SpanBeyondPermascroll`. Intervals that do not fit their
  span are `InvalidTranslation`; any overlap with an existing slot that is not an identical slot is
  `MapConflict`, inserted all-or-nothing.
- `prepare` refuses (`Stale`) until `recover()` has succeeded, and the id counter is seeded from the
  journal's maximum at construction, so an id is never reused. `recover()` is idempotent: it caches
  its report and later calls return it without touching the map, so it cannot re-insert inside slots
  that live commits coalesced. Zero-length and wrapping selections are refused up front, and no
  zero-length piece or slot is ever emitted.
- Journal budget is held, not just checked: `prepare` holds the capture's worst-case record set
  (`Append` + `Appended` + `Committed` + `Aborted`) against `written + held`, so N pending captures
  cannot jointly exhaust it; holds are consumed as records land and returned on retirement (the
  `Append` intent's when `commit()` sees the append `Complete`, since the worker writes it). A retry
  after a refused attempt tops the hold up before submitting.
- Terminal captures leave `pending_`: a committed capture frees its pending slot at once; a certain
  abort erases its `Pending` slots and releases its leases (the range can be retried immediately);
  an uncertain abort (and a swept capture whose append was requested) turns its slots `Pinned`
  in-process, matching what recovery would do after a restart, and releases its leases.
- Journal repair: a 1..11 byte file that is a prefix of our own header is a fresh journal (the first
  record is written with the header in one write, and the file and its directory are fsynced on
  creation); foreign bytes poison. A length prefix that claims more than remains is a torn tail only
  when the bytes present are the self-consistent start of the announced record (known kind, length
  valid for that kind, next expected sequence, interval count agreeing with the length); otherwise
  the journal is `Corrupt` naming the record and nothing is truncated.
- `translationFor(PayloadRange)` is the read-only query: the permanent span a range maps to when it
  lies wholly inside one committed translation, else `std::nullopt`.
- Journal write failures are logged (`vortex.io.capture`, record kind and refusal code, never
  content). `setJournalCrashPoints` threads `CaptureJournalCrashPoints` through the coordinator, and
  tests cover death before and after the `Appended` fsync.
- Crash injection is covered three ways: restart tests tear the whole world down after each durable
  step (intent, append executed, `Appended`, `Committed`, certain and uncertain abort); coordinator
  crash points bracket the `Appended` fsync; the journal's own hooks are exercised directly.
- Structure publication (`capture_publication.{hpp,cpp}`, `CapturePublisher`, attached with
  `setPublisher` before `recover()`). Using only the existing Store API (`makeDimension` through the
  dimension registry, `makeCell`, `makeScalarCell`, `setLink`), a capture publishes: a receipt cell
  that is a scalar whose canonical int64 bits are the `CaptureId`, chained along `d.io.capture`
  (headed by the dimension's own cell); per appended piece a cell whose content is the permanent
  permascroll span, on the receipt's `d.io.range` rank; and per piece a `d.io.provenance` chain of
  scalar cells (source kind, source, generation, occurrence, source offset). The receipt joins
  `d.io.capture` last, so a reachable receipt is a complete publication and a failure part-way
  leaves only unlinked cells. A capture wholly covered by earlier translations appends nothing and
  publishes a receipt with an empty range rank; the spans it reuses were published by the capture
  that appended them. No change to `store.hpp`, `ops.hpp`, `CompactOpNode`, `store.tables` or any
  OpKind was needed.
- Crash ordering (decision): `Appended` (after the permascroll sync, fsynced) -> publish into the
  Store -> Store snapshot written and fsynced (formerly `Store::save`) -> `Committed` -> in-process
  translations. `Committed` therefore proves the bytes, the references and their durability.
  Publication is idempotent keyed by `CaptureId` (a walk of the receipt rank, never a content hash).
  Death after `Appended` and before the save: recovery finds no receipt, publishes once, saves,
  writes `Committed`. Death after the save and before `Committed`: recovery finds the receipt, mints
  nothing, writes `Committed`. A `Committed` capture whose Store lost its receipt is republished the
  same way. A refused publish or save fails `commit` with `PersistenceRefusal` and leaves the
  capture retriable; the retry cannot double-publish. If recovery cannot finish a publication the
  entry reports `PublicationIncomplete`, translations are rebuilt, nothing is pinned, and
  `resumePublications()` retries. Publication runs on `store.latest()`, so the Store must be
  single-writer while a coordinator uses it. With no publisher attached a `Committed` record proves
  only the journal and the map, as before.
- Explicit reconciliation: `reconcile(CaptureId, ReconcileRelease | ReconcilePromote{span?})`
  resolves a pinned capture (open intent, uncertain abort, `SpanBeyondPermascroll`, ambiguous
  order). Promote never searches for bytes: the owner names the span (or the journal's `Appended`
  span is used), and the coordinator verifies scroll, length, the permascroll high-water mark and
  the append digest of the bytes read back through `CaptureByteSink::read` before anything changes;
  a failed check is `Conflict` and the pin stays. The decision is journaled (`Reconciled`) before it
  is acted on, so a restart replays it: Release unpins and leaves any landed bytes orphaned, Promote
  proceeds like an `Appended` record (translations, publication, `Committed`), including after a
  restart between `Reconciled` and `Committed`.
- The real sinks (`UserPermascrollCaptureSink`, `SegmentedPrimediaCaptureSink`) are tested against
  real spools in a temp directory: `sync()` is what moves bytes into the segment file,
  `highWaterBytes()` is the size after reopen, and an end-to-end capture runs through the real
  permascroll and Store save/reload.
- Review r1 (fixer B) corrections to the notes above:
  - A waiter's `d.io.request` link and the capture's append-request link both reach the request
    through a clone view on the request's `d.io.clone` rank (`attachClonedField`, the one
    implementation every field uses), so the request is never a fan-in target and an await cannot
    evict the capture's link. A retired request is taken out of its endpoint's queue and clone ranks
    (`IoCellStore::reapRequest`, called when the service drops the request's state), so the ranks
    every submit walks are as long as the live requests, not every request ever made.
  - A `CaptureId` is unique only within one journal. The journal's `Identity` record names it, a
    Store receipt carries the identity as a scalar on its `d.io.provenance` rank, and publication
    idempotence is keyed by `(journalId, CaptureId)`; the same key naming different pieces is a
    `PersistenceRefusal`, not a skipped publish. A receipt or provenance chain the reader cannot
    parse is a `PersistenceRefusal` too, never a default of zero.
  - Only a durable origin has an identity that outlives its process. `Transient`, `Scratch` and
    `Proxy` ids restart with every process, so recovery validates their records, reports their
    orphan bytes and publishes their spans, but neither rebuilds their translations nor pins their
    intervals: a later session's unrelated source 1 must not inherit the earlier one's bytes or its
    pin. Their Store provenance records the process-local numbers as diagnostics, together with the
    journal identity; it is not a locator, and no locator path from a `PayloadRange` to a source
    exists yet.
  - `translations_` is an in-process dedup cache over journal-proven slots, not a second source of
    truth: the Store's receipt and range ranks are the published fact, the journal is what rebuilds
    the cache, and a lost cache costs a repeated append, never a wrong reference.
  - Nothing is reaped by hand. Before a map or journal bound refuses, the coordinator forgets
    committed translations of origins nothing can read any more (`PayloadResolver::sourceReachable`)
    and rewrites the journal (`CaptureJournal::compact`, atomic temp file, fsync, rename, directory
    sync) without the whole records of captures that are resolved and published, keeping what is
    unresolved (live, pinned, awaiting publication) and what survives a restart (a durable-origin
    capture). The `Identity` record carries the id floor, so ids are never reused. A long run past
    the old ceilings is the test `CaptureStateIsReapedSoALongRunPastTheOldCeilingsWorks`. A durable
    origin never dies, so its captures retire by eviction instead (review r2, R2-1): when the map or
    the journal bound would refuse and nothing resolved is left to drop, the coordinator evicts
    committed translations least recently hit first (a batch of one eighth of `maxMapEntries`, never
    one the current `prepare` covers) and the next compaction drops the records of every durable
    capture no committed translation still backs. A durable capture's records are therefore kept
    only while their translation is, and the cost of an eviction is one repeated append of those
    bytes, never a wrong reference. `stateCounts()` reports live slots, retained captures and what
    was evicted, and a journal refusal logs them. The test is
    `DurableCaptureStateIsEvictedSoALongRunPastBothBoundsWorks`.
  - Zero is no longer "unbounded": `CaptureJournal`, `CaptureCoordinator` and
    `BackendBudget::maxBytes` take `std::optional`, and 0 holds nothing. A zero-extended crash tail
    is cut like any other tear (both journals share the scan).
  - **P3 resolved the §14 deviation for the commit** (details and tests in
    [`p3-implementation.md`](projects/vortex-io-implementation-review/m1-m6/p3-implementation.md)).
    `commit()` no longer syncs, fsyncs or saves on the owner. It validates and submits a
    `CaptureDurable` request (same adapter key as the append) and returns `CommitPending`; the
    permascroll sync and the fsynced `Appended` record run on a scheduler worker; the owner's
    `continueCommit()` then publishes into the Store in memory and takes a `StoreSnapshot` in O(new
    operations) (`Store::saveSnapshot`); a second worker step writes and fsyncs the snapshot
    (`SnapshotWriter`, through `SegmentedOpsSpool::writeSegmentRange`, the one writer of
    `ops.nodes`) and the fsynced `Committed` record; a last `continueCommit()` commits the
    translations. The durable order on disk is unchanged: `Appended` after the sink sync, then the
    save, then `Committed`. A step that threw (a crash point) is rethrown by `continueCommit()`; a
    step that failed refuses it and leaves the capture retriable.
  - What still blocks the owner, by design: the `Aborted` records of the failure paths; journal
    compaction under bound pressure, which takes the journal mutex a worker holds across its fsync
    and so _may wait on a worker_, not only do an fsync itself; and `recover()`, `reconcile()` and
    `resumePublications()`, which run at open or on an explicit owner decision with nothing else in
    flight and save through the same snapshot writer on the calling thread. None of these is on a
    commit's happy path. (`appendRequest()` is no longer on this list: see P5 below.)
  - Review round 3 corrected P3 in four places (`r3-disposition.md`): journal compaction reads,
    chooses and rewrites in one hold of the journal mutex (`CaptureJournal::compactWhere`), so a
    worker's `Appended` or `Committed` cannot land between the read and the rename; the snapshot
    writer tracks what is fsynced apart from what is written, so a `Fsync` write after a `Buffered`
    save still fsyncs `ops.nodes` and `store.tables` before `Committed` claims them; a `Replace` of
    `ops.nodes` is written beside the file and renamed over it, never truncated in place; and one
    writer mutex per directory identity, not per `Store` object. A retired capture's journal hold
    outlives its running step, and a capture that dies mid-step retires the step's request.
  - **P4, applied** (owner-approved core change; details, tests and counts in
    [`p4-p5-implementation.md`](projects/vortex-io-implementation-review/m1-m6/p4-p5-implementation.md)).
    `ArenaManifold::freeCell(ref, incarnation)` frees one cell onto a LIFO free list that the next
    mint reuses under a new incarnation; `reapIoRequest` frees a reaped request's whole graph (the
    request, its field occurrences and views, its outcome and what the outcome carries), so an arena
    that serves requests holds its live requests' cells and not every request's (measured at 18 live
    cells, about 1.2 KB, per served request: six times the earlier price of three cells). A request
    that a capture still views on its clone rank is kept until the view leaves. Retiring is the
    consumer saying the outcome is read: a late poll of a reaped request finds a stale handle. The
    first measurement counted only requests that never parked. A parked await also mints a waiter
    graph (about nine cells: the waiter, its request occurrence and clone view, and three label and
    value pairs), which `reapIoWaiter` now frees when the waiter retires, whether by cancel, by the
    terminal pass or at resume (M7a review R1, tested with ten thousand await cycles); a cell that
    other cells still view through its clone rank is kept whole, with its subgraph.
  - **P5, applied** (owner-approved journal-protocol change; `capture.journal` version 4). The
    `Append` intent is written and fsynced by the append's own worker turn, after the turn's last
    chance to be cancelled and before it hands a byte to the sink, so no journal fsync of a commit
    transaction runs on the owner except the `Aborted` records of the failure paths. An admission
    refusal leaves no record at all (there is no intent to close); an `Aborted` may stand with no
    intent before it and recovery reads that as nothing having landed, while an intent that lands
    after an owner `Aborted` (a worker that raced the owner) opens an attempt recovery pins. The
    intent's journal budget stays held until `commit()` sees the append `Complete`.
- Remaining, deliberately: moving the `capture_defaults` bounds into `system://settings` (M8); there
  is no UI or keymap command invoking `reconcile` yet (it is an owner API); an ambiguous journal
  order is reconcilable only by the same promote/release decisions, with no tool that explains why
  it was ambiguous; a commit's snapshot copies and writes only the operations the directory lacks,
  but `store.tables` is rewritten whole each time (it is O(scrolls), not O(operations)), and the
  first snapshot into a fresh directory, or the first after a refused write, copies the whole store
  on the owner (about 64 MB per million operations, a memcpy and no I/O); and source chunks are not
  pinned by the map, so a capture of a transient origin whose last lease dropped is refused
  `Unresolved` by design (the tests hold a standing lease).

M7a implementation notes and deliberate deferrals:

- What was built, and where. `frontend_diagnostics.{hpp,cpp}` is the shared diagnostic-code
  registry; `frontend_token.hpp` carries `FrontendRunResult<V>`, `FrontendToken`, `FrontendWake` and
  the `FrontendRunner` interface; `frontend_bridge.{hpp,cpp}` is `FrontendBridge` (submit, match,
  await, resume, status gate) and `auditLanguageValue`. `IoHost` gains `routeWake` and
  `takeResumedOutcome`; `io_cells` gains `IoCellTag::Submission` and the submission-cell accessors.
  `VortexCore`, `VortexVM`, `CompactOpNode`, `Store` and the manifolds are untouched. These are C++
  because they are the host seam: the token is owner-thread native state (cursor, waiter,
  continuation), `pumpIo` is the only place that knows a request went terminal, and the sum cell has
  to be minted with an arena runtime tag. None of it is language policy, which stays in each
  language (and, for its standard library, in Vortex).
- `IoSubmissionValue` is a cell, not a C++ value. A `Submission`-tagged runtime-only cell whose arm
  is the graph, not a stored flag: `Admitted` is a `d.io.request` field and `Refused` a
  `d.io.outcome` field, each through an occurrence and a clone view like every other reference to a
  request or outcome (`attachClonedField`), and a cell with neither or both is `MalformedGraph`.
  `submissionRequest`/`submissionOutcome` are exclusive: the wrong arm answers `WrongTag`, which is
  how an await is never handed a refusal. The cell text is `admitted` or `refused`. The sum holds a
  view on the request's clone rank, so `reapIoRequest` keeps the request whole while a submission
  cell names it. The order does not leak any more: `FrontendBridge::release` reaps a request the
  service had already retired once the view is gone, and `FrontendBridge::retire(submission)`
  releases the sum and retires the request when it is terminal (it never cancels a running one).
- `FrontendToken` holds the activation as `shared_ptr<FrontendActivation>` (an opaque base the
  language subclasses), the request and waiter handles, the `ContinuationId` and an optional
  position. The `Parked` result a runner returns and the route the host keeps share that activation;
  neither is a cell. `IoHost::routeWake(token, runner)` accepts only a token matching a parked await
  (request, waiter and continuation), keeps one route per continuation, and `pumpIo` hands the token
  back to the runner as `Resumable` after it has resumed the VM cursor, or `Abandoned` when the
  cursor or request went stale. Wakes are delivered after the pass, so a runner may re-await and
  park again from inside `wake` (VPL's next position) but must not call `pumpIo`. `cancelWaiter`
  that removes the waiter drops the route silently: cancelling an activation never cancels the
  request. `~IoHost` drops remaining routes without calling anyone. A route holds the runner's
  `routeLife()` token (the `FrontendRunner` base owns it), which `pumpIo` re-checks before every
  delivery, so a runner destroyed without cancelling its waiter, or by another runner's wake in the
  same pass, is skipped and never called; `unrouteWake(continuation)` and `unrouteRunner(runner)`
  drop routes explicitly. A `wake` that throws is contained per runner: it is logged, counted
  (`failedWakes()`), its VM continuation is retired and the rest of the batch is still delivered.
  Routing a parked await again with the same runner and activation is accepted; another runner or
  activation is `AlreadyRouted`, which the bridge returns as `IO_TOKEN_STALE` instead of parking
  with the second activation.
- `FrontendBridge::awaitSubmission` is `Completed{outcome}` for any terminal tag (a terminal outcome
  is a value, never a failure), `Parked{token}` for Pending, and `Failed{IO_AWAIT_NOT_ADMITTED}` for
  a refused submission: refusal stays a value, and a language matches before awaiting. A second
  await of the same request and cursor while Pending returns the same continuation and keeps the
  first route (for the same runner and activation; see above for any other). `resume` takes the
  outcome `pumpIo` delivered (`IoHost::takeResumedOutcome`, which retires the VM's parked entry) and
  otherwise re-awaits the same request, rematerializing a rolled-back outcome cell; it never
  submits.
- Status gate: `gate()` returns the tag and a payload range only for `Complete`, `Partial`,
  `Failed`, `Cancelled` and `TimedOut`, and only when bytes were retained; `EndOfStream` and every
  other tag expose none whatever the graph holds, and `payloadOf()` answers
  `IO_PAYLOAD_BEFORE_STATUS` when there is nothing to read. The static "diagnosed where statically
  possible" half is each language's.
- Diagnostic registry: a code is a stable `[A-Z][A-Z0-9_]*` string owned by one frontend and
  carrying its prefix (`IO_`, `VQL_`, `VPL_`, `PROLOG_`, `VLACE_`, `VICON_`, `VAGENT_`); duplicates,
  wrong prefixes and unregistered codes are refused. Built in: the shared `IO_UNSUPPORTED_MODE`,
  `IO_AWAIT_NOT_ADMITTED`, `IO_PAYLOAD_BEFORE_STATUS`, `IO_EFFECT_FORBIDDEN_CONTEXT`,
  `IO_SYNC_FALLBACK_FORBIDDEN`, `IO_TOKEN_STALE`, `IO_CELL_REFUSED`, and the language codes the plan
  names (`VQL_IO_QUERY_CONTEXT`, `VPL_IO_SHAPE_MISMATCH`, `PROLOG_IO_EFFECT_CONTEXT`,
  `VLACE_ASYNC_UNAVAILABLE`, `VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN`). Each language milestone registers
  any further code of its own (`registerCode`) before it emits one.
- The harness (`tests/xuzz/io_frontend_conformance.hpp`). A language writes a `ConformanceDriver`
  whose `begin` runs its real submit-match-await program once through its real runner and returns a
  `FrontendRunResult<OutcomeCell>`, whose `resume` continues the same activation from a woken token
  without submitting, and which reports `visibleCells()` (every cell the language holds) and its
  `submission()`. One line, `XUZZ_FRONTEND_CONFORMANCE(Suite, factory)`, expands to five tests over
  the M1 delayed fake backend: same-request Pending resume (one submission, one wake, same
  continuation and request, outcome equal to the service's), refusal remains a value (`Rejected`
  outcome, nothing admitted, awaiting it is refused), terminal outcomes are not logical failure
  (eight outcome shapes through the status gate), no raw ids in language values (the audit passes on
  the language's cells and fails on the owner-only waiter graph, so it can fail), and a cancelled
  waiter drops its route but not its request. A driver must use only `World` (service, scheduler,
  VM, host, bridge) and its own language; it never sees a `RequestId`, `WaiterId` or
  `ContinuationId` except inside the token it returns.
- The native-id audit is label and tag based, not a type walk, because cells carry text and tags and
  nothing else: it fails on an owner-only `Waiter` or `Capture` cell, on text naming a native
  identity (`continuation`, `waiter`, `cursor`, `token`, `requestid`, `incarnation`), and on text
  equal to, or a scalar value (integer or integral real) equal to, a raw id the test passes in,
  under any label. The harness passes the fake backend's private request tokens (which start at
  `0xC0FFEE00` so they cannot collide with a counter) and the parked token's `ContinuationId`,
  waiter cell and request cell, and plants each of them as a text cell and as a scalar cell behind a
  benign label to prove the audit can fail. A language that invents a transformed encoding of an id
  (hex, a prefix) would be caught only by the label rule.
- Layering is a source test, not a promise: `vortex_core.*` and `vortex_vm.*` include and name none
  of the frontend headers or types, the `stepAwait`..`stepImpl` text contains no language name, and
  the host/bridge files name no language runner. The VM's pre-existing `Vql*` opcodes are not I/O
  and are out of that scan by construction.
- Two harness instances run here: `DirectVortexDriver` (a real `Await` opcode program on a VM
  cursor, the plan's shared-host baseline) and `BridgeAwaitDriver` (a runner with no Await opcode,
  awaiting through the bridge, which is the shape VLace's IO trampoline will take).

Deferred, by design:

- Resolved by P6: the opcode is `OpcodeKind::Submit` (see "P6: the Submit opcode" after the M7-VQL
  notes). Until then a Vortex program reached Await only with a request cell a host already held;
  the baseline's `Submit` is still `IoHost::submitCell` wrapped by `FrontendBridge::submit`, which a
  language host calls from the VM's submit hook.
- `FrontendRunResult` is a template over the language's value; the plan's per-language result types
  (`Parked{scriptActivation, ...}`, `DriverResult`, `IoRunResult`) are each language's own enums
  that carry or convert to the common `FrontendToken`. Their mapping is the language milestone's.
- A parked token has no timeout or watchdog: if a runner is destroyed while its route exists the
  host would call a dangling runner. A runner must cancel its waiter (`IoHost::cancelWaiter`) before
  it dies; making routes self-expiring needs a lifetime handle and waits for the first runner that
  cannot guarantee it.
- Admission refusal is exercised through a decoder refusal. The actor-profile refusal and the
  per-language static refusals (`VQL_IO_QUERY_CONTEXT` and the rest) are registered but emitted only
  by their language milestones.
- Vectorized (VPL) parking with out-of-order completion is a language behavior: the host gives one
  token per active waiter and in-place outcome cells, and the harness proves only the single-request
  contract; the multi-position scan is `M7-VPL`'s test.

M7-VQL implementation notes and deliberate deferrals:

- Grammar. A new top-level statement, `IoEffectStatement`, spelled
  `effect { let $s := submit($cap, read_bytes(off, max)); match $s { ... } }` with the arms
  `admitted($q) => await($q)` and `refused($e) => $e` (in either order, both required, each arm
  yielding the outcome). The capability is a `$variable`; the offset and bound are integer literals
  or `$variables`. It is a `Statement` alternative and not an `ActionClause`, which is how it stays
  out of row traversal: the parser refuses it, with `VQL_IO_QUERY_CONTEXT`, as the action of a
  `for`/`let`/`where`/`if` block and inside a def, lambda, `watch` or `with perspective` body (a
  flag set only for the top-level script body), and refuses `submit`, `await` and `read_bytes` as a
  call name in any expression (those three are now reserved). `effect` itself is contextual (an
  identifier followed by `{`), so no existing query changes meaning. Grammar and semantics are in
  `vql-query-language.md` §4.13 and change-history entry 16.2.
- Execution design (revised by P6; the first cut had no `Submit` opcode and so cut the script at
  each action). The compiler lowers an action to bytecode (`compile_io_effect.cpp`): `MakeTerm`
  builds the request description `read_bytes(offset, max)` as cells, `Submit` admits it, a
  `BranchFalseCell` on the submission's arm ordinal selects `refused($e) => $e` (a `CopyCellRefs` of
  the Rejected outcome) or `admitted($q) => await($q)` (`Await` of the request), and both arms write
  one result slot. The whole script is one program. `ScriptRunner`
  (`apps/common/xanadu/vql/script_runner.{hpp,cpp}`) is the host loop around it: it runs the program
  through `QueryExecutor::begin`, and when the VM stops at an unfinished Await it routes the
  `FrontendToken` and returns `Parked`; `resume` runs the same cursor on from that Await
  (`QueryExecutor::resume`), so the Submit before it never executes twice. The runner answers the
  VM's submit hook (decode the description, stage the read, admit through `FrontendBridge`) and
  reads a finished action's payload text, which lives outside the arena. A parked action returns
  `Parked{FrontendToken}` whose `activation` is the suspended script (plan: `scriptActivation`),
  `position` the action's statement index (`effectAction`, found from the compiled site table) and
  `request` the request cell (`requestCell`). The plan's "`ExecutionBlock` result gains
  `Completed{rows}`/`Parked{...}`" is realised as `ScriptRunResult = FrontendRunResult<rows>` over
  the whole script, since an action is a statement and not a block. `QueryExecutor::execute` is
  synchronous and cannot wait, so it refuses a script holding an action with
  `VQL_IO_EFFECT_NEEDS_RUNNER` (replacing `VQL_IO_EFFECT_NOT_COMPILABLE`); `VQLCompiler::compile`,
  `vqueryc` and `:asm` compile it. A script's actions are pre-flighted for capability-ness and
  non-negative integer operands before anything compiles, so a bad later action then fails the
  script before an earlier statement runs; the staged-read bounds are checked at stage time (r1).
- Values. The script's result is its final executed statement's rows, as before. An action's value
  is the outcome cell, then one text cell with the retained bytes only when the shared status gate
  exposes a payload. A refusal is a `Rejected` outcome row (nothing started) and the script goes on;
  `EndOfStream`, `Failed`, `Cancelled`, `TimedOut`, `Unknown` and the rest are outcome rows. The
  only failures are the script's own: a stretch that fails (`VQL_SCRIPT_FAILED`, position =
  statement index) or an unusable capability, offset or bound (`VQL_IO_EFFECT_ARGUMENT`, raised
  before any read is staged or submitted). Three codes were added to the built-in registry.
- Real reads. `FileReadHost` (`vortex/file_read_host.{hpp,cpp}`) assembles the M4/M5 stack
  (`FileEndpointAdapter`, `ArenaIoCellStore`, `BoundedIoScheduler`, `IoService`, `IoHost`,
  `FrontendBridge`) over one arena and VM, opens a file asynchronously (read-only grant) and stages
  a bounded read as a request specification; it is language-neutral so later frontends reuse it.
  `fileScriptPorts` is the VQL adapter over it.
- CLI audit. `vquery`: before this milestone it had no I/O at all (every query ran synchronously on
  the VM and no `IoHost` existed). Now `--io-file NAME=PATH` and `:open NAME PATH` open a file
  through `beginOpen`, polled with `openedCapability` between non-blocking `pumpIo` calls; a script
  holding an action runs through `ScriptRunner` and `driveToCompletion`, which pumps, takes the
  wake, resumes the same action and, when a pump woke nothing, rests for a named 1 ms interval. No
  `read`, `fread` or `getline` of file data happens on the evaluation thread (the `getline` in the
  REPL reads the user's own line input, as it always has); the scheduler's workers `pread`. There is
  no synchronous fallback: a query path that cannot park is refused, not run blocking. One
  `VortexVM` now serves the whole process because the core has a single write guard that a second VM
  would null on destruction. `vqueryc` is compile-only and opens no file or endpoint; it compiles an
  action (a capability variable it has no binding for becomes a text cell naming it, which no Submit
  accepts, so running that program unbound fails by value). The host is built lazily, so a plain
  query starts no worker. The persistence paths were audited in gate r1: `-o`, `--in-place` and
  `:save` would have written an action's payload text into the permascroll with no capture decision,
  so they are refused with `VQL_IO_PERSIST_NEEDS_CAPTURE` for rows of a script ending in an action
  (e2e `ActionRowsAreNotWrittenByOutputStoreInPlaceOrSave`); `:open` takes a path with spaces; the
  open still has no deadline or cancel, and `lastResults` after a script that fails post-admission
  may name retired cells (deferred).
- Harness. `ConformanceDriver` gained `collectWakes()` (default empty) and `pumpDriver`, because a
  language whose runner is not the driver itself must forward the wakes its runner received; the M7a
  drivers are unchanged. `VqlScriptConformance` instantiates all five checks with a driver that runs
  the real parser and runner. The runner keeps the latest `$submission` sum until the next action or
  its destruction, which is what the harness needs to prove a refusal cannot be awaited. It also
  retires the replaced action's request (through `FrontendBridge::retire`) so a session holds one
  finished action's request, not every one; the rows an action returned stay readable until the next
  action or the runner's destruction, which is why `vquery` keeps one runner for the session and
  prints before the next script runs. A request still running when the runner dies is never
  cancelled, and neither is one an activation of the same runner still awaits.

Deferred, by design:

- Matching on an outcome's tag or fields inside the script (`outcome(complete($payload, ..), ..)` of
  the examples document): the script sees the outcome cell and the gated payload text, and the host
  inspects the tag. Binding the action's result to a variable for later statements is also absent,
  so statements after an action cannot name its value.
- Gate r1 (round 1 of the M7-VQL review; `m7-vql/r1-disposition.md`): the runner keeps
  per-activation submissions (several scripts may park together, resumed in any order; finished
  scripts' rows stay readable until the next admission by any activation; a failed, abandoned or
  backtracked action is accounted for, a running orphan never cancelled); a second runner on a VM is
  refused; `vquery` refuses `-o`, `--in-place` and `:save` of action rows with
  `VQL_IO_PERSIST_NEEDS_CAPTURE` until a capture command exists (deferred); payload read failures
  and non-UTF-8 bytes are `VQL_SCRIPT_FAILED`. **Compiled programs are freed (P7):** a run that
  ends, fails or is abandoned detaches and frees its own cells with the shared cell deletion (Repair
  mode), except the I/O service's cells and the rows; `ManyEffectScriptRunsLeaveTheArenaBounded`
  pins 10^4 runs (`m7-vql/p7-cell-deletion.md`).
- Gate r2 (`m7-vql/r2-disposition.md`): a run whose dynamic-view registry grew (a pivot, a lens, a
  shape value) keeps its compiled program, so the registry never names freed cells; a rollback
  notice is routed to the activation that owns the Submit; `IoService::orphanRequest` is typed
  (`OrphanResult`) and counted, and an orphaned open mints no endpoint; deleting a clone master with
  views is refused unless the caller chooses promotion (`CloneGuard`, `ArenaRefusal::HoldsViews`).
- Retiring the request: the outcome cell is the script's value, so a finished script's request is
  retired through `FrontendBridge::retire` only when the next action is admitted (or the runner
  ends), never earlier.
- Write, capture, `message` and frame modes, `stable` reads, and any action other than a bounded
  read of one file capability; reads over a loop or per row (refused until an explicit later gate).
- The runner needs a `VQLEngine` session (it supplies the bound capability and operand cells at
  compile time and the host-side payload read). The whole script is one program, so a statement
  after an action sees the earlier ones' bindings as it would without an action; an action's own
  value is still not nameable by later statements.
- No overall timeout on a parked script: it waits for the backend's own deadline (open has one; a
  read has none yet), and cancelling a request stays a separate explicit action.
- `submit`, `await` and `read_bytes` are reserved call names; a user `def` of those names can no
  longer be called.

P6: the Submit opcode (applied with owner approval, `vortex_vm.{hpp,cpp}` only):

- `OpcodeKind::Submit`, appended after `Await`. Inputs: a CapabilityCell and a request description
  cell (cells built by bytecode, no serialised text). Outputs: the managed submission sum cell, its
  arm ordinal as an Int64 (0 the admitted arm, 1 the refused arm) and the arm's payload cell (the
  RequestCell or the Rejected OutcomeCell). It is dispatched from `step()` beside Await
  (`stepSubmit`), because it needs a cursor and a host.
- Host hook: `VortexVM::setSubmitHandler`, a function from (capability, description cell, cursor) to
  `VmSubmission`, mirroring `setAwaitHandler`. The VM names no I/O service, language or schema:
  `VmSubmission` carries the sum cell, the arm ordinal and the payload cell already decoded by the
  host. The host answers admission refusal as a value (arm 1), never by throwing.
- Failures are typed step failures that advance nothing and publish nothing: no handler, a wrong
  operand count, an input that is not a live CapabilityCell, an active mutation group (a Submit is
  an effect on the world and cannot be rolled back), a host that throws (contained, as for Await),
  and a returned sum that is stale or not a Submission cell. The mesh evaluator and the artifact
  decoder treat it as they do Await: not provably pure, and a valid ordinal.
- Once per executed instruction. A parked Await resumes at the Await, after the Submit, so a resume
  never re-executes it. Backtracking does not recall a request: `Fail` restores arena cells and
  choice state, but the admitted request is an effect in the service, so executing a Submit again
  after backtracking (or twice in a loop) creates a new operation, as the plan's multiplicity rule
  says. The abandoned submission cells go with the arena rollback, and the request cannot be left
  naming cells that no longer exist (M7-VQL gate r1, vortex M1: each pass of a retry loop leaked an
  admitted request until the budget of 16 refused everything). Chosen semantics: **the host is told
  before the cells go.** `VortexVM::setSubmitRollbackHandler` is called once per Submit admitted
  since the rolled-back choice point (backtracking, a scoped abort, a failed mutation group), newest
  first, while the sum, request and payload cells still exist; the VM only passes the `VmSubmission`
  it already holds and guarantees the order. The host decides. `ScriptRunner` orphans the request
  (`IoHost::orphanRequest` -> `IoService::orphanRequest`): it is **not cancelled and not recalled**
  (backtracking does not undo a world effect), it runs to its own terminal, and the service then
  drops its state without publishing an outcome cell (nothing can name it) and frees its budget.
  Refused alternatives: refusing Submit under a live older choice point (contradicts "a new
  execution creates a new operation, including after backtracking"); keeping the cells outside the
  trail (the arena has no such region and the rank links of the endpoint queue are
  trail-journalled); cancelling on rollback (a recall, wrong for any future write). The VM also
  refuses an arm ordinal above 1.
- **Gate passed.** The M7-VQL gate converged in three rounds: round 3 (`183129f5`) approved with
  zero BLOCKING and zero MAJOR from both reviewers. The round table, reports and dispositions are in
  [`m7-vql/README.md`](projects/vortex-io-implementation-review/m7-vql/README.md); the polish of
  round 3's minors is in `m7-vql/r3-disposition.md`.
- Pattern matching reuses existing opcodes: `BranchFalseCell` on the arm ordinal selects the arm,
  `Await` takes the payload. No second opcode was needed.

Every milestone has unit and integration tests in the owning layer. Keep tests headless per
repository policy. Run source-level focused tests while implementing and full `make -j$(nproc) test`
before release; backend comparison only if graphics code is touched. Do not claim performance
improvements without measured workload. No milestone may mark “async” solely because it returns a
future or because the VM can yield.

## 14. Explicit first-release refusals

The first release refuses: spill storage; implicit capture; inferred file snapshots; atomic file CAS
where backend lacks it; unsupported framing/message semantics; oversized message truncation
presented as complete; unbounded streams or capture maps; silently mixed raw/frame cursor modes;
media whole-file materialization; automatic retry after unknown; actor Submit/Await in atomic turn;
statically effectful VQL filters/projections; Prolog speculative effects; VPL shape mismatch; native
handle deserialization; transient serialization; and any path that blocks the evaluation/UI thread.
Refusal is a typed outcome or compiler diagnostic with no external work started.

## 15. Implementation review record

Independent reviewers should review this frozen plan by role: system architecture/layering, Vortex
execution/continuation semantics, ZigZag schema/identity/lifetime, language usability across six
frontends, and technical writing/completeness. Each reviewer writes only its own report under
`design/projects/vortex-io-implementation-plan-review/`; the author records each finding and
resolution in `dispositions.md`. Review target is the plan's SHA-256 in the review README. Reviewers
must check exact API ownership/lifetime, milestone dependencies, source-backed integration points,
and each refusal/gate. A design-level unresolved BLOCKING/MAJOR finding returns to the author for a
revision round; no source implementation begins from an unreviewed open question.
