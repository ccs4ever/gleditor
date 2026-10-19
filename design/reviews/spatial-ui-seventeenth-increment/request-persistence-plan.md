# Publication inbox request persistence prerequisite

## Scope

This is a design for the next inbox lifecycle increment, not an implementation. Increment 17 remains
frozen pending its canonical build and review. Its existing completed-snapshot verification and
install behavior should be preserved. This change addresses a separate loss: mutable and pinned
requests that fail, cancel, or are interrupted currently exist only in memory, so closing the
application forgets their request ID, exact pin or observed lower bound, and known outcome.

The owner remains `PublicationInbox`; this does not add a service, resume networking automatically,
or change the publication carrier/store format.

## Persisted request model

Persist a bounded snapshot of the inbox request index under the existing inbox directory. The
journal is the source of request identity and state; retained content directories remain the source
of verified bytes. Each record needs:

- stable request ID;
- request kind: mutable name or exact immutable pin;
- publisher key and salt;
- optional exact `PublicationPin` (publisher, salt, carrier hash, sequence, version, title);
- optional observed mutable pointer/lower bound (hash and sequence) and any locally verified
  manifest pin derived from it;
- user-visible phase and verification stage;
- last known error and bounded progress metadata useful to report interrupted work;
- retained root association when installation has begun or completed.

Do not persist a mutable pointer as if it were an immutable pin. A mutable request stores its name
and minimum/last accepted pointer separately. If it had resolved a pointer before interruption,
preserve that exact observed hash/sequence as its lower bound; a later retry may advance only under
the existing signed-pointer and rollback/conflict checks. Exact-pin retry continues to target the
same full pin. The request ID is stable across restart and retry.

Reuse the repository's canonical `PublicationPin` fields/validation. Search found no general-purpose
publication-pin bencode encoder shared by the inbox; the package exchange has its own bencode record
helpers for package-domain records. If a reusable helper is extracted, keep it domain-typed and
internal or in a suitable publication header, and do not alter LinkPackage wire encoding. Avoid
parallel subtly different pin serializers.

## Format, bounds, and atomicity

Use a small, versioned inbox request-journal envelope with an unmistakable signature and explicit
version. Do not silently accept old, unknown, or truncated versions and do not add a compatibility
shim. On refusal, preserve the original journal and all request roots for inspection. If the
existing inbox has no journal yet, it may be initialized from the supported current
retained-snapshot layout; that migration must derive only facts present in the signed retained
manifest and canonical carrier, not invent prior request IDs or mutable request lower bounds.
Existing completed snapshots must remain listable and undergo increment-17 retained-content
verification before `Ready`.

Bound both total journal size and number of retained requests. Give the request-count cap a
deliberate `PublicationInbox::Options` or governed settings owner rather than using an unbounded
filesystem scan. Keep existing manifest/dependency byte and dependency-count limits. Refuse a new
request before allocating its worker/disk state if the bound is reached; expose a clear limit
result. Do not evict failed/cancelled requests or their exact pins silently, since that defeats
durable retry and audit.

Write the new journal to a sibling temporary file, flush and close it, then atomically rename over
the current journal. Where the platform supports it, sync the file and containing directory around
rename so a reported durable transition survives a power loss. Serialize writers under the inbox
state lock or a dedicated persistence lock; worker updates, cancel, retry, submit and shutdown must
not race to publish stale snapshots. A failed journal write must not report a state transition as
durable; retain the prior valid journal and surface a persistence error without deleting content.

Request directories should follow the same safe-root constraints as existing retained snapshots:
reject symlink roots and path escapes, use a staging directory for new installation, and never
delete an earlier verified snapshot as part of retry. The journal points at a root only after the
root's installation step is atomically committed. A crash between root commit and journal commit is
repaired by validating the orphaned signed snapshot and merging it only if its deterministic carrier
identity and all recorded pin facts agree; otherwise preserve it and report a typed recovery
conflict. A crash before root commit leaves a staging directory that can be retained for diagnostics
or safely cleaned only after proving it is an incomplete staging artifact, never by broad
`remove_all` on a request root.

## Startup recovery semantics

Load and validate the journal before starting the worker. Unknown version, malformed fields,
duplicate IDs, invalid pins, unsafe roots, size/count overflow, or conflicting equal-sequence
observations must refuse loudly with a typed inbox-journal error and leave source data intact.

