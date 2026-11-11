# VQL beyond lenses: an annotatable work environment

**Status:** Workflow architecture proposal, 2026-10-21. The prerequisite repairs in §1.2 are
implemented as recorded in §1.3; the external workflow extensions remain proposals. The subsequent
[four-round critique](projects/vql-extension-reviews/critique-rounds.md) quiesced across all five
reviewer perspectives after additional execution repairs and contract corrections.

**Reviewed baseline:** main `7617d3f0`, including the requested VQL implementation from
`worktree-bridge-cse_01W9M6tyvaLMw6tcNVDeWkzk`. Initial reviews examined `732b8280`; supplements
reconcile the later implementation.

VQL could let a person return to a decision and walk through the exact database values, service
responses, command output, and conversations that supported it, then inspect the actions taken and
their consequences. Every part of that chain should admit notes, quotations, links, alternatives,
and its own history.

The useful extension is a shared model of **subjects, evidence, occurrences, and work**. ZZ9P can
carry manifold operations; lenses can project sources. Neither, by itself, specifies what an old
annotation means after refresh, whether a failed request had an external effect, or how an
investigation resumes across several services.

## 1. Evidence and the existing design

Read this alongside [the VQL specification](vql-query-language.md),
[Anything-to-ZigZag](anything-to-zigzag-lens.md), the
[lens implementation plan](zigzag-lenses-implementation-plan.md), and
[store/slice convergence](store-slice-convergence.md). Existing proposals already cover shapes,
functions, perspectives, action cells, watches, source identity maps, and structured-source
unprojection. Adding a connector name is insufficient as a language extension.

Five requested perspectives informed this proposal: a Xanadu/ZigZag Purist, technical writer,
Vortex/systems expert, language/ergonomics expert, and a person integrating everyday work. All ran
as `gpt-6.1-sol` agents at medium effort. Their reports are in
[the review directory](projects/vql-extension-reviews/README.md).

### 1.1 Baseline before repairs

This is a scoped source inspection, not certification of every subsystem. The table records the
reviewed baseline before the repairs; §1.3 records the delivered changes.

| Area                   | Inspected implementation                                                                                         | Already specified beyond that                                         | Extension proposed here                                               |
| ---------------------- | ---------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------- | --------------------------------------------------------------------- |
| VQL execution          | Direct evaluator, parser, compiler, shapes/functions and local multi-store composition                           | General lazy pipelines and comprehensive compiler equivalence         | Shared plans with cardinality, effects, coverage and lineage          |
| V16 types and functors | The requested update adds direct-evaluator contracts, typed destructuring, return checks and bound path functors | Specialized functor opcode and fixed-size lowering                    | Safer workflow contract modes and explicit coercion policy            |
| Compiled V16           | Compiler refuses functor bindings; type annotations are erased without checking                                  | Checked gradual typing at boundaries                                  | Conformance matrix and enforced checks before effect dispatch         |
| Projection             | Session enumerates nodes/edges into an arena and maintains key maps                                              | Generic commit/refresh, span-aware source content and broad writeback | Durable annotation/evidence closure and guarded source actions        |
| Observation            | Direct `executeWatch` evaluates current matches; catch-up is explicitly simulated                                | Suspended watches, replay, live deltas and ZZ9P event streaming       | Source coverage, gaps, demand, durable checkpoints                    |
| Source discovery       | Named-store resolution and ambient-home fallback in generator paths                                              | Native distributed discovery and universal lens dispatch              | Explicit unsupported/unavailable outcomes and connection contracts    |
| External work          | Command transaction/promise structures; actor host, continuation and checkpoint machinery                        | General source orchestration and control                              | Durable intent, attempt, receipt, reconciliation and workflow records |

Inspect [the evaluator](../apps/common/xanadu/vql/vql_engine.cpp),
[compiler](../apps/common/xanadu/vql/compiler.cpp),
[projection interface](../apps/common/xanadu/zigzag/projection_lens.hpp),
[session](../apps/common/xanadu/zigzag/projection_session.hpp),
[command mailbox](../apps/common/xanadu/zigzag/command_transaction.hpp),
[actor model](../apps/common/xanadu/vortex/mesh_actor.hpp), and
[mesh host](../apps/common/xanadu/vortex/mesh_host.hpp).

The VQL specification's new §4.12.5 usefully distinguishes direct evaluation from proposed lowering.
The introduction and conformance matrix now identify the implemented engine boundaries. Codec
round-trips establish encoding; parsing establishes accepted syntax. Neither establishes an
integrated service or resumable journey.

### 1.2 Prerequisite issue register

This register records the issues identified before repair. Delivered scope is in §1.3; remaining
grammar, effects and workflow features are explicitly proposed.

1. **Publish conformance by engine.** Separate syntax accepted, direct semantics, compiled
   semantics, persistence, and service integration. Unsupported generators must refuse instead of
   selecting the ambient home and producing a plausible answer to another question.
