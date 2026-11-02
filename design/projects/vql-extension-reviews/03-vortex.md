# VQL workflow integration: Vortex/systems review

Recommendation: make VQL the query and authoring surface of a durable, inspectable workflow graph,
with a small asynchronous capability boundary shared by every Vortex frontend. The missing
abstraction is broader than another lens: distinguish a source subject, an observation of it, an
occurrence/event, an action attempt, and a workflow continuation. Their relationships let people
quote, annotate, compare, retry, and resume work without confusing an external object with its
current projection.

This is a read-only review at the supplied 732b8280 baseline. I inspected the VQL specification
including v16 §4.12, runtime §§5/7/9, the lens proposal's
invariants/interfaces/identity/diff/structured-source additions, and concrete implementation/tests
cited below. Some large read outputs were truncated; I reread major VQL ranges, but cannot honestly
certify every line of the lens proposal or runtime was visible. I ran no binaries, builds, external
integrations, or benchmarks. All proposals below are proposals, not claims of implemented behavior.

## What exists, versus what prose promises

- **Direct VQL is real, and allocates.** `apps/common/xanadu/vql/vql_engine.hpp` exposes
  vector-returning execute/path/step APIs and a hash-map environment; `vql_engine.cpp:139–175`
  registers definitions/mounts and executes script statements. Thus the spec's universal
  zero-allocation, lazy-streaming, associative-cursor-only statements are design aspirations.
  Existing arena work accounting is useful, but does not supply suspension or bounded output by
  itself.
- **Compiler and interpreter are different implementations.** `compiler.cpp:255` compiles
  `query.expr`, while the direct engine executes `query.body`; `compiler.cpp:360` explicitly rejects
  anchor predicates and directs users to the direct engine. `compiler.cpp:383` emits one Link opcode
  per current compile-time stream cell, rather than implementing a general runtime lazy rank
  iterator. `tests/xuzz/vql_compiler_test.cpp:188` is a useful execution-equivalence seed, not proof
  of v16 coverage. Do not accept parsing or opcode emission as semantic implementation.
- **Discovery/generator names are not discovery integration.** `vql_engine.cpp:478` resolves a named
  local store and otherwise falls back to ambient home; `:653` does analogous generator dispatch,
  with special treatment for `current`. That can produce plausible results for an unsupported remote
  request. Replace this fallback with explicit unsupported/unavailable outcomes. AST version-pin
  fields (`ast.hpp:226,309`) do not establish historical-fold evaluation. The microversion anchor
  currently returns home (`vql_engine.cpp:678` vicinity).
- **Watch is a synchronous simulation.** `vql_engine.cpp:396–467` evaluates current target cells
  once; the since-version branch explicitly says “catch-up simulation” and does no replay. Delta
  metadata is synthesized as “modified.” `tests/xuzz/vql_watch_test.cpp:39` even describes this
  simulation. `zz9p.hpp:188` and `zz9p.cpp` implement watch message encoding, and
  `tests/xuzz/zz9p_test.cpp:291` checks round-trips. My search for
  `SpoolWatcherRegistry`/`EventFrame` in apps/tests found no implementation. Codec support is not a
  server subscription, suspended VQL fiber, resumable cursor, or cancellation implementation.
- **Projection is real; general unprojection is not.** `projection_session.hpp` exposes
  construction, identity lookup, roots, and count, with no commit/refresh/promoteInto.
  `projection_session.cpp:35` synchronously enumerates all nodes then all edges.
  `projection_lens.hpp` has content/attribute mutation callbacks with default refusal, but no
  create/delete/reparent/reorder/batch API. Its NodeContent lacks PrimediaSpan runs, contrary to VQL
  §4.11.3's displayed type. `tests/xuzz/lens_vql_gate_test.cpp` proves shared-arena traversal
  equivalence after C++ constructs a FilesystemLens session; it does not prove the universal
  `lens(...)` language primitive or bidirectional commit. Lens §8A already identifies NULL/blob,
  edge-property, transaction, rekey, and schema gaps: retain those rather than reinventing them as
  new discoveries.
