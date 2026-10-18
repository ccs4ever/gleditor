# Distributed Vortex over the swarm: the Spool-Mesh model

This document specifies the distributed-runtime target. The implementation status below
distinguishes the existing loopback foundation from the remaining work. It evaluates adding
multiprocess and network-transparent distribution — in the Julia and Erlang sense of
location-transparent processes and futures — to the Vortex virtual machine, carried over the
existing BitTorrent, BEP 46, and OpenPGP identity layer. It is the distributed sequel to the
single-machine [Vortex multithreading study](vortex-multithreading-study.md): where that study
stopped the Global Interpreter Lock by giving each OS worker its own `ArenaManifold` over one shared
immutable base, this document asks what changes when the next worker is not on the next core but on
another machine, reachable only through a churning DHT.

The model is called **Spool-Mesh**: a mesh of share-nothing arenas that communicate only by
content-addressed, immutable messages. Its one-sentence thesis is that distribution is not a new
runtime but the existing arena-as-node, share-nothing discipline restretched over the DHT, with the
single, decisive safety property that **there is no remote mutation** — a node never reaches into
another's arena; it receives an immutable value or a sealed write-set and merges it locally by
minting its own operations. Every claim below is grounded in source that was read, not assumed, and
the file-and-line citations are retained throughout because they are the investigation's whole
credibility. The verified anchors are collected once here and repeated in context: `choiceStack_`
(`vortex_vm.hpp:151`), `WorkerContext` (`worker_context.hpp:51`), `VortexWorkerPool`
(`vortex_worker_pool.hpp:35`), `Promoted` and `promote()` (`arena_manifold.hpp:237`, `:28`), the
`ephemeralBit` refusal (`arena_manifold.hpp:24`, `:750`), the portable currency `globalise` /
`localise` / `opRefOf` / `localiseOpRef` with `GlobalSpan` / `GlobalOpRef`
(`publication.hpp:465-548`, `:69`, `:99`), `kMaxPayloadBytes = 64 KB` (`identity_layout.hpp:30`),
`MessageType` (`identity_layout.hpp:245`), `kDefaultHashcashDifficulty = 20`
(`identity_layout.hpp:286`), `PeerChallenge` / `PeerChallengeResponse`
(`identity_layout.hpp:457-479`), `trustDelegation` (`identity_network_controller.hpp:291`), the
standing `RefusingVerifier` default (`payment_verifier.hpp:104`; controller default
`identity_network_controller.hpp:412`), and the `std::expected`-refusing decoders
(`identity_serialization.hpp:62-72`).

## Implementation status (2026-10-17)

The Spool-Mesh distributed runtime is implemented in `apps/common/xanadu/vortex/mesh_*` alongside
the `spool_mesh` loopback foundation.

Verified increments:

- The published-value boundary promotes and seals actual graphs, signs their global root, parent,
  computation identity and continuation metadata, fetches metadata before payload, verifies sealed
  history and content, and imports receiver-local references while preserving published spans,
  scalar bits, cell-relocation handles and foreign operation handles (`ValueKind::ForeignOpHandle`
  with `d.op-label` provenance). `MeshValueTest` checks the round trip, tampering, missing
  dependencies, foreign operation-handle identity preservation and byte-identical operation output
  for differently ordered result batches.
- Immutable jobs fetch source programs and optional base snapshots in a background worker, enforce
  signed capabilities, delegated device identity, Hashcash and explicit default-refuse admission,
  evaluate through the real VPL/VQL/VProlog/VLace engines under resource limits, and seed signed
  results. Raw Vortex source is refused explicitly until a sealed instruction artifact has an
  execution entry point. `MeshJobsTest` checks all four frontends, asynchronous polling,
  deduplication and refusal.
- The actor extension has bounded named-error codecs, authenticated peer dispatch, cumulative BEP 10
  fragment framing and queued sends. Actual session plugins share the content listener, and each
  downloaded torrent is isolated under its own info-hash directory. `MeshSwarmTest` verifies
  delegated device authentication, actor control and verified program content over a real two-peer
  socket connection. Identity extensions use local IDs 24–27 to avoid libtorrent built-ins; actor
  local ID remains 6.