1. **Choose view and edit semantics explicitly.** §4.8 describes shape application as severing
   omitted links; §4.10 uses projection for a virtual view. Recommend ordinary
   `project(subject, shape)` for a view and effectful `apply_shape(subject, shape)` for edits. A
   normal annotation adds structure; it must not accidentally replace the subject's neighborhood.
1. **Reconcile cardinality and quantification.** §3's existential comparisons contradict §4.3's
   prohibition. Specify stream/stream comparisons, empty inputs and the existing existential
   inequality trap. Make the domain of universal quantification explicit.
1. **Separate editable identity from equal content.** Identical files are not automatically one
   `d.clone` authority. A quotation shares identified spans; a reference identifies another subject;
   repeated membership has its own occurrence. Repair the clone-chain examples and specify old-rank
   collisions before using implicit clones as universal fan-out.
1. **Use the converged primitive rules throughout.** Remove contradictory cell-zero and allocation
   sentinel claims from the normative language description. Keep optional absence in APIs and
   bounded wire/on-disk encodings at their actual boundary.
1. **Treat immutable evidence correctly.** Permascroll bytes do not change. A new document assembly
   or live selector can change; an exact frozen quotation stays pinned. Backlink discovery returns
   known accessible results with stated coverage.
1. **Replace absolute performance claims with measured scope.** Arbitrary computed dimensions,
   joins, sorts, source I/O and retention have costs. The current direct engine returns vectors and
   uses hash maps. Fixed descriptor sizes, SIMD strategies, ABI passage and nanosecond claims are
   implementation hypotheses, with fallbacks and benchmarks owed.
1. **Make documented examples a conformance corpus.** The baseline EBNF gaps included empty shapes,
   named arguments, arithmetic/conditional values and several example spellings. Empty shapes are
   now accepted; named arguments and arithmetic/conditional values remain outside the delivered
   subset. The worktree update resolves the banner and V16 contract grammar/parser gaps reported in
   the initial reviews.

### 1.3 Implementation record

The prerequisite repair implementation is documented in [engine conformance](vql-conformance.md) and
the [performance spike](vql-performance-spike.md).

| Repair                        | Delivered boundary                                                                                                                            |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| 1. Conformance                | Full-query compiler preflight; explicit source/watch/version refusals; per-engine matrix                                                      |
| 2. Views/edits                | Detached project masks preserve reciprocal source links and span provenance; apply_shape edits explicitly; ambiguous bare application refuses |
| 3. Cardinality/quantification | Cartesian existential stream comparisons, explicit-domain quantifiers, cardinality and same_cell helpers                                      |
| 4. Identity                   | Ordered leftmost clone authority, occupied-rank rejection, content-equality/dedup documentation corrected                                     |
| 5. Primitive rules            | Literal cell zero/overflow and numeric link targets refuse; allocation uses percent creation, explicit masks isolate                          |
| 6. Evidence                   | Immutable quotation and retained-history coverage semantics corrected; simulated watch/delta success removed                                  |
| 7. Measured scope             | Configurable real evaluator/functor allocation/timing spike, descriptor layouts and compiler-generated ABI evidence                           |
| 8. Examples                   | File-backed common-engine and direct-only corpus, parser/engine refusal fixtures, whole-input parser validation                               |

These repairs do not introduce distributed discovery, external connectors, suspended watches, source
writeback or arbitrary rank compilation. Unsupported behavior refuses instead of silently claiming
those semantics. The compiler's verified creation/identity subset and the direct evaluator's broader
arena subset are tested separately. Exact decimal/null/effect systems and the shared workflow
records below remain subsequent work.

## 2. The shared structural model

These are open structural contracts and library conventions over ordinary cells, not a new class
hierarchy, storage format, or collection of privileged dimensions.

| Record      | Identity and meaning                                                                                   | Things that can be annotated                                                    |
| ----------- | ------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------- |
| Connection  | Versioned local source/account definition, adapter and schema edition, capabilities and capture policy | Endpoint, account claim, schema, scope, auth failure and policy                 |
| Subject     | Durable local continuity claim about an external entity                                                | Row, resource, message, actor, command template or conversation                 |
| Field       | Durable field/schema identity, separate from its observed values                                       | Column, property, header, argument slot and continuity claims                   |
| Observation | Immutable witnessed state of a subject or query execution                                              | Source revision, raw representation, typed field, capture time and completeness |
| Occurrence  | One membership, relationship, event, result position or delivery                                       | FK relation, recipient, repeated row, channel event, order and role             |
| Anchor      | Explicit target and follow/pin policy for a note or quotation                                          | Subject, field, observation, exact span, occurrence, failure or action          |
| Derivation  | Program edition, parameters, inputs, transformations and output mapping                                | Aggregate, computed field, normalization, explanation or failed computation     |
| Intent      | Requested external work, exact arguments, target and preconditions                                     | Draft command, SQL patch, request, audience and authority requirements          |
| Attempt     | One dispatch of an intent revision                                                                     | Start, progress, output chunks, retries, cancellation and uncertain outcome     |
| Receipt     | Evidence of acceptance, completion, rejection or subsequent reconciliation                             | Protocol response, affected rows, status lookup and recipient-specific outcome  |
| Workflow    | Versioned definition plus separate runs, dependencies and checkpoints                                  | Schedule, branches, decisions, step state, gaps and recovery                    |