- **Useful async/distributed machinery exists.** `vortex/mesh_host.hpp` has a background owner,
  bounded request/command counts, snapshots, leases, retries and worker credits. `mesh_actor.hpp`
  has explicit grants/resource limits, request deduplication records, authenticated admission and
  message-boundary continuations. `mesh_checkpoint.hpp` requires exact signed pointer
  acknowledgement; `tests/xuzz/mesh_checkpoint_test.cpp:40` checks it.
  `mesh_actor_resume_test.cpp:152,196` covers state/dedup preservation and fresh capability binding.
  Reuse these ownership and state-machine patterns.
- **Do not overstate the mesh lens.** `mesh_lens_channel.cpp` sends delimiter-separated
  `LENS_REQ`/`LENS_RESP` strings. `pollIncoming(ArenaManifold &)` ignores its arena argument,
  updates proxy metadata only, and re-sends unhandled messages. This is not evidence of
  authenticated graph transport, span provenance, source writeback, or an end-to-end frame-time
  guarantee.
- **VM cancellation is a starting seam.** `vortex_vm.hpp` offers `run(..., stop_token)` and
  scheduler stepping; `vortex_vm.cpp:1126,1160` checks cancellation. The opcode enum contains no
  host-await/resume machinery. `VortexHost::executeVQL` (`vortex_host.cpp:635`) is synchronous.
  Existing worker pools isolate arenas; they do not automatically make source I/O nonblocking.

The VQL header still labels itself v15.1 while its history/§4.12 specify v16, and the introduction
says nothing is wired into the build. Fix these documentation contradictions before using the spec
as a release contract. Treat 3 ns virtual evaluators, cycle-count claims, 16-byte universal handles
and three-register passage of a 24-byte aggregate as hypotheses requiring measurements/ABI
validation, not architectural premises.

## The shared asynchronous host contract

Introduce an engine-side HostServices registry, bound to a Vortex execution context rather than
global process state. It accepts typed requests carrying a capability handle, operation kind,
canonical arguments, limits, expected source version, and stable request ID. Capabilities grant
specific account/resource/operation scopes; read does not imply send, delete, spawn, or arbitrary
SQL. Secret tokens remain in host-owned stores; cells carry an opaque grant reference and
inspectable scope, never executable authority merely because they contain a credential-shaped
string.

Submission returns a task handle immediately. Completion delivers immutable bounded batches through
the owning context's queue; only that owner mutates its arena. A small VM primitive
submits/suspends/resumes at explicit safe points. Vortex library routines compose pagination, joins,
debounce, retries, deduplication, approval policies, compensation and checkpoint progression. New
C++ implements sockets/TLS/database drivers/process spawning and translates protocol results; it
should not become a second workflow language. Host primitive semantics, queues and lifetime
enforcement necessarily require runtime C++, but policies should not.

Tasks expose pending, running, partial, succeeded, failed, cancelled and outcome-unknown states as
navigable cells, with input, output, error, receipt and causal links. A pending cell is a meaningful
query result; an unavailable source is not an empty rank. Structured errors retain source key, retry
classification, protocol code and bounded diagnostic text. People can annotate an error, quote a
result, link a note to an action, and revisit the exact attempt after restart. Durable annotations
target stable subject/observation IDs, never a transient dense arena ref.

Per-task demand/credits, maximum buffered bytes, row counts, deadlines and fair instruction slices
constrain streaming. Cancellation stops accepting results, requests adapter cancellation, releases
leases and suppresses late completions by task-generation check. For remote mutation, cancellation
can mean “stop waiting” while the external action may have completed: expose outcome-unknown and
reconcile using a receipt/status lookup. Never promise rollback across HTTP, SMTP, IRC and SQL.

## Identity, provenance, persistence and replay

Use five separate records: source identity; stable subject identity within source; immutable
observation with source version/ETag/transaction snapshot and captured bytes; occurrence with event
ID/order/checkpoint; action attempt with arguments/expected version/request ID/outcome. Workflow
instances reference exact program and input versions. Observation timestamps and source sequence are
different axes; local hypertime is the history of our knowledge, not automatically the remote
server's event order.

A SQL primary key needs database/table identity and key-generation semantics; a process needs boot
identity plus PID plus start time; an email message needs account/mailbox UIDVALIDITY/UID or
provider identity, with Message-ID as evidence rather than universal uniqueness; ActivityPub object
and activity IDs are distinct. IRC needs server/channel/session context and server msgid when
available. Absent durable upstream IDs, use explicit observation/occurrence identity and documented
matching, not invented permanent remote identity.