- Actor contexts, opaque PIDs, persistent monotonic device epochs (`mesh_epoch`), bounded mailboxes,
  acknowledgements/retries, content deduplication, immutable redispatch and three-state monitors
  with fresh discovery evidence are covered by `MeshEpochTest` and `MeshActorTest`. Resource limits
  apply inside frontend evaluation, including recursive work.
- Signed message-boundary continuation encoding, the pending-to-committed DHT publication state
  machine and checkpoint reactivation (`MeshActorRuntime::resume`, `resolveCheckpoint`) are
  implemented. `MeshCheckpointTest` verifies that only an acknowledgement for the exact key, salt,
  hash and sequence commits a checkpoint, and `MeshCheckpointNetworkTest` (under
  `tools/swarm-netns-test.sh`) verifies real BEP 46 DHT publication, publisher teardown, route
  blackhole partition refusal, partition healing, state/dedup restoration and higher-epoch
  incarnation handoff.
- `MeshHost` and the shared CLI runner (`mesh_cli`, exposed on `vpl`, `vquery`, `vprolog` and
  `vlace` via `--mesh-worker`, `--mesh-run` and `--mesh-help`) wire the swarm transport,
  authenticated job admission, background evaluation and deterministic multi-worker result merging
  into a complete multiprocess lifecycle. `MeshHostTest`, `MeshHostSocketTest` and `MeshCliTest`
  verify closed worker sets, degraded/expired worker refusal, two-host socket futures and separate
  requester subprocesses across all four frontends against a real worker and GnuPG signer.

Application command names use `std:xuzz`; shared project/protocol names use `xanadu`. Naming cleanup
retains explicit compatibility aliases and fixture paths pending the separate migration work.

## Scope and relationship to the multithreading study

The multithreading study found that the arena "is not a lock to be made faster, it is a node
boundary," and its §4 then viewed the resulting single-machine design as a well-behaved distributed
system: arenas as share-nothing nodes, the immutable base and `ops.nodes` as the shared substrate,
`promote()` as a distributed commit, and determinism enforced at a merge boundary rather than inside
the workers. This document takes §4's distributed framing at its word and carries it one machine
further out. Everything that study localized to an in-process worker is already localized to a
process; the only genuinely new work is a wire, a few proxy and mailbox cells, and an honest account
of what the swarm transport can and cannot carry.

The scope is deliberately narrow. In: node and process identity, addressing, discovery, message
transport, durable actor state, failure detection and supervision, determinism of the merged result,
and the security posture, each mapped onto machinery that already exists. Out, and stated as
non-goals up front rather than discovered late: distributed backtracking over a shared choice stack,
synchronous remote-procedure calls, chatty fine-grained messaging, strong or total ordering, and
remote mutation of a foreign arena. The distributed-systems reasoning — the Erlang-and-Julia
mapping, the failure model, the delivery guarantee — is folded into the sections below rather than
carried as a separate voice.

## The grounded substrate: what Vortex provides today

The design is cheap because the substrate it needs already exists, and the first task is to say
exactly what that is.

The unit of distribution is the `WorkerContext`, verbatim
`{ArenaManifold arena; VortexCore core; VortexVM vm; VortexStdLib stdlib;}`
(`worker_context.hpp:62-66`), deliberately non-copyable and non-movable because its later members
hold references into its earlier ones (`worker_context.hpp:58-61`). The multithreading study already
named this the node: an `ArenaManifold` plus the `VortexCore` and `VortexVM` bound to it, owning its
dense vectors and its choice-point state and nothing anyone else can see. That is already a
share-nothing BEAM-style process — per-process heap in the arena's `slots_` / `links_` / `content_`
/ `scratch_` / `trail_`, per-process scheduler state in `cursorPC_` and `choiceStack_`, per-process
garbage collection through `collectGarbage()` and `compact()`. The jump from thread to machine
changes exactly one thing: the base `Manifold*` the context overlays (`worker_context.hpp:52`) is no
longer a shared in-process pointer but a replicated, content-addressed snapshot keyed by a
`MicroversionId`. Nothing in the struct assumes shared memory; it assumes a read-only base it never
writes.