For each valid record:

- `Ready` remains eligible only if it points to a retained installed snapshot. Queue the existing
  increment-17 local verification worker and show `Verifying` until signed manifest, exact carrier
  hash, dependencies, and reconstructed native store checks pass. Do not block Session/UI
  construction.
- Requests persisted as `Resolving`, `Downloading`, `Verifying`, or `Queued` at shutdown become
  `Cancelled`/interrupted with the same ID, exact pin or mutable lower bound, and a clear
  “interrupted; retry explicitly” diagnostic. Do not start a network request on startup.
- `Failed` and `Cancelled` retain their exact known outcome/error and retry identity. Explicit Retry
  reuses the same request ID and pin/lower bound. A pinned retry does not resolve a mutable name;
  mutable retry uses its retained lower bound and current rollback protection.
- If manifest verification had completed but dependencies were incomplete, preserve that truthful
  stage where it remains supported by persisted evidence. Do not display `Complete` until local
  retained bytes pass all existing checks.

Persist each meaningful transition (including mutable pointer advancement, manifest verification,
phase/error, and retry reset) so a crash cannot silently downgrade a known lower bound or make a
known failure look like a fresh request. Progress counters can be checkpointed coarsely to avoid
excessive sync writes, as long as they never imply bytes were durably retained when they were not.

## Recovery and lifecycle atomicity

The intended order for a mutable request is: persist request/name; resolve signed pointer; validate
pointer and monotonic lower bound; persist observed hash/sequence; fetch into a request-scoped
staging root; verify manifest and persist its exact pin/stage; validate dependencies; install to an
immutable snapshot root; sync/commit that root; then persist `Ready`/`Complete` and root
association. A crash at any boundary retains either the last durable request record or an
independently verifiable immutable snapshot. The UI must never observe `Ready` before both root and
journal commits complete.

For exact pinned requests, persist the full pin and request ID before queueing worker activity.
Fetch and verify that exact hash only. On failure, preserve the request record and error; do not
erase the root as a retry side effect. Use isolated attempt subdirectories or carefully tracked
staging paths so retry cannot overwrite prior diagnostic or verified data. Repeated submission of
the same full pin should return the extant request ID, including failed/cancelled/interrupted
states; only explicit Retry restarts it. Different title/version/hash remains a different pin.

Cancellation is a durable transition. Check cancellation under the same synchronization boundary as
successful install/Ready publication, then atomically commit either `Cancelled` or `Ready`, never a
race where cancellation returns successfully but Ready wins afterward.

## Tests

Add owning `PublicationInbox` tests that use the existing transport doubles only for deterministic
worker behavior; they are not native transport proof.

- Submit mutable and exact-pin requests, wait for a known phase/failure, destroy and reopen the
  inbox; assert stable IDs, full pin/lower bound, last error, and phase survive.
- Simulate interruption in queued, resolving, downloading, and verifying phases; reopen must show
  explicit interrupted/cancelled state and must not call the transport factory until `retry(id)`.
- Retry a restored exact-pin failure: same ID and full pin, no mutable resolve call, and no old root
  deletion. Retry a mutable request: exact observed lower bound preserved and
  rollback/equal-sequence conflict still refused.
- Crash-boundary fixtures for journal-temp write, root install before journal commit, and journal
  commit after root install. Recovery either restores verified Ready state or reports a typed
  conflict; it never reports Ready from metadata alone and never destroys the retained bytes.
- Corrupt/unknown journal signature/version, duplicate IDs, wrong pin hash/title/version, unsafe
  path, oversized record, count overflow, and tampered lower bound refuse loudly while preserving
  files.
- Preserve completed increment-17 behavior: reopened snapshots are locally revalidated off the UI
  thread; dependency or native-store corruption cannot become `Ready`/`Complete`.
- Concurrency test submit/cancel/retry/worker checkpoint serialization for stale-snapshot lost
  update and cancellation-versus-ready races.

## Native validation

No app run is needed for the engine contract. After implementation, root should run the owning
headless inbox tests and canonical build. Native UI validation should be a separate, explicit UX
pass: create a pinned download, stop the app during transfer, reopen and inspect the same request ID
and interrupted state, then explicitly retry; verify status remains visible through another restart
and the retained pin never retargets. That workflow proves local request durability, not peer
availability or public DHT reachability. Keep network configuration and publication privacy policy
unchanged during this increment.