Imported external bytes acquire archival source observations in primedia, with attribution and
source version; subsequent quotes preserve those addresses. This initial import necessarily copies
bytes from foreign systems lacking our permascroll. Label source retrieval and local observation
authorship separately: local capture does not establish the sender's cryptographic authorship. Do
not promise recovery of deleted remote content unless it was actually retained.

Browsing, refresh, axis movement, speculative fetch and query scratch work do not append to visited
stores. Explicitly retained observations, user annotations and authorized workflow/action records
append to their own designated stores through ordinary promotion. Apply existing
mark/release/compact restrictions; task leases pin the source/base/session for the entire async
borrow. Queue stable serialized identifiers or owned values across workers, not CellRefs/spans into
reclaimable arenas. Promotion remaps refs and scratch content; never write ephemeral refs directly
to persistent operations.

Journal intent before external execution and receipt after it. A crash between them is inherently
ambiguous. Deterministic replay reuses recorded observations/receipts; refresh is a new effect, and
resending a command is an explicit new attempt unless the adapter has a verified idempotency
contract. Actor message deduplication alone does not make SMTP sends or process execution
exactly-once. Checkpoint only at quiescent message/effect boundaries, following ActorContinuation
rather than serializing native callbacks/PCs; re-admit capabilities after restart. Cross-source
workflows are sagas with explicit compensating actions when possible, not a fictitious distributed
transaction.

## Integration sketches beyond lenses

**SQL:** expose catalog/schema, row subjects, typed values including NULL, decimal and blob, query
observations and transaction receipts. Compile eligible pure predicates/projections/limits into
parameterized plans. Keep updates as action cells with optimistic versions or explicit transaction
policy; surface constraint failures and partial/batch atomicity honestly. Persist annotation-to-row
identity separately from ephemeral result ordering.

**REST:** retain endpoint/account identity, resource ID, ETag, pagination cursors, response
observations and rate-limit state. Conditional writes are actions; PATCH semantics and
retry/idempotency support are adapter declarations. Link fetched JSON fields to the exact captured
response, avoiding a claim of original character provenance after normalization.

**Process/shell history:** distinguish recorded command text from execution. Capture argv, working
directory, environment references, exit status and stdout/stderr spans as one execution occurrence.
Use an argv spawn boundary by default; a shell parser is an explicit capability. Historical commands
remain annotatable without re-executing them. ProcLens control simulation is not this execution
engine.

**Email:** project threads, messages, MIME parts and delivery attempts; preserve raw message bytes
and decoded-part provenance. Draft/send/archive are separate action types. “Sent” requires the
protocol's actual acceptance receipt and does not imply delivery. Reconcile ambiguous sends rather
than blindly retrying.

**IRC:** model sessions, channels, messages, membership observations and reconnect gaps.
Capability-limited send/join actions have bounded queues. Where backlog is unavailable, record the
gap explicitly; a watch cannot reconstruct events never observed or retained upstream.

**ActivityPub/fediverse:** separate object state from Create/Update/Delete/Announce occurrences,
provenance and visibility scopes. Retain attributed revisions and tombstones according to retention
policy; federated delivery/verification and partial acceptance remain visible. A URL is a source
identifier worth preserving even when native published names also exist.

## Optimization and staged gates

Add a shared logical plan with effect/ordering/cardinality annotations. Push down only operations
whose null, collation, ordering, snapshot and comparison semantics match. Keep residual predicates
locally and show pushed predicates, estimated/actual rows, bytes and cancellation status in an
explainable plan. A limit cannot commute past an effectful function; negative indices/sorts need
finite bounds or explicit windowing. Reuse source observations and incremental joins keyed by stable
identities; avoid one network request per cell by batching compatible lookups.

1. **Semantic honesty:** unsupported generators/pins/watch/compiler constructs refuse explicitly;
   publish a parser/direct/compiler/runtime feature matrix. Gate direct/compiled common-subset
   equivalence and zero source ops for browsing.