A cursor is the process. `spawnCursor` mints a cell, sets its opcode, and links it onto the tail of
home's `+d.cursors` rank (`vortex_vm.cpp:127-141`); `activeCursors()` reads that rank back
(`vortex_vm.cpp:143-146`). A green cursor with a mailbox is an Erlang actor in all but name. The
federation machinery already prefigures a remote one: `ArenaManifold` attaches a foreign `Space`
(`attach`, `arena_manifold.hpp:302`) and mints one canonical proxy per foreign cell on first ask
(`proxyFor`, `:316`), with `foreignOf()` returning the `ForeignRef{space, index}` a proxy stands for
(`:333`) and `resolveForeign()` returning the store-and-local-ref pair that `promote()` and
`opRefOf()` want (`:325-328`); `materializeFrontier()` pre-mints a frontier of proxies walking a
foreign rank (`:398-401`). A proxy is a local cell that transparently stands for a cell living
elsewhere — network transparency in embryo. The honest caveat is that today's `Space` carries a
`const Manifold*` and a `SpanReader*` (`arena_manifold.hpp:84-91`), in-process pointers; the
federated, networked case is explicitly deferred in the bridge design. The shape prefigures it; the
transport does not exist. That is the gap, not a contradiction.

`promote()` is the one road from speculative scratch to persisted operations
(`arena_manifold.hpp:28`), and it already does the ref translation a distributed commit needs: it
"works by mapping refs rather than by being trusted with them" (`arena_manifold.hpp:742-758`),
returning `Promoted{MicroversionId version, vector<CellRef> cells}` — real refs in the order they
were minted (`:228-233`). It takes an explicit parent `MicroversionId`, the version-vector edge, and
refuses an untranslated ref, the validation half of a commit.

The portable currency exists today. A bare `CellRef` is a raw `std::uint32_t` vector index — inside
an arena "an index, not an operation name" (`arena_manifold.hpp:94-108`), with `refOf` literally
`ephemeralBit | dense` (`:437`) — and is meaningless on any other arena. What crosses instead are
the two "the way another machine can read it" types the publication layer already defines:
`GlobalSpan`, a span addressed so two machines overlap exactly where they quote the same content
(`publication.hpp:62-83`), and `GlobalOpRef`, the global identity of an operation across the
docuverse (`extern_ref.hpp:55-73`). Both are bencode-encoded already — `encodeSpan` / `decodeSpan`
appear in both `publication.cpp:53-77` and `link_package.cpp:44-68` — and `globalise` / `localise` /
`opRefOf` / `localiseOpRef` (`publication.hpp:465-548`) serialize exactly what crosses and nothing
else. The compact binary op encoding `CompactBinaryV4` (referenced at `arena_manifold.hpp:771`) is
the designated portable op wire format. Some values are not wire-ready and must be sealed first: a
scratch span has no address outside its arena until `promote()` gives it one
(`arena_manifold.hpp:287-290`), and an arena scalar carries bits only, its rendering deferred to
`promote()` (`:530-545`). The message boundary is therefore a fixed pipeline: **seal (promote) →
globalise → wire → localise → read through the receiver's `SpanReader`.**

Underneath all of it is a transport that already names nodes, authenticates them, discovers them,
and carries both inline control frames and bulk payload. One libtorrent session is the only thing
that talks to the network (`swarm.hpp:131`); payload, wire extensions, and metadata rendezvous share
the single listener (`swarm.hpp:109`). Node identity is a device-salted BEP 46 key from
`createMutableKeys()` (`swarm.hpp:62`), gaining authority only when bound to a verified
`DeviceDelegation` through `trustDelegation()` — "the only way a device key becomes acceptable for
peer authentication" (`identity_network_controller.hpp:280-292`). A BEP 46 put is not committed
until DHT-acknowledged (`publicationAcknowledged()`, `swarm.hpp:234-237`). Four BEP 10 wire
extensions already exist (`identity_layout.hpp:245-272`), one of them `LiveOpBroadcast` carrying
live collaborative edits between connected peers (`swarm.hpp:255-305`) — the exact shape an actor
control message would take.

