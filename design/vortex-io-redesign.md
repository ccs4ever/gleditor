# Vortex I/O redesign

| Field          | Value                                                                                                               |
| -------------- | ------------------------------------------------------------------------------------------------------------------- |
| Version        | 0.13                                                                                                                |
| Date           | 2026-11-22                                                                                                          |
| Status         | Draft; accepted U-1–U-8 recommendations; proof gates remain open                                                    |
| Scope          | Vortex I/O, source lenses, language bridges, transient payloads, mutable-file observations, rich media, and capture |
| Implementation | Document only; no runtime or on-disk format change                                                                  |

This document describes the inspected working tree, including its pre-existing runtime and compiler
changes. It does not modify or normalize that work. Section 2 records source observations; sections
3–4 distinguish landed foundations from plans; sections 5–11 specify proposed behavior. Existing
tests are evidence about current semantics. Section 6.5 adds illustrative API and language-exposure
traces; every new API and syntax sketch is proposed pseudocode, not an implemented or frozen
interface. No runtime tests or benchmarks were executed for this document.

Related designs: [canonical runtime](vortex-hyperstructural-runtime.md),
[multithreading study](vortex-multithreading-study.md), [Spool-Mesh](vortex-distribution.md),
[Anything-to-ZigZag](anything-to-zigzag-lens.md), [lens candidates](lens-candidates.md),
[lens implementation plan](zigzag-lenses-implementation-plan.md),
[enfilade synthesis](zigzag-lenses-enfilade-synthesis.md), and
[store/slice convergence](store-slice-convergence.md).

## Contents