### 2.1 Stable identity requires a continuity policy

A projected `CellRef` expires with its arena/session. Durable notes cannot store it as a
cross-session or cross-store address. Resolve or mint a persistent local subject through normal
Structure operations; use the existing store/global operation-reference machinery when published.
External bindings remain explicit assertions:

```text
source authority + account/namespace + source incarnation + external key
    -> local subject
local subject + witnessed revision/capture
    -> observation
observation + field/relationship/event occurrence
    -> exact annotation target
```

A primary key, inode, PID, URL, nickname or Message-ID is not a universal permanent identity.
Adapters declare reuse and rename behavior. An uncertain match produces a continuity claim with
evidence and alternatives, rather than silently moving old notes. Equal bytes do not establish
identity. A keyless source may support observation-local identity only.

A subject-following field note needs a stable field subject scoped to its entity/schema, distinct
from each observation's field occurrence. Column rename, drop/recreate and reused JSON keys can
change meaning; preserve schema editions and explicit correspondence evidence. A display name or
JSON pointer is not sufficient proof of field continuity.

### 2.2 Relationships are annotation subjects

The lens proposal §8A calls reified relationships awkward phantom nodes. For this extension,
addressable relationship occurrences are essential: a foreign key, recipient occurrence, reply,
reaction, join match or delivery can itself be disputed and annotated.

Keep the two-neighbor invariant on every dimension. A container reaches its first membership on
`d.contains`; memberships form a sibling rank on `d.seq`. Each membership resolves its subject
through a reference/facet with enough incidence structure to keep both directions within valence.
The same subject can occur in many collections without stealing another collection's links.
Efficient internal edges may coexist with these addressable occurrence cells.

Provide `children`, `members` and `descendants` helpers with defined inclusion, order, duplicate and
cycle policies. They must include the first member; a naive `contains/seq` composition can omit it
because ordinary rank traversal starts beyond its context.

### 2.3 Evidence capture and transclusion

An external system ordinarily has no Xanadu primedia addresses. Exact durable quotation therefore
requires an initial retained capture of the relevant bytes, with source locator, revision,
representation, acquisition context and verification evidence. This import has a real storage cost.
Later reuse can transclude the captured immutable spans.

Record the observer's capture separately from the claimed source author. Signing a capture does not
establish the remote author's signature. Raw bytes, decoded text and derived values are different
artifacts: MIME decoding, JSON escapes and normalization need mappings where feasible; computed
totals need derivation links, not invented character provenance.

A lazy BLOB/body reference is useful for browsing. Its existence does not guarantee historical
availability. Annotating it either retains the required evidence closure or clearly records a
reference-only anchor whose content may become unavailable.

### 2.4 What “everything is annotatable” commits to

Every exposed semantic item must offer an addressable anchor, including connection schemas,
arguments, individual fields, relationships, pending work, errors, capture gaps, stream chunks,
receipts and checkpoints. Annotations are addressable too.

The operation that saves a note on a transient item must:

1. Resolve its subject, observation and anchor policy.
1. Apply the chosen retention policy and expose retained scope and budget/access limits. Ask for a
   new decision only when the requested scope exceeds that policy.
1. Persist the required identities, mappings and selected evidence into the chosen private store.
1. Attach the note using persistent references after promotion remaps scratch identities.

Default an exact quotation to its observation. Let the person explicitly choose a subject-following
note. Refresh creates another observation and shows correspondence; it never silently reanchors a
quotation on different text. A deleted source can have a tombstone while retained evidence remains
separately inspectable under its retention policy.

Browsing and speculative projection stay ephemeral. Saving notes, retaining evidence and recording
authorized work append to their designated stores; they do not append to visited sources. This
extends R8 through explicit destinations and authorization. It does not introduce a sixth system
configuration store or conflate workflow history with reader navigation activity.

## 3. Language improvements that make the extension usable

### 3.1 Cardinality, values and identity

Keep `/d.name` as the existing whole-rank traversal. Add library operations for
`neighbor(cell, dim)`, `one(stream)`, `optional_one(stream)` and `first(stream)`. One-hop structural
contracts and rank-valued paths must not be taught as interchangeable.

The new direct V16 implementation establishes non-stream cardinality and optional fields in type
contracts, and preserves cell references in `let $y := $x`. Extend those distinctions consistently
to value expressions, where selecting/rendering the first result must be explicit.

Use exact typed value equality and named `same_cell`, `same_subject`, `same_observation` and
`same_occurrence` predicates. Exact integers, identifiers and money must survive conversion;
approximate numeric comparison belongs in an explicitly named operation. Decimal/exact values,
binary data and source representations need structural library contracts.