## The unified model: node, process, PID, message

Spool-Mesh is a mesh of these share-nothing arenas communicating only by content-addressed,
immutable messages, exactly as the single-machine study already requires between in-process
`WorkerContext`s, each owning its own `cursorPC_` and `choiceStack_` (`worker_context.hpp:51`).
Distribution is that same discipline with one channel — the thread handoff — restretched over the
DHT. Four definitions carry the model.

A **node** is `(authorKey, deviceKey, arenaEpoch)`: an OpenPGP-ledger-bound author, a device-salted
BEP 46 key verified through `trustDelegation` (`identity_network_controller.hpp:291`) against a
`DeviceDelegation`, and a monotonic epoch the device bumps on restart. It is never a host and port;
the swarm has no stable name-to-host map, and a peer disconnects once it has nothing left to give,
so peer counts drain to zero after a transfer (`swarm.hpp:243-245`).

A **process** is a cursor: a Vortex green thread running a `WorkerContext`, minted on a `+d.cursors`
rank, with no shared mutable state once per-arena isolation lands. Give it a mailbox and it is an
Erlang actor.

A **process address (PID)** is `(node, cursorRef, spawnSeqno)`. The `cursorRef` is a node-local
opaque handle minted at spawn; it needs no global uniqueness because it is always qualified by the
node. The `spawnSeqno` is Erlang's incarnation guard: it keeps a `CellRef` reused after restart from
being mistaken for the dead cursor. A PID resolves lazily and the resolution is never cached — the
ledger confirms the device key is live and delegated, a BEP 46 `resolveMutable`
(`swarm.hpp:206-224`) yields the device's current rendezvous pointer, and topic rendezvous finds who
is seeding that node's mailbox.

A **message** is content-addressed and immutable. Control messages are inline frames on a new BEP 10
wire extension; bulk payloads are sealed torrents named by infohash. A message carries no bare
`CellRef` — only `GlobalSpan` / `GlobalOpRef` (`publication.hpp:69`, `:99`) or a sealed `Promoted`
write-set. Because the substrate addresses by content, idempotent deduplication on arrival is free.

On the Erlang-versus-Julia question the decision is **both, in two layers.** Erlang actors are the
control plane — location-transparent PIDs, mailboxes, monitors — carried over the wire extension.
Julia-style futures and data parallelism (VPL `map` and `fold`, VQL scan partitions) stay intra-node
on the existing `VortexWorkerPool` (`vortex_worker_pool.hpp:35`), exactly as the multithreading
study parallelizes them, and the only cross-machine data primitive is a content-addressed `Future`
that resolves to a seeded `Promoted` value. Julia's master-owns-truth `addprocs` model is rejected:
there is no master here, only signing authors, and the engine's `branch = keep-both` posture has no
central scheduler to resolve a `Future` into. The actor spine fits keep-both; the future leaves fit
immutability.

## What maps cleanly

The reuse is large, and the "unusually cheap" verdict rests on types that already exist rather than
on new infrastructure. The boundary is drawn, not invented.