1. **Host runtime:** fake delayed adapter tests prove immediate submission, fair VM stepping,
   credit-limited batches, cancellation, late completion rejection, session teardown, and promotion
   across retained observations. No frame-time claim without an actual measured UI test.
1. **One complete workflow:** SQL read → local annotated analysis → conditional action → receipt →
   restart/resume. Test stale versions, denied capability, NULL/blob fidelity, transactional failure
   and duplicate request mismatch.
1. **Internet adapters:** mocked paginated REST, email ambiguous send, IRC reconnect gap and
   ActivityPub duplicate/out-of-order activity fixtures. Crash at every
   intent/dispatch/receipt/checkpoint boundary; verify no unauthorized replay and explicit
   uncertainty.
1. **End state:** a saved workspace contains inspectable query programs, pinned source observations,
   annotations, decisions, action histories and resumable workflows in the same multidimensional
   space. VQL asks and reshapes; VPL computes; Vlog decides; reusable Vortex routines orchestrate.
   Every result can answer where it came from, which version it represents, what is still pending,
   and what external action actually happened.

## Supplement: fast-forward to 7617d3f0 (delta review only)

The 732b8280..7617d3f0 change resolves important v16 direct-evaluator gaps. `ast.hpp` now represents
unions, optional/generic/shape types, typed/defaulted destructuring fields, return contracts and the
`isFunctor` flag. `parser.cpp` parses these forms. `vql_engine.cpp:174–379` implements shape
matching and type satisfaction; `:479–639` checks lambda/UDF parameters and UDF returns. Parameter
mismatch filters the call; return mismatch throws `TypeContractError`; unknown type names are
refused. `String` and numeric contracts intentionally overlap for parseable textual numbers. These
are runtime checks on cell-based values, not evidence of the spec's 16-byte HyperHandle runtime.

Functor implementation is now concrete: `FunctorSteps` is a shared immutable vector; `functorOf`,
`bindLet` and `applyFunctor` preserve and compose steps, with bare functor-variable steps inlined
when bound. Untyped UDF parameters can carry functors. `cellsOfValueExpr` and `bindLet` also correct
`let $y := $x` to retain cell identity rather than mint a value copy. New
`tests/xuzz/vql_types_functors_test.cpp:136–431` covers typed calls, cardinality, defaults, all four
shape modes, return refusal, identity-preserving let, functor composition and compiler refusal.
These are inspected tests, not tests I ran.

The document header now correctly says v16.0, and new §4.12.5 clearly distinguishes direct
evaluation from unbuilt `CurriedMorphism` lowering. Retire the original report's header
inconsistency finding. Preserve its broader allocation/streaming and direct/compiler divergence
findings: the implementation uses vectors/maps, and `compiler.cpp:269` adds explicit refusal of a
functor let, while §4.12.5 explicitly says compiler types are erased without being checked.
Consequently direct-evaluator parameter/return checks cannot establish compiled mutation-boundary
safety. Nor is a return check a preflight: the body executes before its result contract is checked.
The delta does not introduce a whole-program type/effect pass or checks around every mutation. Any
future external action must still validate arguments, capability and expected source version at the
host dispatch boundary, independently of frontend annotations.

The proposed common runtime seam should stay smaller than a connector framework inside the VM:
submit a typed request, suspend its continuation, accept bounded completion batches on the context
owner, and cancel/release the task. C++ must enforce capability lookup, request/result limits, queue
ownership, generation/lifetime checks, adapter cancellation and protocol I/O; Vortex code cannot
safely replace these host guarantees. Vortex should own retry selection, pagination loops, joins,
workflow transitions and compensation, using inspectable library code. A generic VM host operation
may reference an explicitly bound service; it must not deserialize arbitrary native callbacks from
stored or remote programs.

The synthesis's explicit submit-only action rule is stronger than inferring dispatch from edits to
any `d.ctl`-shaped graph: ordinary authoring can prepare or annotate an action without executing it.
Submit first promotes/journals intent, then dispatches, then appends receipts; ambiguous crash
outcomes remain visible. Serialize stable source/request IDs and recorded inputs, not arena
references. Retain message/effect-boundary checkpoints, demand credits and explicit stream gaps.
None of the fast-forward changes implements these async/effect guarantees or changes the earlier
watch/projection/mesh-lens findings.