Represent absence, source null, empty text, denied/redacted fields, pending acquisition, failure,
partial capture and gaps as distinct states. `T?` means optional presence. A default handles
absence; it must not silently replace a failed request or SQL NULL.

Make `all(stream, predicate)` the canonical universal quantifier, with corresponding `any` and
`none` overloads. The filtered-path form can remain sugar with an explicit retained domain. Binding
a stream to a variable must not change what “all” means. A complete result over captured members
also needs a coverage claim before it supports a universal claim about the source.

### 3.2 Contracts and effects

The new implementation deliberately makes parameter-contract mismatch filter a call, while a
return-contract violation throws. Preserve this as an explicit query-matching mode. Workflow and
action boundaries also need a strict validation mode: silently dropping an invoice or action
recipient is an unacceptable way to report an invalid request.

V16 direct scalar contracts accept parseable text, so `"95"` can satisfy both String and Int.
Distinguish representation checking from coercion and parsed values; publish rules for overflow,
NaN, normalization, timezone, decimal scale, unions and defaults. Unknown type names refuse by name.
Compiled execution must enforce the required checks before dispatch even if other annotations are
erased.

Retain approachable `pure`/`weave` declarations, with richer inferred effects underneath: read
pinned evidence, read mutable external state, clock/random input, construct arena structure,
retain/persist, subscribe, spawn, and deliver externally. Higher-order calls propagate effects. No
persistent write is a different property from deterministic evaluation over explicit inputs.

Dispatch pins the connection, adapter and policy revisions. The host rechecks the effective target
and current authority immediately before I/O, including redirects, resource expansion and queued
work. It never forwards credentials to an ungranted target. Receipts retain permitted evidence of
the resolved target. Tests must cover an out-of-scope redirect and revocation between queueing and
execution.

A capability is host-bound authority with account/resource/operation scope, limits and revocation. A
cell containing its identifier conveys no authority by itself. Unknown plugin functions receive
conservative effects. Optimizers cannot duplicate, discard, cache or reorder effectful calls as
though they were pure traversals.

### 3.3 Query composition and explanation

Add reusable joins, groups, aggregates, distinctness, ordering and windows as libraries:

- Joins declare inner/left/semi/anti policy, key cardinality, null behavior and input coverage.
- Groups and aggregates retain member/field witnesses; duplicate result rows keep distinct
  occurrences. Distinctness names value or identity equivalence.
- A sorted view constructs an ephemeral result rank; persistent `sort_into` remains an edit.
- Windows declare event versus observation time, bounds, watermark/late-data policy and gaps.
  Unbounded sort/group/negative indexing requires a finite window or explicit resource policy.
- Derived outputs pin query edition, input observations and semantic policies. Recompute produces
  another result edition so old notes still address the earlier answer.

Provide queryable `describe`, `schema`, `explain`, `trace` and `why_empty` results. Show inferred
cardinality/effects/capabilities, source versions, completeness, pushdown, materialization,
predicate witnesses and resource use. Static explain performs no external action; bounded trace
explicitly runs work.

Most extensions fit ordinary functions and structural schemas. Reusable type aliases, callable
effect/cardinality signatures and tagged-state narrowing are plausible later grammar additions. No
source-specific SQL, HTTP, shell, mail, IRC or ActivityPub keyword is needed.

Keep rejected contract matches as inspectable occurrences when a workflow needs complete evidence.
Strict validation should expose accepted and rejected inputs together, with source observation,
field path and reason. A result's derivation includes its exclusions and coverage as well as its
contributing values.

Projection snapshots also need queryable source-to-result correspondence: source subject and
observation/version, copied cell, selected edges and chosen clone authority. The current detached
`project` returns roots and keeps its copy map locally; retained workflow results must persist this
correspondence as derivation records. Shared primedia spans alone do not reconstruct structural
identity. This record is proposed, rather than part of the current arena-only projection API.

## 4. External actions and resumable work

### 4.1 A shared asynchronous host boundary

Introduce a capability-limited host-service interface shared by VQL, VPL, Vlog and other Vortex
frontends. VQL constructs/query-shapes work; reusable Vortex routines compose it. C++ implements the
runtime suspension/ownership boundary and OS/protocol interfaces: database drivers, sockets/TLS,
process spawning and bounded transport.

A request carries an operation, canonical arguments, capability, limits, expected source version and
request identity. Submission returns a task handle immediately. Workers deliver owned values or
stable serialized identities in bounded batches to the owning execution context; only that owner
mutates its arena. Do not pass borrowed scratch spans or local CellRefs into another worker.

Use demand/credits, byte/row limits, deadlines and fair execution slices. Pin required source
lifetimes during asynchronous work; fence stale arena/continuation writes after cancellation or
generation change. Authenticated late completions for durable attempts still enter the attempt's
receipt/reconciliation inbox, recording arrival and cancellation causality. They must not revive a
cancelled continuation or dispatch downstream work. Reopening after cancellation must show late
acceptance without another send or stale arena write. Cancellation requests adapter cancellation and
releases local resources, while preserving any uncertainty about remote effects.