| Distribution need            | Existing mechanism                                                    | Citation                                                                 |
| ---------------------------- | --------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| Node (share-nothing)         | `ArenaManifold` + `VortexCore`/`VortexVM` in a `WorkerContext`        | `worker_context.hpp:62-66`                                               |
| Process / actor              | a cursor on `+d.cursors`                                              | `vortex_vm.cpp:127-141`                                                  |
| Remote handle                | `proxyFor` / `ForeignRef` / `resolveForeign`                          | `arena_manifold.hpp:316`, `:333`, `:325-328`                             |
| Commit                       | `promote()`, the one ref-mapping road to persistence                  | `arena_manifold.hpp:28`, `:742-758`                                      |
| Durable commit over the wire | `publicationAcknowledged()` (DHT put + ack)                           | `swarm.hpp:234-237`                                                      |
| Portable message types       | `GlobalSpan` / `GlobalOpRef`, bencode `encodeSpan`                    | `publication.hpp:62-83`, `extern_ref.hpp:55-73`                          |
| Portable op encoding         | `CompactBinaryV4`                                                     | `arena_manifold.hpp:771`                                                 |
| Message currency / codec     | `globalise` / `localise` / `opRefOf` / `localiseOpRef`                | `publication.hpp:465-548`                                                |
| Immutable copy-on-send       | content-addressed value → free idempotent dedup                       | the substrate's addressing                                               |
| Node identity                | device-salted BEP 46 key + `DeviceDelegation`                         | `swarm.hpp:62`, `user_permascroll.hpp:38`                                |
| Node authentication          | `PeerChallenge` / `PeerChallengeResponse`, gated by `trustDelegation` | `identity_layout.hpp:457-479`, `identity_network_controller.hpp:280-292` |
| Discovery                    | topic rendezvous (`xanadu:topic:` / `xanadu:collab:`)                 | `author_catalog.cpp:131`, `swarm.cpp:1378-1384`                          |
| Durable actor state pointer  | BEP 46 mutable name                                                   | `swarm.hpp:40-54`, `:226-232`                                            |
| Bulk write-set transfer      | sealed torrent fetched metadata-first by infohash                     | `swarm.hpp:177-182`                                                      |
| Backpressure / cancellation  | `run maxCycles`, `PromotionBudget`, `allocationLimit_`                | `vortex_vm.cpp:1050`, `arena_manifold.hpp:222-224`, `:409-414`           |

None of these need redesign. The portable currency already serializes exactly what crosses and
nothing else, and `promote()` already performs the ref translation a cross-machine commit requires.

## What fights the model, and the resolutions

Four genuine misfits, each resolved by a firm decision rather than a workaround.

**A bare `CellRef` is garbage off-arena.** It is a raw vector index into one arena, and an
`ephemeralBit`-tagged ref (`arena_manifold.hpp:24`) is refused by `Store::setLink()` by construction
(`:750`). Shipping one is shipping a raw pointer. The resolution: only `GlobalOpRef`, published
`PrimediaSpan` and `GlobalSpan`, and sealed `Promoted` write-sets ever cross the wire. A remote
process is named in the local arena by a proxy-cursor cell that holds a PID, not a foreign index. A
`PrimediaSpan` over an already-published scroll is wire-ready as it stands; one over an unpublished
local scroll is not until its scroll is published. Nothing with `ephemeralBit` set is ever
serialized, and the existing R8 refusals catch the leak loudly for free.

**The global `choiceStack_` does not distribute.** It is a single global vector
(`vortex_vm.hpp:151`); `cutBarrier` is `choiceStack_.size()` at push, so one cursor's cut barrier
indexes another cursor's frames, and each `ChoicePoint::mark` names lengths into one arena's dense
vectors that `release()` truncates. A mark from node A truncates nothing on node B, and A's cut
barrier indexes A's stack. Distributed OR-parallel logic search — the Aurora and Muse
multiple-binding problem, now with WAN latency — re-fights the GIL over the wire. The resolution: no
cross-machine AND or OR search in v1. The choice stack stays strictly per-node. OR-parallelism, if
it is ever done, is node-forking merged by a stable key, never a shared distributed stack.

**Single-writer truncation versus remote mutation.** The arena's `release()` and `discard()`
truncation model assumes one writer, and `compact()` moves every run and is refused while a mark is
outstanding (`arena_manifold.hpp:594-620`) precisely because truncation undo is coherent only with a
single appender. The resolution is the headline safety property: **there is no remote mutation.** A
node never reaches into another's arena; it receives an immutable value or a `Promoted` write-set
and merges it locally by minting its own operations against its own parent `MicroversionId`.
Branch-not-overwrite is CRDT keep-both — two promotes from one parent become two children, never a
lock-protected last-writer-wins. The single-writer model that forbids a shared mutable arena is
exactly what makes network transparency safe: there is no write to race on. A remote reference to a
node's content is a `GlobalSpan` into its published, append-only scroll, never a pointer into its
ephemeral arena, so a node's own garbage collection can never pull the rug from a remote reader.

