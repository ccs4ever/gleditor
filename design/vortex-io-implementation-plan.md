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
| Cell schema        | `d.io.*` dimensions; `IoCellTag`; `IoOutcomeTag`                                                                           | Managed graph authoritative; private service metadata only accelerates                            | M0 topology/persistence-refusal tests                                                    |
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

| Cell value       | Authoritative graph fields                                                                                     | Native sidecar role                                         |
| ---------------- | -------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- |
| Endpoint         | kind, generation, state, supported operation/consistency capabilities, read/write queue occurrences            | OS/backend handle, current adapter binding                  |
| Capability       | endpoint occurrence, endpoint generation, rights, byte/concurrency limits, consistency requirements            | Revocation/authority proof; never reconstructed from fields |
| Request          | operation, immutable arguments, capability occurrence, endpoint generation, state, progress, outcome, deadline | Scheduling/index data and native cancellation token         |
| Waiter           | request occurrence, owner/cursor registration, continuation reference, active state                            | Wake queue token resolved to one owner continuation         |
| Payload          | source generation/occurrence, offset, length, provenance, lease association                                    | Chunk pointers and physical lease reference                 |
| Outcome/Progress | outcome tag, typed error/cause, progress units, optional payload/result occurrence                             | Rebuildable cache only                                      |
| Capture          | selected source intervals, map reservation, append request occurrence, phase                                   | Journal sequence and recovery index                         |

`OpenedEndpoint` contains only owner-created endpoint/capability cell references.
`CaptureAppendReceipt`, `ProbeValue`, and `CommitReceipt` contain typed managed fields. Logical cell
links and tags are canonical; native indices can be discarded and rebuilt from those fields plus
validated incarnations.

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
Cell links/tags are semantic state; native sidecars may index them but must be rebuildable/validated
against incarnation.

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

`IoCellStore` is the owner-only arena bridge held by IoService. Its mint methods create
authoritative managed request/outcome/endpoint/capability graph values; waiters are owner-only
transient cells. `mintWaiter(RequestCell, OwnerWaiterRegistration)` and `retireWaiter(WaiterCell)`
are owner-only. `IoService::await` serializes with owner `pump`, checks terminal state and
registers/mints within one critical section; duplicate
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

The new private durable capture journal is `capture.journal`, signature `\x89XUDCAP`, version 1,
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
| M6 — capture                          | Per-user overlap map, capture coordinator and v1 journal. Depends M0/M1/M5.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            | \[0,10) then \[5,15) reuses overlap; discontiguous split; mismatched append request/outcome refuses publication; failures injected after each durable step; restart reconciles; no hash identity; orphan bytes handled explicitly.                                                                                                                                                                                      |
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
  binding the same `PayloadRange` through its own owner lease.
  `outcomeCellReadObservation` is the owner-side reader.
- File endpoints are served by `FileEndpointAdapter` (`fileOpenAdapterKey`/`fileDataAdapterKey`,
  `d.io.kind` "file"): `admitOpen` runs `::open` inside `execute` on the scheduler worker (§7.2's
  off-owner-thread rule) and adopts the descriptor in a per-locator `FileSourceAdapter`, so every
  later op addresses the opened inode (`FileSourceIdentity::OpenedInode`, declared on
  `FileOpenCapabilities`). `makeFileRequestDecoder` freezes spec-cell text into `CellRequest`s;
  `stageAt` keys are the minted spec cells' refs.
- Live/stable consistency is typed vocabulary, not adapter-private: `SourceConsistency` lives in
  `io_cells.hpp`, `ioConsistencyLive`/`ioConsistencyStable` mask
  `OpenSpec`/`OpenGrant::consistency`, and `FileSourceAdapter` offers `Live` only — a `Stable`
  request is refused before admission because a plain POSIX file proves no snapshot/CAS.
- §7.1's framing/message/cursor vocabulary is deferred wholesale until the consumer milestone that
  needs it: `BufferPolicy` (`Raw`/`Fixed`/`Line`/`Message`), `OversizePolicy`, `EncodingPolicy`,
  the `Sequential`/`AtOffset`/`Reset` cursor modes, and the `ReadFrame`/`ReceiveMessage`/
  `SendMessage`/`FlushAfter`/`Seek`/`DecodeRange` operations have no adapter or cell-schema
  representation yet. File/media M5 ships `ReadBytes`/`WriteBytes`-equivalent transfers only;
  §14's "unsupported framing/message semantics" refusal stands until then.
- `FileSourceAdapter`'s `DecodeLease` pool (`acquireDecodeLease`/`releaseDecodeLease`,
  `maxDecodeLeases` bound) is a placeholder accounting seam: no decoder or derived-product cache
  exists behind it, so leases record source range + generation but retain no decoded bytes. It
  exists so the lease/budget class is settled before a real decoder lands.
- `generationFromStat` (size + mtime + inode) detects an uncertain write but cannot see a
  same-size, same-timestamp in-place rewrite — proven by
  `SameSizeRewriteWithRestoredMtimeSharesTheGeneration`. That blind spot is why `Stable`/CAS is
  refused rather than synthesized from the generation value.

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