The existing MeshHost, worker contexts, actor admissions and message-boundary continuations are
useful patterns. They do not already provide a generic host-await primitive or integrated external
connector runtime.

### 4.2 Intent, dispatch, attempts and receipts

Build on private command transaction/promise cells from lens plan A6/T2. Drafting, navigating,
annotating or reopening an intent does not dispatch it. Source edits through a lens stage a guarded
intent; reconcile this explicitly with §4.11.3's automatic unprojection-on-completion design.

An explicitly authorized dispatch binds the exact intent revision, preconditions, target, audience,
capability and input evidence:

```text
draft -> validated intent -> durable dispatch record -> attempt
    -> accepted / rejected / partial / completed / failed / outcome unknown
    -> receipt or reconciliation evidence
```

Each workflow declares a dispatch-admission domain and execution owner. In its authoritative
journal, admitting a pending attempt is an atomic transition identifying executor and generation;
other recovering executors observe that claim and reconcile. Divergent offline journals or an
unavailable owner default to awaiting coordination. Explicit independent submissions are distinct
attempts with visible duplication risk. An expired lease does not prove the previous owner avoided
remote execution; takeover follows the unknown-outcome policy. Failure to durably admit an attempt
refuses before I/O. Test simultaneous recovery by two authorized executors, then loss of the
admitted owner after remote acceptance: no second ordinary submission or lease-triggered resend is
allowed.

Persist intent before execution and record receipts afterward. A crash between remote execution and
receipt persistence can leave an unknown outcome. Recovery must not automatically resend a mail,
repeat a command or charge an account. Reconcile against source receipts/status or a declared
idempotency contract. A local request ID alone cannot create remote exactly-once behavior.

Concurrent offline edits to an intent remain separate revisions. Merging body, target, audience or
preconditions creates another reviewable revision; prior dispatch authorization does not transfer
unless its explicit scope covers that exact result. Annotation and definition merges preserve
authorship and alternatives. Test disconnected body/audience edits followed by merge: no queued
attempt may send the synthesized revision under the earlier authorization.

Acceptance is distinct from completion and recipient delivery. Retrying creates a new attempt tied
to the original intent. Compensation is a new external action with its own limits and possible
failure. Hypertime undo restores local structure; it does not reverse the external world.

### 4.3 Observation, replay and checkpoint contracts

Differentiate four operations:

| Operation       | Contract                                                                       |
| --------------- | ------------------------------------------------------------------------------ |
| Replay          | Evaluate using retained observations and receipts; dispatch no external effect |
| Refresh         | Acquire a new observation with current authority and source state              |
| Retry/reconcile | Resolve or attempt specific unfinished work under explicit policy              |
| Rerun           | Create a new workflow run or action attempt from selected historical inputs    |

A subscription records source cursor, source incarnation, coverage, adapter edition, captured
events, gaps and local checkpoint. Commit a local event/work record and its acknowledged checkpoint
in one defined durability unit before acknowledging upstream. If storage spans stores, specify the
recovery protocol instead of assuming a multi-store atomic commit.

Duplicates can resolve to the same source-event identity while retaining separate delivery
occurrences. Expired cursors, unavailable IRC history and dropped/truncated output become gap
records. Polling a snapshot cannot reconstruct every intermediate state.

Use per-source order and version vectors/checkpoint sets for multi-source runs; no universal
timestamp or atomic snapshot exists across arbitrary services. Retained local hypertime is the
history of what was captured, rather than proof of complete remote history.

Checkpoint at quiescent message/effect boundaries. Persist definitions, completed effects and
meaningful input/output state; rederive traversal positions from pinned evidence. Re-admit
capabilities on restart. Durable workflow checkpoints do not imply persistence of the ephemeral
Vortex cursors that R8 discards.

A pending-work inbox is a query/view over the user's workflow records: show blocked dependencies,
unsent drafts, failed pages, expired authority, partial effects and unknown outcomes together. Offer
selective revalidation, reconciliation, retry or abandonment of one step without rerunning a
completed branch.

Scheduled definitions declare overlap behavior: skip, queue, coalesce or run concurrently.
Deduplication scope and source-event identity are explicit. A run pins the definition and authority
requirements it started with; a changed schedule, revoked grant or account switch cannot silently
authorize its old queued actions.

## 5. Source contracts and full extensions

Every adapter must describe schema, identity/continuity, value fidelity, supported operations,
consistency, coverage, authority, cancellation, retries and retention. Unsupported writeback is a
typed refusal.

### 5.1 SQL databases

Expose connection/database incarnation, schema editions, tables, columns, constraints, row subjects,
typed field observations, FK occurrences, query templates/parameters, executions, result positions
and transaction receipts. Preserve null, exact numeric representation, temporal semantics and BLOBs.