**64 KiB frames and churn and latency.** `kMaxPayloadBytes` is 64 KB (`identity_layout.hpp:30`);
peers churn, reads block up to a 30-second read or 60-second metadata timeout (`swarm.hpp:149-151`),
and the swarm offers no synchronous RPC and no flow control. The resolution: control messages ride
inline; bulk write-sets go as a sealed torrent referenced by infohash, fetched metadata-first so the
receiver budgets before pulling payload. Delivery is **at-least-once plus content-addressed
idempotent deduplication, which is effectively-once** — the only honest guarantee over this
transport. No synchronous RPC exists and none is faked; a Vortex `call` that would block on a remote
reply is modeled as an asynchronous `Send` followed by a later `DeliverValue`, or it is not
distributed. Ordering is causal, derived from `promote()`'s explicit parent `MicroversionId`, not
total: a message depending on version V carries V, and the receiver delays or branches. BEP 46's
"highest seqno wins" is the single last-writer-wins point, and it may name only a branch-head
pointer — it must never silently discard a branch's content.

## The wire mapping

Live actor control is the one thing the swarm layer lacks and the one new piece this design
proposes: a BEP 10 wire extension named `xanadu_vortex_actor`, faithful to the existing four
(`identity_layout.hpp:245-272`). It advertises itself in the handshake `m` dictionary with a fixed
local id. The identity extensions use local ids 24 through 27 to avoid libtorrent built-ins; the
actor extension uses **local id 6**, and they occupy `MessageType` codes `0x01` through `0x3F`, so
the proposal reserves the sub-range `0x40` through `0x4F`. The frame format is unchanged: a 4-byte
big-endian length, extended id 20, the peer's local id, then one `MessageType` byte and a bencoded
dictionary, no larger than `kMaxPayloadBytes` of 64 KiB (`identity_layout.hpp:30`).

| `MessageType`          | Code   | Purpose                                                                       |
| ---------------------- | ------ | ----------------------------------------------------------------------------- |
| `ActorSpawn`           | `0x40` | request a cursor on the remote node; carries a program ref and Hashcash stamp |
| `ActorSpawnAck`        | `0x41` | granted (the new cursor-id) or refused                                        |
| `ActorSend`            | `0x42` | message to `(node, salt, cursor-id)`; a small inline bencoded value           |
| `ActorMonitor`         | `0x43` | ask to be told when a cursor exits                                            |
| `ActorDown`            | `0x44` | the monitored cursor exited or failed (Erlang `DOWN`/`EXIT`)                  |
| `ActorAck`             | `0x45` | delivery or receipt acknowledgement                                           |
| `ActorDeliverValue`    | `0x46` | a computed result returned to a waiting cursor                                |
| `ActorDeliverWriteSet` | `0x47` | infohash and sequence of a sealed write-set torrent, never the bytes          |

The 64 KiB rule is load-bearing, not incidental. `ActorSend` and `ActorDeliverValue` carry small
values inline; anything large — a promoted arena, a bulk write-set — goes as a sealed torrent
referenced by `ActorDeliverWriteSet`, and the receiver pulls it metadata-first (`addMagnet` with
`metadataOnly`, then `startDownload`, `swarm.hpp:177-182`) and verifies it before use. This mirrors
how the layer already keeps identity entries beside their proofs but never inlines bulk payload. Two
rules of the existing extensions are kept intact: every proposed struct carries an `isValid()` and
decoders return `std::expected` naming the failure in `SerializationError`
(`identity_serialization.hpp:62-72`), so a malformed frame is refused by name rather than read as
empty; and a frame whose length disagrees with its body is dropped, while a quarantined peer is not
listened to at all.