1. [Problem and settled decisions](#1-problem-and-settled-decisions)
1. [Current implementation](#2-current-implementation)
1. [Current and planned lenses](#3-current-and-planned-lenses)
1. [Language bridges](#4-language-bridges)
1. [Specification reconciliation](#5-specification-reconciliation)
1. [Service and cell protocol](#6-service-and-cell-protocol)
1. [Suspension and backtracking](#7-suspension-and-backtracking)
1. [Transient identity and reclamation](#8-transient-identity-and-reclamation)
1. [Permanent capture](#9-permanent-capture)
1. [Ordering, cancellation, and recovery](#10-ordering-cancellation-and-recovery)
1. [Migration and acceptance gates](#11-migration-and-acceptance-gates)
1. [Proposal register](#12-proposal-register)
1. [Open questions](#13-open-questions)
1. [Version history](#14-version-history)

## 1. Problem and settled decisions

External data must be usable through cells without blocking evaluation, becoming permanent by
accident, or being confused with reversible solver state. An arena is an evaluation space. A socket,
process, database request, or sent byte has a lifetime outside that space.

The owner has settled the following requirements. They describe proposed work.

| Decision               | Required behavior                                                                                                                                                                                                                                                                                                 |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Asynchronous execution | All runtime and lens I/O executes off evaluation/UI threads, including opens, pages, refresh probes, commits, and content resolution.                                                                                                                                                                             |
| Program boundary       | Endpoints, capabilities, requests, payloads, progress, and outcomes are cells. Descriptors, pointers, and native buffers remain private implementation data.                                                                                                                                                      |
| Incoming content       | Transient by default; permanent registration requires explicit capture.                                                                                                                                                                                                                                           |
| Memory                 | Bounded memory and backpressure; the initial implementation has no spill backend, including opt-in spill. Unpausable sources report overflow or gaps using reserved reporting capacity.                                                                                                                           |
| Explicit submission    | Every fresh execution freezes/pins output and creates a new operation, including execution reached again through backtracking.                                                                                                                                                                                    |
| Automatic resumption   | Observe the same admitted request; never submit it again.                                                                                                                                                                                                                                                         |
| External effects       | Sent bytes cannot be undone. Cancellation reports actual partial progress and uncertainty.                                                                                                                                                                                                                        |
| Existing policy        | Preserve declarative lens transactions, VAgent post-commit effects, and Evidence grants, durable attempts, receipts, and explicit retry rules.                                                                                                                                                                    |
| ABI freedom            | There are no production users or production ABI to preserve. Change in-memory ABI and interfaces freely when a cleaner design requires it; do not add compatibility shims for obsolete interfaces. Existing on-disk/wire format versioning and the 64-byte `CompactOpNode` size/alignment invariants still apply. |
| Capture identity       | Preserve overlaps across captures by source-generation interval translation; equal content does not establish identity.                                                                                                                                                                                           |

The cell boundary must cover payload access itself. A connection cell that directs programs to an
unrelated byte-stream API leaves the boundary incomplete. Native byte storage is an implementation
choice; programs read and slice payload cells through the same resolver-aware value interface.

## 2. Current implementation

These observations are source-backed as of 2026-11-20. A class name, comment promising a future
feature, or promise-shaped graph does not establish asynchronous execution.

### 2.1 Vortex and arena state

The [VM opcode enum](../apps/common/xanadu/vortex/vortex_vm.hpp) has no general external-I/O
submission or await instruction. RelationNext calls its provider inline with a mutable arena. An
ArenaSavepoint covers answer construction; the VM validates and publishes the answer references
atomically and guards against provider reentry. This is a synchronous relation extension point, not
an asynchronous worker ABI.

ChoicePoint snapshots include cell-reference bindings, streams, iterators, activation frames,
guards, and mutation state. Existing tests
[stream rollback tests](../tests/xuzz/vortex_stream_test.cpp) pin stream and iterator restoration.
[relation publication tests](../tests/xuzz/vortex_relation_next_test.cpp) pin reversible relation
publication and reentry refusal.

The [resume side table](../apps/common/xanadu/vortex/vortex_resume.cpp) deliberately survives
ChoicePoint rewind while token references backtrack. ResumeInit and ResumeSet accept only Int64 and
Bool. This provides precedent for non-trailed control state; it supplies neither general I/O
continuations nor buffer ownership.

[ArenaManifold](../apps/common/xanadu/zigzag/arena_manifold.cpp) releases a mark by restoring old
slots and truncating cells, links, content, and scratch storage. New ephemeral CellRefs derive from
dense allocation indices. A released number can therefore name a later allocation in the same arena.
contains(ref) checks slot existence, not allocation incarnation. A coarse arena generation plus
contains(ref) cannot validate an old result cache.

Current arena scratch appends and truncates with marks. compact repacks content-span and link runs;
it is not a scratch-byte collector and refuses outstanding marks. Existing PrimediaSpan and scratch
resolution do not establish support for independently leased transient sources.

### 2.2 Projection and transport foundations

[ProjectionLens](../apps/common/xanadu/zigzag/projection_lens.hpp) enumerates external keys, nodes,
and edges. SourceNode carries text/scalars or an existing-cell reference.
[ProjectionSession](../apps/common/xanadu/zigzag/projection_session.hpp) provides identity maps,
dirty edits, refresh, commit, and checkpoints. Its session pins refuse rewind; they are not
byte-range leases.

Content and attribute writes return synchronously. WindowedLens materializePage also returns
synchronously. discardMaterializedAfter drops off-arena page buffers during projection rewind.
pollChanged and optional waitableFd expose change hints, not general operation submission or VM
suspension. Current session refresh is driven at marks-quiesced points.

[NetLens](../apps/common/xanadu/zigzag/net_lens.cpp) projects supplied ConnectionSnapshot values,
allocates local instances, changes local state, and exposes an in-memory NetStreamRing. It does not
open sockets or implement a network reactor. The ring has bounded storage but no demonstrated
thread-safety, cancellation, or transport ordering contract.

[CommandMailbox](../apps/common/xanadu/zigzag/command_transaction.cpp) mints transaction and promise
cells, invokes its supplied handler inline, fills the promise, and then returns.
[MeshLensChannel](../apps/common/xanadu/vortex/mesh_lens_channel.cpp) has a nonblocking remote-proxy
state machine, while incoming content is retained in native proxy metadata. These are partial
precedents, not a finished shared cell-based I/O service.

The compute-worker pool is not an I/O scheduler: its run waits for work, and the pool-of-one path
can run on the caller. Wrapping inline work in a future would preserve the same blocking behavior.

### 2.3 Language and effect bridges

Vlace performs real host I/O: [lower_io.cpp](../apps/common/xanadu/vlace/lower_io.cpp) uses streams
and getline; [lower_effects.cpp](../apps/common/xanadu/vlace/lower_effects.cpp) opens lenses and
performs gestures synchronously. VIcon yields values for host rendering; control-flow suspension
alone does not move OS work off the evaluation thread.

VProlog compiles queries to Vortex VM frames: `Compiler::solve` delegates to
`VortexStdLib::solveQuery`, which calls `logic::compileAndRunQuery`; clause alternatives, calls,
unification, and returns run through VM opcodes and backtracking
(`../apps/common/xanadu/vprolog/compiler.cpp:243-250`,
`../apps/common/xanadu/vortex/logic_solve.cpp:79-93`,
`../apps/common/xanadu/vortex/logic_compile_core.cpp:6-18`). The older recursive-resolver
description in `vprolog-lens-bridge.md` is stale for this path. An asynchronous Prolog bridge must
preserve the compiled VM activation plus Prolog bindings/trail at Pending; the shared VM suspension
boundary is relevant, while Prolog goal-placement and outcome policy remain frontend-owned.

The [VAgent adapter](../apps/common/xanadu/vagent/adapter.cpp) drains post-commit outbox entries. It
marks a native effect dispatched, invokes the bound handler inline, and delivers EffectDone or
EffectFailed by correlation. The native ABI receives mutable worker context and returns a cell.
Moving that callback unchanged onto an I/O worker would violate arena ownership. Its dispatched
marker prevents ordinary repeated drains/checkpoint replay; it is not proof of exactly-once delivery
at an external service.

[Evidence EffectRuntime](../apps/common/xanadu/evidence/effect_runtime.cpp) already executes work on
a worker thread. Dispatch checks the exact intent's session grant, records and durably saves an
admitted attempt, then launches work. Owner-side poll retains acceptance and completion receipts,
including late receipts when the original case is unavailable. Recovery does not silently resend an
attempted intent. An independent retry can require explicit duplication acknowledgement.
Cancellation does not prove that the effect was avoided.

The redesign must preserve these policies. Adapting Evidence's execution and result transport does
not authorize removing grants, durable admission, receipts, or uncertain outcomes.

### 2.4 Mutable files and media ranges

These are source observations, not a claim that current file or media access is a Vortex service.
[FilesystemLens](../apps/common/xanadu/zigzag/filesystem_lens.cpp) keys entries by device and inode
and records size and modification time during a one-time walk. It projects names and metadata, not
file contents or content generations. Its `pathFor` result is a locator.
[GitLens](../apps/common/xanadu/zigzag/git_lens.cpp) projects commit/ref metadata and watches HEAD,
a resolved ref, and packed-refs using file stamps and `FileWatch` notifications. Commit SHA keys
identify Git objects; this lens does not expose blob bytes. A Git working-tree path remains mutable
independently of the commit DAG.

`FileWatch` reports filesystem notifications, while GitLens's fallback stamps compare modification
time and size. Neither proves that bytes read before or after a notification form one coherent file
version. An open descriptor or mapping likewise does not prevent in-place mutation. The generic
[FileMediaStream](../src/media_stream.cpp) records a range size when constructed, then reopens its
path for each random-access read; `subspan` reconstructs from that path. The memory-backed stream
instead owns shared immutable bytes. Current media import and publication paths also read whole
files into strings (`apps/common/ui/xanadoc/views.cpp`, `apps/common/xanadu/publication.cpp`), which
does not define a bounded large-media path.

This differs from [UserPermascroll](../apps/common/xanadu/user_permascroll.hpp): published bytes are
append-only and immutable at their permanent coordinates. A store contains edit decisions and
references the author's permascroll; it does not own arbitrary source files. Do not give a mutable
file a permascroll identity or weaken those permanent-address guarantees to make file editing fit.

## 3. Current and planned lenses

No general IoLens was found in the inspected implementation. ProjectionLens is a general projection
protocol. The following are class/interface foundations, not a claim that every backend or control
path is production-complete.

| Family                     | Landed foundation                                                               | Planned or incomplete scope                                                                                               |
| -------------------------- | ------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| Filesystem/process/network | FilesystemLens, ProcLens, NetLens                                               | Reactive OS sources, typed endpoint operations, DevLens, and shared asynchronous execution.                               |
| Writable sources           | SqlLens, ConfigLens                                                             | Asynchronous open/page/probe/commit lifecycle with current validation and transaction semantics.                          |
| History/events             | GitLens, LogLens, JournalLens                                                   | Worker staging and bounded retained-payload ownership.                                                                    |
| Composite/derived          | RouterLens, UnionLens, MetaLens, VqlResultLens; SourceNode project-by-reference | Operation ownership across composition without duplicating source identity.                                               |
| Swarm/control              | SwarmLens, MeshLensChannel, ReconcileLens, Treefilade, DimCap foundations       | Broader remote projection, telemetry, and real control adapters; mock OS control is not process I/O.                      |
| Additional sources         | Proposals in lens-candidates                                                    | XanadocLens, Mail/IMAP, Calendar/iCal, MediaMeta, Activity, and Micropayment; Micropayment remains unscheduled phase two. |
| Transductive               | TransductiveView and enfilade primitives                                        | Filtering, sorting, predicate pushdown, slicing, and viewport integration are separate work.                              |
| Presentation               | LatticeSliceView, XanadocSliceView, AcmeSliceView                               | Consumers of staged results and indexes, not transport engines.                                                           |

The enfilade synthesis's synthetic, transductive, and presentation tiers do not constitute a general
I/O scheduler. Index proposals and prior performance discussions are not measured acceptance
evidence for this redesign. Early lens documents saying no lenses exist or SQL is only specified are
stale relative to the foundations above.

A source lens will snapshot a native operation plan on its owner, execute backend work through the
service, and publish staged results on its owner. refresh probes must not perform hidden filesystem,
database, or network work in the UI loop. Source version/generation checks protect delayed commits.
Router/Union delegation retains the originating request and source identity instead of reissuing
operations at each wrapper layer.

## 4. Language bridges

| Language | Current seam and bridge status                                                                                                                    | Required change                                                                                                                                        |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| VQL      | REPL projection and query/step machinery; the [generator/gesture bridge](vql-lens-bridge.md) is proposed.                                         | Add query suspension and explicit effect multiplicity; incidental row traversal must not submit effects.                                               |
| VPL      | Applicative evaluation and REPL lenses; the [system-verb bridge](vpl-lens-bridge.md) is proposed.                                                 | Define N-cell submission order, multiplicity, bounded concurrency, and asynchronous array results.                                                     |
| vprolog  | Prolog queries compile to VM frames through `Compiler::solve` and `logic::compileAndRunQuery`; proposed lens predicates remain a separate bridge. | Add Pending suspension for the VM activation; keep submit placement, Prolog bindings, typed recovery, and choice/trail policy in the compiler/adapter. |
| Vlace    | Implemented IO stream/lens handlers; the [cell-directed bridge](vlace-lens-bridge.md) is partly landed.                                           | Replace blocking stream/backend calls with resumable driver state.                                                                                     |
| VIcon    | Compiled produce/yield and host-presented values; the [request-gate bridge](vicon-lens-bridge.md) is proposed.                                    | Add explicit asynchronous requests; distinguish VM yield from an external host sink and keep effect policy in its frontend.                            |
| VAgent   | Post-commit outbox, correlation await, and native dispatch exist.                                                                                 | Preserve admission and correlation; snapshot inputs on owner, execute without mutable worker context, and mint replies on owner.                       |

All front ends share service ownership, range leases, endpoint generations, and cancellation. Their
evaluation models determine notation and multiplicity, not separate resource implementations. Runner
stdout/stdin, REPL streams, imports, and module content resolution are host bridges to audit.

A backtracked explicit read or write is a fresh submission when executed again. Awaiting an already
retained request returns that request's observation. Thus a read request consumes its source
sequence once; rewind can observe its retained result again, while a new read requests subsequent
input. Query generators must expose this distinction instead of silently attaching I/O to lookup.

VPL must specify deterministic order when an array expression creates several submissions; the
service preserves each endpoint's admitted order. VAgent's declarative outbox remains post-commit.
An explicit stream operation must not inherit outbox deduplication merely because both use the same
native service.

### Shared effect contract

All language bridges share these static rules even where their notation and evaluation model differ:

- **Submit** is an explicit effect. Every time its designated submit action is evaluated, it admits
  a new request or returns a typed admission refusal. Backtracking and reevaluation do not
  deduplicate it.
- **Await** observes a named retained request and may suspend. Resumption observes that request; it
  never evaluates submit again.
- **Render/display** is observational and cannot initiate work. Guards, predicates/filters, and
  read-only answers are pure; they cannot submit, capture, cancel, or mutate endpoint state.
- Each bridge identifies statically checked submit positions and states their evaluation
  multiplicity under that language's control model. Invalid effect placement receives a static
  diagnostic where the frontend supports checking; a runtime guard remains a backstop, not the
  language contract.

A backend's terminal status is represented by a typed Outcome cell, never implicitly translated into
logical failure. EOF is a normal, distinct terminal observation. Partial payload and progress remain
available alongside rejection, error, cancellation, overflow, or unknown status. Each bridge defines
how its control model recovers from that typed value; cancellation of a language-level wait is
separate from cancelling the underlying request.

### Spool-Mesh actor turn boundary

A `MeshActorRuntime` executor callback remains one atomic message or wake turn. Direct IoService
`Submit` and `Await` are statically disallowed in that callback; actor-program validation rejects
those sites before the turn runs, with a typed refusal. The runtime repeats the check as a backstop.
This restriction applies before any request admission. If an invalid dynamic path reaches `Await`
after a separate `Submit` was already admitted, refusing the wait does not retract or undo that
request; its original outcome remains observable under the shared contract. The callback never
blocks the owner pump and never returns a parked VM PC or native callback.

Existing VAgent handler effect calls remain reversible staged intents in an actor turn; they are not
IoService `Submit` and start no external work before commit. Before commit can expose an intent, its
semantics must identify immutable bytes and endpoint ordering so later actor-cell mutation cannot
change what the intent means. On successful commit, the adapter transfers/retains a bounded
immutable payload lease and durably records attempt metadata before service work; the journal
records metadata, not transient payload bytes. A failed or rewound turn stages nothing. VAgent plan
effect calls that would suspend inside the atomic callback are statically refused for actor
programs. After a successful turn commits, the adapter dispatcher queues a durable attempt-record
write for each staged intent. The write and flush run off evaluation/UI threads; IoService `Submit`
waits for the owner to receive a durable acknowledgement and never blocks that owner. Queue bytes,
journal bytes, and flush work are charged to bounded service budgets; a full or unavailable journal
refuses admission before external work. Stable intent identity includes actor generation, committed
turn identity, and intent ordinal, so replay of an older checkpoint can find the original attempt.

A durable attempt ledger records admission before the service starts work. On restore/replay, the
runtime checks the actor checkpoint's causal floor against that ledger before invoking the executor.
An absent, unknown, or unreconciled proof refuses actor-profile restore/replay with a typed outcome;
it never dispatches the intent again. A checkpoint is also refused with `NotQuiescent` while an
attempt is being durably admitted or service work is unresolved. The ledger retains each attempt and
its receipt/unknown result until the actor checkpoint floor and causal acknowledgements permit
retirement; bounded retention exhaustion applies backpressure or refuses new actor I/O. A crash
between durable admission and a conclusive receipt yields `Unknown`, not automatic retry. The ledger
stores attempt metadata only and does not capture transient payload bytes.

After a successful durable admission, completion is delivered as a later owner-queued actor
message/turn carrying actor generation, request/correlation identity, and typed outcome, with no
`CellRef`. The actor handles that event through ordinary message-boundary execution; it does not
resume a serialized VM continuation. Actor grants bound admitted I/O bytes and request count in
addition to their existing turn and mailbox budgets. A late completion for an old actor generation
only reconciles and releases its original request.

## 5. Specification reconciliation

The [canonical runtime](vortex-hyperstructural-runtime.md) requires staged content resolution:
missing content returns Pending rather than blocking, and SpanReader work happens off the evaluation
thread. Its byte-returning resolve surface needs an explicit cell-result replacement. The
[multithreading study](vortex-multithreading-study.md) requires one owner per mutable arena and
share-nothing workers over immutable data. Arena disposal cancels unpublished computation; it cannot
undo already submitted I/O. The lens event contract similarly separates source-thread capture from
owner-thread projection.

Older lens provisions deliberately bypass cells for Net payload rings and invoke local commands
synchronously while linking. The new protocol supersedes those provisions at the program boundary. A
complete implementation must reconcile those documents and their language bridges, rather than
claiming that a promise or gate already satisfies the asynchronous contract.

Nelson's [Xanadu model](https://xanadu.com/zigzag/fw99/XUmodel.html) connects transclusion identity
to permanent addresses and describes registration at arrival. This proposal deliberately places
transient ingress before permanent registration. Its recyclable buffers are not a permascroll;
explicit capture crosses that boundary.

His [ZigZag reference](https://xanadu.com/zigzag/ZZdnld/zzRefDef/) describes media-span cells, cell
inputs/results, and one connection in each direction on a dimension. A schema cannot link every
private request directly to the same target on one reciprocal d.target edge: later links would
displace earlier ones. Use private capability occurrences and ordered occurrence ranks. These
sources guide identity and graph structure; they do not establish an asynchronous scheduler or a
reclamation algorithm.

## 6. Service and cell protocol

Everything in this section is proposed. IoService, IoLens, and operation names below are design
names, not existing classes or agreed opcode ordinals.

### 6.1 Layer and execution ownership

IoService belongs under apps/common/xanadu, beside the runtime and lenses. Source adapters use it;
NetLens is one client. Generic OS primitives may live in the library only if they remain independent
of Xanadu and the plain editor's configuration. Hardware/OS calls, scheduling, native buffer
ownership, and suspension machinery justify C++; composable transformations and orchestration belong
in Vortex standard-library code where existing primitives suffice.

**Normative frontend boundary:** `VortexCore`, `VortexVM`, and the shared Vortex I/O capability
protocol remain language-neutral. They define submit admission, Pending/Await/resume, typed
outcomes, and lifetime behavior once for every caller. They do not inspect frontend names, ASTs,
syntax modes, purity rules, effect placement/multiplicity, or frontend-specific
continuation/callback state. Each language compiler and adapter owns its grammar, types,
purity/effect checks, evaluation multiplicity, lowering, and any frontend-local continuation state;
it maps these to the common Vortex cell/capability protocol. Direct Vortex programs exercise that
same protocol. Capabilities are informed by the needs of all frontends, while the VM and core stay
generic.

Host-specific execution policy belongs in a generic host/profile interface, not in Vortex opcodes or
frontend-name branches. For example, a mesh actor profile can reject a suspending callback at the
host boundary; the VAgent adapter alone maps VAgent's reversible staged handler intent to
post-commit service submission. The profile does not make VortexCore or VortexVM VAgent-aware.

The current VM has VQL-specific closure arity and parameter-pipeline opcode exceptions
(`apps/common/xanadu/vortex/vortex_vm.hpp:308–310` and
`apps/common/xanadu/vortex/vortex_vm.cpp:1714–1718`). These are baseline layering debt, not
precedent for new asynchronous branches. Cleaning current frontend references out of the core is a
separate project: this redesign neither requires it nor makes it an I/O enablement gate or mandatory
P1/P4 work. Its boundary and acceptance checks apply to new or expanded I/O coupling only; they
compare against this documented baseline.

The arena owner validates cell arguments and produces an owned native plan. Workers receive only
that plan, service identities, cancellation state, and immutable buffer leases. They receive no
CellRef, arena reference, scratch string_view, projection cell, or callback that can mutate the VM.
Native completion messages contain no solver references. Only the owner materializes cells.

Readiness-driven transports and a bounded blocking-worker backend may coexist. Opens, DNS,
filesystem probes, database calls, and persistence can block before a descriptor becomes waitable;
they must also leave the evaluation/UI thread. The host pumps completions without joining running
workers. A deferred future, caller-runs fallback, or nested event loop executing a blocking handler
does not meet this requirement.

### 6.2 Cell-visible values

| Value                 | Proposed meaning                                                                                                                          |
| --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| Endpoint              | Inspectable description of a resource and its service generation/state.                                                                   |
| Capability occurrence | Owner-minted authority to specified operations on that endpoint; copying preserves managed authority, not an editable descriptor integer. |
| Request               | Reference to one immutable admitted operation plus its state and correlation.                                                             |
| Payload               | Resolver-backed transient or durable byte ranges, with slicing and source identity.                                                       |
| Progress              | Monotonic accepted/transferred counts and typed phase information.                                                                        |
| Outcome               | Payload/result, EOF, rejection, error, cancellation, partial completion, overflow, or unknown outcome.                                    |

Payload cells are binary-safe: embedded NUL and invalid UTF-8 bytes remain addressable data. Text
decoding is an explicit view/operation with its own boundary/error rules. An empty payload is a
successful zero-length value; EOF is a distinct typed outcome. Neither empty data, pending work, nor
a cancelled request may be represented by logical failure or an absent-cell sentinel.

Message-oriented transport is an optional endpoint capability, distinct from byte-stream reads. A
successful `ReceiveMessage` yields exactly one complete message with its length and boundary, even
when its payload is zero bytes; it is not stream EOF. A `SendMessage` request freezes exactly one
message. Buffering or native coalescing may combine implementation work only if each admitted
request and each message boundary remain independently observable. These names are proposed protocol
terms, not ABI commitments, and adapters need not support message operations. An adapter without the
requested guarantee returns a typed unsupported-capability refusal before admission.

Message bounds never silently turn truncation into a complete message. If a received message exceeds
the admitted maximum, return typed `MessageTooLarge`/overflow information, not a successful partial
message. The adapter must declare whether that oversized message was consumed/discarded or remains
available for a later bounded receive; if it cannot report that distinction, it refuses the mode.
Stream EOF is not synthesized for message endpoints. A zero-length message remains a valid message
outcome, distinct from an empty byte-stream read and from EOF.

Native descriptors and credential material stay private. Rendered cell content, user-forged
topology, and guessed integers cannot mint authority. Session/resource generations distinguish a
reopened endpoint from a reused OS descriptor. Copying a managed value retains its lease through the
owner; serializing its text does not reproduce that authority.

Requests own private field/capability occurrences. Endpoint queues use occurrence ranks, respecting
one neighbor per direction per dimension. Shared canonical endpoint or capability cells are never
targets of many incoming links on the same dimension/direction. For two requests sharing an endpoint
and capability, each request's private occurrence links to its own view cell; each view cell is a
posward clone of the canonical endpoint/capability on `d.clone`. The endpoint queue links the two
request occurrences as a reciprocal ordered rank. Field lookup walks from the request through its
own occurrence to its view and then to the canonical managed value; it does not link both requests
directly to one target. The spike must demonstrate this graph, including reciprocal links and clone
placement, with at least two requests sharing a queue and capability.

The exact dimension names, tags, and resolver representation remain an ABI decision for the
transient-integration spike. The graph above is a topological constraint, not a commitment to those
names or to a particular resolver implementation.

### State ownership boundary

| State                                                                                         | Owner and representation                                                                                                    | Reclamation rule                                                                          |
| --------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- |
| Endpoint/request identity, status, correlation, outcomes, progress, and continuation identity | Inspectable cells and Vortex-owned values; cell topology is authoritative.                                                  | Follow semantic cell roots, marks/trails, and request ownership.                          |
| OS handles, worker queues, native buffers, kernel registrations, completion transport         | Private native service state.                                                                                               | Release only after the OS/backend acknowledges that no user remains.                      |
| Lookup maps, range indexes, and caches                                                        | Native acceleration indexes only; rebuildable from authoritative managed state or validated against allocation incarnation. | Invalidate on release/reuse, endpoint generation change, and final semantic-root release. |

A native map keyed by `CellRef` may not become a parallel store of logical status or outcome. Each
index has an owner, a rebuild/validation rule, a bounded cost, and a deletion rule. C++ is justified
for OS calls, scheduling, and opaque native resources; cell-visible semantics and composable
orchestration remain in Vortex where existing primitives suffice.

### 6.3 Admission and request state

Preparing a plan is reversible local work. An explicit submit validates authority, endpoint state,
input bounds, and budget reservations. Rejected admission starts no external work. Successful
admission allocates a native RequestId and freezes output before the worker can observe it.

The native lifecycle is Prepared → Queued → Running → Terminal. Rejected admission and unstarted
cancellation have explicit outcomes. Cancellation-requested is an orthogonal flag, not proof of
terminal cancellation. A request has one terminal transition; late reconciliation is additional
evidence about that original request.

Reserve bytes and metadata for a terminal result before starting. Variable payloads require a
declared maximum or bounded streaming windows. Progress may be coalesced; terminal outcomes and
accepted payload ownership must not be silently dropped. Completion transport itself is charged.

RequestRecords live outside solver marks. Their retention follows semantic owners and in-flight
operations; it is not an immortal registry. Last-owner abandonment removes unstarted work or
requests best-effort cancellation of started work. A bounded record/tombstone and required buffer
leases remain until the backend acknowledges release. Admission stops when those reservations cannot
be obtained; an unresponsive backend does not justify spawning unbounded replacement work.

### 6.4 Declarative and durable policy layers

A declarative lens commit snapshots an edit batch, validates its source version, and retains the
backend's transaction/conflict behavior. It is not lowered to a series of stream writes. Its backend
I/O runs off-thread; owner publication consumes its staged result.

VAgent still admits its effect after the deliberation turn commits. The adapter snapshots native
arguments and later creates correlation replies on the owner. Repeated outbox drain observes the
same admitted request. A fresh explicit stream submit remains a new operation.

Evidence keeps its exact-intent grants, durable pre-I/O attempt, receipts, authority revocation,
duplication acknowledgement, and no-silent-resend recovery. The service sits beneath this policy.
Required durable Evidence records are not automatic spill of transient ingress.

### 6.5 Worked trace and frontend exposure (proposed pseudocode)

This section is an aid to implementers, not a declaration of current APIs or grammar. `io.submit`,
`io.await`, `io.capture`, `io.render`, `Pending`, and record fields below are proposed names.
Concrete cell dimensions, tags, handles, layouts, and opcodes remain open; this example does not fix
an ABI. The generic protocol owns authority, request admission, worker state, typed outcomes, and
payload leases. Each frontend decides where its source language may submit and how many times
evaluation visits that site.

```text
cap  = io.capability(endpointCell, operations = [read], maxBytes = 4096)
req  = io.submit(cap, Read { offset: 0, maxBytes: 4096 })  # explicit new effect
seen = io.await(req)                                      # observe this request only
match seen:
  Pending            -> return pending(req)
  Complete(payload)  -> consume(payload)
  Eof                -> finish_input()
  Partial(payload, eof=true) -> consume_then_finish(payload)
  Cancelled(progress, uncertain) -> report_cancel(progress, uncertain)
  Failed(kind, progress) -> report_failure(kind, progress)
  Overflow(gap)      -> report_gap(gap)
  Unknown            -> require_explicit_reconcile_or_retry()
```

`submit` validates capability, endpoint generation, size, and reserved capacity before enqueue.
Refusal starts no work. Successful admission freezes arguments and yields one request identity; only
then may a worker run. `await` may yield `Pending`, which parks execution and lets the host continue
pumping. A later await observes the same request and cannot cause another read. Evaluating the
explicit submit site again creates another request, even after backtracking. `render` only presents
already materialized cells; it is pure and cannot admit or await I/O. `capture` is separate: it
copies selected leased source ranges into permanent permascroll coordinates and publishes the
durable mapping only after successful append/publication.

| Stage                                        | Owner and effect                                                                                      | Observable result                                                                   |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Capability selection / argument construction | Frontend-local pure evaluation creates cells; no OS access.                                           | Capability and immutable request description, or typed local validation error.      |
| Admission (`submit`)                         | Owner validates authority and reserves request, terminal-result, and payload capacity before enqueue. | Admitted request cell or refusal; no external work on refusal.                      |
| Worker execution                             | Service owns native handles and immutable argument/buffer leases off evaluation/UI threads.           | Progress, bytes/EOF, partial transfer, cancellation evidence, or typed failure.     |
| Await / owner publication                    | VM parks for Pending; owner checks request and generations, then materializes outcome cells.          | Pending or typed terminal outcome; re-await observes the same request.              |
| Render / inspect                             | Pure reader consumes existing cells only.                                                             | Display output; no hidden submit, refresh, or capture.                              |
| Permanent capture                            | Explicit owner action pins ranges; service performs bounded append; owner publishes translation.      | Durable span mapping or typed refusal/failure; no implied capture from read/render. |

Terminal status is independent of payload length: an empty successful payload is not EOF. Partial
bytes may accompany EOF, cancellation, or failure. Cancelling an awaiter only stops that waiter;
request cancellation is separate and does not claim the backend stopped. Terminal outcomes report
progress where meaningful; `Unknown` means recovery cannot prove whether an external effect
completed and never triggers silent resend. An unpausable producer that cannot reserve payload
capacity reports an explicit gap/overflow through reserved reporting capacity.

The following sketches are proposed notation only. They state placement and multiplicity, not syntax
commitments; implementations map each operation to the common cell protocol while retaining current
frontend-specific purity, control flow, and failure rules.

A host using an interactive stream can express ordering without pretending admission is completion:

```text
promptReq = io.submit(outCap, WriteBytes(frozenPrompt))
flushReq  = io.submit(outCap, FlushAfter(promptReq))  # ordered fence on this output direction
flushSeen = io.await(flushReq)
match flushSeen:
  Complete -> lineReq = io.submit(inCap, ReadLine(maxBytes = 4096))
  Pending  -> park_same_flush_request()
  other    -> report_without_submitting_line(other)
lineSeen  = io.await(lineReq)
```

`FlushAfter`, `ReadLine`, and the names are proposed. If the host needs the prompt visible before it
waits for input, the input submit is sequenced after successful flush observation; failure does not
fall through to the read. Flush means the adapter's declared flush boundary only, not durable
storage or remote-peer acknowledgement. A durable `Sync` or peer receipt is a separate optional
request and must be refused when unsupported. `CloseGracefully` drains admitted writes before close;
`Abort` or request cancellation reports bytes not sent, partial progress, and uncertainty.

A message-capable endpoint uses a message operation rather than interpreting byte-stream EOF:

```text
emptyReq = io.submit(msgCap, ReceiveMessage(maxBytes = 4096))
emptySeen = io.await(emptyReq)  # CompleteMessage(payload = empty, length = 0)
nextReq = io.submit(msgCap, ReceiveMessage(maxBytes = 4096))
nextSeen = io.await(nextReq)    # CompleteMessage(payload = bytes, length = 12)
firstSend = io.submit(msgCap, SendMessage(frozenMessageA))
secondSend = io.submit(msgCap, SendMessage(frozenMessageB))
```

`ReceiveMessage`, `SendMessage`, and `CompleteMessage` are proposed. The two receives observe two
messages, not one stream read followed by EOF; the sends retain separate request identities even if
the backend coalesces system calls. For an oversized message, the outcome identifies its bound and
whether the message was consumed or retained; it never presents a partial payload as a complete
message. An adapter unable to report boundary/oversize behavior typed-refuses message capability.

Full-duplex streams need independent read/write progress. For example, a child process may block
writing stdout when its pipe fills while waiting for stdin. A caller that awaits full stdout before
draining it can deadlock the child. The service should account bounded bytes and terminal/control
reserve by endpoint direction/request, allow read progress while a write is backpressured where the
adapter permits, and cancel one request without tearing down sibling operations unless endpoint
policy declares that effect. This does not require every adapter to support duplex or durable sync;
unsupported guarantees are typed refusals. Each request has an independent identity and outcome;
completion may arrive out of admission order. VPL preserves input position/shape and VQL preserves
row association by retaining those request cells, so completion order never becomes result order. A
generic owner collection of request cells can correlate a fan-out without a batch opcode; only add
group identity if shared deadline/accounting/cancel selection cannot be expressed by that cell rank,
and preserve partial admission and per-request outcomes.

| Frontend       | Proposed exposure sketch                                                                                                        | Placement, multiplicity, and purity contract                                                                                                                                                                                                                                                                                                                                                       |
| -------------- | ------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| VQL            | `let req = submit(cap, read(offset, 0, 4096)); await(req)`                                                                      | Explicit effect form, never implied by row traversal, projection, or `render`. Compiler defines a submission site and traversal multiplicity; resume awaits the retained request. Querying staged rows stays pure.                                                                                                                                                                                 |
| VPL            | `Reqs ← submit¨ Caps ReadSpecs`; `Out ← await¨ Reqs`                                                                            | Explicit array operation with declared shape/order and bounded concurrency; each element submission is fresh. Lowering keeps each request at its input position independent of completion order. Formatting/rendering and arithmetic do not submit.                                                                                                                                                |
| vprolog / Vlog | `submit(Cap, read(0,4096), Req), await(Req, Outcome)`                                                                           | Explicit predicate, outside speculative search unless a declared policy stages it. A backtracked visit to `submit` is new work; `await` on a bound request is observational.                                                                                                                                                                                                                       |
| Vlace          | `perform (submit cap (Read 0 4096)) >>= await`                                                                                  | Pure description construction remains pure; only explicit perform/driver boundary admits work. Rendering an `IO` description is not performance. Suspension and EOF/error conversion are adapter-owned. A graceful close follows the explicit flush result/barrier; abandoned admitted work retains service-owned leases, while abort is explicit and GC never blocks or implies successful close. |
| VIcon          | `req := submit(cap, read(0,4096)); await(req)`                                                                                  | The current `write(e)` yields a cell to the host; the runner may print that value to stdout. VM yield and pure rendering do not submit to IoService. A runner stream sink uses explicit write/await requests; re-executing submit creates a new request.                                                                                                                                           |
| VAgent         | `stage effect Write{writer, frozen(bytes)}; stage effect Flush{writer, after: lastWrite}; on EffectDone{request, outcome}: ...` | Actor turn freezes byte/order semantics into reversible intents; only after successful commit does the adapter transfer a bounded lease and durably admit metadata before service work. Completion arrives in a later owner turn. Local-runner explicit `perform` has its own submit point. Failed/rewound turns admit nothing; replay does not resubmit.                                          |
| Direct Vortex  | `Submit(capCell, requestCell) -> requestCell; Await(requestCell) -> Pending \| Outcome`                                         | Baseline user of the language-neutral protocol, not another surface language. Core op semantics contain no frontend syntax or multiplicity policy.                                                                                                                                                                                                                                                 |

### VPL array, framed input, and buffered output (proposed pseudocode)

VPL lowering preserves input positions and shape when producing request cells. Empty input produces
an empty request view; ragged input retains its extents. Completion order is independent of
position. For zipped input views, equal shape is required here; a mismatch is a typed frontend
refusal. Per-position admission refusal and terminal outcomes stay at their original positions.

```apl
Reqs ← submit¨ Caps ReadSpecs
Out  ← await¨ Reqs
```

A framed read and pure decoding remain separate. The transport owns the bounded source cursor and
unread suffix; VPL owns the numeric/text view and tokenization.

```apl
Req ← submit Cap (ReadFrame mode: Line maxBytes: 4096)
```

The trace below describes how the VPL host handles the shared execution result; it is not VPL
grammar. `Pending` is a parked-execution result, not a terminal `Outcome`. The host retains this VPL
activation and the same `Req`, then resumes this await when notified. It does not read a payload or
submit the next sequential read while the result is Pending.

```text
await Req -> Pending:
  park this activation; when resumed, await the same Req again
await Req -> Outcome(Complete {payload}):
  Data = decodeUtf8(payload)  # pure; malformed/truncated are typed decode outcomes
  Head = 2 ↑ Data             # view trimming does not rewind the source cursor
  Next = submit Cap (ReadFrame mode: Line maxBytes: 4096)
await Req -> Outcome(Partial {payload, terminal}):
  decode only the supplied payload; inspect terminal status before deciding whether to read again
await Req -> Outcome(Eof | Failed | Overflow | MessageTooLarge | Rejected | Cancelled | Unknown):
  inspect the typed outcome; do not dereference a missing payload or submit Next implicitly
```

`ReadFrame`, `decodeUtf8`, and the outcome fields are proposed. A zero-length payload is distinct
from `Eof`; `Short`, `Malformed`, `Truncated`, `Overflow`, and `MessageTooLarge` stay typed. If
framing reads ahead past a delimiter, the bounded suffix lease remains with the endpoint for `Next`;
trimming `Head` changes only the VPL view. An oversized message also reports whether it was consumed
or retained. Unsupported framing or decoding modes return typed refusal.

A pure local output buffer is inert until explicit service submission. A flush fence orders accepted
writes; close and abort are separate lifecycle choices.

```apl
Buffer   ← appendBytes emptyBuffer EncodedChunk  ⍝ pure bounded local construction
WriteReq ← submit OutCap (WriteBytes freeze Buffer)
FlushReq ← submit OutCap (FlushAfter WriteReq)
```

Assume `WriteReq` was admitted; on a typed refusal, inspect it and do not submit `FlushReq`. Inspect
every later submit result before awaiting its request. The host applies these transitions; Pending
cannot trigger a dependent close or abort:

```text
await FlushReq -> Pending:
  park this activation; when resumed, await the same FlushReq again
await FlushReq -> Outcome(Complete):
  CloseReq = submit OutCap (CloseGracefully after: FlushReq)
  await CloseReq -> Pending:
    park this activation; when resumed, await the same CloseReq again
  await CloseReq -> Outcome(closeStatus): inspect closeStatus
await FlushReq -> Outcome(Partial | Failed | Cancelled | Unknown | Rejected):
  inspect transfer progress and uncertainty
  if policy explicitly selects Abort:
    AbortReq = submit OutCap (Abort)
    await AbortReq -> Pending:
      park this activation; when resumed, await the same AbortReq again
    await AbortReq -> Outcome(abortStatus): inspect abortStatus
```

These are illustrative host/adapter transitions, not a VPL-specific VM mode or grammar. `WriteReq`,
`FlushReq`, `CloseReq`, and `AbortReq` keep independent identities. A native backend may coalesce
transfers without merging outcomes; unsupported framing/flush/close guarantees are typed refusals.
No VPL/core opcode is added for array shape or token grammar.

### VIcon yield and stdout sink (proposed async boundary)

Current VIcon `write(value)` yields a value to the host; it does not itself write bytes
(`apps/common/xanadu/vicon/compiler.cpp:793-822`). The standalone runner prints yielded results to
`std::cout` (`apps/vicon/main.cpp:41-46, 138-152`). A future asynchronous runner models its stdout
sink separately from VM yield and pure rendering:

```vicon
write(valueCell)  # existing VIcon yield: offer a cell to the host
# Proposed host sink, not existing VIcon syntax:
req := io.submit(stdoutCap, io.writeBytes(encode(valueCell)))
seen := io.await(req)
```

The renderer may display an already-available cell without submitting. If the host selects an
external stream sink, it freezes bytes and uses shared write/await/flush requests; Pending resumes
the same VIcon activation, and re-executing the submit site is new work. The current synchronous CLI
sink is implementation evidence, not a reason to exempt stdout from the proposed I/O boundary.

Buffering and framing are composable layers, not frontend cases in the VM. A transport/service may
coalesce native reads/writes or retain bounded chunks; a byte request selects an unbuffered, fixed
length, line-delimited, or adapter-supported frame policy; decoding incrementally maps byte ranges
to text and reports malformed/truncated sequences; a language lexer/tokenizer consumes already
materialized bytes in its own compiler/adapter. “Unbuffered” means no additional service read-ahead
or aggregation policy, not synchronous I/O or a promise to bypass kernel/device buffering. Tokenized
means framing bytes for a caller, not running a frontend lexer in the core.

There are two distinct buffer layers. A frontend may build a pure, bounded local buffer value:
append, encode, inspect, or render does not submit. An explicit frontend action such as VLace
`perform(flush buffer)` freezes selected bytes and reaches the shared admission point; VAgent actor
staging freezes semantic bytes/order before commit, then reaches durable shared admission after
commit. Separately, an adapter may retain service-owned buffered writes after admission and coalesce
native transfers. Those requests already have identities, and service coalescing never merges or
changes their semantic outcomes. Each layer charges its retained bytes and releases them by its own
lifetime rule.

A buffered write submission freezes one buffer version and immediately gets its own request
identity. Admission only means the service accepted bounded work; it does not assert that bytes
transferred. Flushing is an explicit asynchronous barrier/operation ordered after earlier writes on
its endpoint; its outcome reports transferred bytes and partial/unknown progress. Durable sync is a
separate backend-specific request and may be refused when the backend cannot promise it. Graceful
close drains accepted writes then reports close/flush outcome; abort/cancel requests stop and
reports unsent, partial, and uncertain bytes. Neither operation retracts bytes already sent. A
second execution of buffered submit is fresh work; await/replay never resends the frozen request.

Read framing may read ahead across a delimiter or fixed frame. The endpoint retains the unread
suffix under a bounded lease so the next sequential read begins at the correct byte; EOF, empty
frame, and successful zero-byte payload stay distinct. Raw reads, framed reads, seek, and reset
share an explicit cursor rule: mixing modes either advances one source-generation cursor by
returned/consumed ranges or is refused until a reset/rebind, never silently skips buffered suffix
bytes. Checked wide offsets and lengths apply to media/range reads. Delimiter, UTF-8 code point, and
frontend token boundaries may split across chunks; the adapter retains bounded incremental
decoder/framer state and returns typed partial/overflow/truncated outcomes. Oversized
line/frame/token policy must be explicit (bounded partial plus continuation, drain-to-delimiter with
a reported gap, or refusal); never grow without limit. Frontend tokenization and grammar remain
outside generic service/core. These are protocol questions for the spike and adapters; the exact
mode vocabulary and cursor API remain open. A token/framing operation owns only the bounded byte
state it needs; a VQL/VPL/VProlog/VIcon/VLace/VAgent tokenizer remains a pure frontend operation
over captured bytes. Sequential frame read-ahead must retain the unread suffix, while raw byte
access, seek, and reset either transfer/advance one explicit cursor or refuse ambiguous mixing. The
owner may cancel a derived decode/seek request without canceling an unrelated source read; leases
release independently. A tokenizer can hold a partial token across chunk boundaries, but its grammar
and token continuation are frontend-owned.

For the Vlace sketch, constructing or extending a pure local buffer does not submit; only the
explicit `perform(flush buffer)` freezes its contents and reaches admission. For VAgent actor
effects, staged buffer contents have fixed meaning before commit and durable admission occurs after
commit. A service-owned buffered-write request is different: it was already admitted and has an
identity before native coalescing begins. A service may optimize transfer grouping, but it preserves
every request's identity, progress, and outcome.

Runner stdin/stdout, module/file loading, REPL commands, and host renderers are adapters, not
alternate protocols. A runner reading source before compilation currently performs host I/O; a
proposed service migration must keep it off evaluation/UI threads without smuggling bytes through a
non-cell program API. Reads of mutable paths state their generation/snapshot guarantee. Large media
reads use bounded ranges and explicit seeks rather than implicit whole-file loading. Direct
Vortex/future-front-end traces must also cover full-duplex multiplexing, independent read/write
cancellation and accounting, zero-length datagrams versus stream EOF, deadlines/late completion, and
wide byte offsets. Adapters may refuse consistency or atomicity guarantees that their backend cannot
provide.

## 7. Suspension and backtracking

### 7.1 Pending is an execution result

Await binds an existing RequestId to owner-side continuation state. A ready request materializes its
outcome; a pending request returns control to the host without success, logical failure, or
busy-waiting. Outstanding choice points and arena marks are allowed. The VM must retain its cursor,
frames, output bindings, guards, and execution phase needed to resume the same await.

A proposed Pending execution result and runtime await primitive must extend the actual VM/host
stepping protocol. They cannot be simulated by running a blocking relation provider or calling the
VM recursively. Cycle/work budgets do not burn while parked; deadlines remain service time.

Completion arrival changes native request state once. The owner checks the run, arena, capability,
endpoint generation, and continuation identity before applying any local result. Automatic resume
observes that request directly; it never jumps through submit again.

### 7.2 Reversible materialization

Materialized payload/result cells and their bindings are ordinary trailed arena state. Rewind may
remove them while the RequestRecord and its retained observation remain. A later await on the same
live request rematerializes that observation in the current branch.

A result cache needs exact allocation incarnation or release-aware invalidation. Validation of only
arena generation and contains(ref) is insufficient: release can truncate a result slot, then mint an
unrelated cell at the same number. The same rule covers request wrappers, cursor bindings, proxy
caches, frame ports, and cell-to-native ownership maps. No raw CellRef crosses arenas.

Lease bookkeeping must follow mark, trail, overwrite, release, discard/cut, frame disposal, proxy
retirement, and arena destruction. Saving an old slot acquires or transfers the ownership needed to
restore it; discarding that saved state releases it. Existing arena callbacks/finalizers are not
assumed to provide this. A spike must prove the hooks and exception paths.

### 7.3 Projection refresh and waits

A result local to an await can be materialized under outstanding marks. Whole-projection refresh or
replacement still honors its own quiescence/version requirements. A staged refresh must not mutate
the old projection beneath saved choices.

Completion needed to reach quiescence must remain deliverable through the await path. Waiting for
all marks to disappear before delivering every completion would deadlock a program suspended under a
choice. Delayed refresh buffers remain bounded and apply backpressure or explicit overflow.

## 8. Transient identity and reclamation

### 8.1 Candidate storage model

Use immutable chunks with range leases as the initial candidate. A transient address names a source
generation, occurrence/range, and offset independently of physical allocation. Chunks never change
while leased; memory can be reused after its last semantic/kernel owner releases it.

A connection reincarnation gets a new source generation. A changed external snapshot cannot reuse an
old interval name for new bytes. Byte-preserving slices retain origin intervals. Transformations
that change bytes create derived identity and explicit provenance; they cannot impersonate the
original interval by matching text or hashes.

The payload resolver must make these ranges usable as cell content, not merely as a native buffer
handle. Durable resolution, scratch resolution, transient resolution, proxy slicing, and persistence
refusal need an explicit typed design. Do not overload scratchScroll, invent an unvalidated scroll
number, or assume current PrimediaSpan supports this lifetime.

### 8.2 Semantic roots

| Root                           | Ownership that must remain live                                                                    |
| ------------------------------ | -------------------------------------------------------------------------------------------------- |
| Request/capability wrappers    | The referenced managed resource; an endpoint capability alone does not retain its traffic history. |
| Pending continuations          | Request, frozen arguments, continuation values, and results required for resume.                   |
| Live cell/result/proxy content | Every represented transient range, including quoted/sliced aliases.                                |
| VM execution state             | Stack/frame values, locals, closures, stream accumulators, iterators, and activation ports.        |
| Choices and trails             | Old values, saved maps, snapshots, and shadowed content that a future release can restore.         |
| Native work                    | Queued writes, active reads, backend/kernel buffer users, and completion queue entries.            |
| Capture work                   | Selected source ranges, prepared translations, and bounded capture metadata.                       |

Overwriting the current cell does not release bytes still present in a choice or trail. Conversely,
keeping a capability or closed endpoint must not retain all previously received chunks. A terminal
request with no semantic owners is reclaimable after native users acknowledge release. Native
completion state may survive arena destruction long enough to finish cleanup, without accessing the
destroyed arena.

### 8.3 Bounds and pressure

Charge bytes and metadata across ingress buffers, frozen output, backend staging, chunks, partially
pinned fragmentation, materialized proxies, requests, completions, continuations, choices, and
capture maps. Bound chunk count and metadata as well as payload bytes. Physical allocation retained
by a tiny slice is charged in full unless compaction safely relocates it and updates managed
indirection.

Long-lived choices can pin old input until budgets fill. The honest result is backpressure or
explicit refusal; rollback does not justify overwriting those bytes. Pausable sources stop reads or
demand. Unpausable sources emit a gap/overflow outcome with source sequence information, backed by
reserved reporting capacity. Repeated losses coalesce into bounded gap ranges or terminate the
source explicitly; reporting cannot create an unbounded queue. Terminal/control reserve cannot be
consumed by ordinary payload.

Defaults and user-adjustable bounds belong in appropriate system configuration with schema, units,
and notes. Backend invariants have named typed constants. This document chooses no arbitrary queue,
chunk, timeout, or memory size.

| Charge class                                                                  | Budget kind and unit                                                 | Accounting owner                          | Delivery                                                                                    |
| ----------------------------------------------------------------------------- | -------------------------------------------------------------------- | ----------------------------------------- | ------------------------------------------------------------------------------------------- |
| Ingress, frozen output, staged buffers, payload chunks, pinned fragmentation  | Configurable aggregate bytes                                         | IoService, including worker/kernel leases | Immutable resolved budget snapshot supplied by host                                         |
| Requests, completions, continuations, proxy/index metadata, capture intervals | Configurable aggregate entries/bytes, with per-class accounting      | IoService and capture owner               | Same snapshot; each class reports its charge                                                |
| Owner-side validation, freezing, overlap planning, interval traversal         | Configurable work units per evaluation turn                          | Arena owner/host pump                     | Turn budget; exhaustion yields a resumable preparation job or explicit typed budget outcome |
| OS descriptor and backend protocol constraints                                | Named typed backend invariants                                       | Backend adapter                           | Compile-time or adapter configuration; document the limit and refusal                       |
| Terminal/control reserve                                                      | Configurable reserved bytes/entries, protected from ordinary payload | IoService                                 | Snapshot includes a separately accounted reserve                                            |

Settings are resolved once for a host/service generation and do not change underneath admitted
requests. Numeric defaults are implementation evidence, but every user-adjustable class requires a
setting schema/default/notes entry when implemented. Owner-side planning cannot scan or freeze an
unbounded selection inline: on budget exhaustion it yields resumable preparation or returns a typed
refusal, without doing backend I/O on the evaluation/UI thread.

### 8.4 Alternatives and indexing

A versioned physical ring is viable only if it never overwrites a pinned range and detects stale
generations. With long-lived choices it stalls at its bound. A spill tier remains an unselected
future alternative; the initial implementation has none, even as an opt-in mode. Whole-epoch arenas
provide cheap bulk disposal but cannot selectively retain arbitrary results without
promotion/copying. Both must be measured against immutable chunks.

An enfilade may index logical ranges, translations, or ownership summaries; the original account is
[Project Xanadu's enfilade introduction](https://xanadu.com/tech/). It is neither byte storage nor
permission to overwrite live content. No asymptotic or throughput improvement is claimed without a
reproducible workload and measurement.

### 8.5 Mutable-file observations and rich media

A file path, device/inode pair, and byte offset locate a read target; none alone identifies
immutable content. Every file adapter declares whether it can provide a stable snapshot/version
token, a live view, conditional writes, or only best-effort observations. A Git blob addressed by
object ID can be an immutable source when read from Git's object store. A working-tree file is a
live external source, even when a GitLens commit node happens to point near it.

A successful live read materializes an immutable **read observation**: the exact bytes retained for
that operation, its byte interval, source locator, and the adapter's stated consistency result. The
observation stays readable while leased even if the source later changes. It does not imply that the
read was an atomic snapshot, or that separate reads came from one version; an adapter that cannot
prove coherence must say so, and the bytes may reflect an in-place write during the read. Matching
before/after stat values cannot upgrade that guarantee. Raw-range cache keys include the
source-generation or read-observation identity and byte range; decoder cache keys also include the
relevant decode parameters. Generation advance retires old entries from live lookup, while retained
bytes stay readable only through existing leases. Path/inode-only cache keys cannot reuse bytes
across observations. Equal bytes, equal inode, or equal path do not merge distinct observations.
Preserve interval overlap identity only within a backend-proven immutable snapshot/version or the
same materialized observation. Separate observations receive separate origin identities.

For a declared stable snapshot, requests bind to that snapshot/version and can read bounded ranges
from it. For a live view, a queued or unmaterialized read records the expected source generation. At
execution, the adapter validates that expectation using its strongest declared primitive; obsolete
expectations yield typed `Stale`, `Conflict`, or `Unknown` outcomes. Once bytes have been retained
as an observation, source invalidation does not revoke them: stale/conflict applies to the live view
or an unmaterialized operation, never retroactively to already-owned bytes. Logical invalidation
retires future access by the old live generation. Physical bytes remain until all cell, choice,
trail, continuation, capture, decoder, cache, and native-operation leases end.

Writes through the service serialize against its own endpoint sequence and carry an expected
generation/version. The backend must state whether it can atomically compare-and-write or only
detect some conflicts. Advance the service generation only after an acknowledged write. On partial
or uncertain write outcome, invalidate the live generation and its caches and refuse
generation-bound operations until revalidation or rebind; prior retained observations remain
readable under leases. Queued operations with an obsolete expectation refuse or report a typed
conflict/uncertain outcome. An external writer can race between validation and write unless the
backend supplies an atomic primitive. Watches are refresh hints only; open descriptors, mappings,
and before/after metadata checks are not atomic snapshot or compare-and-write mechanisms; a mapping
also does not make reads safe across truncation. A strict request refuses when the backend cannot
meet its requested consistency. No generic promise of external-filesystem atomic snapshots is made.
An optional editor overlay may represent new immutable writes and pinned base observations as a
virtual EDL; exporting or committing the overlay to an OS file is a separate conflict-checked
operation. The external file itself is never a writable UserPermascroll.

Rich media uses bounded random-access byte ranges with integer byte offsets and checked offset,
length, and stride arithmetic. Large offsets (including offsets beyond 4 GiB) must work without
whole-file materialization; timestamps are never byte addresses. Time-to-byte mapping, keyframe or
sample selection, codec/container dependencies, and decoder behavior belong in adapters; the VM gets
no media-specific opcode. This proposal adds no unsupported format claim or codec overhaul. A
decoded video frame or audio sample is a derived value with its own bounded
decoder/cache/GPU-surface lease, not an identity for source bytes. Cancellation and seek release
obsolete decode work only after native users acknowledge release. Charge encoded staging, retained
chunks, decoder output, indexes, and GPU surfaces to separately bounded classes; backpressure or
typed refusal applies before they exceed their budgets. Capturing selected encoded ranges preserves
only those bytes and their origin. A self-contained playable artifact is a distinct capture that
must include the container headers, indexes, and referenced codec/container dependencies needed by
the selected adapter. Neither action implicitly captures an entire source file; full-file capture is
an explicit user selection subject to the same bounds and capture rules.

## 9. Permanent capture

### 9.1 Identity across captures

Capture explicitly registers selected transient content in the author's one permascroll and creates
durable references through the existing Structure/store boundary. Stores continue to hold edit
decisions rather than primedia. Capturing a live capability produces an inert description, never
serialized executable authority.

Maintain a bounded map from source-generation intervals to permanent permascroll spans. Capturing
\[0,10), then \[5,15), reuses the permanent address of \[5,10) and appends only \[10,15). A later
discontiguous selection splits against every mapped interval; adjacency can coalesce only when both
source and destination translations are contiguous.

Equal text at another logical source occurrence or source generation is a different origin. Request
identity alone neither establishes nor breaks payload identity: two requests naming the same
verified source-generation interval share that interval. Content hashes may check integrity but
never select an identity translation. Already durable spans retain their identity and provenance.
Unauthenticated ingress gains no invented remote signature when captured locally.

The translation map survives across captures while the corresponding transient ranges remain
addressable. Its permanent-address entries do not pin old ingress bytes. Charge interval metadata to
a separate bound; reserve it before capture, coalesce exact adjacent translations, and refuse or
backpressure further admission when it is exhausted. Retire mappings only when their source domain
or ranges cease to be addressable. Durable captured spans remain valid after that retirement.

### 9.2 Capture execution and failure

The arena owner freezes the selection and acquires leases. A serialized capture/persistence owner
reserves mapping and storage capacity, resolves overlap, appends uncovered bytes, and publishes
durable translations/references. It receives frozen native data, not a mutable evaluation arena.
Store/permascroll writer ownership and off-thread persistence must be explicit in that adapter.

Concurrent overlapping captures serialize translation decisions for their source domain. Reserve a
capture identity and pending intervals before appending, so two jobs cannot independently append the
same uncovered origin. Publish a success only after the required permanent bytes and mapping are
known durable; every durable structure reference must resolve to permanent data.

Append-only bytes cannot be rolled back on failure. Already registered bytes may remain even if
later structure publication fails. Retain or recover committed translations so a retry uses their
addresses rather than appending the same source interval under another identity. An ambiguous
append/publication outcome requires explicit reconciliation; overlapping capture cannot silently
proceed with a guessed mapping.

The spike must choose the capture journal/recovery representation and prove ordering between
permascroll append, translation commitment, and Structure publication. It must bound owner-side
selection freezing and overlap planning by bytes and interval/cell visits per turn; exhaustion
yields resumable preparation or an explicit refusal. If that changes an on-disk format, bump its
version and regenerate affected fixtures; do not introduce a compatibility shim. This document
itself changes no format.

### 9.3 File and media capture

Capture from a mutable file begins with a materialized read observation or a backend-proven stable
snapshot, never with a path/offset alone. Repeated overlapping captures reuse permanent translations
only when they select the same retained observation interval or proven immutable source-version
interval. A later read of the same path is a new observation unless the adapter proves it names the
same immutable version. This keeps source overlap identity without using content equality as a
substitute for origin.

A selected-range capture stores exactly the selected bytes in the author's permascroll and preserves
its interval mapping. A playable-artifact capture is a separate explicit operation that additionally
selects and records the format metadata and dependency closure required by the media adapter. If the
adapter cannot bound or identify that closure, it refuses the self-contained-artifact claim. Large
files are read in bounded chunks under service budgets; no admission or capture path first builds an
unbounded whole-file string. A caller can explicitly select the whole file, but that is still
budgeted and can be refused or prepared over multiple owner turns.

## 10. Ordering, cancellation, and recovery

### 10.1 Endpoint sequence

Each endpoint has a service-owned control sequence for submit, cancel, close, and rebind. Reads and
writes follow explicit backend concurrency rules; ordered byte writes preserve admission order
across partial writes. Progress distinguishes queued/accepted bytes from transferred bytes and does
not promise peer receipt, application processing, or remote durability.

Graceful close rejects new submissions immediately, drains accepted work, then releases resources.
Abortive close rejects new work, requests cancellation, and waits for native resource/buffer
release. A timeout can report an unknown or partial outcome; it cannot free memory still used by the
OS.

Rebinding/reopening creates a new endpoint generation. A late old-generation completion finalizes
only its original request and releases that request's leases. It cannot update the new endpoint,
satisfy a new await, or close a recycled descriptor.

### 10.2 Cancellation and abandonment

Cancellation races with queued work, partial transfer, and completion. Requesting cancellation is
idempotent and reports whether work had started, known accepted/transferred counts, and any
uncertainty. The backend's acknowledged outcome determines whether no effect occurred. Logical
failure, exception, and arena disposal do not retract sent bytes.

Last-owner abandonment removes queued unstarted work or requests cancellation of running work.
Retain bounded cleanup/tombstone state until the backend acknowledges release, including after the
arena or UI session has gone. A blocked driver can exhaust its admitted budget; further operations
then fail or wait explicitly rather than starting unbounded threads.

Explicit stream reexecution creates a new operation. Automatic continuation, duplicate wakeups,
checkpoint restoration, or repeated polling never resubmits an existing request. Durable policy
layers may impose stronger independent retry/admission requirements.

### 10.3 Persistence and replay

Live native capabilities persist only as inert descriptive cells. Loading a document, restoring a
checkpoint, rendering a result, or reopening a workspace never reconnects or submits I/O. A user
must explicitly acquire fresh authority and initiate live work.

Captured transcript replay is an explicit mode that observes recorded content/outcomes. Missing
evidence returns replay-evidence-unavailable; replay never falls through to live I/O. Replaying a
captured read does not create a signed assertion about an external source that was never retained.

Evidence continues to retain late/unknown receipts and refuses silent resend after an admitted
attempt. No general exactly-once external-delivery guarantee is claimed.

## 11. Migration and acceptance gates

### 11.1 Order of work

1. Define native identities, admission budgets, owner pumping, and a delayed fake backend.
1. Prove transient payload resolution, lease roots, release/incarnation validation, and persistence
   refusal without changing the on-disk operation layout unnecessarily.
1. Extend VM/host stepping for Pending and marked await; preserve stream/iterator/choice behavior.
1. Implement one real transport plus partial progress, pressure, cancel, close, and rebind.
1. Convert one source lens's open/page/probe/commit path; prove projection quiescence does not block
   await completion.
1. Implement permanent capture and its overlap/recovery consistency gate.
1. Migrate language bridges through the shared protocol and then additional lens families.
1. Reconcile superseded documents and audit runner, REPL, import, and content-resolution paths.

Each gate needs evidence before broader migration. Vortex/stdlib reuse and correct engine/UI/library
layering are review requirements. A blocking compatibility path cannot be called a completed
asynchronous bridge.

### 11.2 Required experiments and regression scenarios

| Gate                   | Evidence required                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Real async boundary    | Delayed open/read/page/probe/commit keeps evaluation/UI pumping; no caller-runs or deferred inline backend.                                                                                                                                                                                                                                                                                                                        |
| Transient cells        | Read/slice/alias binary payload cells, distinguish empty from EOF, resolve boundaries, refuse accidental promotion, and preserve durable/scratch behavior.                                                                                                                                                                                                                                                                         |
| Marked await           | Completion arrives under outstanding marks; rewind and re-await rematerialize the same request without resubmission.                                                                                                                                                                                                                                                                                                               |
| Dense-slot reuse       | Rewind a result, mint an unrelated cell at the same CellRef, then await without returning or overwriting that cell.                                                                                                                                                                                                                                                                                                                |
| Root accounting        | Replace current payload while a choice/trail/frame/closure/stream/proxy retains the old one; bytes survive until the final root ends.                                                                                                                                                                                                                                                                                              |
| Whole-lifetime bounds  | Charge worker/kernel/completion buffers, tiny pinned fragments, metadata, requests, and capture intervals under sustained pressure.                                                                                                                                                                                                                                                                                                |
| Overflow               | Pause a cooperative source; report a gap for an unpausable source even when ordinary payload capacity is exhausted.                                                                                                                                                                                                                                                                                                                |
| Buffering / framing    | Exercise unbuffered, fixed-length, line/delimited, and framed reads; bounded read-ahead suffix reuse; split delimiter/UTF-8/token boundaries; oversized line/token outcomes; raw/frame/seek/reset mixing; frozen buffered submit; asynchronous flush barrier vs backend durable sync; graceful close vs abort/cancel with unsent/partial/unknown progress. Verify bounded service retention and no frontend lexer/grammar in core. |
| Message transport      | When an adapter advertises message capability, receive exactly one bounded message per request, including zero-length messages, and send exactly one frozen message per request; preserve boundaries and identity through coalescing. Test empty then nonempty receives, two independent sends, oversized-message consumed/retained outcome, typed refusal when unsupported, and no synthesized stream EOF.                        |
| Submit multiplicity    | Fresh explicit execution after backtrack creates a new effect; automatic resume and duplicate completion wakeups do not.                                                                                                                                                                                                                                                                                                           |
| Write semantics        | Freeze output before submit; exercise partial transfer, cancellation races, ordering, graceful/abortive close, and rebind.                                                                                                                                                                                                                                                                                                         |
| Capture overlap        | Repeated and concurrent overlapping captures reuse permanent addresses across capture calls, without equal-content deduplication.                                                                                                                                                                                                                                                                                                  |
| Capture failure        | Inject failures around append/map/Structure publication and recover translations without dangling durable references.                                                                                                                                                                                                                                                                                                              |
| Actor I/O boundary     | Reject direct `Submit`/`Await` before an actor turn runs; stage only handler intents; persist and await admission proof off-thread before one service submission; refuse uncertain restore/replay; deliver completion in a later owner turn; enforce grant and ledger bounds; test no-resubmission across older checkpoints.                                                                                                       |
| Frontend/core boundary | Exercise the same generic capability trace from direct Vortex and VQL, VPL, vprolog, Vlace, VIcon, and VAgent lowering; verify equivalent admission/Pending/Await/outcome behavior and that I/O changes add no frontend names/ASTs/callback state/opcode policy in `VortexCore`/`VortexVM` beyond the documented legacy baseline; also verify generic host-profile actor refusal.                                                  |
| Policy preservation    | VAgent post-commit/correlation semantics and Evidence grants, attempts, uncertainty, late receipts, and no-silent-retry behavior remain intact.                                                                                                                                                                                                                                                                                    |
| Replay                 | Restored live descriptors stay inert; missing captured evidence fails explicitly and makes no live call.                                                                                                                                                                                                                                                                                                                           |
| Mutable file identity  | Rename/replace the path, including same-path replacement; rewrite the same inode, truncate, and mutate externally. Distinguish locator, live generation, immutable read observation, typed stale/conflict/unknown results, generation-keyed raw/decoder caches, and physical lease reclamation. Verify no path/stat/watch/fd shortcut claims an unsupported snapshot.                                                              |
| File write conflicts   | Queue reads and writes around service-owned and external writes; check expected-generation validation, backend atomicity declarations, partial/uncertain outcomes, and refusal where requested strict semantics are unsupported. Optional EDL overlay export conflicts explicitly.                                                                                                                                                 |
| Large media ranges     | Seek and cancel across byte ranges, including offsets above 4 GiB; check offset/length/stride overflow and integer byte units. Bound encoded chunks, indexes, decoded frame/audio cache, and GPU surfaces independently. Time/frame/sample mapping and codec dependency invalidation stay adapter-owned, with no VM media opcode.                                                                                                  |
| Media capture          | Capture selected overlapping ranges from one observation and a later observation of the same path; only same-origin overlap reuses permanent translations. Separately verify playable-artifact capture includes declared headers/index/dependencies, stays bounded, and is never implied by selected-byte capture.                                                                                                                 |
| Front ends             | Verify VQL generators; VPL request-position/shape plus same-request Pending re-await, payload/status gating, and guarded input/output dependencies; VProlog choice; VLace handlers; VIcon yield versus async stdout sink; and VAgent intention Pending/resume behavior.                                                                                                                                                            |

The current stream/relation tests in section 2 remain regression anchors. The marked-await,
transient lease, pressure, overlap, replay, and cancellation scenarios above are proposed tests;
this document does not claim they exist or pass. Performance comparisons need measured latency,
throughput, peak retained memory, and allocation costs on reproducible workloads.

## 12. Proposal register

The owner approved P1–P7 on 2026-11-23. This approval authorizes the proposed work to proceed,
subject to the acceptance gates below; the designs remain drafts and no implementation is claimed.
Each entry names an initial concrete file scope; its acceptance gate may narrow or revise that scope
before code changes. If a gate proves a behavior composes without touching a listed existing file,
remove that file from the final implementation plan and record the evidence.

| Proposal                                                         | Existing files and reason                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        | Why not caller/Vortex-only                                                                                                                                                                                                                                                       | Cost to measure                                                                                                                                                                                                                                                                                | Fallback if declined                                                                                                                                                                                                           |
| ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **P1 — Pending execution and owner continuation**                | `apps/common/xanadu/vortex/vortex_core.hpp/.cpp` for typed execution status; `vortex_vm.hpp/.cpp` for parked cursor/frames/choice-safe resume; `vortex_host.hpp/.cpp` for owner pumping, completion validation, and resumption. VM/core execution results and continuations are language-neutral; new or expanded I/O work adds no frontend names, ASTs, effect policy, or frontend-local continuation state to core/VM.                                                                                                                                                                                                                                                                                                                                                                                                                         | A caller cannot preserve VM-internal cursor, guard, frame, trail, and choice state or safely resume `step`; separate caller protocols would diverge. Vortex library code cannot own OS scheduling or host completion delivery.                                                   | Parked VM state per continuation; scheduler/completion queue bytes; enqueue/dequeue and validation cycles; per-turn pump work.                                                                                                                                                                 | No asynchronous await feature. Do not substitute blocking providers, recursive VM calls, or caller-runs handlers.                                                                                                              |
| **P2 — Typed transient cell resolution and lifetime validation** | `apps/common/xanadu/zigzag/arena_manifold.hpp/.cpp` for resolver-backed cell content and lease-aware mark/trail/release ownership; `apps/common/xanadu/zigzag/cell_views.hpp` only if shared typed views need extension; `apps/common/xanadu/vortex/vortex_vm.hpp/.cpp` for managed continuation roots if required.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              | Callers cannot make transient bytes participate in normal cell reads/slices, enforce shared lifetime rules, or detect dense-slot reuse consistently across languages. A Vortex-only encoding cannot make foreign ranges valid `PrimediaSpan`s or attach arena release ownership. | Resolver dispatch and slice cost; per-range/lease metadata; retained bytes under aliases, marks, and trails; root acquire/release work.                                                                                                                                                        | Transient bytes remain opaque native buffers; programs cannot read/slice them as cells and no persistence path accepts them.                                                                                                   |
| **P3 — Explicit durable capture and recovery**                   | `apps/common/xanadu/user_permascroll.hpp/.cpp` for append ownership; `store.hpp/.cpp` for durable Structure references if required; `zigzag/arena_manifold.hpp/.cpp` for promotion/translation input if required. Add a journal file only if the selected recovery protocol needs a new on-disk format; then bump version and regenerate fixtures.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               | Caller code cannot coordinate append-only permascroll writes with durable translations and Structure publication across capture callers. Vortex alone cannot make append/recovery atomic or own the user's permascroll.                                                          | Translation/journal bytes per interval; owner planning work; append and sync latency; recovery scan; orphaned append bytes after failure.                                                                                                                                                      | Capture is unavailable; transient payloads stay transient and promotion refuses them.                                                                                                                                          |
| **P4 — Shared language effect and resume bridge**                | Initial migration sites: `apps/common/xanadu/vql/compile_steps.cpp` and `vql_engine.cpp`; `vpl/graph_emit.cpp` and `vpl/execution.cpp`; `vprolog/compiler.cpp` and `vlog.cpp`; `vlace/lower_io.cpp`, `lower_effects.cpp`, `driver.cpp`, and `run_vm.cpp`; `vicon/compiler.cpp` and `driver.cpp`; `vagent/compiler.cpp`, `executor.cpp`, and `adapter.cpp`. Compilers/adapters own syntax, types, purity/effect placement, multiplicity, lowering, and frontend-local continuation state; all lower to the same generic cell/capability protocol. The gate may narrow this list if no source change is needed.                                                                                                                                                                                                                                    | A single caller cannot impose static placement and evaluation-multiplicity rules across frontends, and separate frontend protocols would diverge in submit/resume behavior. Vortex alone cannot encode each language's purity/evaluation model.                                  | Added code/cell costs, static-check time, pending-state bytes, resumption overhead, and bridge-specific materialization work.                                                                                                                                                                  | Leave that frontend without async I/O submission/await; no blocking compatibility bridge or unreviewed frontend-specific effect semantics.                                                                                     |
| **P5 — Actor post-commit I/O events and recovery proof**         | `apps/common/xanadu/vortex/mesh_actor.hpp/.cpp` for actor-grant limits, turn admission, completion delivery, checkpoint and restore refusal; `mesh_checkpoint.hpp/.cpp` for a versioned continuation causal floor; new `mesh_actor_io_journal.hpp/.cpp` for a versioned, bounded durable attempt/receipt log written before external submission (transport idempotency metadata only; logical intent/outcome state remains in cells); `apps/common/xanadu/identity/identity_layout.hpp` and `actor_wire.cpp` for stable actor turn/intent correlation; `apps/common/xanadu/vagent/checker.cpp`, `compiler.cpp`, `executor.hpp/.cpp`, and `adapter.hpp/.cpp` for VAgent staged-intent identity/preflight/dispatch only. Actor admission and checkpoint behavior use a generic host execution profile; no VAgent-name branch enters VortexCore/VM. | A caller cannot preserve actor mailbox atomicity or prevent checkpoint replay from duplicating a committed external effect. The actor runtime owns the causal floor and restore gate; Vortex cannot infer durable service admission from a live arena mark.                      | Bounded journal bytes and records; async flush queue bytes/latency; recovery scan and floor reconciliation work; stable key/correlation metadata; completion queue bytes.                                                                                                                      | Until durable proof is available and reconciled, refuse actor-profile I/O and restore/replay with typed `Unknown`/`Unavailable`; never block evaluation/UI threads or resubmit. Proof metadata does not capture payload bytes. |
| **P6 — User-configurable I/O budgets**                           | `apps/common/xanadu/system_docs.hpp/.cpp` for setting names, schemas, defaults, and notes; `apps/common/xanadu/vortex/vortex_host.hpp/.cpp` for resolving a generation-scoped immutable service budget snapshot.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 | Call-site flags or Vortex bytecode cannot provide persistent user-editable system settings with the repository's schema/default/notes behavior.                                                                                                                                  | Setting-cell count and storage; snapshot resolution cost; accounting bytes/work per admitted request; migration cost for existing settings stores.                                                                                                                                             | Keep the service budget immutable and explicitly supplied at host construction, using documented backend invariants only; do not claim user-adjustable system settings until these settings are implemented and proven.        |
| **P7 — Mutable-file versions and bounded rich-media ranges**     | `apps/common/xanadu/zigzag/filesystem_lens.hpp/.cpp` only if the lens gains content access; a new/selected `apps/common/xanadu` file-source adapter and resolver; `include/gleditor/media_stream.hpp` and `src/media_stream.cpp` only if the generic path-backed stream is adapted to expose declared consistency; `apps/common/xanadu/publication.cpp` and `apps/common/ui/xanadoc/views.cpp` for bounded staged-media read/capture paths. Keep media time/codec policy in adapters, not Vortex VM opcodes.                                                                                                                                                                                                                                                                                                                                     | Current file lenses provide locator/metadata, FileMediaStream reopens a mutable path per read, and current import/publication read whole files. They cannot promise retained observation identity, atomic conflict handling, or budgeted large-media capture.                    | Version-token and observation metadata per interval; encoded-range leases; bytes/seek work per admitted read; decoder/index/GPU cache bytes; playable dependency inventory and selection work; invalidation and cleanup costs. Backend guarantees and identity mapping are scoped per adapter. | Keep the lens metadata-only and live file edits unsupported; serve only best-effort read observations or refuse strict snapshot/write semantics; do not capture unless explicitly selected and bounded.                        |

Any additional existing runtime, compiler, or stdlib file discovered by a gate becomes a separately
priced `P<n>` before implementation. These entries do not authorize source edits. The unresolved
mechanism choices and proof gates are listed next.

## 13. Accepted decisions and proof gates

The owner accepted the U-1–U-8 recommendations on 2026-11-23. These decisions narrow the proposed
contract and defaults; they do not pass implementation milestones. Each proof gate below remains
open until its named real-code evidence exists. Standalone models and host-OS observations inform
the design but do not satisfy production gates.

| ID  | Accepted resolution                                                                                                                                                                                                                                    | Interim default / prohibited assumption                                                                                                                                                                                  | Proof gate retained                                                                                                                                                                                                                                                                                       |
| --- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| U-1 | Use tagged private reciprocal occurrences, with posward clone views to shared endpoint/capability cells; slice and serialize payloads only through the typed resolver, which refuses transient persistence.                                            | Preserve the reciprocal clone/occurrence topology in §6.2; do not create fan-in or treat native IDs as the cell API.                                                                                                     | M0: real `ArenaManifold` cells must prove shared endpoint/capability traversal, reciprocal queues, typed binary read/slice, and transient persistence refusal. The standalone topology model is not an M0 pass.                                                                                           |
| U-2 | Validate allocation incarnation and explicit lease ownership through marks, trails, overwritten slots, frames, proxies, cuts, exceptions, and arena destruction.                                                                                       | `contains(ref)` and arena generation alone do not validate identity; no native index may retain stale `CellRef`s.                                                                                                        | M0: integrated release/reuse and exception tests must prove acquire/transfer/release on every lifecycle path, including same-dense-index remint. The modeled incarnation check leaves M0 open.                                                                                                            |
| U-3 | Persist capture intent, append only unmapped source intervals, then durably bind an append receipt to that intent before publishing Structure. Orphan append bytes are allowed but never guessed; refuse overlapping capture while state is uncertain. | On ambiguity, refuse further overlapping capture until reconciliation; never guess or silently append a duplicate interval.                                                                                              | M6: real permascroll and Structure integration must inject failures around every durable step and prove restart reconciliation, mapping reuse, valid permanent spans, and orphan handling. The filesystem model is not an M6 pass.                                                                        |
| U-4 | Keep explicit immutable host budget snapshots until numeric defaults are measured. Reserve request, metadata, terminal/control, and declared payload capacity before admission; calibrate user settings and control reserve from pressure evidence.    | Require a supplied immutable host/service snapshot and named backend invariants; do not claim user-adjustable settings or hide ceilings in `IoService`.                                                                  | M8: measure every charge class and owner-turn work under delayed-backend pressure; then P6 may add selected controls through `system_docs.hpp/.cpp` with schema/default/notes and host resolution. No numeric defaults or settings gate is approved by this decision alone.                               |
| U-5 | Keep the six planned bridges, with each frontend owning its effect policy and diagnostics under the shared typed contract. VQL is initially outside the implementation-plan rows until its existing coupling is accounted for.                         | The shared static effect and typed-outcome contract in §4 applies; no bridge may infer submission from rendering/query traversal.                                                                                        | M7: each bridge must specify static positions, multiplicity, recovery, diagnostics, and typed outcome use, then pass repeated-evaluation and backtracking tests. The actual frontend spike is blocked until the service and frontend implementations are available; no bridge gate is passed.             |
| U-6 | Require a durable actor intent acknowledgement off-thread after commit and before dispatch. On restore, missing, unknown, or unreconciled proof yields `Unknown` and refusal; never automatically resubmit.                                            | Dispatch only after the stable attempt record is durably acknowledged off-thread. Missing, unknown, or unreconciled proof refuses actor I/O and actor-profile restore/replay; no automatic retry and no payload capture. | M3: real actor/journal/checkpoint integration must inject crashes before and after journal flush, service submission, receipt, checkpoint publication, and older-causal-floor restore; prove bounded retention, reconciliation, and asynchronous recovery. The Python filesystem model is not an M3 pass. |
| U-7 | Treat the first strict source as immutable. Keep live observations bound to the exact bytes materialized; require backend evidence before advertising snapshot or compare-and-swap guarantees.                                                         | Locator is not origin; a materialized observation remains readable under leases. Watchers/stat checks are hints; no atomic snapshot or CAS promise without backend support. Strict unsupported operations refuse.        | M5: test immutable Git blobs and mutable worktrees, replacement, truncation, in-place and external writes, queued operations, and same-path rereads; verify exact stale/conflict/unknown outcomes and no cross-observation dedup. Linux/Git observations do not prove a production adapter guarantee.     |
| U-8 | Use bounded byte ranges with separate encoded, index, decoded frame/audio, and GPU-surface budgets. Treat playable-artifact closure as explicit capture and refuse when dependency evidence is absent.                                                 | Byte ranges are source data; decoded frame/audio values are derived and separately bounded. Time/codec/dependency logic stays in adapters; no whole-file implicit capture or VM media opcode.                            | M5: exercise large offsets, overflow, random seek, cancellation, decode/cache eviction, selected-range overlap capture, and playable-artifact dependency closure/refusal with measured budgets. Available fixture/tool probes are not an M5 pass.                                                         |

M1/M2/M4 also remain open: their delayed backend, lifetime/stream, and source-adapter integration
evidence is still pending. M7's frontend integration evidence is blocked until the service and
frontend work exists. No probe recorded in the spike index automatically satisfies a production
milestone; use the implementation plan's named acceptance tests and dependencies when advancing a
gate.

## 14. Version history

| Version | Date       | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ------- | ---------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0.1     | 2026-11-19 | First on-disk pass: source-verified current state, settled decisions, and architecture review gates.                                                                                                                                                                                                                                                                                                                                                      |
| 0.2     | 2026-11-19 | Expanded after peer review: current/planned lenses and language bridges; complete asynchronous cell boundary; request/endpoint lifecycle; marked await and incarnation safety; semantic roots and bounded reclamation; capture overlap/recovery; policy-preserving migration and acceptance gates.                                                                                                                                                        |
| 0.3     | 2026-11-19 | Round-one critique revision: shared effect and outcome contract, reciprocal occurrence topology, state ownership boundary, bounded owner work and budget inventory, concrete proposal register, and gated open questions.                                                                                                                                                                                                                                 |
| 0.4     | 2026-11-19 | Round-two revision as first written: actor turn/checkpoint policy and interim settings behavior; the P5/P6 rows and matching U-4 fallback were omitted and detected in focused round three.                                                                                                                                                                                                                                                               |
| 0.5     | 2026-11-19 | Corrected the proposal register with P5 actor staging/dispatch and P6 system settings; clarified Submit/Await legality in atomic actor turns and aligned U-4 with the immutable interim fallback.                                                                                                                                                                                                                                                         |
| 0.6     | 2026-11-19 | Round-four recovery revision: P5 requires off-thread durable attempt proof before actor service submission, causal-floor reconciliation, and typed refusal of uncertain restore/replay; added U-6 and a bounded ledger gate.                                                                                                                                                                                                                              |
| 0.7     | 2026-11-19 | Round-five layering revision: made the language-neutral Vortex core/VM boundary normative, assigned frontend semantics to compilers/adapters, kept actor policy in a generic host profile, and added cross-frontend capability-trace checks. Existing VQL-specific VM coupling is recorded as separate debt.                                                                                                                                              |
| 0.8     | 2026-11-20 | Owner-requested mutable-file and rich-media extension: distinguished locators, live generations, retained read observations, and immutable permascroll identity; specified adapter-declared snapshot/write guarantees, bounded byte-range media/decode lifetimes, explicit playable-artifact capture, and new acceptance gates. Added a separate source analysis; the prior six-critic convergence remains historical and does not review this extension. |
| 0.9     | 2026-11-20 | Added concrete proposed API and per-frontend exposure traces, lifecycle-stage distinctions, typed terminal examples, current six-language roster context, and explicit preproduction ABI freedom; retained the v0.8 historical review record.                                                                                                                                                                                                             |
| 0.10    | 2026-11-22 | Round-one stakeholder revision: specified optional message-mode boundaries and typed refusal/oversize behavior; corrected VPL shape-preserving syntax and added framed input/output examples; separated VIcon yield, pure render, and async stdout sink; repaired the Direct Vortex table cell.                                                                                                                                                           |
| 0.11    | 2026-11-22 | Round-two VPL clarification: separated Pending execution from terminal outcomes in the framed input/output traces; same-request re-awaits Pending, payload access is terminal and status-gated, and dependent next-read/close/abort operations await and inspect their outcomes.                                                                                                                                                                          |
| 0.12    | 2026-11-23 | Recorded owner approval of P1–P7 and updated current approval-dependent wording; retained draft status and all implementation and proof gates. The v0.11 reviewed target remains a historical snapshot; its review records are unchanged.                                                                                                                                                                                                                 |
| 0.13    | 2026-11-23 | Recorded owner acceptance of U-1–U-8 recommendations with concise resolutions and probe limitations; all production gates remain open. The v0.11 implementation plan and prior reviews remain frozen.                                                                                                                                                                                                                                                     |