Start with parameterized native SQL plus structural results. Offer a structured builder later where
it improves composition. Push down pure predicates/projections/limits only when null, collation,
ordering, timezone, authority and snapshot semantics match; keep residual work local and explain it.
Batch compatible lookups to avoid one remote request per input cell.

Use stable source keys with explicit reuse/incarnation policy. Keyless rows default to
observation-local identity. SQLite documents that VACUUM may change rowids without an explicit
INTEGER PRIMARY KEY, so a bare rowid cannot justify permanent annotation continuity.
[SQLite VACUUM documentation](https://www.sqlite.org/lang_vacuum.html).

Writes stage typed patches, expected versions and source-local transactions. Affected-row counts,
constraint failures and rollback outcomes are receipts. The existing lens proposal §8A already
identifies batch, BLOB/null, schema, rekey and edge issues; complete and refine those contracts.
Source change capture is optional and declares gaps, retention and snapshot-to-stream handoff.

### 5.2 REST/HTTP endpoints

Expose request templates, methods, locators, parameter/header/body fields, pagination windows,
responses, status, validators, rate limits, raw captured bodies and decoded representations. Connect
resource subjects to response observations; a URL remains a useful locator without becoming proof of
eternal identity.

HTTP conditional requests can guard writes where the service supports them. Record validators and
conflict responses rather than overwrite stale data. Retry policy depends on operation semantics and
the service's idempotency support.
[HTTP semantics, preconditions and idempotence](https://www.rfc-editor.org/rfc/rfc9110.html).

A page-set/scan record pins request/filter edition, pagination lineage, source snapshot or validator
when available, acquisition interval, overlap/dedup policy and stability guarantee. Fetching all
pages and obtaining a complete coherent collection are separate claims. Resuming a stale cursor
starts a new scan edition or explicitly marks mixed-time coverage; it cannot silently combine
yesterday's pages with today's into a coherent snapshot. Test insertion, deletion and reordering
between pages and across restart. Earlier annotations remain pinned to their observations.

Each page has coverage and an annotatable request/response. A failed third page must not masquerade
as a complete smaller collection. Authentication errors and missing fields are different results.

### 5.3 Shell execution and history

Import shell history as evidence, preserving repeated identical entries and recording missing
timestamps, working directory, exit status and session context as unknown. Import never requests
execution. Searching and annotating history should work before implementing process dispatch.

New execution intents carry executable identity, argv, cwd, declared environment inputs, stdin
evidence, resource limits and output policy. Default to argv execution; shell interpretation is an
explicit mode with interpreter and script identity. Pipes become dependencies between stream/input
occurrences, with backpressure and independent stderr.

Link selected input artifacts and observed output artifacts to their versions, paths/locators and
capture quality. A path alone does not freeze the file bytes a command read. This supports an
annotatable dependency graph across commands, files, database queries and network observations.

An attempt exposes start/end, stdout/stderr chunks, exit/signal, truncation, output artifacts,
cancellation and child-process uncertainty. Capture raw terminal updates separately from a derived
terminal presentation. Every chunk and failure admits notes. Rerunning a historical invocation
creates another attempt against declared current or pinned inputs.

### 5.4 Email

Separate a raw message artifact, decoded MIME parts, interpreted message, mailbox memberships,
threading assertions, sender identity evidence, recipients, flags and send/delivery attempts.
Message-ID is useful evidence, not sufficient deduplication or identity.

IMAP identity is scoped to server/account/mailbox and UIDVALIDITY plus UID; sequence numbers are
positions. Changes of mailbox binding need explicit continuity evidence.
[IMAP message identity](https://www.rfc-editor.org/rfc/rfc9051.html#section-2.3.1.1). JMAP adapters
can use account-scoped object identities and state/change methods, with an explicit resync path when
history cannot be calculated.
[JMAP change tracking](https://www.rfc-editor.org/rfc/rfc8620.html#section-5.2).

Draft, send, flag, move and delete are distinct operations. Notes on quotes or attachments retain
their evidence and decoding mapping. Submission acceptance does not prove delivery. Reconnection and
ambiguous sends follow the common attempt/reconciliation model.

### 5.5 IRC

Expose network/session/channel identity, membership and topic observations, message events, sender
identity evidence, replies, local capture order and reconnect gaps. Nicknames are mutable labels.
Use server message IDs when available, scoped to their authority; retain honest local occurrence
identities otherwise.

Negotiate available history facilities and record their coverage. IRCv3 CHATHISTORY retrieves
available server/bouncer history, with capability and message-reference rules; it cannot guarantee
that every disconnected event remains available.
[IRCv3 CHATHISTORY specification](https://ircv3.net/specs/extensions/chathistory.html).

Sending, joining and moderation are capability-scoped intents. Rate limits, delivery uncertainty,
disconnects and failed recovery are annotatable. Duplicate delivery is different from a person
posting identical text twice.

### 5.6 Fediverse and ActivityPub

Separate actor/object identities, object observations, activities, collection memberships, replies,
reactions, boosts, visibility/audience and delivery occurrences. A Create or Update activity and its
object are different subjects. Deduplicate known activity identity without deleting delivery
history.

ActivityPub defines Update and Delete behavior, including possible Tombstones. Its client-to-server
and server-to-server roles differ; an adapter must declare which server API and role it implements.
An account's REST posting API is not automatically a universal ActivityPub endpoint.
[ActivityPub recommendation](https://www.w3.org/TR/activitypub/).

Append new observations for edits. Reflect deletions in the current view and record the treatment of
retained private evidence explicitly. Drafting, posting, replying, boosting and deleting are
separate intents with exact audience and evidence boundaries. Local acceptance cannot establish
successful delivery to every federated participant.

## 6. Journeys that justify the architecture

All API names below are **proposed library pseudocode**, not runnable examples. They use existing
VQL-shaped expression/function forms; package registration, signatures and strict effect syntax
remain design work.

Every acquisition returns an envelope, including pending, failed and partial work. Awaiting an
observation preserves that envelope and its receipts/coverage; success-only extraction is a separate
explicit operation. An HTTP error does not become an empty successful stream.

### 6.1 Explain an invoice using database and ticket evidence

Query an invoice, fetch its support ticket, retain the exact observations, annotate the amount field
and disputed response span, then create an email draft from selected evidence.

```vql
let $db_task := acquire($billing, $invoice_query, $capture_policy, $read_cap)
let $api_task := acquire($support, $ticket_request, $capture_policy, $read_cap)
let $db_observation := await_observation($db_task)
let $api_observation := await_observation($api_task)
let $case := case_create("Invoice 1042", list($db_observation, $api_observation))
let $draft := mail_draft($mail, $selected_evidence, $recipients)
return $case, $draft
```

Awaiting suspends through the host boundary. Acquisition may be ephemeral; the explicit case/capture
policy determines retained evidence. Page failure stays visible in the observation envelope. Refresh
shows the new amount alongside the annotated old amount. A correction stages a guarded SQL intent; a
stale row produces a conflict with base, proposal and current values.

### 6.2 Investigate an IRC incident through shell history

Link an IRC report to a historical deployment command and a REST health observation. Edit an argv
draft, explicitly dispatch the new attempt, then quote its stderr into an unsent incident reply.

```vql
let $history := history_import($history_source, $capture_policy)
let $prior := history_find($history, "deploy")
let $intent := process_draft($prior, $argument_patch)
return $intent

// A separate explicit action against the reviewed intent revision:
let $attempt := dispatch($sealed_intent, $run_cap)
return $attempt
```

Imported history never invents an exit code or working directory. Reopening the case runs nothing.
The attempt's result, stream gaps and uncertain cancellation remain addressable after restart.

### 6.3 Correct a release across mail, IRC and the fediverse

Make one case from a mail proposal, IRC regression report and social announcement. Annotate exact
claims, derive audience-specific correction drafts, then dispatch each independently.

```vql
let $case := case_create("Release correction", $selected_observations)
let $mail_draft := mail_reply_draft($thread, $approved_evidence, $mail_audience)
let $irc_draft := irc_message_draft($channel, $short_correction)
let $social_draft := social_post_draft($account, $public_correction, $public_audience)
return $case, $mail_draft, $irc_draft, $social_draft
```

Private investigation notes stay private. Each send binds its draft edition and audience; each
recipient/target can have a different receipt. A later edit or deletion never makes an earlier
quoted observation appear current.

### 6.4 Resume a recurring investigation

A saved workflow compares overdue invoices, ticket state and conversation evidence. Each run pins
its definition, connection revisions, input observation set, coverage and decisions.

On reopen, show “database complete; REST page three incomplete; mail draft unsent.” Replayed events
resolve to prior work without repeating completed effects. An expired cursor creates an explicit gap
and resynchronization run. Branching from yesterday's analysis preserves both earlier conclusions
and new alternatives. VPL can calculate; Vlog can decide; reusable Vortex routines orchestrate.

## 7. Privacy, authority and publication are part of the model

Keep credential values, authentication headers and secret environment values in a host-owned
credential broker. Inspectable cells hold scope, status and an opaque binding. Filter secrets before
append-only retention; hiding them afterward cannot remove their historical bytes.

If policy withholds content, either retain the permitted original under the declared access policy,
or identify the saved representation as a redacted derivative. Record permitted range mappings,
withholding and unavailable-original status; filtered bytes must never masquerade as exact raw
evidence. Even range/length metadata follows the disclosure policy.

Private annotation, capture and workflow stores need explicit access/publication policies. Endpoint
names, account identifiers, receipts and even opaque bindings may disclose context. Every outgoing
action has an audience and a payload manifest.

A projection mask neither enforces authorization nor sanitizes publication history. Build an
independent export artifact from the selected allowed evidence closure; review its spans, links,
identifiers, attachments and history. Pin the selected observation/annotation editions and closure
manifest so refresh cannot change what the export includes. Do not publish private bytes merely
because a visible dimension was removed. Private notes cannot also be universally discoverable
backlinks.

## 8. Delivery order and acceptance

| Stage                        | Concrete deliverable                                                                                                    | Evidence required                                                                             |
| ---------------------------- | ----------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| 0. Semantic repairs          | Engine conformance matrix, explicit unsupported states, view/edit ruling, identity/cardinality/value contracts          | Example parsing plus common-subset direct/compiled checks; no silent source fallback          |
| 1. Complete evidence journey | Read-only SQLite + REST GET, private case, field/span notes, offline reopen and refresh comparison                      | Notes retain identities/evidence through arena teardown; page failure remains incomplete      |
| 2. Controlled effects        | Fake delayed host adapter, durable intent/attempt/receipt, guarded SQL patch, history import and argv process execution | Crash/cancel/restart boundaries, stale versions, unknown outcomes and no automatic resend     |
| 3. Communications            | Mail, IRC and fediverse reading/drafting, then scoped delivery                                                          | MIME identity, reconnect gaps, duplicate/out-of-order activity and audience-specific receipts |
| 4. Full work environment     | Joins, windows, subscriptions, branching workflow runs, reusable Vortex packages and selective sharing                  | Sustained bounded workloads, explainable lineage, resumable work and safe export closure      |

Test the seams with controlled sources before live service actions:

- Annotation anchors survive refresh, sort, pagination, source deletion, session destruction and
  restart. Content availability follows the declared retention policy. An ambiguous source rekey
  never silently transfers a note.
- Duplicate rows/events/deliveries and repeated commands retain occurrence distinctions. Two equal
  strings do not become one subject or clone master.
- NULL, missing, empty, redacted, pending, failed, partial and gapped inputs remain distinguishable.
  Strict workflow contracts report invalid inputs rather than filtering them away.
- Browse/axis movement adds no operations to visited stores. Promotion is bounded and refuses
  ephemeral references that were not remapped.
- Slow sources do not block the UI/VM owner. Demand, byte limits, teardown and late completions
  enforce bounded memory and lifetime correctness.
- Crash immediately before/after dispatch, remote completion, receipt and checkpoint; reopen without
  silently repeating an effect. Source-local transactions fail atomically where promised.
- Every exposed semantic item, including errors, gaps and receipts, can receive a durable note.
  Export contains only the selected allowed closure.

The first complete journey must also be reachable through the interface: define connections, inspect
requests and coverage, annotate a field/span, close, reopen offline, refresh, compare and prepare a
selective export. CLI-only or scripted access is useful groundwork, not a completed end-user
integration. Run the repository's UX-validation workflow when implementing these journeys.

## 9. Decisions and costs that remain

1. **Shape shorthand:** should bare braces select a view, edit structure, or be confined to an
   explicit effect context? The library operations can be specified before choosing shorthand.
1. **Fan-out:** retain implicit content-sharing clones for compatibility, or require explicit
   reference/occurrence construction? The Purist and writer favor separating these meanings.
1. **Contract failure:** keep parameter mismatch as filtering for queries, with a strict action
   boundary, or add an explicit function-level matching/validation distinction?
1. **Retention:** retain the exact selected span only, a representation, or the context needed to
   understand it? Offer declared policies and budget failures; permanence consumes storage.
1. **Continuity:** when may an adapter assert stable identity after rekey/move/replacement, and when
   must the person accept a correspondence claim?
1. **Background authority:** which definitions may refresh, retain and dispatch unattended, under
   which capability revision and expiry? Define this per authorized workflow.
1. **Publication:** how can a selected evidence closure be packaged without leaking private history
   or claiming the remote author's signature? This needs the existing publication machinery.

Universal addressability costs cells and indexes. Capture costs bytes. Joins and durable automation
cost state. On-demand materialization, bounded promotion, windows and explicit retention keep those
costs accountable; they cannot eliminate them.

The recommended first investment is the **SQLite + REST evidence/annotation/offline journey**, built
on shared contracts. Then add controlled effects and communication adapters. The ambitious end state
is one navigable fabric of sources, evidence, reasoning, actions and consequences, with
source-shaped presentations and the user's annotations remaining independent of any particular
service.

## 10. Primary conceptual grounding

Nelson's rank/neighbor and clone model supports structural reuse with independent placements; it
does not prescribe this repository's dimension names or connector policies.
[A Cosmology for a Different Computer Universe](https://xanadu.com/zigzag/ZZdnld/zzRefDef/).

His ZX notes explicitly discuss email fields and uniform traceability of text, titles and
calculation results. That motivates annotation below a connector's top level.
[ZX Internals](https://www.xanadu.net/zigzag/fw99/zxInternals.html).

Enfilades serve reference/version relationships, and transclusion preserves identifiable context.
They should inform provenance while leaving uncertainty about foreign sources visible.
[Project Xanadu technologies](https://xanadu.com/tech/),
[Nelson on transclusion](https://xanadu.com/transclusion/).