Node identity and durable actor state ride the BEP 46 machinery unchanged. A node is a
`(publicKey, salt)` from `createMutableKeys()` (`swarm.hpp:62`) with no registry, gaining authority
only through a `trustDelegation()`-verified `DeviceDelegation` whose master fingerprint, X.509
certificate, and GPG signature verify against the master OpenPGP key (`user_permascroll.hpp:79-83`),
with the Merkle ledger linking that fingerprint to a verified email and inclusion proofs. A
long-lived actor's committed state is a BEP 46 mutable pointer (`publishMutable`,
`swarm.hpp:226-232`) to a sealed torrent of its promoted arena, and "committed" has exactly one
meaning: `publicationAcknowledged()` is true only after a DHT node acknowledged this exact signed
put, never from the local cache (`swarm.hpp:234-237`). The commit protocol is therefore: promote the
arena, seal it to a torrent, `publishMutable` at sequence + 1, and spin until acknowledged; highest
acked sequence wins. This gives genuine survival and migration — a new node resolves the pointer,
fetches the sealed arena, and resumes — but honestly only at **checkpoint granularity**: a DHT put
and ack is seconds, not microseconds. It is Erlang `hibernate`-to-disk-and-respawn, fine for a
durable service actor, wrong for an actor that mutates state thousands of times a second. Discovery
is topic rendezvous — SHA-1 of `xanadu:topic:` plus the canonical topic (`author_catalog.cpp:131`),
or the ephemeral room target `xanadu:collab:` (`swarm.cpp:1378-1384`) — and authentication is the
device-key challenge-response `PeerChallenge` / `PeerChallengeResponse`
(`identity_layout.hpp:457-479`), the challenge an unpredictable nonce, a peer that fails isolated
and disconnected by `ConnectionDenial`.

## Failure and supervision

The transport cannot distinguish a crashed process from a slow DHT put from a network partition —
FLP in full force. "Let it crash" is therefore wrong here, because restarting a merely-partitioned
worker duplicates work and can fork hypertime needlessly. A `monitor(PID)` is **three-state** —
`alive`, `suspect`, `gone` — on a phi-accrual detector, and it reports `{DOWN, reason=unreachable}`,
never `crashed`, and only after a lease expires, a rendezvous re-query finds no seeder, and the
ledger shows no newer epoch. Unreachable is not dead.

**Links are demoted to monitors.** Bidirectional death propagation is too aggressive for a
partitionable mesh, where a transient partition would cascade kills across otherwise-healthy actors.
Supervision is **idempotent re-dispatch, not restart**: because remote work is a content-addressed
program over an immutable snapshot, re-sending it to another seeder is safe — same inputs, same
hash, deduplicated on arrival. Erlang's `one_for_one` maps onto re-spawning from the same immutable
input; `one_for_all` has no analogue and is rejected. This dovetails with the engine's standing
"never block the UI" posture: a `suspect` worker's `Future` resolves to a degraded partial of
whatever is already seeded, rather than hanging on a reply that may never come.

## Determinism and security end to end

The multithreading study's deterministic merge key was the submission index, but there is no global
clock or submission order across machines. The replacement is a **content-and-identity-derived total
order**: the merge is sorted by `(GlobalOpRef | content-hash, authorKey, parentMicroversionId)`, a
lexicographic key every node computes identically from immutable inputs, independent of arrival
order or wall-clock. "First solution" means the lowest content-hash of the solution root, never the
lowest latency. The guarantee is that given the same base version and the same set of author-signed
results, every machine computes a byte-identical store, verifiable with `xudu-dump --section=ops`.
Timing nondeterminism must never leak into content.

Security is **default-refuse**, reusing seams that already exist, so that a stranger spawns nothing.
Dispatch is gated by Hashcash (`kDefaultHashcashDifficulty = 20`, `identity_layout.hpp:286`),
authenticated by a device-key `PeerChallengeResponse`, and authorized through ledger delegation and
oracle trust — an unverified stranger never reaches the spawn handler. On the compute-economics
seam, payment is deliberately a seam whose default is `RefusingVerifier`
(`payment_verifier.hpp:104`), the controller's standing default
(`identity_network_controller.hpp:412`): a node not told how it is paid gives nothing away. A
dispatched program runs in a budget-capped arena (`run maxCycles` and a promotion budget) with
`ephemeralBit` isolation, authorized by a **capability** — a signed grant naming the program hash
and budget — not by bare identity. Misbehaviour is handled by the existing peer quarantine: a cursor
that floods or sends bad frames gets its node quarantined and its frames ignored.

## Phased plan

The plan orders by payoff and safety, and each slice is a real increment rather than a rewrite.

**Slice 0 — loopback, no socket.** Two in-process `WorkerContext`s exchange one `GlobalSpan` over an
in-memory channel; the receiver `localise`s it and `promote`s an answer back. This proves the seal →
globalise → wire → localise → promote path end to end before a single network byte is sent, and it
touches none of the hard pieces.

**Slice 1 — the shippable content-addressed `Future`.** No actors, no mailboxes, no supervision. A
node publishes "evaluate program P over base V" by topic rendezvous; a worker fetches both —
immutable and seedable — evaluates in a private arena, `promote()`s, and seeds the resulting
`Promoted` under its content hash; the requester fetches from any seeder, deduplicates on content,
merges by the stable key, and branches on conflict. This reuses `promote()`, publishing, the DHT,
and identity whole, blocks nothing, and needs none of the hard pieces. It is the honest sweet spot
of the transport: coarse, asynchronous, idempotent, tolerant of eventual consistency.

**Slice 2 — the `xanadu_vortex_actor` BEP 10 extension.** The new local id, the `MessageType`
sub-range, and `std::expected` decoders that refuse bad frames, then PIDs, mailboxes, and monitors
in that order. Links come last, if ever.

## Non-goals and open questions

**Non-goals in v1.** A distributed `choiceStack_` or any cross-machine OR-parallel logic search;
synchronous RPC; chatty fine-grained messaging; strong or total ordering; and remote mutation of a
foreign arena. These are not deferrals of good ideas but statements about what the transport cannot
honestly carry. The swarm layer is the wrong tool for latency-sensitive synchronous request/response
(no RPC primitive, 30-to-60-second timeouts, peers that churn to zero), for high-frequency ping-pong
between two actors (connection caps and no rate control thrash the lifecycle), and for strong
ordering or consensus (BEP 46 sequence number is the only order, and that is last-writer-wins, not
linearizability). A tightly-coupled parallel algorithm with chatty synchronous exchange — the Julia
`@distributed` reduce-every-iteration case — must use a direct TCP or QUIC side-channel between
cooperating nodes, with the swarm used only for discovery, identity, and durable checkpoints.

**Open questions.** The phi-accrual lease constants want tuning against real DHT churn rather than a
guessed timeout. The exact capability-grant format for an authorized spawn — what it names, how it
is signed, how a budget is encoded — is unspecified. And it is unresolved whether
checkpoint-granularity handoff (BEP 46 to a sealed torrent, seconds of latency, Erlang
hibernate-and-respawn rather than hot message-passing) is acceptable for live actor migration, or
whether a non-swarm side-channel is eventually required for latency-sensitive coupling. See
[`design/vortex-transport-and-or-parallelism-study.md`](vortex-transport-and-or-parallelism-study.md)
for the follow-up architectural study evaluating transport alternatives (QUIC/Iroh, libp2p,
Kafka-style `SegmentedOpsSpool` offset tailing, SQS/NATS leases) and cross-machine OR-parallelism
(`choiceStack_`, DelPhi/Derby oracle paths, and Muse shadow-delta hydration).

______________________________________________________________________

Change history: 2026-10-17 — initial assembly of this distribution design from the three expert
positions (`_mp_position_codebase_realist.md`, `position-bittorrent-expert.md`) and the
orchestrating architect's synthesis (`synthesis-convergence.md`), with the distributed-systems
position reflected through the synthesis; cross-referenced against the single-machine
[multithreading study](vortex-multithreading-study.md), whose §4 distributed framing this document
extends from threads to machines. The implementation status above records subsequent work; the
original file-and-line citations remain the investigation's factual basis.
